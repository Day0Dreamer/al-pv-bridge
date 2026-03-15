/*
	Auto-Inject — SceneHookData that auto-injects BucketCapture VideoPost
	(C) Amber Light, 2026

	Automatically inserts the BucketCapture VideoPost into every RenderData
	in the active document so that bucket capture is "always on."

	Three injection triggers ensure complete coverage:

	  1. MSG_DOCUMENTINFO (document load, merge, set-active, new project)
	     → fires once per lifecycle event, immediate injection

	  2. SceneHookData::Execute() in the priority pipeline
	     → catches edge cases (e.g. new RenderData created mid-session)
	     → runs before rendering starts, so no race condition

	  3. PluginMessage(C4DPL_PROGRAM_STARTED) via AutoInjectOnStartup()
	     → catches the initial document at Cinema 4D launch

	Toggle: ALBT_AUTO_INJECT env var (default: disabled, "1" to enable)
*/

#include "auto_inject.h"
#include "bucket_capture.h"
#include "c4d_scenehookdata.h"

using namespace cinema;

// ---------------------------------------------------------------------------
// IsAutoInjectEnabled — check ALBT_AUTO_INJECT env var
// Default: disabled.  Set ALBT_AUTO_INJECT=1 to enable.
// ---------------------------------------------------------------------------
static Bool IsAutoInjectEnabled()
{
	const char* env = getenv("ALBT_AUTO_INJECT");
	if (env && env[0] == '1')
		return true;
	return false;
}

// ---------------------------------------------------------------------------
// HasBucketCapturePost — check if a RenderData already has our VideoPost
// ---------------------------------------------------------------------------
static Bool HasBucketCapturePost(RenderData* rd)
{
	BaseVideoPost* vp = rd->GetFirstVideoPost();
	while (vp)
	{
		if (vp->GetType() == BUCKET_CAPTURE_PLUGIN_ID)
			return true;
		vp = vp->GetNext();
	}
	return false;
}

// ---------------------------------------------------------------------------
// EnsureVideoPostInRenderData — insert BucketCapture VP if missing
// Returns true if a new VideoPost was inserted.
// ---------------------------------------------------------------------------
static Bool EnsureVideoPostInRenderData(RenderData* rd)
{
	if (HasBucketCapturePost(rd))
		return false;

	BaseVideoPost* vp = BaseVideoPost::Alloc(BUCKET_CAPTURE_PLUGIN_ID);
	if (!vp)
		return false;

	// Insert last so it doesn't interfere with user's Effect ordering
	rd->InsertVideoPostLast(vp);
	return true;
}

// ---------------------------------------------------------------------------
// InjectIntoAllRenderData — traverse entire RenderData tree
// RenderData can have siblings (GetNext) and children (GetDown).
// ---------------------------------------------------------------------------
static Int32 InjectIntoAllRenderData(RenderData* rd)
{
	Int32 count = 0;
	while (rd)
	{
		if (EnsureVideoPostInRenderData(rd))
			count++;

		// Recurse into children (nested render settings)
		RenderData* child = static_cast<RenderData*>(rd->GetDown());
		if (child)
			count += InjectIntoAllRenderData(child);

		rd = static_cast<RenderData*>(rd->GetNext());
	}
	return count;
}

// ---------------------------------------------------------------------------
// InjectIntoDocument — main entry point for injection
// ---------------------------------------------------------------------------
static void InjectIntoDocument(BaseDocument* doc)
{
	if (!doc)
		return;

	if (!IsAutoInjectEnabled())
		return;

	RenderData* firstRD = doc->GetFirstRenderData();
	if (!firstRD)
		return;

	Int32 count = InjectIntoAllRenderData(firstRD);

	if (count > 0)
	{
		String msg = "[AutoInject] Inserted BucketCapture into "_s;
		msg += String::IntToString(count);
		msg += " RenderData(s)"_s;
		GePrint(msg);
	}
}

// ---------------------------------------------------------------------------
// AutoInjectSceneHook — SceneHookData implementation
// ---------------------------------------------------------------------------
class AutoInjectSceneHook : public SceneHookData
{
	INSTANCEOF(AutoInjectSceneHook, SceneHookData)

public:
	static NodeData* Alloc() { return NewObjClear(AutoInjectSceneHook); }

	// ---------------------------------------------------------------
	// Message — catch document lifecycle events
	// ---------------------------------------------------------------
	Bool Message(GeListNode* node, Int32 type, void* data) override
	{
		if (type == MSG_DOCUMENTINFO && data)
		{
			DocumentInfoData* did = static_cast<DocumentInfoData*>(data);

			switch (did->type)
			{
				case MSG_DOCUMENTINFO_TYPE_LOAD:
				case MSG_DOCUMENTINFO_TYPE_MERGE:
				case MSG_DOCUMENTINFO_TYPE_SETACTIVE:
				case MSG_DOCUMENTINFO_TYPE_NEWPROJECT_AFTER:
				{
					InjectIntoDocument(did->doc);
					break;
				}
				default:
					break;
			}
		}

		return SceneHookData::Message(node, type, data);
	}

	// ---------------------------------------------------------------
	// Execute — runs in priority pipeline before rendering
	// Catches RenderData created mid-session that MSG_DOCUMENTINFO missed.
	// Early-returns quickly if all RenderData already have our VP.
	// ---------------------------------------------------------------
	EXECUTIONRESULT Execute(BaseSceneHook* node, BaseDocument* doc,
		BaseThread* bt, Int32 priority, EXECUTIONFLAGS flags) override
	{
		InjectIntoDocument(doc);
		return EXECUTIONRESULT::OK;
	}
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
Bool RegisterAutoInjectSceneHook()
{
	return RegisterSceneHookPlugin(
		AUTO_INJECT_SCENEHOOK_ID,
		"BucketCapture Auto-Inject"_s,
		0,                              // no special flags
		AutoInjectSceneHook::Alloc,
		EXECUTIONPRIORITY_INITIAL,      // run early in pipeline
		0                               // disklevel
	);
}

// ---------------------------------------------------------------------------
// AutoInjectOnStartup — called from PluginMessage(C4DPL_PROGRAM_STARTED)
// ---------------------------------------------------------------------------
void AutoInjectOnStartup()
{
	BaseDocument* doc = GetActiveDocument();
	InjectIntoDocument(doc);

	// Log transport mode once at startup
	const char* sinkUrl = getenv("ALBT_SINK_URL");
	if (sinkUrl && sinkUrl[0] != '\0')
	{
		String msg = "[BucketCapture] Transport: TCP -> "_s;
		msg += String(sinkUrl);
		GePrint(msg);
	}
	else
	{
		const char* envDir = getenv("ALBT_STREAM_DIR");
		String msg = "[BucketCapture] Transport: FILE -> "_s;
		msg += envDir ? String(envDir) : "C:\\temp\\albt_streams"_s;
		GePrint(msg);
	}
}
