#include "PlaytestSubsystem.h"
#include "PlaytestLevels.h"

#include <atomic>

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GeneralProjectSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "IWebSocket.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PlaytestAgentService.h"
#include "PlaytestAura.h"
#include "PlaytestInternal.h"
#include "PlaytestJson.h"
#include "PlaytestPreferences.h"
#include "PlaytestSettings.h"
#include "UObject/Package.h"
#include "WebSocketsModule.h"

// (TArray::RemoveAt's shrink argument: a bool before 5.4)
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#define PLAYTEST_NO_SHRINK false
#else
#define PLAYTEST_NO_SHRINK EAllowShrinking::No
#endif

namespace
{
	/**
	 * Exit with a code CI can see. A clean exit request drops the code on Windows (it only posts a quit
	 * message, and the launcher returns the engine's own error level), so exit cleanly as usual and set
	 * the code at the very end of shutdown, once the world and the engine are gone and the log is flushed.
	 */
	void ExitWithCode(int32 Code)
	{
		static int32 ExitCode = 0;
		static bool bHooked = false;
		ExitCode = Code;
		if (!bHooked && Code != 0)
		{
			bHooked = true;
			FCoreDelegates::OnExit.AddLambda([]()
			{
				if (ExitCode != 0)
				{
					if (GLog)
					{
						GLog->Flush();
					}
	#if UE_VERSION_OLDER_THAN(5, 3, 0)
				FPlatformMisc::RequestExitWithStatus(true, (uint8)ExitCode);
#else
				FPlatformMisc::RequestExitWithStatus(true, (uint8)ExitCode, TEXT("AutopilotPlaytest"));
#endif
				}
			});
		}
#if UE_VERSION_OLDER_THAN(5, 3, 0)
		FPlatformMisc::RequestExitWithStatus(false, (uint8)Code);
#else
		FPlatformMisc::RequestExitWithStatus(false, (uint8)Code, TEXT("AutopilotPlaytest"));
#endif
	}
}

DEFINE_LOG_CATEGORY(LogPlaytest);

/**
 * Parses inspection messages (`jev`, `memory`, `memories`) on a pool thread, one batch at a time and in order (a
 * call names the questions unchanged since the one before), so a large message never costs the game thread a frame.
 */
struct FPlaytestInspectWork
{
	struct FResult
	{
		TSharedPtr<FPlaytestJevCall> Call;
		TSharedPtr<FPlaytestMemoryItem> Memory;
		bool bKept = false;  // a `memories` list: Kept replaces the memories kept
		TArray<TSharedPtr<FPlaytestMemoryItem>> Kept;
	};

	FCriticalSection Lock;
	TArray<FResult> Results;  // for the game thread
	std::atomic<bool> bBusy{false};
	PlaytestInspect::FQuestionMemo Memo;  // the pool thread's only

	void Process(const TArray<FString>& Batch)
	{
		TArray<FResult> Out;
		for (const FString& Text : Batch)
		{
			if (Text.IsEmpty())
			{
				Memo.Reset();  // a new connection: the next call carries every question
				continue;
			}
			const TSharedPtr<FJsonObject> Message = PlaytestJson::Parse(Text);
			if (!Message.IsValid())
			{
				continue;
			}
			FString Type;
			Message->TryGetStringField(TEXT("type"), Type);
			if (Type == TEXT("jev"))
			{
				if (TSharedPtr<FPlaytestJevCall> Call = PlaytestInspect::ParseJev(Message, Memo))
				{
					Out.AddDefaulted_GetRef().Call = Call;
				}
			}
			else if (Type == TEXT("memory"))
			{
				if (TSharedPtr<FPlaytestMemoryItem> Item = PlaytestInspect::ParseMemory(Message))
				{
					Out.AddDefaulted_GetRef().Memory = Item;
				}
			}
			else if (Type == TEXT("memories"))
			{
				FResult& Result = Out.AddDefaulted_GetRef();
				Result.bKept = true;
				const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
				if (Message->TryGetArrayField(TEXT("memories"), Entries) && Entries)
				{
					for (const TSharedPtr<FJsonValue>& Entry : *Entries)
					{
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (Entry.IsValid() && Entry->TryGetObject(Object) && Object)
						{
							if (TSharedPtr<FPlaytestMemoryItem> Item = PlaytestInspect::ParseMemory(*Object))
							{
								Item->Change = TEXT("kept");
								Result.Kept.Add(Item);
							}
						}
					}
				}
			}
		}
		{
			FScopeLock Locked(&Lock);
			Results.Append(MoveTemp(Out));
		}
		bBusy = false;
	}
};

namespace
{
	constexpr double MaxBackoff = 30.0;
	constexpr int32 MaxLogEventsPerSecond = 50;
	/** How long after the agent service's last request a run counts as using the game (IsRunUsingGame). */
	constexpr double RunUseSeconds = 30.0;
	const TCHAR* DefaultUrl = TEXT("ws://127.0.0.1:8765/v1/bridge");

	double Now()
	{
		return FPlatformTime::Seconds();
	}

	/** The address to connect to when none is set: an Autopilot of your own running on this machine, on whatever port it says it uses, else the default one. */
	FString AutoUrl()
	{
		const FString Announced = PlaytestAura::AnnouncedUrl(TEXT("brain.json"));
		return Announced.IsEmpty() ? FString(DefaultUrl) : Announced;
	}

	TSharedRef<FJsonValue> GameTimeJson(double GameTime)
	{
		if (GameTime < 0)
		{
			return MakeShared<FJsonValueNull>();
		}
		return PlaytestJson::Number(GameTime);
	}
}

UPlaytestSettings::UPlaytestSettings()
{
	ServiceUrl = DefaultUrl;
}

UPlaytestSubsystem* UPlaytestSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UPlaytestSubsystem>() : nullptr;
}

void UPlaytestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Detectors = MakeShared<FPlaytestDetectors>(*this);
	LogCapture = MakeShared<FPlaytestLogCapture>();
	Media = MakeShared<FPlaytestMedia>(*this);
	InspectWork = MakeShared<FPlaytestInspectWork, ESPMode::ThreadSafe>();
	if (GLog)
	{
		GLog->AddOutputDevice(LogCapture.Get());
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UPlaytestSubsystem::Tick));
	ReadCommandLine();
	if (IsRunningCommandlet())
	{
		return;
	}
	if (GIsEditor)
	{
		// The agent service this editor starts when it connects and none is running (see ManagesAgentService)
		FPlaytestAgentService::FOptions Options;
		Options.SettingsDir = PlaytestAura::SettingsDir();
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Autopilot")))
		{
			const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("AgentService")));
			Options.AgentServiceDir = FPaths::DirectoryExists(Dir) ? Dir : FString();
		}
		Options.Search = []() { return FPlaytestServiceSearch::FromEnvironment(GetDefault<UPlaytestSettings>()->AgentServiceCommand); };
		Options.ExitWithPid = FPlatformProcess::GetCurrentProcessId();
		AgentService = MakeShared<FPlaytestAgentService>(MoveTemp(Options));
	}
	if (CommandLineRun.IsValid() && ConnectTimeout > 0)
	{
		ConnectDeadline = Now() + ConnectTimeout;
	}
	// The editor connects by itself once the first-run setup is done (the Autopilot tab connects when it ends); an
	// unattended editor (automation, a build) only when asked
	if (CommandLineRun.IsValid() || (GIsEditor && !FApp::IsUnattended() && !IsRunningCommandlet() && PlaytestPreferences::Load().bOnboarded))
	{
		Connect();
	}
}

void UPlaytestSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	PlaytestCodex::Shutdown();  // (an export still running ends with the game)
	if (Media.IsValid())
	{
		Media->Shutdown();
	}
	if (GLog && LogCapture.IsValid())
	{
		GLog->RemoveOutputDevice(LogCapture.Get());
	}
	if (Detectors.IsValid())
	{
		Detectors->Reset();  // (its handler on the game's world goes: the world may outlive this subsystem)
	}
	bWantConnected = false;
	if (AgentService.IsValid())
	{
		AgentService->Shutdown();  // (it ends by itself once the editor has: --exit-with)
	}
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		Socket->Close();
		Socket.Reset();
	}
	Super::Deinitialize();
}

void UPlaytestSubsystem::ReadCommandLine()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString Kind;
	if (!FParse::Value(Cmd, TEXT("Playtest="), Kind) || Kind.IsEmpty())
	{
		return;
	}
	TSharedRef<FJsonObject> Run = MakeShared<FJsonObject>();
	Run->SetStringField(TEXT("kind"), Kind.ToLower());
	FString Text;
	if (FParse::Value(Cmd, TEXT("PlaytestClaim="), Text))
	{
		Run->SetStringField(TEXT("claim"), Text.TrimQuotes());
	}
	if (FParse::Value(Cmd, TEXT("PlaytestGoal="), Text))
	{
		Run->SetStringField(TEXT("goal"), Text.TrimQuotes());
	}
	// The design brief: given on the command line, from a file, or the project's setting
	FString Brief;
	if (FParse::Value(Cmd, TEXT("PlaytestBriefFile="), Text))
	{
		FFileHelper::LoadFileToString(Brief, *Text.TrimQuotes());
	}
	else if (FParse::Value(Cmd, TEXT("PlaytestBrief="), Text, false))
	{
		Brief = Text.TrimQuotes();
	}
	else
	{
		Brief = GetDefault<UPlaytestSettings>()->DesignBrief;
	}
	if (!Brief.TrimStartAndEnd().IsEmpty())
	{
		Run->SetStringField(TEXT("brief"), Brief.TrimStartAndEnd());
	}
	int32 Number = 0;
	if (FParse::Value(Cmd, TEXT("PlaytestMinutes="), Number))
	{
		Run->SetNumberField(TEXT("minutes"), Number);
	}
	if (FParse::Value(Cmd, TEXT("PlaytestSteps="), Number))
	{
		Run->SetNumberField(TEXT("steps"), Number);
	}
	if (FParse::Value(Cmd, TEXT("PlaytestAttempts="), Number))
	{
		Run->SetNumberField(TEXT("attempts"), Number);
	}
	// The run's whole time limit, in minutes (a game has no Settings page to give it; none: the agent service's own)
	if (FParse::Value(Cmd, TEXT("PlaytestTimeLimit="), Number) && Number > 0)
	{
		Run->SetNumberField(TEXT("time_limit_minutes"), Number);
	}
	CommandLineRun = Run;
	FParse::Value(Cmd, TEXT("PlaytestUrl="), CommandLineUrl);
	FParse::Value(Cmd, TEXT("PlaytestToken="), CommandLineToken);
	FParse::Value(Cmd, TEXT("PlaytestReportDir="), CommandLineReportDir);
	bExitWhenDone = !FParse::Param(Cmd, TEXT("PlaytestStayOpen"));
	FParse::Value(Cmd, TEXT("PlaytestConnectTimeout="), ConnectTimeout);
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Run requested on the command line: %s"), *PlaytestJson::Write(Run));
}

void UPlaytestSubsystem::Connect(const FString& InUrl, const FString& InToken)
{
	if (AgentService.IsValid() && AgentService->IsRemoved() && !CommandLineRun.IsValid())
	{
		return;  // (Autopilot was removed from this computer: nothing connects until Set up again)
	}
	const UPlaytestSettings* Settings = GetDefault<UPlaytestSettings>();
	// Read the settings again at every Connect: the user may have changed them since the last one.
	// -PlaytestUrl/-PlaytestToken (CI) come before them, and AUTOPILOT_PLAYTEST_TOKEN after them.
	// With Aura (Use with Aura, unless an address is given here or on the command line): the agent service the Aura app
	// runs. Else the address set, and left at the default address the agent service running on this machine, wherever
	// it says it listens (AutoUrl). Either is looked up again at every attempt: one that restarts may have another port
	const FString Given = !InUrl.IsEmpty() ? InUrl : CommandLineUrl;
	bWithAura = Given.IsEmpty() && Settings->bUseWithAura;
	const FString Chosen = !Given.IsEmpty() ? Given : Settings->ServiceUrl;
	bAutoUrl = !bWithAura && (Chosen.IsEmpty() || Chosen == DefaultUrl);
	Url = bWithAura ? PlaytestAura::ServiceUrl() : bAutoUrl ? AutoUrl() : Chosen;
	Token = !InToken.IsEmpty() ? InToken : !CommandLineToken.IsEmpty() ? CommandLineToken : Settings->AccessToken;
	if (Token.IsEmpty())
	{
		Token = FPlatformMisc::GetEnvironmentVariable(TEXT("AUTOPILOT_PLAYTEST_TOKEN"));
	}
	Token.TrimStartAndEndInline();
	bWantConnected = true;
	Backoff = 1.0;
	Problem.Empty();
	ProblemCode = 0;
	if (bAutoUrl && ManagesAgentService())
	{
		AgentService->Retry();  // (asked again: a problem it had is tried again)
	}
	OpenSocket();
}

void UPlaytestSubsystem::Disconnect()
{
	bWantConnected = false;
	bRunActive = false;
	Problem.Empty();
	ProblemCode = 0;
	if (Socket.IsValid())
	{
		Socket->Close();
	}
	ForgetMemoryState();
	Status(TEXT("Disconnected"));
}

void UPlaytestSubsystem::ForgetMemoryState()
{
	bMemoryKnown = false;
	MemoryRuns = 0;
	bMemoryNotes = false;
	bMemoryLib = false;
}

bool UPlaytestSubsystem::IsConnected() const
{
	return bConnected;
}

bool UPlaytestSubsystem::IsRunUsingGame() const
{
	return bConnected && (bRunActive || (LastRequestAt >= 0 && Now() - LastRequestAt < RunUseSeconds));
}

void UPlaytestSubsystem::OpenSocket()
{
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
		Socket.Reset();
		// (its handlers are gone: it never says it closed, so the connection ends here)
		const bool bWas = bConnected;
		bConnected = false;
		bWelcomedHere = false;
		BrainRole.Reset();
		ForgetMemoryState();
		if (bWas)
		{
			OnConnectionChanged.Broadcast(false, TEXT("reconnecting"));
		}
	}
	FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	if (bWithAura)
	{
		Url = PlaytestAura::ServiceUrl();
		if (Url.IsEmpty())  // (Aura's agent service isn't running: try again soon, it may be starting)
		{
			Problem = PlaytestAura::IsInstalled()
				? TEXT("Aura isn’t running on this machine. Start Aura, and Autopilot connects to it by itself.")
				: TEXT("Aura isn’t installed on this machine. Get Aura, or turn off Use with Aura.");
			ProblemCode = 0;
			NextReconnect = Now() + 2.0;
			return;
		}
	}
	else if (bAutoUrl)
	{
		Url = AutoUrl();
		// The editor starts the agent service itself when none is running (installing it is the tab's to ask for)
		if (ManagesAgentService())
		{
			FString Running;
			switch (AgentService->Prepare(Running))
			{
			case FPlaytestAgentService::EPrepared::Ready:
				Url = Running;
				break;
			case FPlaytestAgentService::EPrepared::Busy:  // (starting or installing: look again soon)
				Problem.Empty();
				ProblemCode = 0;
				NextReconnect = Now() + 0.25;
				return;
			case FPlaytestAgentService::EPrepared::Failed:  // (one started by hand at the default address may still answer)
				break;
			}
		}
	}
	// The optional access token, for a brain that asks for one (none: connect without it)
	TMap<FString, FString> Headers;
	if (!Token.IsEmpty())
	{
		Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
	}
	Socket = FWebSocketsModule::Get().CreateWebSocket(Url, FString(), Headers);
	Socket->SetTextMessageMemoryLimit(256ull * 1024 * 1024);
	TWeakObjectPtr<UPlaytestSubsystem> WeakThis(this);
	Socket->OnConnected().AddLambda([WeakThis]()
	{
		if (UPlaytestSubsystem* This = WeakThis.Get())
		{
			This->bConnected = true;
			This->bWelcomedHere = false;
			This->bBrainAccount = false;
			This->LastRequestAt = -1.0;
			++This->ConnectionSerial;
			This->Backoff = 1.0;
			This->SendHello();
		}
	});
	Socket->OnConnectionError().AddLambda([WeakThis](const FString& Error)
	{
		if (UPlaytestSubsystem* This = WeakThis.Get())
		{
			const bool bWas = This->bConnected;
			This->bConnected = false;
			This->BrainRole.Reset();
			This->ForgetMemoryState();
			if (bWas)
			{
				This->OnConnectionChanged.Broadcast(false, Error);
			}
			UE_LOG(LogPlaytest, Verbose, TEXT("Connection to %s failed: %s"), *This->Url, *Error);
			if (!bWas && This->ProblemCode == 0)
			{
				This->Problem = FString::Printf(TEXT("Can’t reach the agent service at %s. Retrying…"), *This->Url);
			}
			This->ScheduleReconnect();
		}
	});
	Socket->OnClosed().AddLambda([WeakThis](int32 Code, const FString& Reason, bool bWasClean)
	{
		if (UPlaytestSubsystem* This = WeakThis.Get())
		{
			const bool bWas = This->bConnected;
			This->bConnected = false;
			This->BrainRole.Reset();
			This->bRunActive = false;
			This->ForgetMemoryState();
			if (bWas)
			{
				This->Status(FString::Printf(TEXT("Connection closed (%d %s)"), Code, *Reason));
				This->OnConnectionChanged.Broadcast(false, Reason);
			}
			This->ScheduleReconnect();
		}
	});
	Socket->OnMessage().AddLambda([WeakThis](const FString& Message)
	{
		if (UPlaytestSubsystem* This = WeakThis.Get())
		{
			FScopeLock Lock(&This->IncomingLock);
			This->Incoming.Add(Message);
		}
	});
	NextReconnect = TNumericLimits<double>::Max();  // until this attempt fails
	Socket->Connect();
}

void UPlaytestSubsystem::ScheduleReconnect()
{
	if (!bWantConnected)
	{
		return;
	}
	NextReconnect = Now() + Backoff;
	Backoff = FMath::Min(Backoff * 2.0, MaxBackoff);
}

void UPlaytestSubsystem::SendHello()
{
	TSharedRef<FJsonObject> Hello = MakeShared<FJsonObject>();
	Hello->SetStringField(TEXT("type"), TEXT("hello"));
	Hello->SetNumberField(TEXT("protocol"), ProtocolVersion);
	FString Version = TEXT("1.0.0");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Autopilot")))
	{
		Version = Plugin->GetDescriptor().VersionName;
	}
	Hello->SetStringField(TEXT("plugin_version"), Version);
	// What this plugin does beyond the protocol's first version: the brain falls back for a plugin without them
	TArray<TSharedPtr<FJsonValue>> Features;
	Features.Add(MakeShared<FJsonValueString>(TEXT("timed_hold")));  // key hold with "seconds", let go in-game
	Features.Add(MakeShared<FJsonValueString>(TEXT("levels")));  // `levels`, and `session` playing the `map` asked for
	Features.Add(MakeShared<FJsonValueString>(TEXT("pointer")));  // `pointer`: the mouse to a place, clicks land there
	if (FPlaytestMedia::CanCapture())
	{
		Features.Add(MakeShared<FJsonValueString>(TEXT("media")));  // `record` and `capture` (a game that renders)
	}
	Hello->SetArrayField(TEXT("features"), Features);
	// Whether the user shares runs' data (preferences.json, or -PlaytestShare): read at every hello, so a change on the
	// Settings page holds from the next connection
	Hello->SetBoolField(TEXT("share"), PlaytestPreferences::ShareData());
	// Whether runs have the research agent, once the user chose (the Settings page's switch; -PlaytestResearch): until
	// then the agent service decides (off in the Aura app's, Use with Aura; on in the user's own, signed in to Aura or not)
	const TOptional<bool> Research = PlaytestPreferences::ResearchAgent();
	if (Research.IsSet())
	{
		Hello->SetBoolField(TEXT("research"), Research.GetValue());
	}
	Hello->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
	Hello->SetStringField(TEXT("project"), FApp::GetProjectName());
	Hello->SetStringField(TEXT("engine"), TEXT("unreal"));
	Hello->SetObjectField(TEXT("input"), PlaytestOps::InputInfo());  // (as Unity's and Godot's hellos)
	// The project's own GUID (DefaultGame.ini, made with the project and committed with it): a stable id a brain can
	// keep what it learns by, so two games with the same name (every "MyProject") stay apart
	const FGuid& ProjectId = GetDefault<UGeneralProjectSettings>()->ProjectID;
	if (ProjectId.IsValid())
	{
		Hello->SetStringField(TEXT("project_id"), ProjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
	}
	Hello->SetStringField(TEXT("host"), GIsEditor ? TEXT("editor") : TEXT("game"));
	if (WantsInspection())
	{
		Hello->SetBoolField(TEXT("inspect"), true);  // send the Jev calls and memories (docs/protocol.md)
	}
	if (CommandLineRun.IsValid() && !bSentRun)
	{
		Hello->SetObjectField(TEXT("run"), CommandLineRun);
		bSentRun = true;
		bRunActive = true;
	}
	// The tab holds a run that is going (or lost its connection under): ask for the activity events it hasn't got
	// (docs/protocol.md, The activity feed; the agent service sends them after its welcome)
	const int32 Since = ActivitySince ? ActivitySince() : -1;
	if (Since >= 0)
	{
		Hello->SetNumberField(TEXT("activity_since"), Since);
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Asking for activity since %d"), Since);
	}
	Send(Hello);
}

void UPlaytestSubsystem::Send(const TSharedRef<FJsonObject>& Message)
{
	if (Socket.IsValid() && bConnected)
	{
		Socket->Send(PlaytestJson::Write(Message));
	}
}

void UPlaytestSubsystem::SendRaw(const FString& Text)
{
	if (Socket.IsValid() && bConnected)
	{
		Socket->Send(Text);
	}
}

void UPlaytestSubsystem::Status(const FString& Text)
{
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] %s"), *Text);
	OnStatus.Broadcast(Text);
}

void UPlaytestSubsystem::StartRun(const TSharedRef<FJsonObject>& Run)
{
	if (!bConnected)
	{
		Status(TEXT("Not connected: connect first"));
		return;
	}
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("start"));
	Message->SetObjectField(TEXT("run"), Run);
	Send(Message);
	bRunActive = true;
	// (a run the decision model reads the kind of has none to say yet)
	const FString Kind = Run->GetStringField(TEXT("kind"));
	Status(Kind == TEXT("auto") ? FString(TEXT("Started run")) : FString::Printf(TEXT("Started %s run"), *Kind));
}

void UPlaytestSubsystem::StopRun()
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("stop"));
	Send(Message);
	Status(TEXT("Stopping…"));
	// (else it loads every asset on until it ends, and the run's stop waits for it)
	PlaytestCodex::StopExport();
}

bool UPlaytestSubsystem::GetExportProgress(int32& OutDone, int32& OutTotal) const
{
	return PlaytestCodex::ExportProgress(OutDone, OutTotal);
}

void UPlaytestSubsystem::SteerRun(const FString& Text)
{
	const FString Said = Text.TrimStartAndEnd();
	if (!CanSteer() || Said.IsEmpty())
	{
		return;
	}
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("steer"));
	Message->SetStringField(TEXT("text"), Said);
	Send(Message);
}

void UPlaytestSubsystem::SendFeedback(const FString& Signature, const FString& Verdict, const FString& Note)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("feedback"));
	Message->SetStringField(TEXT("signature"), Signature);
	Message->SetStringField(TEXT("verdict"), Verdict);
	if (!Note.IsEmpty())
	{
		Message->SetStringField(TEXT("note"), Note);
	}
	Send(Message);
}

void UPlaytestSubsystem::ClearMemory()
{
	if (!bConnected)
	{
		Status(TEXT("Not connected: connect first to reset the memory"));
		return;
	}
	if (bRunActive)
	{
		Status(TEXT("A run is going: reset the memory once it is done"));
		return;
	}
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("clear_memory"));
	Send(Message);
	Status(TEXT("Resetting memory…"));
}

UWorld* UPlaytestSubsystem::GetGameWorld() const
{
	if (!GEngine)
	{
		return nullptr;
	}
	UWorld* Game = nullptr;
	UWorld* Pie = nullptr;
	int32 PieInstance = MAX_int32;
	bool bAnyPie = false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (!World)
		{
			continue;
		}
		if (Context.WorldType == EWorldType::PIE)
		{
			// (the first of several clients: the lowest PIE instance; a dedicated server's is never played)
			bAnyPie = true;
			if (!Context.RunAsDedicated && World->GetNetMode() != NM_DedicatedServer && Context.PIEInstance < PieInstance)
			{
				Pie = World;
				PieInstance = Context.PIEInstance;
			}
		}
		else if (Context.WorldType == EWorldType::Game && !Game)
		{
			Game = World;
		}
	}
	return bAnyPie ? Pie : Game;
}

UWorld* UPlaytestSubsystem::GetServerWorld() const
{
	UWorld* Played = GetGameWorld();
	if (!Played || Played->GetNetMode() != NM_Client || !GEngine)
	{
		return Played;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && World != Played && Context.WorldType == EWorldType::PIE && World->GetNetMode() != NM_Client)
		{
			return World;
		}
	}
	return nullptr;
}

FString UPlaytestSubsystem::GetPlayProblem() const
{
	if (!GEngine || GetGameWorld())
	{
		return FString();
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return TEXT("Play In Editor runs a dedicated server, but its client hasn't joined it (yet): no player to "
				"play as");
		}
	}
	return FString();
}

double UPlaytestSubsystem::GameTime() const
{
	UWorld* World = GetGameWorld();
	return World ? World->GetTimeSeconds() : -1.0;
}

void UPlaytestSubsystem::QueueEvent(const TSharedRef<FJsonObject>& Event)
{
	OutgoingEvents.Add(MakeShared<FJsonValueObject>(Event));
}

void UPlaytestSubsystem::Respond(int64 Id, const TSharedPtr<FJsonValue>& Result)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("response"));
	Message->SetNumberField(TEXT("id"), (double)Id);
	Message->SetBoolField(TEXT("ok"), true);
	Message->SetField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	Message->SetField(TEXT("t"), GameTimeJson(GameTime()));
	Send(Message);
}

void UPlaytestSubsystem::RespondError(int64 Id, const FString& Error, int32 StatusCode)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("response"));
	Message->SetNumberField(TEXT("id"), (double)Id);
	Message->SetBoolField(TEXT("ok"), false);
	Message->SetNumberField(TEXT("status"), StatusCode);
	Message->SetStringField(TEXT("error"), Error);
	Message->SetField(TEXT("t"), GameTimeJson(GameTime()));
	Send(Message);
}

void UPlaytestSubsystem::AddPending(FPending&& Pending)
{
	// Give it its first tick right away, so e.g. movement starts in the frame the request arrived
	if (Pending.Tick && Pending.Tick(Pending, Now()))
	{
		return;
	}
	PendingOps.Add(MoveTemp(Pending));
}

void UPlaytestSubsystem::HandleMessage(const FString& Text)
{
	// What the brain does, for the inspector: parsed off the game thread (a Jev call can be a large message)
	if (PlaytestInspect::IsInspectType(PlaytestInspect::PeekType(Text)))
	{
		QueueInspect(Text);
		return;
	}
	TSharedPtr<FJsonObject> Message = PlaytestJson::Parse(Text);
	if (!Message.IsValid())
	{
		UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] Unreadable message from the agent service: %s"), *Text.Left(200));
		return;
	}
	const FString Type = Message->GetStringField(TEXT("type"));
	if (Type == TEXT("request"))
	{
		LastRequestAt = Now();  // (a run is using the game: IsRunUsingGame)
		const int64 Id = (int64)Message->GetNumberField(TEXT("id"));
		const TSharedPtr<FJsonObject>* Args = nullptr;
		TSharedPtr<FJsonObject> ArgsObj = Message->TryGetObjectField(TEXT("args"), Args) && Args ? *Args : MakeShared<FJsonObject>();
		HandleRequest(Id, Message->GetStringField(TEXT("op")), ArgsObj);
	}
	else if (Type == TEXT("welcome"))
	{
		BrainRole = Message->GetStringField(TEXT("role"));
		bWelcomed = true;
		bWelcomedHere = true;
		bBrainSteers = false;
		bBrainAccount = false;
		const TArray<TSharedPtr<FJsonValue>>* BrainFeatures = nullptr;
		if (Message->TryGetArrayField(TEXT("features"), BrainFeatures) && BrainFeatures)
		{
			for (const TSharedPtr<FJsonValue>& Feature : *BrainFeatures)
			{
				const FString Name = Feature.IsValid() && Feature->Type == EJson::String ? Feature->AsString() : FString();
				bBrainSteers |= Name == TEXT("steer");
				bBrainAccount |= Name == TEXT("account");
			}
		}
		Problem.Empty();
		ProblemCode = 0;
		++AccountRevision;  // (what the account can do changed)
		if (bBrainAccount)
		{
			SendType(TEXT("account"));  // (asked again at every connection: what the tab knows may be stale)
		}
		Status(TEXT("Connected to the agent service"));
		OnConnectionChanged.Broadcast(true, BrainRole);
		if (BrainRole == TEXT("cli"))
		{
			bRunActive = true;
		}
		if (WantsInspection())
		{
			InspectPending.Add(FString());  // (the questions start afresh on this connection)
			RequestMemories();
		}
	}
	else if (Type == TEXT("status"))
	{
		Status(Message->GetStringField(TEXT("text")));
	}
	else if (Type == TEXT("activity"))
	{
		OnActivity.Broadcast(Message);  // (one event of the run's activity feed: the Autopilot tab lists it under its stage)
	}
	else if (Type == TEXT("finding"))
	{
		const TSharedPtr<FJsonObject>* Finding = nullptr;
		if (Message->TryGetObjectField(TEXT("finding"), Finding) && Finding)
		{
			OnFinding.Broadcast(*Finding);
			Status(FString::Printf(TEXT("Finding: %s"), *(*Finding)->GetStringField(TEXT("summary"))));
		}
	}
	else if (Type == TEXT("result"))
	{
		HandleResult(Message);
	}
	else if (Type == TEXT("memory_state"))
	{
		// What the agent service remembers about this project (docs/protocol.md): the memory the tab shows
		double Runs = 0;
		Message->TryGetNumberField(TEXT("runs"), Runs);
		MemoryRuns = FMath::Max(0, (int32)Runs);
		bMemoryNotes = false;
		bMemoryLib = false;
		Message->TryGetBoolField(TEXT("notes"), bMemoryNotes);
		Message->TryGetBoolField(TEXT("lib"), bMemoryLib);
		bMemoryKnown = true;
	}
	else if (Type == TEXT("usage"))
	{
		OnUsage.Broadcast(Message);  // (the Autopilot tab keeps it with the run: its Settings page)
	}
	else if (Type == TEXT("bill"))
	{
		OnBill.Broadcast(Message);  // (kept with the run too: "Billed $0.59")
	}
	else if (Type == TEXT("run_started"))
	{
		// A run the user's own Claude Code or Codex started (docs/protocol.md, Runs): going, as a run the tab started is
		// (a run going already is the tab's own: this one isn't)
		const TSharedPtr<FJsonObject>* Run = nullptr;
		if (!bRunActive && Message->TryGetObjectField(TEXT("run"), Run) && Run && Run->IsValid())
		{
			bRunActive = true;
			OnRunStarted.Broadcast(*Run);
		}
	}
	else if (Type == TEXT("run_kind"))
	{
		OnRunKind.Broadcast(Message);  // (the Autopilot tab shows the run as that kind from now on)
	}
	else if (Type == TEXT("memory_cleared"))
	{
		bool bOk = false;
		Message->TryGetBoolField(TEXT("ok"), bOk);
		if (bOk)
		{
			double Files = 0;
			Message->TryGetNumberField(TEXT("files"), Files);
			UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Memory reset (%d file(s))"), (int32)Files);
			Status(TEXT("Memory reset successful"));
		}
		else
		{
			FString Why;
			Message->TryGetStringField(TEXT("message"), Why);
			Status(FString::Printf(TEXT("Memory not reset: %s"), *Why));
		}
		OnMemoryCleared.Broadcast(bOk);
	}
	else if (Type == TEXT("account"))
	{
		PlaytestAccount::ParseAccount(Message, Account);  // (the tab's setup and Settings page show it)
		++AccountRevision;
		// A sign-in asked on a connection that has closed since: its answer may have gone with it. Finished meanwhile
		// (this frame has its session), or asked again on this one: the agent service kept it going and says where
		// it is, or starts it again when it was restarted
		if (SignInSerial != ConnectionSerial && PlaytestAccount::IsSignInGoing(SignIn))
		{
			SignInSerial = ConnectionSerial;
			FPlaytestSignIn Finished;
			if (PlaytestAccount::SignInFinishedAway(SignIn, Account, Finished))
			{
				SignIn = Finished;
			}
			else
			{
				SendType(TEXT("aura_sign_in"));
			}
		}
	}
	else if (Type == TEXT("aura_sign_in"))
	{
		FPlaytestSignIn Said;
		if (PlaytestAccount::ParseSignIn(Message, Said))
		{
			// (a sign-in cancelled before this one was asked for says so late: it isn't about this one)
			const bool bStale = SignIn.State == TEXT("asked") && Said.State == TEXT("error") && Said.Reason == TEXT("cancelled");
			if (!bStale)
			{
				SignIn = Said;
				++AccountRevision;
			}
		}
	}
	else if (PlaytestInspect::IsInspectType(Type))
	{
		QueueInspect(Text);  // (a message whose type didn't come first)
	}
	else if (Type == TEXT("error"))
	{
		const FString Error = Message->GetStringField(TEXT("message"));
		double Code = 0;
		Message->TryGetNumberField(TEXT("code"), Code);
		Problem = Error;
		ProblemCode = (int32)Code;
		Status(Error);
		// Retrying with the same token wouldn't help
		bWantConnected = false;
		if (bExitWhenDone && CommandLineRun.IsValid())
		{
			ExitWithCode(1);
		}
	}
}

bool UPlaytestSubsystem::ManagesAgentService() const
{
	if (!AgentService.IsValid() || CommandLineRun.IsValid() || !CommandLineUrl.IsEmpty())
	{
		return false;
	}
	const UPlaytestSettings* Settings = GetDefault<UPlaytestSettings>();
	return !Settings->bUseWithAura
		&& (Settings->ServiceUrl.TrimStartAndEnd().IsEmpty() || Settings->ServiceUrl.TrimStartAndEnd() == DefaultUrl);
}

void UPlaytestSubsystem::SendType(const TCHAR* Type)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), Type);
	Send(Message);
}

void UPlaytestSubsystem::RequestAccount()
{
	if (HasAccountFeature())
	{
		SendType(TEXT("account"));
	}
}

bool UPlaytestSubsystem::SignInWithAura(bool bSignOutFirst)
{
	if (!HasAccountFeature())
	{
		return false;
	}
	if (bSignOutFirst)
	{
		SendType(TEXT("aura_sign_out"));
	}
	SignIn = FPlaytestSignIn();
	SignIn.State = TEXT("asked");
	SignInSerial = ConnectionSerial;
	++AccountRevision;
	SendType(TEXT("aura_sign_in"));
	return true;
}

void UPlaytestSubsystem::CancelAuraSignIn()
{
	if (IsWelcomed() && !SignIn.State.IsEmpty() && SignIn.State != TEXT("signed_in") && SignIn.State != TEXT("error"))
	{
		SendType(TEXT("aura_sign_in_cancel"));
	}
	ClearSignIn();
}

void UPlaytestSubsystem::SignOutOfAura()
{
	if (HasAccountFeature())
	{
		SendType(TEXT("aura_sign_out"));
		SendType(TEXT("account"));
	}
}

void UPlaytestSubsystem::ClearSignIn()
{
	SignIn = FPlaytestSignIn();
	++AccountRevision;
}

bool UPlaytestSubsystem::WantsInspection() const
{
	return GIsEditor && GetDefault<UPlaytestSettings>()->bShowInspector;
}

void UPlaytestSubsystem::RequestMemories()
{
	if (!bConnected)
	{
		return;
	}
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("inspect"));
	Message->SetBoolField(TEXT("snapshot"), true);
	Send(Message);
}

void UPlaytestSubsystem::QueueInspect(const FString& Text)
{
	if (WantsInspection())
	{
		InspectPending.Add(Text);
	}
}

void UPlaytestSubsystem::PumpInspect()
{
	if (!InspectWork.IsValid())
	{
		return;
	}
	TArray<FPlaytestInspectWork::FResult> Results;
	{
		FScopeLock Locked(&InspectWork->Lock);
		Results = MoveTemp(InspectWork->Results);
		InspectWork->Results.Reset();
	}
	for (const FPlaytestInspectWork::FResult& Result : Results)
	{
		if (Result.Call.IsValid())
		{
			AddJevCall(Result.Call);
		}
		else if (Result.Memory.IsValid())
		{
			AddMemory(Result.Memory);
		}
		else if (Result.bKept)
		{
			ReplaceKeptMemories(Result.Kept);
		}
	}
	if (InspectPending.Num() && !InspectWork->bBusy)
	{
		InspectWork->bBusy = true;
		TSharedPtr<FPlaytestInspectWork, ESPMode::ThreadSafe> Work = InspectWork;
		Async(EAsyncExecution::ThreadPool, [Work, Batch = MoveTemp(InspectPending)]()
		{
			Work->Process(Batch);
		});
		InspectPending.Reset();
	}
}

void UPlaytestSubsystem::AddJevCall(const TSharedPtr<FPlaytestJevCall>& Call)
{
	JevCalls.Add(Call);
	JevBytes += Call->GetPackedSize();
	PlaytestInspect::AddToStats(JevStats, JevCalls);
	int32 Drop = 0;
	while (JevCalls.Num() - Drop > 1 && (JevCalls.Num() - Drop > MaxJevCalls || JevBytes > MaxJevBytes))
	{
		JevBytes -= JevCalls[Drop]->GetPackedSize();
		++Drop;
	}
	if (Drop)
	{
		JevCalls.RemoveAt(0, Drop, PLAYTEST_NO_SHRINK);
	}
	OnJevCall.Broadcast(Call);
}

void UPlaytestSubsystem::AddMemory(const TSharedPtr<FPlaytestMemoryItem>& Item)
{
	Item->Id = NextMemoryId++;
	Memories.Add(Item);
	MemoryBytes += Item->GetPackedSize();
	TrimMemories();
	OnMemory.Broadcast(Item);
}

void UPlaytestSubsystem::ReplaceKeptMemories(const TArray<TSharedPtr<FPlaytestMemoryItem>>& Kept)
{
	// The memories kept now come first, then what formed or changed since the brain last listed them
	Memories.RemoveAll([this](const TSharedPtr<FPlaytestMemoryItem>& Item)
	{
		const bool bKept = Item->Change == TEXT("kept");
		if (bKept)
		{
			MemoryBytes -= Item->GetPackedSize();
		}
		return bKept;
	});
	for (const TSharedPtr<FPlaytestMemoryItem>& Item : Kept)
	{
		Item->Id = NextMemoryId++;
		MemoryBytes += Item->GetPackedSize();
	}
	Memories.Insert(Kept, 0);
	TrimMemories();
	OnInspectReset.Broadcast();
}

void UPlaytestSubsystem::TrimMemories()
{
	// Kept first (see ReplaceKeptMemories), then the events: the oldest events go first, the kept ones (what the brain
	// keeps now) only once no event is left
	auto Over = [this](int32 Removed) { return Memories.Num() - Removed > MaxMemories || MemoryBytes > MaxMemoryBytes; };
	int32 KeptNum = 0;
	while (KeptNum < Memories.Num() && Memories[KeptNum]->Change == TEXT("kept"))
	{
		++KeptNum;
	}
	int32 Events = 0;
	while (KeptNum + Events < Memories.Num() && Over(Events))
	{
		MemoryBytes -= Memories[KeptNum + Events]->GetPackedSize();
		++Events;
	}
	if (Events)
	{
		Memories.RemoveAt(KeptNum, Events, PLAYTEST_NO_SHRINK);
	}
	int32 KeptOnes = 0;
	while (KeptOnes < KeptNum && Memories.Num() - KeptOnes > 1 && Over(KeptOnes))
	{
		MemoryBytes -= Memories[KeptOnes]->GetPackedSize();
		++KeptOnes;
	}
	if (KeptOnes)
	{
		Memories.RemoveAt(0, KeptOnes, PLAYTEST_NO_SHRINK);
	}
}

void UPlaytestSubsystem::HandleRequest(int64 Id, const FString& Op, const TSharedPtr<FJsonObject>& Args)
{
	PlaytestOps::Handle(*this, Id, Op, Args);
}

void UPlaytestSubsystem::HandleResult(const TSharedPtr<FJsonObject>& Message)
{
	bRunActive = false;
	const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
	TSharedPtr<FJsonObject> Result = Message->TryGetObjectField(TEXT("result"), ResultPtr) && ResultPtr ? *ResultPtr : MakeShared<FJsonObject>();

	const FString Dir = MakeReportDir();
	const TSharedPtr<FJsonObject>* Files = nullptr;
	if (Message->TryGetObjectField(TEXT("files"), Files) && Files)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& File : (*Files)->Values)
		{
			const FString Name = FPaths::GetCleanFilename(File.Key);  // never outside the report folder
			if (Name.IsEmpty() || !File.Value.IsValid())
			{
				continue;
			}
			FFileHelper::SaveStringToFile(File.Value->AsString(), *FPaths::Combine(Dir, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}
	FFileHelper::SaveStringToFile(PlaytestJson::Write(Result.ToSharedRef()), *FPaths::Combine(Dir, TEXT("result.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	Status(FString::Printf(TEXT("Run finished. Report: %s"), *Dir));
	OnResult.Broadcast(Result, Dir);

	if (bExitWhenDone && CommandLineRun.IsValid())
	{
		int32 Code = 0;
		double Number = 0;
		FString Verdict;
		if (Result->TryGetNumberField(TEXT("exit_code"), Number))
		{
			Code = (int32)Number;
		}
		else if (Result->TryGetStringField(TEXT("verdict"), Verdict))
		{
			Code = Verdict == TEXT("CONFIRMED") ? 0 : Verdict == TEXT("DENIED") ? 2 : 3;
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* Bugs = nullptr;
			Code = (Result->TryGetArrayField(TEXT("bugs"), Bugs) && Bugs && Bugs->Num() > 0) ? 2 : 0;
		}
		UE_LOG(LogPlaytest, Display, TEXT("Exiting with code %d"), Code);
		if (SessionHandler && (GetGameWorld() || !GetPlayProblem().IsEmpty()))
		{
			// Exiting the editor with Play In Editor still running trips engine asserts: end it first
			SessionHandler(TEXT("stop"), FString(), [Code](bool, const FString&)
			{
				ExitWithCode(Code);
			});
			return;
		}
		ExitWithCode(Code);
	}
}

FString UPlaytestSubsystem::MakeReportDir() const
{
	FString Dir = CommandLineReportDir;
	if (Dir.IsEmpty())
	{
		Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Playtest"), FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
	}
	Dir = FPaths::ConvertRelativePathToFull(Dir);
	IFileManager::Get().MakeDirectory(*Dir, true);
	return Dir;
}

void UPlaytestSubsystem::FailConnectTimeout()
{
	ConnectDeadline = 0;
	bWantConnected = false;  // stop retrying
	if (Socket.IsValid())
	{
		Socket->Close();
	}
	const FString Error = FString::Printf(TEXT("No welcome from the agent service at %s within %.0f s (-PlaytestConnectTimeout)"),
		*Url, ConnectTimeout);
	Problem = Error;
	UE_LOG(LogPlaytest, Error, TEXT("[Autopilot] %s"), *Error);
	OnStatus.Broadcast(Error);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("kind"), CommandLineRun->GetStringField(TEXT("kind")));
	Result->SetStringField(TEXT("error"), Error);
	Result->SetNumberField(TEXT("exit_code"), 1);
	const FString Dir = MakeReportDir();
	FFileHelper::SaveStringToFile(PlaytestJson::Write(Result), *FPaths::Combine(Dir, TEXT("result.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Run failed. Report: %s"), *Dir);
	if (bExitWhenDone)
	{
		UE_LOG(LogPlaytest, Display, TEXT("Exiting with code 1"));
		ExitWithCode(1);
	}
}

void UPlaytestSubsystem::FlushEvents()
{
	if (OutgoingEvents.IsEmpty())
	{
		return;
	}
	if (bConnected)
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("events"));
		Message->SetArrayField(TEXT("events"), OutgoingEvents);
		Message->SetField(TEXT("t"), GameTimeJson(GameTime()));
		Send(Message);
	}
	OutgoingEvents.Reset();
}

void UPlaytestSubsystem::CheckSessionChange()
{
	UWorld* World = GetGameWorld();
	if (World == LastWorld.Get() && (World != nullptr) == LastWorld.IsValid())
	{
		return;
	}
	LastWorld = World;
	Detectors->Reset();
	TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("kind"), TEXT("session"));
	Event->SetField(TEXT("t"), GameTimeJson(GameTime()));
	Event->SetBoolField(TEXT("running"), World != nullptr);
	Event->SetStringField(TEXT("scene"), World ? PlaytestLevels::PackageOf(World) : FString());  // (as Unity's)
	QueueEvent(Event);
}

void UPlaytestSubsystem::NoteLaunchMap()
{
	if (!LaunchMap.IsEmpty())
	{
		return;
	}
	// The first game world that began play (not the empty world a game instance makes before it loads its first map)
	UWorld* World = GetGameWorld();
	if (World && World->HasBegunPlay() && World->GetPackage())
	{
		LaunchMap = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
	}
}

bool UPlaytestSubsystem::Tick(float DeltaTime)
{
	const double T = Now();
	const double RealDelta = LastTickReal > 0 ? T - LastTickReal : 0.0;
	LastTickReal = T;

	if (ConnectDeadline > 0 && !bWelcomed && T >= ConnectDeadline && bWantConnected)
	{
		FailConnectTimeout();  // (a command-line run: the agent service never welcomed it)
	}
	if (bWantConnected && !bConnected && T >= NextReconnect)
	{
		OpenSocket();
	}

	TArray<FString> Messages;
	{
		FScopeLock Lock(&IncomingLock);
		Messages = MoveTemp(Incoming);
		Incoming.Reset();
	}
	for (const FString& Message : Messages)
	{
		HandleMessage(Message);
	}
	PlaytestOps::TickKeys(*this, DeltaTime);  // held keys and axes, every frame (all let go once disconnected)
	PumpInspect();  // (also once disconnected: what was already received still arrives)
	Media->Tick();  // (also once disconnected: a recording ends with its connection)
	NoteLaunchMap();  // (connected or not: the map launched with, before any travel)

	if (!bConnected)
	{
		LogCapture->bEnabled = false;
		bDetectorsOn = false;
		LogCapture->Take();
		OutgoingEvents.Reset();
		return true;
	}

	CheckSessionChange();
	PlaytestScreen::TickUnpainted(GetGameWorld(), DeltaTime);

	for (int32 i = PendingOps.Num() - 1; i >= 0; --i)
	{
		if (PendingOps[i].Tick(PendingOps[i], T))
		{
			PendingOps.RemoveAt(i);
		}
	}

	UWorld* World = GetGameWorld();
	const double GameNow = World ? World->GetTimeSeconds() : -1.0;
	// The detectors and the log capture work only while a run uses the game: in the user's own Play sessions nothing
	// reads what they find, and on a big level their sweeps over every actor cost frames (IsRunUsingGame)
	const bool bDetect = IsRunUsingGame();
	if (bDetect && !bDetectorsOn)
	{
		Detectors->Reset();  // (afresh: what was seen before this run isn't this run's)
	}
	bDetectorsOn = bDetect;
	LogCapture->bEnabled = World != nullptr && bDetect;
	TArray<FPlaytestLogCapture::FLine> Lines = LogCapture->Take();
	if (Lines.Num() && bDetect)
	{
		int32 Sent = 0;
		for (const FPlaytestLogCapture::FLine& Line : Lines)
		{
			if (++Sent > MaxLogEventsPerSecond)
			{
				break;  // a flood: the detector below still counts every line
			}
			TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
			Event->SetStringField(TEXT("kind"), TEXT("log"));
			Event->SetField(TEXT("t"), GameTimeJson(GameNow));
			Event->SetStringField(TEXT("verbosity"), Line.Verbosity);
			Event->SetStringField(TEXT("category"), Line.Category);
			Event->SetStringField(TEXT("message"), Line.Message.Left(2000));
			QueueEvent(Event);
		}
		Detectors->OnLogLines(Lines, GameNow);
	}

	if (World && bDetect)
	{
		Detectors->Tick(World, RealDelta);
	}

	if (T - LastHeartbeat >= 1.0)
	{
		LastHeartbeat = T;
		TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("kind"), TEXT("heartbeat"));
		Event->SetField(TEXT("t"), GameTimeJson(GameNow));
		Event->SetNumberField(TEXT("fps"), FMath::RoundToDouble(DeltaTime > 0 ? 1.0 / DeltaTime : 0.0));
		Event->SetBoolField(TEXT("paused"), World && World->IsPaused());
		QueueEvent(Event);
	}

	FlushEvents();
	return true;
}
