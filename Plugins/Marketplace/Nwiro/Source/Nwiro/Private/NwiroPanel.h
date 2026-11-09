// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Docking/SDockTab.h"

class SWebBrowser;

/**
 * Editor panel for Nwiro — Embedded React frontend via WebBrowser widget.
 */
class FNwiroPanel : public TSharedFromThis<FNwiroPanel>
{
public:
	static TSharedPtr<FNwiroPanel> Instance;
	static TSharedPtr<SWebBrowser> WebBrowserWidget;

	static void Initialize();
	static void Shutdown();
	static void OpenPanel();

	/** Level-editor toolbar button visibility (persisted, default ON). The web
	 *  UI toggles it via the bridge; the setter refreshes the toolbar live. */
	static bool IsToolbarButtonEnabled();
	static void SetToolbarButtonEnabled(bool bEnabled);

	/** Add a log entry (shown in UE log) */
	static void AddLog(const FString& Message);

private:
	TSharedPtr<SDockTab> PanelDock;

	void RegisterMenus();
	TSharedRef<SDockTab> OnSpawnTab(const FSpawnTabArgs& SpawnTabArgs);
	void CreateBrowserWidget();
};
