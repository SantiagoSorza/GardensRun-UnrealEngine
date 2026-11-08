#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "AutopilotSession.h"
#include "AutopilotStyle.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "PlaytestLevels.h"
#include "PlaytestSubsystem.h"
#include "SPlaytestPanel.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "Autopilot"

namespace
{
	const FName TabName(TEXT("AutopilotPlaytest"));
	constexpr double SessionTimeout = 30.0;

	bool PieRunning()
	{
		return GEditor && GEditor->PlayWorld != nullptr;
	}

	/** Play In Editor is up with a world to play in: the player's (a client of a dedicated server joins it after the
	 *  server's world has begun play). */
	bool PieReady()
	{
		const UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
		const UWorld* World = Bridge ? Bridge->GetGameWorld() : nullptr;
		return GEditor && GEditor->PlayWorld && World && World->HasBegunPlay();
	}

	/** Call Done once Condition holds (checked every tick), or with an error after the timeout. */
	void WaitFor(TFunction<bool()> Condition, TFunction<void(const FString&)> Done)
	{
		const double Start = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Condition, Done, Start](float)
		{
			if (Condition())
			{
				Done(FString());
				return false;
			}
			if (FPlatformTime::Seconds() - Start > SessionTimeout)
			{
				const UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
				const FString Problem = Bridge ? Bridge->GetPlayProblem() : FString();
				Done(Problem.IsEmpty() ? FString(TEXT("Timed out waiting for the play session"))
					: FString::Printf(TEXT("Timed out waiting for the play session: %s"), *Problem));
				return false;
			}
			return true;
		}));
	}

	/** Play In Editor, in the level Map when given (else the one open in the editor). Another level plays through
	 *  the play session's map override, as Movie Render Queue's PIE renders do: the editor keeps its own level open,
	 *  unsaved changes and all. */
	void StartPie(const FString& Map, TFunction<void(bool, const FString&)> Done)
	{
		FAutopilotSession::HoldExceptionBreaks();  // (an Accessed None mustn't stop the run's game: put back at its end)
		if (PieReady())
		{
			Done(true, FString());
			return;
		}
		FString Package, Error;
		if (!Map.IsEmpty() && !PlaytestLevels::Resolve(Map, Package, Error))
		{
			Done(PieRunning(), Error);
			return;
		}
		if (!PieRunning() && !GEditor->IsPlaySessionRequestQueued())
		{
			FRequestPlaySessionParams Params;
			if (!Package.IsEmpty() && Package != PlaytestLevels::EditorLevel())  // (the open one plays as it is, unsaved)
			{
				Params.GlobalMapOverride = PlaytestLevels::ObjectPath(Package);
			}
			GEditor->RequestPlaySession(Params);
		}
		WaitFor(&PieReady, [Done](const FString& Error) { Done(PieRunning(), Error); });
	}

	void StopPie(TFunction<void(bool, const FString&)> Done)
	{
		if (!PieRunning())
		{
			Done(false, FString());
			return;
		}
		GEditor->RequestEndPlayMap();
		WaitFor([]() { return !PieRunning(); }, [Done](const FString& Error) { Done(PieRunning(), Error); });
	}

	void HandleSession(const FString& Action, const FString& Map, TFunction<void(bool, const FString&)> Done)
	{
		if (!GEditor)
		{
			Done(false, TEXT("No editor"));
			return;
		}
		if (Action == TEXT("start"))
		{
			// Playing already, but another level asked for: that one, as a restart in it (as Unity's and Godot's)
			FString Package, Error;
			const UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
			if (!Map.IsEmpty() && PieReady() && Bridge && PlaytestLevels::Resolve(Map, Package, Error)
				&& Package != PlaytestLevels::PackageOf(Bridge->GetGameWorld()))
			{
				HandleSession(TEXT("restart"), Map, Done);
				return;
			}
			StartPie(Map, Done);
		}
		else if (Action == TEXT("stop"))
		{
			StopPie(Done);
		}
		else if (Action == TEXT("restart"))
		{
			FString Package, Error;
			if (!Map.IsEmpty() && !PlaytestLevels::Resolve(Map, Package, Error))
			{
				Done(PieRunning(), Error);  // (before stopping: a level that can't play leaves the game as it is)
				return;
			}
			StopPie([Map, Done](bool, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					Done(PieRunning(), Error);
					return;
				}
				StartPie(Map, Done);
			});
		}
		else
		{
			Done(PieRunning(), FString());
		}
	}
}

class FAutopilotPlaytestEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get())
		{
			Bridge->SessionHandler = &HandleSession;
			Bridge->SessionRequested = []() { return GEditor && GEditor->IsPlaySessionRequestQueued(); };
		}
		FAutopilotStyle::Initialize();
		FAutopilotSession::Get().Startup();  // (the runs are kept whether the tab is open or not)
		EndPieHandle = FEditorDelegates::EndPIE.AddLambda([](bool) { FAutopilotSession::ReleaseExceptionBreaks(); });
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel);
			TWeakPtr<SPlaytestPanel> Weak = Panel;
			return SNew(SDockTab).TabRole(ETabRole::NomadTab)
				.OnExtendContextMenu_Lambda([Weak](FMenuBuilder& Menu)  // (the tab's right-click menu: Connect, Settings…)
				{
					if (TSharedPtr<SPlaytestPanel> P = Weak.Pin())
					{
						P->ExtendTabMenu(Menu);
					}
				})
				[Panel];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "Autopilot"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Autopilot: an AI agent that tests your game. Verify claims, play goals and explore the game for bugs"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAutopilotStyle::GetStyleSetName(), "Autopilot.Icon"));

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAutopilotPlaytestEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			if (UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get())
			{
				Bridge->SessionHandler = nullptr;
				Bridge->SessionRequested = nullptr;
			}
		}
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
		FAutopilotSession::ReleaseExceptionBreaks();
		FAutopilotSession::Get().Shutdown();
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		}
		FAutopilotStyle::Shutdown();
	}

private:
	FDelegateHandle EndPieHandle;

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
		FToolMenuSection& Section = Toolbar->FindOrAddSection("AutopilotPlaytest");
		// The Autopilot logo (the label shows only where the toolbar overflows into a menu)
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"OpenAutopilotPlaytest",
			FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(TabName); })),
			LOCTEXT("ToolbarLabel", "Autopilot"),
			LOCTEXT("ToolbarTooltip", "Open Autopilot"),
			FSlateIcon(FAutopilotStyle::GetStyleSetName(), "Autopilot.Icon.Toolbar")));
	}
};

IMPLEMENT_MODULE(FAutopilotPlaytestEditorModule, AutopilotPlaytestEditor)

#undef LOCTEXT_NAMESPACE
