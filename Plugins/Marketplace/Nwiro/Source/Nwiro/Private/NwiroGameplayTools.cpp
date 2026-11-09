// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroGameplayTools.h"
#include "NwiroTransactionHelper.h"
#include "Editor.h"
#include "NwiroEditorTools.h"
#include "Engine/World.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "EditorAssetLibrary.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Sound/AmbientSound.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/HUD.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IProjectManager.h"
#include "Json.h"
#include "Logging/MessageLog.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroGameplay, Log, All);

static UWorld* GetGPWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// ============================================================
// BUILD NAVIGATION
// ============================================================

FString FNwiroGameplayTools::BuildNavigation(const FString& JsonCommand)
{
	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) return TEXT("{\"success\":false,\"error\":\"No navigation system\"}");

	// Check for NavMeshBoundsVolume
	TActorIterator<ANavMeshBoundsVolume> Bounds(World);
	if (!Bounds)
		return TEXT("{\"success\":false,\"error\":\"No NavMeshBoundsVolume in level. Add one first.\"}");

	NavSys->Build();
	return TEXT("{\"success\":true,\"message\":\"Navigation build started\"}");
}

// ============================================================
// QUERY NAVIGATION PATH
// ============================================================

FString FNwiroGameplayTools::QueryNavigationPath(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	double StartX = Cmd->GetNumberField(TEXT("startX"));
	double StartY = Cmd->GetNumberField(TEXT("startY"));
	double StartZ = Cmd->GetNumberField(TEXT("startZ"));
	double EndX = Cmd->GetNumberField(TEXT("endX"));
	double EndY = Cmd->GetNumberField(TEXT("endY"));
	double EndZ = Cmd->GetNumberField(TEXT("endZ"));

	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) return TEXT("{\"success\":false,\"error\":\"No navigation system\"}");

	FPathFindingQuery Query;
	Query.StartLocation = FVector(StartX, StartY, StartZ);
	Query.EndLocation = FVector(EndX, EndY, EndZ);

	FPathFindingResult Result = NavSys->FindPathSync(Query);

	TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject());
	Out->SetBoolField(TEXT("success"), true);
	Out->SetBoolField(TEXT("pathFound"), Result.IsSuccessful());

	if (Result.IsSuccessful() && Result.Path.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> PathPts;
		float TotalDist = 0;
		FVector Prev = FVector(StartX, StartY, StartZ);
		for (const FNavPathPoint& Pt : Result.Path->GetPathPoints())
		{
			TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetNumberField(TEXT("x"), Pt.Location.X);
			P->SetNumberField(TEXT("y"), Pt.Location.Y);
			P->SetNumberField(TEXT("z"), Pt.Location.Z);
			PathPts.Add(MakeShareable(new FJsonValueObject(P)));
			TotalDist += FVector::Dist(Prev, Pt.Location);
			Prev = Pt.Location;
		}
		Out->SetArrayField(TEXT("pathPoints"), PathPts);
		Out->SetNumberField(TEXT("distance"), TotalDist);
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Out, W);
	return OutStr;
}

// ============================================================
// GET NAVIGATION INFO
// ============================================================

FString FNwiroGameplayTools::GetNavigationInfo(const FString& JsonCommand)
{
	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) return TEXT("{\"success\":false,\"error\":\"No navigation system\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("isNavigationBuilt"), NavSys->IsNavigationBuilt(World->GetWorldSettings()));

	int32 BoundsCount = 0;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It) BoundsCount++;
	Result->SetNumberField(TEXT("boundsVolumeCount"), BoundsCount);

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

// ============================================================
// SPAWN SOUND
// ============================================================

FString FNwiroGameplayTools::SpawnSound(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SoundPath = Cmd->GetStringField(TEXT("sound"));
	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;
	FString Label = Cmd->GetStringField(TEXT("label"));

	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	USoundBase* Sound = Cast<USoundBase>(UEditorAssetLibrary::LoadAsset(SoundPath));
	if (!Sound) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sound not found: %s\"}"), *SoundPath);

	// >>> NWIRO_PARITY BEGIN: spawn-sound-dirty
	// Validated above: the helper's Modify() dirties the package even if the spawn never happens.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SpawnSound", "AI: Spawn Sound"), /*no object: matches UEditorEngine::AddActor, so a World Partition map package stays clean*/ nullptr);

	FActorSpawnParameters Params;
	AAmbientSound* SoundActor = World->SpawnActor<AAmbientSound>(FVector(X, Y, Z), FRotator::ZeroRotator, Params);
	if (!SoundActor) return TEXT("{\"success\":false,\"error\":\"Failed to spawn AmbientSound\"}");
	Tx.AlsoModify(SoundActor);

	SoundActor->GetAudioComponent()->SetSound(Sound);
	if (!Label.IsEmpty()) SoundActor->SetActorLabel(Label);

	if (Cmd->HasField(TEXT("volume")))
		SoundActor->GetAudioComponent()->SetVolumeMultiplier((float)Cmd->GetNumberField(TEXT("volume")));
	if (Cmd->HasField(TEXT("pitch")))
		SoundActor->GetAudioComponent()->SetPitchMultiplier((float)Cmd->GetNumberField(TEXT("pitch")));
	if (Cmd->HasField(TEXT("autoActivate")))
		SoundActor->GetAudioComponent()->bAutoActivate = Cmd->GetBoolField(TEXT("autoActivate"));

	// GetOutermost() follows the One File Per Actor package, so this dirties the actor's own
	// package on a World Partition map and the map package on a classic .umap.
	SoundActor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"sound\":\"%s\",\"dirtyAfter\":%s}"),
		*SoundActor->GetActorLabel(), *Sound->GetName(),
		SoundActor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: spawn-sound-dirty
}

// ============================================================
// SET AUDIO PROPERTIES
// ============================================================

FString FNwiroGameplayTools::SetAudioProperties(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetGPWorld();
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName) { Actor = *It; break; }
	}
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UAudioComponent* Audio = Actor->FindComponentByClass<UAudioComponent>();
	if (!Audio) return TEXT("{\"success\":false,\"error\":\"No audio component\"}");

	if (Cmd->HasField(TEXT("volume"))) Audio->SetVolumeMultiplier((float)Cmd->GetNumberField(TEXT("volume")));
	if (Cmd->HasField(TEXT("pitch"))) Audio->SetPitchMultiplier((float)Cmd->GetNumberField(TEXT("pitch")));
	if (Cmd->HasField(TEXT("autoActivate"))) Audio->bAutoActivate = Cmd->GetBoolField(TEXT("autoActivate"));

	Actor->MarkPackageDirty();
	// TODO: actor mutation should save the LEVEL package, not the actor — UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *Actor->GetActorLabel());
}

// ============================================================
// GET SOUND INFO
// ============================================================

FString FNwiroGameplayTools::GetSoundInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	USoundBase* Sound = Cast<USoundBase>(UEditorAssetLibrary::LoadAsset(Path));
	if (!Sound) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sound not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Sound->GetName());
	Result->SetStringField(TEXT("class"), Sound->GetClass()->GetName());
	Result->SetNumberField(TEXT("duration"), Sound->Duration);

	if (USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		Result->SetNumberField(TEXT("sampleRate"), Wave->GetSampleRateForCurrentPlatform());
		Result->SetNumberField(TEXT("numChannels"), Wave->NumChannels);
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

// ============================================================
// GAME FRAMEWORK - Create BP helpers
// ============================================================

static FString CreateFrameworkBP(const FString& JsonCommand, UClass* ParentClass, const FString& DefaultPrefix, const FString& DefaultPath)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = DefaultPath;
	if (!Name.StartsWith(DefaultPrefix)) Name = DefaultPrefix + Name;

	FString FullPath = Path / Name;
	// Idempotent guard — CreateBlueprint asserts if the BP already exists in the package.
	if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName(), *ParentClass->GetName());
	}
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package) return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	if (UBlueprint* Existing = FindObject<UBlueprint>(Package, *Name))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName(), *ParentClass->GetName());
	}

	// >>> NWIRO_PARITY BEGIN: create-framework-bp-ordering
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateFrameworkBP", "AI: Create Framework Blueprint"));

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	// Registration deliberately follows the create: Modify() dirties the package even when the create
	// fails, and a failed call must leave nothing behind. There is no pre-create state to record here -
	// UE has no undo for asset creation (the Content Browser's own create path opens no transaction and
	// Ctrl+Z never deletes a created asset), so Layer 0's guarantee for an asset-creating tool is that
	// the new work is VISIBLE as unsaved, not that one undo removes it.
	if (!BP) return TEXT("{\"success\":false,\"error\":\"Failed to create blueprint\"}");
	Tx.AlsoModify(Package);
	Tx.AlsoModify(BP);

	FKismetEditorUtilities::CompileBlueprint(BP);
	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();
	// <<< NWIRO_PARITY END: create-framework-bp-ordering

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\"}"),
		*Name, *BP->GetPathName(), *ParentClass->GetName());
}

FString FNwiroGameplayTools::CreateGameMode(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, AGameModeBase::StaticClass(), TEXT("GM_"), TEXT("/Game/Blueprints"));
}

FString FNwiroGameplayTools::CreatePlayerController(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, APlayerController::StaticClass(), TEXT("PC_"), TEXT("/Game/Blueprints"));
}

FString FNwiroGameplayTools::CreateGameState(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, AGameStateBase::StaticClass(), TEXT("GS_"), TEXT("/Game/Blueprints"));
}

FString FNwiroGameplayTools::CreatePlayerState(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, APlayerState::StaticClass(), TEXT("PS_"), TEXT("/Game/Blueprints"));
}

FString FNwiroGameplayTools::CreateHUD(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, AHUD::StaticClass(), TEXT("HUD_"), TEXT("/Game/Blueprints"));
}

FString FNwiroGameplayTools::GetGameFrameworkInfo(const FString& JsonCommand)
{
	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	AWorldSettings* WS = World->GetWorldSettings();
	if (WS)
	{
		Result->SetStringField(TEXT("defaultGameMode"), WS->DefaultGameMode ? WS->DefaultGameMode->GetPathName() : TEXT("None"));
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

// ============================================================
// BUILD / VALIDATION
// ============================================================

FString FNwiroGameplayTools::GetProjectInfo(const FString& JsonCommand)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Result->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

FString FNwiroGameplayTools::ListProjectModules(const FString& JsonCommand)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	TArray<TSharedPtr<FJsonValue>> Modules;
	TArray<FModuleStatus> ModuleStatuses;
	FModuleManager::Get().QueryModules(ModuleStatuses);

	int32 Count = 0;
	for (const FModuleStatus& Status : ModuleStatuses)
	{
		if (Count >= 100) break; // Cap
		TSharedRef<FJsonObject> M = MakeShareable(new FJsonObject());
		M->SetStringField(TEXT("name"), Status.Name);
		M->SetBoolField(TEXT("loaded"), Status.bIsLoaded);
		Modules.Add(MakeShareable(new FJsonValueObject(M)));
		Count++;
	}
	Result->SetArrayField(TEXT("modules"), Modules);

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

FString FNwiroGameplayTools::ValidateAssets(const FString& JsonCommand)
{
	// Basic asset validation - check for missing references
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	int32 TotalAssets = Assets.Num();
	int32 Issues = 0;
	TArray<TSharedPtr<FJsonValue>> Problems;

	// Check for assets with missing dependencies (simple check)
	// Full validation would use FAssetValidator but that's not always available
	for (const FAssetData& A : Assets)
	{
		if (!A.IsValid())
		{
			TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetStringField(TEXT("asset"), A.AssetName.ToString());
			P->SetStringField(TEXT("issue"), TEXT("Invalid asset data"));
			Problems.Add(MakeShareable(new FJsonValueObject(P)));
			Issues++;
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("totalAssets"), TotalAssets);
	Result->SetNumberField(TEXT("issues"), Issues);
	Result->SetArrayField(TEXT("problems"), Problems);

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}


namespace NwiroVerificationMap
{
	// This device exists only for the synchronous check. Structured engine message-log
	// counts remain authoritative even if output mirroring is disabled or diagnostics
	// exceed this presentation limit. Never parse a localized completion sentence.
	class FScopedMapCheckCapture final : public FOutputDevice
	{
	public:
		struct FEntry
		{
			FString Message;
			ELogVerbosity::Type Severity;
		};

		FScopedMapCheckCapture() { GLog->AddOutputDevice(this); }
		~FScopedMapCheckCapture() { GLog->RemoveOutputDevice(this); }

		TArray<FEntry> Snapshot() const
		{
			FScopeLock Scope(&Lock);
			return Entries;
		}

		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity,
			const FName& Category) override
		{
			if (!IsInGameThread() || Category != FName(TEXT("MapCheck"))) return;
			const ELogVerbosity::Type Severity =
				static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
			if (Severity != ELogVerbosity::Error
				&& Severity != ELogVerbosity::Fatal
				&& Severity != ELogVerbosity::Warning) return;
			FScopeLock Scope(&Lock);
			if (Entries.Num() < 500) Entries.Add({FString(Text), Severity});
		}

	private:
		mutable FCriticalSection Lock;
		TArray<FEntry> Entries;
	};

	static FString RunMapCheck(UWorld* World)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("verificationId"),
			FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
		Result->SetStringField(TEXT("verificationKind"), TEXT("mapCheck"));
		Result->SetBoolField(TEXT("savePerformed"), false);
		Result->SetArrayField(TEXT("messages"), {});
		Result->SetStringField(TEXT("summary"), TEXT(""));
		Result->SetBoolField(TEXT("truncated"), false);
		if (World)
		{
			Result->SetStringField(TEXT("world"), World->GetPathName());
			Result->SetBoolField(TEXT("packageDirtyBefore"), World->GetOutermost()->IsDirty());
		}

		auto Finish = [&]() -> FString
		{
			Result->SetStringField(TEXT("completedAt"), FDateTime::UtcNow().ToIso8601());
			if (World) Result->SetBoolField(TEXT("packageDirty"), World->GetOutermost()->IsDirty());
			FString Out;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Result, Writer);
			return Out;
		};
		auto Unknown = [&](const TCHAR* Error) -> FString
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("verificationStatus"), TEXT("unknown"));
			Result->SetStringField(TEXT("error"), Error);
			Result->SetField(TEXT("errorCount"), MakeShared<FJsonValueNull>());
			Result->SetField(TEXT("warningCount"), MakeShared<FJsonValueNull>());
			return Finish();
		};

		if (!IsInGameThread()) return Unknown(TEXT("Map Check requires the game thread"));
		if (!GEditor || !World) return Unknown(TEXT("No editor world"));
		if (!GLog || !GWarn || !FMessageLog::OnGetLog().IsBound())
			return Unknown(TEXT("Structured Map Check logging is unavailable"));

		FScopedMapCheckCapture Capture;
		// Own an empty structured page before dispatch. Even an editor override that
		// handles the command without checking cannot expose earlier diagnostics.
		FMessageLog MapCheckLog(TEXT("MapCheck"));
		MapCheckLog.NewPage(FText::FromString(TEXT("Nwiro verification ")
			+ Result->GetStringField(TEXT("verificationId"))));
		// The public command retains our page and avoids opening any results UI.
		const bool bRan = GEditor->Exec(World,
			TEXT("MAP CHECK DONTDISPLAYDIALOG NOCLEARLOG"), *GWarn);
		if (!bRan) return Unknown(TEXT("The editor could not run Map Check"));

		if (MapCheckLog.NumMessages(EMessageSeverity::Info) == 0)
			return Unknown(TEXT("Map Check returned no structured result"));
		const int32 ErrorCount = MapCheckLog.NumMessages(EMessageSeverity::Error);
		// NumMessages is an inclusive severity filter, just as in UEditorEngine::Map_Check.
		const int32 WarningCount =
			MapCheckLog.NumMessages(EMessageSeverity::Warning) - ErrorCount;

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const FScopedMapCheckCapture::FEntry& Entry : Capture.Snapshot())
		{
			const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
			Message->SetStringField(TEXT("severity"),
				Entry.Severity == ELogVerbosity::Warning ? TEXT("Warning") : TEXT("Error"));
			Message->SetStringField(TEXT("message"), Entry.Message);
			Messages.Add(MakeShared<FJsonValueObject>(Message));
		}
		Result->SetBoolField(TEXT("success"), true); // The check ran; counts decide map validity.
		Result->SetStringField(TEXT("verificationStatus"), TEXT("completed"));
		Result->SetNumberField(TEXT("errorCount"), ErrorCount);
		Result->SetNumberField(TEXT("warningCount"), WarningCount);
		Result->SetArrayField(TEXT("messages"), Messages);
		Result->SetBoolField(TEXT("truncated"), Messages.Num() < ErrorCount + WarningCount);
		Result->SetStringField(TEXT("summary"), FString::Printf(
			TEXT("Map check complete: %d Error(s), %d Warning(s)."), ErrorCount, WarningCount));
		return Finish();
	}
}

FString FNwiroGameplayTools::GetMapCheckErrors(const FString& JsonCommand)
{
	return NwiroVerificationMap::RunMapCheck(
		GEditor && IsInGameThread() ? GEditor->GetEditorWorldContext().World() : nullptr);
}

FString FNwiroGameplayTools::GetBuildConfiguration(const FString& JsonCommand)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

#if UE_BUILD_DEBUG
	Result->SetStringField(TEXT("configuration"), TEXT("Debug"));
#elif UE_BUILD_DEVELOPMENT
	Result->SetStringField(TEXT("configuration"), TEXT("Development"));
#elif UE_BUILD_SHIPPING
	Result->SetStringField(TEXT("configuration"), TEXT("Shipping"));
#else
	Result->SetStringField(TEXT("configuration"), TEXT("Unknown"));
#endif

	Result->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroMapCheckVerificationSpec.inl"
#endif
