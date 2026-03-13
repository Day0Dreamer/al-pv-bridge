/*
	Bucket Capture — VideoPostData implementation
	(C) Amber Light, 2026

	Findings from v1:
	  - VIDEOPOSTCALL::TILE never fires (Standard/Physical renderer)
	  - ExecuteLine() fires per-scanline with bucket coordinates (line, xmin, xmax)
	  - pp->cpu_num identifies which render thread owns the bucket
	  - Bucket boundaries detected by tracking per-CPU (x-range, line sequence)

	Strategy (v2 → v4):
	  1. Track per-CPU bucket state in ExecuteLine()
	  2. When boundary detected → save completed tile IMMEDIATELY from render thread
	  3. At INNER close, flush only the last active bucket per CPU

	V4 fixes:
	  - Progressive saving: tiles appear on disk as each bucket completes (not deferred)
	  - Bug 1 (false merges):  Cap bucket height at render-settings bucket size
	  - Bug 2 (multi-pass re-save): Structurally eliminated (no accumulation buffer)
	  - Bug 3 (false splits): Allow 1-scanline gap tolerance in line sequence
	  - Thread safety: per-CPU tile counters + CPU ID in filenames (no shared mutation)

	Output goes to C:\temp\bucket_capture_cpp\ as individual bucket PNGs.
*/

#include "bucket_capture.h"
#include "c4d_videopostdata.h"

using namespace cinema;

// ---------------------------------------------------------------------------
// Per-CPU bucket tracker — each render thread gets its own slot
// ---------------------------------------------------------------------------
struct CPUBucketState
{
	// Currently accumulating bucket
	Int32 firstLine;
	Int32 lastLine;
	Int32 xMin;
	Int32 xMax;
	Bool  active;

	// Per-CPU tile counter (thread-safe: each CPU only writes its own slot)
	Int32 savedCount;

	void Reset()
	{
		active = false;
		firstLine = -1;
		lastLine = -1;
		xMin = 0;
		xMax = 0;
		savedCount = 0;
	}
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

	// Per-CPU bucket tracking
	static const Int32 MAX_CPUS = 128;
	CPUBucketState _cpuState[MAX_CPUS];

	// Stored render pointer (set at RENDER OPEN, used at INNER CLOSE)
	Render* _activeRender;

	// Frame dimensions (from RayParameter)
	Int32 _frameLeft;
	Int32 _frameTop;
	Int32 _frameRight;
	Int32 _frameBottom;

	// Bucket size from render settings (Bug 1 fix: cap height to prevent false merges)
	Int32 _bucketSizeX;
	Int32 _bucketSizeY;

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

	// Build a tile filename: "f01_c03_tile_0005_x64_y128_64x64.png"
	// CPU number in filename guarantees uniqueness across render threads
	Filename MakeTileFilename(Int32 frame, Int32 cpu, Int32 index, Int32 x, Int32 y, Int32 w, Int32 h)
	{
		String frameStr = String::IntToString(frame);
		while (frameStr.GetLength() < 2)
		{
			String padded = "0"_s;
			padded += frameStr;
			frameStr = padded;
		}

		String cpuStr = String::IntToString(cpu);
		while (cpuStr.GetLength() < 2)
		{
			String padded = "0"_s;
			padded += cpuStr;
			cpuStr = padded;
		}

		String numStr = String::IntToString(index);
		while (numStr.GetLength() < 4)
		{
			String padded = "0"_s;
			padded += numStr;
			numStr = padded;
		}

		String name = "f"_s;
		name += frameStr;
		name += "_c"_s;
		name += cpuStr;
		name += "_tile_"_s;
		name += numStr;
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

	// Save a single completed bucket tile immediately
	void SaveCompletedBucket(Render* render, Int32 frame, Int32 cpu, CPUBucketState& bs)
	{
		if (!render)
			return;

		bs.savedCount++;
		Int32 tileW = bs.xMax - bs.xMin + 1;
		Int32 tileH = bs.lastLine - bs.firstLine + 1;
		Filename fn = MakeTileFilename(frame, cpu, bs.savedCount, bs.xMin, bs.firstLine, tileW, tileH);
		SaveBucketTile(render, fn, bs.xMin, bs.firstLine, bs.xMax, bs.lastLine);
	}

	// Flush remaining active buckets at INNER close
	// Most tiles are already saved progressively from ExecuteLine;
	// this only catches the last in-progress bucket on each CPU.
	void FlushRemainingBuckets(Render* render, Int32 frame)
	{
		if (!render)
			return;

		Int32 flushed = 0;
		Int32 totalSaved = 0;
		for (Int32 cpu = 0; cpu < MAX_CPUS; cpu++)
		{
			CPUBucketState& bs = _cpuState[cpu];
			if (bs.active)
			{
				SaveCompletedBucket(render, frame, cpu, bs);
				bs.active = false;
				flushed++;
			}
			totalSaved += bs.savedCount;
		}

		String msg = "[BucketCapture] INNER CLOSE — flushed "_s;
		msg += String::IntToString(flushed);
		msg += " remaining, "_s;
		msg += String::IntToString(totalSaved);
		msg += " total tiles this pass"_s;
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
					// Read actual frame number from the document timeline
					_frameIndex = 0;
					BaseDocument* frameDoc = node->GetDocument();
					if (frameDoc)
					{
						_frameIndex = frameDoc->GetTime().GetFrame(frameDoc->GetFps());
					}

					_lineCount = 0;
					_activeRender = nullptr;
					_bucketSizeX = 64;
					_bucketSizeY = 64;

					// Reset all per-CPU state
					for (Int32 i = 0; i < MAX_CPUS; i++)
						_cpuState[i].Reset();

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

					Int32 totalTiles = 0;
					for (Int32 i = 0; i < MAX_CPUS; i++)
						totalTiles += _cpuState[i].savedCount;

					String msg = "[BucketCapture] --- FRAME CLOSE --- lines="_s;
					msg += String::IntToString(_lineCount);
					msg += " savedTiles="_s;
					msg += String::IntToString(totalTiles);
					GePrint(msg);
				}
				break;
			}

			case VIDEOPOSTCALL::RENDER:
			{
				if (vps->open)
				{
					_activeRender = vps->render;

					// Bug 1 fix: read bucket size from render settings
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
							_frameLeft   = ray->left;
							_frameTop    = ray->top;
							_frameRight  = ray->right;
							_frameBottom = ray->bottom;

							String msg = "[BucketCapture] RENDER OPEN — "_s;
							msg += String::IntToString(_frameRight - _frameLeft + 1);
							msg += "x"_s;
							msg += String::IntToString(_frameBottom - _frameTop + 1);
							msg += " bucketSize="_s;
							msg += String::IntToString(_bucketSizeX);
							msg += "x"_s;
							msg += String::IntToString(_bucketSizeY);
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
					// INNER close — flush last active bucket on each CPU
					// (all earlier buckets were already saved progressively)
					FlushRemainingBuckets(vps->render ? vps->render : _activeRender, _frameIndex);
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
	// Detects bucket boundaries and saves tiles IMMEDIATELY from the
	// render thread. Each CPU writes its own state — no shared mutation.
	// VPBuffer is readable during ExecuteLine (SDK contract).
	// -----------------------------------------------------------------
	virtual void ExecuteLine(BaseVideoPost* node, PixelPost* pp) override
	{
		if (!pp)
			return;

		_lineCount++;

		Int32 cpu = pp->cpu_num;
		if (cpu < 0 || cpu >= MAX_CPUS)
			return;

		CPUBucketState& bs = _cpuState[cpu];

		// Detect bucket boundary: x-range changed, line gap > 1, or height cap reached
		Bool newBucket = !bs.active
			|| pp->xmin != bs.xMin
			|| pp->xmax != bs.xMax
			|| pp->line > bs.lastLine + 2                         // Bug 3: 1-line gap tolerance
			|| (bs.lastLine - bs.firstLine + 1) >= _bucketSizeY;  // Bug 1: height cap

		if (newBucket)
		{
			// Save previous bucket immediately — tile appears on disk now
			if (bs.active && _activeRender)
			{
				SaveCompletedBucket(_activeRender, _frameIndex, cpu, bs);
			}

			// Start tracking new bucket
			bs.active = true;
			bs.firstLine = pp->line;
			bs.xMin = pp->xmin;
			bs.xMax = pp->xmax;
		}

		bs.lastLine = pp->line;

		// Log first 5 lines for debugging
		if (_lineCount <= 5)
		{
			String msg = "[BucketCapture] ExecuteLine: cpu="_s;
			msg += String::IntToString(cpu);
			msg += " line="_s;
			msg += String::IntToString(pp->line);
			msg += " x="_s;
			msg += String::IntToString(pp->xmin);
			msg += ".."_s;
			msg += String::IntToString(pp->xmax);
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
