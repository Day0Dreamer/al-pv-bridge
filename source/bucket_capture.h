/*
	Bucket Capture -- VideoPostData for intercepting render buckets (v8)
	(C) Amber Light, 2026

	VideoPostData plugin that intercepts individual render buckets from
	Cinema 4D's render pipeline and streams them as .albt binary records.

	  - Standard/Physical: ExecuteLine() per-scanline callbacks (multi-threaded)
	  - GPU renderers (Redshift, Octane, Arnold): VPBuffer polling thread
	    samples center-pixel per cell every 250ms to detect rendered regions

	Transport (v8):
	  - Primary: TCP socket to an ALBT sink process (set ALBT_SINK_URL=host:port)
	  - Fallback: local file write (original v7 behaviour) when no sink configured

	Output format: .albt v1 binary stream
	See: docs/adr/009-binary-tile-stream-for-render-farm.md
*/

#ifndef BUCKET_CAPTURE_H__
#define BUCKET_CAPTURE_H__

#include "c4d.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

static const cinema::Int32 BUCKET_CAPTURE_PLUGIN_ID = 1067795;

// Scene UUID container ID -- matches al_plugin_ids.py AMBERLIGHT_SCENE_UUID
static const cinema::Int32 AMBERLIGHT_SCENE_UUID = 1065288;

// Winsock lifecycle (called from main.cpp PluginStart/PluginEnd)
cinema::Bool InitWinsock();
void CleanupWinsock();

cinema::Bool RegisterBucketCapture();

#endif // BUCKET_CAPTURE_H__
