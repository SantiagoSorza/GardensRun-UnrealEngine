// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroRenderSyncLibrary.h"

#include "Engine/Texture2D.h"
#include "RenderingThread.h"          // FlushRenderingCommands
#include "AssetCompilingManager.h"    // FAssetCompilingManager

void UNwiroRenderSyncLibrary::PrepareTextureForCapture(UTexture2D* Texture)
{
	if (!Texture)
	{
		return;
	}

	// Pure render-thread sync. UPROPERTY-change notifications (i.e.
	// PostEditChange) are caller's responsibility before invoking this.
	// Used by the HDRI bake recipe ([6b]) to settle a freshly-modified
	// source texture before SceneCapture samples it.
	Texture->bForceMiplevelsToBeResident       = true;
	Texture->bGlobalForceMipLevelsToBeResident = true;
	Texture->UpdateResource();
	Texture->WaitForStreaming();
	FlushRenderingCommands();
}

void UNwiroRenderSyncLibrary::FlushRendering()
{
	FlushRenderingCommands();
}

void UNwiroRenderSyncLibrary::FinishAssetCompilation()
{
	// Block until every async compiler (textures, shaders, materials, static
	// meshes, ...) has drained. Closes the gap WaitForStreaming leaves open:
	// a freshly-imported texture or just-recompiled material that is still
	// building when the HDRI material step samples it. Pumps the game thread
	// while waiting; HDRI re-entry is guarded by GNwiroHDRIRunning elsewhere.
	FAssetCompilingManager::Get().FinishAllCompilation();
}
