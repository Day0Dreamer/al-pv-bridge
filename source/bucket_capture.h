/*
	Bucket Capture -- VideoPostData for intercepting render buckets
	(C) Amber Light, 2026

	VideoPostData plugin that intercepts individual render buckets from
	Cinema 4D's Standard/Physical renderer pipeline and writes them as
	an .albt binary stream (ADR-009).

	Key findings:
	  - VIDEOPOSTCALL::TILE never fires (dead enum for Standard/Physical)
	  - ExecuteLine() IS the real bucket hook -- per-scanline, multi-threaded
	  - Bucket boundaries detected via grid mapping (cellX = xmin/bucketW)
	  - VPBuffer safely readable at INNER close (single-threaded)
	  - Two-phase save: progressive from ExecuteLine, final at INNER close

	Output: C:\temp\albt_streams\{uuid}.albt
	Format: docs/adr/009-binary-tile-stream-for-render-farm.md
*/

#ifndef BUCKET_CAPTURE_H__
#define BUCKET_CAPTURE_H__

#include "c4d.h"

// TEMPORARY dev ID -- register at plugincafe.com for production
static const cinema::Int32 BUCKET_CAPTURE_PLUGIN_ID = 1064201;

// Scene UUID container ID -- matches al_plugin_ids.py AMBERLIGHT_SCENE_UUID
static const cinema::Int32 AMBERLIGHT_SCENE_UUID = 1065288;

cinema::Bool RegisterBucketCapture();

#endif // BUCKET_CAPTURE_H__
