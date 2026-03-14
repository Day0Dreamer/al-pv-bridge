/*
	Bucket Capture -- VideoPostData for intercepting render buckets (v7)
	(C) Amber Light, 2026

	VideoPostData plugin that intercepts individual render buckets from
	Cinema 4D's render pipeline and writes them as an .albt binary stream
	(ADR-009).  Works with ALL renderers:

	  - Standard/Physical: ExecuteLine() per-scanline callbacks (multi-threaded)
	  - GPU renderers (Redshift, Octane, Arnold): VPBuffer polling thread
	    samples center-pixel per cell every 250ms to detect rendered regions

	Key findings:
	  - VIDEOPOSTCALL::TILE never fires (dead enum for Standard/Physical)
	  - ExecuteLine() IS the real bucket hook -- but only for CPU renderers
	  - GPU renderers skip ExecuteLine entirely; VPBuffer poll fills the gap
	  - Bucket boundaries detected via grid mapping (cellX = xmin/bucketW)
	  - VPBuffer safely readable at INNER close (single-threaded)
	  - Three-phase save: progressive from ExecuteLine OR poll, final at INNER

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
