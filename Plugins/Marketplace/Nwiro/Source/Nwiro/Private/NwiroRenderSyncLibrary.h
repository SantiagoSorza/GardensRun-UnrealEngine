// Copyright 2026 Nwiro. All Rights Reserved.

// Nwiro — render-thread synchronization primitives exposed to Python.
//
// Background: UE Python does not expose FlushRenderingCommands or
// UTexture2D::WaitForStreaming. The HDRI bake pipeline (and any other
// future feature that captures into a scene-capture render target) needs
// real synchronization, not time.sleep guesses, to be reliable across
// hardware and editor states.
//
// This library exposes exactly two static UFUNCTIONs to Python:
//   * PrepareTextureForCapture — force a texture fully resident and wait
//     for the render thread + streaming to settle. Use after modifying a
//     UTexture2D's UPROPERTYs (compression, address modes, force-resident
//     flags) and before sampling it in a SceneCapture pass.
//   * FlushRendering — block until the render thread has finished prior
//     commands. Use after triggering CaptureScene to ensure the capture
//     pass actually completed before reading from the RT.
//
// Contains zero HDRI-specific logic. Generic render-sync primitives that
// can be reused by any Python-orchestrated capture workflow.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NwiroRenderSyncLibrary.generated.h"

class UTexture2D;

UCLASS()
class UNwiroRenderSyncLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Force the texture's top mip resident and synchronously wait for the
	 * runtime resource + render thread to settle.
	 *
	 * Internally calls:
	 *   UpdateResource()          — rebuild the GPU resource from current settings
	 *   WaitForStreaming()        — block until streaming finishes
	 *   FlushRenderingCommands()  — block until the render thread is idle
	 *
	 * Caller is responsible for any PostEditChange() before invoking — this
	 * helper's job is render-thread sync only. Used by the HDRI bake recipe
	 * ([6b]) to settle a freshly-modified source texture before SceneCapture
	 * samples it.
	 *
	 * Safe to call on a null texture (no-op). Call from the game thread only.
	 *
	 * @param Texture  the working source texture about to be sampled in a capture
	 */
	UFUNCTION(BlueprintCallable, Category = "Nwiro|Render")
	static void PrepareTextureForCapture(UTexture2D* Texture);

	/**
	 * Block until the render thread has finished all prior commands.
	 *
	 * Equivalent to a single FlushRenderingCommands() call. Use after
	 * triggering CaptureScene to guarantee the capture pass completed
	 * before reading from the render target.
	 *
	 * Call from the game thread only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nwiro|Render")
	static void FlushRendering();

	/**
	 * Block until ALL pending async asset compilation has finished — textures,
	 * shaders, materials, static meshes, etc.
	 *
	 * Wraps FAssetCompilingManager::Get().FinishAllCompilation(). UE5 compiles
	 * textures and shaders on background threads, so a freshly-imported source
	 * texture (or a just-recompiled material) can still be building when the HDRI
	 * bake's MaterialEditingLibrary calls run against it. WaitForStreaming (in
	 * PrepareTextureForCapture) only waits for mip streaming of an already-BUILT
	 * texture — it does NOT wait for the async build/compile. This closes that gap.
	 *
	 * Defensive barrier for the HDRI bake: the confirmed bake crash was a
	 * per-bake Python state problem (fixed in pythonEmitter.ts via globals
	 * isolation); this guards against the separate, plausible async-compile race
	 * rather than a proven crash. The emitter calls it before binding a
	 * freshly-imported texture into a material, and again after recompile_material
	 * so the shader compile settles before capture.
	 *
	 * Note: FinishAllCompilation pumps the game thread while waiting, so it can
	 * re-enter editor tick callbacks — HDRI re-entry is guarded separately
	 * (GNwiroHDRIRunning). Call from the game thread only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nwiro|Render")
	static void FinishAssetCompilation();
};
