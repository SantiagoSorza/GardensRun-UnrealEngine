// What the Autopilot tab shows, kept whether the tab is open or not: the runs (newest last), each with its feed (the agent
// service's activity events, what the agent did or said, and its status lines, each under the stage it belongs to), its
// stage, its findings, its result, its policy steps and the tokens its models used. Fed by the bridge's events
// (UPlaytestSubsystem: OnStatus, OnActivity, OnFinding, OnResult, OnJevCall, OnUsage, OnConnectionChanged). The runs are
// saved to the project's Saved/Autopilot/Runs (one file each, all but their policy steps) and loaded again when the editor
// starts. A memory reset takes the runs before it off the tab, their files moved to Saved/Autopilot/Runs/.reset-backup/
// (kept; the Settings page still counts their usage).

#pragma once

#include "CoreMinimal.h"
#include "AutopilotStyle.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "PlaytestAccount.h"
#include "PlaytestInspect.h"
#include "PlaytestPreferences.h"
#include "Styling/SlateColor.h"
#include "UObject/WeakObjectPtr.h"

class UPlaytestSubsystem;

/** Where a run is: driven by the agent service's status lines. */
enum class EAutopilotStage : uint8
{
	None,
	Launch,   // "Starting the game"
	Analyze,  // "Reading the game's code"
	Probe,    // "Trying things in the game"
	Build,    // "Writing the harness"
	Rollout,  // "Trying the harness (run N)"
	Results,  // "Looking at the results" (until the next rollout or the report; an older brain went back to Analyze/Build)
	Report,   // "Saving what it learned to memory", then the result came
};

/** A finding: one of the plugin's in-engine detectors (engine), or one the agent service sent. */
struct FAutopilotFinding
{
	FString Signature;
	FString Severity;
	FString Summary;
	FString Detector;
	FString Actor;
	FString Source;
	int32 Count = 1;
	bool bHasWhere = false;
	FVector Where = FVector::ZeroVector;
	double GameTime = -1.0;
	bool bDismissed = false;  // marked "not a bug"

	bool IsEngine() const { return Source.IsEmpty() || Source == TEXT("engine"); }
};

enum class EAutopilotFeedKind : uint8
{
	Status,   // a status line, as translated
	Attempt,  // "Attempt N"
	Now,      // "Now: ..."
	Finding,  // an engine finding (tagged ENGINE)
	Result,   // the result came
	Notice,   // the plugin's own lines (started, stopping, the connection)
	Memory,   // after the result: the run changed what the agent remembers about the game
	Activity, // one event of the agent service's activity feed (what the agent did or said; docs/protocol.md)
	Heading,  // (the Feed tab's own: a stage's heading over its lines; never a run's line, never saved)
};

/** A line of a run's feed: a status line as translated, the plugin's own notices, a finding, the result, or one event of
 *  the agent service's activity feed (Kind Activity: ActivityId, ActivityKind, Detail; when the event was a call that came
 *  back, its result folded into it: Result, ResultDetail). Every line knows the stage it belongs to (the Feed tab groups
 *  by it). */
struct FAutopilotFeedLine
{
	double Elapsed = 0.0;  // seconds since Run was pressed (an activity event: since the run started, as the agent service counts)
	FString Text;
	EAutopilotFeedKind Kind = EAutopilotFeedKind::Status;
	int32 Attempt = 0;
	TSharedPtr<FAutopilotFinding> Finding;
	/** The stage the line belongs to: the run's when it came, or the activity frame's (None: before any). */
	EAutopilotStage Stage = EAutopilotStage::None;
	// An activity event (docs/protocol.md, The activity feed)
	int32 ActivityId = 0;      // the event's id in the run (0: not an activity event)
	FString ActivityKind;      // text, read, search, write, probe, episode, look, restart, steer, report, alert
	FString Detail;            // what opens under the line ("" for none): the code tried, the path, the whole message
	FString Result;            // the text of the event that ended this one (a call's result), "" until it came
	FString ResultDetail;      // its detail (what came back)
	int32 ResultId = 0;        // that event's id (0: none yet)
	/** The event's own clock: the frame's `t`, seconds since the run started as the agent service counts (-1: not an
	 *  activity event, or a run saved before this was kept). Elapsed is the plugin's clock when the event arrived, the
	 *  one every other line shows, so the feed's times never run backwards; this is kept for ordering and replay. */
	double AgentT = -1.0;

	bool IsActivity() const { return Kind == EAutopilotFeedKind::Activity; }
	/** The icon before an activity line's words, by its kind, named as every Autopilot UI names them (robot: the agent's
	 *  words; magnifier: read; binoculars: searched; pencil: wrote; flask: tried in the game; gamepad: played the harness;
	 *  eye: looked at the picture; circular-arrow: restarted; speech-bubble: the user steered; flag: the report;
	 *  megaphone: an alert; notebook: the refiner edited the game's memory). "" for a kind this build doesn't know.
	 *  FAutopilotStyle::ActivityIcon draws it. */
	FString Icon() const;
	/** What the icon stands for: the tooltip ("Read a file", "Tried something in the game"...). */
	FString Title() const;
	/** The one letter shown in the icon's place when the editor has no brush for it (? the agent's words, R read,
	 *  S searched, W wrote, P tried in the game, E episode, L looked, ! report or alert, > steer, * restart, M memory
	 *  refined). */
	FString Glyph() const;
	/** The fold control's words: "+ inspect the code" while closed, "– hide the code" while open (the detail's name
	 *  stays, so a call's fold and its result's can be told apart). */
	static FString FoldWords(const FString& Label, bool bOpen);
	/** The fold's word for the detail ("the code", "what was written", "the whole message"...; "" when it has none to fold). */
	FString DetailLabel() const;
	/** The fold's word for the result's detail ("the output", "the summary"...; "" when none). */
	FString ResultLabel() const;
	/** Whether Text (or Result) is better folded than shown: more than one line, or long. */
	static bool Folds(const FString& Detail);
};

/** A policy step of a run, and the attempt it belongs to (a new one starts when the step count goes down). */
struct FAutopilotStep
{
	TSharedPtr<FPlaytestJevCall> Call;
	int32 Attempt = 1;
	int32 Index = 0;  // in the run
};

struct FAutopilotRun
{
	int32 Id = 0;
	FString Kind;    // verify, explore, goal; "" for a run the agent service started
	FString Prompt;  // the claim, the goal or the exploration's brief
	TSharedPtr<FJsonObject> Frame;  // what was sent (Run again)
	/** Who started it when the tab didn't (`run_started`'s `by`: "claude-code", "codex", another coding agent's name);
	 *  "" for a run started here. Saved with it. */
	FString StartedBy;
	FDateTime Started;
	double StartSeconds = 0.0;  // FPlatformTime
	double EndSeconds = -1.0;
	bool bDone = false;
	bool bLostConnection = false;  // ended (NO RESULT) only because the connection closed: the run may still go on

	EAutopilotStage Stage = EAutopilotStage::None;
	/** When each stage began (seconds since Run, by EAutopilotStage; -1: not yet): the stage bar's times. Not saved. */
	double StageAt[8] = { -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0 };
	/** Moves the run to InStage (never back from the report), noting when it began. */
	void EnterStage(EAutopilotStage InStage);
	/** How long stage Index (1..7) took, or has taken so far (-1: not reached). */
	double StageSeconds(int32 Index) const;
	int32 Attempt = 0;  // the latest "Attempt N"
	FString Now;        // the latest "Now: ..." (without "Now: ")

	// The result
	FString Verdict;  // CONFIRMED, DENIED, INCONCLUSIVE, GOAL HIT, GOAL MISSED, EXPLORED, GAME TESTED, NO RESULT
	FString Reason;
	TArray<FString> Evidence;
	TArray<FString> AgentNotes;  // what the agent found exploring
	TArray<FString> Changes;     // what the agent changed in the game directly, not through play (a verification's setup)
	/** What the user must know to trust the outcome or to get a better one, as the result's `alerts` word it (the game's
	 *  picture couldn't be looked at...): shown before the reason, saved with the run. None for most runs. */
	TArray<FString> Alerts;
	FString ReportDir;
	FString RunId;
	/** The run's recording (an MP4): a copy in ReportDir once copied, else where the agent service wrote it; "" when none. */
	FString Recording;
	double RecordingSeconds = -1.0;  // its length as the result said (-1: not said)
	/** Why it has no recording, decided when its result came (FAutopilotSession::NoRecordingWhy) and saved with it: the
	 *  result shows "No recording: <this>" where the player would be. "" when it has one, when nothing need be said, and
	 *  for runs saved before this was kept. */
	FString NoRecording;
	double ResultSeconds = -1.0;
	int32 ResultAttempts = -1;

	TArray<TSharedPtr<FAutopilotFinding>> Findings;
	TArray<TSharedPtr<FAutopilotFeedLine>> Feed;
	/** The highest activity event id received (0: none): what the next hello asks the feed from (`activity_since`). Saved. */
	int32 LastActivityId = 0;
	/** Events that came again (ids shown already: the feed replayed after a reconnect) since the last new one: counted,
	 *  said in the log when the replay moves past them. Not saved. */
	int32 ReplaySkipped = 0;
	/** The line that is activity event EventId, or the call it ended (ResultId); null when the run has none. */
	TSharedPtr<FAutopilotFeedLine> FindActivity(int32 EventId) const;

	/** The tokens its models used: the agent service's latest `usage` (cumulative for the run), saved with it. */
	TArray<FPlaytestModelUsage> Usage;
	bool bUsageFinal = false;
	/** What Autopilot was billed for it through Aura (the agent service's `bill`), saved with it; none for BYOK runs. */
	FPlaytestRunBill Bill;

	TArray<TSharedPtr<FAutopilotStep>> Steps;  // kept (the session keeps a bounded number over all runs)
	int32 StepCount = 0;                   // received, kept or not
	int32 StepAttempts = 0;                // attempts seen in the steps
	int64 LastCall = -1;                   // the latest step's number (a new attempt starts when it goes down)
	TArray<float> StepMs;                  // every step's latency (for the median)
	TMap<FString, int32> ActionColors;     // action name -> its color's index, in order of appearance

	int32 Revision = 0;      // bumped on any change
	int32 StepRevision = 0;  // bumped when steps arrive
	FString HistoryFile;     // its file in Saved/Autopilot/Runs

	double Elapsed() const;
	/** Prompt to result: the result's own seconds when it said, else the plugin's timing. */
	double Took() const;
	int32 Attempts() const { return ResultAttempts >= 0 ? ResultAttempts : Attempt; }
	int32 EngineAlerts() const;
	double MedianStepMs() const;
	/** Its state: LIVE the accent; CONFIRMED and GOAL HIT success; DENIED and GOAL MISSED red; EXPLORED and GAME TESTED info; NO RESULT
	 *  neutral; INCONCLUSIVE amber. */
	EAutopilotTone Tone() const;
	/** The verdict block's color (its tone's). */
	FLinearColor VerdictColor() const;
	FLinearColor ColorOf(const FString& Action) const;
	/** Its words as typed; none (a run the agent service started): Explore, Play a goal, Verify or Run, by its kind. */
	FString ShownPrompt() const;
	/** The run's id line: RUN 20261006-220832 // GOAL // Oct 6 22:08 (shown upper case) */
	FString IdLine() const;
	/** The tab bar's line (wide): RUN 20261006-220832 // GOAL // DURATION 03:26 // 2 ATTEMPTS (going: the time so far). */
	FString MetaLine() const;
	/** Its line in the run history: "Oct 19 13:25 · 03:26 · 2 attempts" (its harness attempts). */
	FString HistoryLine() const;
	FString KindWord() const;
	/** The line under its prompt saying where it was started: "Started from Claude Code", "Started from Codex", "Started
	 *  from your coding agent"; "" for a run started here. */
	FString OriginLine() const;
	/** The report as Markdown: the prompt, the verdict, the reason, the evidence, the direct changes and the findings. */
	FString ReportMarkdown() const;
};

class FAutopilotSession
{
	friend class FAutopilotPanelLayoutTest;  // (lays the Settings page out with runs of its own: AutopilotPanelTests.cpp)
	friend class FAutopilotRunBillTest;
	friend class FAutopilotRunEndTest;
	friend class FAutopilotSteerTest;
	friend class FAutopilotBackgroundTest;
	friend class FAutopilotStatusLineTest;
	friend class FAutopilotRunStartedTest;
	friend class FAutopilotRunKindTest;
	friend class FAutopilotActivityTest;
	friend class FAutopilotResetRunsTest;
	friend class FAutopilotUsageLedgerTest;
public:
	static FAutopilotSession& Get();
	/** Blueprint Break on Exceptions (Editor Preferences > Experimental) off while Autopilot plays: an Accessed None would
	 *  stop the game mid-run, and a run reports the error instead (the log_error detector), as Godot's addon lets a game
	 *  stopped at a script error go on and Unity's holds Error Pause off. Not saved: put back by ReleaseExceptionBreaks
	 *  (the play session's end, the module's shutdown). */
	static void HoldExceptionBreaks();
	static void ReleaseExceptionBreaks();

	void Startup();
	void Shutdown();
	/** Autopilot removed from this computer without keeping its memories: the runs forgotten, and what the plugin keeps
	 *  about them in Saved/ deleted (the history, each run's report files and recording, the codex export and its log). */
	void Forget();

	/** Oldest first. */
	const TArray<TSharedPtr<FAutopilotRun>>& GetRuns() const { return Runs; }
	TSharedPtr<FAutopilotRun> GetActiveRun() const;
	TSharedPtr<FAutopilotRun> FindRun(int32 Id) const;
	TSharedPtr<FAutopilotRun> GetLatestRun() const { return Runs.Num() ? Runs.Last() : nullptr; }

	/** Start a run (Kind and Prompt are what the history shows; Frame is sent as is). Null when not connected. */
	TSharedPtr<FAutopilotRun> StartRun(const FString& Kind, const FString& Prompt, const TSharedRef<FJsonObject>& Frame);
	void StopRun();
	/** The user's words to the live run, sent and shown in its feed; false when the run can't be steered now (not
	 *  connected, the agent service doesn't take steer, or the run isn't at its Rollout: SteersAt). */
	bool SteerRun(const FString& Text);
	/** A run is going, connected, and the agent service takes steer (whatever its stage: the steer row shows). */
	bool CanSteer() const;
	/** Whether a run takes the user's words now: only while it plays an episode (its Rollout), never before, after
	 *  or once done. The one rule for the steer box, its button and SteerRun. */
	static bool SteersAt(const FAutopilotRun& Run) { return !Run.bDone && Run.Stage == EAutopilotStage::Rollout; }
	void MarkNotABug(const TSharedPtr<FAutopilotRun>& Run, const TSharedPtr<FAutopilotFinding>& Finding);

	/** The latest run the user's own coding agent started (`run_started`; INDEX_NONE: none this editor session): the tab
	 *  shows it as it shows a run started from its prompt box. */
	int32 GetAgentStartedRunId() const { return AgentStartedRunId; }

	/** Bumped on any change: the tab refreshes what depends on it. */
	int32 GetRevision() const { return Revision; }
	/** Bumped when a run's usage changes, or runs come or go: the Settings page refreshes. */
	int32 GetUsageRevision() const { return UsageRevision; }
	/** The runs the Settings page counts, oldest first: those gone from the history past MaxSavedRuns (the usage
	 *  ledger), those a memory reset took off the tab, then GetRuns(). */
	TArray<TSharedPtr<FAutopilotRun>> GetUsageRuns() const;
	/** The usage of every run kept (summed model by model), and how many runs have some. */
	TArray<FPlaytestModelUsage> ProjectUsage(int32& OutRuns) const;
	/** The bills of every run kept that shows one, summed ("Billed by Aura $X.XX"), and how many runs have one. */
	double ProjectBill(int32& OutRuns) const;
	/** The latest line that belongs to no run (the connection, the memory). */
	const FString& GetNotice() const { return Notice; }
	/** Says Text on the tab's status line, until the next message or 8 s. */
	void Say(const FString& Text);
	/** The status line's words now ("" once 8 s have gone by). */
	FString StatusLine() const;
	/** The memory reset's last word ("Resetting memory…", "Memory reset successful", "Memory not reset: …"; "" since a run started). */
	FString GetResetLine() const
	{
		// ("Resetting memory…" until the outcome; the outcome for 8 s)
		return ResetLine.StartsWith(TEXT("Resetting")) || FPlatformTime::Seconds() - ResetAt < 8.0 ? ResetLine : FString();
	}

	/** A status line as the user sees it, and what it says of the run's progress. */
	static FString Translate(const FString& Line, EAutopilotStage& OutStage, int32& OutAttempt, bool& bOutNow);
	/** The stage an activity frame's `stage` names (analyze, probe, build, play, results, report; None for any other). */
	static EAutopilotStage StageOf(const FString& Name);
	/** A stage's heading on the Feed tab (Launch, Analyze, Probe, Build harness, Play, Results, Report; "" for None). */
	static FString StageHeading(EAutopilotStage Stage);
	/** What the hello asks the activity feed from: the latest run's last event id while it goes on (or lost its connection
	 *  under), else -1 (nothing asked: a finished run's feed is here already). */
	int32 ActivitySince() const;
	/** Words the user never sees, replaced (in text from the agent service). */
	static FString Scrub(const FString& Text);
	static FString Clock(double Seconds);  // a duration: mm:ss, h:mm:ss from an hour
	static FString ClockShort(double Seconds);  // a feed time, a player's position: m:ss, h:mm:ss from an hour

	/** Play In Editor is asked for but has no world playing yet (the run waits for it). */
	static bool PieLoading();
	/** The start frame for the composer's words (docs/protocol.md, Runs): kind "auto", the decision model reads which
	 *  kind of run they ask for (run_kind). TimeLimitMinutes: the composer's Time limit, a verify or goal run's time limit
	 *  and an exploration's minutes of play (none when 0); DesignBrief: the Design Brief setting, for an exploration.
	 *  OutPrompt: what the history shows (the words as typed). */
	static TSharedRef<FJsonObject> RunFrame(const FString& Text, int32 TimeLimitMinutes, const FString& DesignBrief, FString& OutPrompt);
	/** The start frame of an exploration asked for as one (the first-run card's): Minutes of play, the brief. */
	static TSharedRef<FJsonObject> ExploreFrame(int32 Minutes, const FString& DesignBrief);
	/** Why a result has no recording, for the run's NoRecording: "" when it has one; else its `no_recording` as sent (none
	 *  sent: ""). */
	static FString NoRecordingWhy(const TSharedPtr<FJsonObject>& Result);
	/** The feed's last line when a run changed what the agent remembers. */
	static const TCHAR* const MemoryLine;
	static FString When(const FDateTime& Time);  // "Oct 08 13:25"

	static constexpr int32 MaxKeptSteps = 3000;
	static constexpr int32 MaxSavedRuns = 50;
	/** How many runs the usage ledger keeps (the Settings page's count past the history's MaxSavedRuns), as Unity's and
	 *  Godot's do. */
	static constexpr int32 MaxUsageRuns = 1000;

private:
	void OnStatus(const FString& Text);
	/** One event of the run's activity feed (`activity`): a line of the run's feed, or, when it ends a call (`ends`), that
	 *  call's result folded into its line; its stage moves the run's. An id seen already (the feed sent again after a
	 *  reconnect) changes nothing. */
	void OnActivity(const TSharedPtr<FJsonObject>& Frame);
	void OnFinding(const TSharedPtr<FJsonObject>& Finding);
	void OnResult(const TSharedPtr<FJsonObject>& Result, const FString& ReportDir);
	void OnJevCall(const TSharedPtr<FPlaytestJevCall>& Call);
	void OnConnection(bool bConnected, const FString& Info);
	void OnUsage(const TSharedPtr<FJsonObject>& Usage);
	void OnBill(const TSharedPtr<FJsonObject>& Bill);
	/** A run the user's own Claude Code or Codex started (`run_started`): a run of the tab's from now on, unless one is
	 *  going already (then nothing). */
	void OnRunStarted(const TSharedPtr<FJsonObject>& Frame);
	/** The kind of run the decision model read the live run's words as (`run_kind`): the run is that kind from now on. */
	void OnRunKind(const TSharedPtr<FJsonObject>& Frame);
	/** Reset memory's answer (`memory_cleared`): when done, the runs before leave the tab (SetRunsAside). */
	void OnMemoryCleared(bool bOk);
	/** The runs ended taken off the tab (a run going stays), their files moved to a new folder in HistoryDir()'s
	 *  ResetBackup named for now (-1, -2… after another this second), never listed again; kept in SetAside for the
	 *  Settings page. The folder ("" when no file moved). */
	FString SetRunsAside();
	/** The recording copied into the run's folder (off the game thread): the run keeps the copy from then on. */
	void CopyRecording(const FAutopilotRun& Run);
	void OnRecordingCopied(int32 RunId, const FString& Path);

	/** The run an event belongs to: the live one, else one that ended a moment ago; else, when the agent service runs
	 *  one the tab didn't start (bCreate), a new one. */
	TSharedPtr<FAutopilotRun> RunForEvent(bool bCreate, bool bReopen = true);
	/** The latest run, open again when it ended only because the connection closed and the agent service, connected
	 *  again, still sends its events (its status lines, its result): null otherwise. */
	TSharedPtr<FAutopilotRun> ReopenLost();
	TSharedPtr<FAutopilotRun> NewRun(const FString& Kind, const FString& Prompt);
	void AddLine(FAutopilotRun& Run, const FString& Text, EAutopilotFeedKind Kind, const TSharedPtr<FAutopilotFinding>& Finding = nullptr);
	TSharedPtr<FAutopilotFinding> AddFinding(FAutopilotRun& Run, const TSharedPtr<FJsonObject>& Finding);
	void TrimSteps();
	void Changed(FAutopilotRun* Run = nullptr);
	/** Whether the editor must keep its pace now (in the background, minimized): a run plays, or played a moment ago.
	 *  Asked by the engine every frame (ShouldDisableCPUThrottlingDelegates). */
	bool KeepsPlaying();

	/** The saved runs, loaded once (oldest first); a run that was live when the editor closed ends with NO RESULT. */
	void LoadHistory();
	/** Write the runs changed since the last save. */
	void SaveHistory();
	static FString HistoryDir();
	/** Where Reset memory puts the files of the runs it takes off the tab: HistoryDir()/.reset-backup/<time>/ (as the
	 *  agent service keeps the memory it resets: autopilot/memory.py); Forget deletes it with the rest of Saved/Autopilot. */
	static const TCHAR* const ResetBackup;
	/** The usage ledger: the usage and bill of each run whose history file went past MaxSavedRuns (up to
	 *  MaxUsageRuns, oldest first), HistoryDir()/.usage/ledger.json; Forget deletes it with the rest of Saved/Autopilot. */
	static FString UsageLedgerPath();
	void LoadUsageLedger();
	void SaveUsageLedger() const;
	/** A run whose history file is deleted (past MaxSavedRuns): its usage and bill kept in the ledger, when it has any. */
	void Retire(const TSharedPtr<FAutopilotRun>& Run);
	/** A run as the usage ledger keeps it: when, its kind, words, id, usage and bill. */
	static TSharedRef<FJsonObject> UsageToJson(const FAutopilotRun& Run);
	/** The same run in Runs, SetAside and Retired: its id and start. */
	static FString UsageKey(const FAutopilotRun& Run);
	static FString HistoryDirOverride;  // (tests: the history's folder instead of the project's; "": the project's)
	static TSharedRef<FJsonObject> RunToJson(const FAutopilotRun& Run);
	static TSharedPtr<FAutopilotRun> RunFromJson(const TSharedPtr<FJsonObject>& Object);

	TArray<TSharedPtr<FAutopilotRun>> Runs;
	TArray<TSharedPtr<FAutopilotRun>> SetAside;  // the runs a memory reset took off the tab, oldest first (their usage)
	TArray<TSharedPtr<FAutopilotRun>> Retired;  // the usage ledger's runs (gone from the history), oldest first
	bool bLedgerLoaded = false;  // (Retired read from the ledger: saving it never drops what it held)
	int32 NextId = 1;
	int32 AgentStartedRunId = INDEX_NONE;
	int32 Revision = 0;
	int32 UsageRevision = 0;
	int32 KeptSteps = 0;
	FString Notice;
	FString Status;
	double StatusAt = -1.0;
	FString ResetLine;
	double ResetAt = -1000.0;
	bool bHistoryLoaded = false;
	TSet<int32> Unsaved;  // the runs changed since the last save
	FTSTicker::FDelegateHandle SaveTicker;
	FDelegateHandle ThrottleHandle;  // KeepsPlaying, in GEditor's ShouldDisableCPUThrottlingDelegates
	double LastPlayed = -1.0;        // when a run was last seen playing (FPlatformTime::Seconds); -1: none lately
	TWeakObjectPtr<UPlaytestSubsystem> Bound;
	FDelegateHandle StatusHandle;
	FDelegateHandle FindingHandle;
	FDelegateHandle ResultHandle;
	FDelegateHandle JevHandle;
	FDelegateHandle ConnectionHandle;
	FDelegateHandle UsageHandle;
	FDelegateHandle BillHandle;
	FDelegateHandle RunStartedHandle;
	FDelegateHandle RunKindHandle;
	FDelegateHandle ActivityHandle;
	FDelegateHandle MemoryClearedHandle;
};
