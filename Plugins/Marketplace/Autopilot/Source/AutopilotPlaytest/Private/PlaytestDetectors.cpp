#include "PlaytestInternal.h"

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/App.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "EngineDefines.h"
#include "Interfaces/IProjectManager.h"
#include "Internationalization/Regex.h"
#include "PlaytestJson.h"
#include "PlaytestSubsystem.h"
#include "ProjectDescriptor.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// ---------------------------------------------------------------------------------------------------
// Log capture

FPlaytestLogCapture::FPlaytestLogCapture()
{
}

FPlaytestLogCapture::~FPlaytestLogCapture()
{
}

void FPlaytestLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (!bEnabled || !V)
	{
		return;
	}
	static const FName PlaytestCategory(TEXT("LogPlaytest"));
	static const FName WebSockets(TEXT("LogWebSockets"));
	static const FName Http(TEXT("LogHttp"));
	static const FName Script(TEXT("LogScript"));
	if (Category == PlaytestCategory || Category == WebSockets || Category == Http)
	{
		return;
	}
	const ELogVerbosity::Type Level = (ELogVerbosity::Type)(Verbosity & ELogVerbosity::VerbosityMask);
	const bool bError = Level == ELogVerbosity::Error || Level == ELogVerbosity::Fatal;
	// Blueprint runtime errors ("Accessed None", infinite loops) are LogScript warnings; LogBlueprint
	// warnings tagged [AssetLog] are compile notes from loading assets, not something that happened in play
	const bool bScriptWarning = Level == ELogVerbosity::Warning
		&& (Category == Script || FCString::Stristr(V, TEXT("Accessed None")) || FCString::Stristr(V, TEXT("Ensure condition failed")));
	if (!bError && !bScriptWarning)
	{
		return;
	}
	if (!bError && FCString::Stristr(V, TEXT("[AssetLog]")))
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	if (Lines.Num() >= 500)
	{
		++Dropped;
		return;
	}
	Lines.Add({bError ? TEXT("Error") : TEXT("Warning"), Category.ToString(), FString(V)});
}

TArray<FPlaytestLogCapture::FLine> FPlaytestLogCapture::Take()
{
	FScopeLock ScopeLock(&Lock);
	TArray<FLine> Out = MoveTemp(Lines);
	Lines.Reset();
	Dropped = 0;
	return Out;
}

// ---------------------------------------------------------------------------------------------------
// Detectors

namespace
{
	FString ClassLabel(const UObject* Object)
	{
		if (!Object)
		{
			return TEXT("None");
		}
		FString Name = Object->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	/** A log message without its variable parts, so repeats of the same error share a signature. */
	FString Normalize(const FString& Message)
	{
		FString Out;
		Out.Reserve(Message.Len());
		bool bInNumber = false;
		for (int32 i = 0; i < Message.Len(); ++i)
		{
			const TCHAR C = Message[i];
			if (FChar::IsDigit(C))
			{
				if (!bInNumber)
				{
					Out.AppendChar(TEXT('#'));
				}
				bInNumber = true;
				continue;
			}
			bInNumber = false;
			Out.AppendChar(C);
		}
		return Out.Left(160);
	}

	bool NumericValue(const FProperty* Property, const void* Container, double& Out)
	{
		if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
		{
			if (Byte->Enum)
			{
				return false;
			}
		}
		const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
		if (!Numeric || Property->ArrayDim != 1)
		{
			return false;
		}
		const void* Ptr = Property->ContainerPtrToValuePtr<void>(Container);
		Out = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(Ptr) : (double)Numeric->GetSignedIntPropertyValue(Ptr);
		return true;
	}

	TSharedRef<FJsonObject> Vec(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetField(TEXT("X"), PlaytestJson::Number(FMath::RoundToDouble(V.X * 10) / 10));
		O->SetField(TEXT("Y"), PlaytestJson::Number(FMath::RoundToDouble(V.Y * 10) / 10));
		O->SetField(TEXT("Z"), PlaytestJson::Number(FMath::RoundToDouble(V.Z * 10) / 10));
		return O;
	}

	FString FormatNumber(double V)
	{
		return PlaytestJson::Write(PlaytestJson::Number(FMath::RoundToDouble(V * 1000) / 1000));
	}
}

FPlaytestDetectors::FPlaytestDetectors(UPlaytestSubsystem& InBridge)
	: Bridge(InBridge)
{
	ProjectPackages.Add(FString::Printf(TEXT("/Script/%s"), FApp::GetProjectName()));
	if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
	{
		for (const FModuleDescriptor& Module : Project->Modules)
		{
			ProjectPackages.Add(FString::Printf(TEXT("/Script/%s"), *Module.Name.ToString()));
		}
	}
}

void FPlaytestDetectors::Configure(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return;
	}
	Args->TryGetBoolField(TEXT("enabled"), bEnabled);
	Args->TryGetNumberField(TEXT("teleport_factor"), TeleportFactor);
	Args->TryGetNumberField(TEXT("teleport_min"), TeleportMin);
	Args->TryGetNumberField(TEXT("long_fall_seconds"), LongFallSeconds);
	Args->TryGetNumberField(TEXT("hitch_seconds"), HitchSeconds);
	Args->TryGetNumberField(TEXT("growth_seconds"), GrowthSeconds);
	Args->TryGetNumberField(TEXT("hud_every"), HudEvery);
	Args->TryGetNumberField(TEXT("property_every"), PropertyEvery);
}

void FPlaytestDetectors::Reset()
{
	if (UWorld* W = World.Get(); W && ActorDestroyedHandle.IsValid())
	{
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 5
		W->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
#else
		W->RemoveOnActorDestroyededHandler(ActorDestroyedHandle);
#endif
	}
	ActorDestroyedHandle.Reset();
	Seen.Reset();
	Pawns.Reset();
	GrowthSamples.Reset();
	World.Reset();
	SessionStartReal = FPlatformTime::Seconds();
	LastGameTime = -1;
	NextActorCheck = NextGrowthSample = NextPropertyCheck = NextHudCheck = 0;
}

bool FPlaytestDetectors::IsProjectClass(const UClass* Class) const
{
	if (!Class)
	{
		return false;
	}
	const FString Package = Class->GetOutermost()->GetName();
	return Package.StartsWith(TEXT("/Game/")) || ProjectPackages.Contains(Package);
}

void FPlaytestDetectors::Report(const FString& Detector, const FString& Severity, const FString& Summary, const FString& Signature,
	double GameTime, const AActor* Actor, const TSharedPtr<FJsonObject>& Details)
{
	int32& Count = Seen.FindOrAdd(Signature);
	++Count;
	if (Count != 1 && Count != 10 && Count != 100 && Count != 1000)
	{
		return;  // the same problem again: only report how often now and then
	}
	TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
	Finding->SetStringField(TEXT("detector"), Detector);
	Finding->SetStringField(TEXT("severity"), Severity);
	Finding->SetStringField(TEXT("summary"), Summary);
	Finding->SetStringField(TEXT("signature"), Signature);
	Finding->SetField(TEXT("t"), PlaytestJson::Number(GameTime));
	Finding->SetNumberField(TEXT("count"), Count);
	Finding->SetStringField(TEXT("source"), TEXT("engine"));
	if (Actor)
	{
		Finding->SetStringField(TEXT("actor"), Actor->GetName());
		Finding->SetObjectField(TEXT("where"), Vec(Actor->GetActorLocation()));
	}
	if (Details.IsValid())
	{
		Finding->SetObjectField(TEXT("details"), Details);
	}
	TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("kind"), TEXT("finding"));
	Event->SetField(TEXT("t"), PlaytestJson::Number(GameTime));
	Event->SetObjectField(TEXT("finding"), Finding);
	Bridge.QueueEvent(Event);
	Bridge.OnFinding.Broadcast(Finding);
}

void FPlaytestDetectors::OnLogLines(const TArray<FPlaytestLogCapture::FLine>& Lines, double GameTime)
{
	if (!bEnabled)
	{
		return;
	}
	for (const FPlaytestLogCapture::FLine& Line : Lines)
	{
		const bool bEnsure = Line.Message.Contains(TEXT("Ensure condition failed"));
		const bool bScript = Line.Message.Contains(TEXT("Accessed None")) || Line.Category == TEXT("LogScript");
		const FString Detector = bEnsure ? TEXT("ensure") : TEXT("log_error");
		const FString Severity = (Line.Verbosity == TEXT("Error") || bEnsure || bScript) ? TEXT("error") : TEXT("warning");
		const FString Kind = bEnsure ? TEXT("An ensure failed") : bScript ? TEXT("Blueprint runtime error") : TEXT("Engine error");
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("category"), Line.Category);
		Details->SetStringField(TEXT("verbosity"), Line.Verbosity);
		Details->SetStringField(TEXT("message"), Line.Message.Left(2000));
		Report(Detector, Severity, FString::Printf(TEXT("%s (%s): %s"), *Kind, *Line.Category, *Line.Message.Left(200)),
			FString::Printf(TEXT("%s:%s:%s"), *Detector, *Line.Category, *Normalize(Line.Message)), GameTime < 0 ? 0 : GameTime, nullptr, Details);
	}
}

void FPlaytestDetectors::Tick(UWorld* InWorld, double RealDelta)
{
	if (!bEnabled || !InWorld)
	{
		return;
	}
	if (World.Get() != InWorld)
	{
		Reset();
		World = InWorld;
		TWeakObjectPtr<UWorld> WeakWorld = InWorld;
		ActorDestroyedHandle = InWorld->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([this, WeakWorld](AActor* Actor)
		{
			// The engine destroys what falls below KillZ at once: check here, since a sampled check may miss it
			UWorld* W = WeakWorld.Get();
			if (!bEnabled || !W || World.Get() != W || !Actor || !Actor->GetRootComponent())
			{
				return;
			}
			const AWorldSettings* Settings = W->GetWorldSettings();
			if (Settings && Settings->bEnableWorldBoundsChecks && Actor->GetActorLocation().Z < Settings->KillZ && Actor->IsA<APawn>())
			{
				Report(TEXT("out_of_world"), TEXT("warning"),
					FString::Printf(TEXT("%s fell out of the world (below KillZ %.0f) and was destroyed"), *ClassLabel(Actor), Settings->KillZ),
					FString::Printf(TEXT("out_of_world:%s"), *ClassLabel(Actor)), W->GetTimeSeconds(), Actor);
			}
		}));
	}
	const double Now = FPlatformTime::Seconds();
	const double GameTime = InWorld->GetTimeSeconds();
	const double WorldDelta = LastGameTime >= 0 ? GameTime - LastGameTime : 0.0;
	LastGameTime = GameTime;

	if (RealDelta > HitchSeconds && !InWorld->IsPaused() && Now - SessionStartReal > 10.0)
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetField(TEXT("seconds"), PlaytestJson::Number(RealDelta, true));
		Report(TEXT("hitch"), TEXT("warning"), FString::Printf(TEXT("A frame took %.2f s (hitch)"), RealDelta),
			TEXT("hitch"), GameTime, nullptr, Details);
	}
	if (WorldDelta > 0)
	{
		CheckPawns(InWorld, GameTime, WorldDelta);
	}
	if (Now >= NextActorCheck)
	{
		NextActorCheck = Now + 0.1;
		CheckActors(InWorld, GameTime);
	}
	if (GameTime >= NextGrowthSample)
	{
		NextGrowthSample = GameTime + 10.0;
		CheckGrowth(InWorld, GameTime);
	}
	if (Now >= NextPropertyCheck)
	{
		NextPropertyCheck = Now + PropertyEvery;
		CheckProperties(InWorld, GameTime);
	}
	if (Now >= NextHudCheck)
	{
		NextHudCheck = Now + HudEvery;
		CheckHud(InWorld, GameTime);
	}
}

void FPlaytestDetectors::CheckPawns(UWorld* InWorld, double GameTime, double WorldDelta)
{
	TSet<TWeakObjectPtr<AActor>> Alive;
	for (TActorIterator<APawn> It(InWorld); It; ++It)
	{
		APawn* Pawn = *It;
		if (!IsValid(Pawn))
		{
			continue;
		}
		Alive.Add(Pawn);
		FPawnTrack& Track = Pawns.FindOrAdd(Pawn);
		const FVector Location = Pawn->GetActorLocation();
		if (Track.Frames > 5)
		{
			double Speed = Pawn->GetVelocity().Size();
			if (const UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
			{
				Speed = FMath::Max(Speed, (double)Movement->GetMaxSpeed());
			}
			const double Allowed = FMath::Max(TeleportMin, TeleportFactor * Speed * WorldDelta);
			const double Moved = FVector::Dist(Location, Track.Location);
			if (Moved > Allowed)
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetObjectField(TEXT("from"), Vec(Track.Location));
				Details->SetObjectField(TEXT("to"), Vec(Location));
				Details->SetField(TEXT("moved"), PlaytestJson::Number(FMath::RoundToDouble(Moved)));
				Details->SetField(TEXT("allowed"), PlaytestJson::Number(FMath::RoundToDouble(Allowed)));
				Report(TEXT("teleport"), TEXT("suspicion"),
					FString::Printf(TEXT("%s moved %.0f units in one frame (at most %.0f expected)"), *ClassLabel(Pawn), Moved, Allowed),
					FString::Printf(TEXT("teleport:%s"), *ClassLabel(Pawn)), GameTime, Pawn, Details);
			}
		}
		if (const ACharacter* Character = Cast<ACharacter>(Pawn))
		{
			const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
			if (Movement && Movement->IsFalling())
			{
				if (Track.FallingSince < 0)
				{
					Track.FallingSince = GameTime;
				}
				else if (GameTime - Track.FallingSince > LongFallSeconds)
				{
					Report(TEXT("long_fall"), TEXT("warning"),
						FString::Printf(TEXT("%s has been falling for %.0f s (fell through the floor or off the level?)"), *ClassLabel(Pawn), GameTime - Track.FallingSince),
						FString::Printf(TEXT("long_fall:%s"), *ClassLabel(Pawn)), GameTime, Pawn);
				}
			}
			else
			{
				Track.FallingSince = -1;
			}
		}
		Track.Location = Location;
		++Track.Frames;
	}
	for (auto It = Pawns.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || !Alive.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
}

void FPlaytestDetectors::CheckActors(UWorld* InWorld, double GameTime)
{
	const AWorldSettings* Settings = InWorld->GetWorldSettings();
	const bool bKillZ = Settings && Settings->bEnableWorldBoundsChecks;
	for (TActorIterator<AActor> It(InWorld); It; ++It)
	{
		AActor* Actor = *It;
		const USceneComponent* Root = IsValid(Actor) ? Actor->GetRootComponent() : nullptr;
		if (!Root)
		{
			continue;
		}
		const FTransform& Transform = Root->GetComponentTransform();
		const FVector L = Transform.GetLocation();
		if (Transform.ContainsNaN() || !FMath::IsFinite(L.X) || !FMath::IsFinite(L.Y) || !FMath::IsFinite(L.Z))
		{
			Report(TEXT("nan"), TEXT("error"), FString::Printf(TEXT("%s has an invalid (NaN or infinite) transform"), *ClassLabel(Actor)),
				FString::Printf(TEXT("nan:%s"), *ClassLabel(Actor)), GameTime, nullptr);
			continue;
		}
		if (Root->Mobility != EComponentMobility::Movable)
		{
			continue;
		}
		const FVector Location = Transform.GetLocation();
		if (bKillZ && Location.Z < Settings->KillZ)
		{
			Report(TEXT("out_of_world"), TEXT("warning"),
				FString::Printf(TEXT("%s is below the world's KillZ (%.0f)"), *ClassLabel(Actor), Settings->KillZ),
				FString::Printf(TEXT("out_of_world:%s"), *ClassLabel(Actor)), GameTime, Actor);
		}
		else if (Location.GetAbsMax() > UE_OLD_HALF_WORLD_MAX)
		{
			Report(TEXT("out_of_world"), TEXT("warning"),
				FString::Printf(TEXT("%s is beyond the world bounds (%.0f)"), *ClassLabel(Actor), Location.GetAbsMax()),
				FString::Printf(TEXT("out_of_world:%s"), *ClassLabel(Actor)), GameTime, Actor);
		}
	}
}

void FPlaytestDetectors::CheckGrowth(UWorld* InWorld, double GameTime)
{
	// (counted by class, then named once per class: an open world has tens of thousands of actors)
	TMap<const UClass*, int32> ByClass;
	for (TActorIterator<AActor> It(InWorld); It; ++It)
	{
		if (IsValid(*It))
		{
			++ByClass.FindOrAdd(It->GetClass());
		}
	}
	TMap<FString, int32> Counts;
	for (const TPair<const UClass*, int32>& Class : ByClass)
	{
		FString Label = Class.Key->GetName();
		Label.RemoveFromEnd(TEXT("_C"));
		Counts.FindOrAdd(Label) += Class.Value;
	}
	GrowthSamples.Add(MoveTemp(Counts));
	const int32 Needed = FMath::Max(2, FMath::RoundToInt(GrowthSeconds / 10.0) + 1);
	while (GrowthSamples.Num() > Needed)
	{
		GrowthSamples.RemoveAt(0);
	}
	if (GrowthSamples.Num() < Needed)
	{
		return;
	}
	for (const TPair<FString, int32>& Last : GrowthSamples.Last())
	{
		const int32 First = GrowthSamples[0].FindRef(Last.Key);
		if (Last.Value < 2 * First || Last.Value - First < 50)
		{
			continue;
		}
		bool bRising = true;
		for (int32 i = 1; i < GrowthSamples.Num() && bRising; ++i)
		{
			bRising = GrowthSamples[i].FindRef(Last.Key) >= GrowthSamples[i - 1].FindRef(Last.Key);
		}
		if (bRising)
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetNumberField(TEXT("from"), First);
			Details->SetNumberField(TEXT("to"), Last.Value);
			Details->SetField(TEXT("seconds"), PlaytestJson::Number(GrowthSeconds));
			Report(TEXT("actor_growth"), TEXT("warning"),
				FString::Printf(TEXT("%s actors kept growing from %d to %d in %.0f s (a possible leak)"), *Last.Key, First, Last.Value, GrowthSeconds),
				FString::Printf(TEXT("actor_growth:%s"), *Last.Key), GameTime, nullptr, Details);
		}
	}
}

void FPlaytestDetectors::CheckProperties(UWorld* InWorld, double GameTime)
{
	// What to read on each class is worked out once per class (ChecksOf), not once per object: an open world holds
	// thousands of the same Blueprint, and this runs every PropertyEvery seconds
	TMap<const UClass*, FClassChecks> Plans;
	TMap<const UClass*, bool> Project;
	for (TActorIterator<AActor> It(InWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}
		{
			const FClassChecks& Checks = ChecksOf(Actor->GetClass(), Plans, Project);  // (used before the next ChecksOf adds to Plans)
			if (Checks.Properties.Num())
			{
				CheckObjectProperties(Checks, Actor, Actor, GameTime);
			}
		}
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (!IsValid(Component))
			{
				continue;
			}
			const FClassChecks& Checks = ChecksOf(Component->GetClass(), Plans, Project);
			if (Checks.Properties.Num())
			{
				CheckObjectProperties(Checks, Component, Actor, GameTime);
			}
		}
	}
}

bool FPlaytestDetectors::IsProjectClassCached(const UClass* Class, TMap<const UClass*, bool>& Project) const
{
	if (const bool* Found = Project.Find(Class))
	{
		return *Found;
	}
	return Project.Add(Class, IsProjectClass(Class));
}

const FPlaytestDetectors::FClassChecks& FPlaytestDetectors::ChecksOf(const UClass* Class, TMap<const UClass*, FClassChecks>& Plans,
	TMap<const UClass*, bool>& Project) const
{
	if (const FClassChecks* Found = Plans.Find(Class))
	{
		return *Found;
	}
	FClassChecks Checks;
	if (Class && IsProjectClassCached(Class, Project))
	{
		// A Max partner is found as PlaytestJson::FindProperty finds a property: by its name, else by the name it was
		// authored with (a Blueprint variable's), any case; the first in the class's order wins
		TMap<FString, const FProperty*> ByName;
		TMap<FString, const FProperty*> ByAuthored;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			ByName.FindOrAdd(It->GetName().ToLower(), *It);
			ByAuthored.FindOrAdd(It->GetAuthoredName().ToLower(), *It);
		}
		auto Find = [&ByName, &ByAuthored](const FString& Name) -> const FProperty*
		{
			const FString Key = Name.ToLower();
			if (const FProperty* const* Found = ByName.Find(Key))
			{
				return *Found;
			}
			const FProperty* const* Found = ByAuthored.Find(Key);
			return Found ? *Found : nullptr;
		};
		auto IsNumber = [](const FProperty* Property)
		{
			const FByteProperty* Byte = CastField<FByteProperty>(Property);
			return CastField<FNumericProperty>(Property) && Property->ArrayDim == 1 && !(Byte && Byte->Enum);
		};
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsNumber(Property) || !IsProjectClassCached(Property->GetOwnerClass(), Project))
			{
				continue;  // engine plumbing, or not a number
			}
			FPropertyCheck Check;
			Check.Property = Property;
			Check.Name = Property->GetAuthoredName();
#if WITH_METADATA
			if (const FString* Min = Property->FindMetaData(TEXT("ClampMin")))
			{
				Check.Min = *Min;
				if (Min->IsNumeric())
				{
					Check.MinValue = FCString::Atod(**Min);
				}
			}
			if (const FString* Max = Property->FindMetaData(TEXT("ClampMax")))
			{
				Check.Max = *Max;
				if (Max->IsNumeric())
				{
					Check.MaxValue = FCString::Atod(**Max);
				}
			}
#endif
			if (!Check.Name.StartsWith(TEXT("Max"), ESearchCase::CaseSensitive) && !Check.Name.EndsWith(TEXT("Max"), ESearchCase::CaseSensitive))
			{
				const FProperty* MaxProperty = Find(TEXT("Max") + Check.Name);
				if (!MaxProperty)
				{
					MaxProperty = Find(Check.Name + TEXT("Max"));
				}
				if (MaxProperty && MaxProperty != Property && IsNumber(MaxProperty))
				{
					Check.MaxProperty = MaxProperty;
					Check.MaxName = MaxProperty->GetAuthoredName();
				}
			}
			if (Check.MinValue.IsSet() || Check.MaxValue.IsSet() || Check.MaxProperty)
			{
				Checks.Properties.Add(MoveTemp(Check));  // (one with nothing to compare against can't report)
			}
		}
	}
	return Plans.Add(Class, MoveTemp(Checks));
}

void FPlaytestDetectors::CheckObjectProperties(const FClassChecks& Checks, UObject* Object, const AActor* Actor, double GameTime)
{
	auto Where = [Object, Actor]()
	{
		return Object == Actor ? ClassLabel(Actor) : FString::Printf(TEXT("%s.%s"), *ClassLabel(Actor), *Object->GetName());
	};
	for (const FPropertyCheck& Check : Checks.Properties)
	{
		double Value = 0;
		if (!NumericValue(Check.Property, Object, Value))
		{
			continue;
		}
		const bool bBelow = Check.MinValue.IsSet() && Value < Check.MinValue.GetValue() - 1e-4;
		const bool bAbove = Check.MaxValue.IsSet() && Value > Check.MaxValue.GetValue() + 1e-4;
		if (bBelow || bAbove)
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("property"), Check.Name);
			Details->SetField(TEXT("value"), PlaytestJson::Number(Value));
			Details->SetStringField(TEXT("clamp"), FString::Printf(TEXT("[%s, %s]"), *Check.Min, *Check.Max));
			Report(TEXT("clamp"), TEXT("warning"),
				FString::Printf(TEXT("%s %s is %s, outside its allowed range %s..%s"), *Where(), *Check.Name, *FormatNumber(Value),
					*Check.Min, *Check.Max),
				FString::Printf(TEXT("clamp:%s.%s"), *ClassLabel(Object), *Check.Name), GameTime, Actor, Details);
		}
		double MaxValue = 0;
		if (Check.MaxProperty && NumericValue(Check.MaxProperty, Object, MaxValue) && MaxValue > 0 && Value > MaxValue + 1e-4)
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("property"), Check.Name);
			Details->SetField(TEXT("value"), PlaytestJson::Number(Value));
			Details->SetStringField(TEXT("max_property"), Check.MaxName);
			Details->SetField(TEXT("max"), PlaytestJson::Number(MaxValue));
			Report(TEXT("over_max"), TEXT("warning"),
				FString::Printf(TEXT("%s %s is %s, above %s %s"), *Where(), *Check.Name, *FormatNumber(Value), *Check.MaxName, *FormatNumber(MaxValue)),
				FString::Printf(TEXT("over_max:%s.%s"), *ClassLabel(Object), *Check.Name), GameTime, Actor, Details);
		}
	}
}

void FPlaytestDetectors::CheckHud(UWorld* InWorld, double GameTime)
{
	static const FRegexPattern Fraction(TEXT("(?<![0-9/.])([0-9]+)\\s*/\\s*([0-9]+)(?![0-9/.])"));
	for (const PlaytestScreen::FShownText& Shown : PlaytestScreen::Read(InWorld))
	{
		const FString& Text = Shown.Text;
		int32 Slashes = 0;
		for (TCHAR C : Text)
		{
			Slashes += C == TEXT('/');
		}
		if (Slashes != 1 || Text.Contains(TEXT(":")))
		{
			continue;  // dates, times and paths aren't counts
		}
		FRegexMatcher Matcher(Fraction, Text);
		while (Matcher.FindNext())
		{
			const int64 A = FCString::Atoi64(*Matcher.GetCaptureGroup(1));
			const int64 B = FCString::Atoi64(*Matcher.GetCaptureGroup(2));
			if (B > 0 && A > B)
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("key"), Shown.Key);
				Details->SetStringField(TEXT("text"), Text);
				Details->SetNumberField(TEXT("count"), (double)A);
				Details->SetNumberField(TEXT("max"), (double)B);
				Report(TEXT("hud_fraction"), TEXT("warning"),
					FString::Printf(TEXT("On-screen text %s shows \"%s\": %lld is more than its stated maximum %lld"), *Shown.Key, *Text, A, B),
					FString::Printf(TEXT("hud_fraction:%s"), *Shown.Key), GameTime, nullptr, Details);
			}
		}
	}
}
