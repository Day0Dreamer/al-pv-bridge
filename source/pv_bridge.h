/*
	PV Bridge — Picture Viewer Bridge for Render Farm Streaming
	(C) Amber Light, 2026

	Exposes Cinema 4D's C++-only Picture Viewer API to Python via
	GePluginMessage. C++ owns all bitmaps and handles PV lifecycle.
	Python sends pixel data via CMD_WRITE_PIXELS using in-memory
	pointer transfer (same-process) or file fallback.

	Protocol: Python packs a BaseContainer with command + parameters,
	calls GePluginMessage(PV_BRIDGE_PLUGIN_ID, &bc). C++ processes the
	command synchronously and writes the result code back into the
	same container.

	NOTE: BaseBitmap cannot be stored in GeData/BaseContainer (no
	GetBitmap method in 2026 SDK). C++ allocates bitmaps on
	CMD_BEGIN_FRAME and writes pixels from data sent via CMD_WRITE_PIXELS.
*/

#ifndef PV_BRIDGE_H__
#define PV_BRIDGE_H__

#include "c4d.h"
#include "lib_pictureviewer.h"

// ---------------------------------------------------------------------------
// Plugin ID — must match al_plugin_ids.py PV_BRIDGE_PLUGIN_ID
// ---------------------------------------------------------------------------
static const cinema::Int32 PV_BRIDGE_PLUGIN_ID = 1067794;

// MessageData registration ID — only used for RegisterMessagePlugin uniqueness.
// NOT referenced from Python; the SpecialEventAdd dispatch checks PV_BRIDGE_PLUGIN_ID.
// Registered at plugincafe as "amberlight_pv_bridge_messagedata".
static const cinema::Int32 PV_BRIDGE_MSG_PLUGIN_ID = 1067797;

// ---------------------------------------------------------------------------
// BaseContainer field IDs — must match wrappers/c4d/picture_viewer/constants.py
// ---------------------------------------------------------------------------
static const cinema::Int32 FLD_COMMAND        = 1;   // Int32: command code
static const cinema::Int32 FLD_SESSION_NAME   = 2;   // String: human-readable name
static const cinema::Int32 FLD_FPS            = 3;   // Float: frames per second
static const cinema::Int32 FLD_FRAME_START    = 4;   // Int32: first frame number
static const cinema::Int32 FLD_FRAME_END      = 5;   // Int32: last frame number
static const cinema::Int32 FLD_FRAME_NUMBER   = 6;   // Int32: frame number (per-frame)
static const cinema::Int32 FLD_WIDTH          = 7;   // Int32: image width
static const cinema::Int32 FLD_HEIGHT         = 8;   // Int32: image height
// Field 9 reserved (originally planned for bitmap, but GeData has no bitmap support)
static const cinema::Int32 FLD_RESULT         = 10;  // Int32: return code
static const cinema::Int32 FLD_JOB_ID         = 11;  // String: render farm job ID
static const cinema::Int32 FLD_SESSION_HANDLE = 12;  // Int32: session index
static const cinema::Int32 FLD_BUCKET_X       = 13;  // Int32: bucket X offset
static const cinema::Int32 FLD_BUCKET_Y       = 14;  // Int32: bucket Y offset
static const cinema::Int32 FLD_BUCKET_W       = 15;  // Int32: bucket width
static const cinema::Int32 FLD_BUCKET_H       = 16;  // Int32: bucket height
static const cinema::Int32 FLD_PIXEL_DATA     = 17;  // Memory: raw RGB pixel bytes (C++ only)
static const cinema::Int32 FLD_PIXEL_FILE     = 18;  // Filename: path to raw RGB pixel file
static const cinema::Int32 FLD_FILL_R         = 19;  // Int32: fill color red (0-255)
static const cinema::Int32 FLD_FILL_G         = 20;  // Int32: fill color green (0-255)
static const cinema::Int32 FLD_FILL_B         = 21;  // Int32: fill color blue (0-255)

// ---------------------------------------------------------------------------
// In-memory pixel transfer fields (same-process pointer passing).
//
// Python and C++ share a single address space inside Cinema 4D.
// Instead of writing pixel data to a temp file, Python pins bytes in a
// ctypes buffer and passes the virtual address as Int64 through the
// BaseContainer. C++ casts it back to UChar* and reads directly.
//
// Safety contract:
//   - Synchronous paths: Python is blocked (CallCommand or poll loop),
//     so the buffer is alive on the Python stack for the entire dispatch.
//   - Fire-and-forget: Python pins the buffer in a module-level variable
//     (_pending_pixel_buf) and releases it only after _drain_pending()
//     confirms C++ has finished (FLD_RESULT != RESULT_PENDING).
//   - C++ MUST NOT cache or store the pointer beyond HandleWritePixels.
//     The pointer is only valid for the duration of a single dispatch.
//
// Fallback: If FLD_PIXEL_PTR is 0 (absent), HandleWritePixels falls
// back to FLD_PIXEL_FILE (temp file path) for backward compatibility.
// ---------------------------------------------------------------------------
static const cinema::Int32 FLD_PIXEL_PTR      = 22;  // Int64: virtual address of raw RGB pixel buffer
static const cinema::Int32 FLD_PIXEL_SIZE     = 23;  // Int64: byte count of pixel buffer

// ---------------------------------------------------------------------------
// Command codes
// ---------------------------------------------------------------------------
static const cinema::Int32 CMD_TEST_PV        = 50;   // Debug: ShowImage test
static const cinema::Int32 CMD_FILL_FRAME     = 60;   // Debug: fill frame with solid color
static const cinema::Int32 CMD_OPEN_SESSION   = 100;
static const cinema::Int32 CMD_BEGIN_FRAME    = 200;
static const cinema::Int32 CMD_WRITE_PIXELS   = 250;
static const cinema::Int32 CMD_END_FRAME      = 300;
static const cinema::Int32 CMD_CLOSE_SESSION  = 400;
static const cinema::Int32 CMD_QUERY_STATUS   = 500;

// ---------------------------------------------------------------------------
// Result / error codes
// ---------------------------------------------------------------------------
static const cinema::Int32 ERR_OK              = 0;
static const cinema::Int32 ERR_UNKNOWN_COMMAND = 1;
static const cinema::Int32 ERR_NO_SESSION      = 2;
static const cinema::Int32 ERR_SESSION_FULL    = 3;
static const cinema::Int32 ERR_PV_FAILED       = 4;
static const cinema::Int32 ERR_BITMAP_MISSING  = 5;
static const cinema::Int32 ERR_FRAME_OOB       = 6;  // frame out of bounds
static const cinema::Int32 ERR_ALLOC_FAILED    = 7;
static const cinema::Int32 ERR_PIXEL_DATA      = 8;  // pixel data missing or wrong size

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------
struct FrameState
{
	cinema::Int32       frameNumber;
	cinema::BaseBitmap* bitmap;     // C++ allocated, registered with PV
	cinema::GeListNode* pvNode;     // returned by BeginRendering
	cinema::Bool        begun;      // BeginRendering called
	cinema::Bool        ended;      // EndRendering called
};

struct SessionState
{
	cinema::PictureViewer* pv;
	cinema::GeListNode*    session;     // returned by OpenRendering
	cinema::String         sessionName;
	cinema::String         jobId;
	cinema::Int32          frameStart;
	cinema::Int32          frameEnd;
	cinema::Int32          width;
	cinema::Int32          height;
	cinema::Float          fps;
	FrameState*            frames;      // heap array, indexed by (frame - frameStart)
	cinema::Int32          frameCount;
	cinema::Bool           active;
};

static const cinema::Int32 kMaxSessions = 8;

// ---------------------------------------------------------------------------
// Handler declarations
// ---------------------------------------------------------------------------
// Public API (called from main.cpp)
cinema::Bool RegisterPVBridgeCommand();
cinema::Bool HandlePVBridgeMessage(cinema::Int32 id, void* data);
void CleanupAllSessions();
void DispatchPVBridgeCommand();

#endif // PV_BRIDGE_H__
