#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "PlaytestAccount.h"
#include "PlaytestInspect.h"
#include "Subsystems/EngineSubsystem.h"
#include "PlaytestSubsystem.generated.h"

class IWebSocket;
class UWorld;
class FPlaytestDetectors;
class FPlaytestLogCapture;
class FPlaytestMedia;
class FPlaytestAgentService;
struct FPlaytestInspectWork;

DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestStatusEvent, const FString& /*Text*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestFindingEvent, const TSharedPtr<FJsonObject>& /*Finding*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FPlaytestResultEvent, const TSharedPtr<FJsonObject>& /*Result*/, const FString& /*ReportDir*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FPlaytestConnectionEvent, bool /*bConnected*/, const FString& /*Info*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestJevEvent, const TSharedPtr<FPlaytestJevCall>& /*Call*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestMemoryEvent, const TSharedPtr<FPlaytestMemoryItem>& /*Item*/);
DECLARE_MULTICAST_DELEGATE(FPlaytestInspectResetEvent);
/** The agent service's `usage` message: the run's tokens so far, per model (see PlaytestUsage::Parse). */
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestUsageEvent, const TSharedPtr<FJsonObject>& /*Usage*/);
/** The agent service's `run_started` message: a run the tab didn't start (the user's own Claude Code or Codex started it),
 *  its fields a start frame's, with `by` (docs/protocol.md, Runs). */
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestRunStartedEvent, const TSharedPtr<FJsonObject>& /*Run*/);
/** The agent service's `activity` message: one event of the run's activity feed, what the agent did or said
 *  ({id, t, stage, kind, text, detail, ends}; docs/protocol.md, The activity feed). */
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestActivityEvent, const TSharedPtr<FJsonObject>& /*Frame*/);
/** The agent service's `memory_cleared` message: whether Reset memory reset it (docs/protocol.md). */
DECLARE_MULTICAST_DELEGATE_OneParam(FPlaytestMemoryClearedEvent, bool /*bOk*/);

/** Starts, stops or restarts the play session (PIE in the editor), in the level Map when given (a package name or a
 *  level's name: PlaytestLevels::Resolve; empty: the level open in the editor). Calls Done(running) once finished. */
using FPlaytestSessionHandler = TFunction<void(const FString& /*Action*/, const FString& /*Map*/, TFunction<void(bool /*bRunning*/, const FString& /*Error*/)> /*Done*/)>;

/**
 * The bridge between the game and the Autopilot brain (see docs/protocol.md): connects out over a WebSocket,
 * answers the brain's requests on the game thread, and streams events (engine errors, in-engine detector
 * findings, heartbeats). Inactive until Connect() is called: by the editor once its first-run setup is done, the
 * Autopilot tab, or the -Playtest command line. Never built into Shipping.
 */
UCLASS()
class AUTOPILOTPLAYTEST_API UPlaytestSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	static constexpr int32 ProtocolVersion = 1;

	static UPlaytestSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Connect to the brain (Url empty: the setting). Reconnects with backoff until Disconnect(). */
	void Connect(const FString& Url = FString(), const FString& Token = FString());
	void Disconnect();
	bool IsConnected() const;
	bool WantsConnection() const { return bWantConnected; }
	const FString& GetUrl() const { return Url; }
	bool IsRunActive() const { return bRunActive; }
	/**
	 * Whether a run is using the game now: one is going (IsRunActive), or the agent service asked the game something
	 * in the last half minute (a run this side didn't hear start). Only then do the detectors and the log capture
	 * work: when nothing would read what they find, the user's own Play sessions don't pay for them.
	 */
	bool IsRunUsingGame() const;
	/** What the agent service remembers about this project (its memory_state on this connection; false until one
	 *  came): the earlier runs it keeps, whether it has notes and code of its own for the game. Nothing at all: the
	 *  next run is cold. */
	bool IsMemoryKnown() const { return bMemoryKnown; }
	int32 GetMemoryRuns() const { return MemoryRuns; }
	bool HasMemoryNotes() const { return bMemoryNotes; }
	bool HasMemoryLib() const { return bMemoryLib; }
	/** Whether the user can steer the run going now (the agent service takes `steer`: its welcome said so). */
	bool CanSteer() const { return bConnected && bRunActive && bBrainSteers; }
	/**
	 * Why there is no connection, in words for the user ("" when connected or not trying): the agent service
	 * refused it (its `error` frame's message), or it can't be reached.
	 */
	const FString& GetProblem() const { return Problem; }
	/** Whether this connection goes to Aura (Use with Aura). */
	bool IsWithAura() const { return bWithAura; }
	/** The `code` of the agent service's `error` frame when it refused the connection (e.g. 401, 403); 0 otherwise. */
	int32 GetProblemCode() const { return ProblemCode; }

	/**
	 * The agent service this editor starts itself (FPlaytestAgentService): null outside the editor. Used by connections
	 * when ManagesAgentService().
	 */
	FPlaytestAgentService* GetAgentService() const { return AgentService.Get(); }
	/**
	 * Whether connecting starts the agent service when none is running: the editor (no command-line run), the setting
	 * "Start the agent service automatically", the default Agent Service URL, and not Use with Aura.
	 */
	bool ManagesAgentService() const;

	// The account (docs/protocol.md, "Account"): how the agent service runs, and the Aura sign-in it does for the plugin.

	/** A welcome came on this connection. */
	bool IsWelcomed() const { return bConnected && bWelcomedHere; }
	/** The agent service on this connection takes the account frames (its welcome's features had "account"). */
	bool HasAccountFeature() const { return IsWelcomed() && bBrainAccount; }
	/** The last `account` frame (bKnown false before one). */
	const FPlaytestAccount& GetAccount() const { return Account; }
	/** The last `aura_sign_in` frame, or "asked" once asked. */
	const FPlaytestSignIn& GetSignIn() const { return SignIn; }
	/** Bumped at each account or sign-in frame (and each request). */
	int32 GetAccountRevision() const { return AccountRevision; }
	/** Ask for the account frame again. */
	void RequestAccount();
	/** Ask the agent service to sign in to Aura in the browser (bSignOutFirst: "Use another account"). False when it can't. */
	bool SignInWithAura(bool bSignOutFirst = false);
	/** Stop waiting for the browser. */
	void CancelAuraSignIn();
	/** Sign out of Aura on this computer (every editor here). */
	void SignOutOfAura();
	/** Forget the last sign-in frame (the tab shows nothing for it). */
	void ClearSignIn();

	/** Ask the brain to start a run: {"kind": "verify"|"explore"|"goal", ...} (see docs/protocol.md). */
	void StartRun(const TSharedRef<FJsonObject>& Run);
	/** Ask the brain to stop the run; the Blueprint export running for it (UnrealEditor-Cmd) ends now. */
	void StopRun();
	/** Whether the Blueprint export (the AutopilotCodex commandlet, run for the brain's `codex_manifest`) is running,
	 *  and how far it is: files exported, of all it exports (both 0 until it has counted them). */
	bool GetExportProgress(int32& OutDone, int32& OutTotal) const;
	/** What the user types to steer the run going now: it goes on from where the game is (see docs/protocol.md). */
	void SteerRun(const FString& Text);
	/** The run the connection dropped under is still played by the agent service (its events came again once
	 *  connected): it counts as going again, so Stop and Steer work. */
	void ResumeRun() { if (bConnected) { bRunActive = true; } }
	void SendFeedback(const FString& Signature, const FString& Verdict, const FString& Note = FString());
	/** Ask the brain to forget what it learned about this project (between runs; see docs/protocol.md). */
	void ClearMemory();

	/** The world being played: the one with the player in it, the PIE world in the editor (the first client's when the
	 *  editor plays as clients of a dedicated server), the game world otherwise. Never a dedicated server's world: it
	 *  has no local player, no viewport, and its player controllers only stand in for remote players. */
	UWorld* GetGameWorld() const;

	/** The world with authority over the game being played: GetGameWorld's own unless that is a client, then the
	 *  server's when it runs in this process (the editor's dedicated server, or a listen server); nullptr when the server
	 *  is in another process. */
	UWorld* GetServerWorld() const;

	/** Why no game world is being played although the editor is in Play In Editor (the client hasn't joined its
	 *  dedicated server yet, or never will), "" otherwise. */
	FString GetPlayProblem() const;

	/** Set by the editor module: starts/stops PIE. Without it (a game build), restart reopens the launch map. */
	FPlaytestSessionHandler SessionHandler;

	/** Set by the editor module: whether a play session has been asked for and hasn't started yet. */
	TFunction<bool()> SessionRequested;

	/** The map the game was launched with: the long package name of the first game world seen begin play (no PIE prefix), ""
	 *  before one. A game build's restart reopens it, not the map the game has travelled to since (a match that ended
	 *  moves the server to the next map of its rotation). */
	const FString& GetLaunchMap() const { return LaunchMap; }

	FPlaytestStatusEvent OnStatus;
	FPlaytestFindingEvent OnFinding;
	FPlaytestResultEvent OnResult;
	FPlaytestConnectionEvent OnConnectionChanged;
	/** Each `usage` message (cumulative for the run going: each replaces the one before; docs/protocol.md, "Usage"). */
	FPlaytestUsageEvent OnUsage;
	/** Each `bill` message: what Autopilot was billed for the run through Aura that just finished (before its result;
	 *  PlaytestAccount::ParseBill). */
	FPlaytestUsageEvent OnBill;
	/** Each `run_started` message while no run is going: the run counts as going (Stop and Steer work on it), and the
	 *  Autopilot tab shows it as one of its own. */
	FPlaytestRunStartedEvent OnRunStarted;
	/** Each `run_kind` message: the kind of run the decision model read the run's words as ({kind, tested}; docs/protocol.md,
	 *  Runs). */
	FPlaytestUsageEvent OnRunKind;
	/** Each `activity` message: one event of the run's activity feed (the Autopilot tab lists them under its stages). */
	FPlaytestActivityEvent OnActivity;
	/** Each `memory_cleared` message, after the status line said how it went (the Autopilot tab's history then lets go
	 *  of the runs before a reset). */
	FPlaytestMemoryClearedEvent OnMemoryCleared;
	/** Set by the editor module: the activity id the tab's current run holds (0: none yet), or -1 to ask for no replay.
	 *  Each hello asks the agent service for the feed's events after it (`activity_since`; docs/protocol.md). Unset
	 *  (a game build): never asked. */
	TFunction<int32()> ActivitySince;

	// What the brain does, for the Autopilot tab's policy log (docs/protocol.md: `inspect`, `jev`, `memory`, `memories`).
	// Asked for in the hello by the editor only (the setting bShowInspector): a game run (CI) never gets it.
	// Messages are parsed off the game thread, one batch at a time, and kept here, bounded, so the tab shows a run's
	// history even when opened late or after the brain has gone.

	/** Whether this plugin asks the brain for its Jev calls and memories (the editor, unless the setting says no). */
	bool WantsInspection() const;
	/** The Jev calls received, oldest first: at most MaxJevCalls, and MaxJevBytes packed. */
	const TArray<TSharedPtr<FPlaytestJevCall>>& GetJevCalls() const { return JevCalls; }
	const FPlaytestJevStats& GetJevStats() const { return JevStats; }
	/** The memories: the kept ones the brain listed last (oldest file first), then those formed, changed or forgotten
	 *  since, in arrival order. At most MaxMemories, and MaxMemoryBytes packed: the oldest formed, changed or forgotten
	 *  go first, the kept ones only once none of those is left. */
	const TArray<TSharedPtr<FPlaytestMemoryItem>>& GetMemories() const { return Memories; }
	/** Ask the brain for every memory it keeps now (it answers with `memories`). */
	void RequestMemories();

	FPlaytestJevEvent OnJevCall;
	FPlaytestMemoryEvent OnMemory;
	/** The memories were replaced (the brain listed them again) or everything was cleared: read them all again. */
	FPlaytestInspectResetEvent OnInspectReset;

	static constexpr int32 MaxJevCalls = 3000;
	static constexpr int64 MaxJevBytes = 256ll * 1024 * 1024;
	static constexpr int32 MaxMemories = 10000;
	static constexpr int64 MaxMemoryBytes = 64ll * 1024 * 1024;

	/** Queue an event for the brain (sent once per tick, batched). */
	void QueueEvent(const TSharedRef<FJsonObject>& Event);

	/** Game time of the played world, or a negative number when there is none. */
	double GameTime() const;

	// Used by the request handlers
	struct FPending
	{
		int64 Id = 0;
		FString Op;
		double StartReal = 0;
		double Seconds = 0;
		TSharedPtr<FJsonObject> Args;
		TSharedPtr<FJsonObject> State;  // op-specific
		TFunction<bool(FPending&, double /*Now*/)> Tick;  // returns true when it has responded
	};
	void Respond(int64 Id, const TSharedPtr<FJsonValue>& Result);
	void RespondError(int64 Id, const FString& Error, int32 Status = 400);
	void AddPending(FPending&& Pending);
	/** Send a message already written as JSON text (a frame or a still, written off the game thread). */
	void SendRaw(const FString& Text);
	/** Bumped at each connection: what was asked on one is never answered on the next. */
	int32 GetConnectionSerial() const { return ConnectionSerial; }
	/** `record` and `capture`. */
	FPlaytestMedia& GetMedia() { return *Media; }

	/** Settings for the in-engine detectors, from the `detectors` op. */
	FPlaytestDetectors& GetDetectors() { return *Detectors; }

private:
	bool Tick(float DeltaTime);
	void OpenSocket();
	void ScheduleReconnect();
	void HandleMessage(const FString& Text);
	void HandleRequest(int64 Id, const FString& Op, const TSharedPtr<FJsonObject>& Args);
	void Send(const TSharedRef<FJsonObject>& Message);
	void SendHello();
	void FlushEvents();
	void HandleResult(const TSharedPtr<FJsonObject>& Message);
	/** The folder a run's report goes to (-PlaytestReportDir, else Saved/Playtest/<date-time>), made if missing. */
	FString MakeReportDir() const;
	/** The command-line run's welcome didn't come in time: result.json gets the error, and the game exits with 1. */
	void FailConnectTimeout();
	void Status(const FString& Text);
	void ReadCommandLine();
	void CheckSessionChange();
	void NoteLaunchMap();
	void QueueInspect(const FString& Text);
	void PumpInspect();
	void AddJevCall(const TSharedPtr<FPlaytestJevCall>& Call);
	void AddMemory(const TSharedPtr<FPlaytestMemoryItem>& Item);
	void ReplaceKeptMemories(const TArray<TSharedPtr<FPlaytestMemoryItem>>& Kept);
	void TrimMemories();

	TSharedPtr<IWebSocket> Socket;
	FTSTicker::FDelegateHandle TickHandle;
	FString Url;
	FString Token;
	/** From -PlaytestUrl= / -PlaytestToken= (CI): used instead of the settings at every Connect. */
	FString CommandLineUrl;
	FString CommandLineToken;
	FString BrainRole;
	FString Problem;
	int32 ProblemCode = 0;
	bool bWantConnected = false;
	/** No address was set (the default): each attempt goes to the brain that says where it runs, else the default. */
	bool bAutoUrl = true;
	/** Use with Aura: each attempt goes to the agent service the Aura app runs (PlaytestAura). */
	bool bWithAura = false;
	bool bConnected = false;
	int32 ConnectionSerial = 0;
	bool bRunActive = false;
	double LastRequestAt = -1.0;  // when the agent service's last request came (FPlatformTime::Seconds; -1: none on this connection)
	bool bDetectorsOn = false;  // the detectors worked on the last tick (IsRunUsingGame): they start afresh when on again
	bool bBrainSteers = false;  // the welcome's features had "steer"
	bool bBrainAccount = false;  // ... "account"
	bool bWelcomedHere = false;  // a welcome came on this connection
	FPlaytestAccount Account;
	FPlaytestSignIn SignIn;
	int32 SignInSerial = 0;  // the connection the sign-in going on was asked on
	int32 AccountRevision = 0;
	TSharedPtr<FPlaytestAgentService> AgentService;
	void SendType(const TCHAR* Type);
	bool bMemoryKnown = false;  // a memory_state came on this connection
	int32 MemoryRuns = 0;
	bool bMemoryNotes = false;
	bool bMemoryLib = false;
	/** The connection closed: its memory_state isn't true any more (the agent service removed from this computer,
	 *  say). The next connection's says again. */
	void ForgetMemoryState();
	double NextReconnect = 0;
	double Backoff = 1.0;
	double LastHeartbeat = 0;
	double LastTickReal = 0;

	TArray<FString> Incoming;
	FCriticalSection IncomingLock;
	TArray<TSharedPtr<FJsonValue>> OutgoingEvents;
	TArray<FPending> PendingOps;

	TSharedPtr<FJsonObject> CommandLineRun;  // from -Playtest=...
	FString CommandLineReportDir;
	bool bExitWhenDone = false;
	bool bSentRun = false;
	bool bWelcomed = false;  // a welcome came on some connection (since the subsystem started)
	/** -PlaytestConnectTimeout= (seconds, default 120; 0: wait forever): a command-line run not welcomed by then fails. */
	double ConnectTimeout = 120.0;
	double ConnectDeadline = 0;  // when the command-line run gives up waiting for its welcome (0: never)

	TWeakObjectPtr<UWorld> LastWorld;
	FString LaunchMap;  // see GetLaunchMap

	TSharedPtr<FPlaytestDetectors> Detectors;
	TSharedPtr<FPlaytestLogCapture> LogCapture;
	TSharedPtr<FPlaytestMedia> Media;

	TArray<FString> InspectPending;  // inspection messages waiting for the next batch ("" starts the questions afresh)
	TSharedPtr<FPlaytestInspectWork, ESPMode::ThreadSafe> InspectWork;
	TArray<TSharedPtr<FPlaytestJevCall>> JevCalls;
	int64 JevBytes = 0;
	FPlaytestJevStats JevStats;
	TArray<TSharedPtr<FPlaytestMemoryItem>> Memories;
	int64 MemoryBytes = 0;  // their packed sizes (see FPlaytestMemoryItem::GetPackedSize)
	int64 NextMemoryId = 1;
};
