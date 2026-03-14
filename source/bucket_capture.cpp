/*
	Bucket Capture — VideoPostData implementation
	(C) Amber Light, 2026

	Findings from v1:
	  - VIDEOPOSTCALL::TILE never fires (Standard/Physical renderer)
	  - ExecuteLine() fires per-scanline with bucket coordinates (line, xmin, xmax)
	  - pp->cpu_num identifies which render thread owns the bucket
	  - Bucket boundaries detected by tracking per-CPU (x-range, line sequence)

	Strategy (v5 — grid-based):
	  1. At RENDER OPEN, compute the bucket grid from frame dimensions + bucket size
	  2. In ExecuteLine(), map each scanline to its grid cell and count lines received
	  3. When a cell reaches its expected line count → save tile immediately
	  4. At INNER close, save any unsaved cells as a safety net

	Previous heuristic approach (v2-v4) tried to detect bucket boundaries from
	per-CPU line sequences. This produced off-grid tiles when gap tolerance and
	height caps interacted with thread scheduling. The grid approach is exact:
	each scanline self-identifies its cell via cellX = xmin/bucketW, cellY = line/bucketH.

	Output goes to C:\temp\bucket_capture_cpp\ as individual bucket PNGs.
*/

#include "bucket_capture.h"
#include "c4d_videopostdata.h"

using namespace cinema;

// ---------------------------------------------------------------------------
// Grid cell — tracks how many scanlines have been received for one bucket
// ---------------------------------------------------------------------------
struct GridCell
{
	Int32 linesReceived;
	Bool  saved;
};

// ---------------------------------------------------------------------------
// BucketCapturePost — VideoPostData that intercepts render buckets
// ---------------------------------------------------------------------------
class BucketCapturePost : public VideoPostData
{
	INSTANCEOF(BucketCapturePost, VideoPostData)

private:
	// Per-sequence state
	Filename _outputDir;
	Int32    _frameIndex;

	// Per-frame counters
	Int32 _lineCount;

	// Stored render pointer (set at RENDER OPEN, used at INNER CLOSE)
	Render* _activeRender;

	// Frame dimensions (from RayParameter)
	Int32 _frameW;
	Int32 _frameH;

	// Bucket size from render settings
	Int32 _bucketSizeX;
	Int32 _bucketSizeY;

	// Bucket grid — each cell tracks scanline count for one bucket position
	static const Int32 MAX_GRID_CELLS = 16384;  // up to ~128x128 grid
	GridCell _grid[MAX_GRID_CELLS];
	Int32    _gridCols;
	Int32    _gridRows;

	// Save just the bucket tile region from VPBuffer
	Bool SaveBucketTile(Render* render, const Filename& path,
		Int32 tileX1, Int32 tileY1, Int32 tileX2, Int32 tileY2)
	{
		VPBuffer* rgba = render->GetBuffer(VPBUFFER_RGBA, NOTOK);
		if (!rgba)
			return false;

		Int32 cpp = rgba->GetInfo(VPGETINFO::CPP);

		Int32 tileW = tileX2 - tileX1 + 1;
		Int32 tileH = tileY2 - tileY1 + 1;

		if (tileW <= 0 || tileH <= 0)
			return false;

		BaseBitmap* bmp = BaseBitmap::Alloc();
		if (!bmp)
			return false;

		if (bmp->Init(tileW, tileH) != IMAGERESULT::OK)
		{
			BaseBitmap::Free(bmp);
			return false;
		}

		Int bufSize = cpp * tileW;
		Float32* buffer = nullptr;
		iferr (buffer = NewMemClear(Float32, bufSize))
		{
			BaseBitmap::Free(bmp);
			return false;
		}

		for (Int32 y = tileY1; y <= tileY2; y++)
		{
			rgba->GetLine(tileX1, y, tileW, buffer, 32, true);

			Float32* b = buffer;
			for (Int32 x = 0; x < tileW; x++, b += cpp)
			{
				Int32 r = (Int32)(ClampValue(b[0], 0.0f, 1.0f) * 255.0f);
				Int32 g = (Int32)(ClampValue(b[1], 0.0f, 1.0f) * 255.0f);
				Int32 bl = (Int32)(ClampValue(b[2], 0.0f, 1.0f) * 255.0f);
				bmp->SetPixel(x, y - tileY1, r, g, bl);
			}
		}

		DeleteMem(buffer);

		Bool ok = (bmp->Save(path, FILTER_PNG, nullptr, SAVEBIT::NONE) == IMAGERESULT::OK);
		BaseBitmap::Free(bmp);
		return ok;
	}

	// Save full frame from VPBuffer
	Bool SaveFullFrame(Render* render, const Filename& path)
	{
		VPBuffer* rgba = render->GetBuffer(VPBUFFER_RGBA, NOTOK);
		if (!rgba)
			return false;

		Int32 w = rgba->GetBw();
		Int32 h = rgba->GetBh();
		Int32 cpp = rgba->GetInfo(VPGETINFO::CPP);

		BaseBitmap* bmp = BaseBitmap::Alloc();
		if (!bmp)
			return false;

		if (bmp->Init(w, h) != IMAGERESULT::OK)
		{
			BaseBitmap::Free(bmp);
			return false;
		}

		Int bufSize = cpp * w;
		Float32* buffer = nullptr;
		iferr (buffer = NewMemClear(Float32, bufSize))
		{
			BaseBitmap::Free(bmp);
			return false;
		}

		for (Int32 y = 0; y < h; y++)
		{
			rgba->GetLine(0, y, w, buffer, 32, true);

			Float32* b = buffer;
			for (Int32 x = 0; x < w; x++, b += cpp)
			{
				Int32 r = (Int32)(ClampValue(b[0], 0.0f, 1.0f) * 255.0f);
				Int32 g = (Int32)(ClampValue(b[1], 0.0f, 1.0f) * 255.0f);
				Int32 bl = (Int32)(ClampValue(b[2], 0.0f, 1.0f) * 255.0f);
				bmp->SetPixel(x, y, r, g, bl);
			}
		}

		DeleteMem(buffer);

		Bool ok = (bmp->Save(path, FILTER_PNG, nullptr, SAVEBIT::NONE) == IMAGERESULT::OK);
		BaseBitmap::Free(bmp);
		return ok;
	}

	// Build a tile filename: "f01_x064_y128_64x64.png"
	Filename MakeTileFilename(Int32 frame, Int32 x, Int32 y, Int32 w, Int32 h)
	{
		String frameStr = String::IntToString(frame);
		while (frameStr.GetLength() < 2)
		{
			String padded = "0"_s;
			padded += frameStr;
			frameStr = padded;
		}

		String name = "f"_s;
		name += frameStr;
		name += "_x"_s;
		name += String::IntToString(x);
		name += "_y"_s;
		name += String::IntToString(y);
		name += "_"_s;
		name += String::IntToString(w);
		name += "x"_s;
		name += String::IntToString(h);
		name += ".png"_s;
		return _outputDir + Filename(name);
	}

	// Build a simple filename: "f01_frame.png"
	Filename MakeFrameFilename(Int32 frame)
	{
		String frameStr = String::IntToString(frame);
		while (frameStr.GetLength() < 2)
		{
			String padded = "0"_s;
			padded += frameStr;
			frameStr = padded;
		}

		String name = "f"_s;
		name += frameStr;
		name += "_frame.png"_s;
		return _outputDir + Filename(name);
	}

	// Compute the pixel rect for a grid cell, handling right/bottom edge
	void GetCellRect(Int32 cellX, Int32 cellY,
		Int32& outX, Int32& outY, Int32& outW, Int32& outH)
	{
		outX = cellX * _bucketSizeX;
		outY = cellY * _bucketSizeY;
		outW = _bucketSizeX;
		outH = _bucketSizeY;

		// Clamp to frame bounds for edge cells
		if (outX + outW > _frameW)
			outW = _frameW - outX;
		if (outY + outH > _frameH)
			outH = _frameH - outY;
	}

	// Save a grid cell's tile from VPBuffer
	void SaveGridCell(Render* render, Int32 frame, Int32 cellX, Int32 cellY)
	{
		if (!render)
			return;

		Int32 x, y, w, h;
		GetCellRect(cellX, cellY, x, y, w, h);

		if (w <= 0 || h <= 0)
			return;

		Filename fn = MakeTileFilename(frame, x, y, w, h);
		SaveBucketTile(render, fn, x, y, x + w - 1, y + h - 1);
	}

	// Re-save ALL grid cells with final VPBuffer data (guaranteed correct at INNER close).
	// Progressive saves from ExecuteLine may have stale pixel data — this overwrites them.
	void FlushAllCells(Render* render, Int32 frame)
	{
		if (!render)
			return;

		Int32 totalCells = _gridCols * _gridRows;
		Int32 progressiveCount = 0;

		for (Int32 i = 0; i < totalCells; i++)
		{
			if (_grid[i].saved)
				progressiveCount++;

			Int32 cellX = i % _gridCols;
			Int32 cellY = i / _gridCols;
			SaveGridCell(render, frame, cellX, cellY);
			_grid[i].saved = true;
		}

		String msg = "[BucketCapture] INNER CLOSE -- re-saved all "_s;
		msg += String::IntToString(totalCells);
		msg += " tiles ("_s;
		msg += String::IntToString(progressiveCount);
		msg += " were progressive previews)"_s;
		GePrint(msg);
	}

public:
	static NodeData* Alloc() { return NewObjClear(BucketCapturePost); }

	// Request per-scanline callbacks
	virtual VIDEOPOSTINFO GetRenderInfo(BaseVideoPost* node) override
	{
		return VIDEOPOSTINFO::EXECUTELINE;
	}

	// Accept all render engines for exploration
	virtual Bool RenderEngineCheck(const BaseVideoPost* node, Int32 id) const override
	{
		if (id == RDATA_RENDERENGINE_PREVIEWHARDWARE)
			return false;
		return true;
	}

	virtual RENDERRESULT Execute(BaseVideoPost* node, VideoPostStruct* vps) override
	{
		if (!vps)
			return RENDERRESULT::OK;

		switch (vps->vp)
		{
			case VIDEOPOSTCALL::FRAMESEQUENCE:
			{
				if (vps->open)
				{
					GePrint("[BucketCapture] === FRAMESEQUENCE OPEN ==="_s);

					_outputDir = Filename("C:\\temp\\bucket_capture_cpp"_s);
					GeFCreateDir(_outputDir);

					_frameIndex = 0;
				}
				else
				{
					GePrint("[BucketCapture] === FRAMESEQUENCE CLOSE ==="_s);
				}
				break;
			}

			case VIDEOPOSTCALL::FRAME:
			{
				if (vps->open)
				{
					_frameIndex++;
					_lineCount = 0;
					_activeRender = nullptr;
					_bucketSizeX = 64;
					_bucketSizeY = 64;
					_frameW = 0;
					_frameH = 0;
					_gridCols = 0;
					_gridRows = 0;

					String msg = "[BucketCapture] --- FRAME OPEN (frame "_s;
					msg += String::IntToString(_frameIndex);
					msg += ")"_s;
					GePrint(msg);
				}
				else
				{
					// Save the final full frame
					if (vps->render)
					{
						Filename fn = MakeFrameFilename(_frameIndex);
						if (SaveFullFrame(vps->render, fn))
						{
							GePrint("[BucketCapture] Saved final frame"_s);
						}
					}

					String msg = "[BucketCapture] --- FRAME CLOSE --- lines="_s;
					msg += String::IntToString(_lineCount);
					GePrint(msg);
				}
				break;
			}

			case VIDEOPOSTCALL::RENDER:
			{
				if (vps->open)
				{
					_activeRender = vps->render;

					// Read bucket size from render settings
					_bucketSizeX = 64;
					_bucketSizeY = 64;
					BaseDocument* doc = node->GetDocument();
					if (doc)
					{
						RenderData* rd = doc->GetActiveRenderData();
						if (rd)
						{
							const BaseContainer* bc = rd->GetDataInstance();
							if (bc)
							{
								Bool autoSize = bc->GetBool(7002, true);  // RDATA_AUTOMATICBUCKETSIZE
								if (!autoSize)
								{
									_bucketSizeX = bc->GetInt32(7000, 64);  // RDATA_BUCKETSIZEX
									_bucketSizeY = bc->GetInt32(7001, 64);  // RDATA_BUCKETSIZEY
								}
							}
						}
					}

					if (vps->vd)
					{
						const RayParameter* ray = vps->vd->GetRayParameter();
						if (ray)
						{
							_frameW = ray->right - ray->left + 1;
							_frameH = ray->bottom - ray->top + 1;

							// Compute grid dimensions
							_gridCols = (_frameW + _bucketSizeX - 1) / _bucketSizeX;
							_gridRows = (_frameH + _bucketSizeY - 1) / _bucketSizeY;

							Int32 totalCells = _gridCols * _gridRows;
							if (totalCells > MAX_GRID_CELLS)
								totalCells = MAX_GRID_CELLS;

							// Reset grid
							for (Int32 i = 0; i < totalCells; i++)
							{
								_grid[i].linesReceived = 0;
								_grid[i].saved = false;
							}

							String msg = "[BucketCapture] RENDER OPEN "_s;
							msg += String::IntToString(_frameW);
							msg += "x"_s;
							msg += String::IntToString(_frameH);
							msg += " bucket="_s;
							msg += String::IntToString(_bucketSizeX);
							msg += "x"_s;
							msg += String::IntToString(_bucketSizeY);
							msg += " grid="_s;
							msg += String::IntToString(_gridCols);
							msg += "x"_s;
							msg += String::IntToString(_gridRows);
							msg += " ("_s;
							msg += String::IntToString(_gridCols * _gridRows);
							msg += " cells)"_s;
							GePrint(msg);
						}
					}
				}
				else
				{
					GePrint("[BucketCapture] RENDER CLOSE"_s);
				}
				break;
			}

			case VIDEOPOSTCALL::INNER:
			{
				if (!vps->open)
				{
					// Re-save ALL cells with final pixel data (overwrites progressive previews)
					FlushAllCells(vps->render ? vps->render : _activeRender, _frameIndex);
				}
				break;
			}

			default:
				break;
		}

		return RENDERRESULT::OK;
	}

	// -----------------------------------------------------------------
	// ExecuteLine — per-scanline pixel interception
	// Maps each scanline to its grid cell and increments the line counter.
	// When a cell has received all its expected lines → save immediately.
	// -----------------------------------------------------------------
	virtual void ExecuteLine(BaseVideoPost* node, PixelPost* pp) override
	{
		if (!pp || _gridCols <= 0 || _gridRows <= 0)
			return;

		_lineCount++;

		// Map scanline to grid cell
		Int32 cellX = pp->xmin / _bucketSizeX;
		Int32 cellY = pp->line / _bucketSizeY;

		if (cellX < 0 || cellX >= _gridCols || cellY < 0 || cellY >= _gridRows)
			return;

		Int32 cellIdx = cellY * _gridCols + cellX;
		if (cellIdx >= MAX_GRID_CELLS)
			return;

		GridCell& cell = _grid[cellIdx];
		cell.linesReceived++;

		// Check if this cell is now complete
		if (!cell.saved)
		{
			// Expected height for this cell (edge cells may be shorter)
			Int32 expectedH = _bucketSizeY;
			Int32 cellBottom = (cellY + 1) * _bucketSizeY;
			if (cellBottom > _frameH)
				expectedH = _frameH - cellY * _bucketSizeY;

			if (cell.linesReceived >= expectedH && _activeRender)
			{
				cell.saved = true;
				SaveGridCell(_activeRender, _frameIndex, cellX, cellY);
			}
		}

		// Log first 5 lines for debugging
		if (_lineCount <= 5)
		{
			String msg = "[BucketCapture] ExecuteLine: cpu="_s;
			msg += String::IntToString(pp->cpu_num);
			msg += " line="_s;
			msg += String::IntToString(pp->line);
			msg += " x="_s;
			msg += String::IntToString(pp->xmin);
			msg += ".."_s;
			msg += String::IntToString(pp->xmax);
			msg += " -> cell("_s;
			msg += String::IntToString(cellX);
			msg += ","_s;
			msg += String::IntToString(cellY);
			msg += ")"_s;
			GePrint(msg);
		}
	}
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
Bool RegisterBucketCapture()
{
	return RegisterVideoPostPlugin(
		BUCKET_CAPTURE_PLUGIN_ID,
		"Bucket Capture"_s,
		0,
		BucketCapturePost::Alloc,
		String(),      // no description resource
		0,             // disklevel
		0              // priority
	);
}
