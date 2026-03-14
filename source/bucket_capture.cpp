/*
	Bucket Capture -- VideoPostData implementation
	(C) Amber Light, 2026

	Strategy (v7 -- .albt binary stream + VPBuffer polling):
	  1. At FRAMESEQUENCE OPEN, read scene UUID and open .albt file
	  2. At RENDER OPEN, compute bucket grid, write stream header, start poll thread
	  3. In ExecuteLine(), map scanline to grid cell; when complete, write progressive record
	     (Standard/Physical only -- GPU renderers skip ExecuteLine entirely)
	  4. Poll thread samples VPBuffer center-pixel per cell every 250ms,
	     writes progressive tiles for changed cells (GPU renderers)
	  5. At INNER close, stop poll thread, re-save ALL cells as final + sentinel
	  6. At FRAMESEQUENCE CLOSE, close .albt file

	The poll thread is harmless for Standard/Physical: ExecuteLine marks cells
	saved first, so the poll thread finds them saved and writes nothing.

	Output: $ALBT_STREAM_DIR\{uuid}.albt  (default: C:\temp\albt_streams)
	Format: docs/adr/009-binary-tile-stream-for-render-farm.md
*/

#include "bucket_capture.h"
#include "c4d_videopostdata.h"

#include <cstdio>     // FILE*, fwrite, fopen, fclose, fflush
#include <cstdlib>    // getenv
#include <mutex>      // std::mutex, std::lock_guard
#include <thread>     // std::thread
#include <atomic>     // std::atomic<bool>
#include <chrono>     // std::chrono::milliseconds

using namespace cinema;

// .albt phase constants
static const UChar PHASE_PROGRESSIVE = 0x00;
static const UChar PHASE_FINAL       = 0x01;
static const UChar PHASE_SENTINEL    = 0xFF;

// ---------------------------------------------------------------------------
// Grid cell -- tracks how many scanlines have been received for one bucket
// ---------------------------------------------------------------------------
struct GridCell
{
	Int32 linesReceived;
	Bool  saved;
};

// ---------------------------------------------------------------------------
// BucketCapturePost -- VideoPostData that writes .albt binary stream
// ---------------------------------------------------------------------------
class BucketCapturePost : public VideoPostData
{
	INSTANCEOF(BucketCapturePost, VideoPostData)

private:
	// Per-sequence state
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

	// Bucket grid -- each cell tracks scanline count for one bucket position
	static const Int32 MAX_GRID_CELLS = 16384;  // up to ~128x128 grid
	GridCell _grid[MAX_GRID_CELLS];
	Int32    _gridCols;
	Int32    _gridRows;

	// === .albt stream state ===
	FILE*       _albtFile;          // opened at FRAMESEQUENCE open, closed at close
	Int64       _streamStartTime;   // GeGetTimer() ms at file open
	Bool        _headerWritten;     // deferred to RENDER open (needs frame dims)
	String      _jobUUID;           // from doc BaseContainer[AMBERLIGHT_SCENE_UUID]
	Bool        _sentinelWritten;   // prevents duplicate sentinels on multi-pass
	std::mutex  _writeMutex;        // protects all fwrite() calls

	// === VPBuffer polling thread (for GPU renderers) ===
	std::thread        _pollThread;
	std::atomic<bool>  _pollRunning{false};
	Float32*           _baseline;    // 1 RGBA sample per cell (heap, ~32KB for 4K)
	Bool               _debugPoll;       // set ALBT_DEBUG_POLL=1 to enable poll logging
	Int32              _pollIterations;   // diagnostic counter
	Int32              _pollDetections;   // cells detected as changed
	Int32              _pollWriteOK;     // successful WriteTileRecord calls from poll
	Int32              _pollWriteFail;   // failed WriteTileRecord calls from poll

	// ---------------------------------------------------------------
	// GetCellRect -- compute pixel rect for a grid cell
	// ---------------------------------------------------------------
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

	// ---------------------------------------------------------------
	// PollVPBuffer -- sample center pixel of each unsaved cell,
	// compare vs baseline, write progressive tile if changed.
	// Called from poll thread only.
	// ---------------------------------------------------------------
	void PollVPBuffer()
	{
		_pollIterations++;

		if (!_activeRender)
		{
			if (_debugPoll && _pollIterations <= 3)
				GePrint("[Poll] BAIL: _activeRender is null"_s);
			return;
		}
		if (!_albtFile)
		{
			if (_debugPoll && _pollIterations <= 3)
				GePrint("[Poll] BAIL: _albtFile is null"_s);
			return;
		}
		if (!_baseline)
		{
			if (_debugPoll && _pollIterations <= 3)
				GePrint("[Poll] BAIL: _baseline is null"_s);
			return;
		}

		VPBuffer* rgba = _activeRender->GetBuffer(VPBUFFER_RGBA, NOTOK);
		if (!rgba)
		{
			if (_debugPoll && _pollIterations <= 3)
				GePrint("[Poll] BAIL: VPBuffer RGBA is null"_s);
			return;
		}

		Int32 cpp = rgba->GetInfo(VPGETINFO::CPP);
		if (cpp < 3)
		{
			if (_debugPoll && _pollIterations <= 3)
			{
				String msg = "[Poll] BAIL: cpp="_s;
				msg += String::IntToString(cpp);
				GePrint(msg);
			}
			return;
		}

		Int32 totalCells = _gridCols * _gridRows;
		Float32 sample[4] = {0, 0, 0, 0};
		Int32 detectedThisRound = 0;
		Int32 skippedSaved = 0;

		for (Int32 i = 0; i < totalCells; i++)
		{
			if (_grid[i].saved)
			{
				skippedSaved++;
				continue;
			}

			Int32 cellX = i % _gridCols;
			Int32 cellY = i / _gridCols;

			Int32 px, py, pw, ph;
			GetCellRect(cellX, cellY, px, py, pw, ph);
			if (pw <= 0 || ph <= 0)
				continue;

			// Sample center pixel
			Int32 cx = px + pw / 2;
			Int32 cy = py + ph / 2;

			rgba->GetLine(cx, cy, 1, sample, 32, true);

			// Compare RGB against baseline (exact compare, raw buffer values)
			Int32 bIdx = i * 4;
			if (sample[0] != _baseline[bIdx]   ||
				sample[1] != _baseline[bIdx+1] ||
				sample[2] != _baseline[bIdx+2])
			{
				detectedThisRound++;

				if (_debugPoll && detectedThisRound == 1 && _pollIterations <= 8)
				{
					String msg = "[Poll] iter="_s;
					msg += String::IntToString(_pollIterations);
					msg += " cell("_s;
					msg += String::IntToString(cellX);
					msg += ","_s;
					msg += String::IntToString(cellY);
					msg += ") baseline=("_s;
					msg += String::FloatToString(_baseline[bIdx], -1, 4);
					msg += ","_s;
					msg += String::FloatToString(_baseline[bIdx+1], -1, 4);
					msg += ","_s;
					msg += String::FloatToString(_baseline[bIdx+2], -1, 4);
					msg += ") sample=("_s;
					msg += String::FloatToString(sample[0], -1, 4);
					msg += ","_s;
					msg += String::FloatToString(sample[1], -1, 4);
					msg += ","_s;
					msg += String::FloatToString(sample[2], -1, 4);
					msg += ")"_s;
					GePrint(msg);
				}

				// Update baseline to prevent re-triggering
				_baseline[bIdx]   = sample[0];
				_baseline[bIdx+1] = sample[1];
				_baseline[bIdx+2] = sample[2];

				// Write progressive tile record
				_grid[i].saved = true;

				if (_debugPoll)
				{
					long posBefore = _albtFile ? ftell(_albtFile) : -1;
					SaveGridCell(_activeRender, (UInt32)_frameIndex,
						PHASE_PROGRESSIVE, cellX, cellY);
					long posAfter = _albtFile ? ftell(_albtFile) : -1;
					if (posAfter > posBefore)
						_pollWriteOK++;
					else
						_pollWriteFail++;
				}
				else
				{
					SaveGridCell(_activeRender, (UInt32)_frameIndex,
						PHASE_PROGRESSIVE, cellX, cellY);
				}

				_grid[i].saved = false;  // allow re-detection on next poll
				_pollDetections++;
			}
		}

		if (_debugPoll && (_pollIterations % 4 == 0 || detectedThisRound > 0))
		{
			long filePos = _albtFile ? ftell(_albtFile) : -1;
			String msg = "[Poll] iter="_s;
			msg += String::IntToString(_pollIterations);
			msg += " checked="_s;
			msg += String::IntToString(totalCells - skippedSaved);
			msg += " saved="_s;
			msg += String::IntToString(skippedSaved);
			msg += " detected="_s;
			msg += String::IntToString(detectedThisRound);
			msg += " fpos="_s;
			msg += String::IntToString((Int32)filePos);
			msg += " writeOK="_s;
			msg += String::IntToString(_pollWriteOK);
			msg += " writeFail="_s;
			msg += String::IntToString(_pollWriteFail);
			GePrint(msg);
		}
	}

	// ---------------------------------------------------------------
	// PollLoop -- thread entry point: poll VPBuffer every 250ms
	// ---------------------------------------------------------------
	void PollLoop()
	{
		while (_pollRunning.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			if (_pollRunning.load())
				PollVPBuffer();
		}
	}

	// ---------------------------------------------------------------
	// StartPollThread -- alloc baseline, capture initial samples,
	// launch polling thread.  Called at RENDER OPEN.
	// ---------------------------------------------------------------
	void StartPollThread()
	{
		if (_pollRunning.load())
			return;

		if (!_activeRender || !_albtFile || _gridCols <= 0 || _gridRows <= 0)
		{
			if (_debugPoll)
			{
				String msg = "[Poll] StartPollThread BAIL: render="_s;
				msg += _activeRender ? "ok"_s : "NULL"_s;
				msg += " file="_s;
				msg += _albtFile ? "ok"_s : "NULL"_s;
				msg += " grid="_s;
				msg += String::IntToString(_gridCols);
				msg += "x"_s;
				msg += String::IntToString(_gridRows);
				GePrint(msg);
			}
			return;
		}

		Int32 totalCells = _gridCols * _gridRows;
		_pollIterations = 0;
		_pollDetections = 0;
		_pollWriteOK = 0;
		_pollWriteFail = 0;

		// Allocate baseline: 4 floats (RGBA) per cell
		iferr (_baseline = NewMemClear(Float32, totalCells * 4))
		{
			_baseline = nullptr;
			return;
		}

		// Capture initial baseline samples from VPBuffer
		VPBuffer* rgba = _activeRender->GetBuffer(VPBUFFER_RGBA, NOTOK);
		if (rgba)
		{
			Float32 sample[4];
			for (Int32 i = 0; i < totalCells; i++)
			{
				Int32 cellX = i % _gridCols;
				Int32 cellY = i / _gridCols;

				Int32 px, py, pw, ph;
				GetCellRect(cellX, cellY, px, py, pw, ph);
				if (pw <= 0 || ph <= 0)
					continue;

				Int32 cx = px + pw / 2;
				Int32 cy = py + ph / 2;

				rgba->GetLine(cx, cy, 1, sample, 32, true);
				Int32 bIdx = i * 4;
				_baseline[bIdx]   = sample[0];
				_baseline[bIdx+1] = sample[1];
				_baseline[bIdx+2] = sample[2];
				_baseline[bIdx+3] = sample[3];
			}

			if (_debugPoll)
			{
				String msg = "[Poll] Baseline captured: cells="_s;
				msg += String::IntToString(totalCells);
				msg += " cpp="_s;
				msg += String::IntToString(rgba->GetInfo(VPGETINFO::CPP));
				GePrint(msg);
			}
		}
		else if (_debugPoll)
		{
			GePrint("[Poll] WARNING: VPBuffer null at baseline capture!"_s);
		}

		_pollRunning.store(true);
		_pollThread = std::thread([this]() { PollLoop(); });

		if (_debugPoll)
			GePrint("[Poll] Thread launched"_s);
	}

	// ---------------------------------------------------------------
	// StopPollThread -- signal stop, join thread, free baseline.
	// Called at INNER CLOSE and defensively at FRAMESEQUENCE CLOSE.
	// ---------------------------------------------------------------
	void StopPollThread()
	{
		if (!_pollRunning.load())
			return;

		_pollRunning.store(false);

		if (_pollThread.joinable())
			_pollThread.join();

		if (_debugPoll)
		{
			String msg = "[Poll] Thread stopped: iterations="_s;
			msg += String::IntToString(_pollIterations);
			msg += " detections="_s;
			msg += String::IntToString(_pollDetections);
			msg += " writeOK="_s;
			msg += String::IntToString(_pollWriteOK);
			msg += " writeFail="_s;
			msg += String::IntToString(_pollWriteFail);
			GePrint(msg);
		}

		if (_baseline)
		{
			DeleteMem(_baseline);
			_baseline = nullptr;
		}
	}

	// ---------------------------------------------------------------
	// WriteStreamHeader -- 12 bytes, called once at first RENDER open
	// ---------------------------------------------------------------
	void WriteStreamHeader()
	{
		if (!_albtFile)
			return;

		const char magic[4] = {'A', 'L', 'B', 'T'};
		UInt16 version = 1;
		UInt16 fw = (UInt16)_frameW;
		UInt16 fh = (UInt16)_frameH;
		UInt16 bs = (UInt16)_bucketSizeX;

		std::lock_guard<std::mutex> lock(_writeMutex);
		fwrite(magic, 1, 4, _albtFile);
		fwrite(&version, 2, 1, _albtFile);
		fwrite(&fw, 2, 1, _albtFile);
		fwrite(&fh, 2, 1, _albtFile);
		fwrite(&bs, 2, 1, _albtFile);
		fflush(_albtFile);
	}

	// ---------------------------------------------------------------
	// WriteTileRecord -- 25-byte header + raw RGB pixels
	//
	// Reads VPBuffer (same GetLine approach as old SaveBucketTile),
	// converts Float32 -> UChar RGB888, writes to .albt under lock.
	// VPBuffer read happens OUTSIDE lock (expensive part).
	// ---------------------------------------------------------------
	Bool WriteTileRecord(Render* render, UInt32 frame, UChar phase,
		Int32 tileX, Int32 tileY, Int32 tileW, Int32 tileH)
	{
		if (!_albtFile || !render)
			return false;

		VPBuffer* rgba = render->GetBuffer(VPBUFFER_RGBA, NOTOK);
		if (!rgba)
			return false;

		Int32 cpp = rgba->GetInfo(VPGETINFO::CPP);
		if (tileW <= 0 || tileH <= 0)
			return false;

		// Allocate RGB output buffer
		Int rgbSize = tileW * tileH * 3;
		UChar* rgbBuf = nullptr;
		iferr (rgbBuf = NewMemClear(UChar, rgbSize))
			return false;

		// Allocate scanline read buffer
		Int bufSize = cpp * tileW;
		Float32* lineBuffer = nullptr;
		iferr (lineBuffer = NewMemClear(Float32, bufSize))
		{
			DeleteMem(rgbBuf);
			return false;
		}

		// Read VPBuffer -> convert Float32 -> RGB888 (OUTSIDE lock)
		for (Int32 y = 0; y < tileH; y++)
		{
			rgba->GetLine(tileX, tileY + y, tileW, lineBuffer, 32, true);
			Float32* src = lineBuffer;
			UChar* dst = rgbBuf + y * tileW * 3;

			for (Int32 x = 0; x < tileW; x++, src += cpp)
			{
				dst[0] = (UChar)(ClampValue(src[0], 0.0f, 1.0f) * 255.0f);
				dst[1] = (UChar)(ClampValue(src[1], 0.0f, 1.0f) * 255.0f);
				dst[2] = (UChar)(ClampValue(src[2], 0.0f, 1.0f) * 255.0f);
				dst += 3;
			}
		}
		DeleteMem(lineBuffer);

		// Compute timestamp (microseconds since stream start)
		Int64 nowMs = GeGetTimer();
		UInt64 timestampUs = (UInt64)((nowMs - _streamStartTime) * 1000);
		UInt32 pixLen = (UInt32)rgbSize;

		// Pack tile header fields
		UInt16 x16 = (UInt16)tileX;
		UInt16 y16 = (UInt16)tileY;
		UInt16 w16 = (UInt16)tileW;
		UInt16 h16 = (UInt16)tileH;

		// Write header + pixels UNDER LOCK
		{
			std::lock_guard<std::mutex> lock(_writeMutex);
			fwrite(&frame, 4, 1, _albtFile);
			fwrite(&phase, 1, 1, _albtFile);
			fwrite(&x16, 2, 1, _albtFile);
			fwrite(&y16, 2, 1, _albtFile);
			fwrite(&w16, 2, 1, _albtFile);
			fwrite(&h16, 2, 1, _albtFile);
			fwrite(&timestampUs, 8, 1, _albtFile);
			fwrite(&pixLen, 4, 1, _albtFile);
			fwrite(rgbBuf, 1, rgbSize, _albtFile);
			fflush(_albtFile);
		}

		DeleteMem(rgbBuf);
		return true;
	}

	// ---------------------------------------------------------------
	// WriteFrameSentinel -- 5 bytes
	// ---------------------------------------------------------------
	void WriteFrameSentinel(UInt32 frame)
	{
		if (!_albtFile)
			return;

		std::lock_guard<std::mutex> lock(_writeMutex);
		fwrite(&frame, 4, 1, _albtFile);
		UChar sentinel = PHASE_SENTINEL;
		fwrite(&sentinel, 1, 1, _albtFile);
		fflush(_albtFile);
	}

	// ---------------------------------------------------------------
	// SaveGridCell -- writes one tile record for a grid cell
	// ---------------------------------------------------------------
	void SaveGridCell(Render* render, UInt32 frame, UChar phase,
		Int32 cellX, Int32 cellY)
	{
		Int32 x, y, w, h;
		GetCellRect(cellX, cellY, x, y, w, h);
		if (w <= 0 || h <= 0)
			return;

		WriteTileRecord(render, frame, phase, x, y, w, h);
	}

	// ---------------------------------------------------------------
	// FlushAllCells -- re-save ALL cells as final + frame sentinel
	// ---------------------------------------------------------------
	void FlushAllCells(Render* render, UInt32 frame)
	{
		if (!render || !_albtFile)
			return;

		if (_sentinelWritten)
			return;  // multi-pass: already flushed this frame

		Int32 totalCells = _gridCols * _gridRows;
		for (Int32 i = 0; i < totalCells; i++)
		{
			Int32 cellX = i % _gridCols;
			Int32 cellY = i / _gridCols;
			SaveGridCell(render, frame, PHASE_FINAL, cellX, cellY);
			_grid[i].saved = true;
		}

		WriteFrameSentinel(frame);
		_sentinelWritten = true;

		String msg = "[BucketCapture] Flushed "_s;
		msg += String::IntToString(totalCells);
		msg += " final tiles + sentinel for frame "_s;
		msg += String::IntToString((Int32)frame);
		if (_debugPoll)
		{
			msg += " fpos="_s;
			msg += String::IntToString((Int32)ftell(_albtFile));
		}
		GePrint(msg);
	}

public:
	static NodeData* Alloc() { return NewObjClear(BucketCapturePost); }

	// Request per-scanline callbacks
	virtual VIDEOPOSTINFO GetRenderInfo(BaseVideoPost* node) override
	{
		return VIDEOPOSTINFO::EXECUTELINE;
	}

	// Accept all render engines except hardware preview
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

					// Read scene UUID from document
					_jobUUID = ""_s;
					BaseDocument* doc = node->GetDocument();
					if (doc)
					{
						const BaseContainer* bc = doc->GetDataInstance();
						if (bc)
							_jobUUID = bc->GetString(AMBERLIGHT_SCENE_UUID, ""_s);
					}

					if (_jobUUID.IsEmpty())
					{
						_jobUUID = "no-uuid"_s;
						GePrint("[BucketCapture] WARNING: No scene UUID found"_s);
					}

					// Create output directory (ALBT_STREAM_DIR env var or default)
					const char* envDir = getenv("ALBT_STREAM_DIR");
					String dirStr = envDir ? String(envDir) : "C:\\temp\\albt_streams"_s;
					Filename streamDir(dirStr);
					GeFCreateDir(streamDir);

					// Build path: {dir}\{uuid}.albt
					String pathStr = dirStr;
					pathStr += "\\"_s;
					pathStr += _jobUUID;
					pathStr += ".albt"_s;

					Char pathBuf[512];
					pathStr.GetCString(pathBuf, sizeof(pathBuf), STRINGENCODING::UTF8);
					_albtFile = fopen(pathBuf, "wb");

					if (!_albtFile)
					{
						GePrint("[BucketCapture] ERROR: Failed to open .albt file"_s);
					}
					else
					{
						String openMsg = "[BucketCapture] Opened "_s;
						openMsg += pathStr;
						GePrint(openMsg);
					}

					_streamStartTime = GeGetTimer();
					_headerWritten = false;
					_frameIndex = 0;
					_debugPoll = (getenv("ALBT_DEBUG_POLL") != nullptr);
				}
				else
				{
					GePrint("[BucketCapture] === FRAMESEQUENCE CLOSE ==="_s);
					StopPollThread();  // defensive: ensure thread stopped
					if (_albtFile)
					{
						fclose(_albtFile);
						_albtFile = nullptr;
					}
				}
				break;
			}

			case VIDEOPOSTCALL::FRAME:
			{
				if (vps->open)
				{
					if (vps->doc)
					{
						const Int32 fps = vps->doc->GetFps();
						_frameIndex = (Int32)vps->doc->GetTime().GetFrame(fps);
					}
					else
					{
						_frameIndex++;
					}
					_lineCount = 0;
					_activeRender = nullptr;
					_pollRunning.store(false);
					_baseline = nullptr;
					_sentinelWritten = false;
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
								Bool autoSize = bc->GetBool(7002, true);
								if (!autoSize)
								{
									_bucketSizeX = bc->GetInt32(7000, 64);
									_bucketSizeY = bc->GetInt32(7001, 64);
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

							_gridCols = (_frameW + _bucketSizeX - 1) / _bucketSizeX;
							_gridRows = (_frameH + _bucketSizeY - 1) / _bucketSizeY;

							Int32 totalCells = _gridCols * _gridRows;
							if (totalCells > MAX_GRID_CELLS)
								totalCells = MAX_GRID_CELLS;

							for (Int32 i = 0; i < totalCells; i++)
							{
								_grid[i].linesReceived = 0;
								_grid[i].saved = false;
							}

							// Write stream header on first RENDER open
							if (!_headerWritten && _albtFile)
							{
								WriteStreamHeader();
								_headerWritten = true;
							}

							String msg = "[BucketCapture] RENDER OPEN "_s;
							msg += String::IntToString(_frameW);
							msg += "x"_s;
							msg += String::IntToString(_frameH);
							msg += " grid="_s;
							msg += String::IntToString(_gridCols);
							msg += "x"_s;
							msg += String::IntToString(_gridRows);
							GePrint(msg);

							// Start VPBuffer poll thread (for GPU renderers)
							StartPollThread();
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
					// Stop poll thread before final flush
					StopPollThread();

					FlushAllCells(
						vps->render ? vps->render : _activeRender,
						(UInt32)_frameIndex);
				}
				break;
			}

			default:
				break;
		}

		return RENDERRESULT::OK;
	}

	// -----------------------------------------------------------------
	// ExecuteLine -- per-scanline pixel interception
	// Maps each scanline to its grid cell and increments the line counter.
	// When a cell has received all its expected lines -> write progressive tile.
	// Grid logic unchanged from V5. Only the save call changes (PNG -> .albt).
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
			Int32 expectedH = _bucketSizeY;
			Int32 cellBottom = (cellY + 1) * _bucketSizeY;
			if (cellBottom > _frameH)
				expectedH = _frameH - cellY * _bucketSizeY;

			if (cell.linesReceived >= expectedH && _activeRender)
			{
				cell.saved = true;
				SaveGridCell(_activeRender, (UInt32)_frameIndex,
					PHASE_PROGRESSIVE, cellX, cellY);
			}
		}

		// Log first 5 lines for debugging
		if (_lineCount <= 5)
		{
			String msg = "[BucketCapture] ExecuteLine: line="_s;
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
		String(),
		0,
		0
	);
}
