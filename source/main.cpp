/*
	PV Bridge — Module entry points
	(C) Amber Light, 2026

	Components:
	  1. PV Bridge — Python-driven PV session lifecycle (CommandData + WorldPluginData)
	  2. Bucket Capture — VideoPostData for intercepting render buckets
*/

#include "c4d_plugin.h"
#include "c4d_resource.h"
#include "pv_bridge.h"
#include "bucket_capture.h"

// Forward declarations from pv_bridge.cpp
cinema::Bool RegisterPVBridgeCommand();
cinema::Bool HandlePVBridgeMessage(cinema::Int32 id, void* data);
void CleanupAllSessions();

cinema::Bool cinema::PluginStart()
{
	GePrint("PV Bridge plugin loaded"_s);

	if (!RegisterPVBridgeCommand())
	{
		GePrint("PV Bridge: RegisterCommandPlugin FAILED"_s);
		return false;
	}

	if (!RegisterBucketCapture())
		return false;

	return true;
}

void cinema::PluginEnd()
{
	CleanupAllSessions();
}

cinema::Bool cinema::PluginMessage(cinema::Int32 id, void* data)
{
	switch (id)
	{
		case C4DPL_INIT_SYS:
		{
			if (!cinema::g_resource.Init())
				return false;
			return true;
		}
	}

	// Dispatch PV Bridge messages (non-UI only; PV ops use CommandData)
	if (HandlePVBridgeMessage(id, data))
		return true;

	return false;
}
