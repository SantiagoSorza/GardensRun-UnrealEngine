// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Movie Render Queue / Movie Render Graph still-render tools.
 *
 * Renders one frame from a named camera actor (or the current viewport) through
 * the provisioned SCG Movie Render Graph and writes the model-ready bundle:
 * depth.jpeg (reciprocal depth, near=white), normals.jpeg, a tonemapped
 * preview.jpeg, and a camera.json sidecar (no EXR).
 *
 * Rendering is asynchronous because MRQ needs game-loop ticks — blocking the
 * game thread on it deadlocks. render_still_passes submits and returns a
 * job_id immediately; get_render_job_status polls it; cancel_render_job
 * requests an executor cancel.
 */
class FNwiroRenderTools
{
public:
	// Submit an async still render. Returns {"job_id":...} immediately.
	static FString RenderStillPasses(const FString& JsonCommand);

	// Submit an async SEQUENCE render (video): renders an existing LevelSequence
	// through the provisioned /Game movie graph into depth.mp4 + render_meta.json.
	// The sequence's camera cut owns the viewpoint and the movie graph owns the
	// output resolution (no size parameters); every graph setting the tool
	// touches is restored afterwards. Windows-only (the engine MP4 encoder).
	// Same job registry as the still path: poll/cancel with the tools below.
	static FString RenderPassSequence(const FString& JsonCommand);

	// Poll job state: running | succeeded | failed, plus output dir + found files.
	static FString GetRenderJobStatus(const FString& JsonCommand);

	// Request cancellation of a running render job.
	static FString CancelRenderJob(const FString& JsonCommand);

	// Copy the shipped SCG render assets (depth material instance + still/movie
	// graphs) into /Game/NwiroAssets/SceneGeneration under versioned names
	// (copy-if-absent, never overwrites; a new version is a new copy), point each
	// fresh graph's depth pass at the depth material copy, then validate +
	// version-stamp. Idempotent; safe to call before every render.
	// Returns {"success":..., "copied":[...], "skipped":[...], "warnings":[...]}.
	static FString EnsureProvisioned(const FString& JsonCommand);
};
