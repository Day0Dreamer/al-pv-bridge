/*
	PV Bridge — CommandData-based PV API dispatcher
	(C) Amber Light, 2026

	Uses CommandData (triggered by c4d.CallCommand from Python) instead of
	PluginMessage. CommandData::Execute runs in a UI-safe context where
	PictureViewer operations (Open, OpenRendering, ShowImage, etc.) work
	correctly. PluginMessage runs at too low a level for GUI operations
	and causes deadlocks/crashes.

	Protocol:
	  1. Python writes command to SetWorldPluginData(PV_BRIDGE_PLUGIN_ID, bc)
	  2. Python calls c4d.CallCommand(PV_BRIDGE_PLUGIN_ID)
	  3. C++ CommandData::Execute reads from GetWorldPluginData
	  4. C++ processes, writes result back to SetWorldPluginData
	  5. Python reads result from GetWorldPluginData(PV_BRIDGE_PLUGIN_ID)

	C++ owns all BaseBitmaps. Python sends pixel data via
	CMD_WRITE_PIXELS using in-memory pointer transfer (same-process)
	or file fallback for backward compatibility.
*/

#include "pv_bridge.h"

using namespace cinema;

// ---------------------------------------------------------------------------
// Global session storage
// ---------------------------------------------------------------------------
static SessionState g_sessions[kMaxSessions];
static Bool g_initialized = false;

static void EnsureInitialized()
{
	if (g_initialized)
		return;

	for (Int32 i = 0; i < kMaxSessions; i++)
	{
		g_sessions[i].pv         = nullptr;
		g_sessions[i].session    = nullptr;
		g_sessions[i].frames     = nullptr;
		g_sessions[i].frameCount = 0;
		g_sessions[i].active     = false;
	}
	g_initialized = true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static Int32 FindFreeSession()
{
	for (Int32 i = 0; i < kMaxSessions; i++)
	{
		if (!g_sessions[i].active)
			return i;
	}
	return -1;
}

static Bool IsValidSession(Int32 handle)
{
	return handle >= 0 && handle < kMaxSessions && g_sessions[handle].active;
}

static void FreeSessionFrames(SessionState& sess)
{
	if (sess.frames)
	{
		// Free any bitmaps we allocated
		for (Int32 i = 0; i < sess.frameCount; i++)
		{
			if (sess.frames[i].bitmap)
			{
				BaseBitmap::Free(sess.frames[i].bitmap);
				sess.frames[i].bitmap = nullptr;
			}
		}
		DeleteMem(sess.frames);
		sess.frames = nullptr;
	}
	sess.frameCount = 0;
}

// ---------------------------------------------------------------------------
// CMD_TEST_PV (50) — Debug command
//
// Creates a PictureViewer, allocates a small colored bitmap, and calls
// ShowImage. Verifies the PV API works in CommandData context.
// ---------------------------------------------------------------------------
static void HandleTestPV(BaseContainer* bc)
{
	GePrint("[PVBridge] TEST_PV: Creating PictureViewer..."_s);

	PictureViewer* pv = CreatePictureViewer(PICTUREVIEWER_INIT_ACTIVE);
	if (!pv)
	{
		GePrint("[PVBridge] TEST_PV: CreatePictureViewer returned nullptr"_s);
		bc->SetInt32(FLD_RESULT, ERR_PV_FAILED);
		return;
	}

	GePrint("[PVBridge] TEST_PV: PV created, calling Open..."_s);

	Bool opened = pv->Open();
	String openMsg = "[PVBridge] TEST_PV: Open() returned "_s;
	openMsg += opened ? "true"_s : "false"_s;
	GePrint(openMsg);

	// Create a small test bitmap (32x32, bright red)
	BaseBitmap* bmp = BaseBitmap::Alloc();
	if (!bmp)
	{
		GePrint("[PVBridge] TEST_PV: BaseBitmap::Alloc failed"_s);
		bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
		return;
	}

	if (bmp->Init(32, 32) != IMAGERESULT::OK)
	{
		GePrint("[PVBridge] TEST_PV: BaseBitmap::Init failed"_s);
		BaseBitmap::Free(bmp);
		bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
		return;
	}

	// Fill with red
	for (Int32 y = 0; y < 32; y++)
	{
		for (Int32 x = 0; x < 32; x++)
		{
			bmp->SetPixel(x, y, 255, 0, 0);
		}
	}

	GePrint("[PVBridge] TEST_PV: Calling ShowImage..."_s);
	Bool shown = pv->ShowImage(bmp, "PVBridge Test Image"_s);

	String showMsg = "[PVBridge] TEST_PV: ShowImage returned "_s;
	showMsg += shown ? "true"_s : "false"_s;
	GePrint(showMsg);

	// PV copies the bitmap, so we free ours
	BaseBitmap::Free(bmp);

	bc->SetInt32(FLD_RESULT, shown ? ERR_OK : ERR_PV_FAILED);
}

// ---------------------------------------------------------------------------
// CMD_FILL_FRAME (60) — Debug command
//
// Fills a frame's bitmap with a solid color. Like pv-simulator but
// triggered from Python. Useful for testing without pixel transfer.
// Fields: SESSION_HANDLE, FRAME_NUMBER, FILL_R, FILL_G, FILL_B
// ---------------------------------------------------------------------------
static void HandleFillFrame(BaseContainer* bc)
{
	Int32 handle = bc->GetInt32(FLD_SESSION_HANDLE);
	if (!IsValidSession(handle))
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	SessionState& sess = g_sessions[handle];
	Int32 frame = bc->GetInt32(FLD_FRAME_NUMBER);

	Int32 idx = frame - sess.frameStart;
	if (idx < 0 || idx >= sess.frameCount)
	{
		bc->SetInt32(FLD_RESULT, ERR_FRAME_OOB);
		return;
	}

	FrameState& fs = sess.frames[idx];
	if (!fs.begun || !fs.bitmap)
	{
		bc->SetInt32(FLD_RESULT, ERR_BITMAP_MISSING);
		return;
	}

	Int32 r = bc->GetInt32(FLD_FILL_R);
	Int32 g = bc->GetInt32(FLD_FILL_G);
	Int32 b = bc->GetInt32(FLD_FILL_B);

	for (Int32 y = 0; y < sess.height; y++)
	{
		for (Int32 x = 0; x < sess.width; x++)
		{
			fs.bitmap->SetPixel(x, y, r, g, b);
		}
	}

	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// CMD_OPEN_SESSION (100)
//
// Creates a PictureViewer, opens it, allocates session state, and
// immediately calls OpenRendering to start the rendering session.
// ---------------------------------------------------------------------------
static void HandleOpenSession(BaseContainer* bc)
{
	EnsureInitialized();

	Int32 handle = FindFreeSession();
	if (handle < 0)
	{
		bc->SetInt32(FLD_RESULT, ERR_SESSION_FULL);
		return;
	}

	SessionState& sess = g_sessions[handle];

	// Read parameters
	sess.sessionName = bc->GetString(FLD_SESSION_NAME);
	sess.fps         = bc->GetFloat(FLD_FPS);
	sess.frameStart  = bc->GetInt32(FLD_FRAME_START);
	sess.frameEnd    = bc->GetInt32(FLD_FRAME_END);
	sess.width       = bc->GetInt32(FLD_WIDTH);
	sess.height      = bc->GetInt32(FLD_HEIGHT);
	sess.jobId       = bc->GetString(FLD_JOB_ID);
	sess.frameCount  = sess.frameEnd - sess.frameStart + 1;

	// Allocate frame state array
	iferr (sess.frames = NewMemClear(FrameState, sess.frameCount))
	{
		bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
		return;
	}

	// Initialize frame states
	for (Int32 i = 0; i < sess.frameCount; i++)
	{
		sess.frames[i].frameNumber = sess.frameStart + i;
		sess.frames[i].bitmap      = nullptr;
		sess.frames[i].pvNode      = nullptr;
		sess.frames[i].begun       = false;
		sess.frames[i].ended       = false;
	}

	// Create and open Picture Viewer
	GePrint("[PVBridge] OPEN_SESSION: Creating PV..."_s);
	sess.pv = CreatePictureViewer(PICTUREVIEWER_INIT_ACTIVE);
	if (!sess.pv)
	{
		FreeSessionFrames(sess);
		bc->SetInt32(FLD_RESULT, ERR_PV_FAILED);
		return;
	}

	GePrint("[PVBridge] OPEN_SESSION: Opening PV..."_s);
	Bool opened = sess.pv->Open();
	if (!opened)
	{
		GePrint("[PVBridge] OPEN_SESSION: PV Open() failed"_s);
		FreeSessionFrames(sess);
		sess.pv = nullptr;
		bc->SetInt32(FLD_RESULT, ERR_PV_FAILED);
		return;
	}

	// Open rendering session
	GePrint("[PVBridge] OPEN_SESSION: Calling OpenRendering..."_s);
	BaseContainer renderSettings;
	sess.session = sess.pv->OpenRendering(
		sess.sessionName,
		sess.fps,
		sess.frameStart,
		sess.frameEnd,  // lEnd = last frame number
		&renderSettings
	);

	if (!sess.session)
	{
		GePrint("[PVBridge] OPEN_SESSION: OpenRendering returned nullptr"_s);
		FreeSessionFrames(sess);
		sess.pv = nullptr;
		bc->SetInt32(FLD_RESULT, ERR_PV_FAILED);
		return;
	}

	sess.active = true;

	GePrint("[PVBridge] OPEN_SESSION: success"_s);
	bc->SetInt32(FLD_SESSION_HANDLE, handle);
	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// CMD_BEGIN_FRAME (200)
//
// C++ allocates a BaseBitmap and registers it with the PV.
// The bitmap starts black (zero-initialized). Python sends pixel
// data via CMD_WRITE_PIXELS; PV polls the bitmap automatically.
// ---------------------------------------------------------------------------
static void HandleBeginFrame(BaseContainer* bc)
{
	Int32 handle = bc->GetInt32(FLD_SESSION_HANDLE);
	if (!IsValidSession(handle))
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	SessionState& sess = g_sessions[handle];
	Int32 frame = bc->GetInt32(FLD_FRAME_NUMBER);

	// Validate frame is within session range
	Int32 idx = frame - sess.frameStart;
	if (idx < 0 || idx >= sess.frameCount)
	{
		bc->SetInt32(FLD_RESULT, ERR_FRAME_OOB);
		return;
	}

	FrameState& fs = sess.frames[idx];

	// Allocate bitmap
	fs.bitmap = BaseBitmap::Alloc();
	if (!fs.bitmap)
	{
		bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
		return;
	}

	if (fs.bitmap->Init(sess.width, sess.height) != IMAGERESULT::OK)
	{
		BaseBitmap::Free(fs.bitmap);
		fs.bitmap = nullptr;
		bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
		return;
	}

	// Bitmap starts black (zero-initialized) — appears as "rendering in progress"

	// Register bitmap with PV
	BaseTime time(frame, sess.fps);
	String name = "Frame "_s;
	name += String::IntToString(frame);

	fs.pvNode = sess.pv->BeginRendering(
		sess.session, fs.bitmap, name, time, frame,
		Filename(), false, Filename(), 0, STEREOTYPE::REGULAR
	);

	if (!fs.pvNode)
	{
		BaseBitmap::Free(fs.bitmap);
		fs.bitmap = nullptr;
		bc->SetInt32(FLD_RESULT, ERR_PV_FAILED);
		return;
	}

	fs.begun = true;
	fs.ended = false;

	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// CMD_WRITE_PIXELS (250)
//
// Writes a rectangle of pixels into the frame's bitmap.
//
// Two transfer paths (selected automatically):
//
//   1. In-memory pointer (FLD_PIXEL_PTR != 0):
//      Python and C++ share one address space inside Cinema 4D.
//      Python pins raw RGB bytes in a ctypes buffer and passes the
//      virtual address as Int64 via BaseContainer. C++ casts it back
//      to UChar* and reads directly — zero disk I/O, zero copies.
//
//      Buffer lifetime is guaranteed by Python:
//        - Synchronous (CallCommand): buffer is a local variable on
//          the Python stack; Python is blocked for the entire dispatch.
//        - Fire-and-forget (SpecialEventAdd + poll): Python pins the
//          buffer in _pending_pixel_buf and releases it only after
//          _drain_pending() confirms FLD_RESULT != RESULT_PENDING.
//
//      C++ MUST NOT cache or store the pointer. It is valid only for
//      the duration of this single HandleWritePixels call.
//
//   2. File fallback (FLD_PIXEL_PTR == 0):
//      Legacy path for backward compatibility. Python writes raw RGB
//      to a temp file (FLD_PIXEL_FILE). C++ reads it, paints pixels,
//      and deletes the file. Kept so that a newer C++ plugin can work
//      with an older Python wrapper that hasn't been updated yet.
//
// Pixel format: raw RGB bytes (3 bytes per pixel), row-major,
// top-to-bottom. Expected size: bw * bh * 3.
//
// Fields: SESSION_HANDLE, FRAME_NUMBER, BUCKET_X, BUCKET_Y,
//         BUCKET_W, BUCKET_H, PIXEL_PTR (Int64), PIXEL_SIZE (Int64),
//         PIXEL_FILE (Filename, fallback only)
// ---------------------------------------------------------------------------
static void HandleWritePixels(BaseContainer* bc)
{
	Int32 handle = bc->GetInt32(FLD_SESSION_HANDLE);
	if (!IsValidSession(handle))
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	SessionState& sess = g_sessions[handle];
	Int32 frame = bc->GetInt32(FLD_FRAME_NUMBER);

	Int32 idx = frame - sess.frameStart;
	if (idx < 0 || idx >= sess.frameCount)
	{
		bc->SetInt32(FLD_RESULT, ERR_FRAME_OOB);
		return;
	}

	FrameState& fs = sess.frames[idx];
	if (!fs.begun || !fs.bitmap)
	{
		bc->SetInt32(FLD_RESULT, ERR_BITMAP_MISSING);
		return;
	}

	// Read bucket rectangle
	Int32 bx = bc->GetInt32(FLD_BUCKET_X);
	Int32 by = bc->GetInt32(FLD_BUCKET_Y);
	Int32 bw = bc->GetInt32(FLD_BUCKET_W);
	Int32 bh = bc->GetInt32(FLD_BUCKET_H);

	Int expectedSize = (Int)bw * (Int)bh * 3;

	// --- Pixel source: pointer or file ---
	// pixelData points to the raw RGB bytes (owned by Python or by us).
	// ownedBuf is non-null only when we allocated the buffer (file path).
	const UChar* pixelData = nullptr;
	UChar* ownedBuf = nullptr;

	Int64 ptr = bc->GetInt64(FLD_PIXEL_PTR);
	if (ptr != 0)
	{
		// Path 1: In-memory pointer — same-process, zero-copy.
		// Python passed the virtual address of a ctypes buffer.
		// Safe to dereference: Python guarantees the buffer outlives
		// this dispatch (see docblock above for lifetime contract).
		Int64 bufSize = bc->GetInt64(FLD_PIXEL_SIZE);
		if (bufSize < expectedSize)
		{
			GePrint("[PVBridge] WRITE_PIXELS: pointer buffer too small"_s);
			bc->SetInt32(FLD_RESULT, ERR_PIXEL_DATA);
			return;
		}
		pixelData = reinterpret_cast<const UChar*>(ptr);
	}
	else
	{
		// Path 2: File fallback — legacy path for backward compatibility.
		Filename pixelFile = bc->GetFilename(FLD_PIXEL_FILE);
		if (!GeFExist(pixelFile))
		{
			GePrint("[PVBridge] WRITE_PIXELS: pixel file not found"_s);
			bc->SetInt32(FLD_RESULT, ERR_PIXEL_DATA);
			return;
		}

		AutoAlloc<BaseFile> file;
		if (!file)
		{
			bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
			return;
		}

		if (!file->Open(pixelFile, FILEOPEN::READ, FILEDIALOG::NONE))
		{
			GePrint("[PVBridge] WRITE_PIXELS: failed to open pixel file"_s);
			bc->SetInt32(FLD_RESULT, ERR_PIXEL_DATA);
			return;
		}

		iferr (ownedBuf = NewMemClear(UChar, expectedSize))
		{
			bc->SetInt32(FLD_RESULT, ERR_ALLOC_FAILED);
			return;
		}

		Int bytesRead = file->ReadBytes(ownedBuf, expectedSize);
		file->Close();

		if (bytesRead < expectedSize)
		{
			GePrint("[PVBridge] WRITE_PIXELS: pixel file too small"_s);
			DeleteMem(ownedBuf);
			bc->SetInt32(FLD_RESULT, ERR_PIXEL_DATA);
			return;
		}

		pixelData = ownedBuf;

		// Delete the pixel file after reading (cleanup)
		GeFKill(pixelFile);
	}

	// --- Shared: write pixels row by row using SetPixelCnt ---
	Int32 rowBytes = bw * 3;

	for (Int32 row = 0; row < bh; row++)
	{
		Int32 dstY = by + row;
		if (dstY >= sess.height)
			break;

		// SDK SetPixelCnt takes UChar* (non-const) but only reads the data.
		// const_cast is safe here — the bitmap copies pixels internally.
		fs.bitmap->SetPixelCnt(
			bx, dstY, bw,
			const_cast<UChar*>(pixelData + row * rowBytes),
			COLORBYTES_RGB,
			COLORMODE::RGB,
			PIXELCNT::NONE
		);
	}

	// Cleanup: only the file path allocates a buffer we own
	if (ownedBuf)
	{
		DeleteMem(ownedBuf);
	}

	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// CMD_END_FRAME (300)
//
// Finalizes a frame — tells PV this frame is complete.
// PV copies the bitmap (KEEP_NODE_AND_COPYBMP). C++ frees its bitmap.
// ---------------------------------------------------------------------------
static void HandleEndFrame(BaseContainer* bc)
{
	Int32 handle = bc->GetInt32(FLD_SESSION_HANDLE);
	if (!IsValidSession(handle))
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	SessionState& sess = g_sessions[handle];
	Int32 frame = bc->GetInt32(FLD_FRAME_NUMBER);

	Int32 idx = frame - sess.frameStart;
	if (idx < 0 || idx >= sess.frameCount)
	{
		bc->SetInt32(FLD_RESULT, ERR_FRAME_OOB);
		return;
	}

	FrameState& fs = sess.frames[idx];
	if (!fs.begun || !fs.pvNode)
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	if (fs.ended)
	{
		// Already ended — idempotent
		bc->SetInt32(FLD_RESULT, ERR_OK);
		return;
	}

	sess.pv->EndRendering(
		fs.pvNode,
		PVFRAME_FINISH::KEEP_NODE_AND_COPYBMP,
		false,
		nullptr,
		nullptr
	);

	fs.ended = true;

	// Free our bitmap — PV has its copy now
	if (fs.bitmap)
	{
		BaseBitmap::Free(fs.bitmap);
		fs.bitmap = nullptr;
	}

	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// CMD_CLOSE_SESSION (400)
//
// End any un-ended frames, close the rendering session, release resources.
// ---------------------------------------------------------------------------
static void HandleCloseSession(BaseContainer* bc)
{
	Int32 handle = bc->GetInt32(FLD_SESSION_HANDLE);
	if (!IsValidSession(handle))
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	SessionState& sess = g_sessions[handle];

	// End any frames that were begun but not ended
	for (Int32 i = 0; i < sess.frameCount; i++)
	{
		if (sess.frames[i].begun && !sess.frames[i].ended && sess.frames[i].pvNode)
		{
			sess.pv->EndRendering(
				sess.frames[i].pvNode,
				PVFRAME_FINISH::KEEP_NODE_AND_COPYBMP,
				false,
				nullptr,
				nullptr
			);
			sess.frames[i].ended = true;
		}
	}

	// Close the rendering session
	if (sess.session && sess.pv)
	{
		sess.pv->CloseRendering(sess.session);
	}

	// Clean up (FreeSessionFrames also frees bitmaps)
	FreeSessionFrames(sess);
	sess.pv         = nullptr;
	sess.session    = nullptr;
	sess.active     = false;

	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// CMD_QUERY_STATUS (500)
//
// Returns current session state. Light-weight diagnostic command.
// Uses FLD_FRAME_START for begun count, FLD_FRAME_END for ended count.
// ---------------------------------------------------------------------------
static void HandleQueryStatus(BaseContainer* bc)
{
	Int32 handle = bc->GetInt32(FLD_SESSION_HANDLE);
	if (!IsValidSession(handle))
	{
		bc->SetInt32(FLD_RESULT, ERR_NO_SESSION);
		return;
	}

	SessionState& sess = g_sessions[handle];

	Int32 begunCount = 0;
	Int32 endedCount = 0;
	for (Int32 i = 0; i < sess.frameCount; i++)
	{
		if (sess.frames[i].begun)
			begunCount++;
		if (sess.frames[i].ended)
			endedCount++;
	}

	bc->SetString(FLD_SESSION_NAME, sess.sessionName);
	bc->SetString(FLD_JOB_ID, sess.jobId);
	bc->SetInt32(FLD_FRAME_START, begunCount);
	bc->SetInt32(FLD_FRAME_END, endedCount);
	bc->SetInt32(FLD_WIDTH, sess.width);
	bc->SetInt32(FLD_HEIGHT, sess.height);
	bc->SetInt32(FLD_RESULT, ERR_OK);
}

// ---------------------------------------------------------------------------
// Command dispatcher — shared by CommandData::Execute and MessageData::CoreMessage
//
// Command serialization is enforced by _command_lock in bridge.py.
// All Python callers are serialized before reaching WorldPluginData.
//
// Reads command from WorldPluginData, dispatches to handler,
// writes result back to WorldPluginData.
// ---------------------------------------------------------------------------
void DispatchPVBridgeCommand()
{
	BaseContainer* wpd = GetWorldPluginData(PV_BRIDGE_PLUGIN_ID);
	if (!wpd)
	{
		GePrint("[PVBridge] ERROR: No WorldPluginData found"_s);
		return;
	}

	// Copy the data so we can write results back
	BaseContainer bc = *wpd;
	Int32 cmd = bc.GetInt32(FLD_COMMAND);

	switch (cmd)
	{
		case CMD_TEST_PV:
			GePrint("[PVBridge] -> TEST_PV"_s);
			HandleTestPV(&bc);
			break;
		case CMD_FILL_FRAME:
			GePrint("[PVBridge] -> FILL_FRAME"_s);
			HandleFillFrame(&bc);
			break;
		case CMD_OPEN_SESSION:
			GePrint("[PVBridge] -> OPEN_SESSION"_s);
			HandleOpenSession(&bc);
			break;
		case CMD_BEGIN_FRAME:
			HandleBeginFrame(&bc);
			break;
		case CMD_WRITE_PIXELS:
			HandleWritePixels(&bc);
			break;
		case CMD_END_FRAME:
			HandleEndFrame(&bc);
			break;
		case CMD_CLOSE_SESSION:
			GePrint("[PVBridge] -> CLOSE_SESSION"_s);
			HandleCloseSession(&bc);
			break;
		case CMD_QUERY_STATUS:
			HandleQueryStatus(&bc);
			break;
		default:
		{
			String unknownMsg = "[PVBridge] -> UNKNOWN cmd="_s;
			unknownMsg += String::IntToString(cmd);
			GePrint(unknownMsg);
			bc.SetInt32(FLD_RESULT, ERR_UNKNOWN_COMMAND);
			break;
		}
	}

	// Write result back to WorldPluginData for Python to read
	SetWorldPluginData(PV_BRIDGE_PLUGIN_ID, bc, false);
}

// ---------------------------------------------------------------------------
// CommandData plugin — UI-safe execution context
//
// Python triggers this via c4d.CallCommand(PV_BRIDGE_PLUGIN_ID).
// CommandData::Execute runs in the same context as menu items and
// toolbar buttons, so PictureViewer UI operations are safe here.
// ---------------------------------------------------------------------------
class PVBridgeCommand : public CommandData
{
public:
	Bool Execute(BaseDocument* doc, GeDialog* parentManager) override
	{
		DispatchPVBridgeCommand();
		return true;
	}

	Int32 GetState(BaseDocument* doc, GeDialog* parentManager) override
	{
		return CMD_ENABLED;
	}
};

// ---------------------------------------------------------------------------
// MessageData plugin — main-thread dispatch for background threads
//
// When Python runs on a background thread (e.g. MCP), it can't call
// CallCommand safely. Instead it calls SpecialEventAdd(PV_BRIDGE_PLUGIN_ID)
// which posts an event to the main thread's event queue. CoreMessage
// picks it up and dispatches the PV command on the main thread.
//
// Python sets FLD_RESULT = -999 (sentinel) before posting. After C++
// processes the command, it sets the real result code. Python polls
// WorldPluginData until FLD_RESULT != -999.
// ---------------------------------------------------------------------------
static const Int32 RESULT_PENDING = -999;

class PVBridgeMessageData : public MessageData
{
public:
	Bool CoreMessage(Int32 id, const BaseContainer& bc) override
	{
		// SpecialEventAdd(PV_BRIDGE_PLUGIN_ID) sends our ID directly as 'id'
		if (id != PV_BRIDGE_PLUGIN_ID)
			return true;

		// Check if there's a pending command (sentinel = -999)
		BaseContainer* wpd = GetWorldPluginData(PV_BRIDGE_PLUGIN_ID);
		if (!wpd)
			return true;

		if (wpd->GetInt32(FLD_RESULT) != RESULT_PENDING)
			return true;  // No pending command, or already processed

		DispatchPVBridgeCommand();

		return true;
	}
};

// ---------------------------------------------------------------------------
// Registration — called from PluginStart
// ---------------------------------------------------------------------------
Bool RegisterPVBridgeCommand()
{
	// Command plugin for main-thread callers (CallCommand)
	if (!RegisterCommandPlugin(
		PV_BRIDGE_PLUGIN_ID,
		"PV Bridge"_s,
		PLUGINFLAG_HIDEPLUGINMENU,
		nullptr,
		"PV Bridge command interface"_s,
		NewObjClear(PVBridgeCommand)
	))
		return false;

	// MessageData for background-thread callers (SpecialEventAdd)
	if (!RegisterMessagePlugin(
		PV_BRIDGE_MSG_PLUGIN_ID,
		"PV Bridge Message Handler"_s,
		0,
		NewObjClear(PVBridgeMessageData)
	))
		return false;

	return true;
}

// ---------------------------------------------------------------------------
// PluginMessage handler — kept for non-UI commands only
//
// IMPORTANT: Do NOT call PictureViewer APIs from here — they deadlock.
// This handler only processes data-only commands (QUERY_STATUS) or
// acts as a no-op for the PV_BRIDGE_PLUGIN_ID.
// ---------------------------------------------------------------------------
Bool HandlePVBridgeMessage(Int32 id, void* data)
{
	if (id != PV_BRIDGE_PLUGIN_ID)
		return false;

	// PluginMessage is NOT safe for PV operations.
	// All commands go through CallCommand -> CommandData::Execute.
	// This handler exists only to claim the message ID.
	return true;
}

// ---------------------------------------------------------------------------
// Cleanup — called from PluginEnd
// ---------------------------------------------------------------------------
void CleanupAllSessions()
{
	for (Int32 i = 0; i < kMaxSessions; i++)
	{
		if (g_sessions[i].active)
		{
			// Best-effort cleanup — end frames and close session
			for (Int32 f = 0; f < g_sessions[i].frameCount; f++)
			{
				if (g_sessions[i].frames[f].begun &&
					!g_sessions[i].frames[f].ended &&
					g_sessions[i].frames[f].pvNode &&
					g_sessions[i].pv)
				{
					g_sessions[i].pv->EndRendering(
						g_sessions[i].frames[f].pvNode,
						PVFRAME_FINISH::KEEP_NODE_AND_COPYBMP,
						false, nullptr, nullptr
					);
				}
			}

			if (g_sessions[i].session && g_sessions[i].pv)
			{
				g_sessions[i].pv->CloseRendering(g_sessions[i].session);
			}

			FreeSessionFrames(g_sessions[i]);
			g_sessions[i].active = false;
		}
	}
}
