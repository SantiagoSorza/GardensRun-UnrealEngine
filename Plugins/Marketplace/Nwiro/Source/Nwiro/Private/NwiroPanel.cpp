// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroPanel.h"
#include "NwiroBridge.h"
#include "NwiroImageDrop.h"
#include "NwiroStyle.h"
#include "LevelEditor.h"
#include "SWebBrowser.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ConfigCacheIni.h"

static const FName NwiroTabName("Nwiro");
static const FText NwiroTabDisplay = FText::FromString("Nwiro Pro");
static const FText NwiroTabDesc = FText::FromString("Nwiro Pro");
static const FString FrontendURL = TEXT("https://app.nwiro.ai");

// Persisted toolbar-button visibility (per-project ini). Default ON.
static const TCHAR* ToolbarIniSection = TEXT("Nwiro.UI");
static const TCHAR* ToolbarIniKey = TEXT("ShowToolbarButton");

TSharedPtr<FNwiroPanel> FNwiroPanel::Instance;
TSharedPtr<SWebBrowser> FNwiroPanel::WebBrowserWidget = nullptr;

void FNwiroPanel::Initialize()
{
	if (!Instance.IsValid())
	{
		Instance = MakeShareable(new FNwiroPanel);
		Instance->RegisterMenus();
	}
}

void FNwiroPanel::Shutdown()
{
	if (Instance.IsValid())
	{
		// Unbind UE's text input method system on editor-quit path.
		// Mirrors OnTabClosed_Lambda — Shutdown fires when quitting with
		// the panel still open, OnTabClosed fires when the user closes the
		// tab manually. Both need the unbind to prevent dangling-context
		// crash on subsequent async IME focus events.
		//
		// Fix Point E (proactive, mirrors NwiroIKPanel): defensive Slate-
		// availability guard. SWebBrowser::UnbindInputMethodSystem internals
		// are unverifiable from outside Epic; if Slate is torn down here, the
		// bound ITextInputMethodSystem* (which came from Slate) is invalid
		// anyway, so skipping the unbind is correct cleanup, not a leak.
		// Scope note: this guard catches the Slate-gone teardown class only;
		// it does NOT protect against widget-internal CEF IME state corruption.
		if (WebBrowserWidget.IsValid() && FSlateApplication::IsInitialized())
		{
			WebBrowserWidget->UnbindInputMethodSystem();
		}
		WebBrowserWidget.Reset();
		// Precautionary: FGlobalTabmanager is owned by Slate; guard against
		// edge-case teardown ordering when Shutdown() runs after Slate has been
		// destroyed. Same defensive pattern applied to NwiroIKPanel (Fix Point B)
		// — proactively applied here for symmetry per council audit, even though
		// no confirmed crash report exists in the Pro plugin.
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(NwiroTabName);
		}
		// Editor exit: stop every adapter process deterministically, release the
		// root so BeginDestroy runs on the final purge, and null the singleton so
		// any post-teardown async path sees nullptr instead of a stale pointer.
		if (UNwiroBridge::Instance)
		{
			UNwiroBridge::Instance->OnPanelDetached();
			UNwiroBridge::Instance->StopAllAdapterProcesses(TEXT("editor_exit"));
			UNwiroBridge::Instance->RemoveFromRoot();
		}
		UNwiroBridge::Instance = nullptr;
		Instance.Reset();
	}
}

void FNwiroPanel::OpenPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(NwiroTabName);
}

bool FNwiroPanel::IsToolbarButtonEnabled()
{
	bool bShow = true; // default ON
	if (GConfig)
	{
		GConfig->GetBool(ToolbarIniSection, ToolbarIniKey, bShow, GEditorPerProjectIni);
	}
	return bShow;
}

void FNwiroPanel::SetToolbarButtonEnabled(bool bEnabled)
{
	if (GConfig)
	{
		GConfig->SetBool(ToolbarIniSection, ToolbarIniKey, bEnabled, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
	// Rebuild the level-editor toolbar so the dynamic entry re-runs and the
	// button appears/disappears without an editor restart.
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->RefreshAllWidgets();
	}
}

void FNwiroPanel::AddLog(const FString& Message)
{
}

void FNwiroPanel::RegisterMenus()
{
	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("MainFrame.MainMenu.Window");
	// Place Nwiro under its own "Assistant" category instead of "Get Content"
	// so it's easier to find and visually separate from marketplace tabs.
	FToolMenuSection* ContentSectionPtr = WindowMenu->FindSection("NwiroAssistant");
	if (!ContentSectionPtr)
	{
		ContentSectionPtr = &WindowMenu->AddSection("NwiroAssistant",
			NSLOCTEXT("MainAppMenu", "NwiroAssistantHeader", "Assistant"));
	}
	ContentSectionPtr->AddMenuEntry(
		"OpenNwiroTab",
		NwiroTabDisplay,
		NwiroTabDesc,
		FSlateIcon(FNwiroStyle::GetStyleSetName(), "Nwiro.Logo"),
		FUIAction(FExecuteAction::CreateStatic(&FNwiroPanel::OpenPanel), FCanExecuteAction())
	);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(NwiroTabName,
		FOnSpawnTab::CreateRaw(Instance.Get(), &FNwiroPanel::OnSpawnTab))
		.SetDisplayName(NwiroTabDisplay)
		.SetTooltipText(NwiroTabDesc)
		.SetAutoGenerateMenuEntry(false);

	// Level-editor toolbar button. UE5's main toolbar is UToolMenus-based;
	// legacy FExtender hooks no longer render there, so we extend the toolbar's
	// "User" section directly. A dynamic entry lets the show/hide toggle take
	// effect on RefreshAllWidgets() without an editor restart.
	if (UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User"))
	{
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("Nwiro");
		Section.AddDynamicEntry("NwiroToolbarButton", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			// User can hide the button from the plugin's settings (default ON).
			if (!IsToolbarButtonEnabled()) return;

			InSection.AddEntry(FToolMenuEntry::InitToolBarButton(
				"OpenNwiro",
				FUIAction(FExecuteAction::CreateStatic(&FNwiroPanel::OpenPanel)),
				NwiroTabDisplay,
				NwiroTabDesc,
				FSlateIcon(FNwiroStyle::GetStyleSetName(), "Nwiro.ToolbarIcon")));
		}));
	}
}

void FNwiroPanel::CreateBrowserWidget()
{
	WebBrowserWidget = SNew(SWebBrowser)
		.InitialURL(FrontendURL)
		.ShowControls(false)
		.ShowAddressBar(false)
		.ShowErrorMessage(true)
		.SupportsTransparency(false)
		.OnBeforePopup_Lambda([](FString URL, FString Frame)
		{
			FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
			return true;
		});
}

TSharedRef<SDockTab> FNwiroPanel::OnSpawnTab(const FSpawnTabArgs& SpawnTabArgs)
{
	CreateBrowserWidget();

	SAssignNew(PanelDock, SDockTab)
		.OnTabClosed_Lambda([WeakThis = AsWeak()](TSharedRef<SDockTab>)
		{
			TSharedPtr<FNwiroPanel> Pin = WeakThis.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			// Unbind UE's text input method system before resetting the browser.
			// Without this, ITextInputMethodSystem keeps a dangling context
			// pointer to the destroyed browser and crashes on the next async
			// IME focus event. Paired with BindInputMethodSystem in OnSpawnTab.
			//
			// Fix Point E (proactive, mirrors NwiroIKPanel): defensive Slate-
			// availability guard — see Shutdown() for full rationale.
			// Scope note: this guard catches the Slate-gone teardown class only;
			// it does NOT protect against widget-internal CEF IME state corruption.
			if (WebBrowserWidget.IsValid() && FSlateApplication::IsInitialized())
			{
				WebBrowserWidget->UnbindInputMethodSystem();
			}
			WebBrowserWidget.Reset();
			// The bridge stays alive (rooted in OnSpawnTab) so adapter sessions and
			// processes survive the tab close; it only needs to know no renderer is
			// attached. It is released in Shutdown, at engine pre-exit.
			if (UNwiroBridge::Instance) UNwiroBridge::Instance->OnPanelDetached();
			Pin->PanelDock.Reset();
		})
		.TabRole(NomadTab)
		[
			// Image files dropped from the operating system never reach the off-screen browser;
			// this wrapper hands them to the page as bridge events (NwiroImageDrop.h).
			SNew(SNwiroImageDropTarget)
			[
				WebBrowserWidget.ToSharedRef()
			]
		];

	// Bind the bridge
	if (!UNwiroBridge::Instance)
	{
		UNwiroBridge::Instance = NewObject<UNwiroBridge>();
		// Rooted for the editor's lifetime: the bridge owns the adapter sessions
		// and processes, which must survive a tab close (see OnPanelDetached).
		// Unrooted in Shutdown, so BeginDestroy still runs on the final purge.
		UNwiroBridge::Instance->AddToRoot();
	}
	WebBrowserWidget->BindUObject(TEXT("bridge"), UNwiroBridge::Instance);
	UNwiroBridge::Instance->OnPanelAttached();

	// Wire UE's text input method system into the CEF browser so CJK IMEs
	// (Bopomofo, Pinyin, Japanese MS-IME, Korean Hangul) work inside the
	// embedded chat input. UE 5.6+'s SWebBrowser uses CEF in off-screen
	// rendering (OSR) mode — there is no native child HWND on Windows or
	// NSView on macOS for IME messages to target. Instead, UE already
	// provides FCEFImeHandler + FCEFTextInputMethodContext which forward
	// to CefBrowserHost::ImeSetComposition / ImeCommitText. The bind call
	// below is the documented entry point that activates this whole chain
	// — without it, FCEFImeHandler::TextInputMethodSystem stays null and
	// every IME focus event is silently dropped on the InitContext check.
	// Mirrors the equivalent fix applied to NwiroIntegrationKit's panel.
	if (ITextInputMethodSystem* Ims = FSlateApplication::Get().GetTextInputMethodSystem())
	{
		WebBrowserWidget->BindInputMethodSystem(Ims);
	}

	return PanelDock.ToSharedRef();
}
