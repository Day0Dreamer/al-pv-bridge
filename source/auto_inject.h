/*
	Auto-Inject — SceneHookData that auto-injects BucketCapture VideoPost
	(C) Amber Light, 2026

	Registers a SceneHookData plugin that automatically inserts the
	BucketCapture VideoPost into every RenderData in the document.
	This ensures bucket capture is "always on" without manual setup.

	Controlled by environment variable ALBT_AUTO_INJECT:
	  - Not set or "1": enabled (default)
	  - "0": disabled

	Injection triggers:
	  1. MSG_DOCUMENTINFO (document load, merge, set-active)
	  2. SceneHookData::Execute() in priority pipeline (catches edge cases)
	  3. PluginMessage(C4DPL_PROGRAM_STARTED) for the initial document
*/

#ifndef AUTO_INJECT_H__
#define AUTO_INJECT_H__

#include "c4d.h"

static const cinema::Int32 AUTO_INJECT_SCENEHOOK_ID = 1064203;

cinema::Bool RegisterAutoInjectSceneHook();

// Called from PluginMessage(C4DPL_PROGRAM_STARTED) for the initial document
void AutoInjectOnStartup();

#endif // AUTO_INJECT_H__
