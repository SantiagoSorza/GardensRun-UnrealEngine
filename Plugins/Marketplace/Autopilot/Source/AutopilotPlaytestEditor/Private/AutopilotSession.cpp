#include "AutopilotSession.h"

#include "Async/Async.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "AutopilotStyle.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlaytestSubsystem.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/EditorExperimentalSettings.h"

#define LOCTEXT_NAMESPACE "Autopilot"

DEFINE_LOG_CATEGORY_STATIC(LogAutopilotSession, Log, All);

namespace
{
	constexpr int32 MaxFeedLines = 2000;
	constexpr double LateEventSeconds = 10.0;  // an event this soon after a run's result still belongs to it
	constexpr float SaveEverySeconds = 2.f;    // the runs changed are saved at most this often
	constexpr double LetGoAfterSeconds = 10.0;  // with no run this long, the editor slows down in the background again

	const TCHAR* const RolloutPrefix = TEXT("Trying the harness (run ");

	TArray<FString> Strings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				FString S;
				if (Item.IsValid() && Item->TryGetString(S) && !S.TrimStartAndEnd().IsEmpty())
				{
					Out.Add(S.TrimStartAndEnd());
				}
			}
		}
		return Out;
	}

	/** The result's direct changes to the game, each as the brain words it (docs/protocol.md, Runs: `changes`). */
	TArray<FString> ChangeTexts(const TSharedPtr<FJsonObject>& Result)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Result.IsValid() && Result->TryGetArrayField(TEXT("changes"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* Change = nullptr;
				FString Text;
				if (Item.IsValid() && Item->TryGetObject(Change) && Change && (*Change)->TryGetStringField(TEXT("text"), Text)
					&& !Text.TrimStartAndEnd().IsEmpty())
				{
					Out.Add(Text.TrimStartAndEnd());
				}
			}
		}
		return Out;
	}

	bool IsPluginNotice(const FString& Line)
	{
		return Line.StartsWith(TEXT("Started ")) || Line == TEXT("Stopping…") || Line == TEXT("Disconnected")
			|| Line.StartsWith(TEXT("Connection closed")) || Line.StartsWith(TEXT("Connected to ")) || Line.StartsWith(TEXT("Not connected"))
			|| Line.StartsWith(TEXT("Resetting memory")) || Line.StartsWith(TEXT("Memory reset"))
			|| Line.StartsWith(TEXT("Memory not reset")) || Line.StartsWith(TEXT("A run is going"));
	}

	/** The agent service's last status line before the result (part of the report). */
	const TCHAR* const SavingLine = TEXT("Saving what it learned to memory");

}

const TCHAR* const FAutopilotSession::MemoryLine = TEXT("Memory updated");
const TCHAR* const FAutopilotSession::ResetBackup = TEXT(".reset-backup");
FString FAutopilotSession::HistoryDirOverride;

namespace
{
	TOptional<bool> HeldBreakOnExceptions;  // the user's setting while Autopilot holds it off
}

void FAutopilotSession::HoldExceptionBreaks()
{
	UEditorExperimentalSettings* Settings = GetMutableDefault<UEditorExperimentalSettings>();
	if (Settings && !HeldBreakOnExceptions.IsSet() && Settings->bBreakOnExceptions)
	{
		HeldBreakOnExceptions = Settings->bBreakOnExceptions;
		Settings->bBreakOnExceptions = false;  // (not saved: the user's setting stays theirs)
	}
}

void FAutopilotSession::ReleaseExceptionBreaks()
{
	if (HeldBreakOnExceptions.IsSet())
	{
		if (UEditorExperimentalSettings* Settings = GetMutableDefault<UEditorExperimentalSettings>())
		{
			Settings->bBreakOnExceptions = HeldBreakOnExceptions.GetValue();
		}
		HeldBreakOnExceptions.Reset();
	}
}

FString FAutopilotSession::NoRecordingWhy(const TSharedPtr<FJsonObject>& Result)
{
	if (!Result.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject>* Recording = nullptr;
	FString Video;
	if (Result->TryGetObjectField(TEXT("recording"), Recording) && Recording && Recording->IsValid()
		&& (*Recording)->TryGetStringField(TEXT("video"), Video) && !Video.IsEmpty())
	{
		return FString();  // (it has one: the player shows)
	}
	FString Why;
	if (Result->TryGetStringField(TEXT("no_recording"), Why))
	{
		Why.TrimStartAndEndInline();
		if (!Why.IsEmpty())
		{
			return Why;
		}
	}
	return FString();
}

// -- FAutopilotRun ---------------------------------------------------------------------------------------------------

double FAutopilotRun::Elapsed() const
{
	return FMath::Max(0.0, (bDone && EndSeconds >= 0 ? EndSeconds : FPlatformTime::Seconds()) - StartSeconds);
}

void FAutopilotRun::EnterStage(EAutopilotStage InStage)
{
	if (InStage == EAutopilotStage::None || Stage == EAutopilotStage::Report || InStage == Stage)
	{
		return;  // (the report is the last stage: nothing moves it back)
	}
	Stage = InStage;
	const int32 At = (int32)InStage;
	if (At >= 0 && At < 8 && StageAt[At] < 0.0)
	{
		StageAt[At] = Elapsed();
	}
}

double FAutopilotRun::StageSeconds(int32 Index) const
{
	if (Index <= 0 || Index >= 8 || StageAt[Index] < 0.0)
	{
		return -1.0;
	}
	// (until the next stage that began, else until now or the end)
	for (int32 Next = Index + 1; Next < 8; ++Next)
	{
		if (StageAt[Next] >= 0.0)
		{
			return FMath::Max(0.0, StageAt[Next] - StageAt[Index]);
		}
	}
	return FMath::Max(0.0, Elapsed() - StageAt[Index]);
}

double FAutopilotRun::Took() const
{
	return ResultSeconds >= 0 ? ResultSeconds : Elapsed();
}

int32 FAutopilotRun::EngineAlerts() const
{
	int32 Count = 0;
	for (const TSharedPtr<FAutopilotFinding>& F : Findings)
	{
		Count += F->IsEngine() ? 1 : 0;
	}
	return Count;
}

double FAutopilotRun::MedianStepMs() const
{
	if (StepMs.Num() == 0)
	{
		return -1.0;
	}
	TArray<float> Sorted = StepMs;
	Sorted.Sort();
	return Sorted[Sorted.Num() / 2];
}

EAutopilotTone FAutopilotRun::Tone() const
{
	if (!bDone)
	{
		return EAutopilotTone::Accent;  // (LIVE)
	}
	if (Verdict == TEXT("CONFIRMED") || Verdict == TEXT("GOAL HIT"))
	{
		return EAutopilotTone::Success;
	}
	if (Verdict == TEXT("DENIED") || Verdict == TEXT("GOAL MISSED"))
	{
		return EAutopilotTone::Red;
	}
	if (Verdict == TEXT("EXPLORED") || Verdict == TEXT("GAME TESTED"))
	{
		return EAutopilotTone::Info;
	}
	if (Verdict == TEXT("NO RESULT") || Verdict == TEXT("STOPPED"))
	{
		return EAutopilotTone::Neutral;
	}
	return EAutopilotTone::Amber;  // (INCONCLUSIVE)
}

FLinearColor FAutopilotRun::VerdictColor() const
{
	return FAutopilotStyle::Tone(Tone()).Solid;
}

FLinearColor FAutopilotRun::ColorOf(const FString& Action) const
{
	const int32* Index = ActionColors.Find(Action);
	return Index ? FAutopilotStyle::ActionColor(*Index) : AutopilotColor::Text2();
}

FString FAutopilotRun::KindWord() const
{
	if (Kind == TEXT("verify"))
	{
		return TEXT("VERIFY");
	}
	if (Kind == TEXT("explore"))
	{
		return TEXT("EXPLORE");
	}
	if (Kind == TEXT("goal"))
	{
		return TEXT("GOAL");
	}
	return TEXT("RUN");
}

FString FAutopilotRun::OriginLine() const
{
	if (StartedBy.IsEmpty())
	{
		return FString();
	}
	return StartedBy == TEXT("claude-code") ? TEXT("Started from Claude Code") : StartedBy == TEXT("codex") ? TEXT("Started from Codex")
		: TEXT("Started from your coding agent");
}

FString FAutopilotRun::ShownPrompt() const
{
	if (!Prompt.IsEmpty())
	{
		return Prompt;
	}
	return Kind == TEXT("explore") ? TEXT("Explore") : Kind == TEXT("goal") ? TEXT("Play a goal") : Kind == TEXT("verify") ? TEXT("Verify") : TEXT("Run");
}

FString FAutopilotRun::IdLine() const
{
	return FString::Printf(TEXT("RUN %s // %s // %s"), *RunId, *KindWord(), *FAutopilotSession::When(Started));
}

FString FAutopilotRun::HistoryLine() const
{
	const int32 N = Attempts();
	return FString::Printf(TEXT("%s · %s · %d %s"), *FAutopilotSession::When(Started), *FAutopilotSession::Clock(Took()), N, N == 1 ? TEXT("attempt") : TEXT("attempts"));
}

FString FAutopilotRun::MetaLine() const
{
	const int32 N = Attempts();
	return FString::Printf(TEXT("RUN %s // %s // DURATION %s // %d %s"), *RunId, *KindWord(), *FAutopilotSession::Clock(bDone ? Took() : Elapsed()), N,
		N == 1 ? TEXT("ATTEMPT") : TEXT("ATTEMPTS"));
}

FString FAutopilotRun::ReportMarkdown() const
{
	// (the same as Unity's and Godot's: header, prompt, verdict, duration and counts, alerts as quotes, then the reason,
	// the evidence, the direct changes and the findings)
	FString Out = FString::Printf(TEXT("# Autopilot run %s\n\n"), *RunId);
	Out += FString::Printf(TEXT("**Prompt:** %s\n\n"), *ShownPrompt());
	Out += FString::Printf(TEXT("**Verdict:** %s  \n"), *Verdict);
	Out += FString::Printf(TEXT("**Duration:** %s · **Harnesses:** %d · **Policy steps:** %d · **Alerts:** %d\n"),
		*FAutopilotSession::Clock(Took()), Attempts(), Steps.Num(), EngineAlerts());
	for (const FString& Alert : Alerts)
	{
		Out += FString::Printf(TEXT("\n> %s\n"), *FAutopilotSession::Scrub(Alert));
	}
	if (!Reason.IsEmpty())
	{
		Out += FString::Printf(TEXT("\n## Reason\n\n%s\n"), *FAutopilotSession::Scrub(Reason));
	}
	if (Evidence.Num())
	{
		Out += TEXT("\n## Evidence\n\n");
		for (const FString& Line : Evidence)
		{
			Out += FString::Printf(TEXT("- %s\n"), *Line);
		}
	}
	if (Changes.Num())
	{
		Out += TEXT("\n## Changed directly (not through play)\n\n");
		for (const FString& Line : Changes)
		{
			Out += FString::Printf(TEXT("- %s\n"), *Line);
		}
	}
	// The agent's (its notes, then its findings' summaries: once each), then the engine's (Unity's form)
	TArray<FString> Agent;
	for (const FString& Note : AgentNotes)
	{
		const FString Said = FAutopilotSession::Scrub(Note);
		if (!Said.TrimStartAndEnd().IsEmpty())
		{
			Agent.AddUnique(Said);
		}
	}
	int32 EngineCount = 0;
	for (const TSharedPtr<FAutopilotFinding>& F : Findings)
	{
		if (!F->IsEngine())
		{
			const FString Said = FAutopilotSession::Scrub(F->Summary);
			if (!Said.TrimStartAndEnd().IsEmpty())
			{
				Agent.AddUnique(Said);
			}
		}
		else
		{
			++EngineCount;
		}
	}
	if (Agent.Num() || EngineCount)
	{
		Out += TEXT("\n## Findings\n\n");
		for (const FString& Note : Agent)
		{
			Out += FString::Printf(TEXT("- [agent] %s\n"), *Note);
		}
		for (const TSharedPtr<FAutopilotFinding>& F : Findings)
		{
			if (!F->IsEngine())
			{
				continue;
			}
			FString Line = FString::Printf(TEXT("- [engine · %s · %s] %s"), *F->Severity, *F->Detector, *FAutopilotSession::Scrub(F->Summary));
			if (F->Count > 1)
			{
				Line += FString::Printf(TEXT(" (\u00d7%d)"), F->Count);
			}
			if (F->bHasWhere)
			{
				Line += FString::Printf(TEXT(" at %.0f, %.0f, %.0f cm"), F->Where.X, F->Where.Y, F->Where.Z);
			}
			if (F->bDismissed)
			{
				Line += TEXT(" (not a bug)");
			}
			Out += Line + TEXT("\n");
		}
	}
	return Out;
}

// -- FAutopilotSession -----------------------------------------------------------------------------------------------

FAutopilotSession& FAutopilotSession::Get()
{
	static FAutopilotSession Session;
	return Session;
}

void FAutopilotSession::Startup()
{
	LoadHistory();
	if (!SaveTicker.IsValid())
	{
		SaveTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			SaveHistory();
			return true;
		}), SaveEverySeconds);
	}
	if (GEditor && !ThrottleHandle.IsValid())
	{
		UEditorEngine::FShouldDisableCPUThrottling KeepPlaying = UEditorEngine::FShouldDisableCPUThrottling::CreateLambda(
			[this]() { return KeepsPlaying(); });
		ThrottleHandle = KeepPlaying.GetHandle();
		GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(KeepPlaying));
	}
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B || Bound.Get() == B)
	{
		return;
	}
	Bound = B;
	StatusHandle = B->OnStatus.AddRaw(this, &FAutopilotSession::OnStatus);
	FindingHandle = B->OnFinding.AddRaw(this, &FAutopilotSession::OnFinding);
	ResultHandle = B->OnResult.AddRaw(this, &FAutopilotSession::OnResult);
	JevHandle = B->OnJevCall.AddRaw(this, &FAutopilotSession::OnJevCall);
	ConnectionHandle = B->OnConnectionChanged.AddRaw(this, &FAutopilotSession::OnConnection);
	UsageHandle = B->OnUsage.AddRaw(this, &FAutopilotSession::OnUsage);
	BillHandle = B->OnBill.AddRaw(this, &FAutopilotSession::OnBill);
	RunStartedHandle = B->OnRunStarted.AddRaw(this, &FAutopilotSession::OnRunStarted);
	RunKindHandle = B->OnRunKind.AddRaw(this, &FAutopilotSession::OnRunKind);
	ActivityHandle = B->OnActivity.AddRaw(this, &FAutopilotSession::OnActivity);
	MemoryClearedHandle = B->OnMemoryCleared.AddRaw(this, &FAutopilotSession::OnMemoryCleared);
	B->ActivitySince = [this]() { return ActivitySince(); };  // (each hello asks for the feed from where this run's is)
}

void FAutopilotSession::Shutdown()
{
	if (UObjectInitialized())
	{
		if (UPlaytestSubsystem* B = Bound.Get())
		{
			B->OnStatus.Remove(StatusHandle);
			B->OnFinding.Remove(FindingHandle);
			B->OnResult.Remove(ResultHandle);
			B->OnJevCall.Remove(JevHandle);
			B->OnConnectionChanged.Remove(ConnectionHandle);
			B->OnUsage.Remove(UsageHandle);
			B->OnBill.Remove(BillHandle);
			B->OnRunStarted.Remove(RunStartedHandle);
			B->OnRunKind.Remove(RunKindHandle);
			B->OnActivity.Remove(ActivityHandle);
			B->OnMemoryCleared.Remove(MemoryClearedHandle);
			B->ActivitySince = nullptr;
		}
	}
	Bound.Reset();
	if (SaveTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SaveTicker);
		SaveTicker.Reset();
	}
	if (ThrottleHandle.IsValid())
	{
		if (GEditor)
		{
			GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
				[this](const UEditorEngine::FShouldDisableCPUThrottling& D) { return D.GetHandle() == ThrottleHandle; });
		}
		ThrottleHandle.Reset();
	}
	LastPlayed = -1.0;
	SaveHistory();
	Runs.Reset();  // (before the module goes, not at static destruction)
	SetAside.Reset();
	Unsaved.Reset();
	KeptSteps = 0;
	bHistoryLoaded = false;
}

// -- Playing in the background -----------------------------------------------------------------------------------

bool FAutopilotSession::KeepsPlaying()
{
	// While a run plays, the editor doesn't slow down: not in the background (Editor Preferences' "Use Less CPU when in
	// Background", on by default, drops an unfocused editor to a few frames a second: the agent plays at that pace and
	// its recording has a frame a second, too few to see an animation), not minimized (the editor then all but stops,
	// and with it the run's connection). The engine asks every frame (UEditorEngine::ShouldThrottleCPUUsage, its
	// ShouldDisableCPUThrottlingDelegates); the user's preferences are never changed. A while after a run (a reconnect
	// is not the end) the editor slows down again. A minimized window still draws no picture: such a run says so.
	const double Now = FPlatformTime::Seconds();
	if (GetActiveRun().IsValid())
	{
		if (LastPlayed < 0.0)
		{
			UE_LOG(LogAutopilotSession, Display, TEXT("[Autopilot] The editor keeps its pace in the background while the run plays; it slows down in the background again when the run ends."));
		}
		LastPlayed = Now;
		return true;
	}
	if (LastPlayed >= 0.0 && Now - LastPlayed >= LetGoAfterSeconds)
	{
		LastPlayed = -1.0;
	}
	return LastPlayed >= 0.0;
}

// -- The saved history -------------------------------------------------------------------------------------------

FString FAutopilotSession::HistoryDir()
{
	return HistoryDirOverride.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Autopilot"), TEXT("Runs")) : HistoryDirOverride;
}

TSharedRef<FJsonObject> FAutopilotSession::RunToJson(const FAutopilotRun& Run)
{
	auto StringArray = [](const TArray<FString>& Lines)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Line : Lines)
		{
			Out.Add(MakeShared<FJsonValueString>(Line));
		}
		return Out;
	};
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("version"), 2);  // (2: the Results stage, before Report)
	O->SetStringField(TEXT("kind"), Run.Kind);
	O->SetStringField(TEXT("prompt"), Run.Prompt);
	if (Run.Frame.IsValid())
	{
		O->SetObjectField(TEXT("frame"), Run.Frame);
	}
	if (!Run.StartedBy.IsEmpty())
	{
		O->SetStringField(TEXT("started_by"), Run.StartedBy);
	}
	O->SetStringField(TEXT("started"), Run.Started.ToIso8601());
	O->SetNumberField(TEXT("elapsed"), Run.Elapsed());
	O->SetBoolField(TEXT("done"), Run.bDone);
	O->SetNumberField(TEXT("stage"), (int32)Run.Stage);
	O->SetNumberField(TEXT("attempt"), Run.Attempt);
	O->SetStringField(TEXT("now"), Run.Now);
	O->SetStringField(TEXT("verdict"), Run.Verdict);
	O->SetStringField(TEXT("reason"), Run.Reason);
	O->SetArrayField(TEXT("evidence"), StringArray(Run.Evidence));
	O->SetArrayField(TEXT("notes"), StringArray(Run.AgentNotes));
	O->SetArrayField(TEXT("changes"), StringArray(Run.Changes));
	if (Run.Alerts.Num())
	{
		O->SetArrayField(TEXT("alerts"), StringArray(Run.Alerts));
	}
	O->SetStringField(TEXT("report_dir"), Run.ReportDir);
	O->SetStringField(TEXT("run_id"), Run.RunId);
	O->SetNumberField(TEXT("result_seconds"), Run.ResultSeconds);
	O->SetNumberField(TEXT("result_attempts"), Run.ResultAttempts);
	O->SetNumberField(TEXT("step_count"), Run.StepCount);
	O->SetNumberField(TEXT("step_attempts"), Run.StepAttempts);
	O->SetNumberField(TEXT("median_step_ms"), Run.MedianStepMs());
	if (!Run.Recording.IsEmpty())
	{
		TSharedRef<FJsonObject> Recording = MakeShared<FJsonObject>();
		Recording->SetStringField(TEXT("video"), Run.Recording);
		Recording->SetNumberField(TEXT("seconds"), Run.RecordingSeconds);
		O->SetObjectField(TEXT("recording"), Recording);
	}
	else if (!Run.NoRecording.IsEmpty())
	{
		O->SetStringField(TEXT("no_recording"), Run.NoRecording);
	}

	TArray<TSharedPtr<FJsonValue>> Findings;
	for (const TSharedPtr<FAutopilotFinding>& F : Run.Findings)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("signature"), F->Signature);
		J->SetStringField(TEXT("severity"), F->Severity);
		J->SetStringField(TEXT("summary"), F->Summary);
		J->SetStringField(TEXT("detector"), F->Detector);
		J->SetStringField(TEXT("actor"), F->Actor);
		J->SetStringField(TEXT("source"), F->Source);
		J->SetNumberField(TEXT("count"), F->Count);
		J->SetNumberField(TEXT("t"), F->GameTime);
		J->SetBoolField(TEXT("dismissed"), F->bDismissed);
		if (F->bHasWhere)
		{
			TSharedRef<FJsonObject> Where = MakeShared<FJsonObject>();
			Where->SetNumberField(TEXT("X"), F->Where.X);
			Where->SetNumberField(TEXT("Y"), F->Where.Y);
			Where->SetNumberField(TEXT("Z"), F->Where.Z);
			J->SetObjectField(TEXT("where"), Where);
		}
		Findings.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("findings"), Findings);

	TArray<TSharedPtr<FJsonValue>> Feed;
	for (const TSharedPtr<FAutopilotFeedLine>& Line : Run.Feed)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("t"), Line->Elapsed);
		J->SetStringField(TEXT("text"), Line->Text);
		J->SetNumberField(TEXT("kind"), (int32)Line->Kind);
		J->SetNumberField(TEXT("attempt"), Line->Attempt);
		J->SetNumberField(TEXT("finding"), Line->Finding.IsValid() ? Run.Findings.IndexOfByKey(Line->Finding) : INDEX_NONE);
		J->SetNumberField(TEXT("stage"), (int32)Line->Stage);
		if (Line->ActivityId > 0)
		{
			// (an activity event: its fields, so a reopened run shows the feed as it was, folds included)
			J->SetNumberField(TEXT("aid"), Line->ActivityId);
			J->SetStringField(TEXT("akind"), Line->ActivityKind);
			J->SetNumberField(TEXT("agent_t"), Line->AgentT);
			if (!Line->Detail.IsEmpty())
			{
				J->SetStringField(TEXT("detail"), Line->Detail);
			}
			if (Line->ResultId > 0)
			{
				J->SetNumberField(TEXT("rid"), Line->ResultId);
				J->SetStringField(TEXT("result"), Line->Result);
				if (!Line->ResultDetail.IsEmpty())
				{
					J->SetStringField(TEXT("rdetail"), Line->ResultDetail);
				}
			}
		}
		Feed.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("feed"), Feed);
	if (Run.LastActivityId > 0)
	{
		O->SetNumberField(TEXT("last_activity"), Run.LastActivityId);
	}
	if (Run.Usage.Num())
	{
		TSharedRef<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetBoolField(TEXT("final"), Run.bUsageFinal);
		Usage->SetArrayField(TEXT("models"), PlaytestUsage::ToJson(Run.Usage));
		O->SetObjectField(TEXT("usage"), Usage);
	}
	if (Run.Bill.bKnown)
	{
		O->SetObjectField(TEXT("bill"), PlaytestAccount::BillToJson(Run.Bill));
	}
	return O;
}

TSharedPtr<FAutopilotRun> FAutopilotSession::RunFromJson(const TSharedPtr<FJsonObject>& O)
{
	FString Started;
	if (!O.IsValid() || !O->TryGetStringField(TEXT("started"), Started))
	{
		return nullptr;
	}
	auto Number = [&O](const TCHAR* Field, double Default)
	{
		double Value = Default;
		O->TryGetNumberField(Field, Value);
		return Value;
	};
	TSharedPtr<FAutopilotRun> Run = MakeShared<FAutopilotRun>();
	if (!FDateTime::ParseIso8601(*Started, Run->Started))
	{
		return nullptr;
	}
	O->TryGetStringField(TEXT("kind"), Run->Kind);
	O->TryGetStringField(TEXT("prompt"), Run->Prompt);
	const TSharedPtr<FJsonObject>* Frame = nullptr;
	if (O->TryGetObjectField(TEXT("frame"), Frame) && Frame)
	{
		Run->Frame = *Frame;
	}
	O->TryGetStringField(TEXT("started_by"), Run->StartedBy);
	// The clock of a saved run: it started at 0 and ended at what it took
	Run->StartSeconds = 0.0;
	Run->EndSeconds = FMath::Max(0.0, Number(TEXT("elapsed"), 0.0));
	Run->bDone = true;
	int32 Stage = (int32)Number(TEXT("stage"), 0);
	if (Number(TEXT("version"), 1) < 2 && Stage >= (int32)EAutopilotStage::Results)
	{
		++Stage;  // (kept before the Results stage: its Report was where Results is now)
	}
	Run->Stage = (EAutopilotStage)FMath::Clamp(Stage, 0, (int32)EAutopilotStage::Report);
	Run->Attempt = (int32)Number(TEXT("attempt"), 0);
	O->TryGetStringField(TEXT("now"), Run->Now);
	O->TryGetStringField(TEXT("verdict"), Run->Verdict);
	O->TryGetStringField(TEXT("reason"), Run->Reason);
	Run->Evidence = Strings(O, TEXT("evidence"));
	Run->AgentNotes = Strings(O, TEXT("notes"));
	Run->Changes = Strings(O, TEXT("changes"));
	Run->Alerts = Strings(O, TEXT("alerts"));  // (runs saved before alerts: none)
	O->TryGetStringField(TEXT("report_dir"), Run->ReportDir);
	O->TryGetStringField(TEXT("run_id"), Run->RunId);
	Run->ResultSeconds = Number(TEXT("result_seconds"), -1.0);
	Run->ResultAttempts = (int32)Number(TEXT("result_attempts"), -1.0);
	Run->StepCount = (int32)Number(TEXT("step_count"), 0);
	Run->StepAttempts = (int32)Number(TEXT("step_attempts"), 0);
	const double MedianMs = Number(TEXT("median_step_ms"), -1.0);
	if (MedianMs >= 0)
	{
		Run->StepMs.Add((float)MedianMs);
	}
	// Its recording (runs saved before recordings, and runs with none, have none)
	const TSharedPtr<FJsonObject>* Recording = nullptr;
	if (O->TryGetObjectField(TEXT("recording"), Recording) && Recording && Recording->IsValid())
	{
		(*Recording)->TryGetStringField(TEXT("video"), Run->Recording);
		(*Recording)->TryGetNumberField(TEXT("seconds"), Run->RecordingSeconds);
		// Its copy in the run's folder, when there is one: the editor may have closed before the copy was saved here
		const FString Copied = Run->ReportDir.IsEmpty() ? FString() : FPaths::Combine(Run->ReportDir, TEXT("recording.mp4"));
		if (!Copied.IsEmpty() && FPaths::FileExists(Copied))
		{
			Run->Recording = Copied;
		}
	}
	// Or why it has none (runs saved before this was kept: nothing said)
	if (Run->Recording.IsEmpty())
	{
		O->TryGetStringField(TEXT("no_recording"), Run->NoRecording);
	}

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (O->TryGetArrayField(TEXT("findings"), Items) && Items)
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject>* J = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(J) || !J)
			{
				continue;
			}
			TSharedPtr<FAutopilotFinding> F = MakeShared<FAutopilotFinding>();
			(*J)->TryGetStringField(TEXT("signature"), F->Signature);
			(*J)->TryGetStringField(TEXT("severity"), F->Severity);
			(*J)->TryGetStringField(TEXT("summary"), F->Summary);
			(*J)->TryGetStringField(TEXT("detector"), F->Detector);
			(*J)->TryGetStringField(TEXT("actor"), F->Actor);
			(*J)->TryGetStringField(TEXT("source"), F->Source);
			double Count = 1;
			(*J)->TryGetNumberField(TEXT("count"), Count);
			F->Count = FMath::Max(1, (int32)Count);
			(*J)->TryGetNumberField(TEXT("t"), F->GameTime);
			(*J)->TryGetBoolField(TEXT("dismissed"), F->bDismissed);
			const TSharedPtr<FJsonObject>* Where = nullptr;
			if ((*J)->TryGetObjectField(TEXT("where"), Where) && Where && Where->IsValid())
			{
				F->bHasWhere = true;
				F->Where = FVector((*Where)->GetNumberField(TEXT("X")), (*Where)->GetNumberField(TEXT("Y")), (*Where)->GetNumberField(TEXT("Z")));
			}
			Run->Findings.Add(F);
		}
	}
	if (O->TryGetArrayField(TEXT("feed"), Items) && Items)
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject>* J = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(J) || !J)
			{
				continue;
			}
			TSharedPtr<FAutopilotFeedLine> Line = MakeShared<FAutopilotFeedLine>();
			(*J)->TryGetNumberField(TEXT("t"), Line->Elapsed);
			(*J)->TryGetStringField(TEXT("text"), Line->Text);
			double Kind = 0, Attempt = 0, Finding = INDEX_NONE;
			(*J)->TryGetNumberField(TEXT("kind"), Kind);
			(*J)->TryGetNumberField(TEXT("attempt"), Attempt);
			(*J)->TryGetNumberField(TEXT("finding"), Finding);
			Line->Kind = (EAutopilotFeedKind)FMath::Clamp((int32)Kind, 0, (int32)EAutopilotFeedKind::Activity);
			Line->Attempt = (int32)Attempt;
			if (Run->Findings.IsValidIndex((int32)Finding))
			{
				Line->Finding = Run->Findings[(int32)Finding];
			}
			// The stage it belongs to, and an activity event's own fields (runs saved before them: none)
			double LineStage = 0, ActivityId = 0, ResultId = 0;
			(*J)->TryGetNumberField(TEXT("stage"), LineStage);
			Line->Stage = (EAutopilotStage)FMath::Clamp((int32)LineStage, 0, (int32)EAutopilotStage::Report);
			if ((*J)->TryGetNumberField(TEXT("aid"), ActivityId))
			{
				Line->ActivityId = (int32)ActivityId;
				(*J)->TryGetStringField(TEXT("akind"), Line->ActivityKind);
				(*J)->TryGetNumberField(TEXT("agent_t"), Line->AgentT);
				(*J)->TryGetStringField(TEXT("detail"), Line->Detail);
				(*J)->TryGetStringField(TEXT("result"), Line->Result);
				(*J)->TryGetStringField(TEXT("rdetail"), Line->ResultDetail);
				(*J)->TryGetNumberField(TEXT("rid"), ResultId);
				Line->ResultId = (int32)ResultId;
			}
			Run->Feed.Add(Line);
		}
	}
	double LastActivity = 0;
	O->TryGetNumberField(TEXT("last_activity"), LastActivity);
	Run->LastActivityId = (int32)LastActivity;

	// The tokens its models used (runs saved before usage was counted have none)
	const TSharedPtr<FJsonObject>* Usage = nullptr;
	if (O->TryGetObjectField(TEXT("usage"), Usage) && Usage)
	{
		Run->Usage = PlaytestUsage::Parse(*Usage, &Run->bUsageFinal);
	}
	// What Autopilot was billed for it through Aura (runs saved before the bill, and BYOK runs, have none)
	const TSharedPtr<FJsonObject>* Bill = nullptr;
	if (O->TryGetObjectField(TEXT("bill"), Bill) && Bill)
	{
		Run->Bill = PlaytestAccount::BillFromJson(*Bill);
	}

	bool bDone = true;
	O->TryGetBoolField(TEXT("done"), bDone);
	if (!bDone)
	{
		// Live when the editor closed: its result never came here
		Run->Verdict = TEXT("NO RESULT");
		Run->Reason = TEXT("The editor closed before the result came.");
		TSharedPtr<FAutopilotFeedLine> Line = MakeShared<FAutopilotFeedLine>();
		Line->Elapsed = Run->EndSeconds;
		Line->Text = TEXT("Editor closed: no result");
		Line->Kind = EAutopilotFeedKind::Notice;
		Line->Attempt = Run->Attempt;
		Run->Feed.Add(Line);
	}
	return Run;
}

void FAutopilotSession::Forget()
{
	Runs.Reset();
	SetAside.Reset();
	Retired.Reset();
	Unsaved.Reset();
	KeptSteps = 0;
	bHistoryLoaded = true;  // (nothing to load: it is gone)
	bLedgerLoaded = true;
	++UsageRevision;
	Changed();
	const FString Saved = FPaths::ProjectSavedDir();
	for (const TCHAR* Dir : { TEXT("Autopilot"), TEXT("Playtest"), TEXT("AutopilotCodex") })
	{
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(Saved, Dir), false, true);
	}
	for (const TCHAR* File : { TEXT("AutopilotCodexExport.log"), TEXT("AutopilotCodexExport.progress") })
	{
		IFileManager::Get().Delete(*FPaths::Combine(Saved, TEXT("Logs"), File), false, true, true);
	}
}

void FAutopilotSession::LoadHistory()
{
	if (bHistoryLoaded)
	{
		return;
	}
	bHistoryLoaded = true;
	const FString Dir = HistoryDir();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.json")), true, false);
	TArray<TSharedPtr<FAutopilotRun>> Loaded;
	TArray<TSharedPtr<FAutopilotRun>> Ended;  // live when the editor closed: saved again with their NO RESULT
	for (const FString& File : Files)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, File))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object))
		{
			continue;
		}
		if (TSharedPtr<FAutopilotRun> Run = RunFromJson(Object))
		{
			Run->HistoryFile = File;
			bool bDone = true;
			if (Object->TryGetBoolField(TEXT("done"), bDone) && !bDone)
			{
				Ended.Add(Run);
			}
			Loaded.Add(Run);
		}
	}
	Loaded.Sort([](const TSharedPtr<FAutopilotRun>& A, const TSharedPtr<FAutopilotRun>& B) { return A->Started < B->Started; });
	// The usage ledger first: a run going past MaxSavedRuns now joins it
	LoadUsageLedger();
	// Only the newest are kept (the others' usage and bill stay in the ledger)
	while (Loaded.Num() > MaxSavedRuns)
	{
		Retire(Loaded[0]);
		IFileManager::Get().Delete(*FPaths::Combine(Dir, Loaded[0]->HistoryFile), false, true, true);
		Loaded.RemoveAt(0);
	}
	TArray<TSharedPtr<FAutopilotRun>> Live = MoveTemp(Runs);  // (a run that began before the history loaded stays newest)
	Runs.Reset();
	for (const TSharedPtr<FAutopilotRun>& Run : Loaded)
	{
		Run->Id = NextId++;
		if (Ended.Contains(Run))
		{
			Unsaved.Add(Run->Id);
		}
		Runs.Add(Run);
	}
	Runs.Append(Live);
	// The runs a memory reset took off the tab: only the Settings page counts them
	SetAside.Reset();
	TArray<FString> Resets;
	IFileManager::Get().FindFiles(Resets, *FPaths::Combine(Dir, ResetBackup, TEXT("*")), false, true);
	for (const FString& Reset : Resets)
	{
		const FString Folder = FPaths::Combine(Dir, ResetBackup, Reset);
		TArray<FString> Kept;
		IFileManager::Get().FindFiles(Kept, *FPaths::Combine(Folder, TEXT("*.json")), true, false);
		for (const FString& File : Kept)
		{
			FString Text;
			TSharedPtr<FJsonObject> Object;
			if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File))
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object))
			{
				if (TSharedPtr<FAutopilotRun> Run = RunFromJson(Object))
				{
					Run->Id = NextId++;
					SetAside.Add(Run);
				}
			}
		}
	}
	SetAside.Sort([](const TSharedPtr<FAutopilotRun>& A, const TSharedPtr<FAutopilotRun>& B) { return A->Started < B->Started; });
	if (Loaded.Num() || SetAside.Num() || Retired.Num())
	{
		++UsageRevision;
		Changed();
	}
}

FString FAutopilotSession::UsageLedgerPath()
{
	return FPaths::Combine(HistoryDir(), TEXT(".usage"), TEXT("ledger.json"));
}

FString FAutopilotSession::UsageKey(const FAutopilotRun& Run)
{
	return Run.RunId + TEXT("|") + Run.Started.ToIso8601();
}

TSharedRef<FJsonObject> FAutopilotSession::UsageToJson(const FAutopilotRun& Run)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("run_id"), Run.RunId);
	O->SetStringField(TEXT("started"), Run.Started.ToIso8601());
	O->SetStringField(TEXT("kind"), Run.Kind);
	O->SetStringField(TEXT("prompt"), Run.Prompt);
	O->SetStringField(TEXT("verdict"), Run.Verdict);
	if (Run.Usage.Num())
	{
		TSharedRef<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetBoolField(TEXT("final"), Run.bUsageFinal);
		Usage->SetArrayField(TEXT("models"), PlaytestUsage::ToJson(Run.Usage));
		O->SetObjectField(TEXT("usage"), Usage);
	}
	if (Run.Bill.bKnown)
	{
		O->SetObjectField(TEXT("bill"), PlaytestAccount::BillToJson(Run.Bill));
	}
	return O;
}

void FAutopilotSession::LoadUsageLedger()
{
	Retired.Reset();
	bLedgerLoaded = true;
	FString Text;
	TSharedPtr<FJsonObject> Object;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!FFileHelper::LoadFileToString(Text, *UsageLedgerPath())
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object.IsValid()
		|| !Object->TryGetArrayField(TEXT("runs"), Items) || !Items)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		const TSharedPtr<FJsonObject>* J = nullptr;
		if (Item.IsValid() && Item->TryGetObject(J) && J)
		{
			if (TSharedPtr<FAutopilotRun> Run = RunFromJson(*J))
			{
				Run->Id = NextId++;
				Retired.Add(Run);
			}
		}
	}
	Retired.Sort([](const TSharedPtr<FAutopilotRun>& A, const TSharedPtr<FAutopilotRun>& B) { return A->Started < B->Started; });
}

void FAutopilotSession::SaveUsageLedger() const
{
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const TSharedPtr<FAutopilotRun>& Run : Retired)
	{
		Items.Add(MakeShared<FJsonValueObject>(UsageToJson(*Run)));
	}
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("version"), 1);
	O->SetArrayField(TEXT("runs"), Items);
	FString Text;
	FJsonSerializer::Serialize(O, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	// Written whole, then put in place (as the runs' files are)
	const FString Path = UsageLedgerPath();
	const FString Temp = Path + TEXT(".tmp");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*Path, *Temp, true, true))
	{
		UE_LOG(LogAutopilotSession, Warning, TEXT("[Autopilot] Couldn't save the usage to %s"), *Path);
	}
}

void FAutopilotSession::Retire(const TSharedPtr<FAutopilotRun>& Run)
{
	if (!Run.IsValid() || (!Run->Usage.Num() && !Run->Bill.bKnown))
	{
		return;
	}
	if (!bLedgerLoaded)
	{
		LoadUsageLedger();
	}
	const FString Key = UsageKey(*Run);
	if (Retired.ContainsByPredicate([&Key](const TSharedPtr<FAutopilotRun>& R) { return UsageKey(*R) == Key; }))
	{
		return;
	}
	Retired.Add(Run);
	Retired.Sort([](const TSharedPtr<FAutopilotRun>& A, const TSharedPtr<FAutopilotRun>& B) { return A->Started < B->Started; });
	while (Retired.Num() > MaxUsageRuns)
	{
		Retired.RemoveAt(0);
	}
	SaveUsageLedger();
	++UsageRevision;
}

void FAutopilotSession::OnMemoryCleared(bool bOk)
{
	if (bOk)
	{
		SetRunsAside();
	}
}

FString FAutopilotSession::SetRunsAside()
{
	LoadHistory();  // (all the runs kept, should the tab not have listed them yet)
	SaveHistory();  // (each as it is now)
	TArray<TSharedPtr<FAutopilotRun>> Ended;
	for (const TSharedPtr<FAutopilotRun>& Run : Runs)
	{
		if (Run->bDone)
		{
			Ended.Add(Run);
		}
	}
	if (!Ended.Num())
	{
		return FString();
	}
	const FString Dir = HistoryDir();
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	FString Backup = FPaths::Combine(Dir, ResetBackup, Stamp);
	for (int32 N = 1; IFileManager::Get().DirectoryExists(*Backup); ++N)  // (two resets in one second)
	{
		Backup = FPaths::Combine(Dir, ResetBackup, FString::Printf(TEXT("%s-%d"), *Stamp, N));
	}
	bool bAny = false;
	for (const TSharedPtr<FAutopilotRun>& Run : Ended)
	{
		const FString From = FPaths::Combine(Dir, Run->HistoryFile);
		if (!Run->HistoryFile.IsEmpty() && IFileManager::Get().FileExists(*From))
		{
			IFileManager::Get().MakeDirectory(*Backup, true);
			if (IFileManager::Get().Move(*FPaths::Combine(Backup, Run->HistoryFile), *From, false, true))
			{
				bAny = true;
			}
			else
			{
				UE_LOG(LogAutopilotSession, Warning, TEXT("[Autopilot] Couldn't move %s aside (listed again at the next start)"), *From);
			}
		}
		Runs.Remove(Run);
		Unsaved.Remove(Run->Id);
		SetAside.Add(Run);
	}
	++UsageRevision;
	Changed();
	return bAny ? Backup : FString();
}

void FAutopilotSession::SaveHistory()
{
	if (!Unsaved.Num())
	{
		return;
	}
	const FString Dir = HistoryDir();
	IFileManager::Get().MakeDirectory(*Dir, true);
	for (const int32 Id : Unsaved)
	{
		const TSharedPtr<FAutopilotRun> Run = FindRun(Id);
		if (!Run.IsValid() || Run->HistoryFile.IsEmpty())
		{
			continue;
		}
		FString Text;
		FJsonSerializer::Serialize(RunToJson(*Run), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		// Written whole, then put in place: a crash mid-write never leaves half a run
		const FString Path = FPaths::Combine(Dir, Run->HistoryFile);
		const FString Temp = Path + TEXT(".tmp");
		if (FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			IFileManager::Get().Move(*Path, *Temp, true, true);
		}
	}
	Unsaved.Reset();
}

TSharedPtr<FAutopilotRun> FAutopilotSession::GetActiveRun() const
{
	for (int32 i = Runs.Num() - 1; i >= 0; --i)
	{
		if (!Runs[i]->bDone)
		{
			return Runs[i];
		}
	}
	return nullptr;
}

TSharedPtr<FAutopilotRun> FAutopilotSession::FindRun(int32 Id) const
{
	for (const TSharedPtr<FAutopilotRun>& Run : Runs)
	{
		if (Run->Id == Id)
		{
			return Run;
		}
	}
	return nullptr;
}

TSharedPtr<FAutopilotRun> FAutopilotSession::NewRun(const FString& Kind, const FString& Prompt)
{
	TSharedPtr<FAutopilotRun> Run = MakeShared<FAutopilotRun>();
	Run->Id = NextId++;
	Run->Kind = Kind;
	Run->Prompt = Prompt;
	Run->Started = FDateTime::Now();
	Run->StartSeconds = FPlatformTime::Seconds();
	Run->RunId = Run->Started.ToString(TEXT("%Y%m%d-%H%M%S"));
	Run->HistoryFile = FString::Printf(TEXT("%s-%d.json"), *Run->RunId, Run->Id);
	Runs.Add(Run);
	++UsageRevision;
	// Only the newest are kept on disk (the others' usage and bill stay in the ledger)
	if (Runs.Num() > MaxSavedRuns)
	{
		const TSharedPtr<FAutopilotRun>& Old = Runs[Runs.Num() - 1 - MaxSavedRuns];
		Retire(Old);
		IFileManager::Get().Delete(*FPaths::Combine(HistoryDir(), Old->HistoryFile), false, true, true);
	}
	Changed(Run.Get());
	return Run;
}

TSharedPtr<FAutopilotRun> FAutopilotSession::StartRun(const FString& Kind, const FString& Prompt, const TSharedRef<FJsonObject>& Frame)
{
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B || !B->IsConnected())
	{
		Say(TEXT("Not connected: connect first"));
		return nullptr;
	}
	ResetLine.Empty();  // (a run after a reset: its word is old)
	Frame->RemoveField(TEXT("turn_based"));  // (runs are always real time: a run saved before says nothing of it again)
	// A run still shown live whose result never came ends here
	if (TSharedPtr<FAutopilotRun> Stale = GetActiveRun())
	{
		Stale->bDone = true;
		Stale->EndSeconds = FPlatformTime::Seconds();
		Stale->Verdict = TEXT("NO RESULT");
		if (Stale->Reason.IsEmpty())
		{
			Stale->Reason = TEXT("The connection to the agent service closed before the result came.");
		}
		Changed(Stale.Get());
	}
	TSharedPtr<FAutopilotRun> Run = NewRun(Kind, Prompt);
	Run->Frame = Frame;
	B->StartRun(Frame);
	return Run;
}

void FAutopilotSession::OnRunStarted(const TSharedPtr<FJsonObject>& Frame)
{
	if (!Frame.IsValid() || GetActiveRun().IsValid())
	{
		return;  // (a run going is the tab's own: the agent service runs one at a time)
	}
	FString Kind;
	FString By;
	FString Words;
	Frame->TryGetStringField(TEXT("kind"), Kind);
	Frame->TryGetStringField(TEXT("by"), By);
	// Its words as the history shows a run's: the claim, the goal; an exploration as the composer's words would start one;
	// words the decision model reads the kind of as typed (its kind blank until run_kind says it)
	if (Kind == TEXT("explore"))
	{
		Frame->TryGetStringField(TEXT("brief"), Words);
		Words = Words.TrimStartAndEnd();
		Words = Words.IsEmpty() ? FString() : FString::Printf(TEXT("Explore: %s"), *Words);
	}
	else if (Kind == TEXT("auto"))
	{
		Frame->TryGetStringField(TEXT("prompt"), Words);
		Words = Words.TrimStartAndEnd();
		Kind.Empty();
	}
	else
	{
		Frame->TryGetStringField(Kind == TEXT("verify") ? TEXT("claim") : TEXT("goal"), Words);
		Words = Words.TrimStartAndEnd();
	}
	ResetLine.Empty();  // (a run after a reset: its word is old)
	TSharedPtr<FAutopilotRun> Run = NewRun(Kind, Words);
	// (Run again sends it from the tab: as a start frame, without who started it)
	TSharedRef<FJsonObject> Sent = MakeShared<FJsonObject>();
	Sent->Values = Frame->Values;
	Sent->RemoveField(TEXT("by"));
	Run->Frame = Sent;
	Run->StartedBy = By.IsEmpty() ? FString(TEXT("agent")) : By;
	AddLine(*Run, TEXT("Run started"), EAutopilotFeedKind::Notice);
	AgentStartedRunId = Run->Id;
	Changed(Run.Get());
}

void FAutopilotSession::OnRunKind(const TSharedPtr<FJsonObject>& Frame)
{
	FString Kind;
	TSharedPtr<FAutopilotRun> Live = GetActiveRun();
	if (!Frame.IsValid() || !Live.IsValid() || !Frame->TryGetStringField(TEXT("kind"), Kind)
		|| !(Kind == TEXT("verify") || Kind == TEXT("goal") || Kind == TEXT("explore")))
	{
		return;
	}
	Live->Kind = Kind;
	Changed(Live.Get());
}

void FAutopilotSession::Say(const FString& Text)
{
	Status = Text;
	StatusAt = FPlatformTime::Seconds();
}

FString FAutopilotSession::StatusLine() const
{
	return StatusAt >= 0.0 && FPlatformTime::Seconds() - StatusAt < 8.0 ? Status : FString();
}

void FAutopilotSession::StopRun()
{
	if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
	{
		B->StopRun();
	}
}

bool FAutopilotSession::CanSteer() const
{
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	return B && B->CanSteer() && GetActiveRun().IsValid();
}

bool FAutopilotSession::SteerRun(const FString& Text)
{
	const FString Said = Text.TrimStartAndEnd();
	TSharedPtr<FAutopilotRun> Live = GetActiveRun();
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (Said.IsEmpty() || !Live.IsValid() || !SteersAt(*Live) || !B || !B->CanSteer())
	{
		return false;
	}
	B->SteerRun(Said);
	AddLine(*Live, FString::Printf(TEXT("You: %s"), *Said), EAutopilotFeedKind::Notice);
	Changed(Live.Get());
	return true;
}

void FAutopilotSession::MarkNotABug(const TSharedPtr<FAutopilotRun>& Run, const TSharedPtr<FAutopilotFinding>& Finding)
{
	if (!Finding.IsValid() || Finding->bDismissed)
	{
		return;
	}
	UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	if (!B || !B->IsConnected())
	{
		Say(TEXT("Not connected: connect to send feedback"));
		return;
	}
	B->SendFeedback(Finding->Signature, TEXT("not_a_bug"));
	Say(FString::Printf(TEXT("Told the agent: %s is not a bug"), Finding->Detector.IsEmpty() ? *Finding->Summary : *Finding->Detector));
	Finding->bDismissed = true;
	Changed(Run.Get());
}

TSharedPtr<FAutopilotRun> FAutopilotSession::RunForEvent(bool bCreate, bool bReopen)
{
	if (TSharedPtr<FAutopilotRun> Live = GetActiveRun())
	{
		return Live;
	}
	if (TSharedPtr<FAutopilotRun> Lost = bReopen ? ReopenLost() : nullptr)
	{
		// (the agent service still plays it: Stop and Steer work again)
		if (UPlaytestSubsystem* B = UPlaytestSubsystem::Get())
		{
			B->ResumeRun();
		}
		return Lost;
	}
	if (Runs.Num() && Runs.Last()->bDone && FPlatformTime::Seconds() - Runs.Last()->EndSeconds < LateEventSeconds)
	{
		return Runs.Last();
	}
	return bCreate ? NewRun(FString(), FString()) : nullptr;
}

TSharedPtr<FAutopilotRun> FAutopilotSession::ReopenLost()
{
	if (!Runs.Num() || !Runs.Last()->bDone || !Runs.Last()->bLostConnection)
	{
		return nullptr;
	}
	TSharedPtr<FAutopilotRun> Run = Runs.Last();
	Run->bDone = false;
	Run->bLostConnection = false;
	Run->Verdict.Empty();
	Run->Reason.Empty();
	Run->Changes.Empty();
	AddLine(*Run, TEXT("Connected again: the run goes on"), EAutopilotFeedKind::Notice);
	Changed(Run.Get());
	return Run;
}

void FAutopilotSession::AddLine(FAutopilotRun& Run, const FString& Text, EAutopilotFeedKind Kind, const TSharedPtr<FAutopilotFinding>& Finding)
{
	TSharedPtr<FAutopilotFeedLine> Line = MakeShared<FAutopilotFeedLine>();
	Line->Elapsed = Run.Elapsed();
	Line->Text = Text;
	Line->Kind = Kind;
	Line->Attempt = Run.Attempt;
	Line->Finding = Finding;
	Line->Stage = Run.Stage;  // (the Feed tab groups the lines by it)
	Run.Feed.Add(Line);
	if (Run.Feed.Num() > MaxFeedLines)
	{
		Run.Feed.RemoveAt(0, Run.Feed.Num() - MaxFeedLines);
	}
}

TSharedPtr<FAutopilotFeedLine> FAutopilotRun::FindActivity(int32 EventId) const
{
	if (EventId <= 0)
	{
		return nullptr;
	}
	for (int32 i = Feed.Num() - 1; i >= 0; --i)
	{
		if (Feed[i]->ActivityId == EventId || Feed[i]->ResultId == EventId)
		{
			return Feed[i];
		}
	}
	return nullptr;
}

EAutopilotStage FAutopilotSession::StageOf(const FString& Name)
{
	if (Name == TEXT("analyze")) return EAutopilotStage::Analyze;
	if (Name == TEXT("probe")) return EAutopilotStage::Probe;
	if (Name == TEXT("build")) return EAutopilotStage::Build;
	if (Name == TEXT("play")) return EAutopilotStage::Rollout;
	if (Name == TEXT("results")) return EAutopilotStage::Results;
	if (Name == TEXT("report")) return EAutopilotStage::Report;
	return EAutopilotStage::None;
}

FString FAutopilotSession::StageHeading(EAutopilotStage Stage)
{
	switch (Stage)
	{
	case EAutopilotStage::Launch: return TEXT("Launch");
	case EAutopilotStage::Analyze: return TEXT("Analyze");
	case EAutopilotStage::Probe: return TEXT("Probe");
	case EAutopilotStage::Build: return TEXT("Build harness");
	case EAutopilotStage::Rollout: return TEXT("Play");
	case EAutopilotStage::Results: return TEXT("Results");
	case EAutopilotStage::Report: return TEXT("Report");
	default: return FString();
	}
}

int32 FAutopilotSession::ActivitySince() const
{
	const TSharedPtr<FAutopilotRun> Last = GetLatestRun();
	if (Last.IsValid() && (!Last->bDone || Last->bLostConnection))
	{
		return Last->LastActivityId;  // (0: everything the run has)
	}
	return -1;
}

void FAutopilotSession::OnActivity(const TSharedPtr<FJsonObject>& Frame)
{
	double IdNumber = 0;
	if (!Frame.IsValid() || !Frame->TryGetNumberField(TEXT("id"), IdNumber) || IdNumber <= 0)
	{
		return;
	}
	const int32 Id = (int32)IdNumber;
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	TSharedPtr<FAutopilotRun> Run = RunForEvent(B && B->IsRunActive());
	if (!Run.IsValid())
	{
		return;  // (no run of ours)
	}
	if (Run->FindActivity(Id).IsValid())
	{
		// The event came already: the feed replayed after a reconnect. Counted; the log says so once at the start and
		// again, with the count, when the replay moves past what is here (a reconnect can be checked from the log)
		if (Run->ReplaySkipped++ == 0)
		{
			UE_LOG(LogAutopilotSession, Display, TEXT("[Autopilot] A replay of the activity feed is arriving: event %d is shown already"), Id);
		}
		return;
	}
	if (Run->ReplaySkipped > 0)
	{
		UE_LOG(LogAutopilotSession, Display, TEXT("[Autopilot] The replay skipped %d events shown already (ids up to %d); new events from %d"),
			Run->ReplaySkipped, Run->LastActivityId, Id);
		Run->ReplaySkipped = 0;
	}
	FString Text, Kind, StageName, Detail;
	Frame->TryGetStringField(TEXT("text"), Text);
	Frame->TryGetStringField(TEXT("kind"), Kind);
	Frame->TryGetStringField(TEXT("stage"), StageName);
	Frame->TryGetStringField(TEXT("detail"), Detail);  // (null: none)
	Text = Scrub(Text.TrimStartAndEnd());
	// The line's time is the plugin's clock now (what every other line shows: one clock per feed); the frame's `t`,
	// the agent service's own count, is kept on the line (AgentT). A replayed event gets the time it arrives too
	double T = -1.0;
	Frame->TryGetNumberField(TEXT("t"), T);
	double EndsNumber = 0;
	const int32 Ends = Frame->TryGetNumberField(TEXT("ends"), EndsNumber) ? (int32)EndsNumber : 0;  // (null: a call, or a line of its own)
	Run->LastActivityId = FMath::Max(Run->LastActivityId, Id);
	const EAutopilotStage Stage = StageOf(StageName);
	if (!Run->bDone && Stage != EAutopilotStage::None)
	{
		Run->EnterStage(Stage);  // (the frame's stage drives the track; the status lines still do for an older agent service)
	}
	if (Ends > 0)
	{
		if (TSharedPtr<FAutopilotFeedLine> Call = Run->FindActivity(Ends))
		{
			if (Call->ResultId == 0)
			{
				// The call and what came back: one line (its words, then the result's, quieter; two folds)
				Call->Result = Text;
				Call->ResultDetail = Detail;
				Call->ResultId = Id;
			}
			Changed(Run.Get());
			return;
		}
	}
	TSharedPtr<FAutopilotFeedLine> Line = MakeShared<FAutopilotFeedLine>();
	Line->Elapsed = Run->Elapsed();
	Line->AgentT = T;
	Line->Text = Text;
	Line->Kind = EAutopilotFeedKind::Activity;
	Line->Attempt = Run->Attempt;
	Line->Stage = Stage != EAutopilotStage::None ? Stage : Run->Stage;
	Line->ActivityId = Id;
	Line->ActivityKind = Kind;
	Line->Detail = Detail;
	Run->Feed.Add(Line);
	if (Run->Feed.Num() > MaxFeedLines)
	{
		Run->Feed.RemoveAt(0, Run->Feed.Num() - MaxFeedLines);
	}
	Changed(Run.Get());
}

FString FAutopilotFeedLine::Icon() const
{
	if (ActivityKind == TEXT("text")) return TEXT("robot");
	if (ActivityKind == TEXT("read")) return TEXT("magnifier");
	if (ActivityKind == TEXT("search")) return TEXT("binoculars");
	if (ActivityKind == TEXT("write")) return TEXT("pencil");
	if (ActivityKind == TEXT("probe")) return TEXT("flask");
	if (ActivityKind == TEXT("episode")) return TEXT("gamepad");
	if (ActivityKind == TEXT("look")) return TEXT("eye");
	if (ActivityKind == TEXT("restart")) return TEXT("circular-arrow");
	if (ActivityKind == TEXT("steer")) return TEXT("speech-bubble");
	if (ActivityKind == TEXT("report")) return TEXT("flag");
	if (ActivityKind == TEXT("alert")) return TEXT("megaphone");
	if (ActivityKind == TEXT("refine")) return TEXT("notebook");
	return FString();
}

FString FAutopilotFeedLine::Title() const
{
	if (ActivityKind == TEXT("text")) return TEXT("What the agent said");
	if (ActivityKind == TEXT("read")) return TEXT("Read a file");
	if (ActivityKind == TEXT("search")) return TEXT("Searched the code");
	if (ActivityKind == TEXT("write")) return TEXT("Wrote a file");
	if (ActivityKind == TEXT("probe")) return TEXT("Tried something in the game");
	if (ActivityKind == TEXT("episode")) return TEXT("Played the harness");
	if (ActivityKind == TEXT("look")) return TEXT("Looked at the picture");
	if (ActivityKind == TEXT("restart")) return TEXT("Restarted the level");
	if (ActivityKind == TEXT("steer")) return TEXT("You steered");
	if (ActivityKind == TEXT("report")) return TEXT("Reported");
	if (ActivityKind == TEXT("alert")) return TEXT("Announcement");
	if (ActivityKind == TEXT("refine")) return TEXT("Refined the game's memory");
	return FString();
}

FString FAutopilotFeedLine::FoldWords(const FString& Label, bool bOpen)
{
	return (bOpen ? TEXT("– hide ") : TEXT("+ inspect ")) + Label;
}

FString FAutopilotFeedLine::Glyph() const
{
	if (ActivityKind == TEXT("text")) return TEXT("?");
	if (ActivityKind == TEXT("read")) return TEXT("R");
	if (ActivityKind == TEXT("search")) return TEXT("S");
	if (ActivityKind == TEXT("write")) return TEXT("W");
	if (ActivityKind == TEXT("probe")) return TEXT("P");
	if (ActivityKind == TEXT("episode")) return TEXT("E");
	if (ActivityKind == TEXT("look")) return TEXT("L");
	if (ActivityKind == TEXT("report") || ActivityKind == TEXT("alert")) return TEXT("!");
	if (ActivityKind == TEXT("steer")) return TEXT(">");
	if (ActivityKind == TEXT("restart")) return TEXT("*");
	if (ActivityKind == TEXT("refine")) return TEXT("M");
	return TEXT("·");
}

bool FAutopilotFeedLine::Folds(const FString& Detail)
{
	return !Detail.IsEmpty() && (Detail.Contains(TEXT("\n")) || Detail.Len() > 80);
}

FString FAutopilotFeedLine::DetailLabel() const
{
	if (!Folds(Detail))
	{
		return FString();
	}
	if (ActivityKind == TEXT("probe")) return TEXT("the code");
	if (ActivityKind == TEXT("write")) return TEXT("what was written");
	if (ActivityKind == TEXT("episode")) return TEXT("what was asked");
	if (ActivityKind == TEXT("look")) return TEXT("the questions");
	if (ActivityKind == TEXT("report")) return TEXT("the summary");
	if (ActivityKind == TEXT("read")) return TEXT("the path");
	if (ActivityKind == TEXT("search")) return TEXT("the pattern");
	if (ActivityKind == TEXT("refine")) return TEXT("the edit and its evidence");
	return TEXT("the whole message");
}

FString FAutopilotFeedLine::ResultLabel() const
{
	if (!Folds(ResultDetail))
	{
		return FString();
	}
	if (ActivityKind == TEXT("probe")) return TEXT("the output");
	if (ActivityKind == TEXT("episode")) return TEXT("the summary");
	if (ActivityKind == TEXT("look")) return TEXT("the description");
	return TEXT("what came back");
}

void FAutopilotSession::Changed(FAutopilotRun* Run)
{
	++Revision;
	if (Run)
	{
		++Run->Revision;
		Unsaved.Add(Run->Id);
	}
}

void FAutopilotSession::OnStatus(const FString& Text)
{
	const FString Line = Text.TrimStartAndEnd();
	if (Line.IsEmpty() || Line.StartsWith(TEXT("Finding: ")) || Line.StartsWith(TEXT("Run finished.")))
	{
		return;  // (the finding and the result come as their own events)
	}
	if (IsPluginNotice(Line))
	{
		Notice = Line;
		// (as the design writes them: "…", not "...")
		const FString Said = Line.EndsWith(TEXT("...")) ? Line.LeftChop(3) + TEXT("\u2026") : Line;
		if (Line.StartsWith(TEXT("Resetting memory")) || Line.StartsWith(TEXT("Memory reset")) || Line.StartsWith(TEXT("Memory not reset")))
		{
			ResetLine = Said;  // (shown under the memory row)
			ResetAt = FPlatformTime::Seconds();
		}
		Say(Said);
		if (TSharedPtr<FAutopilotRun> Live = GetActiveRun())
		{
			AddLine(*Live, Line.StartsWith(TEXT("Started ")) ? FString(TEXT("Run started")) : Line, EAutopilotFeedKind::Notice);
			Changed(Live.Get());
		}
		else
		{
			Changed();
		}
		return;
	}
	EAutopilotStage Stage = EAutopilotStage::None;
	int32 Attempt = 0;
	bool bNow = false;
	const FString Shown = Translate(Line, Stage, Attempt, bNow);
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	TSharedPtr<FAutopilotRun> Run = RunForEvent(B && B->IsRunActive());
	if (!Run.IsValid())
	{
		Notice = Shown;
		Changed();
		return;
	}
	EAutopilotFeedKind Kind = EAutopilotFeedKind::Status;
	if (!Run->bDone)
	{
		Run->EnterStage(Stage);  // (the report is the last stage: nothing moves it back)
		if (Stage == EAutopilotStage::Results)
		{
			Run->Now.Reset();  // (the rollout's last action is over)
		}
		if (Attempt > 0)
		{
			Run->Attempt = Attempt;
			Kind = EAutopilotFeedKind::Attempt;
		}
		if (bNow)
		{
			Run->Now = Shown.Mid(5);
			Kind = EAutopilotFeedKind::Now;
			if (Run->Stage < EAutopilotStage::Rollout)
			{
				Run->EnterStage(EAutopilotStage::Rollout);  // (playing)
			}
		}
	}
	AddLine(*Run, Shown, Kind);
	Changed(Run.Get());
}

TSharedPtr<FAutopilotFinding> FAutopilotSession::AddFinding(FAutopilotRun& Run, const TSharedPtr<FJsonObject>& Finding)
{
	if (!Finding.IsValid())
	{
		return nullptr;
	}
	FString Signature;
	Finding->TryGetStringField(TEXT("signature"), Signature);
	FString Summary;
	Finding->TryGetStringField(TEXT("summary"), Summary);
	if (Signature.IsEmpty())
	{
		Signature = Summary;
	}
	TSharedPtr<FAutopilotFinding>* Existing = Run.Findings.FindByPredicate([&Signature](const TSharedPtr<FAutopilotFinding>& F) { return F->Signature == Signature; });
	TSharedPtr<FAutopilotFinding> Item = Existing ? *Existing : MakeShared<FAutopilotFinding>();
	Item->Signature = Signature;
	Item->Summary = Summary;
	Finding->TryGetStringField(TEXT("severity"), Item->Severity);
	Finding->TryGetStringField(TEXT("detector"), Item->Detector);
	Finding->TryGetStringField(TEXT("actor"), Item->Actor);
	Finding->TryGetStringField(TEXT("source"), Item->Source);
	Finding->TryGetNumberField(TEXT("t"), Item->GameTime);
	double Count = 0;
	Item->Count = Finding->TryGetNumberField(TEXT("count"), Count) ? FMath::Max(1, (int32)Count) : FMath::Max(1, Item->Count + (Existing ? 1 : 0));
	const TSharedPtr<FJsonObject>* Where = nullptr;
	if (Finding->TryGetObjectField(TEXT("where"), Where) && Where && Where->IsValid())
	{
		Item->bHasWhere = true;
		Item->Where = FVector((*Where)->GetNumberField(TEXT("X")), (*Where)->GetNumberField(TEXT("Y")), (*Where)->GetNumberField(TEXT("Z")));
	}
	if (!Existing)
	{
		Run.Findings.Add(Item);
	}
	return Item;
}

void FAutopilotSession::OnFinding(const TSharedPtr<FJsonObject>& Finding)
{
	TSharedPtr<FAutopilotRun> Run = RunForEvent(false, false);  // (an engine finding never reopens a run: the game sends them alone)
	if (!Run.IsValid() || !Finding.IsValid())
	{
		return;
	}
	if (TSharedPtr<FAutopilotFinding> Item = AddFinding(*Run, Finding))
	{
		AddLine(*Run, Item->Count > 1 ? FString::Printf(TEXT("%s (\u00d7%d)"), *Item->Summary, Item->Count) : Item->Summary,
			EAutopilotFeedKind::Finding, Item);
		Changed(Run.Get());
	}
}

void FAutopilotSession::OnResult(const TSharedPtr<FJsonObject>& Result, const FString& ReportDir)
{
	TSharedPtr<FAutopilotRun> Run = GetActiveRun();
	if (!Run.IsValid())
	{
		Run = ReopenLost();  // (the run whose connection dropped: its result came once connected again)
	}
	if (!Run.IsValid())
	{
		Run = NewRun(FString(), FString());  // (a run the agent service started, its result only)
	}
	Run->ReportDir = ReportDir;
	if (!ReportDir.IsEmpty())
	{
		Run->RunId = FPaths::GetCleanFilename(ReportDir);
	}
	bool bMemoryUpdated = false;
	if (Result.IsValid())
	{
		FString Verdict;
		if (Result->TryGetStringField(TEXT("verdict"), Verdict))
		{
			Verdict = Verdict.ToUpper();
			Run->Verdict = Verdict == TEXT("CONFIRMED") || Verdict == TEXT("DENIED") || Verdict == TEXT("INCONCLUSIVE") ? Verdict : FString(TEXT("NO RESULT"));
			Result->TryGetStringField(TEXT("reason"), Run->Reason);
			FString Claim;
			if (Run->Prompt.IsEmpty() && Result->TryGetStringField(TEXT("claim"), Claim))
			{
				Run->Prompt = Claim;
			}
			if (Run->Kind.IsEmpty())
			{
				Run->Kind = TEXT("verify");
			}
			Run->Evidence = Strings(Result, TEXT("evidence"));
		}
		else
		{
			TArray<FString> Notes = Strings(Result, TEXT("notes"));
			FString Summary;
			if (Notes.Num())
			{
				Run->Reason = Notes[0];
				Notes.RemoveAt(0);
			}
			else if (Result->TryGetStringField(TEXT("summary"), Summary) || Result->TryGetStringField(TEXT("reason"), Summary))
			{
				Run->Reason = Summary;
			}
			FString Goal;
			if (Run->Prompt.IsEmpty() && Result->TryGetStringField(TEXT("goal"), Goal))
			{
				Run->Prompt = Goal;
			}
			bool bReached = false;
			const bool bHasReached = Result->TryGetBoolField(TEXT("reached"), bReached);
			if (Run->Kind == TEXT("explore") || !bHasReached)
			{
				// (an exploration asked for as a test in the user's own words: docs/protocol.md, Runs)
				bool bTested = false;
				Result->TryGetBoolField(TEXT("tested"), bTested);
				Run->Verdict = bTested ? TEXT("GAME TESTED") : TEXT("EXPLORED");
				Run->AgentNotes = Notes;  // what it found
				if (Run->Kind.IsEmpty())
				{
					Run->Kind = TEXT("explore");
				}
			}
			else
			{
				Run->Verdict = bReached ? TEXT("GOAL HIT") : TEXT("GOAL MISSED");
				Run->Evidence = Notes;
				if (Run->Kind.IsEmpty())
				{
					Run->Kind = TEXT("goal");
				}
			}
		}
		Run->Changes = ChangeTexts(Result);
		Run->Alerts = Strings(Result, TEXT("alerts"));
		const TArray<TSharedPtr<FJsonValue>>* Bugs = nullptr;
		if (Result->TryGetArrayField(TEXT("bugs"), Bugs) && Bugs)
		{
			for (const TSharedPtr<FJsonValue>& Bug : *Bugs)
			{
				const TSharedPtr<FJsonObject>* BugObject = nullptr;
				if (Bug.IsValid() && Bug->TryGetObject(BugObject) && BugObject)
				{
					if (TSharedPtr<FAutopilotFinding> Item = AddFinding(*Run, *BugObject))
					{
						if (Item->Source.IsEmpty())
						{
							Item->Source = TEXT("agent");
						}
					}
				}
			}
		}
		double Number = 0;
		if (Result->TryGetNumberField(TEXT("seconds"), Number) && Number >= 0)
		{
			Run->ResultSeconds = Number;
		}
		if (Result->TryGetNumberField(TEXT("attempts"), Number) && Number >= 0)
		{
			Run->ResultAttempts = (int32)Number;
		}
		// The run's recording: kept with the run (a copy in its folder, so it outlives a Reset memory)
		const TSharedPtr<FJsonObject>* Recording = nullptr;
		FString Video;
		if (Result->TryGetObjectField(TEXT("recording"), Recording) && Recording && Recording->IsValid()
			&& (*Recording)->TryGetStringField(TEXT("video"), Video) && !Video.IsEmpty())
		{
			Run->Recording = Video;  // (the agent service's, until the copy is made)
			Run->RecordingSeconds = -1.0;
			(*Recording)->TryGetNumberField(TEXT("seconds"), Run->RecordingSeconds);
			CopyRecording(*Run);
		}
		// Or why there is none (the result says where the player would be, and after a restart: saved with the run)
		Run->NoRecording = Run->Recording.IsEmpty() ? NoRecordingWhy(Result) : FString();
		// What the agent remembers about the game changed
		const TSharedPtr<FJsonObject>* Memory = nullptr;
		if (Result->TryGetObjectField(TEXT("memory"), Memory) && Memory && Memory->IsValid())
		{
			(*Memory)->TryGetBoolField(TEXT("updated"), bMemoryUpdated);
		}
	}
	const bool bStopped = Run->Feed.ContainsByPredicate([](const TSharedPtr<FAutopilotFeedLine>& L) { return L->Text == TEXT("Stopping\u2026"); });
	if (Run->Verdict.IsEmpty() || (bStopped && Run->Verdict == TEXT("NO RESULT")))
	{
		Run->Verdict = bStopped ? TEXT("STOPPED") : TEXT("NO RESULT");
	}
	if (Run->Reason.IsEmpty() && Run->Verdict == TEXT("STOPPED"))
	{
		Run->Reason = TEXT("Stopped before a result.");
	}
	else if (Run->Reason.IsEmpty() && Run->Verdict == TEXT("NO RESULT"))
	{
		Run->Reason = TEXT("The connection to the agent service closed before the result came.");
	}
	Run->Stage = EAutopilotStage::Report;
	Run->bDone = true;
	Run->EndSeconds = FPlatformTime::Seconds();
	// (the verdict in sentence case: "Result: Goal hit")
	FString Said = Run->Verdict.ToLower();
	if (Said.Len())
	{
		Said[0] = FChar::ToUpper(Said[0]);
	}
	AddLine(*Run, FString::Printf(TEXT("Result: %s"), *Said), EAutopilotFeedKind::Result);
	if (bMemoryUpdated)
	{
		AddLine(*Run, MemoryLine, EAutopilotFeedKind::Memory);
	}
	Changed(Run.Get());
}

void FAutopilotSession::CopyRecording(const FAutopilotRun& Run)
{
	if (Run.Recording.IsEmpty() || Run.ReportDir.IsEmpty())
	{
		return;
	}
	const FString From = Run.Recording;
	const FString To = FPaths::Combine(Run.ReportDir, TEXT("recording.mp4"));
	if (FPaths::IsSamePath(From, To))
	{
		return;
	}
	const int32 Id = Run.Id;
	// (a recording can be large: never on the editor's game thread; when it can't be copied, the agent service's stays)
	Async(EAsyncExecution::ThreadPool, [From, To, Id]()
	{
		// (to a .part first: an editor closed mid-copy never leaves a cut recording.mp4 for history to find)
		const FString Part = To + TEXT(".part");
		const bool bCopied = IFileManager::Get().Copy(*Part, *From, true, true) == COPY_OK
			&& IFileManager::Get().Move(*To, *Part, true, true);
		if (!bCopied)
		{
			IFileManager::Get().Delete(*Part, false, true, true);
		}
		if (bCopied)
		{
			AsyncTask(ENamedThreads::GameThread, [To, Id]()
			{
				FAutopilotSession::Get().OnRecordingCopied(Id, To);
			});
		}
	});
}

void FAutopilotSession::OnRecordingCopied(int32 RunId, const FString& Path)
{
	if (TSharedPtr<FAutopilotRun> Run = FindRun(RunId))
	{
		Run->Recording = Path;
		Changed(Run.Get());
	}
}

void FAutopilotSession::OnJevCall(const TSharedPtr<FPlaytestJevCall>& Call)
{
	if (!Call.IsValid())
	{
		return;
	}
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	TSharedPtr<FAutopilotRun> Run = RunForEvent(B && B->IsRunActive());
	if (!Run.IsValid())
	{
		return;
	}
	if (Run->StepAttempts == 0 || Call->Call <= Run->LastCall)
	{
		++Run->StepAttempts;  // the step count went down: a new attempt
	}
	Run->LastCall = Call->Call;
	TSharedPtr<FAutopilotStep> Step = MakeShared<FAutopilotStep>();
	Step->Call = Call;
	Step->Attempt = Run->StepAttempts;
	Step->Index = Run->StepCount++;
	Run->Steps.Add(Step);
	if (!Call->Failed() && Call->Seconds() >= 0)
	{
		Run->StepMs.Add((float)(Call->Seconds() * 1000.0));
	}
	// Each action keeps its color for the whole run, in order of appearance
	auto Color = [&Run](const FString& Name)
	{
		if (!Name.IsEmpty() && !Run->ActionColors.Contains(Name))
		{
			Run->ActionColors.Add(Name, Run->ActionColors.Num());
		}
	};
	Color(Call->NowDoing);
	Color(Call->Chosen);
	TArray<FPlaytestPick> Picks = Call->Pick;
	Picks.StableSort([](const FPlaytestPick& A, const FPlaytestPick& B) { return A.P > B.P; });
	for (const FPlaytestPick& Pick : Picks)
	{
		Color(Pick.Name);
	}
	++KeptSteps;
	TrimSteps();
	++Run->StepRevision;
	Changed(Run.Get());
}

void FAutopilotSession::TrimSteps()
{
	for (const TSharedPtr<FAutopilotRun>& Run : Runs)
	{
		if (KeptSteps <= MaxKeptSteps)
		{
			return;
		}
		const int32 Drop = FMath::Min(Run->Steps.Num(), KeptSteps - MaxKeptSteps);
		if (Drop > 0)
		{
			Run->Steps.RemoveAt(0, Drop);
			KeptSteps -= Drop;
			++Run->StepRevision;
		}
	}
}

void FAutopilotSession::OnUsage(const TSharedPtr<FJsonObject>& Usage)
{
	// The run going (or one that ended a moment ago: the final count comes just before the result)
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	TSharedPtr<FAutopilotRun> Run = RunForEvent(B && B->IsRunActive(), false);
	if (!Run.IsValid())
	{
		return;
	}
	bool bFinal = false;
	TArray<FPlaytestModelUsage> Models = PlaytestUsage::Parse(Usage, &bFinal);
	if (Run->bUsageFinal && !bFinal && Run->bDone)
	{
		return;  // (a late count never replaces the final one)
	}
	Run->Usage = MoveTemp(Models);
	Run->bUsageFinal = bFinal;
	++UsageRevision;
	Changed(Run.Get());
}

void FAutopilotSession::OnBill(const TSharedPtr<FJsonObject>& Message)
{
	// Sent when a run through Aura is finished, before its result: the run going, or one that ended a moment ago
	const UPlaytestSubsystem* B = UPlaytestSubsystem::Get();
	TSharedPtr<FAutopilotRun> Run = RunForEvent(B && B->IsRunActive(), false);
	FPlaytestRunBill Bill;
	if (!Run.IsValid() || !PlaytestAccount::ParseBill(Message, Bill))
	{
		return;
	}
	Run->Bill = Bill;
	++UsageRevision;
	Changed(Run.Get());
}

TArray<TSharedPtr<FAutopilotRun>> FAutopilotSession::GetUsageRuns() const
{
	// (a run past MaxSavedRuns this session is in Runs and in the ledger both: counted once)
	TSet<FString> Listed;
	for (const TSharedPtr<FAutopilotRun>& Run : SetAside)
	{
		Listed.Add(UsageKey(*Run));
	}
	for (const TSharedPtr<FAutopilotRun>& Run : Runs)
	{
		Listed.Add(UsageKey(*Run));
	}
	TArray<TSharedPtr<FAutopilotRun>> All;
	for (const TSharedPtr<FAutopilotRun>& Run : Retired)
	{
		if (!Listed.Contains(UsageKey(*Run)))
		{
			All.Add(Run);
		}
	}
	All.Append(SetAside);
	All.Append(Runs);
	return All;
}

double FAutopilotSession::ProjectBill(int32& OutRuns) const
{
	double Total = 0.0;
	OutRuns = 0;
	for (const TSharedPtr<FAutopilotRun>& Run : GetUsageRuns())
	{
		if (Run->Bill.Shows())
		{
			Total += Run->Bill.Usd;
			++OutRuns;
		}
	}
	return Total;
}

TArray<FPlaytestModelUsage> FAutopilotSession::ProjectUsage(int32& OutRuns) const
{
	TArray<FPlaytestModelUsage> Total;
	OutRuns = 0;
	for (const TSharedPtr<FAutopilotRun>& Run : GetUsageRuns())
	{
		if (Run->Usage.Num())
		{
			PlaytestUsage::Add(Total, Run->Usage);
			++OutRuns;
		}
	}
	return Total;
}

void FAutopilotSession::OnConnection(bool bConnected, const FString& Info)
{
	if (!bConnected)
	{
		if (TSharedPtr<FAutopilotRun> Live = GetActiveRun())
		{
			Live->bDone = true;
			Live->EndSeconds = FPlatformTime::Seconds();
			Live->Verdict = TEXT("NO RESULT");
			Live->bLostConnection = true;
			Live->Reason = TEXT("The connection to the agent service closed before the result came.");
			AddLine(*Live, TEXT("Connection closed: no result"), EAutopilotFeedKind::Notice);
			Changed(Live.Get());
		}
	}
	Changed();
}

FString FAutopilotSession::When(const FDateTime& Time)
{
	static const TCHAR* const Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
		TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
	return FString::Printf(TEXT("%s %02d %02d:%02d"), Months[FMath::Clamp(Time.GetMonth() - 1, 0, 11)], Time.GetDay(),
		Time.GetHour(), Time.GetMinute());
}

FString FAutopilotSession::Translate(const FString& Line, EAutopilotStage& OutStage, int32& OutAttempt, bool& bOutNow)
{
	OutStage = EAutopilotStage::None;
	OutAttempt = 0;
	bOutNow = false;
	const FString T = Line.TrimStartAndEnd();
	if (T == TEXT("Starting the game"))
	{
		OutStage = EAutopilotStage::Launch;
		return TEXT("Launching the environment");
	}
	if (T == TEXT("Reading the game's code"))
	{
		OutStage = EAutopilotStage::Analyze;
		return TEXT("Analyzing the project");
	}
	if (T == TEXT("Trying things in the game"))
	{
		OutStage = EAutopilotStage::Probe;
		return TEXT("Probing the environment");
	}
	if (T == TEXT("Writing the harness"))
	{
		OutStage = EAutopilotStage::Build;
		return TEXT("Building the harness");
	}
	if (T == TEXT("Looking at the results"))
	{
		OutStage = EAutopilotStage::Results;
		return TEXT("Processing results");
	}
	if (T == SavingLine)
	{
		OutStage = EAutopilotStage::Report;  // (after the results, just before the result: part of the report)
		return T;
	}
	const int32 PrefixLen = FCString::Strlen(RolloutPrefix);
	if (T.StartsWith(RolloutPrefix, ESearchCase::CaseSensitive) && T.EndsWith(TEXT(")")) && T.Len() > PrefixLen + 1)
	{
		const FString Number = T.Mid(PrefixLen, T.Len() - PrefixLen - 1);
		if (Number.IsNumeric())
		{
			OutStage = EAutopilotStage::Rollout;
			OutAttempt = FCString::Atoi(*Number);
			return FString::Printf(TEXT("Harness %d"), OutAttempt);
		}
	}
	if (T.StartsWith(TEXT("Now: "), ESearchCase::CaseSensitive))
	{
		bOutNow = true;
		return T;
	}
	return T;
}

bool FAutopilotSession::PieLoading()
{
	if (!GEditor)
	{
		return false;
	}
	const UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
	const UWorld* World = Bridge ? Bridge->GetGameWorld() : nullptr;
	return GEditor->IsPlaySessionRequestQueued() || (GEditor->PlayWorld && !(World && World->HasBegunPlay()));
}

TSharedRef<FJsonObject> FAutopilotSession::RunFrame(const FString& Text, int32 TimeLimitMinutes, const FString& DesignBrief, FString& OutPrompt)
{
	OutPrompt = Text.TrimStartAndEnd();  // (the words typed, as typed)
	TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(TEXT("kind"), TEXT("auto"));
	Frame->SetStringField(TEXT("prompt"), OutPrompt);
	if (TimeLimitMinutes > 0)
	{
		Frame->SetNumberField(TEXT("time_limit_minutes"), TimeLimitMinutes);
	}
	const FString Brief = DesignBrief.TrimStartAndEnd();
	if (!Brief.IsEmpty())
	{
		Frame->SetStringField(TEXT("design_brief"), Brief);
	}
	return Frame;
}

TSharedRef<FJsonObject> FAutopilotSession::ExploreFrame(int32 Minutes, const FString& DesignBrief)
{
	TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(TEXT("kind"), TEXT("explore"));
	Frame->SetNumberField(TEXT("minutes"), Minutes);
	const FString Brief = DesignBrief.TrimStartAndEnd();
	if (!Brief.IsEmpty())
	{
		Frame->SetStringField(TEXT("brief"), Brief);
	}
	Frame->SetBoolField(TEXT("repro"), true);
	Frame->SetBoolField(TEXT("escalate"), true);
	return Frame;
}

FString FAutopilotSession::Scrub(const FString& Text)
{
	FString Out = Text;
	Out.ReplaceInline(TEXT("Jev call"), TEXT("policy step"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("Jev"), TEXT("policy"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("brain"), TEXT("agent"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("Brain"), TEXT("Agent"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("codex"), TEXT("project index"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("Codex"), TEXT("Project index"), ESearchCase::CaseSensitive);
	return Out;
}

FString FAutopilotSession::Clock(double Seconds)
{
	const int32 Total = FMath::Max(0, FMath::FloorToInt(Seconds));
	return Total >= 3600 ? FString::Printf(TEXT("%d:%02d:%02d"), Total / 3600, Total / 60 % 60, Total % 60)
		: FString::Printf(TEXT("%02d:%02d"), Total / 60, Total % 60);
}

FString FAutopilotSession::ClockShort(double Seconds)
{
	const int32 Total = FMath::Max(0, FMath::FloorToInt(Seconds));
	return Total >= 3600 ? FString::Printf(TEXT("%d:%02d:%02d"), Total / 3600, Total / 60 % 60, Total % 60)
		: FString::Printf(TEXT("%d:%02d"), Total / 60, Total % 60);
}

#undef LOCTEXT_NAMESPACE
