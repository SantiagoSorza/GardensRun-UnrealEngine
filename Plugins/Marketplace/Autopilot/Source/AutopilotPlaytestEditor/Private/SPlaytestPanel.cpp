#include "SPlaytestPanel.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"

#include "Editor.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "AutopilotStyle.h"
#include "AutopilotWidgets.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "PlaytestAccount.h"
#include "PlaytestAgentService.h"
#include "PlaytestAura.h"
#include "PlaytestPreferences.h"
#include "PlaytestSettings.h"
#include "PlaytestSubsystem.h"
#include "SPlaytestPolicyLog.h"
#include "SPlaytestVideo.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Autopilot"

namespace
{
	constexpr float RailWidth = 280.f;  // (with its 1px rule at the right)
	constexpr float NarrowBelow = 700.f;
	constexpr float FoldHeight = 320.f;  // (a fold's detail block: at most this tall, then it scrolls)

	using AutopilotUI::Str;

	/** Text for SRichTextBlock's markup: its <, >, & and " as escapes, so code shows as code. */
	FString Markup(const FString& Text)
	{
		return Text.Replace(TEXT("&"), TEXT("&amp;")).Replace(TEXT("<"), TEXT("&lt;")).Replace(TEXT(">"), TEXT("&gt;")).Replace(TEXT("\""), TEXT("&quot;"));
	}

	FString PromptOf(const FAutopilotRun& Run)
	{
		return Run.ShownPrompt();  // (its words as typed; none: Explore, Play a goal, Verify, Run)
	}

	/** The history's prompt: wrapped, at most 140 characters (then "…"). */
	FString HistoryPrompt(const FAutopilotRun& Run)
	{
		const FString Words = Run.ShownPrompt();
		return Words.Len() > 140 ? Words.Left(139).TrimEnd() + TEXT("\u2026") : Words;
	}

	/** A finding's severity as its tag says it: ERROR, WARNING, else INFO. */
	FString SeverityWord(const FString& Severity)
	{
		const EAutopilotTone Tone = AutopilotUI::SeverityTone(Severity);
		return Tone == EAutopilotTone::Red ? TEXT("error") : Tone == EAutopilotTone::Amber ? TEXT("warning") : TEXT("info");
	}

	FString TagWord(const FAutopilotRun& Run)
	{
		return Run.bDone ? Run.Verdict : FString(TEXT("LIVE"));
	}

	const TCHAR* StageNames[] = { TEXT("Launch"), TEXT("Analyze"), TEXT("Probe env"), TEXT("Build harness"), TEXT("Rollout"), TEXT("Process result"), TEXT("Report") };

	/** Use with Aura changed: saved for this project, and a connection going now goes to the new place. */
	void SetUseWithAura(bool bUse)
	{
		UPlaytestSettings* Settings = GetMutableDefault<UPlaytestSettings>();
		if (Settings->bUseWithAura == bUse)
		{
			return;
		}
		Settings->bUseWithAura = bUse;
		Settings->SaveConfig();
		if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
		{
			B->RequestAccount();  // (and again on the connection that follows: every welcome asks)
			if (B->WantsConnection() || B->IsConnected())
			{
				B->Disconnect();
				B->Connect();
			}
		}
	}

	/** Disconnect (or Stop on the connect card) pressed: the tab doesn't connect by itself again this editor session, until Reconnect. */
	bool GUserDisconnected = false;

	void UserDisconnect()
	{
		GUserDisconnected = true;
		if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
		{
			B->Disconnect();
		}
	}

	/** Reconnect, Try again, Try now: connect again (an install that failed runs again first, then the agent service starts). */
	void TryConnectAgain()
	{
		GUserDisconnected = false;
		UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
		if (!B)
		{
			return;
		}
		FPlaytestAgentService* S = B->GetAgentService();
		if (S && S->GetState() == EPlaytestServiceState::InstallFailed && S->HasInstaller())
		{
			S->Install();
		}
		B->Connect();  // (clears the problem and starts it again)
	}

	/** Connect again now, when connected and no run is going: the next hello says what changed (e.g. sharing). */
	void ReconnectIfIdle()
	{
		UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
		if (B && B->IsConnected() && !B->IsRunActive() && !FAutopilotSession::Get().GetActiveRun().IsValid())
		{
			B->Disconnect();
			B->Connect();
		}
	}

}

namespace
{
	FPlaytestAgentService* ServiceOf(const UPlaytestSubsystem* B)
	{
		return B ? B->GetAgentService() : nullptr;
	}

	/** Editor Preferences > Plugins > Autopilot. */
	void ShowSettings()
	{
		if (ISettingsModule* Module = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
		{
			const UPlaytestSettings* Settings = GetDefault<UPlaytestSettings>();
			Module->ShowViewer(Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
		}
	}

	void OpenLog(const FString& Path)
	{
		if (!Path.IsEmpty() && FPaths::FileExists(Path))
		{
			FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
		}
	}

	/** The widest each page's column gets (its padding inside it): wider pages center it. */
	constexpr float ComposerMax = 620.f;
	constexpr float SettingsMax = 640.f;
	constexpr float SetupMax = 600.f;
	constexpr float ConnectMax = 540.f;  // (the connect card, the removal's)
	/** Narrower than this, rows stack (the run area's width). */
	constexpr float CompactBelow = 520.f;

	/** Content in a column Max wide at most (Padding inside it), centered beyond it. */
	TSharedRef<SWidget> Capped(const TSharedRef<SWidget>& Content, float Max, const TAttribute<FMargin>& Padding = FMargin(0.f))
	{
		return SNew(SBox)
			.HAlign(HAlign_Center)
			[
				SNew(SBox)
				.MinDesiredWidth(Max)
				.MaxDesiredWidth(Max)
				.Padding(Padding)
				[
					Content
				]
			];
	}

	/** A row of buttons, 6 apart, that wraps when the tab is narrow. */
	TSharedRef<SWrapBox> ButtonRow(float Gap = 6.f)
	{
		return SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(Gap, 6.f));
	}

	void AddTo(const TSharedRef<SWrapBox>& Row, const TSharedRef<SWidget>& Widget)
	{
		Row->AddSlot().VAlign(VAlign_Center)[Widget];
	}

	/** A progress line: the spinner and its words (13px, 500). */
	TSharedRef<SWidget> Progress(const FString& Line, float Px = 13.f, const FLinearColor& Color = AutopilotColor::Text())
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(AutopilotUI::SSpinner)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				AutopilotUI::Body(Str(Line), Px, Color, true, EAutopilotFont::BodyMedium)
			];
	}

	/** A plate of the setup and the Settings page: 12 x 14 inside, Outline its edge (a state's, else the rule). */
	TSharedRef<SWidget> Card(const TSharedRef<SWidget>& Content, const FLinearColor& Outline = AutopilotColor::Rule(), const FLinearColor& FillColor = AutopilotColor::Plate())
	{
		return AutopilotUI::Plate(FMargin(14.f, 12.f), Content, Outline, FillColor);
	}

	/** A line of mono text in a square well (a link), with a button after it. */
	TSharedRef<SWidget> Well(const FString& Text, const TSharedPtr<SWidget>& After = nullptr)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Str(Text))
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Text3()))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.ToolTipText(Str(Text))
			];
		if (After.IsValid())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)[After.ToSharedRef()];
		}
		return AutopilotUI::Box(AutopilotColor::Well(), AutopilotColor::Rule(), 0.f, FMargin(10.f, 4.f, 4.f, 4.f), Row);
	}

	/** A plate's head: its micro-label, and a tag after it. */
	TSharedRef<SWidget> CardHead(const FText& Name, const FText& TagText = FText::GetEmpty(), EAutopilotTone Tone = EAutopilotTone::Success)
	{
		TSharedRef<SWrapBox> Head = ButtonRow(8.f);
		AddTo(Head, AutopilotUI::Label(Name));
		if (!TagText.IsEmpty())
		{
			AddTo(Head, AutopilotUI::Tag(TagText, Tone));
		}
		return Head;
	}

	/** A small button (24 tall, 11.5px). */
	TSharedRef<SWidget> SmallButton(const FText& Text, const FName& Style, FOnClicked OnClicked)
	{
		return AutopilotUI::Button(Text, Style, OnClicked, true, nullptr, true);
	}

	/** A square dot (the agent service's state). */
	TSharedRef<SWidget> Dot(const FLinearColor& Color, float Side = 6.f)
	{
		return SNew(SBox).WidthOverride(Side).HeightOverride(Side)[AutopilotUI::Fill(FSlateColor(Color))];
	}

	/** A page's title (the setup's, the removal's): the logo (20px) and the words, 18px 600. */
	TSharedRef<SWidget> PageTitle(const FText& Text)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(SImage).Image(FAutopilotStyle::Brush("Autopilot.Icon.Large"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(FAutopilotStyle::Font(EAutopilotFont::BodySemi, 18.f, -10))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
				.AutoWrapText(true)
			];
	}

	/** A page's header (New run, Settings): its title (15px, 500), 12 x 16 inside, a rule under it. */
	TSharedRef<SWidget> PageHeader(const TSharedRef<SWidget>& Content, const TAttribute<FMargin>& Padding = FMargin(16.f, 12.f))
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Padding(Padding).MinDesiredHeight(44.f).VAlign(VAlign_Center)[Content]
			]
			+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()];
	}

	TSharedRef<STextBlock> HeaderTitle(const TAttribute<FText>& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 15.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Text()));
	}
}

void SPlaytestPanel::Construct(const FArguments& InArgs)
{
	const UPlaytestSettings* Settings = GetDefault<UPlaytestSettings>();
	Minutes = Settings->DefaultExploreMinutes;
	TimeLimit = Settings->RunTimeLimitMinutes;
	Prefs =PlaytestPreferences::Load();
	bSetupShare = Prefs.bShareData;
	KeysDir = InArgs._KeysDir;
	SeenAgentRunId = FAutopilotSession::Get().GetAgentStartedRunId();  // (one started before the tab opened isn't shown again)
	SavedCodingAgent = PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_AGENT"), KeysDir);
	SavedLlmProvider = PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_OPENCODE_PROVIDER"), KeysDir);
	for (const FString& Agent : PlaytestAccount::CodingAgents())
	{
		CodingAgentItems.Add(MakeShared<FString>(Agent));
	}
	for (const FString& Provider : PlaytestAccount::LlmProviders())
	{
		LlmProviderItems.Add(MakeShared<FString>(Provider));
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAutopilotStyle::Brush("Autopilot.White"))
		.BorderBackgroundColor(FSlateColor(AutopilotColor::Ground()))
		.ForegroundColor(FSlateColor(AutopilotColor::Text()))
		.Padding(0)
		[
			// (pages and views switch once the one coming is laid out: no flash)
			SNew(AutopilotUI::SSettle)
			.WidgetIndex_Lambda([this]() { return IsRemovedShown() ? 2 : IsSetupShown() ? 1 : 0; })
			.Pages({
			SNew(SHorizontalBox)
			// The history (wide): 280 with its rule
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(RailWidth - 1.f)
				.Visibility_Lambda([this]() { return bNarrow ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					MakeRail()
				]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return bNarrow ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					AutopilotUI::VRule()
				]
			]
			// The run picked
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return bNarrow ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						MakeNarrowBar()
					]
				]
				+ SVerticalBox::Slot().AutoHeight()[MakeNotice()]
				+ SVerticalBox::Slot().AutoHeight()[MakeTopBar()]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SNew(AutopilotUI::SSettle)
					.WidgetIndex_Lambda([this]() { return ViewIndex(); })
					.Pages({
						MakeConnectCard(),
						MakeComposer(),
						MakeLive(),
						// (the padding on a box in the slot: a scroll box's slot takes no padding that changes, Slate drops it)
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(SBox).Padding(TAttribute<FMargin>::CreateLambda([this]() { return FMargin(bNarrow ? 12.f : 16.f); }))
							[
								SAssignNew(ResultSettle, AutopilotUI::SSettle)
							]
						],
						// The feed (the tab): the Live page's plate, the page's height; the steer box under it while the run goes
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().FillHeight(1.f)
						[
							SNew(SBox).Padding(TAttribute<FMargin>::CreateLambda([this]() { return FMargin(bNarrow ? 12.f : 16.f); }))
							[
								SNew(SWidgetSwitcher)
								.WidgetIndex_Lambda([this]() { return FeedItems.Num() ? 1 : 0; })
								+ SWidgetSwitcher::Slot()[AutopilotUI::Body(LOCTEXT("FeedEmpty", "Nothing yet."), 12.f, AutopilotColor::Muted())]
								+ SWidgetSwitcher::Slot()[AutopilotUI::Plate(FMargin(11.f, 6.f, 11.f, 8.f), MakeFeedList(FeedList))]
							]
						]
						+ SVerticalBox::Slot().AutoHeight()[MakeSteerBar(true)],
						SAssignNew(PolicyLog, SPlaytestPolicyLog),
						MakeSettings() })
				]
				+ SVerticalBox::Slot().AutoHeight()[MakeStatusLine()]
			],
			// The first-run setup, in place of the rest until it is done
			MakeSetup(),
			// Autopilot removed from this computer (or being removed): in place of the rest, the setup's too
			MakeRemoved() })
		]
	];
	SyncRuns();
	if (!FAutopilotStyle::FontsFound())
	{
		FAutopilotSession::Get().Say(TEXT("A bundled font couldn\u2019t be read: using the editor\u2019s font"));
	}
	CheckServiceMissing();
	AutoConnect();  // (the tab opened: connect now, which starts the agent service when none is running)
}

void SPlaytestPanel::AutoConnect()
{
	// Always by itself once set up: never while the setup or the removal shows, after the user's Disconnect (until
	// Reconnect), while a refusal stands (Try again), or in the tests
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B || !Prefs.bOnboarded || IsSetupShown() || IsRemovedShown() || GUserDisconnected || GIsAutomationTesting || TestConnected.IsSet()
		|| B->WantsConnection() || B->IsConnected() || !B->GetProblem().IsEmpty())
	{
		return;
	}
	B->Connect();
}

void SPlaytestPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	PanelWidth = AllottedGeometry.GetLocalSize().X;
	bNarrow = PanelWidth < NarrowBelow;
	if (IsCompact() != bCompactShown)
	{
		// What is built once per change (the result, the Settings page) is built again for the other layout
		bCompactShown = IsCompact();
		ResultRevision = -1;
		Video.Reset();  // (its controls stack, or not)
		UsageSeen = -1;
		AccountShownKey.Empty();
		ConnectShownKey.Empty();
		SetupShownKey.Empty();
		RemovedShown = -1;
	}
	TickSignIn();
	AutoConnect();
	if (IsRemovedShown() && (int32)UninstallShown() != RemovedShown)
	{
		RebuildRemoved();
	}
	if (IsSetupShown() && SetupStep == StepFirstRun && MemoryView() == EMemoryView::Something)
	{
		CloseSetup();  // (step 4 is skipped when the game's memory holds something)
	}
	if (IsSetupShown())
	{
		FollowLlmProvider();  // (an account frame can change it too)
		const FString Key = SetupKey();
		if (Key != SetupShownKey)
		{
			SetupShownKey = Key;
			RebuildSetup();
		}
	}
	const FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetRevision() != SeenRevision)
	{
		SeenRevision = Session.GetRevision();
		SyncRuns();
	}
	SyncFeed();
	const TSharedPtr<FAutopilotRun> Run = Selected();
	if (Run.IsValid() && Run->bDone && (Run->Id != ResultRunId || Run->Revision != ResultRevision))
	{
		RebuildResult();
	}
	if (Video.IsValid() && ViewIndex() != ViewResult)
	{
		Video->Pause();  // (its result isn't shown)
	}
	if (PolicyLog.IsValid())
	{
		PolicyLog->SetRun(Run);
	}
	if (Tab == TabPolicy && (!Run.IsValid() || Run->Steps.Num() == 0))
	{
		Tab = TabMain;
	}
	if (ViewIndex() == ViewConnect)
	{
		const FString Key = ConnectKey();
		if (Key != ConnectShownKey)
		{
			ConnectShownKey = Key;
			RebuildConnect();
		}
	}
	if (ViewIndex() == ViewSettings)
	{
		const FString Key = AccountKey();
		if (Key != AccountShownKey)
		{
			AccountShownKey = Key;
			RebuildAccount();
		}
		if (Session.GetUsageRevision() != UsageSeen)
		{
			RebuildSettings();
		}
	}
}

// -- State -------------------------------------------------------------------------------------------------------

float SPlaytestPanel::MainWidth() const
{
	return bNarrow ? PanelWidth : PanelWidth - RailWidth;
}

bool SPlaytestPanel::IsCompact() const
{
	return MainWidth() < CompactBelow;
}

TSharedPtr<FAutopilotRun> SPlaytestPanel::Selected() const
{
	return SelectedId == INDEX_NONE ? nullptr : FAutopilotSession::Get().FindRun(SelectedId);
}

void SPlaytestPanel::Select(const TSharedPtr<FAutopilotRun>& Run)
{
	const int32 Id = Run.IsValid() ? Run->Id : INDEX_NONE;
	bSettingsPage = false;
	if (Id == INDEX_NONE && SelectedId != INDEX_NONE)
	{
		PreviousRunId = SelectedId;  // (New run, or the connect card: Back to the last run goes there)
	}
	if (Id != SelectedId)
	{
		SelectedId = Id;
		Tab = TabMain;
	}
	if (RunList.IsValid())
	{
		if (Run.IsValid())
		{
			RunList->SetSelection(Run, ESelectInfo::Direct);
			RunList->RequestScrollIntoView(Run);
		}
		else
		{
			RunList->ClearSelection();
		}
	}
	if (NarrowMenu.IsValid())
	{
		NarrowMenu->SetIsOpen(false);
	}
	SyncFeed();
}

int32 SPlaytestPanel::ViewIndex() const
{
	if (bSettingsPage)
	{
		return ViewSettings;
	}
	const TSharedPtr<FAutopilotRun> Run = Selected();
	if (!Run.IsValid())
	{
		return IsConnected() ? ViewComposer : ViewConnect;
	}
	if (Tab == TabFeed)
	{
		return ViewFeed;
	}
	if (Tab == TabPolicy && Run->Steps.Num() > 0)
	{
		return ViewPolicy;
	}
	return Run->bDone ? ViewResult : ViewLive;
}

bool SPlaytestPanel::IsConnected() const
{
	if (TestConnected.IsSet())
	{
		return TestConnected.GetValue();
	}
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return B && B->IsConnected();
}

bool SPlaytestPanel::IsConnecting() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return B && B->WantsConnection() && !B->IsConnected() && B->GetProblem().IsEmpty();
}

bool SPlaytestPanel::IsUnreachable() const
{
	// Trying, and the agent service can't be reached (a refusal stops the trying)
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return B && B->WantsConnection() && !B->IsConnected() && !B->GetProblem().IsEmpty();
}

bool SPlaytestPanel::IsRefused() const
{
	// The agent service answered with an `error` frame: the plugin stopped trying, and says why
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return B && !B->WantsConnection() && !B->IsConnected() && !B->GetProblem().IsEmpty();
}

bool SPlaytestPanel::IsRunning() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return (B && B->IsConnected() && B->IsRunActive()) || FAutopilotSession::Get().GetActiveRun().IsValid();
}

FString SPlaytestPanel::PromptText() const
{
	return PromptBox.IsValid() ? PromptBox->GetText().ToString().TrimStartAndEnd() : FString();
}

bool SPlaytestPanel::CanRun() const
{
	return IsConnected() && !IsRunning() && !PromptText().IsEmpty();
}

void SPlaytestPanel::SyncRuns()
{
	const TArray<TSharedPtr<FAutopilotRun>>& Runs = FAutopilotSession::Get().GetRuns();
	if (SelectedId != INDEX_NONE && !Selected().IsValid())
	{
		SelectedId = INDEX_NONE;  // (a memory reset took it off the tab: New run)
	}
	// A run the agent service started (or one that began while New run was shown) is shown as it comes
	if (Runs.Num() > SeenRunCount && SelectedId == INDEX_NONE && SeenRevision > 0 && !Runs.Last()->bDone)
	{
		SelectedId = Runs.Last()->Id;
		Tab = TabMain;
	}
	// A run the user's own coding agent started is shown as one started from the prompt box is, wherever the tab was (the
	// setup, when open, stays over it until it closes)
	const int32 AgentRunId = FAutopilotSession::Get().GetAgentStartedRunId();
	if (AgentRunId != SeenAgentRunId)
	{
		SeenAgentRunId = AgentRunId;
		const TSharedPtr<FAutopilotRun> AgentRun = FAutopilotSession::Get().FindRun(AgentRunId);
		if (AgentRun.IsValid() && !AgentRun->bDone)
		{
			SelectedId = AgentRun->Id;
			Tab = TabMain;
			bSettingsPage = false;
		}
	}
	SeenRunCount = Runs.Num();
	RunItems.Reset(Runs.Num());
	for (int32 i = Runs.Num() - 1; i >= 0; --i)
	{
		RunItems.Add(Runs[i]);
	}
	if (RunList.IsValid())
	{
		RunList->RequestListRefresh();
	}
	if (NarrowList.IsValid())
	{
		NarrowList->RequestListRefresh();
	}
	const TSharedPtr<FAutopilotRun> Run = Selected();
	if (RunList.IsValid())
	{
		if (Run.IsValid())
		{
			if (!RunList->IsItemSelected(Run))
			{
				RunList->SetSelection(Run, ESelectInfo::Direct);
			}
		}
		else
		{
			RunList->ClearSelection();
		}
	}
}

void SPlaytestPanel::SyncFeed()
{
	const TSharedPtr<FAutopilotRun> Run = Selected();
	const int32 Id = Run.IsValid() ? Run->Id : INDEX_NONE;
	const int32 Revision = Run.IsValid() ? Run->Revision : -1;
	if (Id == FeedRunId && Revision == FeedCount)
	{
		return;
	}
	if (Id != FeedRunId)
	{
		OpenFolds.Reset();  // (another run: its folds start closed)
	}
	FeedRunId = Id;
	FeedCount = Revision;
	// The Feed tab: every line under its stage's heading; the Live page: the last 10 lines, as they are
	FeedItems.Reset();
	LiveFeedItems.Reset();
	if (Run.IsValid())
	{
		GroupFeed(Run->Feed, FeedItems);
		for (int32 i = FMath::Max(0, Run->Feed.Num() - 10); i < Run->Feed.Num(); ++i)
		{
			LiveFeedItems.Add(Run->Feed[i]);
		}
	}
	for (const TSharedPtr<SListView<TSharedPtr<FAutopilotFeedLine>>>& List : { LiveFeedList, FeedList })
	{
		if (List.IsValid())
		{
			List->RequestListRefresh();
			if (FeedItems.Num())
			{
				List->RequestScrollIntoView(FeedItems.Last());
			}
		}
	}
}

// -- The run history -------------------------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeRunTag(const TSharedPtr<FAutopilotRun>& Run)
{
	// The verdict's tone (LIVE: the accent), the same whether the run is the one picked or not (the row shows that)
	TWeakPtr<FAutopilotRun> Weak = Run;
	return AutopilotUI::Tag(
		TAttribute<FText>::CreateLambda([Weak]() { const TSharedPtr<FAutopilotRun> R = Weak.Pin(); return R.IsValid() ? Str(TagWord(*R)) : FText::GetEmpty(); }),
		TAttribute<EAutopilotTone>::CreateLambda([Weak]()
		{
			const TSharedPtr<FAutopilotRun> R = Weak.Pin();
			return R.IsValid() ? R->Tone() : EAutopilotTone::Neutral;
		}));
}

TSharedRef<SWidget> SPlaytestPanel::MakeRail()
{
	return AutopilotUI::Fill(FSlateColor(AutopilotColor::Rail()), FMargin(0),
		SNew(SVerticalBox)
		// The header: the logo, History, the gear and New run (12 x 14, a rule under it)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).Padding(FMargin(14.f, 12.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SImage).Image(FAutopilotStyle::Brush("Autopilot.Icon.Small"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("RunHistory", "History"))
					.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 13.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					MakeSettingsButton(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					AutopilotUI::Button(LOCTEXT("NewRun", "New run"), "Autopilot.Button.Primary",
						FOnClicked::CreateLambda([this]() { Select(nullptr); return FReply::Handled(); }), true, nullptr, true)
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.Padding(FMargin(14.f, 10.f))
			.Visibility_Lambda([]() { return FAutopilotSession::Get().GetRuns().Num() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				AutopilotUI::Body(LOCTEXT("NoRuns", "No runs yet in this session."), 12.f, AutopilotColor::Muted())
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(RunList, SListView<TSharedPtr<FAutopilotRun>>)
			.ListItemsSource(&RunItems)
			.OnGenerateRow_Lambda([this](TSharedPtr<FAutopilotRun> Run, const TSharedRef<STableViewBase>& Owner) { return MakeRunRow(Run, Owner, false); })
			.SelectionMode(ESelectionMode::Single)
			.OnSelectionChanged_Lambda([this](TSharedPtr<FAutopilotRun> Run, ESelectInfo::Type How)
			{
				if (How != ESelectInfo::Direct && Run.IsValid())
				{
					Select(Run);
				}
			})
		]
		+ SVerticalBox::Slot().AutoHeight()[MakeSaveData()]);
}

TSharedRef<ITableRow> SPlaytestPanel::MakeRunRow(TSharedPtr<FAutopilotRun> Run, const TSharedRef<STableViewBase>& Owner, bool bPopup)
{
	TWeakPtr<FAutopilotRun> Weak = Run;
	auto IsPicked = [this, Weak]() { const TSharedPtr<FAutopilotRun> R = Weak.Pin(); return R.IsValid() && R->Id == SelectedId; };
	// (rows alternate #0f1013 / #121316 in the rail, none in the narrow list; hovered: #1d2026)
	TSharedRef<TWeakPtr<SWidget>> RowOf = MakeShared<TWeakPtr<SWidget>>();
	TAttribute<FLinearColor> Stripe = TAttribute<FLinearColor>::CreateLambda([this, Weak, RowOf, bPopup]()
	{
		const TSharedPtr<SWidget> Row = RowOf->Pin();
		if (Row.IsValid() && Row->IsHovered())
		{
			return AutopilotColor::Hover();
		}
		if (bPopup)
		{
			return FLinearColor::Transparent;
		}
		const int32 At = RunItems.IndexOfByKey(Weak.Pin());
		return At % 2 == 1 ? AutopilotColor::Ground() : AutopilotColor::Rail();
	});
	TSharedRef<STableRow<TSharedPtr<FAutopilotRun>>> Row = SNew(STableRow<TSharedPtr<FAutopilotRun>>, Owner)
	.Style(&FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row"))
	.Padding(FMargin(0.f))
	[
		AutopilotUI::Picked(TAttribute<bool>::CreateLambda(IsPicked),
		SNew(SBox).Padding(bPopup ? FMargin(12.f, 9.f) : FMargin(14.f, 9.f))
		[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[MakeRunTag(Run)]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Text_Lambda([Weak]()
				{
					const TSharedPtr<FAutopilotRun> R = Weak.Pin();
					return R.IsValid() ? Str(R->HistoryLine()) : FText::GetEmpty();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text_Lambda([Weak]() { const TSharedPtr<FAutopilotRun> R = Weak.Pin(); return R.IsValid() ? Str(HistoryPrompt(*R)) : FText::GetEmpty(); })
			.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.5f))
			.LineHeightPercentage(AutopilotUI::BodyLineHeight)
			.ColorAndOpacity_Lambda([IsPicked]() { return FSlateColor(IsPicked() ? AutopilotColor::Text() : AutopilotColor::Text2()); })
			.AutoWrapText(true)
		]
		], Stripe)
	];
	*RowOf = Row;
	return Row;
}

TSharedRef<SWidget> SPlaytestPanel::MakeNarrowBar()
{
	// The history as a button (the run picked: its tag and line; a list under the bar when pressed, the memory row last),
	// the gear and New run; a rule under it
	auto Shown = [](bool bShow) { return bShow ? EVisibility::Visible : EVisibility::Collapsed; };
	TSharedRef<SButton> History = SNew(SButton)
		.ButtonStyle(&FAutopilotStyle::Get().GetWidgetStyle<FButtonStyle>("Autopilot.Button.History"))
		.ContentPadding(FMargin(8.f, 0.f))
		.AccessibleText(LOCTEXT("HistoryShort", "History"))
		.IsEnabled_Lambda([]() { return FAutopilotSession::Get().GetRuns().Num() > 0; })
		.ToolTip(AutopilotUI::Tip(LOCTEXT("HistoryShort", "History")))
		.OnClicked_Lambda([this]()
		{
			if (NarrowMenu.IsValid())
			{
				NarrowMenu->SetIsOpen(!NarrowMenu->IsOpen());
			}
			return FReply::Handled();
		})
		[
			SNew(SBox)
			.HeightOverride(AutopilotUI::ButtonHeight - 2.f)
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				// (the run picked: its tag and line; New run: New, and how many runs this session)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					AutopilotUI::Tag(TAttribute<FText>::CreateLambda([this]() { const TSharedPtr<FAutopilotRun> R = Selected(); return R.IsValid() ? Str(TagWord(*R)) : LOCTEXT("NewTag", "New"); }),
						TAttribute<EAutopilotTone>::CreateLambda([this]() { const TSharedPtr<FAutopilotRun> R = Selected(); return R.IsValid() ? R->Tone() : EAutopilotTone::Neutral; }))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Text3()))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([this]()
					{
						if (const TSharedPtr<FAutopilotRun> R = Selected())
						{
							return Str(R->HistoryLine());
						}
						const int32 N = FAutopilotSession::Get().GetRuns().Num();
						return N ? FText::Format(LOCTEXT("RunsThisSession", "{0} {0}|plural(one=run,other=runs) this session"), N) : LOCTEXT("NoRunsYet", "No runs yet");
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SImage).Image(FAutopilotStyle::Brush("Autopilot.Chevron"))
				]
			]
		];
	// The list: a menu (the popover, its outline, 8px corners, the editor's menu shadow), the bar's width less 10 each side
	TSharedRef<SWidget> Menu = SNew(SBox)
		.Padding(FMargin(10.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride_Lambda([this]() { return FOptionalSize(FMath::Max(120.f, PanelWidth - 20.f)); })
			.MaxDesiredHeight(560.f)
			[
				AutopilotUI::Box(AutopilotColor::Popover(), AutopilotColor::Line(), AutopilotRadius::Plate, FMargin(1.f),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox)
						.Padding(FMargin(12.f, 10.f))
						.Visibility_Lambda([]() { return FAutopilotSession::Get().GetRuns().Num() ? EVisibility::Collapsed : EVisibility::Visible; })
						[
							AutopilotUI::Body(LOCTEXT("NoRuns", "No runs yet in this session."), 12.f, AutopilotColor::Muted())
						]
					]
					+ SVerticalBox::Slot().FillHeight(1.f)
					[
						SAssignNew(NarrowList, SListView<TSharedPtr<FAutopilotRun>>)
						.ListItemsSource(&RunItems)
						.OnGenerateRow_Lambda([this](TSharedPtr<FAutopilotRun> Run, const TSharedRef<STableViewBase>& Owner) { return MakeRunRow(Run, Owner, true); })
						.SelectionMode(ESelectionMode::Single)
						.OnSelectionChanged_Lambda([this](TSharedPtr<FAutopilotRun> Run, ESelectInfo::Type How)
						{
							if (How != ESelectInfo::Direct && Run.IsValid())
							{
								Select(Run);
								NarrowList->ClearSelection();
							}
						})
					]
					+ SVerticalBox::Slot().AutoHeight()[MakeSaveData(true)])
			]
		];
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(NarrowMenu, SMenuAnchor)
			.Placement(MenuPlacement_BelowAnchor)
			.Method(EPopupMethod::UseCurrentWindow)
			.MenuContent(Menu)
			[
				SNew(SBox).Padding(FMargin(10.f, 8.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[History]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
					[
						MakeSettingsButton(false)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
					[
						AutopilotUI::Button(LOCTEXT("NewRun", "New run"), "Autopilot.Button.Primary",
							FOnClicked::CreateLambda([this]() { Select(nullptr); return FReply::Handled(); }))
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()];
}

TSharedRef<SWidget> SPlaytestPanel::MakeSettingsButton(bool bSmall)
{
	// The gear: quiet, or "on" (#1d2026, its outline, the accent) while the Settings page is open
	auto Gear = [this, bSmall](const FName& Style)
	{
		return AutopilotUI::IconButton(FAutopilotStyle::Brush("Autopilot.Gear"), LOCTEXT("SettingsTip", "Settings"), Style,
			FOnClicked::CreateLambda([this]() { ToggleSettings(); return FReply::Handled(); }), true, bSmall);
	};
	TSharedRef<SButton> Off = Gear("Autopilot.Button.Quiet");
	TSharedRef<SButton> On = Gear("Autopilot.Button.On");
	SettingsButtons.Add(Off);
	SettingsButtons.Add(On);
	return SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([this]() { return bSettingsPage ? 1 : 0; })
		+ SWidgetSwitcher::Slot()[Off]
		+ SWidgetSwitcher::Slot()[On];
}

void SPlaytestPanel::ExtendTabMenu(FMenuBuilder& Menu)
{
	Menu.BeginSection(TEXT("Autopilot"), LOCTEXT("TabMenuSection", "Autopilot"));
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (B && (B->WantsConnection() || B->IsConnected()))
	{
		Menu.AddMenuEntry(LOCTEXT("TabMenuDisconnect", "Disconnect"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { UserDisconnect(); })));
	}
	else
	{
		Menu.AddMenuEntry(LOCTEXT("TabMenuConnect", "Connect"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { TryConnectAgain(); })));
	}
	TWeakPtr<SPlaytestPanel> Weak = SharedThis(this);
	Menu.AddMenuEntry(LOCTEXT("TabMenuSettings", "Settings"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Weak]() { if (TSharedPtr<SPlaytestPanel> P = Weak.Pin()) { P->ToggleSettings(); } }),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([Weak]() { TSharedPtr<SPlaytestPanel> P = Weak.Pin(); return P.IsValid() && P->bSettingsPage; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.AddMenuEntry(LOCTEXT("RerunSetup", "Rerun setup"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Weak]()
		{
			TSharedPtr<SPlaytestPanel> P = Weak.Pin();
			if (P.IsValid() && P->CanChangeAccount())
			{
				P->OpenSetup();
			}
		}), FCanExecuteAction::CreateLambda([Weak]() { TSharedPtr<SPlaytestPanel> P = Weak.Pin(); return P.IsValid() && P->CanChangeAccount(); })));
	Menu.AddMenuEntry(LOCTEXT("ResetMemory", "Reset memory…"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Weak]()
		{
			if (TSharedPtr<SPlaytestPanel> P = Weak.Pin())
			{
				if (P->bSettingsPage)
				{
					P->ToggleSettings();  // (asked in place on the runs' rail)
				}
				P->OnDeleteSave();
			}
		}), FCanExecuteAction::CreateLambda([Weak]() { TSharedPtr<SPlaytestPanel> P = Weak.Pin(); return P.IsValid() && P->IsConnected() && !P->IsRunning(); })));
	Menu.EndSection();
}

void SPlaytestPanel::ToggleSettings()
{
	bSettingsPage = !bSettingsPage;
	bConfirmSignOut = false;
	if (NarrowMenu.IsValid())
	{
		NarrowMenu->SetIsOpen(false);
	}
	if (bSettingsPage)
	{
		Prefs = PlaytestPreferences::Load();  // (another engine's Autopilot may have changed it)
		if (UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get())
		{
			Bridge->RequestAccount();
		}
	}
}

TSharedRef<SWidget> SPlaytestPanel::MakeSaveData(bool bPopup)
{
	// What the agent remembers about this game, and Reset memory (asked in place first: a red-edged plate in its place)
	TSharedRef<SWrapBox> Ask = ButtonRow();
	AddTo(Ask, SmallButton(LOCTEXT("ResetMemoryYes", "Reset memory"), "Autopilot.Button.DangerFill", FOnClicked::CreateLambda([this]()
	{
		bConfirmReset = false;
		if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
		{
			B->ClearMemory();
		}
		return FReply::Handled();
	})));
	AddTo(Ask, SmallButton(LOCTEXT("S_Cancel", "Cancel"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
	{
		bConfirmReset = false;
		return FReply::Handled();
	})));
	TSharedRef<SButton> Reset = AutopilotUI::Button(LOCTEXT("ResetMemory", "Reset memory…"), "Autopilot.Button.Danger",
		FOnClicked::CreateSP(this, &SPlaytestPanel::OnDeleteSave),
		TAttribute<bool>::CreateLambda([]() { return !FAutopilotSession::Get().GetResetLine().StartsWith(TEXT("Resetting")); }), nullptr, true);
	Reset->SetToolTip(AutopilotUI::Tip(TAttribute<FText>::CreateLambda([this]()
	{
		return IsRunning() ? LOCTEXT("ResetMemoryBusy", "A run is going: reset memory once it is done")
			: LOCTEXT("ResetMemoryTip", "Reset memory: the agent forgets what it has learned about this game");
	})));
	// The reset's last word under the row: Resetting memory… (the spinner), Memory reset successful (green), Memory not reset: … (red)
	auto MemoryLine = []() { return FAutopilotSession::Get().GetResetLine(); };
	TSharedRef<SWidget> Outcome = SNew(SBox)
		.Visibility_Lambda([this, MemoryLine]() { return bConfirmReset || MemoryLine().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		.Padding(bPopup ? FMargin(12.f, 0.f, 12.f, 8.f) : FMargin(14.f, 0.f, 14.f, 9.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SBox)
				.Visibility_Lambda([MemoryLine]() { return MemoryLine().StartsWith(TEXT("Resetting")) ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(AutopilotUI::SSpinner)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
				.AutoWrapText(true)
				.Text_Lambda([MemoryLine]() { return Str(MemoryLine()); })
				.ColorAndOpacity_Lambda([MemoryLine]()
				{
					const FString L = MemoryLine();
					return FSlateColor(L.StartsWith(TEXT("Memory reset")) ? AutopilotColor::Success() : L.StartsWith(TEXT("Memory not reset")) ? AutopilotColor::Red() : AutopilotColor::Text3());
				})
			]
		];
	return SNew(SVerticalBox)
	+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
	+ SVerticalBox::Slot().AutoHeight()
	[
		SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([this]() { return bConfirmReset ? 1 : 0; })
		+ SWidgetSwitcher::Slot()
		[
			SNew(SBox).Padding(bPopup ? FMargin(12.f, 8.f) : FMargin(14.f, 9.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					AutopilotUI::Label(LOCTEXT("Memory", "Memory"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Text3()))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Clipping(EWidgetClipping::ClipToBounds)  // (the ellipsis is cut at the clip rect: its own, not the rail's)
					.ToolTip(AutopilotUI::Tip(LOCTEXT("MemoryTip", "What the agent remembers about this game, kept between runs: its earlier runs, its notes on how the game works, what it built to play it. It is what makes later runs better: they are faster and play better with it. \u201cEmpty\u201d: the next run starts as on a new game.")))
					.Text_Lambda([this]()
					{
						if (TestMemoryLine.IsSet())
						{
							return Str(TestMemoryLine.GetValue());
						}
						const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
						if (!B || !B->IsMemoryKnown())
						{
							return FText::GetEmpty();  // (an agent service that doesn't say)
						}
						const int32 N = B->GetMemoryRuns();
						if (N == 0 && !B->HasMemoryNotes() && !B->HasMemoryLib())
						{
							return Str(TEXT("· empty"));
						}
						return Str(FString::Printf(TEXT("· %d %s"), N, N == 1 ? TEXT("run") : TEXT("runs")));
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					// (not connected or a run going: greyed, and pressed it says why on the status line)
					AutopilotUI::Greyed(TAttribute<bool>::CreateLambda([this]() { return !IsConnected() || IsRunning(); }), Reset)
				]
			]
		]
		+ SWidgetSwitcher::Slot()
		[
			SNew(SBox).Padding(FMargin(10.f))
			[
				AutopilotUI::Plate(FMargin(12.f, 11.f),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						AutopilotUI::Body(LOCTEXT("ResetMemoryAsk", "Reset memory?"), 13.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
					[
						AutopilotUI::Body(LOCTEXT("ResetMemoryWhat", "The agent forgets what it has learned about this game."), 12.f, AutopilotColor::Text3())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
					[
						Ask
					], AutopilotColor::RedLine())
			]
		]
	]
	+ SVerticalBox::Slot().AutoHeight()[Outcome];
}

// -- The status line: what just happened (Copied, Showing ..., why something was refused), the main column's last row --

TSharedRef<SWidget> SPlaytestPanel::MakeStatusLine()
{
	// A rule over it, 6 x 16 inside, mono 11 muted, one line cut short (the whole of it as its tooltip); nothing at all
	// while there is nothing to say (a message stays 8 s, or until the next)
	auto Line = []() { return FAutopilotSession::Get().StatusLine(); };
	return SNew(SVerticalBox)
		.Visibility_Lambda([Line]() { return Line().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight().Padding(16.f, 6.f)
		[
			SNew(STextBlock)
			.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Clipping(EWidgetClipping::ClipToBounds)
			.Text_Lambda([Line]() { return Str(Line()); })
			.ToolTip(AutopilotUI::Tip(TAttribute<FText>::CreateLambda([Line]() { return Str(Line()); })))
		];
}

void SPlaytestPanel::SaySaveFailed()
{
	const uint32 Error = FPlatformMisc::GetLastError();
	FAutopilotSession::Get().Say(FString::Printf(TEXT("Couldn’t save the setup to %s (error %u)"), *PlaytestPreferences::PreferencesPath(), Error));
}

// -- The banner when a run is shown and there is no connection --------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeNotice()
{
	auto Trying = []() { const UPlaytestSubsystem* B = UPlaytestSubsystem::Get(); return B && B->WantsConnection(); };
	return SNew(SVerticalBox)
		.Visibility_Lambda([this]()
		{
			return !bSettingsPage && Selected().IsValid() && !IsConnected() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAutopilotStyle::Brush("Autopilot.White"))
			.BorderBackgroundColor(FSlateColor(AutopilotColor::AmberFill()))
			.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bNarrow ? FMargin(10.f, 7.f) : FMargin(16.f, 7.f); }))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::AmberText()))
					.AutoWrapText(true)
					.Text_Lambda([Trying]()
					{
						return Trying() ? LOCTEXT("Reconnecting", "Reconnecting to the agent service…") : LOCTEXT("NotConnectedNotice", "Not connected to the agent service");
					})
				]
				// (trying: what goes on, on the connect card; disconnected by hand: connect again)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SWidgetSwitcher)
					.WidgetIndex_Lambda([Trying]() { return Trying() ? 0 : 1; })
					+ SWidgetSwitcher::Slot()[SmallButton(LOCTEXT("Details", "Details"), "Autopilot.Button", FOnClicked::CreateLambda([this]() { Select(nullptr); return FReply::Handled(); }))]
					+ SWidgetSwitcher::Slot()[SmallButton(LOCTEXT("C_Reconnect", "Reconnect"), "Autopilot.Button", FOnClicked::CreateLambda([]() { TryConnectAgain(); return FReply::Handled(); }))]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule(AutopilotColor::AmberLine())];
}

// -- The top bar -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeTopBar()
{
	// New run and Settings: a header with the page's title; a run: its tabs, and (wide) its line and, live, the link
	auto TabToggle = [this](int32 Which, TAttribute<FText> Text, TAttribute<EVisibility> Shown)
	{
		auto On = [this, Which]() { return !bSettingsPage && Tab == Which; };
		return SNew(SCheckBox)
			.Style(&FAutopilotStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Autopilot.PanelTab"))
			.Visibility(Shown)
			.IsChecked_Lambda([On]() { return On() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Which](ECheckBoxState) { Tab = Which; bSettingsPage = false; })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bNarrow ? FMargin(0.f, 10.f, 0.f, 8.f) : FMargin(0.f, 12.f, 0.f, 10.f); }))
				[
					SNew(STextBlock)
					.Text(Text)
					.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 12.f))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).HeightOverride(2.f)
					[
						AutopilotUI::Fill(TAttribute<FSlateColor>::CreateLambda([On]() { return FSlateColor(On() ? AutopilotColor::Accent() : FLinearColor::Transparent); }))
					]
				]
			];
	};
	auto Gap = [this]() { return FMargin(bNarrow ? 18.f : 20.f, 0.f, 0.f, 0.f); };
	TSharedRef<SWidget> Tabs = SNew(SBox)
		.Padding(TAttribute<FMargin>::CreateLambda([this]() { return FMargin(bNarrow ? 12.f : 16.f, 0.f); }))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
			[
				TabToggle(TabMain, TAttribute<FText>::CreateLambda([this]()
				{
					const TSharedPtr<FAutopilotRun> Run = Selected();
					return Run.IsValid() && !Run->bDone ? LOCTEXT("LiveTab", "Live") : LOCTEXT("ResultTab", "Result");
				}), EVisibility::Visible)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(TAttribute<FMargin>::CreateLambda(Gap))
			[
				TabToggle(TabFeed, LOCTEXT("FeedTab", "Feed"), EVisibility::Visible)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(TAttribute<FMargin>::CreateLambda(Gap))
			[
				TabToggle(TabPolicy, TAttribute<FText>::CreateLambda([this]()
				{
					const TSharedPtr<FAutopilotRun> Run = Selected();
					return FText::Format(LOCTEXT("PolicyTab", "Policy log · {0}"), FText::AsNumber(Run.IsValid() ? Run->Steps.Num() : 0, &FNumberFormattingOptions::DefaultNoGrouping()));
				}), TAttribute<EVisibility>::CreateLambda([this]()
				{
					const TSharedPtr<FAutopilotRun> Run = Selected();
					return Run.IsValid() && Run->Steps.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;  // (a saved run's steps aren't kept)
				}))
			]
			// (wide: the run's line, at the right, cut short rather than pushing the tabs out)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(20.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return bNarrow ? EVisibility::Collapsed : EVisibility::Visible; })
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f, 60))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
				.Justification(ETextJustify::Right)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Clipping(EWidgetClipping::ClipToBounds)
				.Text_Lambda([this]() { const TSharedPtr<FAutopilotRun> Run = Selected(); return Run.IsValid() ? Str(Run->MetaLine()) : FText::GetEmpty(); })
			]
			// (wide, live: the link)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(20.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					const TSharedPtr<FAutopilotRun> Run = Selected();
					return !bNarrow && Run.IsValid() && !Run->bDone && !LinkText().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f, 60))
				.Text(this, &SPlaytestPanel::LinkText)
				.ColorAndOpacity(FSlateColor(AutopilotColor::Success()))
			]
		];
	// (narrow: 10 x 12)
	TSharedRef<SWidget> NewRunHead = PageHeader(HeaderTitle(LOCTEXT("NewRunTitle", "New run")),
		TAttribute<FMargin>::CreateLambda([this]() { return bNarrow ? FMargin(12.f, 10.f) : FMargin(16.f, 12.f); }));
	TSharedRef<SWidget> SettingsHead = PageHeader(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[HeaderTitle(LOCTEXT("SettingsTitle", "Settings"))]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SmallButton(LOCTEXT("BackToRuns", "Back to runs"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
			{
				if (bSettingsPage)
				{
					ToggleSettings();
				}
				return FReply::Handled();
			}))
		]);
	return SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([this]()
		{
			if (bSettingsPage)
			{
				return 3;
			}
			const int32 View = ViewIndex();
			return View == ViewConnect ? 0 : View == ViewComposer ? 1 : 2;
		})
		+ SWidgetSwitcher::Slot()[SNullWidget::NullWidget]
		+ SWidgetSwitcher::Slot()[NewRunHead]
		+ SWidgetSwitcher::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Tabs]
			+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		]
		+ SWidgetSwitcher::Slot()[SettingsHead];
}

TSharedRef<SButton> SPlaytestPanel::MakeRerunSetup(const FName& Style)
{
	// The first-run setup again, from its first step (not while a run goes: changing the account would end its connection)
	TSharedRef<SButton> Button = AutopilotUI::Button(LOCTEXT("RerunSetup", "Rerun setup"), Style, FOnClicked::CreateLambda([this]()
	{
		if (CanChangeAccount())
		{
			OpenSetup();
		}
		return FReply::Handled();
	}), TAttribute<bool>::CreateSP(this, &SPlaytestPanel::CanChangeAccount), nullptr, true);
	Button->SetToolTip(AutopilotUI::Tip(TAttribute<FText>::CreateSP(this, &SPlaytestPanel::RerunSetupTip)));
	return Button;
}

FText SPlaytestPanel::RerunSetupTip() const
{
	return CanChangeAccount() ? LOCTEXT("RerunSetupTip", "The first-run setup again: your account (Aura or BYOK), the agent service, your privacy settings")
		: LOCTEXT("RunBusy", "Finish or stop the run first");
}

bool SPlaytestPanel::CanChangeAccount() const
{
	return TestRunning.IsSet() ? !TestRunning.GetValue() : !IsRunning();
}

FText SPlaytestPanel::LinkText() const
{
	// "● LOCAL AGENT" while connected to an agent service on this computer (a loopback address); else nothing
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B || !B->IsConnected())
	{
		return FText::GetEmpty();
	}
	const FString& Url = B->GetUrl();
	const bool bLocal = Url.Contains(TEXT("127.0.0.1")) || Url.Contains(TEXT("localhost")) || Url.Contains(TEXT("[::1]"));
	return bLocal ? LOCTEXT("LinkLocal", "● LOCAL AGENT") : FText::GetEmpty();
}

FSlateColor SPlaytestPanel::LinkColor() const
{
	if (IsConnected())
	{
		return FSlateColor(AutopilotColor::Success());
	}
	return FSlateColor(IsConnecting() || IsUnreachable() ? AutopilotColor::Amber() : AutopilotColor::Muted());
}

// -- Not connected -----------------------------------------------------------------------------------------------

SPlaytestPanel::EConnectView SPlaytestPanel::ConnectView() const
{
	if (TestConnectView.IsSet())
	{
		return TestConnectView.GetValue();
	}
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B)
	{
		return EConnectView::NoBridge;
	}
	if (B->IsWithAura() && B->WantsConnection() && !B->IsConnected() && !B->GetProblem().IsEmpty())
	{
		return EConnectView::Aura;  // (Aura not running, or not installed)
	}
	// The agent service the editor starts: installing, starting, or why it can't
	if (B->ManagesAgentService() && !B->IsConnected() && B->GetAgentService())
	{
		switch (ServiceView())
		{
		case EServiceView::Installing:
			return EConnectView::Installing;
		case EServiceView::Starting:
			return EConnectView::Starting;
		case EServiceView::NotInstalled:
			return EConnectView::NotInstalled;
		case EServiceView::NoInstaller:
			return EConnectView::NoInstaller;
		case EServiceView::Problem:
			return EConnectView::Failed;
		default:
			break;
		}
	}
	if (IsRefused())
	{
		return EConnectView::Refused;
	}
	if (IsUnreachable())
	{
		return EConnectView::Unreachable;
	}
	return IsConnecting() ? EConnectView::Connecting : EConnectView::Disconnected;
}

FText SPlaytestPanel::ConnectMessage() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	switch (ConnectView())
	{
	case EConnectView::NoBridge:
		return LOCTEXT("NoBridge", "Autopilot isn’t available in this editor.");
	case EConnectView::Aura:
		// (installed, not running: the Aura app connects once it starts; else what the subsystem says)
		return PlaytestAura::IsInstalled() || !B ? LOCTEXT("C_AuraNotRunning", "Aura isn’t running on this machine.")
			: LOCTEXT("C_AuraNotInstalled", "Aura isn’t installed on this machine.");
	case EConnectView::Installing:
		return LOCTEXT("C_Installing", "Installing the agent service…");
	case EConnectView::Starting:
		return LOCTEXT("C_Starting", "Starting the agent service…");
	case EConnectView::NotInstalled:
		return LOCTEXT("C_NotInstalled", "The agent service isn’t installed on this computer.");
	case EConnectView::NoInstaller:
		return LOCTEXT("C_NoInstaller", "No agent service found.");
	case EConnectView::Failed:
		return LOCTEXT("C_Failed", "The agent service didn\u2019t start.");  // (what went wrong: the detail line under it)
	case EConnectView::Refused:
	{
		const FText Why = Str(FAutopilotSession::Scrub(B->GetProblem()));
		const int32 Code = B->GetProblemCode();
		return Code == 401 || Code == 403 ? LOCTEXT("Unauthorized", "The agent service refused the connection: the access token is wrong.") : Why;
	}
	case EConnectView::Unreachable:
		return LOCTEXT("Unreachable", "Can’t reach the agent service, retrying");
	case EConnectView::Connecting:
		return LOCTEXT("Connecting", "Connecting to the agent service…");
	default:
		return FText::GetEmpty();  // (Disconnected: the tag says it)
	}
}

FString SPlaytestPanel::ConnectKey() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	return FString::Printf(TEXT("%d|%d|%s|%s|%s|%d|%d|%d|%d"), PreviousRunId, (int32)ConnectView(), *ConnectMessage().ToString(), B ? *B->GetUrl() : TEXT(""),
		S ? *S->GetLogPath() : TEXT(""), S && S->GetState() == EPlaytestServiceState::BadCommand ? 1 : 0,
		PlaytestAura::IsInstalled() ? 1 : 0, PlaytestAura::ServiceUrl().IsEmpty() ? 0 : 1, GetDefault<UPlaytestSettings>()->bUseWithAura ? 1 : 0);
}

TSharedRef<SWidget> SPlaytestPanel::MakeConnectCard()
{
	return SNew(SScrollBox)
	+ SScrollBox::Slot()
	[
		Capped(SAssignNew(ConnectBox, AutopilotUI::SSettle), ConnectMax, FMargin(20.f, 24.f))
	];
}

TSharedRef<SWidget> SPlaytestPanel::MakeAdvanced()
{
	// Advanced (folded): where the agent service is, its access token and its command (the plugin's settings, also in
	// Editor Preferences > Plugins > Autopilot), each kept when Enter is pressed or the field is left
	const FEditableTextBoxStyle* BoxStyle = &FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox");
	const FEditableTextBoxStyle* MonoStyle = &FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Mono");
	auto Field = [](const FText& Name, const TSharedRef<SWidget>& Box) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 5.f)[AutopilotUI::Label(Name)]
			+ SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(AutopilotUI::ButtonHeight)[Box]];
	};
	auto Save = [](FString UPlaytestSettings::* Member, const FText& Text)
	{
		UPlaytestSettings* Settings = GetMutableDefault<UPlaytestSettings>();
		const FString Value = Text.ToString().TrimStartAndEnd();
		if (Settings->*Member != Value)
		{
			Settings->*Member = Value;
			Settings->SaveConfig();
		}
	};
	TSharedRef<SVerticalBox> Fields = SNew(SVerticalBox)
		.Visibility_Lambda([this]() { return bAdvancedOpen ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			Field(LOCTEXT("C_Url", "Agent service URL"), SNew(SEditableTextBox)
				.Style(MonoStyle)
				.ToolTip(AutopilotUI::Tip(LOCTEXT("C_UrlTip", "Empty: ws://127.0.0.1:8765/v1/bridge, the agent service on this computer. You may use your own endpoint.")))
				.Text_Lambda([]() { return Str(GetDefault<UPlaytestSettings>()->ServiceUrl); })
				.OnTextCommitted_Lambda([Save](const FText& Text, ETextCommit::Type) { Save(&UPlaytestSettings::ServiceUrl, Text); }))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			Field(LOCTEXT("C_Token", "Access token · optional"), SNew(SEditableTextBox)
				.Style(BoxStyle)
				.IsPassword(true)
				.Text_Lambda([]() { return Str(GetDefault<UPlaytestSettings>()->AccessToken); })
				.OnTextCommitted_Lambda([Save](const FText& Text, ETextCommit::Type) { Save(&UPlaytestSettings::AccessToken, Text); }))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			Field(LOCTEXT("C_Command", "Agent service command · optional"), SNew(SEditableTextBox)
				.Style(BoxStyle)
				.HintText(LOCTEXT("C_CommandHint", "Detected automatically"))
				.ToolTip(AutopilotUI::Tip(LOCTEXT("C_CommandTip", "The command that runs the agent service, without “serve”: e.g. C:\\autopilot\\.venv\\Scripts\\python.exe -m autopilot")))
				.Text_Lambda([]() { return Str(GetDefault<UPlaytestSettings>()->AgentServiceCommand); })
				.OnTextCommitted_Lambda([Save](const FText& Text, ETextCommit::Type) { Save(&UPlaytestSettings::AgentServiceCommand, Text); }))
		];
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).Padding(FMargin(14.f, 9.f, 14.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
				[
					SNew(SButton)
					.ButtonStyle(&FAutopilotStyle::Get().GetWidgetStyle<FButtonStyle>("Autopilot.Button.Bare"))
					.ContentPadding(FMargin(0.f))
					.OnClicked_Lambda([this]() { bAdvancedOpen = !bAdvancedOpen; return FReply::Handled(); })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
						[
							// (the disclosure mark: down when open)
							SNew(SImage)
							.Image(FAutopilotStyle::Brush("Autopilot.Chevron"))
							.RenderTransformPivot(FVector2D(0.5, 0.5))
							.RenderTransform_Lambda([this]() { return TOptional<FSlateRenderTransform>(FSlateRenderTransform(TQuat2<float>(bAdvancedOpen ? 0.f : -0.5f * PI))); })
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							AutopilotUI::Label(LOCTEXT("C_Advanced", "Advanced"))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight()[Fields]
			]
		];
}

void SPlaytestPanel::RebuildConnect()
{
	if (!ConnectBox.IsValid())
	{
		return;
	}
	const EConnectView View = ConnectView();
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	// "Agent service", not connected, and how it starts: with autopilot serve, or the Aura app's
	Body->AddSlot().AutoHeight()
	[
		SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8.f, 4.f))
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("C_Title", "Agent service"))
			.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 15.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
		]
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::Tag(LOCTEXT("C_NotConnected", "Not connected"), EAutopilotTone::Neutral)
		]
	];
	const bool bWithAura = GetDefault<UPlaytestSettings>()->bUseWithAura;
	Body->AddSlot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
	[
		bWithAura ? StaticCastSharedRef<SWidget>(AutopilotUI::Body(LOCTEXT("C_StartAura", "Start the Aura app: it runs the agent service."), 12.f, AutopilotColor::Text3()))
		: StaticCastSharedRef<SWidget>(SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.f, 0.f))
			+ SWrapBox::Slot().VAlign(VAlign_Center)
			[
				AutopilotUI::Body(LOCTEXT("C_StartWith", "Start the agent service with"), 12.f, AutopilotColor::Text3(), false)
			]
			+ SWrapBox::Slot().VAlign(VAlign_Center)
			[
				AutopilotUI::Mono(Str(TEXT("autopilot serve")), 11.5f, FSlateColor(AutopilotColor::Accent()))
			])
	];
	// The state: an alert saying it (info with the spinner while it goes, amber to act on, red when it failed)
	EAutopilotTone Tone = EAutopilotTone::Info;
	bool bProgress = false;
	FString Detail;
	switch (View)
	{
	case EConnectView::Connecting:
	case EConnectView::Starting:
	case EConnectView::Installing:
		bProgress = true;
		break;
	case EConnectView::NotInstalled:
	case EConnectView::NoInstaller:
	case EConnectView::Unreachable:
	case EConnectView::Aura:
		Tone = EAutopilotTone::Amber;
		if (View == EConnectView::NoInstaller)
		{
			Detail = TEXT("Set Agent service command under Advanced, or run autopilot serve.");
		}
		else if (View == EConnectView::Unreachable && B)
		{
			Detail = B->GetUrl();
		}
		else if (View == EConnectView::Aura && PlaytestAura::IsInstalled())
		{
			Detail = TEXT("Autopilot connects once it starts.");
		}
		break;
	case EConnectView::Failed:
		Tone = EAutopilotTone::Red;
		Detail = S ? S->GetProblem() : FString();  // (the service's own words, small, under the headline)
		break;
	case EConnectView::Refused:
	case EConnectView::NoBridge:
		Tone = EAutopilotTone::Red;
		break;
	default:
		break;
	}
	const FText Message = ConnectMessage();
	if (!Message.IsEmpty())
	{
		const FAutopilotTone& T = FAutopilotStyle::Tone(Tone);
		TSharedRef<SVerticalBox> Says = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				bProgress ? Progress(Message.ToString(), 13.f, T.Text)
					: StaticCastSharedRef<SWidget>(AutopilotUI::Body(Message, 13.f, T.Text, true, EAutopilotFont::BodyMedium))
			];
		if (!Detail.IsEmpty())
		{
			Says->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Str(Detail))
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.5f))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Neutral()))
				.AutoWrapText(true)
			];
		}
		Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[AutopilotUI::Alert(Tone, Says)];
	}
	TSharedRef<SWrapBox> Row = ButtonRow();
	auto Add = [&Row](const FText& Text, const FName& Style, TFunction<void()> Do)
	{
		AddTo(Row, AutopilotUI::Button(Text, Style, FOnClicked::CreateLambda([Do]() { Do(); return FReply::Handled(); })));
	};
	const TFunction<void()> StopTrying = []() { UserDisconnect(); };
	const TFunction<void()> Again = []() { TryConnectAgain(); };
	switch (View)
	{
	case EConnectView::Connecting:
		Add(LOCTEXT("C_Stop", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	case EConnectView::Disconnected:
		Add(LOCTEXT("C_Reconnect", "Reconnect"), "Autopilot.Button.Primary", Again);
		break;
	case EConnectView::Starting:
	case EConnectView::Installing:
		Add(LOCTEXT("StopTrying", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	case EConnectView::NotInstalled:
		Add(LOCTEXT("C_Install", "Install"), "Autopilot.Button.Primary", []()
		{
			GUserDisconnected = false;
			UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
			if (FPlaytestAgentService* Service = ServiceOf(Bridge))
			{
				Service->Install();
				Bridge->Connect();  // (it starts once installed)
			}
		});
		Add(LOCTEXT("StopTrying", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	case EConnectView::NoInstaller:
		Add(LOCTEXT("TryAgain", "Try again"), "Autopilot.Button.Primary", Again);
		Add(LOCTEXT("StopTrying", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	case EConnectView::Unreachable:
		Add(LOCTEXT("TryNow", "Try now"), "Autopilot.Button.Primary", Again);
		Add(LOCTEXT("StopTrying", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	case EConnectView::Failed:
	{
		Add(LOCTEXT("TryAgain", "Try again"), "Autopilot.Button.Primary", Again);
		const FString Log = S ? S->GetLogPath() : FString();
		if (S && !Log.IsEmpty())
		{
			Add(LOCTEXT("C_OpenLog", "View logs"), "Autopilot.Button", [Log]() { OpenLog(Log); });
		}
		Add(LOCTEXT("StopTrying", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	}
	case EConnectView::Refused:
		Add(LOCTEXT("TryAgain", "Try again"), "Autopilot.Button.Primary", Again);
		break;
	case EConnectView::Aura:
		if (PlaytestAura::IsInstalled() && PlaytestAura::ServiceUrl().IsEmpty())
		{
			Add(LOCTEXT("StartAura", "Start Aura"), "Autopilot.Button", []()
			{
				if (!PlaytestAura::Launch())
				{
					FAutopilotSession::Get().Say(TEXT("Couldn’t start Aura"));
				}
			});
		}
		else if (!PlaytestAura::IsInstalled())
		{
			// (the Aura download page)
			Add(LOCTEXT("GetAura", "Get Aura"), "Autopilot.Button", []() { FPlatformProcess::LaunchURL(PlaytestAura::DownloadUrl, nullptr, nullptr); });
		}
		Add(LOCTEXT("TryNow", "Try now"), "Autopilot.Button.Primary", Again);
		Add(LOCTEXT("StopTrying", "Stop"), "Autopilot.Button.Quiet", StopTrying);
		break;
	default:
		break;
	}
	if (Row->GetChildren()->Num())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Row];
	}
	// Disconnected, a run shown before: Back to the last run, under the card
	const TSharedPtr<FAutopilotRun> Last = View == EConnectView::Disconnected && PreviousRunId != INDEX_NONE ? FAutopilotSession::Get().FindRun(PreviousRunId) : nullptr;
	TSharedRef<SVerticalBox> Page = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			AutopilotUI::Plate(FMargin(0.f),
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[SNew(SBox).Padding(FMargin(14.f, 12.f))[Body]]
				+ SVerticalBox::Slot().AutoHeight()[MakeAdvanced()])
		];
	if (Last.IsValid())
	{
		Page->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f).HAlign(HAlign_Left)
		[
			SmallButton(LOCTEXT("BackToLastRun", "Back to the last run"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this, Last]()
			{
				Select(Last);
				return FReply::Handled();
			}))
		];
	}
	ConnectBox->SetContent(Page);
}

// -- New run -----------------------------------------------------------------------------------------------------

namespace
{
	/** A number field (minutes): a square mono well, 28 tall, Width wide, its number at the right; Set(value, committed). */
	TSharedRef<SWidget> NumberField(TFunction<int32()> Get, TFunction<void(int32, bool)> Set, float Width)
	{
		return SNew(SBox)
			.WidthOverride(Width)
			.HeightOverride(AutopilotUI::ButtonHeight)
			[
				SNew(SSpinBox<int32>)
				.Style(&FAutopilotStyle::Get().GetWidgetStyle<FSpinBoxStyle>("Autopilot.SpinBox"))
				.MinValue(1).MaxValue(600)
				.Justification(ETextJustify::Right)
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 12.f))
				.Value_Lambda([Get]() { return Get(); })
				.OnValueChanged_Lambda([Set](int32 V) { Set(V, false); })
				.OnValueCommitted_Lambda([Set](int32 V, ETextCommit::Type) { Set(V, true); })
			];
	}
}

TSharedRef<SWidget> SPlaytestPanel::MakeComposer()
{
	// One box (the composer: the prompt, then its Time limit and Run), a line while a run goes, and at the foot the
	// connection and Disconnect. The kind of run the words ask for is the decision model's to read once the run starts
	auto Shown = [](bool bShow) { return bShow ? EVisibility::Visible : EVisibility::Collapsed; };
	TSharedRef<SHorizontalBox> RunRow = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 8.f, 0.f)
		[
			SNew(SBox)
			.ToolTip(AutopilotUI::Tip(LOCTEXT("TimeLimitTip", "Minutes the run may take: a claim's or a goal's whole run, an exploration's play")))
			[
				AutopilotUI::Label(LOCTEXT("ComposerTimeLimit", "Time limit"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			// (kept for you on this project: the next run, and the tab opened again, start with it)
			NumberField([this]() { return TimeLimit; }, [this](int32 V, bool bSave)
			{
				TimeLimit = FMath::Clamp(V, 1, 600);
				UPlaytestSettings* Settings = GetMutableDefault<UPlaytestSettings>();
				Settings->RunTimeLimitMinutes = TimeLimit;
				if (bSave)
				{
					Settings->SaveConfig();
				}
			}, 52.f)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			// (no words: disabled; not connected or a run going: greyed, and pressed it says why on the status line)
			AutopilotUI::Greyed(TAttribute<bool>::CreateLambda([this]() { return !PromptText().IsEmpty() && !CanRun(); }),
				AutopilotUI::Button(LOCTEXT("Run", "Run"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnRun),
					TAttribute<bool>::CreateLambda([this]() { return !PromptText().IsEmpty(); }), nullptr, false, 18.f))
		];
	TSharedRef<SWidget> Composer = AutopilotUI::Box(AutopilotColor::Well(), AutopilotColor::Line(), AutopilotRadius::Plate, FMargin(12.f, 11.f, 12.f, 9.f),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			// (five lines high, as the design's; six in a narrow dock)
			SNew(SBox).MinDesiredHeight_Lambda([this]() { return FOptionalSize((bNarrow ? 6.f : 5.f) * 13.f * 1.55f); })
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
				.Style(&FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Bare"))
				.HintText(LOCTEXT("PromptHint", "Describe a claim to verify, a goal to reach, or what to test"))
				.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			RunRow
		]);
	// (the address cut short, not the row, in a narrow dock)
	TSharedRef<SWidget> Foot = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					AutopilotUI::Mono(LOCTEXT("Connected", "● CONNECTED"), 10.f, FSlateColor(AutopilotColor::Success()), false, 100)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Clipping(EWidgetClipping::ClipToBounds)
					.Text_Lambda([]() { const UPlaytestSubsystem* B = UPlaytestSubsystem::Get(); return B ? Str(B->GetUrl()) : FText::GetEmpty(); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				AutopilotUI::Button(LOCTEXT("Disconnect", "Disconnect"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([]()
				{
					UserDisconnect();  // (no connecting by itself again until Reconnect)
					return FReply::Handled();
				}), true, nullptr, true)
			]
		];
	return Capped(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[MakeOutOfCredits()]
				+ SVerticalBox::Slot().AutoHeight()[Composer]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(SBox)
					.Visibility_Lambda([this, Shown]() { return Shown(IsRunning()); })
					[
						AutopilotUI::Body(LOCTEXT("RunGoing", "A run is going: stop it or wait for its result."), 12.f, AutopilotColor::Muted())
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Foot],
		ComposerMax, TAttribute<FMargin>::CreateLambda([this]() { return bNarrow ? FMargin(12.f, 16.f, 12.f, 12.f) : FMargin(20.f, 28.f, 20.f, 20.f); }));
}

TSharedRef<SWidget> SPlaytestPanel::MakeOutOfCredits()
{
	// Aura's words while the team has no credits left for a run (the account frame's out_of_credits), red, over the
	// composer, with Add credits when Aura names where; Run stays: Aura checks again when a run starts
	auto Words = [this]() { const FPlaytestAccount A = ShownAccount(); return A.Mode == TEXT("aura") ? A.OutOfCredits : FString(); };
	auto Url = [this]() { const FPlaytestAccount A = ShownAccount(); return A.Mode == TEXT("aura") ? A.AddCreditsUrl : FString(); };
	return SNew(SBox)
		.Visibility_Lambda([Words]() { return Words().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		.Padding(0.f, 0.f, 0.f, 10.f)
		[
			AutopilotUI::Box(AutopilotColor::RedFill(), AutopilotColor::RedLine(), AutopilotRadius::Plate, FMargin(12.f, 9.f),
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::RedText()))
					.AutoWrapText(true)
					.Text_Lambda([Words]() { return Str(Words()); })
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.f, 8.f, 0.f, 0.f)
				[
					SNew(SBox)
					.Visibility_Lambda([Url]() { return Url().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SmallButton(LOCTEXT("AddCredits", "Add credits"), "Autopilot.Button", FOnClicked::CreateLambda([Url]()
						{
							FPlatformProcess::LaunchURL(*Url(), nullptr, nullptr);
							return FReply::Handled();
						}))
					]
				])
		];
}

FReply SPlaytestPanel::OnRun()
{
	if (PromptText().IsEmpty())
	{
		return FReply::Handled();
	}
	if (!IsConnected())
	{
		FAutopilotSession::Get().Say(TEXT("Not connected: connect first"));
		return FReply::Handled();
	}
	if (IsRunning())
	{
		FAutopilotSession::Get().Say(TEXT("A run is going: wait for its result, or stop it"));
		return FReply::Handled();
	}
	FString Prompt;
	const TSharedRef<FJsonObject> Frame = FAutopilotSession::RunFrame(PromptText(), TimeLimit, GetDefault<UPlaytestSettings>()->DesignBrief, Prompt);
	// (its kind blank until the decision model reads it: run_kind)
	if (TSharedPtr<FAutopilotRun> Run = FAutopilotSession::Get().StartRun(FString(), Prompt, Frame))
	{
		SyncRuns();
		Select(Run);
	}
	return FReply::Handled();
}

// -- A run going -------------------------------------------------------------------------------------------------

int32 SPlaytestPanel::AttemptSquaresShown() const
{
	const TSharedPtr<FAutopilotRun> Run = Selected();
	const int32 Attempts = Run.IsValid() ? FMath::Max(0, Run->Attempt) : 0;
	if (Attempts == 0)
	{
		return 0;
	}
	// The row's room (the Live page's, 16 px each side) less its label, the count and a little of the policy steps
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const float Label = Measure->Measure(FString(TEXT("Harness attempts")).ToUpper(), FAutopilotStyle::Label(10.f)).X;
	const float Count = Measure->Measure(FString::Printf(TEXT("%d"), Attempts), FAutopilotStyle::Font(EAutopilotFont::MonoBold, 13.f)).X;
	const int32 Steps = Run->StepCount;
	const float StepsWidth = Steps > 0
		? FMath::Min(100.f, Measure->Measure(FString::Printf(TEXT("%d %s"), Steps, Steps == 1 ? TEXT("policy step") : TEXT("policy steps")),
			FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f)).X) : 0.f;
	const float Room = MainWidth() - 32.f - (Label + 8.f) - (6.f + Count) - (12.f + StepsWidth) - 2.f;
	const int32 Fit = FMath::Max(0, FMath::FloorToInt((Room + 4.f) / 14.f));  // (10 px each, 4 apart)
	return FMath::Min3(Attempts, MostAttemptSquares, Fit);
}

float SPlaytestPanel::StageTrackWidth() const
{
	return FMath::Max(1.f, MainWidth() - 32.f);  // (the Live page's, with 16 px each side)
}

TSharedRef<SWidget> SPlaytestPanel::MakeStageTrack()
{
	// One 2px bar (the rule; the accent as far as the run has come), then the stages on a row that wraps: each as wide as its
	// whole name (never cut), the row's width shared out among them; when the names don't fit on one line, the rest go on
	// the next
	StageLabels.Reset();
	const int32 Count = (int32)UE_ARRAY_COUNT(StageNames);
	TSharedRef<SWrapBox> Track = SNew(SWrapBox)
		.UseAllottedSize(true)
		.PreferredSize_Lambda([this]() { return StageTrackWidth(); })  // (until it is laid out: its width then)
		.HAlign(HAlign_Fill)
		.InnerSlotPadding(FVector2D(2.f, 8.f));
	for (int32 i = 0; i < Count; ++i)
	{
		const int32 Stage = i + 1;  // (EAutopilotStage: None is 0)
		auto State = [this, Stage]()
		{
			const TSharedPtr<FAutopilotRun> Run = Selected();
			const int32 Now = Run.IsValid() ? (int32)Run->Stage : 0;
			return Now == Stage ? 0 : Now > Stage ? -1 : 1;  // current, done, to come
		};
		auto Name = [this, i]()
		{
			if (i == 4)
			{
				const TSharedPtr<FAutopilotRun> Run = Selected();
				const int32 Attempt = Run.IsValid() ? Run->Attempt : 0;
				return Attempt > 0 ? Str(FString::Printf(TEXT("Rollout · H%d"), Attempt)) : Str(TEXT("Rollout"));
			}
			return Str(StageNames[i]);
		};
		TSharedRef<STextBlock> Label = SNew(STextBlock)
			.Font_Lambda([State]() { return FAutopilotStyle::Font(State() == 0 ? EAutopilotFont::MonoBold : EAutopilotFont::Mono, 10.f, 80); })
			.TransformPolicy(ETextTransformPolicy::ToUpper)
			.ColorAndOpacity_Lambda([State]()
			{
				const int32 S = State();
				return FSlateColor(S == 0 ? AutopilotColor::Accent() : S < 0 ? AutopilotColor::Text2() : AutopilotColor::Muted());
			})
			.Text_Lambda(Name);
		StageLabels.Add(Label);
		Track->AddSlot().VAlign(VAlign_Top)
		[
			// (room for the name in medium, the current stage's: the row doesn't move when the stage changes)
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(STextBlock)
				.Visibility(EVisibility::Hidden)
				.Font(FAutopilotStyle::Font(EAutopilotFont::MonoBold, 10.f, 80))
				.TransformPolicy(ETextTransformPolicy::ToUpper)
				.Text_Lambda(Name)
			]
			+ SOverlay::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[Label]
				// (how long it took, or has taken so far: m:ss, the current one's in the accent)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
					.ColorAndOpacity_Lambda([State]() { return FSlateColor(State() == 0 ? AutopilotColor::Accent() : AutopilotColor::Muted()); })
					.Text_Lambda([this, Stage]()
					{
						const TSharedPtr<FAutopilotRun> Run = Selected();
						const double Took = Run.IsValid() ? Run->StageSeconds(Stage) : -1.0;
						return Took >= 0.0 ? Str(FAutopilotSession::ClockShort(Took)) : Str(TEXT(" "));
					})
				]
			]
		];
	}
	auto Progress = [this, Count]()
	{
		// (half a stage into the current one)
		const TSharedPtr<FAutopilotRun> Run = Selected();
		const int32 Now = Run.IsValid() ? (int32)Run->Stage : 0;
		return Now <= 0 ? 0.f : FMath::Clamp((Now - 0.5f) / Count, 0.f, 1.f);
	};
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).HeightOverride(2.f)
			[
				AutopilotUI::Fill(FSlateColor(AutopilotColor::Rule()), FMargin(0.f),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(TAttribute<float>::CreateLambda([Progress]() { return FMath::Max(Progress(), 0.0001f); }))
					[
						AutopilotUI::Fill(FSlateColor(AutopilotColor::Accent()))
					]
					+ SHorizontalBox::Slot().FillWidth(TAttribute<float>::CreateLambda([Progress]() { return FMath::Max(1.f - Progress(), 0.0001f); })))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			Track
		];
}

TSharedRef<SWidget> SPlaytestPanel::MakeLive()
{
	// The head: ● LIVE, the prompt, the time so far and Stop; under them, where the run was started when not here (a rule
	// under it)
	TSharedRef<SWidget> Head = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).Padding(FMargin(16.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
				[
					AutopilotUI::Mono(LOCTEXT("LiveMark", "● LIVE"), 10.f, FSlateColor(AutopilotColor::Accent()), false, 100)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 15.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Clipping(EWidgetClipping::ClipToBounds)
					.Text_Lambda([this]() { const TSharedPtr<FAutopilotRun> Run = Selected(); return Run.IsValid() ? Str(PromptOf(*Run)) : FText::GetEmpty(); })
					.ToolTipText_Lambda([this]() { const TSharedPtr<FAutopilotRun> Run = Selected(); return Run.IsValid() ? Str(PromptOf(*Run)) : FText::GetEmpty(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 12.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this]() { return IsCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
					[
						AutopilotUI::Label(LOCTEXT("Elapsed", "Elapsed"))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						AutopilotUI::Mono(TAttribute<FText>::CreateLambda([this]()
						{
							const TSharedPtr<FAutopilotRun> Run = Selected();
							return Run.IsValid() ? Str(FAutopilotSession::Clock(Run->Elapsed())) : FText::GetEmpty();
						}), 18.f, FSlateColor(AutopilotColor::Text()), true)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					// (Stopping…, greyed, once asked: the agent service said it is stopping)
					AutopilotUI::Button(TAttribute<FText>::CreateLambda([this]() { return IsStopping() ? LOCTEXT("Stopping", "Stopping…") : LOCTEXT("Stop", "Stop"); }),
						"Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestPanel::OnStop),
						TAttribute<bool>::CreateLambda([this]() { const UPlaytestSubsystem* B = UPlaytestSubsystem::Get(); return B && B->IsConnected() && B->IsRunActive() && !IsStopping(); }))
				]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Text3()))
					.Text_Lambda([this]() { const TSharedPtr<FAutopilotRun> Run = Selected(); return Run.IsValid() ? Str(Run->OriginLine()) : FText::GetEmpty(); })
					.Visibility_Lambda([this]()
					{
						const TSharedPtr<FAutopilotRun> Run = Selected();
						return Run.IsValid() && !Run->OriginLine().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
					})
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()];

	// What the agent does now: a plate (Now · harness N, then the line)
	TSharedRef<SWidget> NowCard = AutopilotUI::Plate(FMargin(11.f, 9.f),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			AutopilotUI::Label(TAttribute<FText>::CreateLambda([this]()
			{
				const TSharedPtr<FAutopilotRun> Run = Selected();
				return Run.IsValid() && Run->Attempt > 0 ? FText::Format(LOCTEXT("NowHarness", "Now · harness {0}"), Run->Attempt) : LOCTEXT("Now", "Now");
			}))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				Dot(AutopilotColor::Accent())
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				AutopilotUI::Body(TAttribute<FText>::CreateLambda([this]()
				{
					const TSharedPtr<FAutopilotRun> Run = Selected();
					if (!Run.IsValid())
					{
						return FText::GetEmpty();
					}
					if (!Run->Now.IsEmpty())
					{
						return Str(Run->Now);
					}
					for (int32 i = Run->Feed.Num() - 1; i >= 0; --i)
					{
						if (Run->Feed[i]->Kind == EAutopilotFeedKind::Status || Run->Feed[i]->Kind == EAutopilotFeedKind::Attempt)
						{
							// (the game it reads the code of is still loading: that is what the run waits on)
							if (!Run->bDone && Run->Feed[i]->Stage == EAutopilotStage::Analyze && FAutopilotSession::PieLoading())
							{
								return LOCTEXT("WaitingForPie", "Waiting for PIE");
							}
							// (then the Blueprint export, which loads every asset: how far it is)
							int32 Done = 0;
							int32 Total = 0;
							const UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
							if (!Run->bDone && Run->Feed[i]->Stage == EAutopilotStage::Analyze && Bridge && Bridge->GetExportProgress(Done, Total))
							{
								return Total > 0 ? FText::Format(LOCTEXT("IndexingFilesCount", "Indexing files ({0}/{1})"), Done, Total)
									: LOCTEXT("IndexingFiles", "Indexing files");
							}
							return Str(Run->Feed[i]->Text);
						}
					}
					return LOCTEXT("Waiting", "Waiting for the agent service…");
				}), 14.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
			]
		]);

	// Its harness attempts: a square each (filled: done; outlined: the current one), their count, and its policy steps
	// (one row, left to right, 4 px apart, never wrapping: the newest attempts that fit, at most 24, so the current one
	// is always the last; the count after them says them all)
	TSharedRef<SHorizontalBox> Squares = SNew(SHorizontalBox);
	AttemptSquares.Reset();
	for (int32 i = 1; i <= MostAttemptSquares; ++i)
	{
		TSharedRef<SBox> Square = SNew(SBox)
			.WidthOverride(10.f)
			.HeightOverride(10.f)
			.Visibility_Lambda([this, i]() { return i <= AttemptSquaresShown() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SBorder)
				.BorderImage_Lambda([this, i]()
				{
					// (square i of those shown: attempt N - shown + i, the last one attempt N)
					const TSharedPtr<FAutopilotRun> Run = Selected();
					const bool bCurrent = Run.IsValid() && i == AttemptSquaresShown() && !Run->bDone;
					return bCurrent ? FAutopilotStyle::Box(FLinearColor::Transparent, AutopilotColor::Accent(), 0.f, 2.f)
						: FAutopilotStyle::Box(AutopilotColor::Accent(), AutopilotColor::Accent());
				})
			];
		AttemptSquares.Add(Square);
		Squares->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(i > 1 ? 4.f : 0.f, 0.f, 0.f, 0.f)[Square];
	}
	TSharedRef<SWidget> Attempts = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			AutopilotUI::Label(LOCTEXT("AttemptsSoFar", "Harness attempts"))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Squares
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
		[
			SAssignNew(AttemptCount, SBox)
			[
				AutopilotUI::Mono(TAttribute<FText>::CreateLambda([this]()
				{
					const TSharedPtr<FAutopilotRun> Run = Selected();
					return Str(FString::Printf(TEXT("%d"), Run.IsValid() ? Run->Attempt : 0));
				}), 13.f, FSlateColor(AutopilotColor::Text()), true)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).HAlign(HAlign_Right).Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Text_Lambda([this]()
			{
				const TSharedPtr<FAutopilotRun> Run = Selected();
				const int32 N = Run.IsValid() ? Run->StepCount : 0;
				return N > 0 ? Str(FString::Printf(TEXT("%d %s"), N, N == 1 ? TEXT("policy step") : TEXT("policy steps"))) : FText::GetEmpty();
			})
		];

	// The feed: a plate, its name over the lines
	TSharedRef<SWidget> Feed = AutopilotUI::Plate(FMargin(0.f),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(11.f, 9.f)
		[
			AutopilotUI::Body(LOCTEXT("FeedLabel", "Feed"), 12.f, AutopilotColor::Text(), false, EAutopilotFont::BodyMedium)
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(11.f, 4.f, 11.f, 8.f)
		[
			MakeFeedList(LiveFeedList, &LiveFeedItems, true)
		]);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[Head]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(16.f, 18.f, 16.f, 18.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[MakeStageTrack()]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)[NowCard]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)[Attempts]
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(0.f, 16.f, 0.f, 0.f)[Feed]
		]
		+ SVerticalBox::Slot().AutoHeight()[MakeSteerBar(false)];
}

TSharedRef<SWidget> SPlaytestPanel::MakeSteerBar(bool bFeed)
{
	// Steer: the user's words to the run while it plays (it goes on from where the game is). Shown while the run goes, on
	// the Live page and under the Feed tab (one box: what is typed in one shows in the other). Words can be typed at any
	// stage and are kept; Steer only at the Rollout; both locked while it stops
	TSharedRef<SButton> Steer = AutopilotUI::Button(LOCTEXT("Steer", "Steer"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSteer),
		TAttribute<bool>::CreateLambda([this]()
		{
			return IsSteerOpen() && !IsStopping() && !SteerText().TrimStartAndEnd().IsEmpty();
		}));
	Steer->SetToolTip(AutopilotUI::Tip(TAttribute<FText>::CreateLambda([this]()
	{
		return IsSteerOpen() || IsStopping() ? FText::GetEmpty() : LOCTEXT("SteerLater", "You can steer once the game is playing");
	})));
	(bFeed ? FeedSteerButton : SteerButton) = Steer;
	TSharedPtr<SMultiLineEditableTextBox>& Box = bFeed ? FeedSteerBox : SteerBox;
	return SNew(SVerticalBox)
		.Visibility_Lambda([this]() { return IsSteerShown() ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight().Padding(TAttribute<FMargin>::CreateLambda([this]() { return bNarrow ? FMargin(12.f, 10.f, 12.f, 12.f) : FMargin(16.f, 10.f, 16.f, 12.f); }))
		[
			// (at 55% while it can't be used: stopping)
			SNew(SBorder)
			.BorderImage(FAppStyle::GetNoBrush())
			.Padding(0.f)
			.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.f, 1.f, 1.f, IsStopping() ? 0.55f : 1.f); })
			[
			AutopilotUI::Box(AutopilotColor::Well(), AutopilotColor::Line(), AutopilotRadius::Plate, FMargin(12.f, 6.f, 6.f, 6.f),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					// (one line, 28 tall; two when narrow, where the hint wraps)
					SNew(SBox).MinDesiredHeight_Lambda([this]() { return FOptionalSize(IsCompact() ? 2.f * 13.f * 1.55f : AutopilotUI::ButtonHeight); })
					[
						SAssignNew(Box, SMultiLineEditableTextBox)
						.Style(&FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Bare"))
						.HintText(LOCTEXT("SteerHint", "Send a steering prompt to Autopilot"))
						.IsEnabled_Lambda([this]() { return !IsStopping(); })
						.AutoWrapText(true)
						.OnKeyDownHandler(this, &SPlaytestPanel::OnSteerKey)
						.OnTextChanged_Lambda([this, bFeed](const FText& Text)
						{
							// (the other page's box says the same)
							const TSharedPtr<SMultiLineEditableTextBox>& Other = bFeed ? SteerBox : FeedSteerBox;
							if (Other.IsValid() && !Other->GetText().EqualTo(Text))
							{
								Other->SetText(Text);
							}
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Steer
				])
			]
		];
}

FString SPlaytestPanel::SteerText() const
{
	if (SteerBox.IsValid() && !SteerBox->GetText().IsEmpty())
	{
		return SteerBox->GetText().ToString();
	}
	return FeedSteerBox.IsValid() ? FeedSteerBox->GetText().ToString() : FString();
}

TSharedRef<SWidget> SPlaytestPanel::MakeFeedList(TSharedPtr<SListView<TSharedPtr<FAutopilotFeedLine>>>& View, TArray<TSharedPtr<FAutopilotFeedLine>>* Source, bool bCompact)
{
	SAssignNew(View, SListView<TSharedPtr<FAutopilotFeedLine>>)
		.ListItemsSource(Source ? Source : &FeedItems)
		.OnGenerateRow(this, bCompact ? &SPlaytestPanel::MakeLiveFeedRow : &SPlaytestPanel::MakeFeedRow)
		.SelectionMode(ESelectionMode::None);
	return View.ToSharedRef();
}

void SPlaytestPanel::GroupFeed(const TArray<TSharedPtr<FAutopilotFeedLine>>& Feed, TArray<TSharedPtr<FAutopilotFeedLine>>& Out)
{
	// A heading before each stretch of one stage's lines (the stage's name, how many lines follow it): a stage that comes
	// again (Play, then Results, then Play for the next harness) gets a heading each time. Lines before any stage come
	// first, under none
	EAutopilotStage Current = EAutopilotStage::None;
	TSharedPtr<FAutopilotFeedLine> Heading;
	for (const TSharedPtr<FAutopilotFeedLine>& Line : Feed)
	{
		if (!Line.IsValid())
		{
			continue;
		}
		if (Line->Stage != EAutopilotStage::None && Line->Stage != Current)
		{
			Current = Line->Stage;
			Heading = MakeShared<FAutopilotFeedLine>();
			Heading->Kind = EAutopilotFeedKind::Heading;
			Heading->Stage = Current;
			Heading->Text = FAutopilotSession::StageHeading(Current);
			Heading->Elapsed = Line->Elapsed;
			Out.Add(Heading);
		}
		if (Heading.IsValid())
		{
			++Heading->Attempt;  // (the heading's count)
		}
		Out.Add(Line);
	}
}

int64 SPlaytestPanel::FoldKey(const FAutopilotFeedLine& Line, bool bResult) const
{
	return ((int64)FeedRunId << 32) | ((int64)Line.ActivityId << 1) | (bResult ? 1 : 0);
}

TSharedRef<SWidget> SPlaytestPanel::MakeActivityWords(const TSharedPtr<FAutopilotFeedLine>& Line)
{
	// The event's words, then, when a call came back, its result's after two spaces, quieter, wrapping as one text. A
	// short, one-line detail that the words don't say (a file's path) is shown the same way instead of folded
	FString Words = Markup(Line->Text);
	const bool bInlineDetail = !Line->Detail.IsEmpty() && !FAutopilotFeedLine::Folds(Line->Detail) && !Line->Text.Contains(Line->Detail);
	if (bInlineDetail)
	{
		Words += TEXT("  <Autopilot.Feed.Quiet>") + Markup(Line->Detail) + TEXT("</>");
	}
	if (!Line->Result.IsEmpty())
	{
		Words += TEXT("  <Autopilot.Feed.Quiet>") + Markup(Line->Result) + TEXT("</>");
	}
	return SNew(SRichTextBlock)
		.Text(Str(Words))
		.TextStyle(&FAutopilotStyle::Get().GetWidgetStyle<FTextBlockStyle>("Autopilot.Feed.Text"))
		.DecoratorStyleSet(&FAutopilotStyle::Get())
		.AutoWrapText(true);
}

TSharedRef<SWidget> SPlaytestPanel::MakeActivityIcon(const FAutopilotFeedLine& Line, const FLinearColor& Color)
{
	if (const FSlateBrush* Brush = FAutopilotStyle::ActivityIcon(Line.Icon()))
	{
		return SNew(SImage)
			.Image(Brush)
			.ColorAndOpacity(FSlateColor(Color))
			.DesiredSizeOverride(FVector2D(12.0, 12.0));
	}
	return AutopilotUI::Mono(Str(Line.Glyph()), 11.5f, FSlateColor(Color), true);
}

TSharedRef<SWidget> SPlaytestPanel::MakeFold(const TSharedPtr<FAutopilotFeedLine>& Line, const FString& Label, const FString& Detail, bool bResult)
{
	// "+ inspect the code" (mono 10.5, muted; "– hide the code" while open); under it, open, the detail in mono on the
	// well, at most FoldHeight tall (then it scrolls), the text selectable
	const int64 Key = FoldKey(*Line, bResult);
	auto IsOpen = [this, Key]() { return OpenFolds.Contains(Key); };
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(SBox).HAlign(HAlign_Left)
			[
				SNew(SButton)
				.ButtonStyle(&FAutopilotStyle::Get().GetWidgetStyle<FButtonStyle>("Autopilot.Button.Bare"))
				.ContentPadding(FMargin(0.f, 1.f))
				.OnClicked_Lambda([this, Key]()
				{
					if (OpenFolds.Contains(Key))
					{
						OpenFolds.Remove(Key);
					}
					else
					{
						OpenFolds.Add(Key);
					}
					if (FeedList.IsValid())
					{
						FeedList->RequestListRefresh();  // (the row's height changed)
					}
					return FReply::Handled();
				})
				[
					AutopilotUI::Mono(TAttribute<FText>::CreateLambda([IsOpen, Label]()
					{
						return Str(FAutopilotFeedLine::FoldWords(Label, IsOpen()));
					}), 10.5f, FSlateColor(AutopilotColor::Muted()))
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 2.f)
		[
			SNew(SBox)
			.Visibility_Lambda([IsOpen]() { return IsOpen() ? EVisibility::Visible : EVisibility::Collapsed; })
			.MaxDesiredHeight(FoldHeight)
			[
				AutopilotUI::Box(AutopilotColor::Well(), AutopilotColor::Line(), 4.f, FMargin(10.f, 8.f),
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SMultiLineEditableText)
						.Text(Str(Detail))
						.IsReadOnly(true)
						.AutoWrapText(true)
						.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f))
						.TextStyle(&FAutopilotStyle::Get().GetWidgetStyle<FTextBlockStyle>("Autopilot.Feed.Text"))
					])
			]
		];
}

TSharedRef<ITableRow> SPlaytestPanel::MakeFeedRow(TSharedPtr<FAutopilotFeedLine> Line, const TSharedRef<STableViewBase>& Owner)
{
	return FeedRow(Line, Owner, false);
}

TSharedRef<ITableRow> SPlaytestPanel::MakeLiveFeedRow(TSharedPtr<FAutopilotFeedLine> Line, const TSharedRef<STableViewBase>& Owner)
{
	return FeedRow(Line, Owner, true);
}

TSharedRef<ITableRow> SPlaytestPanel::FeedRow(TSharedPtr<FAutopilotFeedLine> Line, const TSharedRef<STableViewBase>& Owner, bool bCompact)
{
	if (Line->Kind == EAutopilotFeedKind::Heading)
	{
		// A stage's heading (the Feed tab's): its name, mono 10 upper case 0.1em, and its count of lines, muted
		return SNew(STableRow<TSharedPtr<FAutopilotFeedLine>>, Owner)
		.Style(&FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row.Plain"))
		.Padding(FMargin(0.f, 14.f, 0.f, 4.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				AutopilotUI::Label(Str(Line->Text), FSlateColor(AutopilotColor::Text2()))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				AutopilotUI::Mono(Str(FString::Printf(TEXT("%d"), Line->Attempt)), 10.f, FSlateColor(AutopilotColor::Muted()))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				AutopilotUI::Rule()
			]
		];
	}
	if (Line->IsActivity())
	{
		// An activity event: its time, its kind's icon (12 px in the kind's color, its title as the tooltip), its words and
		// its result (MakeActivityWords); on the Feed tab, a fold under them for each detail (the call's, the result's)
		const FString Kind = Line->ActivityKind;
		const FLinearColor MarkColor = Kind == TEXT("alert") ? AutopilotColor::Red() : Kind == TEXT("report") ? AutopilotColor::Success()
			: Kind == TEXT("probe") || Kind == TEXT("episode") ? AutopilotColor::Accent() : Kind == TEXT("write") || Kind == TEXT("refine") ? AutopilotColor::Info()
			: Kind == TEXT("look") ? AutopilotColor::Violet() : Kind == TEXT("restart") ? AutopilotColor::Amber()
			: Kind == TEXT("text") || Kind == TEXT("steer") ? AutopilotColor::Text2() : AutopilotColor::Muted();
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[MakeActivityWords(Line)];
		if (!bCompact)
		{
			const FString DetailLabel = Line->DetailLabel();
			if (!DetailLabel.IsEmpty())
			{
				Body->AddSlot().AutoHeight()[MakeFold(Line, DetailLabel, Line->Detail, false)];
			}
			const FString ResultLabel = Line->ResultLabel();
			if (!ResultLabel.IsEmpty())
			{
				Body->AddSlot().AutoHeight()[MakeFold(Line, ResultLabel, Line->ResultDetail, true)];
			}
		}
		return SNew(STableRow<TSharedPtr<FAutopilotFeedLine>>, Owner)
		.Style(&FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row.Plain"))
		.Padding(FMargin(0.f, 5.f, 0.f, 5.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 12.f, 0.f)
			[
				SNew(SBox).WidthOverride(36.f)
				[
					AutopilotUI::Mono(Str(FAutopilotSession::ClockShort(Line->Elapsed)), 11.5f, FSlateColor(AutopilotColor::Muted()))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 1.f, 8.f, 0.f)
			[
				SNew(SBox).WidthOverride(12.f).HeightOverride(14.f).VAlign(VAlign_Center).HAlign(HAlign_Center)
				.ToolTipText(Str(Line->Title()))
				[
					MakeActivityIcon(*Line, MarkColor)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
			[
				Body
			]
		];
	}
	// A line (mono 11.5): its time (36 wide, muted), its tag (a finding's, the memory line's), its words: the plugin's own
	// lines #a4a8b0, the stages and the result in the text's color, a harness in the text's color, 500 (the current one in
	// the accent), "Now: ..." and findings in the reading color, the user's steer ("You: ...") 500, the memory line green
	TAttribute<FSlateColor> TextColor = FSlateColor(AutopilotColor::Text());
	EAutopilotFont Face = EAutopilotFont::Mono;
	FString TagText;
	EAutopilotTone Tone = EAutopilotTone::Neutral;
	FString Text = Line->Text;
	if (Text.EndsWith(TEXT("...")))
	{
		Text = Text.LeftChop(3) + TEXT("…");  // (the plugin's "Stopping...": as the design writes it)
	}
	TWeakPtr<FAutopilotFeedLine> Weak = Line;
	switch (Line->Kind)
	{
	case EAutopilotFeedKind::Attempt:
		Face = EAutopilotFont::MonoBold;
		TextColor = TAttribute<FSlateColor>::CreateLambda([this, Weak]()
		{
			const TSharedPtr<FAutopilotFeedLine> L = Weak.Pin();
			const TSharedPtr<FAutopilotRun> Run = Selected();
			const bool bCurrent = L.IsValid() && Run.IsValid() && !Run->bDone && L->Attempt == Run->Attempt;
			return FSlateColor(bCurrent ? AutopilotColor::Accent() : AutopilotColor::Text());
		});
		break;
	case EAutopilotFeedKind::Now:
		TextColor = FSlateColor(AutopilotColor::Text2());
		break;
	case EAutopilotFeedKind::Finding:
		TextColor = FSlateColor(AutopilotColor::Text2());
		TagText = Line->Finding.IsValid() && !Line->Finding->IsEngine() ? TEXT("Agent") : TEXT("Engine");
		// (the agent's: always red; the engine's: by severity)
		Tone = Line->Finding.IsValid() && !Line->Finding->IsEngine() ? EAutopilotTone::Red
			: Line->Finding.IsValid() ? AutopilotUI::SeverityTone(Line->Finding->Severity) : EAutopilotTone::Info;
		break;
	case EAutopilotFeedKind::Notice:
		if (Text.StartsWith(TEXT("You: ")))
		{
			Face = EAutopilotFont::MonoBold;
		}
		else if (Text.StartsWith(TEXT("Connected again")))
		{
			// (a stage line: the text's color)
		}
		else
		{
			TextColor = FSlateColor(AutopilotColor::Text3());
		}
		break;
	case EAutopilotFeedKind::Memory:
		TextColor = FSlateColor(AutopilotColor::Success());
		TagText = TEXT("Memory");
		Tone = EAutopilotTone::Success;
		break;
	default:  // (a stage, the result)
		break;
	}
	return SNew(STableRow<TSharedPtr<FAutopilotFeedLine>>, Owner)
	.Style(&FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row.Plain"))
	.Padding(FMargin(0.f, Line->Kind == EAutopilotFeedKind::Memory ? 9.f : 5.f, 0.f, 5.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(36.f)
			[
				AutopilotUI::Mono(Str(FAutopilotSession::ClockShort(Line->Elapsed)), 11.5f, FSlateColor(AutopilotColor::Muted()))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox)
			.Visibility(TagText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			[
				AutopilotUI::Tag(Str(TagText), Tone)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
		[
			SNew(STextBlock)
			.Text(Str(Text))
			.Font(FAutopilotStyle::Font(Face, 11.5f))
			.ColorAndOpacity(TextColor)
			.AutoWrapText(true)
		]
	];
}
FReply SPlaytestPanel::OnStop()
{
	FAutopilotSession::Get().StopRun();
	return FReply::Handled();
}

bool SPlaytestPanel::IsStopping() const
{
	// (Stop pressed: the agent service's "Stopping..." came, and the run goes on until its result)
	const TSharedPtr<FAutopilotRun> Run = Selected();
	return Run.IsValid() && !Run->bDone && Run->Feed.ContainsByPredicate([](const TSharedPtr<FAutopilotFeedLine>& L) { return L->Text == TEXT("Stopping…"); });
}

bool SPlaytestPanel::IsSteerShown() const
{
	const TSharedPtr<FAutopilotRun> Run = Selected();
	return Run.IsValid() && !Run->bDone && (TestSteers.IsSet() ? TestSteers.GetValue() : FAutopilotSession::Get().CanSteer());
}

bool SPlaytestPanel::IsSteerOpen() const
{
	return IsSteerShown() && FAutopilotSession::SteersAt(*Selected());
}

FReply SPlaytestPanel::OnSteer()
{
	// (outside the Rollout nothing is sent and the words stay for the next one: SteerRun refuses too)
	if (IsSteerOpen() && !IsStopping() && FAutopilotSession::Get().SteerRun(SteerText()))
	{
		for (const TSharedPtr<SMultiLineEditableTextBox>& Box : { SteerBox, FeedSteerBox })
		{
			if (Box.IsValid())
			{
				Box->SetText(FText::GetEmpty());
			}
		}
	}
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSteerKey(const FGeometry&, const FKeyEvent& Key)
{
	// Enter sends; Shift+Enter is a new line
	if (Key.GetKey() == EKeys::Enter && !Key.IsShiftDown())
	{
		return OnSteer();
	}
	return FReply::Unhandled();
}

// -- A run done --------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeVerdictBanner(const TSharedPtr<FAutopilotRun>& Run)
{
	// The word on a solid block of the verdict's color (16px, 600, upper case, ink)
	return AutopilotUI::Fill(FSlateColor(Run->VerdictColor()), FMargin(10.f, 2.f),
		SNew(STextBlock)
		.Text(Str(Run->Verdict))
		.Font(FAutopilotStyle::Font(EAutopilotFont::BodySemi, 16.f, 40))
		.ColorAndOpacity(FSlateColor(AutopilotColor::OnAccent()))
		.TransformPolicy(ETextTransformPolicy::ToUpper));
}

namespace
{
	/** A code box: the well, a rule round it, 8px corners, mono lines (11.5px, reading color) 4 apart. */
	TSharedRef<SWidget> CodeBox(const TArray<FString>& Lines, const TCHAR* Mark = nullptr, bool bTimes = false)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		for (const FString& Whole : Lines)
		{
			FString Line = Whole;
			TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
			// (bTimes: a line that starts with its harness and time, "H1 12.4 s  picked up orb 1", shows them in the accent)
			FString At;
			int32 Gap = INDEX_NONE;
			if (bTimes && Line.Len() > 3 && Line[0] == TEXT('H') && FChar::IsDigit(Line[1]) && Line.FindChar(TEXT(' '), Gap))
			{
				const int32 Split = Line.Find(TEXT("  "));
				if (Split > Gap)
				{
					At = Line.Left(Split);
					Line = Line.Mid(Split).TrimStart();
				}
			}
			if (!At.IsEmpty())
			{
				Row->AddSlot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 12.f, 0.f)
				[
					AutopilotUI::Mono(Str(At), 11.5f, FSlateColor(AutopilotColor::Accent()))
				];
			}
			if (Mark)
			{
				Row->AddSlot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 8.f, 0.f)
				[
					AutopilotUI::Mono(Str(Mark), 11.5f, FSlateColor(AutopilotColor::Amber()), true)
				];
			}
			Row->AddSlot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Text(Str(Line))
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.5f))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Text2()))
				.AutoWrapText(true)
			];
			Box->AddSlot().AutoHeight().Padding(0.f, Box->NumSlots() ? 4.f : 0.f, 0.f, 0.f)[Row];
		}
		return AutopilotUI::Box(AutopilotColor::Well(), AutopilotColor::Rule(), AutopilotRadius::Plate, FMargin(11.f, 9.f), Box);
	}
}

TSharedRef<SWidget> SPlaytestPanel::MakeAgentCard(const TSharedPtr<FAutopilotRun>& Run, const FString& Note, const TSharedPtr<FAutopilotFinding>& Finding)
{
	// What the agent found exploring: Found, "Agent finding", its words (500), Verify a fix and Copy
	const FString Text = Finding.IsValid() ? Finding->Summary : Note;
	TSharedRef<SWrapBox> Buttons = ButtonRow();
	AddTo(Buttons, SmallButton(LOCTEXT("VerifyFix", "Verify a fix"), "Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestPanel::OnVerifyFix, Text)));
	AddTo(Buttons, SmallButton(LOCTEXT("Copy", "Copy"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnCopy, Text)));
	return AutopilotUI::Plate(FMargin(12.f, 11.f),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				AutopilotUI::Tag(LOCTEXT("FoundTag", "Found"), EAutopilotTone::Red)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				AutopilotUI::Label(LOCTEXT("AgentFinding", "Agent finding"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			AutopilotUI::Body(Str(Text), 13.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			Buttons
		]);
}

TSharedRef<SWidget> SPlaytestPanel::MakeEngineCard(const TSharedPtr<FAutopilotRun>& Run, const TSharedPtr<FAutopilotFinding>& Finding)
{
	// A finding with facts (the engine's detectors, or one the agent service sent): its severity and detector, its words
	// (500), where and when (mono 10, muted), Show in level and Not a bug
	TArray<FString> Facts;
	if (Finding->bHasWhere)
	{
		Facts.Add(FString::Printf(TEXT("at %.0f, %.0f, %.0f cm"), Finding->Where.X, Finding->Where.Y, Finding->Where.Z));
	}
	if (!Finding->Actor.IsEmpty())
	{
		Facts.Add(Finding->Actor);
	}
	if (Finding->GameTime >= 0)
	{
		Facts.Add(FString::Printf(TEXT("t=%.1fs"), Finding->GameTime));
	}
	TWeakPtr<FAutopilotFinding> Weak = Finding;
	TSharedRef<SWidget> Kind = Finding->IsEngine()
		? StaticCastSharedRef<SWidget>(AutopilotUI::Mono(Str(Finding->Detector), 10.f, FSlateColor(AutopilotColor::Muted())))
		: StaticCastSharedRef<SWidget>(AutopilotUI::Label(LOCTEXT("AgentFinding", "Agent finding")));
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				AutopilotUI::Tag(Str(SeverityWord(Finding->Severity)), AutopilotUI::SeverityTone(Finding->Severity))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				Kind
			]
			// (how many times it happened)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				AutopilotUI::Mono(Finding->Count > 1 ? Str(FString::Printf(TEXT("×%d"), Finding->Count)) : FText::GetEmpty(), 12.f, FSlateColor(AutopilotColor::Text()), true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(Str(Finding->Summary))
			.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 13.f))
			.LineHeightPercentage(AutopilotUI::BodyLineHeight)
			.AutoWrapText(true)
			.ColorAndOpacity_Lambda([Weak]()
			{
				const TSharedPtr<FAutopilotFinding> F = Weak.Pin();
				return FSlateColor(F.IsValid() && F->bDismissed ? AutopilotColor::Muted() : AutopilotColor::Text());
			})
		];
	if (Facts.Num())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(Str(FString::Join(Facts, TEXT(" · "))))
			.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
			.AutoWrapText(true)
		];
	}
	Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)[MakeFindingButtons(Run, Finding)];
	return AutopilotUI::Plate(FMargin(12.f, 11.f), Body);
}

TSharedRef<SWidget> SPlaytestPanel::MakeFindingButtons(const TSharedPtr<FAutopilotRun>& Run, const TSharedPtr<FAutopilotFinding>& Finding)
{
	TWeakPtr<FAutopilotFinding> Weak = Finding;
	TSharedRef<SWrapBox> Row = ButtonRow();
	if (Finding->bHasWhere)
	{
		AddTo(Row, SmallButton(LOCTEXT("ShowInLevel", "Show in level"), "Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestPanel::OnShowInLevel, Finding)));
	}
	// Not a bug; once marked, says so (12px, muted) in its place
	AddTo(Row, SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([Weak]() { const TSharedPtr<FAutopilotFinding> F = Weak.Pin(); return F.IsValid() && F->bDismissed ? 1 : 0; })
		+ SWidgetSwitcher::Slot()
		[
			// (not connected: greyed, and pressed it says why on the status line)
			AutopilotUI::Greyed(TAttribute<bool>::CreateLambda([]() { return !(UPlaytestSubsystem::Get() && UPlaytestSubsystem::Get()->IsConnected()); }),
				AutopilotUI::Button(LOCTEXT("NotABug", "Not a bug"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([Run, Finding]()
				{
					FAutopilotSession::Get().MarkNotABug(Run, Finding);
					return FReply::Handled();
				}), TAttribute<bool>::CreateLambda([Weak]()
				{
					const TSharedPtr<FAutopilotFinding> F = Weak.Pin();
					return F.IsValid() && !F->bDismissed;
				}), nullptr, true))
		]
		+ SWidgetSwitcher::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::Body(LOCTEXT("MarkedNotABug", "Marked not a bug"), 12.f, AutopilotColor::Muted(), false)
		]);
	return Row;
}


void SPlaytestPanel::RebuildResult(TSharedPtr<FAutopilotRun> Shown)
{
	const TSharedPtr<FAutopilotRun> Run = Shown.IsValid() ? Shown : Selected();
	ResultRunId = Run.IsValid() ? Run->Id : INDEX_NONE;
	ResultRevision = Run.IsValid() ? Run->Revision : -1;
	// (made again, the result before shown until this one is laid out: no flash)
	SAssignNew(ResultBox, SVerticalBox);
	if (ResultSettle.IsValid())
	{
		ResultSettle->SetContent(ResultBox.ToSharedRef());
	}
	if (!Run.IsValid())
	{
		Video.Reset();
		return;
	}
	constexpr float Gap = 14.f;  // (between the result's blocks)
	// The run's id line, and what to do with the run at its right: Run again (filled), Copy report and Run folder
	// (outlined): icons, named by their tooltips, one row at any width
	TSharedRef<SHorizontalBox> IdRow = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(STextBlock)
			.Text(Str(Run->IdLine()))
			.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f, 60))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
			.TransformPolicy(ETextTransformPolicy::ToUpper)
			.AutoWrapText(true)
		];
	auto AddIcon = [&IdRow](const FName& Icon, const FText& Name, const FName& Style, FOnClicked OnClicked, const TAttribute<bool>& Enabled,
		const TAttribute<bool>& bGreyed = false)
	{
		IdRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(IdRow->NumSlots() > 1 ? 6.f : 0.f, 0.f, 0.f, 0.f)
		[
			AutopilotUI::Greyed(bGreyed, AutopilotUI::IconButton(FAutopilotStyle::Brush(Icon), Name, Style, OnClicked, Enabled))
		];
	};
	// (not connected or a run going: greyed, and pressed it says why on the status line)
	AddIcon("Autopilot.Retry", LOCTEXT("RunAgain", "Run again"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnRunAgain, Run),
		Run->Frame.IsValid(), TAttribute<bool>::CreateLambda([this]() { return !IsConnected() || IsRunning(); }));
	AddIcon("Autopilot.Clipboard", LOCTEXT("CopyReport", "Copy report"), "Autopilot.Button", FOnClicked::CreateLambda([this, Run]()
	{
		FPlatformApplicationMisc::ClipboardCopy(*Run->ReportMarkdown());
		FAutopilotSession::Get().Say(TEXT("Copied the report"));
		return FReply::Handled();
	}), true);
	AddIcon("Autopilot.Folder", LOCTEXT("RunFolder", "Run folder"), "Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestPanel::OnRunFolder, Run),
		TAttribute<bool>::CreateLambda([Run]() { return !Run->ReportDir.IsEmpty() && FPaths::DirectoryExists(Run->ReportDir); }));
	ResultBox->AddSlot().AutoHeight()[IdRow];
	ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
	[
		AutopilotUI::Body(Str(PromptOf(*Run)), 15.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
	];
	// (where it was started, when not here: the user's own coding agent)
	if (!Run->OriginLine().IsEmpty())
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[
			AutopilotUI::Body(Str(Run->OriginLine()), 12.f, AutopilotColor::Text3())
		];
	}
	// What the user must know first (the result's alerts: the game's picture couldn't be looked at...), each as said
	for (const FString& Alert : Run->Alerts)
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			AutopilotUI::Alert(EAutopilotTone::Amber, AutopilotUI::Body(Str(Alert), 13.f, AutopilotColor::AmberText(), true, EAutopilotFont::BodyMedium))
		];
	}
	// The verdict, and why under it (one left edge: a long reason has the page's width)
	FString Why = Run->Reason;
	if (Why.IsEmpty() && Run->Verdict == TEXT("STOPPED"))
	{
		Why = TEXT("Stopped before a result.");
	}
	else if (Why.IsEmpty() && Run->Verdict == TEXT("NO RESULT"))
	{
		Why = TEXT("The connection to the agent service closed before the result came.");
	}
	ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f).HAlign(HAlign_Left)[MakeVerdictBanner(Run)];
	if (!Why.IsEmpty())
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)[AutopilotUI::Body(Str(Why), 13.f, AutopilotColor::Text2())];
	}
	// The run's recording (none for runs without one: nothing shown); the same player is kept while it is the one shown
	if (!Run->Recording.IsEmpty())
	{
		const bool bGone = !FPaths::FileExists(Run->Recording);
		if (!Video.IsValid() || Video->GetPath() != Run->Recording || bGone)
		{
			Video = SNew(SPlaytestVideo).Path(Run->Recording).Seconds(Run->RecordingSeconds).Open(bOpenVideos && !bGone).Gone(bGone);
		}
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)[Video.ToSharedRef()];
	}
	else
	{
		Video.Reset();
		// Why there is none, when the result said; never while the run goes on
		if (Run->bDone && !Run->NoRecording.IsEmpty())
		{
			ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
			[
				AutopilotUI::Body(FText::Format(LOCTEXT("NoRecording", "No recording: {0}"), Str(Run->NoRecording)), 12.f, AutopilotColor::Muted())
			];
		}
	}
	// The evidence
	if (Run->Evidence.Num())
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 6.f)[AutopilotUI::Label(LOCTEXT("Evidence", "Evidence"))];
		ResultBox->AddSlot().AutoHeight()[CodeBox(Run->Evidence, nullptr, true)];
	}
	// What the agent changed in the game directly (a verification's setup): the result was played on a changed game
	if (Run->Changes.Num())
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 6.f)
		[
			AutopilotUI::Label(bNarrow ? FText::Format(LOCTEXT("ChangesShort", "Changed directly · {0}"), Run->Changes.Num())
				: FText::Format(LOCTEXT("Changes", "Changed directly · not through play · {0}"), Run->Changes.Num()), FSlateColor(AutopilotColor::Amber()))
		];
		ResultBox->AddSlot().AutoHeight()[CodeBox(Run->Changes, TEXT("!"))];
	}
	// The findings: the agent's, then the engine's under their own label (none: nothing at all)
	TArray<TSharedPtr<FAutopilotFinding>> AgentFindings;
	TArray<TSharedPtr<FAutopilotFinding>> EngineFindings;
	for (const TSharedPtr<FAutopilotFinding>& F : Run->Findings)
	{
		(F->IsEngine() ? EngineFindings : AgentFindings).Add(F);
	}
	bool bFirst = true;
	auto Next = [&bFirst, Gap]() { const float Top = bFirst ? Gap : 8.f; bFirst = false; return Top; };
	for (const FString& Note : Run->AgentNotes)
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Next(), 0.f, 0.f)[MakeAgentCard(Run, Note, nullptr)];
	}
	for (const TSharedPtr<FAutopilotFinding>& F : AgentFindings)
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Next(), 0.f, 0.f)[MakeEngineCard(Run, F)];
	}
	if (EngineFindings.Num())
	{
		ResultBox->AddSlot().AutoHeight().Padding(0.f, Next(), 0.f, 0.f)
		[
			AutopilotUI::Label(FText::Format(LOCTEXT("EngineFindings", "Engine findings · {0}"), EngineFindings.Num()))
		];
		for (const TSharedPtr<FAutopilotFinding>& F : EngineFindings)
		{
			ResultBox->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)[MakeEngineCard(Run, F)];
		}
	}
}
FReply SPlaytestPanel::OnRunAgain(TSharedPtr<FAutopilotRun> Run)
{
	if (!Run.IsValid() || !Run->Frame.IsValid())
	{
		return FReply::Handled();
	}
	if (IsRunning())
	{
		FAutopilotSession::Get().Say(TEXT("A run is going: wait for its result, or stop it"));
		return FReply::Handled();
	}
	if (!IsConnected())
	{
		FAutopilotSession::Get().Say(TEXT("Not connected: connect first"));
		return FReply::Handled();
	}
	TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->Values = Run->Frame->Values;
	// (it keeps the Time limit it was started with)
	FString FrameKind;
	Frame->TryGetStringField(TEXT("kind"), FrameKind);
	// (one the decision model read the kind of is read again: blank until it is)
	if (TSharedPtr<FAutopilotRun> Again = FAutopilotSession::Get().StartRun(FrameKind == TEXT("auto") ? FString() : Run->Kind, Run->Prompt, Frame))
	{
		SyncRuns();
		Select(Again);
	}
	return FReply::Handled();
}

FReply SPlaytestPanel::OnDeleteSave()
{
	if (!IsConnected())
	{
		FAutopilotSession::Get().Say(TEXT("Not connected: connect first to reset the memory"));
		return FReply::Handled();
	}
	if (IsRunning())
	{
		FAutopilotSession::Get().Say(TEXT("A run is going: reset the memory once it is done"));
		return FReply::Handled();
	}
	bConfirmReset = true;  // (asked in place: Reset memory, Cancel)
	return FReply::Handled();
}

FReply SPlaytestPanel::OnShowInLevel(TSharedPtr<FAutopilotFinding> Finding)
{
	if (!Finding.IsValid() || !Finding->bHasWhere)
	{
		return FReply::Handled();
	}
	const FVector Target = Finding->Where;
	const FVector From = Target + FVector(-500.0, 0.0, 300.0);
	bool bShown = false;
	for (FLevelEditorViewportClient* Client : GEditor ? GEditor->GetLevelViewportClients() : TArray<FLevelEditorViewportClient*>())
	{
		if (Client && Client->IsPerspective())
		{
			Client->SetViewLocation(From);
			Client->SetViewRotation((Target - From).Rotation());
			Client->Invalidate();
			bShown = true;
		}
	}
	FAutopilotSession::Get().Say(!bShown ? FString(TEXT("Open the level to show this finding in it"))
		: Finding->Actor.IsEmpty() ? FString(TEXT("Showing the place in the open level")) : FString::Printf(TEXT("Showing %s"), *Finding->Actor));
	return FReply::Handled();
}

FReply SPlaytestPanel::OnVerifyFix(FString Note)
{
	// A new run that checks the bug is gone: its words start with Verify, so it runs as one
	Select(nullptr);
	if (PromptBox.IsValid())
	{
		PromptBox->SetText(FText::FromString(FString::Printf(TEXT("Verify that this is fixed: %s"), *Note)));
	}
	return FReply::Handled();
}

FReply SPlaytestPanel::OnCopy(FString Text)
{
	if (TestCopied)
	{
		TestCopied(Text);  // (the tests': the user's clipboard is left alone)
		return FReply::Handled();
	}
	FPlatformApplicationMisc::ClipboardCopy(*Text);
	FAutopilotSession::Get().Say(TEXT("Copied"));
	return FReply::Handled();
}

FReply SPlaytestPanel::OnRunFolder(TSharedPtr<FAutopilotRun> Run)
{
	if (Run.IsValid() && !Run->ReportDir.IsEmpty() && FPaths::DirectoryExists(Run->ReportDir))
	{
		FPlatformProcess::ExploreFolder(*Run->ReportDir);
	}
	return FReply::Handled();
}

// -- Usage -------------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeSettings()
{
	return SNew(SScrollBox)
	+ SScrollBox::Slot()
	[
		Capped(SNew(SVerticalBox)
			// The account first: how runs go, the agent service, Rerun setup, Sign out
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(AccountBox, AutopilotUI::SSettle)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(SettingsSettle, AutopilotUI::SSettle)
			],
			SettingsMax, FMargin(20.f, 18.f))
	];
}

namespace
{
	constexpr float UsageWhenWidth = 96.f;
	constexpr float UsageBillWidth = 90.f;

	/** A setting's plate on the Settings page (Time limit, sharing, Claude Code & Codex): its name (the mono
	 * micro-label), what it does (12, Text3: as the sharing text), then what changes it, the same gaps in each. */
	TSharedRef<SWidget> SettingCard(const FText& Title, const FText& Subtitle, const TSharedRef<SWidget>& Control)
	{
		return Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				AutopilotUI::Label(Title)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				AutopilotUI::Body(Subtitle, 12.f, AutopilotColor::Text3())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				Control
			]);
	}
}

TSharedRef<SWidget> SPlaytestPanel::MakeUsageRunRow(const TSharedPtr<FAutopilotRun>& Run)
{
	// A run of this project: when (mono 11), its prompt (12.5), what Aura billed for it (mono 11, at the right; "—" for
	// none). A plain row: nothing to click, no hover, no picked state
	return SNew(SBox)
		.Padding(FMargin(8.f, 7.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SBox).WidthOverride(UsageWhenWidth)
				[
					AutopilotUI::Mono(Str(FAutopilotSession::When(Run->Started)), 11.f, FSlateColor(AutopilotColor::Text3()))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Str(PromptOf(*Run)))
				.ToolTipText(Str(PromptOf(*Run)))
				.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.5f))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SBox).WidthOverride(UsageBillWidth).HAlign(HAlign_Right)
				[
					AutopilotUI::Mono(Str(Run->Bill.Shows() ? PlaytestAccount::Money(Run->Bill.Usd) : FString(TEXT("—"))), 11.f,
						FSlateColor(Run->Bill.Shows() ? AutopilotColor::Text() : AutopilotColor::Muted()))
				]
			]
		];
}

TSharedRef<SWidget> SPlaytestPanel::MakeDataSharing()
{
	// (the switch under its name, and beside it whether sharing is on: "Data sharing on" / "Data sharing off")
	const FText Name = LOCTEXT("ShareRuns", "Share runs to improve Autopilot");
	TSharedRef<SCheckBox> Toggle = AutopilotUI::Switch([this]() { return Prefs.bShareData; }, [this](bool bOn) { SetShareData(bOn); },
		AutopilotUI::Body(TAttribute<FText>::CreateLambda([this]()
		{
			return Prefs.bShareData ? LOCTEXT("SharingOn", "Data sharing on") : LOCTEXT("SharingOff", "Data sharing off");
		}), 13.f, AutopilotColor::Text(), false, EAutopilotFont::BodyMedium), 8.f);
	Toggle->SetToolTipText(Name);
	return SettingCard(Name,
		LOCTEXT("ShareWhat", "Enabling this sends recordings, screenshots, and agent traces and memories to the developers of Autopilot. For runs from your own Claude Code or Codex, the agent traces are its conversation during the run."),
		SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[Toggle]);
}

TSharedRef<SWidget> SPlaytestPanel::MakeResearchAgent()
{
	// (as the sharing card: the switch under its name, and beside it "Research agent on" / "Research agent off")
	const FText Name = LOCTEXT("ResearchAgent", "Research agent");
	TSharedRef<SCheckBox> Toggle = AutopilotUI::Switch([this]() { return ResearchShown(); }, [this](bool bOn) { SetResearchAgent(bOn); },
		AutopilotUI::Body(TAttribute<FText>::CreateLambda([this]()
		{
			return ResearchShown() ? LOCTEXT("ResearchOn", "Research agent on") : LOCTEXT("ResearchOff", "Research agent off");
		}), 13.f, AutopilotColor::Text(), false, EAutopilotFont::BodyMedium), 8.f);
	Toggle->SetToolTipText(Name);
	return SettingCard(Name,
		LOCTEXT("ResearchWhat", "Enabling the research agent allows Autopilot to achieve more difficult goals with fewer attempts, but at a higher cost."),
		SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[Toggle]);
}

bool SPlaytestPanel::ResearchShown() const
{
	if (Prefs.ResearchChoice.IsSet())
	{
		return Prefs.ResearchChoice.GetValue();
	}
	return !GetDefault<UPlaytestSettings>()->bUseWithAura;
}

TSharedRef<SWidget> SPlaytestPanel::MakeCodingAgents()
{
	// Its name, what it is for, each command with Copy (in a well, as a link is)
	auto Command = [this](const FString& Words)
	{
		return Well(Words, SmallButton(LOCTEXT("CA_Copy", "Copy"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnCopy, Words)));
	};
	return SettingCard(LOCTEXT("CA_Title", "Autopilot in Claude Code & Codex"),
		LOCTEXT("CA_What", "One-time CLI setup to run Autopilot through Claude Code or Codex using your Claude or ChatGPT subscriptions, respectively."),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[Command(TEXT("autopilot connect claude"))]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)[Command(TEXT("autopilot connect codex"))]);
}

void SPlaytestPanel::RebuildSettings()
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	UsageSeen = Session.GetUsageRevision();
	if (!SettingsSettle.IsValid())
	{
		return;
	}
	// (made again, the one before shown until this one is laid out: no flash)
	SAssignNew(SettingsBox, SVerticalBox);
	SettingsSettle->SetContent(SettingsBox.ToSharedRef());
	// This project: every run kept (also those a memory reset took off the tab), newest first, with what Aura billed for
	// each and for all (the runs with usage or a bill; none: "No usage yet.")
	TArray<TSharedPtr<FAutopilotRun>> Runs;
	for (const TSharedPtr<FAutopilotRun>& Each : Session.GetUsageRuns())
	{
		if (Each.IsValid() && (Each->Usage.Num() || Each->Bill.bKnown))
		{
			Runs.Add(Each);
		}
	}
	int32 BilledRuns = 0;
	const double ProjectBill = Session.ProjectBill(BilledRuns);
	TSharedRef<SHorizontalBox> Head = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			AutopilotUI::Label(Runs.Num() ? FText::Format(LOCTEXT("ThisProject", "This project · {0} {0}|plural(one=run,other=runs)"), Runs.Num())
				: LOCTEXT("ThisProjectNone", "This project"))
		];
	if (BilledRuns)
	{
		Head->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
			[
				AutopilotUI::Body(LOCTEXT("BilledByAura", "Billed by Aura"), 12.f, AutopilotColor::Text(), false)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				AutopilotUI::Mono(Str(PlaytestAccount::Money(ProjectBill)), 12.f, FSlateColor(AutopilotColor::Text()))
			]
		];
	}
	TSharedRef<SVerticalBox> List = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 6.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)[SNew(SBox).WidthOverride(UsageWhenWidth)[AutopilotUI::Label(LOCTEXT("UsageTime", "Time"))]]
			+ SHorizontalBox::Slot().FillWidth(1.f)[AutopilotUI::Label(LOCTEXT("UsagePrompt", "Prompt"))]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SBox).WidthOverride(UsageBillWidth).HAlign(HAlign_Right)[AutopilotUI::Label(LOCTEXT("UsageBilled", "Billed"))]
			]
		];
	for (int32 i = Runs.Num() - 1; i >= 0; --i)
	{
		List->AddSlot().AutoHeight()[MakeUsageRunRow(Runs[i])];
	}
	SettingsBox->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
	[
		Runs.Num() ? AutopilotUI::Plate(FMargin(0.f),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(14.f, 9.f)[Head]
			+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 4.f, 8.f, 8.f)[List])
		: AutopilotUI::Plate(FMargin(14.f, 12.f),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Head]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				AutopilotUI::Body(LOCTEXT("NoUsage", "No usage yet."), 13.f, AutopilotColor::Text3())
			])
	];
	SettingsBox->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)[MakeDataSharing()];
	SettingsBox->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)[MakeResearchAgent()];
	SettingsBox->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)[MakeCodingAgents()];
	SettingsBox->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)[MakeRemoveSection()];
}

void SPlaytestPanel::SetShareData(bool bShare)
{
	FPlaytestPreferences Now = PlaytestPreferences::Load();
	Now.bShareData = bShare;
	Now.bOnboarded = Now.bOnboarded || Prefs.bOnboarded;
	if (PlaytestPreferences::Save(Now))
	{
		Prefs = PlaytestPreferences::Load();
	}
	else
	{
		SaySaveFailed();
	}
	Prefs.bShareData = bShare;
	ReconnectIfIdle();  // (the hello says it)
}

void SPlaytestPanel::SetResearchAgent(bool bOn)
{
	FPlaytestPreferences Now = PlaytestPreferences::Load();
	Now.ResearchChoice = bOn;
	Now.bOnboarded = Now.bOnboarded || Prefs.bOnboarded;
	if (PlaytestPreferences::Save(Now))
	{
		Prefs = PlaytestPreferences::Load();
	}
	else
	{
		SaySaveFailed();
	}
	Prefs.ResearchChoice = bOn;
	ReconnectIfIdle();  // (the hello says it)
}

// -- First-run setup ---------------------------------------------------------------------------------------------

SPlaytestPanel::EServiceView SPlaytestPanel::ServiceView() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B)
	{
		return EServiceView::Off;
	}
	if (B->IsConnected() && !B->IsWithAura())
	{
		return EServiceView::Ready;
	}
	const FPlaytestAgentService* S = ServiceOf(B);
	if (!S || !B->ManagesAgentService())
	{
		return EServiceView::Manual;
	}
	switch (S->GetState())
	{
	case EPlaytestServiceState::Running:
		return EServiceView::Ready;
	case EPlaytestServiceState::Installing:
		return EServiceView::Installing;
	case EPlaytestServiceState::Starting:
		return EServiceView::Starting;
	case EPlaytestServiceState::NotInstalled:
		return S->HasInstaller() ? EServiceView::NotInstalled : EServiceView::NoInstaller;
	case EPlaytestServiceState::BadCommand:
	case EPlaytestServiceState::InstallFailed:
	case EPlaytestServiceState::Stopped:
		return EServiceView::Problem;
	default:
		return B->WantsConnection() ? EServiceView::Starting : EServiceView::Off;
	}
}

FString SPlaytestPanel::ServiceUrlShown() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (B && B->IsConnected())
	{
		return B->GetUrl();
	}
	const FPlaytestAgentService* S = ServiceOf(B);
	return S ? S->GetUrl() : FString();
}

// The sign-in ---------------------------------------------------------------------------------------------------

SPlaytestPanel::ESignInView SPlaytestPanel::SignInView() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	// (away, it shows the agent service coming back as before it was sent, or why it can't)
	const bool bAway = IsSignInAway();
	if (bSignInWanted || bAway)
	{
		if (!SignInProblem.IsEmpty() || !B)
		{
			return ESignInView::Error;
		}
		const FPlaytestAgentService* S = ServiceOf(B);
		if (S && B->ManagesAgentService() && !B->IsConnected())
		{
			switch (S->GetState())
			{
			case EPlaytestServiceState::NotInstalled:
				if (!S->HasInstaller() || bSignInInstallTried)
				{
					return ESignInView::Error;
				}
				break;
			case EPlaytestServiceState::BadCommand:
			case EPlaytestServiceState::InstallFailed:
			case EPlaytestServiceState::Stopped:
				return ESignInView::Error;
			default:
				break;
			}
		}
		if ((bSignInConnectTried || bAway) && !B->WantsConnection() && !B->IsConnected() && !B->GetProblem().IsEmpty())
		{
			return ESignInView::Error;  // (the agent service refused the connection)
		}
		return ESignInView::Preparing;
	}
	if (bSignInSent && B)
	{
		switch (PlaytestAccount::Phase(B->GetSignIn()))
		{
		case EPlaytestSignInPhase::Asked:
			return ESignInView::Preparing;
		case EPlaytestSignInPhase::Waiting:
			return ESignInView::Waiting;
		case EPlaytestSignInPhase::SignedIn:
			return ESignInView::SignedIn;
		case EPlaytestSignInPhase::Error:
			return ESignInView::Error;
		default:
			break;
		}
	}
	return ESignInView::None;
}

bool SPlaytestPanel::IsSignInAway() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return !bSignInWanted && bSignInSent && B && PlaytestAccount::IsSignInGoing(B->GetSignIn()) && !B->IsConnected();
}

FString SPlaytestPanel::SignInLine() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	const ESignInView View = SignInView();
	if (View == ESignInView::Error)
	{
		if (!SignInProblem.IsEmpty())
		{
			return SignInProblem;
		}
		if (!B)
		{
			return TEXT("Autopilot isn’t available in this editor.");
		}
		if (bSignInWanted || IsSignInAway())
		{
			if (S && S->GetState() == EPlaytestServiceState::NotInstalled && !S->HasInstaller())
			{
				return TEXT("Set Agent service command in settings, or run autopilot serve yourself.");
			}
			if (S && !S->GetProblem().IsEmpty() && !B->IsConnected())
			{
				return S->GetProblem();
			}
			return FAutopilotSession::Scrub(B->GetProblem());
		}
		return PlaytestAccount::ErrorLine(B->GetSignIn());
	}
	if (S && B && B->ManagesAgentService() && !B->IsConnected())
	{
		if (S->GetState() == EPlaytestServiceState::Installing)
		{
			return TEXT("Setting up the agent service…");
		}
		if (S->GetState() == EPlaytestServiceState::Starting)
		{
			return TEXT("Starting the agent service…");
		}
	}
	if (B && B->IsWelcomed())
	{
		return TEXT("Opening Aura’s sign-in in your browser…");
	}
	if (B && !B->ManagesAgentService() && !B->IsConnected())
	{
		return TEXT("Waiting for the agent service: start it with autopilot serve.");
	}
	return TEXT("Connecting to the agent service…");
}

void SPlaytestPanel::BeginSignIn(bool bOutFirst)
{
	bSignInWanted = true;
	bKeysShown = false;
	bSignOutFirst = bOutFirst;
	bCreatingAccount = false;
	bSignInSent = false;
	bSignInConnectTried = false;
	bSignInInstallTried = false;
	SignInProblem.Empty();
	if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
	{
		B->ClearSignIn();
		if (FPlaytestAgentService* S = ServiceOf(B))
		{
			S->Retry();
		}
	}
}

void SPlaytestPanel::CancelSignIn()
{
	if (bSignInSent)
	{
		if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
		{
			B->CancelAuraSignIn();
		}
	}
	bSignInWanted = false;
	bSignOutFirst = false;
	bSignInSent = false;
	SignInProblem.Empty();
}

void SPlaytestPanel::TickSignIn()
{
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B)
	{
		return;
	}
	// Signed in from the Settings page: its account corner shows the account frame that follows
	if (!IsSetupShown() && bSignInSent && PlaytestAccount::Phase(B->GetSignIn()) == EPlaytestSignInPhase::SignedIn)
	{
		bSignInSent = false;
		B->ClearSignIn();
		B->RequestAccount();
	}
	if (!bSignInWanted || SignInView() == ESignInView::Error)
	{
		return;
	}
	// The sign-in is the agent service's: install it, start it and connect to it first
	if (!B->IsWelcomed())
	{
		if (!bSignInConnectTried && !B->IsConnected())
		{
			bSignInConnectTried = true;
			B->Connect();
		}
		FPlaytestAgentService* S = ServiceOf(B);
		if (S && B->ManagesAgentService() && S->GetState() == EPlaytestServiceState::NotInstalled && S->HasInstaller() && !bSignInInstallTried)
		{
			bSignInInstallTried = true;
			S->Install();
		}
		return;
	}
	if (!B->HasAccountFeature())
	{
		SignInProblem = TEXT("This agent service can’t sign in to Aura: install it again, then try again.");
		return;
	}
	B->SignInWithAura(bSignOutFirst);
	bSignInWanted = false;
	bSignOutFirst = false;
	bSignInSent = true;
}

TSharedRef<SWidget> SPlaytestPanel::MakeSignInBody(bool bCompact)
{
	const ESignInView View = SignInView();
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	const float Gap = 10.f;
	auto CancelButton = [this, bCompact]()
	{
		return AutopilotUI::Button(LOCTEXT("S_Cancel", "Cancel"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]() { CancelSignIn(); return FReply::Handled(); }),
			true, nullptr, bCompact);
	};
	if (View == ESignInView::Preparing)
	{
		// Getting ready: setting up the agent service (when it had to be installed), starting it, then the browser
		const EPlaytestServiceState State = S ? S->GetState() : EPlaytestServiceState::Idle;
		const bool bManaged = B && B->ManagesAgentService();
		const bool bInstalling = bManaged && State == EPlaytestServiceState::Installing;
		const bool bInstalled = bSignInInstallTried && !bInstalling;
		const bool bAsked = B && B->IsWelcomed();  // (the agent service is there: the sign-in is asked for)
		auto Step = [](int32 Phase, const FString& Line) -> TSharedRef<SWidget>  // Phase: -1 done, 0 now, 1 to come
		{
			TSharedRef<SWidget> Mark = Phase == 0 ? StaticCastSharedRef<SWidget>(SNew(AutopilotUI::SSpinner))
				: Dot(Phase < 0 ? AutopilotColor::Success() : AutopilotColor::Line(), 12.f);
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(12.f).HAlign(HAlign_Center)[Mark]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					AutopilotUI::Body(Str(Line), Phase == 0 ? 15.f : 13.f, Phase == 0 ? AutopilotColor::Text() : Phase < 0 ? AutopilotColor::Text3() : AutopilotColor::Muted(),
						true, Phase == 0 ? EAutopilotFont::BodyMedium : EAutopilotFont::Body)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					AutopilotUI::Mono(Phase < 0 ? LOCTEXT("S_Done", "done") : FText::GetEmpty(), 10.f, FSlateColor(AutopilotColor::Muted()))
				];
		};
		if (bInstalling || bInstalled)
		{
			Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)[Step(bInstalling ? 0 : -1, TEXT("Setting up the agent service…"))];
		}
		const FString Starting = !bManaged && !(B && B->IsConnected()) ? FString(TEXT("Waiting for the agent service: start it with autopilot serve."))
			: FString(TEXT("Starting the agent service…"));
		Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)[Step(bInstalling ? 1 : bAsked ? -1 : 0, Starting)];
		Box->AddSlot().AutoHeight()[Step(bAsked ? 0 : 1, TEXT("Finish signing in in your browser…"))];
		Box->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f).HAlign(HAlign_Left)[CancelButton()];
	}
	else if (View == ESignInView::Waiting)
	{
		const FString Url = B ? B->GetSignIn().Url : FString();
		Box->AddSlot().AutoHeight()[Progress(TEXT("Finish signing in in your browser…"), bCompact ? 13.f : 15.f)];
		Box->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			Well(Url, SmallButton(LOCTEXT("S_CopyLink", "Copy link"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnCopy, Url)))
		];
		TSharedRef<SWrapBox> Row = ButtonRow();
		AddTo(Row, AutopilotUI::Button(LOCTEXT("S_OpenAgain", "Open again"), "Autopilot.Button", FOnClicked::CreateLambda([Url]()
		{
			FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
			return FReply::Handled();
		}), true, nullptr, bCompact));
		AddTo(Row, CancelButton());
		Box->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)[Row];
	}
	else if (View == ESignInView::Error)
	{
		Box->AddSlot().AutoHeight()[AutopilotUI::Body(Str(SignInLine()), 13.f, AutopilotColor::Red())];
		TSharedRef<SWrapBox> Row = ButtonRow();
		AddTo(Row, AutopilotUI::Button(LOCTEXT("S_TryAgain", "Try again"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([this]()
		{
			BeginSignIn(false);
			return FReply::Handled();
		}), true, nullptr, bCompact));
		const bool bServiceProblem = bSignInWanted && SignInProblem.IsEmpty() && S && !(B && B->IsConnected());
		const EPlaytestServiceState State = S ? S->GetState() : EPlaytestServiceState::Idle;
		if (bServiceProblem && (State == EPlaytestServiceState::InstallFailed || State == EPlaytestServiceState::Stopped) && !S->GetLogPath().IsEmpty())
		{
			const FString Log = S->GetLogPath();
			AddTo(Row, AutopilotUI::Button(LOCTEXT("S_OpenLog", "View logs"), "Autopilot.Button", FOnClicked::CreateLambda([Log]() { OpenLog(Log); return FReply::Handled(); }),
				true, nullptr, bCompact));
		}
		if (bServiceProblem && State == EPlaytestServiceState::NotInstalled && !S->HasInstaller())
		{
			AddTo(Row, AutopilotUI::Button(LOCTEXT("S_OpenSettings", "Open settings"), "Autopilot.Button", FOnClicked::CreateLambda([]() { ShowSettings(); return FReply::Handled(); }),
				true, nullptr, bCompact));
		}
		Box->AddSlot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)[Row];
	}
	return Box;
}

// The steps ------------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeStepper()
{
	// Four steps, 8 apart: a 2px bar (done #6b4122, now the accent, to come the rule) and its micro-label
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 Step = StepAccount; Step <= StepFirstRun; ++Step)
	{
		Row->AddSlot().FillWidth(1.f).Padding(Step ? 8.f : 0.f, 0.f, 0.f, 0.f)
		[
			SNew(SVerticalBox)
			// (4 First run: unless the game's memory holds something, when the setup ends at step 3)
			.Visibility_Lambda([this, Step]()
			{
				return Step != StepFirstRun || SetupStep == StepFirstRun || MemoryView() != EMemoryView::Something ? EVisibility::Visible : EVisibility::Collapsed;
			})
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).HeightOverride(2.f)
				[
					AutopilotUI::Fill(TAttribute<FSlateColor>::CreateLambda([this, Step]()
					{
						return FSlateColor(Step < SetupStep ? AutopilotColor::StepDone() : Step == SetupStep ? AutopilotColor::Accent() : AutopilotColor::Rule());
					}))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Font(FAutopilotStyle::Label())
				.AutoWrapText(true)
				.ColorAndOpacity_Lambda([this, Step]()
				{
					return FSlateColor(Step < SetupStep ? AutopilotColor::Text3() : Step == SetupStep ? AutopilotColor::Accent() : AutopilotColor::Muted());
				})
				.Text_Lambda([this, Step]()
				{
					if (Step == StepAccount)
					{
						return Str(SetupStep > StepAccount && !SetupAccountWord.IsEmpty() ? TEXT("1 ACCOUNT · ") + SetupAccountWord : FString(TEXT("1 ACCOUNT")));
					}
					return Str(Step == StepService ? TEXT("2 AGENT SERVICE") : Step == StepPrivacy ? TEXT("3 PRIVACY SETTINGS") : TEXT("4 FIRST RUN"));
				})
			]
		];
	}
	return Row;
}

TSharedRef<SWidget> SPlaytestPanel::MakeKeysCard()
{
	const FEditableTextBoxStyle* BoxStyle = &FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox");
	const FEditableTextBoxStyle* MonoStyle = &FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Mono");
	auto Labelled = [](const FText& Name, const TSharedRef<SWidget>& Field) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 5.f)[AutopilotUI::Label(Name)]
			+ SVerticalBox::Slot().AutoHeight()[Field];
	};
	// A dropdown of ids shown by name, opening on the one shown (which may have come from the account frame or autopilot.env)
	using FNameOf = FString (*)(const FString&);
	auto Dropdown = [](TArray<TSharedPtr<FString>>* Items, TSharedPtr<SComboBox<TSharedPtr<FString>>>& Combo, const FText& What, FNameOf NameOf,
		TFunction<FString()> Shown, TFunction<void(const FString&)> Pick, const TAttribute<bool>& Enabled) -> TSharedRef<SWidget>
	{
		TSharedPtr<SComboBox<TSharedPtr<FString>>>* ComboOf = &Combo;
		return SNew(SBox)
			.HeightOverride(AutopilotUI::ButtonHeight)
			[
				SAssignNew(Combo, SComboBox<TSharedPtr<FString>>)
				.ComboBoxStyle(&FAutopilotStyle::Get().GetWidgetStyle<FComboBoxStyle>("Autopilot.ComboBox"))
				.ItemStyle(&FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row"))
				.IsEnabled(Enabled)
				.OptionsSource(Items)
				.ToolTipText(What)
				.AccessibleText(What)
				.OnGenerateWidget_Lambda([NameOf](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
				{
					return SNew(SBox).Padding(FMargin(9.f, 5.f))
						[
							SNew(STextBlock)
							.Text(Str(NameOf(Item.IsValid() ? *Item : FString())))
							.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 13.f))
							.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
						];
				})
				.OnComboBoxOpening_Lambda([Items, ComboOf, Shown]()
				{
					const FString Now = Shown();
					for (const TSharedPtr<FString>& Item : *Items)
					{
						if (*Item == Now && ComboOf->IsValid())
						{
							(*ComboOf)->SetSelectedItem(Item);
						}
					}
				})
				.OnSelectionChanged_Lambda([Pick](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						Pick(*Item);
					}
				})
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 13.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Text()))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([NameOf, Shown]() { return Str(NameOf(Shown())); })
				]
			];
	};
	auto Key = [](const FEditableTextBoxStyle* Style, TSharedPtr<SEditableTextBox>& Box, const FText& Hint) -> TSharedRef<SWidget>
	{
		return SNew(SBox).HeightOverride(AutopilotUI::ButtonHeight)
			[
				SAssignNew(Box, SEditableTextBox)
				.Style(Style)
				.IsPassword(true)
				.HintText(Hint)
			];
	};
	// The Coding Agent: full width. It decides the LLM provider for Claude Code (Anthropic) and Codex (OpenAI)
	const FText AgentText = LOCTEXT("CodingAgent", "Coding Agent");
	TSharedRef<SWidget> AgentField = Labelled(AgentText,
		Dropdown(&CodingAgentItems, CodingAgentCombo, AgentText, &PlaytestAccount::AgentName,
			[this]() { return SetupCodingAgent(); }, [this](const FString& Agent) { PickedCodingAgent = Agent; FollowLlmProvider(); }, true));
	// The LLM key: its provider first (a 140px dropdown, locked unless the agent is OpenCode), the key in the rest of the row
	const FText ProviderText = LOCTEXT("LlmProvider", "LLM provider");
	TSharedRef<SWidget> LlmField = Labelled(LOCTEXT("LlmKey", "LLM provider API key"),
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SBox).WidthOverride(140.f)
			[
				Dropdown(&LlmProviderItems, LlmProviderCombo, ProviderText, &PlaytestAccount::ProviderName,
					[this]() { return SetupLlmProvider(); }, [this](const FString& Provider)
					{
						if (PlaytestAccount::AgentProvider(SetupCodingAgent()).IsEmpty())
						{
							PickedLlmProvider = Provider;  // (kept for OpenCode while another agent is picked)
						}
						FollowLlmProvider();
					},
					TAttribute<bool>::CreateLambda([this]() { return PlaytestAccount::AgentProvider(SetupCodingAgent()).IsEmpty(); }))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			Key(MonoStyle, LlmKeyBox, LOCTEXT("LlmKeyHint", "Optional"))  // (the dropdown names the provider)
		]);
	const FText Name = LOCTEXT("SetupKeys", "Bring your own keys (BYOK)");
	TSharedRef<SWidget> Full = Card(SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[AutopilotUI::Label(Name)]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				AutopilotUI::Button(LOCTEXT("S_Hide", "Hide"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]() { bKeysShown = false; return FReply::Handled(); }),
					true, nullptr, true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			Labelled(LOCTEXT("DecisionKey", "Decision provider API key"), Key(MonoStyle, TypeSafeKeyBox, LOCTEXT("DecisionKeyHint", "Optional")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			AgentField
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			LlmField
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f).HAlign(HAlign_Left)
		[
			AutopilotUI::Button(LOCTEXT("UseKeys", "Use my keys"), "Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetupKeys))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Red()))
			.AutoWrapText(true)
			.Visibility_Lambda([this]() { return SetupError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return Str(SetupError); })
		]);
	// Folded until Show (Hide folds it again); dimmed (55%) once signed in. While the sign-in goes on (getting ready, in
	// the browser, the agent service needed): not shown at all, Cancel brings it back
	auto Folded = [this, Name](float Opacity) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.RenderOpacity(Opacity)
			[
				Card(SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[AutopilotUI::Label(Name)]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						AutopilotUI::Button(LOCTEXT("S_Show", "Show"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]() { bKeysShown = true; return FReply::Handled(); }),
							true, nullptr, true)
					])
			];
	};
	return SNew(SWidgetSwitcher)
		.Visibility_Lambda([this]()
		{
			const ESignInView View = SignInView();
			return View == ESignInView::Preparing || View == ESignInView::Waiting ? EVisibility::Collapsed : EVisibility::Visible;
		})
		.WidgetIndex_Lambda([this]()
		{
			return bKeysShown ? 0 : SignInView() == ESignInView::SignedIn ? 2 : 1;
		})
		+ SWidgetSwitcher::Slot()[Full]
		+ SWidgetSwitcher::Slot()[Folded(1.f)]
		+ SWidgetSwitcher::Slot()[Folded(0.55f)];
}
FString SPlaytestPanel::SetupCodingAgent() const
{
	return PickedCodingAgent.IsEmpty() ? PlaytestAccount::PickAgent(ShownAccount(), SavedCodingAgent) : PickedCodingAgent;
}

FString SPlaytestPanel::SetupLlmProvider() const
{
	const FString Fixed = PlaytestAccount::AgentProvider(SetupCodingAgent());
	if (!Fixed.IsEmpty())
	{
		return Fixed;
	}
	return PickedLlmProvider.IsEmpty() ? PlaytestAccount::PickProvider(ShownAccount(), SavedLlmProvider) : PickedLlmProvider;
}

void SPlaytestPanel::FollowLlmProvider()
{
	const FString Provider = SetupLlmProvider();
	if (Provider != LlmKeyProvider)
	{
		if (LlmKeyBox.IsValid())
		{
			LlmKeyBox->SetText(FText::GetEmpty());
		}
		LlmKeyProvider = Provider;
	}
}

TSharedRef<SWidget> SPlaytestPanel::MakeAuraCard()
{
	const ESignInView View = SignInView();
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	const FText Name = LOCTEXT("SetupAura", "Sign in with Aura");
	FLinearColor Outline = AutopilotColor::Rule();
	switch (View)
	{
	case ESignInView::None:
	{
		if (bCreatingAccount)
		{
			// Create Account opened Aura's account creation page: once the account is made, Sign in as below
			Outline = AutopilotColor::InfoLine();
			Box->AddSlot().AutoHeight()[CardHead(Name, LOCTEXT("S_InBrowser", "In your browser"), EAutopilotTone::Info)];
			Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				AutopilotUI::Body(LOCTEXT("S_FinishCreating", "Finish creating your Aura account"), 15.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
			];
			Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				AutopilotUI::Body(LOCTEXT("S_CreatedThenSignIn", "Once you have created your account, sign in and start using Autopilot"), 13.f, AutopilotColor::Text3())
			];
			TSharedRef<SWrapBox> Row = ButtonRow();
			AddTo(Row, AutopilotUI::Button(LOCTEXT("S_SignIn", "Sign in"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetupSignIn)));
			Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Row];
			break;
		}
		// Both in the browser, the Aura app installed or not: Aura's account creation page first, its sign-in second
		// (the Aura app's own choice, OnSetupAuraApp, isn't offered)
		Box->AddSlot().AutoHeight()[CardHead(Name)];
		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			AutopilotUI::Body(LOCTEXT("S_AuraText", "Autopilot is maintained by the developers behind Aura, the multiagent AI solution to Unreal and Unity game development. You can use your Aura subscription to run Autopilot. New users get free Aura credits."),
				13.f, AutopilotColor::Text3())
		];
		TSharedRef<SWrapBox> Row = ButtonRow();
		AddTo(Row, AutopilotUI::Button(LOCTEXT("S_CreateAccount", "Create Account"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetupCreateAccount)));
		AddTo(Row, AutopilotUI::Button(LOCTEXT("S_SignIn", "Sign in"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetupSignIn)));
		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Row];
		break;
	}
	case ESignInView::SignedIn:
	{
		Outline = AutopilotColor::SuccessLine();
		FString Email = B ? B->GetSignIn().Email : FString();
		if (Email.IsEmpty() && B)
		{
			Email = B->GetAccount().Email;
		}
		Box->AddSlot().AutoHeight()[CardHead(Name, LOCTEXT("S_SignedIn", "Signed in"), EAutopilotTone::Success)];
		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8.f, 2.f))
			+ SWrapBox::Slot().VAlign(VAlign_Bottom)
			[
				AutopilotUI::Body(LOCTEXT("S_SignedInAs", "Signed in as"), 12.f, AutopilotColor::Muted(), false)
			]
			+ SWrapBox::Slot().VAlign(VAlign_Bottom)
			[
				AutopilotUI::Body(Email.IsEmpty() ? LOCTEXT("S_YourAccount", "your Aura account") : Str(Email), 15.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
			]
		];
		TSharedRef<SWrapBox> Row = ButtonRow();
		AddTo(Row, AutopilotUI::Button(LOCTEXT("S_Continue", "Continue"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([this]()
		{
			SetupAccess = TEXT("aura");
			SetupAccountWord = TEXT("AURA");
			SetupError.Empty();
			GoToServiceStep();
			return FReply::Handled();
		})));
		AddTo(Row, AutopilotUI::Button(LOCTEXT("S_Another", "Use another account"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
		{
			BeginSignIn(true);
			return FReply::Handled();
		})));
		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Row];
		break;
	}
	case ESignInView::Error:
		Outline = AutopilotColor::RedLine();
		Box->AddSlot().AutoHeight()[CardHead(Name, LOCTEXT("S_NotSignedIn", "Not signed in"), EAutopilotTone::Red)];
		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[MakeSignInBody(false)];
		break;
	default:  // (preparing, waiting)
		Outline = AutopilotColor::InfoLine();
		Box->AddSlot().AutoHeight()[CardHead(Name, LOCTEXT("S_InBrowser", "In your browser"), EAutopilotTone::Info)];
		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[MakeSignInBody(false)];
		break;
	}
	return Card(Box, Outline);
}

TSharedRef<SWidget> SPlaytestPanel::MakeServiceStep()
{
	const EServiceView View = ServiceView();
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	auto Later = [this]()
	{
		return AutopilotUI::Button(LOCTEXT("S_Later", "I’ll do it later"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
		{
			SetupStep = StepPrivacy;
			return FReply::Handled();
		}));
	};
	auto Continue = [this](bool bEnabled)
	{
		return AutopilotUI::Button(LOCTEXT("S_Continue", "Continue"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([this]()
		{
			SetupStep = StepPrivacy;
			return FReply::Handled();
		}), bEnabled, nullptr, false, 20.f);
	};
	// A progress plate: the spinner and its line, the info outline
	auto ProgressCard = [](const FString& Line) -> TSharedRef<SWidget>
	{
		return Card(Progress(Line), AutopilotColor::InfoLine());
	};
	// A plate's first line: a 7px dot (amber: to act on) and its words (500)
	auto DotLine = [](const FText& Text) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)[Dot(AutopilotColor::Amber(), 7.f)]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[AutopilotUI::Body(Text, 13.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)];
	};
	TSharedRef<SWrapBox> Right = ButtonRow();
	switch (View)
	{
	case EServiceView::Ready:
	{
		const FText Where = B && B->ManagesAgentService() ? LOCTEXT("S_StartsWithEditor", "Starts with your editor") : LOCTEXT("S_RunsHere", "Running on this computer");
		Box->AddSlot().AutoHeight()
		[
			Card(SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
				[
					SNew(SImage).Image(FAutopilotStyle::Brush("Autopilot.Check"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						AutopilotUI::Body(LOCTEXT("S_Ready", "Agent service ready"), 13.f, AutopilotColor::SuccessText(), true, EAutopilotFont::BodyMedium)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						AutopilotUI::Body(Where, 12.f, AutopilotColor::Text3())
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return IsCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						AutopilotUI::Mono(Str(ServiceUrlShown()), 10.5f, FSlateColor(AutopilotColor::Muted()))
					]
				],
				AutopilotColor::SuccessLine(), AutopilotColor::SuccessFill())
		];
		AddTo(Right, Continue(true));
		break;
	}
	case EServiceView::Installing:
	case EServiceView::Starting:
	case EServiceView::Off:
	{
		const bool bInstalling = View == EServiceView::Installing;
		Box->AddSlot().AutoHeight()[ProgressCard(bInstalling ? TEXT("Installing the agent service…") : TEXT("Starting the agent service…"))];
		AddTo(Right, Continue(false));
		break;
	}
	case EServiceView::NotInstalled:
	{
		Box->AddSlot().AutoHeight()
		[
			Card(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[DotLine(LOCTEXT("S_NotInstalled", "Not installed on this computer"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					AutopilotUI::Body(LOCTEXT("S_InstallText", "One-time installation takes approximately 1 minute."), 13.f, AutopilotColor::Text3())
				])
		];
		AddTo(Right, Later());
		AddTo(Right, AutopilotUI::Button(LOCTEXT("S_Install", "Install"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([]()
		{
			UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
			if (FPlaytestAgentService* Service = ServiceOf(Bridge))
			{
				Service->Install();
				if (!Bridge->WantsConnection())
				{
					Bridge->Connect();  // (the agent service starts once installed)
				}
			}
			return FReply::Handled();
		}), true, nullptr, false, 20.f));
		break;
	}
	case EServiceView::NoInstaller:
	{
		// No agent service found (a repository checkout): its command here (the Agent service command setting), Start;
		// or autopilot serve in a terminal
		TSharedRef<SEditableTextBox> Command = SNew(SEditableTextBox)
			.Style(&FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Mono"))
			.Text(Str(GetDefault<UPlaytestSettings>()->AgentServiceCommand))
			.HintText(LOCTEXT("S_CommandHint", "e.g. C:\\autopilot\\.venv\\Scripts\\python.exe -m autopilot"));
		CommandBox = Command;
		TSharedRef<SWidget> Start = AutopilotUI::Button(LOCTEXT("S_Start", "Start"), "Autopilot.Button", FOnClicked::CreateLambda([this]()
		{
			UPlaytestSettings* Settings = GetMutableDefault<UPlaytestSettings>();
			Settings->AgentServiceCommand = CommandBox.IsValid() ? CommandBox->GetText().ToString().TrimStartAndEnd() : FString();
			Settings->SaveConfig();
			if (FPlaytestAgentService* Service = ServiceOf(UPlaytestSubsystem::Get()))
			{
				Service->Retry();  // (it looks for the command again)
			}
			TryConnectAgain();
			return FReply::Handled();
		}));
		Box->AddSlot().AutoHeight()
		[
			Card(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[DotLine(LOCTEXT("S_NoInstaller", "No agent service found"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 5.f)
				[
					AutopilotUI::Label(LOCTEXT("S_CommandLabel", "Agent service command"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
					[
						SNew(SBox).HeightOverride(AutopilotUI::ButtonHeight)[Command]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Start]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8.f, 4.f))
					+ SWrapBox::Slot().VAlign(VAlign_Center)
					[
						AutopilotUI::Body(LOCTEXT("S_OrRun", "Or run"), 13.f, AutopilotColor::Text3(), false)
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)
					[
						AutopilotUI::Mono(Str(TEXT("autopilot serve")), 12.f, FSlateColor(AutopilotColor::Accent()))
					]
					+ SWrapBox::Slot().VAlign(VAlign_Center)
					[
						SmallButton(LOCTEXT("S_CopyServe", "Copy"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnCopy, FString(TEXT("autopilot serve"))))
					]
				])
		];
		AddTo(Right, Later());
		break;
	}
	case EServiceView::Problem:
	{
		const FString Log = S ? S->GetLogPath() : FString();
		TSharedRef<SWrapBox> Fix = ButtonRow();
		if (!Log.IsEmpty())
		{
			AddTo(Fix, SmallButton(LOCTEXT("S_OpenLog", "View logs"), "Autopilot.Button", FOnClicked::CreateLambda([Log]() { OpenLog(Log); return FReply::Handled(); })));
		}
		AddTo(Fix, SmallButton(LOCTEXT("S_TryAgain", "Try again"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([]()
		{
			UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
			FPlaytestAgentService* Service = ServiceOf(Bridge);
			if (Service && Service->GetState() == EPlaytestServiceState::InstallFailed && Service->HasInstaller())
			{
				Service->Install();  // (the install failed: run it again, then it starts)
			}
			if (Bridge)
			{
				Bridge->Connect();  // (clears the problem and starts it again)
			}
			return FReply::Handled();
		})));
		// (the board's headline; the service's own words under it, small)
		const FString Problem = TEXT("The agent service didn\u2019t start.");
		const FString Said = S ? S->GetProblem() : FString();
		Box->AddSlot().AutoHeight()
		[
			Card(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					AutopilotUI::Body(Str(Problem), 13.f, AutopilotColor::RedText(), true, EAutopilotFont::BodyMedium)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Visibility(Said.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.Text(Str(Said))
					.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.5f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Neutral()))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					Fix
				], AutopilotColor::RedLine())
		];
		AddTo(Right, Later());
		break;
	}
	case EServiceView::Manual:
	default:
	{
		const UPlaytestSettings* Settings = GetDefault<UPlaytestSettings>();
		const bool bCustom = !Settings->ServiceUrl.TrimStartAndEnd().IsEmpty() && !Settings->ServiceUrl.Contains(TEXT("127.0.0.1:8765"));
		Box->AddSlot().AutoHeight()
		[
			Card(AutopilotUI::Body(Str(bCustom ? FString::Printf(TEXT("Waiting for the agent service at %s\u2026"), *Settings->ServiceUrl)
				: FString(TEXT("Waiting for the agent service: start it with autopilot serve."))), 13.f, AutopilotColor::AmberText(), true, EAutopilotFont::BodyMedium),
				AutopilotColor::AmberLine(), AutopilotColor::AmberFill())
		];
		AddTo(Right, Later());
		AddTo(Right, Continue(false));  // (until it is ready)
		break;
	}
	}
	// Back at the left, the way on at the right, under a rule
	Box->AddSlot().AutoHeight().Padding(0.f, 18.f, 0.f, 0.f)[AutopilotUI::Rule()];
	Box->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 6.f, 0.f)
		[
			AutopilotUI::Button(LOCTEXT("Back", "Back"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
			{
				SetupStep = StepAccount;
				return FReply::Handled();
			}))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right)
		[
			Right
		]
	];
	return Box;
}
FString SPlaytestPanel::SetupKey() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	const ESignInView View = SignInView();
	FString Key = FString::Printf(TEXT("%d|%d|%d|%d|%s|%s|%s|%s"), (int32)View, (int32)ServiceView(), SetupStep, PlaytestAura::IsInstalled() ? 1 : 0,
		*SetupAccess, *ServiceUrlShown(), B && B->ManagesAgentService() ? TEXT("m") : TEXT(""), S && S->HasInstaller() ? TEXT("i") : TEXT(""));
	if (bCreatingAccount)
	{
		Key += TEXT("|creating");
	}
	if (View == ESignInView::Preparing || View == ESignInView::Error)
	{
		Key += TEXT("|") + SignInLine();
	}
	if (B)
	{
		Key += TEXT("|") + B->GetSignIn().Url + TEXT("|") + B->GetSignIn().Email;
	}
	if (S)
	{
		Key += TEXT("|") + S->GetProblem() + TEXT("|") + S->GetLogPath();
	}
	return Key;
}

void SPlaytestPanel::RebuildSetup()
{
	if (AuraCardBox.IsValid())
	{
		AuraCardBox->SetContent(MakeAuraCard());
	}
	if (ServiceStepBox.IsValid())
	{
		ServiceStepBox->SetContent(MakeServiceStep());
	}
}

FText SPlaytestPanel::SetupRecapAccount() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (SetupAccountWord == TEXT("AURA APP"))
	{
		return LOCTEXT("S_RecapApp", "Aura app");
	}
	if (SetupAccountWord == TEXT("AURA"))
	{
		FString Email = B ? B->GetSignIn().Email : FString();
		if (Email.IsEmpty() && B)
		{
			Email = B->GetAccount().Email;
		}
		return Email.IsEmpty() ? LOCTEXT("S_RecapAura", "Aura") : FText::Format(LOCTEXT("S_RecapAuraEmail", "Aura · {0}"), Str(Email));
	}
	if (SetupAccountWord == TEXT("BYOK"))
	{
		return LOCTEXT("S_RecapKeys", "BYOK");
	}
	// Decided later: what was chosen before stays
	if (Prefs.Access == TEXT("aura"))
	{
		return GetDefault<UPlaytestSettings>()->bUseWithAura ? LOCTEXT("S_RecapApp", "Aura app") : LOCTEXT("S_RecapAura", "Aura");
	}
	if (Prefs.Access == TEXT("own_keys"))
	{
		return LOCTEXT("S_RecapKeys", "BYOK");
	}
	return LOCTEXT("S_RecapLater", "Not chosen yet");
}

FText SPlaytestPanel::SetupRecapService() const
{
	if (SetupAccountWord == TEXT("AURA APP") || (SetupAccountWord.IsEmpty() && GetDefault<UPlaytestSettings>()->bUseWithAura))
	{
		return LOCTEXT("S_RecapServiceApp", "The Aura app runs it");
	}
	switch (ServiceView())
	{
	case EServiceView::Ready:
		return LOCTEXT("S_RecapReady", "Ready");
	case EServiceView::Installing:
		return LOCTEXT("S_RecapInstalling", "Installing\u2026");
	case EServiceView::Starting:
		return LOCTEXT("S_RecapStarting", "Starting\u2026");
	case EServiceView::NotInstalled:
	case EServiceView::NoInstaller:
		return LOCTEXT("S_RecapNotInstalled", "Not installed yet");
	default:
		return LOCTEXT("S_RecapNotSetUp", "Not set up yet");
	}
}

TSharedRef<SWidget> SPlaytestPanel::MakeSetup()
{
	constexpr float Gap = 18.f;  // (between the setup's blocks)

	// Step 1: the account (the Aura card, the keys card folded under it)
	TSharedRef<SWidget> Account = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[PageTitle(LOCTEXT("Welcome", "Welcome to Autopilot"))]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			SAssignNew(AuraCardBox, AutopilotUI::SSettle)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			MakeKeysCard()
		];

	// Step 2: the agent service
	TSharedRef<SWidget> Service = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[PageTitle(LOCTEXT("S_ServiceTitle", "Set up the agent service"))]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			SAssignNew(ServiceStepBox, AutopilotUI::SSettle)
		];

	// Step 3: privacy settings (the switch and what sharing sends; what was chosen, between rules; Back, Next)
	auto RecapRow = [](const FText& Name, TAttribute<FText> Value) -> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(FMargin(0.f, 8.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 3.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(130.f)[AutopilotUI::Label(Name)]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
				[
					AutopilotUI::Body(Value, 13.f, AutopilotColor::Text())
				]
			];
	};
	TSharedRef<SWidget> Privacy = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[PageTitle(LOCTEXT("PrivacyTitle", "Privacy settings"))]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			Card(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
				[
					AutopilotUI::Switch([this]() { return bSetupShare; }, [this](bool bOn) { bSetupShare = bOn; },
						AutopilotUI::Body(LOCTEXT("ShareMyRuns", "Share my runs to improve Autopilot"), 13.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					AutopilotUI::Body(LOCTEXT("ShareMyRunsText", "Enabling this sends recordings, screenshots, and agent traces and memories to the developers of Autopilot. For runs from your own Claude Code or Codex, the agent traces are its conversation during the run."),
						13.f, AutopilotColor::Text3())
				])
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight()
		[
			RecapRow(LOCTEXT("S_RecapAccountLabel", "Account"), TAttribute<FText>::CreateSP(this, &SPlaytestPanel::SetupRecapAccount))
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight()
		[
			RecapRow(LOCTEXT("S_RecapServiceLabel", "Agent service"), TAttribute<FText>::CreateSP(this, &SPlaytestPanel::SetupRecapService))
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				AutopilotUI::Button(LOCTEXT("Back", "Back"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
				{
					SetupStep = bSetupServiceShown ? StepService : StepAccount;
					return FReply::Handled();
				}))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				// Finish when the game's memory holds something; else Next, to step 4
				AutopilotUI::Button(TAttribute<FText>::CreateLambda([this]()
					{
						return MemoryView() == EMemoryView::Something ? LOCTEXT("Finish", "Finish") : LOCTEXT("Next", "Next");
					}), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetupFinish), true, nullptr, false, 20.f)
			]
		];

	// (every step the same width: the tab's up to SetupMax, whatever the step holds)
	TSharedRef<SWidget> Setup = SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			Capped(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[MakeStepper()]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, Gap, 0.f, 0.f)
				[
					SNew(AutopilotUI::SSettle)
					.WidgetIndex_Lambda([this]() { return SetupStep; })
					.Pages({ Account, Service, Privacy, MakeFirstRunStep() })
				],
				SetupMax, FMargin(20.f, 28.f, 20.f, 20.f))
		];
	RebuildSetup();
	return Setup;
}
void SPlaytestPanel::OpenSetup()
{
	bSetupOpen = true;
	SetupStep = StepAccount;
	SetupAccess.Empty();
	SetupAccountWord.Empty();
	SetupError.Empty();
	bSetupServiceShown = false;
	bSetupShare = Prefs.bShareData;
	bConfirmSignOut = false;
	bKeysShown = false;
	bCreatingAccount = false;
	PickedCodingAgent.Empty();
	PickedLlmProvider.Empty();
	SavedCodingAgent = PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_AGENT"), KeysDir);
	SavedLlmProvider = PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_OPENCODE_PROVIDER"), KeysDir);
	CancelSignIn();
	if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
	{
		B->RequestAccount();
	}
}

void SPlaytestPanel::GoToServiceStep()
{
	SetupStep = StepService;
	bSetupServiceShown = true;
	// Shown ready at once when one is running; else the editor starts it (installs it when asked: Install)
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (B && !B->IsConnected())
	{
		B->Connect();
	}
}

FReply SPlaytestPanel::OnSetupAuraApp()
{
	// The Aura app's agent service (Use with Aura): nothing to install or start, so step 2 is skipped
	CancelSignIn();
	SetupAccess = TEXT("aura");
	SetupAccountWord = TEXT("AURA APP");
	SetupError.Empty();
	SetUseWithAura(true);
	bSetupServiceShown = false;
	SetupStep = StepPrivacy;
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSetupSignIn()
{
	// The sign-in is the user's own agent service's (it keeps it for the computer): not the Aura app's
	SetupError.Empty();
	SetUseWithAura(false);
	BeginSignIn(false);
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSetupCreateAccount()
{
	// Aura's account creation page in the browser, and the card's next page: Sign in once the account is made
	SetupError.Empty();
	FPlatformProcess::LaunchURL(*PlaytestAura::SignUpUrl(KeysDir), nullptr, nullptr);
	bCreatingAccount = true;
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSetupKeys()
{
	// The agent picked always (and OpenCode's provider); each key only when given (an empty one keeps its line), the LLM
	// key under its provider's name
	const FString Agent = SetupCodingAgent();
	const FString Provider = SetupLlmProvider();
	const TArray<TPair<FString, FString>> Keys = PlaytestPreferences::SetupKeys(TypeSafeKeyBox.IsValid() ? TypeSafeKeyBox->GetText().ToString() : FString(),
		Agent, Provider, LlmKeyBox.IsValid() ? LlmKeyBox->GetText().ToString() : FString());
	const FString Error = PlaytestPreferences::WriteKeys(Keys, KeysDir);  // (never shown or logged)
	if (!Error.IsEmpty())
	{
		SetupError = Error;
		return FReply::Handled();
	}
	SavedCodingAgent = Agent;
	if (PlaytestAccount::AgentProvider(Agent).IsEmpty())
	{
		SavedLlmProvider = Provider;  // (Claude Code's and Codex's leave OpenCode's as it was)
	}
	for (const TSharedPtr<SEditableTextBox>& Box : { TypeSafeKeyBox, LlmKeyBox })
	{
		if (Box.IsValid())
		{
			Box->SetText(FText::GetEmpty());
		}
	}
	CancelSignIn();
	SetupAccess = TEXT("own_keys");
	SetupAccountWord = TEXT("BYOK");
	SetupError.Empty();
	SetUseWithAura(false);
	GoToServiceStep();
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSetupSkip()
{
	CancelSignIn();
	SetupAccess.Empty();
	SetupAccountWord.Empty();
	SetupError.Empty();
	bSetupServiceShown = false;
	SetupStep = StepPrivacy;
	if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
	{
		B->RequestAccount();
	}
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSetupFinish()
{
	// (Next when the game's memory isn't known to hold something: step 4 follows, once the choices are saved)
	const bool bFirstRun = MemoryView() != EMemoryView::Something;
	const bool bWasSharing = Prefs.bShareData;
	FPlaytestPreferences Now = PlaytestPreferences::Load();
	Now.bOnboarded = true;
	Now.bShareData = bSetupShare;
	if (!SetupAccess.IsEmpty())
	{
		Now.Access = SetupAccess;
	}
	const bool bSaved = PlaytestPreferences::Save(Now);
	if (!bSaved)
	{
		SaySaveFailed();
	}
	Prefs = bSaved ? PlaytestPreferences::Load() : Now;  // (not saved: asked again next time)
	Prefs.bOnboarded = true;
	bSetupOpen = false;
	SetupStep = StepAccount;
	SetupAccess.Empty();
	SetupAccountWord.Empty();
	SetupError.Empty();
	bSetupServiceShown = false;
	bSignInWanted = false;
	bSignInSent = false;
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (B)
	{
		B->ClearSignIn();
	}
	if (B && !B->WantsConnection() && !B->IsConnected())
	{
		B->Connect();
	}
	else if (bWasSharing != Prefs.bShareData)
	{
		ReconnectIfIdle();  // (the hello says it)
	}
	if (B)
	{
		B->RequestAccount();  // (the agent service reads the choice from preferences.json)
	}
	if (bFirstRun)
	{
		// Step 4: it waits for the connection and its memory_state, and closes by itself when the memory holds something
		bSetupOpen = true;
		SetupStep = StepFirstRun;
	}
	return FReply::Handled();
}

// -- Step 4: First run -----------------------------------------------------------------------------------------------

SPlaytestPanel::EMemoryView SPlaytestPanel::MemoryView() const
{
	if (TestMemory.IsSet())
	{
		return TestMemory.GetValue();
	}
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B || !B->IsMemoryKnown())
	{
		return EMemoryView::Unknown;
	}
	// (the Memory line's "empty")
	return B->GetMemoryRuns() == 0 && !B->HasMemoryNotes() && !B->HasMemoryLib() ? EMemoryView::Empty : EMemoryView::Something;
}

void SPlaytestPanel::CloseSetup()
{
	bSetupOpen = false;
	SetupStep = StepAccount;
}

TSharedRef<FJsonObject> SPlaytestPanel::FirstRunFrame(FString& OutKind, FString& OutPrompt) const
{
	OutKind = TEXT("explore");
	OutPrompt = TEXT("Explore");
	return FAutopilotSession::ExploreFrame(Minutes, GetDefault<UPlaytestSettings>()->DesignBrief);
}

bool SPlaytestPanel::CanExploreFirst() const
{
	return IsConnected() && (TestRunning.IsSet() ? !TestRunning.GetValue() : !IsRunning());
}

FReply SPlaytestPanel::OnFirstRunExplore()
{
	if (!CanExploreFirst())
	{
		return FReply::Handled();
	}
	FString Kind;
	FString Prompt;
	const TSharedRef<FJsonObject> Frame = FirstRunFrame(Kind, Prompt);
	if (bTestStarts)
	{
		TestStartedFrame = Frame;
		CloseSetup();
		return FReply::Handled();
	}
	if (TSharedPtr<FAutopilotRun> Run = FAutopilotSession::Get().StartRun(Kind, Prompt, Frame))
	{
		CloseSetup();
		SyncRuns();
		Select(Run);  // (shown live)
	}
	return FReply::Handled();
}

FReply SPlaytestPanel::OnFirstRunSkip()
{
	CloseSetup();
	return FReply::Handled();
}

TSharedRef<SWidget> SPlaytestPanel::MakeFirstRunStep()
{
	auto Shown = [](bool bShown) { return bShown ? EVisibility::Visible : EVisibility::Collapsed; };
	auto Offered = [this]() { return IsConnected() && MemoryView() == EMemoryView::Empty; };

	// Not connected, or the memory not said yet: what it waits for (a plate, the spinner), and Skip
	TSharedRef<SWidget> Waiting = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			Card(SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(AutopilotUI::SSpinner)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					AutopilotUI::Body(TAttribute<FText>::CreateLambda([this]()
					{
						return IsConnected() ? LOCTEXT("FR_Checking", "Checking what Autopilot remembers about this game…")
							: LOCTEXT("FR_Connecting", "Connecting to the agent service…");
					}), 13.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
				])
		]
		// (why there is no connection, when there is a reason)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SBox)
			.Visibility_Lambda([this, Shown]() { return Shown(!IsConnected() && (IsUnreachable() || IsRefused() || ServiceView() == EServiceView::Problem)); })
			[
				AutopilotUI::Body(TAttribute<FText>::CreateSP(this, &SPlaytestPanel::ConnectMessage), 13.f, AutopilotColor::Red())
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f).HAlign(HAlign_Left)
		[
			AutopilotUI::Button(LOCTEXT("FR_Skip", "Skip"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnFirstRunSkip))
		];

	// The memory is empty: an Explore run, recommended (its minutes; Explore and finish, Skip and finish under a rule)
	TSharedRef<SWrapBox> Buttons = ButtonRow();
	TSharedRef<SButton> ExploreFirst = AutopilotUI::Button(LOCTEXT("FR_Explore", "Explore and finish"), "Autopilot.Button.Primary",
		FOnClicked::CreateSP(this, &SPlaytestPanel::OnFirstRunExplore), TAttribute<bool>::CreateSP(this, &SPlaytestPanel::CanExploreFirst), nullptr, false, 20.f);
	ExploreFirst->SetToolTip(AutopilotUI::Tip(TAttribute<FText>::CreateLambda([this]()
	{
		if (CanExploreFirst())
		{
			return FText::GetEmpty();
		}
		return IsConnected() ? LOCTEXT("FR_Busy", "A run is going: wait for its result, or stop it") : LOCTEXT("FR_NotConnected", "Not connected: connect first");
	})));
	AddTo(Buttons, ExploreFirst);
	AddTo(Buttons, AutopilotUI::Button(LOCTEXT("FR_SkipFinish", "Skip and finish"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnFirstRunSkip)));
	TSharedRef<SWidget> Offer = Card(SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			CardHead(LOCTEXT("FR_Card", "Explore your game"), LOCTEXT("FR_Recommended", "Recommended"), EAutopilotTone::Success)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			AutopilotUI::Body(LOCTEXT("FR_CardText", "You’ve never used Autopilot with this game before. To help Autopilot understand your game, use this first exploration run and watch it improve. Autopilot still works if you skip this step."),
				13.f, AutopilotColor::Text3())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f).HAlign(HAlign_Left)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				AutopilotUI::Label(LOCTEXT("Minutes", "Minutes"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				// (the composer's minutes: the same value)
				NumberField([this]() { return Minutes; }, [this](int32 V, bool) { Minutes = V; }, 56.f)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			Buttons
		]);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[PageTitle(LOCTEXT("FR_Title", "Let Autopilot learn your game"))]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 18.f, 0.f, 0.f)
		[
			SNew(SBox).Visibility_Lambda([Offered, Shown]() { return Shown(!Offered()); })[Waiting]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).Visibility_Lambda([Offered, Shown]() { return Shown(Offered()); })[Offer]
		];
}
// -- The Settings page's account corner ------------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeServiceLine()
{
	// One row (12px, wrapping): a 6px dot (or the spinner while it installs or starts), what it is, its address once
	// running, and what to do (Install, View logs, Open settings)
	const FPlaytestAgentService* S = ServiceOf(UPlaytestSubsystem::Get());
	FLinearColor DotColor = AutopilotColor::Muted();
	FLinearColor Color = AutopilotColor::Text3();
	bool bSpin = false;
	FString Text;
	FString Url;
	TSharedPtr<SWidget> Action;
	switch (ServiceView())
	{
	case EServiceView::Ready:
		DotColor = AutopilotColor::Success();
		Text = TEXT("Agent service: running");
		Url = ServiceUrlShown();
		break;
	case EServiceView::Installing:
		bSpin = true;
		Text = TEXT("Installing the agent service…");
		break;
	case EServiceView::Starting:
		bSpin = true;
		Text = TEXT("Starting the agent service…");
		break;
	case EServiceView::Problem:
	{
		DotColor = Color = AutopilotColor::Red();
		Text = TEXT("The agent service didn\u2019t start.");
		const FString Log = S ? S->GetLogPath() : FString();
		if (!Log.IsEmpty())
		{
			Action = SmallButton(LOCTEXT("S_OpenLog", "View logs"), "Autopilot.Button", FOnClicked::CreateLambda([Log]() { OpenLog(Log); return FReply::Handled(); }));
		}
		break;
	}
	case EServiceView::NotInstalled:
		DotColor = Color = AutopilotColor::Amber();
		Text = TEXT("Agent service: not installed");
		Action = SmallButton(LOCTEXT("S_Install", "Install"), "Autopilot.Button", FOnClicked::CreateLambda([]()
		{
			UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
			if (FPlaytestAgentService* Service = ServiceOf(Bridge))
			{
				Service->Install();
				Bridge->Connect();  // (it starts once installed)
			}
			return FReply::Handled();
		}));
		break;
	case EServiceView::NoInstaller:
		DotColor = Color = AutopilotColor::Amber();
		Text = TEXT("Agent service: not installed");
		Action = SmallButton(LOCTEXT("S_OpenSettings", "Open settings"), "Autopilot.Button", FOnClicked::CreateLambda([]() { ShowSettings(); return FReply::Handled(); }));
		break;
	case EServiceView::Manual:
		Text = TEXT("Agent service: not running");
		break;
	default:
		Text = TEXT("Agent service: starts when Autopilot connects");
		break;
	}
	TSharedRef<SWrapBox> Line = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8.f, 4.f));
	Line->AddSlot().VAlign(VAlign_Center)
	[
		bSpin ? StaticCastSharedRef<SWidget>(SNew(AutopilotUI::SSpinner)) : Dot(DotColor)
	];
	Line->AddSlot().VAlign(VAlign_Center)[AutopilotUI::Body(Str(Text), 12.f, Color)];
	if (!Url.IsEmpty())
	{
		Line->AddSlot().VAlign(VAlign_Center)[AutopilotUI::Mono(Str(Url), 10.f, FSlateColor(AutopilotColor::Muted()))];
	}
	if (Action.IsValid())
	{
		Line->AddSlot().VAlign(VAlign_Center)[Action.ToSharedRef()];
	}
	return Line;
}
FPlaytestAccount SPlaytestPanel::ShownAccount() const
{
	if (TestAccount.IsSet())
	{
		return TestAccount.GetValue();
	}
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return B ? B->GetAccount() : FPlaytestAccount();
}

FString SPlaytestPanel::AccountKey() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAgentService* S = ServiceOf(B);
	FString Key = FString::Printf(TEXT("%d|%d|%d|%d|%s|%s|%d|%d"), GetDefault<UPlaytestSettings>()->bUseWithAura ? 1 : 0, bConfirmSignOut ? 1 : 0,
		(int32)SignInView(), (int32)ServiceView(), *Prefs.Access, *ServiceUrlShown(), B && B->HasAccountFeature() ? 1 : 0, S && S->IsOurs() ? 1 : 0);
	if (B || TestAccount.IsSet())
	{
		const FPlaytestAccount A = ShownAccount();
		Key += FString::Printf(TEXT("|%d|%s|%s|%d|%s|%s|%s"), A.bKnown ? 1 : 0, *A.Mode, *A.Email, A.bSignedIn ? 1 : 0, *A.DecisionKey, *A.LlmKey,
			B ? *B->GetSignIn().Url : TEXT(""));
		Key += TEXT("|") + A.CodingAgent + TEXT("|") + A.LlmProvider;
		Key += FString::Printf(TEXT("|%d|%.4f|%d|%s|%s"), A.Bill.bKnown ? 1 : 0, A.Bill.Usd, A.Bill.Runs, *A.Bill.From, *A.Bill.To);
		Key += TEXT("|") + A.OutOfCredits + TEXT("|") + A.AddCreditsUrl;
	}
	if (S)
	{
		Key += TEXT("|") + S->GetProblem();
	}
	if (bSignInWanted || bSignInSent)
	{
		Key += TEXT("|") + SignInLine();
	}
	return Key;
}

void SPlaytestPanel::RebuildAccount()
{
	if (!AccountBox.IsValid())
	{
		return;
	}
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	const FPlaytestAccount Account = ShownAccount();
	const EPlaytestAccountKind Kind = PlaytestAccount::UsageKind(Account, Prefs.Access, GetDefault<UPlaytestSettings>()->bUseWithAura);

	// Rerun setup: the setup again, from its first step (one button, first in the row)
	SettingsRerunSetup.Reset();
	auto RerunSetup = [this](const FName& Style)
	{
		TSharedRef<SButton> Button = MakeRerunSetup(Style);
		SettingsRerunSetup = Button;
		return Button;
	};
	FText TagText;
	EAutopilotTone Tone = EAutopilotTone::Success;
	FLinearColor Outline = AutopilotColor::Rule();
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	auto Add = [&Body](const TSharedRef<SWidget>& Widget)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Widget];
	};
	auto Title = [&Add](const FText& Text) { Add(AutopilotUI::Body(Text, 15.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)); };
	auto Line = [&Add](const FText& Text, float Px, const FLinearColor& Color) { Add(AutopilotUI::Body(Text, Px, Color)); };
	// What Autopilot billed the Aura account this billing period, its days in it (nothing when Aura couldn't say):
	// "Billing period (Oct 1 – Oct 31): $12.34 USD", the sum in mono
	const bool bCompactPage = IsCompact();
	auto PeriodBill = [&Add, &Account, bCompactPage]()
	{
		if (!Account.Bill.bKnown)
		{
			return;
		}
		const FString Dates = PlaytestAccount::PeriodDates(Account.Bill);
		const FText Period = Str(Dates.IsEmpty() ? FString(TEXT("Billing period:")) : FString::Printf(TEXT("Billing period (%s):"), *Dates));
		const FText Sum = Str(PlaytestAccount::Money(Account.Bill.Usd) + TEXT(" USD"));
		if (bCompactPage)
		{
			Add(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Body(Period, 13.f, AutopilotColor::Text2())]
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Sum).Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 13.f)).ColorAndOpacity(FSlateColor(AutopilotColor::Text2())).AutoWrapText(true)]);
		}
		else
		{
			Add(SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)[AutopilotUI::Body(Period, 13.f, AutopilotColor::Text2(), false)]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[AutopilotUI::Mono(Sum, 13.f, FSlateColor(AutopilotColor::Text2()))]);
		}
	};
	TSharedRef<SWrapBox> Row = ButtonRow();
	// Aura's words while the team has no credits left for a run, red, under the bill; Add credits first in the row
	auto OutOfCredits = [&Line, &Row, &Account]()
	{
		if (Account.OutOfCredits.IsEmpty())
		{
			return;
		}
		Line(Str(Account.OutOfCredits), 13.f, AutopilotColor::Red());
		if (!Account.AddCreditsUrl.IsEmpty())
		{
			const FString Url = Account.AddCreditsUrl;
			AddTo(Row, SmallButton(LOCTEXT("AddCredits", "Add credits"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([Url]()
			{
				FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
				return FReply::Handled();
			})));
		}
	};
	const ESignInView SignIn = SignInView();
	const bool bSigningIn = SignIn == ESignInView::Preparing || SignIn == ESignInView::Waiting || SignIn == ESignInView::Error;
	bool bServiceLine = true;  // (under the account in every mode but the Aura app's)
	bool bRuled = true;        // (the buttons under a rule; a confirmation's without)
	switch (Kind)
	{
	case EPlaytestAccountKind::Aura:
		TagText = LOCTEXT("A_Aura", "Aura");
		if (bConfirmSignOut)
		{
			bServiceLine = false;
			bRuled = false;
			Outline = AutopilotColor::RedLine();
			Title(Account.Email.IsEmpty() ? LOCTEXT("A_SignOutAskAnon", "Sign out of Aura on this computer?")
				: FText::Format(LOCTEXT("A_SignOutAsk", "Sign out {0} on this computer?"), Str(Account.Email)));
			Line(LOCTEXT("A_SignOutWhat", "Every editor on this computer stops using Aura until you sign in again."), 13.f, AutopilotColor::Text3());
			AddTo(Row, SmallButton(LOCTEXT("A_SignOut", "Sign out"), "Autopilot.Button.DangerFill", FOnClicked::CreateLambda([this]()
			{
				if (UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get())
				{
					Bridge->SignOutOfAura();  // (and asks for the account again)
				}
				bConfirmSignOut = false;
				return FReply::Handled();
			})));
			AddTo(Row, SmallButton(LOCTEXT("S_Cancel", "Cancel"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
			{
				bConfirmSignOut = false;
				return FReply::Handled();
			})));
			break;
		}
		Title(Account.Email.IsEmpty() ? LOCTEXT("A_AuraNoEmail", "Aura") : Str(Account.Email));
		PeriodBill();
		OutOfCredits();
		AddTo(Row, RerunSetup("Autopilot.Button"));
		{
			const bool bCan = B && B->HasAccountFeature();
			TSharedRef<SButton> SignOut = AutopilotUI::Button(LOCTEXT("A_SignOut", "Sign out"), "Autopilot.Button.Danger", FOnClicked::CreateLambda([this]()
			{
				bConfirmSignOut = true;
				return FReply::Handled();
			}), bCan, nullptr, true);
			if (!bCan)
			{
				SignOut->SetToolTip(AutopilotUI::Tip(LOCTEXT("A_SignOutConnect", "Connect to the agent service to sign out")));
			}
			AddTo(Row, SignOut);
		}
		break;
	case EPlaytestAccountKind::AuraApp:
		bServiceLine = false;
		TagText = LOCTEXT("A_AuraApp", "Aura app");
		Title(LOCTEXT("A_AppService", "The Aura app’s agent service"));
		if (Account.bKnown && Account.Mode == TEXT("aura") && Account.bSignedIn)
		{
			PeriodBill();  // (the Aura app's agent service says it the same way)
			OutOfCredits();
		}
		AddTo(Row, RerunSetup("Autopilot.Button"));
		break;
	case EPlaytestAccountKind::OwnKeys:
	{
		TagText = LOCTEXT("A_Keys", "BYOK");
		Tone = EAutopilotTone::Neutral;
		Title(LOCTEXT("A_OwnKeys", "Your own keys"));
		const FString Saved = PlaytestAccount::SavedKeysLine(Account);
		if (!Saved.IsEmpty())
		{
			Add(SNew(STextBlock)
				.Text(Str(Saved))
				.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f))
				.ColorAndOpacity(FSlateColor(AutopilotColor::Text3()))
				.AutoWrapText(true));
		}
		AddTo(Row, RerunSetup("Autopilot.Button"));
		break;
	}
	case EPlaytestAccountKind::SignedOut:
		TagText = LOCTEXT("A_SignedOut", "Signed out");
		Tone = EAutopilotTone::Red;
		if (!Account.Email.IsEmpty())
		{
			Title(Str(Account.Email));
		}
		Line(LOCTEXT("A_Ended", "Your Aura sign-in on this computer ended."), 13.f, AutopilotColor::Red());
		if (bSigningIn)
		{
			Add(MakeSignInBody(true));
		}
		else
		{
			AddTo(Row, SmallButton(LOCTEXT("A_SignInAgain", "Sign in again"), "Autopilot.Button.Primary", FOnClicked::CreateLambda([this]()
			{
				BeginSignIn(false);  // (here, in place)
				return FReply::Handled();
			})));
			AddTo(Row, RerunSetup("Autopilot.Button.Quiet"));
		}
		break;
	case EPlaytestAccountKind::NotSetUp:
	default:
		TagText = LOCTEXT("A_NotSetUp", "Not set up");
		Tone = EAutopilotTone::Amber;
		Title(LOCTEXT("A_Choose", "Choose how to run Autopilot"));
		AddTo(Row, RerunSetup("Autopilot.Button.Primary"));
		break;
	}
	if (bServiceLine)
	{
		Add(MakeServiceLine());
	}
	if (Row->GetChildren()->Num())
	{
		if (bRuled)
		{
			Add(AutopilotUI::Rule());
		}
		Body->AddSlot().AutoHeight().Padding(0.f, bRuled ? 10.f : 12.f, 0.f, 0.f)[Row];
	}
	AccountBox->SetContent(Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			CardHead(LOCTEXT("A_Account", "Account"), TagText, Tone)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Body
		], Outline));
}
// -- Remove Autopilot from this computer -----------------------------------------------------------------------------

TSharedRef<SWidget> SPlaytestPanel::MakeRemoveSection()
{
	auto Asking = [this]() { return bConfirmRemove ? EVisibility::Visible : EVisibility::Collapsed; };
	auto NotAsking = [this]() { return bConfirmRemove ? EVisibility::Collapsed : EVisibility::Visible; };
	// (not while a run goes: removing ends its connection)
	TSharedRef<SButton> Ask = AutopilotUI::Button(LOCTEXT("R_Ask", "Uninstall"), "Autopilot.Button.Danger", FOnClicked::CreateLambda([this]()
	{
		if (CanChangeAccount())
		{
			bConfirmRemove = true;
			bRemoveDeletesMemory = false;
		}
		return FReply::Handled();
	}), TAttribute<bool>::CreateSP(this, &SPlaytestPanel::CanChangeAccount), nullptr, true);
	Ask->SetToolTip(AutopilotUI::Tip(TAttribute<FText>::CreateLambda([this]()
	{
		return CanChangeAccount() ? FText::GetEmpty() : LOCTEXT("RunBusy", "Finish or stop the run first");
	})));
	RemoveAskButton = Ask;
	Ask->SetTag(TEXT("Autopilot.Uninstall.Ask"));  // (the tests' name for it: never the confirmation's)
	TSharedRef<SWrapBox> ConfirmRow = ButtonRow();
	TSharedRef<SButton> Confirm = AutopilotUI::Button(LOCTEXT("R_Remove", "Uninstall"), "Autopilot.Button.DangerFill", FOnClicked::CreateSP(this, &SPlaytestPanel::OnRemove),
		TAttribute<bool>::CreateSP(this, &SPlaytestPanel::CanChangeAccount), nullptr, true);
	Confirm->SetTag(TEXT("Autopilot.Uninstall.Confirm"));  // (the one that uninstalls: tests never press it)
	AddTo(ConfirmRow, Confirm);
	AddTo(ConfirmRow, SmallButton(LOCTEXT("S_Cancel", "Cancel"), "Autopilot.Button.Quiet", FOnClicked::CreateLambda([this]()
	{
		bConfirmRemove = false;
		return FReply::Handled();
	})));
	// (a check box gives its words all the width they want: they wrap at the page's, less its margins and the box)
	TSharedRef<STextBlock> DeleteText = AutopilotUI::Body(LOCTEXT("R_Delete", "[Optional] Delete Autopilot’s memories for all projects in this machine across all editors. I understand that this action is destructive and deletes Autopilot’s memories for my entire team."),
		12.5f, AutopilotColor::Text2());
	DeleteText->SetWrapTextAt(TAttribute<float>::CreateLambda([this]() { return FMath::Max(120.f, FMath::Min(SettingsMax - 40.f, MainWidth() - 40.f) - 28.f - 24.f); }));
	return SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([this]() { return bConfirmRemove ? 1 : 0; })
		// Not asked: its name and the button, on one row (the button under it when narrow)
		+ SWidgetSwitcher::Slot()
		[
			Card(SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(10.f, 6.f))
				.Visibility_Lambda(NotAsking)
				+ SWrapBox::Slot().VAlign(VAlign_Center)
				[
					AutopilotUI::Label(LOCTEXT("R_Uninstall", "Uninstall Autopilot"))
				]
				+ SWrapBox::Slot().FillEmptySpace(true).HAlign(HAlign_Right).VAlign(VAlign_Center)
				[
					Ask
				])
		]
		// Asked, in place (a red edge): what it removes, Delete Autopilot's memories (unticked: kept), Remove or Cancel
		+ SWidgetSwitcher::Slot()
		[
			Card(SNew(SVerticalBox)
				.Visibility_Lambda(Asking)
				+ SVerticalBox::Slot().AutoHeight()
				[
					AutopilotUI::Label(LOCTEXT("R_Uninstall", "Uninstall Autopilot"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					AutopilotUI::Body(LOCTEXT("R_AskText", "Uninstall Autopilot from this computer?"), 15.f, AutopilotColor::Text(), true, EAutopilotFont::BodyMedium)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					AutopilotUI::Body(LOCTEXT("R_What", "Removes the agent service, your keys, your Aura sign-in and your setup choices, for every project."),
						12.f, AutopilotColor::Text3())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f).HAlign(HAlign_Left)
				[
					AutopilotUI::Check([this]() { return bRemoveDeletesMemory; }, [this](bool bOn) { bRemoveDeletesMemory = bOn; }, DeleteText)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					ConfirmRow
				], AutopilotColor::RedLine())
		];
}

EPlaytestUninstallState SPlaytestPanel::UninstallShown() const
{
	if (TestUninstall.IsSet())
	{
		return TestUninstall.GetValue();
	}
	const FPlaytestAgentService* S = ServiceOf(UPlaytestSubsystem::Get());
	return S && S->IsRemoved() ? S->GetUninstallState() : EPlaytestUninstallState::None;
}

TSharedRef<SWidget> SPlaytestPanel::MakeRemoved()
{
	return SNew(SScrollBox)
	+ SScrollBox::Slot()
	[
		Capped(SAssignNew(RemovedBox, AutopilotUI::SSettle), ConnectMax, FMargin(20.f, 24.f))
	];
}

void SPlaytestPanel::RebuildRemoved()
{
	const EPlaytestUninstallState State = UninstallShown();
	RemovedShown = (int32)State;
	if (!RemovedBox.IsValid())
	{
		return;
	}
	// A plate: the logo and AUTOPILOT, then what it does or did, and the buttons under a rule
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(SImage).Image(FAutopilotStyle::Brush("Autopilot.Icon.Large"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				AutopilotUI::Label(LOCTEXT("R_Brand", "Autopilot"), FSlateColor(AutopilotColor::Text2()))
			]
		];
	auto Add = [&Body](const TSharedRef<SWidget>& Widget) { Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[Widget]; };
	auto Title = [&Add](const FText& Text, const FLinearColor& Color = AutopilotColor::Text())
	{
		Add(AutopilotUI::Body(Text, 15.f, Color, true, EAutopilotFont::BodyMedium));
	};
	const FOnClicked OpenTheLog = FOnClicked::CreateLambda([]()
	{
		if (const FPlaytestAgentService* S = ServiceOf(UPlaytestSubsystem::Get()))
		{
			OpenLog(S->GetUninstallLog());
		}
		return FReply::Handled();
	});
	// (what is left to remove by hand: the plugin, in each project)
	auto Next = [&Add]()
	{
		Add(AutopilotUI::Body(LOCTEXT("R_Next", "To finish, close the editor and delete Plugins/Autopilot/ from each project that has it."), 12.f, AutopilotColor::Text3()));
	};
	TSharedRef<SWrapBox> Row = ButtonRow();
	auto Button = [&Row](const FText& Text, const FName& Style, FOnClicked OnClicked) { AddTo(Row, AutopilotUI::Button(Text, Style, OnClicked)); };
	bool bButtons = true;
	switch (State)
	{
	case EPlaytestUninstallState::Removing:
		Add(Progress(TEXT("Uninstalling Autopilot from this computer…")));
		bButtons = false;
		break;
	case EPlaytestUninstallState::Removed:
		Title(LOCTEXT("R_Done", "Uninstallation successful"));
		Next();
		Button(LOCTEXT("R_SetUpAgain", "Set up again"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetUpAgain));
		Button(LOCTEXT("R_OpenLog", "View logs"), "Autopilot.Button.Quiet", OpenTheLog);
		break;
	case EPlaytestUninstallState::RemovedButSome:
		Title(LOCTEXT("R_Done", "Uninstallation successful"));
		Next();
		Button(LOCTEXT("R_OpenLog", "View logs"), "Autopilot.Button.Primary", OpenTheLog);
		Button(LOCTEXT("R_SetUpAgain", "Set up again"), "Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestPanel::OnSetUpAgain));
		break;
	case EPlaytestUninstallState::Failed:
	default:
		Title(LOCTEXT("R_Failed", "Couldn’t uninstall Autopilot from this computer."), AutopilotColor::Red());
		Button(LOCTEXT("R_OpenLog", "View logs"), "Autopilot.Button", OpenTheLog);
		Button(LOCTEXT("R_TryAgain", "Try again"), "Autopilot.Button.Primary", FOnClicked::CreateSP(this, &SPlaytestPanel::OnRemoveTryAgain));
		Button(LOCTEXT("S_Cancel", "Cancel"), "Autopilot.Button.Quiet", FOnClicked::CreateSP(this, &SPlaytestPanel::OnRemoveCancel));
		break;
	}
	if (bButtons)
	{
		Add(AutopilotUI::Rule());
		Add(Row);
	}
	RemovedBox->SetContent(Card(Body));
}
FReply SPlaytestPanel::OnRemove()
{
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	FPlaytestAgentService* S = ServiceOf(B);
	// (never for real from an automation test: it would uninstall this computer's agent service and its sign-in, and
	// with the box ticked delete the memories kept with the team)
	if (!S || !CanChangeAccount() || GIsAutomationTesting)
	{
		return FReply::Handled();
	}
	bConfirmRemove = false;
	CancelSignIn();
	bSetupOpen = false;
	B->Disconnect();  // (and nothing connects again until Set up again: UPlaytestSubsystem::Connect)
	if (bRemoveDeletesMemory)
	{
		SelectedId = INDEX_NONE;
		FAutopilotSession::Get().Forget();
	}
	S->Uninstall(!bRemoveDeletesMemory);
	return FReply::Handled();
}

FReply SPlaytestPanel::OnRemoveTryAgain()
{
	FPlaytestAgentService* S = ServiceOf(UPlaytestSubsystem::Get());
	if (S && !GIsAutomationTesting)  // (never for real from an automation test)
	{
		S->Uninstall(S->GetUninstallKeepsMemory());
	}
	return FReply::Handled();
}

FReply SPlaytestPanel::OnSetUpAgain()
{
	TestUninstall.Reset();
	if (FPlaytestAgentService* S = ServiceOf(UPlaytestSubsystem::Get()))
	{
		S->LeaveRemoved();
	}
	Prefs = PlaytestPreferences::Load();  // (gone with the rest: the setup from its first step)
	bSettingsPage = false;
	OpenSetup();
	return FReply::Handled();
}

FReply SPlaytestPanel::OnRemoveCancel()
{
	TestUninstall.Reset();
	if (FPlaytestAgentService* S = ServiceOf(UPlaytestSubsystem::Get()))
	{
		S->LeaveRemoved();
	}
	Prefs = PlaytestPreferences::Load();  // (the setup shows if its choices went)
	bSetupOpen = false;
	return FReply::Handled();
}

void SPlaytestPanel::CheckServiceMissing()
{
	static bool bChecked = false;  // (once an editor session)
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	FPlaytestAgentService* S = ServiceOf(B);
	if (bChecked || GIsAutomationTesting || FApp::IsUnattended() || !S || !Prefs.bOnboarded || bSetupOpen || S->IsRemoved()
		|| !B->ManagesAgentService() || B->IsConnected())
	{
		return;
	}
	bChecked = true;
	if (S->IsMissing())
	{
		// The setup's Agent service step: Install (a release download), or how to set it up (a repository checkout)
		OpenSetup();
		GoToServiceStep();
	}
}

#undef LOCTEXT_NAMESPACE
