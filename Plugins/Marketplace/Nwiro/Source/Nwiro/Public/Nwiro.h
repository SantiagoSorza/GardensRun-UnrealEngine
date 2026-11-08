// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "AssetTypeActions_Base.h"

class IAssetEditorInstance;

class FNwiroModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnAssetOpenedInEditor(UObject* Asset, IAssetEditorInstance* EditorInstance);

	TSharedPtr<FAssetTypeActions_Base> NwiroGraphAssetTypeActions;
	TSharedPtr<FAssetTypeActions_Base> NwiroPCGGraphTypeActions;
	FDelegateHandle AssetEditorMonitorHandle;
	// Handle for OnEnginePreExit — Slate-owned panel teardown runs there (while
	// Slate is guaranteed alive) instead of relying on ShutdownModule ordering.
	FDelegateHandle EnginePreExitHandle;
};
