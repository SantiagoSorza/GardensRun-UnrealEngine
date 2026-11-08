#pragma once

#include "CoreMinimal.h"
#include "PlaytestAgentService.h"
#include "PlaytestPreferences.h"
#include "AutopilotSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SBox;
class SButton;
class SEditableTextBox;
class SMenuAnchor;
class SMultiLineEditableTextBox;
class SPlaytestPolicyLog;
class SPlaytestVideo;
class STextBlock;
class SVerticalBox;
namespace AutopilotUI { class SSettle; }

/**
 * The editor's Autopilot tab (The Flatness look: AutopilotStyle). Its history (the runs saved for this project, newest
 * first; a list under a button when the panel is narrow) beside the run picked:
 *   - not connected: the connect card (the agent service: connecting by itself, installing or starting it, or why there
 *     is no connection: it refused it, can't be reached, or Disconnect was pressed);
 *   - New run: the composer, one box whose words say the kind of run (Verify a claim, Explore, Play a goal: runs are real
 *     time);
 *   - a run going: its stages, what it does now, its attempts and its feed (every status line, engine findings among them);
 *   - a run done: its result (the verdict, its recording, the readouts, the evidence, the findings with Show in level and
 *     Not a bug);
 *   - its policy log (SPlaytestPolicyLog), when the agent service sent its policy steps;
 *   - Settings: the account, this project's runs (what Aura billed for each), the time limit, and whether runs' data is
 *     shared.
 * Until the first-run setup is done (preferences.json: onboarded), the setup takes the tab's place: 1 the account (Aura
 * through the browser sign-in the agent service does, the Aura app, or the user's own keys), 2 the agent service (the
 * editor installs and starts it: FPlaytestAgentService), 3 privacy settings (whether to share runs' data), and 4 a first
 * run when the game's memory is empty (an Explore run, recommended and skippable; skipped by itself when the memory holds
 * something). The Settings
 * page (the gear beside New run) starts with the account: how runs go, the agent service, Rerun setup, Sign out. It ends
 * with Remove Autopilot (from this computer): while that runs and once it has, what it did takes the tab's place, the
 * setup's too, until Set up again. Once set up, the tab connects by itself (not after Disconnect, until Reconnect).
 * What it shows is kept by FAutopilotSession and saved to disk, so closing the tab or the editor loses nothing.
 */
class SPlaytestPanel : public SCompoundWidget
{
	friend class FAutopilotPanelLayoutTest;  // (lays every page out at several sizes: AutopilotPanelTests.cpp)
	friend class FAutopilotLlmProviderTest;  // (the setup's LLM provider dropdown: AutopilotPanelTests.cpp)
	friend class FAutopilotCodingAgentTest;  // (the setup's Coding Agent dropdown: AutopilotPanelTests.cpp)
	friend class FAutopilotRerunSetupTest;   // (Rerun setup on the Settings page, the gear: AutopilotPanelTests.cpp)
	friend class FAutopilotConnectTest;      // (the connect card: connecting by itself, Disconnect and Reconnect: AutopilotPanelTests.cpp)
	friend class FAutopilotFirstRunTest;     // (the setup's Aura card and its step 4, First run: AutopilotPanelTests.cpp)
	friend class FAutopilotStatusLineTest;   // (the status line, the steer boxes, Back to the last run: AutopilotPanelTests.cpp)
	friend class FAutopilotRemoveTest;       // (Remove Autopilot on the Settings page, and what the tab shows after: AutopilotPanelTests.cpp)
	friend class FAutopilotRunStartedTest;   // (a run the user's coding agent started; the Claude Code and Codex card: AutopilotPanelTests.cpp)
	friend class FAutopilotSettingCardsTest; // (the Settings page's Time limit, sharing and Claude Code & Codex plates: AutopilotPanelTests.cpp)
	friend class FAutopilotActivityTest;     // (the activity feed: the Feed tab's stage groups, folds; the Live page's lines: AutopilotPanelTests.cpp)
public:
	SLATE_BEGIN_ARGS(SPlaytestPanel) {}
		/** The folder of autopilot.env, which the setup's keys go to ("": the user settings folder; the layout test's own). */
		SLATE_ARGUMENT(FString, KeysDir)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	/** The tab's right-click menu (as Unity's window menu): Connect or Disconnect, Settings, Rerun setup, Reset memory…,
	 *  doing what the tab's own buttons do. */
	void ExtendTabMenu(class FMenuBuilder& Menu);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	enum EView : int32 { ViewConnect = 0, ViewComposer, ViewLive, ViewResult, ViewFeed, ViewPolicy, ViewSettings };
	enum ETab : int32 { TabMain = 0, TabFeed, TabPolicy };

	// Layout
	TSharedRef<SWidget> MakeRail();
	TSharedRef<SWidget> MakeNarrowBar();
	/** The memory row (bPopup: the narrow history list's, a little tighter), Reset memory asked in place. */
	TSharedRef<SWidget> MakeSaveData(bool bPopup = false);
	/** The banner over a run shown while there is no connection: Reconnecting… (Details), or Not connected (Reconnect). */
	TSharedRef<SWidget> MakeNotice();
	/** Over the composer while Aura says the team is out of credits: Aura's words, and Add credits when it names where. */
	TSharedRef<SWidget> MakeOutOfCredits();
	/** The status line: the main column's last row, what just happened (FAutopilotSession::Say), hidden when nothing. */
	TSharedRef<SWidget> MakeStatusLine();
	/** "Couldn't save the setup to <path> (error <n>)" on the status line. */
	void SaySaveFailed();
	/** The steer box and Steer (bFeed: the Feed tab's, else the Live page's): the two say the same words. */
	TSharedRef<SWidget> MakeSteerBar(bool bFeed);
	/** The words in the steer boxes. */
	FString SteerText() const;
	TSharedRef<SWidget> MakeTopBar();
	/** The gear beside New run (the rail's, the narrow bar's): opens the Settings page, or closes it (accent-filled while open). */
	TSharedRef<SWidget> MakeSettingsButton(bool bSmall);
	void ToggleSettings();
	/** "Rerun setup" (the Settings page's): opens the first-run setup at its first step; disabled while a run goes. */
	TSharedRef<SButton> MakeRerunSetup(const FName& Style);
	FText RerunSetupTip() const;
	/** The account can change (not while a run goes: switching would end its connection). */
	bool CanChangeAccount() const;
	TSharedRef<SWidget> MakeConnectCard();
	/** The connect card's Advanced (folded): the agent service's URL, its access token, its command. */
	TSharedRef<SWidget> MakeAdvanced();
	bool bAdvancedOpen = false;
	/** The connect card's state: what it says and offers. */
	enum class EConnectView : uint8 { NoBridge, Connecting, Disconnected, Starting, Installing, NotInstalled, NoInstaller, Unreachable, Failed, Refused, Aura };
	EConnectView ConnectView() const;
	TOptional<EConnectView> TestConnectView;  // (the tests': the connect card's state, in place of the subsystem's)
	/** The connect card again (when what it shows changes: ConnectKey). */
	void RebuildConnect();
	FString ConnectKey() const;
	/** Connect now when set up and not connected (or connecting): not after the user's Disconnect, nor while a refusal stands. */
	void AutoConnect();
	TSharedRef<SWidget> MakeComposer();
	TSharedRef<SWidget> MakeLive();
	TSharedRef<SWidget> MakeStageTrack();
	/** The stage bar's width: the Live page's, less its margins. */
	float StageTrackWidth() const;
	/** A feed's list: Source's lines (none: the Feed tab's, FeedItems: the run's lines under their stages' headings). bCompact:
	 *  the Live page's (one line each, no folds). */
	TSharedRef<SWidget> MakeFeedList(TSharedPtr<SListView<TSharedPtr<FAutopilotFeedLine>>>& View, TArray<TSharedPtr<FAutopilotFeedLine>>* Source = nullptr,
		bool bCompact = false);
	/** The Feed tab's items for a run's feed: its lines in order, a heading (Kind Heading: the stage's name, Attempt its
	 *  count of lines) before each stretch of lines of one stage (lines before any stage have none). */
	static void GroupFeed(const TArray<TSharedPtr<FAutopilotFeedLine>>& Feed, TArray<TSharedPtr<FAutopilotFeedLine>>& Out);
	TSharedRef<SWidget> MakeVerdictBanner(const TSharedPtr<FAutopilotRun>& Run);
	TSharedRef<SWidget> MakeAgentCard(const TSharedPtr<FAutopilotRun>& Run, const FString& Note, const TSharedPtr<FAutopilotFinding>& Finding);
	TSharedRef<SWidget> MakeEngineCard(const TSharedPtr<FAutopilotRun>& Run, const TSharedPtr<FAutopilotFinding>& Finding);
	TSharedRef<SWidget> MakeFindingButtons(const TSharedPtr<FAutopilotRun>& Run, const TSharedPtr<FAutopilotFinding>& Finding);
	TSharedRef<SWidget> MakeRunTag(const TSharedPtr<FAutopilotRun>& Run);
	/** The run picked (or Shown: the layout test's) in the result view. */
	void RebuildResult(TSharedPtr<FAutopilotRun> Shown = nullptr);

	// The Settings page
	TSharedRef<SWidget> MakeSettings();
	void RebuildSettings();
	TSharedRef<SWidget> MakeUsageRunRow(const TSharedPtr<FAutopilotRun>& Run);
	TSharedRef<SWidget> MakeDataSharing();
	/** The card "Research agent": whether runs have the brain's background reader of the game's code (preferences.json
	 *  research_agent, said in every hello). */
	TSharedRef<SWidget> MakeResearchAgent();
	/** What the research agent's switch shows: the user's choice, else what the agent service does without one (off in
	 *  the Aura app's, which Use with Aura reaches; on in the user's own, signed in to Aura or on their own keys). */
	bool ResearchShown() const;
	/** The card "Autopilot in Claude Code & Codex": running Autopilot from the user's own coding agent (autopilot
	 *  connect), each command with Copy. */
	TSharedRef<SWidget> MakeCodingAgents();
	/** The Settings page's account corner (rebuilt when what it shows changes: AccountKey). */
	void RebuildAccount();
	FString AccountKey() const;
	/** The agent service's account as the card shows it (the layout test's TestAccount when set). */
	FPlaytestAccount ShownAccount() const;
	TSharedRef<SWidget> MakeServiceLine();
	/** The Settings page's last section: Uninstall, asked in place (Delete Autopilot's memories). */
	TSharedRef<SWidget> MakeRemoveSection();

	// Removing Autopilot from this computer (FPlaytestAgentService::Uninstall): what the tab shows, in place of the rest
	// (and of the setup), until Set up again or the editor restarts
	EPlaytestUninstallState UninstallShown() const;
	bool IsRemovedShown() const { return UninstallShown() != EPlaytestUninstallState::None; }
	TSharedRef<SWidget> MakeRemoved();
	void RebuildRemoved();
	FReply OnRemove();
	FReply OnRemoveTryAgain();
	FReply OnSetUpAgain();
	FReply OnRemoveCancel();
	/** Set up before, but no agent service is on this computer now (removed by hand): the setup at its Agent service
	 *  step, in place of the connect card. Once an editor session, when the tab opens. */
	void CheckServiceMissing();
	void SetShareData(bool bShare);
	void SetResearchAgent(bool bOn);

	// The first-run setup
	enum ESetupStep : int32 { StepAccount = 0, StepService = 1, StepPrivacy = 2, StepFirstRun = 3 };
	TSharedRef<SWidget> MakeSetup();
	/** What the agent service remembers about this game (its memory_state): not said yet, nothing (no runs, notes or lib), or something. */
	enum class EMemoryView : uint8 { Unknown, Empty, Something };
	EMemoryView MemoryView() const;
	/** Step 4, First run: an Explore run offered when the game's memory is empty (waits for the connection and its memory_state). */
	TSharedRef<SWidget> MakeFirstRunStep();
	/** The setup closed: the tab's usual page. */
	void CloseSetup();
	/** The Explore run step 4 starts: an exploration asked for as one, with Minutes of play. */
	TSharedRef<FJsonObject> FirstRunFrame(FString& OutKind, FString& OutPrompt) const;
	bool CanExploreFirst() const;
	FReply OnFirstRunExplore();
	FReply OnFirstRunSkip();
	TSharedRef<SWidget> MakeStepper();
	TSharedRef<SWidget> MakeKeysCard();
	/** The Coding Agent the keys card's dropdown shows: the one picked, else PlaytestAccount::PickAgent's. */
	FString SetupCodingAgent() const;
	/**
	 * The LLM provider the keys card's dropdown shows: the one the agent takes (Claude Code, Codex: locked), else for
	 * OpenCode the one picked for it, else PlaytestAccount::PickProvider's.
	 */
	FString SetupLlmProvider() const;
	/** The parts of the setup rebuilt when what they show changes (SetupKey): step 1's Aura card, step 2. */
	void RebuildSetup();
	FString SetupKey() const;
	TSharedRef<SWidget> MakeAuraCard();
	TSharedRef<SWidget> MakeServiceStep();
	FText SetupRecapAccount() const;
	FText SetupRecapService() const;
	bool IsSetupShown() const { return bSetupOpen || !Prefs.bOnboarded; }
	void OpenSetup();
	void GoToServiceStep();
	FReply OnSetupAuraApp();
	FReply OnSetupSignIn();
	FReply OnSetupCreateAccount();
	FReply OnSetupKeys();
	FReply OnSetupSkip();
	FReply OnSetupFinish();

	// Sign in with Aura: done by the agent service, asked for by the setup's step 1 and the Settings page
	enum class ESignInView : uint8 { None, Preparing, Waiting, SignedIn, Error };
	ESignInView SignInView() const;
	/** Sent and not answered for good, the agent service lost since: it's asked again once back (UPlaytestSubsystem). */
	bool IsSignInAway() const;
	/** The progress line (Preparing) or the error line (Error). */
	FString SignInLine() const;
	/** The sign-in's state in widgets (Preparing, Waiting, Error): for the Aura card and the Settings page. */
	TSharedRef<SWidget> MakeSignInBody(bool bCompact);
	void BeginSignIn(bool bSignOutFirst);
	void CancelSignIn();
	void TickSignIn();

	// The agent service as the tab shows it
	enum class EServiceView : uint8 { Ready, Installing, Starting, NotInstalled, NoInstaller, Problem, Manual, Off };
	EServiceView ServiceView() const;
	FString ServiceUrlShown() const;

	/** A run in the history (bPopup: the narrow history list's: no stripes). */
	TSharedRef<ITableRow> MakeRunRow(TSharedPtr<FAutopilotRun> Run, const TSharedRef<STableViewBase>& Owner, bool bPopup);
	/** A line of the Feed tab: its time, its mark or tag, its words (an activity event: then its result, quieter, and a
	 *  fold for each detail); a stage's heading. */
	TSharedRef<ITableRow> MakeFeedRow(TSharedPtr<FAutopilotFeedLine> Line, const TSharedRef<STableViewBase>& Owner);
	/** A line of the Live page's feed: the same, one line, no folds. */
	TSharedRef<ITableRow> MakeLiveFeedRow(TSharedPtr<FAutopilotFeedLine> Line, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> FeedRow(TSharedPtr<FAutopilotFeedLine> Line, const TSharedRef<STableViewBase>& Owner, bool bCompact);
	/** An activity line's words and, after them, its result's (quieter), one wrapping block. */
	TSharedRef<SWidget> MakeActivityWords(const TSharedPtr<FAutopilotFeedLine>& Line);
	/** A fold under an activity line ("+ the code"): opens a mono block with Detail, at most 320 px tall, scrollable. */
	TSharedRef<SWidget> MakeFold(const TSharedPtr<FAutopilotFeedLine>& Line, const FString& Label, const FString& Detail, bool bResult);
	/** An activity line's kind icon (FAutopilotStyle::ActivityIcon, 12 px in Color), or its letter when the editor has no brush. */
	static TSharedRef<SWidget> MakeActivityIcon(const FAutopilotFeedLine& Line, const FLinearColor& Color);
	/** The key of a fold's open state: the run's, the event's, call or result. */
	int64 FoldKey(const FAutopilotFeedLine& Line, bool bResult) const;

	// State
	TSharedPtr<FAutopilotRun> Selected() const;
	void Select(const TSharedPtr<FAutopilotRun>& Run);
	int32 ViewIndex() const;
	bool IsConnected() const;
	bool IsConnecting() const;
	bool IsUnreachable() const;
	bool IsRefused() const;
	bool IsRunning() const;
	bool CanRun() const;
	/** The connect card's line for its state ("" for none: Disconnected). */
	FText ConnectMessage() const;
	FText LinkText() const;
	FSlateColor LinkColor() const;
	FString PromptText() const;
	void SyncRuns();
	void SyncFeed();

	// Actions
	FReply OnRun();
	FReply OnRunAgain(TSharedPtr<FAutopilotRun> Run);
	FReply OnStop();
	/** Stop pressed for the run picked, and the agent service said it is stopping (Stopping…). */
	bool IsStopping() const;
	FReply OnSteer();
	FReply OnSteerKey(const FGeometry& Geometry, const FKeyEvent& Key);
	/** The steer row shows: the run picked is going, connected, and the agent service takes steer (TestSteers: the layout test's). */
	bool IsSteerShown() const;
	/** And it can be used now: that run is at its Rollout (FAutopilotSession::SteersAt). Else greyed, its words kept. */
	bool IsSteerOpen() const;
	FReply OnDeleteSave();
	FReply OnShowInLevel(TSharedPtr<FAutopilotFinding> Finding);
	FReply OnVerifyFix(FString Note);
	FReply OnCopy(FString Text);
	FReply OnRunFolder(TSharedPtr<FAutopilotRun> Run);

	int32 Minutes = 10;     // the first-run Explore card's minutes of play
	int32 TimeLimit = 30;   // the composer's Time limit
	int32 SelectedId = INDEX_NONE;  // INDEX_NONE: New run (the composer)
	int32 Tab = TabMain;
	bool bNarrow = false;
	float PanelWidth = 1000.f;  // the tab's width, last tick
	bool bCompactShown = false;  // the layout built last was the compact one (stacked rows)
	/** The run area's width: the tab's, less the run history when it shows. */
	float MainWidth() const;
	/** Narrow enough that rows stack instead of sitting side by side. */
	bool IsCompact() const;

	TSharedPtr<SMultiLineEditableTextBox> PromptBox;  // the composer's words: a claim, an exploration or a goal
	TSharedPtr<SMultiLineEditableTextBox> SteerBox;   // the user's words to the live run
	TSharedPtr<SWidget> SteerButton;
	TSharedPtr<SMultiLineEditableTextBox> FeedSteerBox;  // (the Feed tab's: the same words)
	TSharedPtr<SWidget> FeedSteerButton;
	int32 PreviousRunId = INDEX_NONE;  // the run shown before New run (the connect card's Back to the last run)
	TOptional<bool> TestSteers;  // (the layout test's: whether the agent service takes steer, in place of the connection's)
	TArray<TSharedPtr<STextBlock>> StageLabels;       // the stage bar's names, as last made
	TArray<TSharedPtr<SWidget>> AttemptSquares;       // the Live page's harness-attempt squares (1..24), as last made
	TSharedPtr<SWidget> AttemptCount;                  // the attempts' count, after the squares
	static constexpr int32 MostAttemptSquares = 24;
	/** How many attempt squares the Live page shows: the newest attempts that fit its row, at most 24 (0: none yet). */
	int32 AttemptSquaresShown() const;

	TArray<TSharedPtr<FAutopilotRun>> RunItems;  // newest first
	TSharedPtr<SListView<TSharedPtr<FAutopilotRun>>> RunList;
	TSharedPtr<SListView<TSharedPtr<FAutopilotRun>>> NarrowList;  // (the narrow history's list, in NarrowMenu)
	TSharedPtr<SMenuAnchor> NarrowMenu;
	TArray<TSharedPtr<FAutopilotFeedLine>> FeedItems;      // (the Feed tab's: the run's lines under their stages' headings)
	TArray<TSharedPtr<FAutopilotFeedLine>> LiveFeedItems;  // (the Live page's: the last 10)
	TSet<int64> OpenFolds;                                 // the Feed tab's folds open (FoldKey)
	TSharedPtr<SListView<TSharedPtr<FAutopilotFeedLine>>> LiveFeedList;
	TSharedPtr<SListView<TSharedPtr<FAutopilotFeedLine>>> FeedList;
	TSharedPtr<SVerticalBox> ResultBox;  // the result shown: made again for each, in ResultSettle
	TSharedPtr<AutopilotUI::SSettle> ResultSettle;
	TSharedPtr<SPlaytestVideo> Video;  // the result's recording (kept while the same recording is shown)
	bool bOpenVideos = true;           // (false: the layout test's, recordings laid out without a player)
	TSharedPtr<SPlaytestPolicyLog> PolicyLog;

	bool bSettingsPage = false;
	TSharedPtr<SVerticalBox> SettingsBox;  // (made again each time, in SettingsSettle)
	TSharedPtr<AutopilotUI::SSettle> SettingsSettle;
	int32 UsageSeen = -1;

	FPlaytestPreferences Prefs;  // preferences.json as last read or written
	bool bSetupOpen = false;     // the setup, opened again from the Settings page
	int32 SetupStep = StepAccount;
	FString SetupAccess;         // "aura", "own_keys", or "" (decided later)
	FString SetupAccountWord;    // what the stepper says step 1 chose: "AURA", "AURA APP", "BYOK" ("": nothing)
	bool bSetupServiceShown = false;  // step 2 was shown (Back from step 3 goes there)
	bool bSetupShare = true;
	FString SetupError;
	TSharedPtr<SEditableTextBox> TypeSafeKeyBox;
	TSharedPtr<SEditableTextBox> LlmKeyBox;
	TSharedPtr<SEditableTextBox> CommandBox;  // step 2's Agent service command (no agent service found)
	TArray<TSharedPtr<FString>> CodingAgentItems;  // the Coding Agent dropdown's options (PlaytestAccount::CodingAgents)
	TSharedPtr<SComboBox<TSharedPtr<FString>>> CodingAgentCombo;
	FString PickedCodingAgent;   // picked in the dropdown since the setup opened ("": not yet)
	FString SavedCodingAgent;    // autopilot.env's AUTOPILOT_AGENT, read when the setup opens
	TArray<TSharedPtr<FString>> LlmProviderItems;  // the dropdown's options (PlaytestAccount::LlmProviders)
	TSharedPtr<SComboBox<TSharedPtr<FString>>> LlmProviderCombo;
	FString PickedLlmProvider;   // picked in the dropdown for OpenCode since the setup opened ("": not yet)
	FString SavedLlmProvider;    // autopilot.env's AUTOPILOT_OPENCODE_PROVIDER, read when the setup opens
	FString LlmKeyProvider;      // the LLM provider the LLM key box's text is for
	/** A key typed for one LLM provider is never saved as another's: the box empties when the provider changes. */
	void FollowLlmProvider();
	FString KeysDir;             // autopilot.env's folder ("": the user settings folder)
	TSharedPtr<AutopilotUI::SSettle> AuraCardBox;
	TSharedPtr<AutopilotUI::SSettle> ServiceStepBox;
	FString SetupShownKey;

	bool bSignInWanted = false;        // asked for, not yet sent (the agent service is being installed, started or connected)
	bool bSignOutFirst = false;        // "Use another account"
	bool bCreatingAccount = false;     // Create Account opened Aura's account creation page: the Aura card says Sign in next
	bool bSignInSent = false;          // sent: the subsystem's sign-in state shows
	bool bSignInConnectTried = false;
	bool bSignInInstallTried = false;
	FString SignInProblem;             // why it couldn't be asked for

	TSharedPtr<AutopilotUI::SSettle> AccountBox;
	FString AccountShownKey;
	bool bConfirmSignOut = false;
	TOptional<FPlaytestAccount> TestAccount;  // (the layout test's account, in place of the agent service's)
	TOptional<bool> TestRunning;              // (the tests' run going, in place of the session's)
	TOptional<bool> TestConnected;            // (the tests' connection, in place of the subsystem's)
	TOptional<EMemoryView> TestMemory;        // (the tests' memory_state, in place of the subsystem's)
	TOptional<FString> TestMemoryLine;        // (the tests' words after Memory, in place of the subsystem's)
	TOptional<bool> TestAuraApp;              // (the tests': the Aura app installed or not)
	bool bTestStarts = false;                 // (the tests': a run started keeps its frame in TestStartedFrame, sends nothing)
	TFunction<void(const FString&)> TestCopied;  // (the tests': what Copy would put on the clipboard, which it then leaves alone)
	TSharedPtr<FJsonObject> TestStartedFrame;
	TArray<TSharedPtr<SButton>> SettingsButtons;  // the gears (the rail's and the narrow bar's: quiet, and accent-filled while open)
	TSharedPtr<AutopilotUI::SSettle> ConnectBox;
	FString ConnectShownKey;
	TWeakPtr<SButton> SettingsRerunSetup;     // the Settings page's (rebuilt with the account)
	bool bConfirmReset = false;  // Reset memory asked, not yet answered
	bool bConfirmRemove = false;      // Uninstall asked, not yet answered
	TWeakPtr<SButton> RemoveAskButton;  // (the Uninstall that asks: the tests press this one, never the confirmation's)
	bool bRemoveDeletesMemory = false;  // its Delete Autopilot's memories (unticked each time it asks)
	TSharedPtr<AutopilotUI::SSettle> RemovedBox;
	int32 RemovedShown = -1;          // the uninstall state RemovedBox was built for (-1: none yet)
	TOptional<EPlaytestUninstallState> TestUninstall;  // (the tests': the uninstall state, in place of the agent service's)
	bool bKeysShown = false;  // the keys card shown again after signing in (Show)

	int32 SeenRevision = -1;
	int32 SeenRunCount = 0;
	int32 SeenAgentRunId = INDEX_NONE;  // the run the user's coding agent started that was last shown (FAutopilotSession)
	int32 FeedRunId = INDEX_NONE;
	int32 FeedCount = -1;
	int32 ResultRunId = INDEX_NONE;
	int32 ResultRevision = -1;
};
