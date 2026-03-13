/*
	Bucket Capture — VideoPostData for intercepting render buckets
	(C) Amber Light, 2026

	VideoPostData plugin that intercepts individual render buckets from
	Cinema 4D's Standard/Physical renderer pipeline.

	Key findings:
	  - VIDEOPOSTCALL::TILE never fires (dead enum for Standard/Physical)
	  - ExecuteLine() IS the real bucket hook — per-scanline, multi-threaded
	  - Bucket boundaries detected via per-CPU state tracking (pp->cpu_num)
	  - VPBuffer safely readable at INNER close (single-threaded)

	Output: C:\temp\bucket_capture_cpp\ — individual bucket PNGs + full frames
*/

#ifndef BUCKET_CAPTURE_H__
#define BUCKET_CAPTURE_H__

#include "c4d.h"

// TEMPORARY dev ID — register at plugincafe.com for production
static const cinema::Int32 BUCKET_CAPTURE_PLUGIN_ID = 1064201;

cinema::Bool RegisterBucketCapture();

#endif // BUCKET_CAPTURE_H__
