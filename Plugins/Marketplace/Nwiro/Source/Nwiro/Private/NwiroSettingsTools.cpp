// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroSettingsTools.h"
#include "NwiroDispatchFence.h"
#include "NwiroIsolatedPython.h"
#include "NwiroAssetGuard.h"
#include "NwiroTransactionHelper.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameMapsSettings.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "BlueprintCompilationManager.h"
#include "EditorAssetLibrary.h"
#include "EngineUtils.h"
#include "Json.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Components/SceneComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroSettings, Log, All);

// ============================================================
// HELPERS
// ============================================================

static UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

static AWorldSettings* GetCurrentWorldSettings()
{
	UWorld* World = GetEditorWorld();
	return World ? World->GetWorldSettings() : nullptr;
}

static UClass* LoadClassFromPath(const FString& Path)
{
	if (Path.IsEmpty()) return nullptr;
	// Guard: an over-long class path gets wrapped in FName during class
	// resolution and trips UE NAME_SIZE (~1024 chars) with a fatal assert.
	// (fuzz: set_game_mode with a 100KB gameModeClass crashed the editor.)
	if (Path.Len() > 1024) return nullptr;

	// Try loading as blueprint - multiple path formats
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
	if (!BP)
	{
		// Try Package.AssetName format
		FString AssetName = FPaths::GetBaseFilename(Path);
		FString FullPath = Path + TEXT(".") + AssetName;
		BP = LoadObject<UBlueprint>(nullptr, *FullPath);
	}
	if (!BP)
	{
		// Try with /Game/ prefix
		if (!Path.StartsWith(TEXT("/")))
		{
			BP = LoadObject<UBlueprint>(nullptr, *(TEXT("/Game/") + Path));
			if (!BP) BP = LoadObject<UBlueprint>(nullptr, *(TEXT("/Game/Blueprints/") + Path));
			if (!BP)
			{
				FString WithExt = TEXT("/Game/Blueprints/") + Path + TEXT(".") + Path;
				BP = LoadObject<UBlueprint>(nullptr, *WithExt);
			}
		}
	}
	if (BP && BP->GeneratedClass) return BP->GeneratedClass;

	// Try loading class directly
	UClass* C = LoadObject<UClass>(nullptr, *Path);
	if (C) return C;

	// Search by name in asset registry
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Contains(Path, ESearchCase::IgnoreCase))
		{
			UBlueprint* FoundBP = Cast<UBlueprint>(NwiroSafeRegistryLoad(Asset));
			if (FoundBP && FoundBP->GeneratedClass) return FoundBP->GeneratedClass;
		}
	}

	return nullptr;
}

// ============================================================
// GET WORLD SETTINGS
// ============================================================

FString FNwiroSettingsTools::GetWorldSettings()
{
	AWorldSettings* WS = GetCurrentWorldSettings();
	if (!WS) return TEXT("{\"success\": false, \"error\": \"No world settings\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	// Gravity
	Result->SetBoolField(TEXT("globalGravitySet"), WS->bGlobalGravitySet);
	Result->SetNumberField(TEXT("globalGravityZ"), WS->GlobalGravityZ);

	// Game mode
	if (WS->DefaultGameMode)
	{
		Result->SetStringField(TEXT("defaultGameMode"), WS->DefaultGameMode->GetPathName());
	}
	else
	{
		Result->SetStringField(TEXT("defaultGameMode"), TEXT("None"));
	}

	// Kill Z
	Result->SetNumberField(TEXT("killZ"), WS->KillZ);

	// World name
	UWorld* World = GetEditorWorld();
	if (World)
	{
		Result->SetStringField(TEXT("worldName"), World->GetName());
		Result->SetStringField(TEXT("mapPath"), World->GetPathName());
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SET WORLD SETTINGS
// ============================================================

FString FNwiroSettingsTools::SetWorldSettings(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	AWorldSettings* WS = GetCurrentWorldSettings();
	if (!WS) return TEXT("{\"success\": false, \"error\": \"No world settings\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetWorldSettings", "AI: Set World Settings"), WS);
	TArray<FString> Changes;

	if (Cmd->HasField(TEXT("globalGravityZ")))
	{
		WS->bGlobalGravitySet = true;
		WS->GlobalGravityZ = (float)Cmd->GetNumberField(TEXT("globalGravityZ"));
		Changes.Add(FString::Printf(TEXT("Gravity: %.1f"), WS->GlobalGravityZ));
	}

	if (Cmd->HasField(TEXT("killZ")))
	{
		WS->KillZ = (float)Cmd->GetNumberField(TEXT("killZ"));
		Changes.Add(FString::Printf(TEXT("KillZ: %.1f"), WS->KillZ));
	}

	if (Cmd->HasField(TEXT("gameModeOverride")))
	{
		FString GMPath = Cmd->GetStringField(TEXT("gameModeOverride"));
		if (GMPath.IsEmpty() || GMPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			WS->DefaultGameMode = nullptr;
			Changes.Add(TEXT("GameMode: cleared"));
		}
		else
		{
			UClass* GMClass = LoadClassFromPath(GMPath);
			if (GMClass && GMClass->IsChildOf(AGameModeBase::StaticClass()))
			{
				WS->DefaultGameMode = GMClass;
				Changes.Add(FString::Printf(TEXT("GameMode: %s"), *GMClass->GetName()));
			}
		}
	}

	WS->MarkPackageDirty();
	// TODO: WorldSettings lives in the LEVEL package; persisting requires UEditorLevelLibrary::SaveCurrentLevel.

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("changes"), FString::Join(Changes, TEXT(", ")));

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroSettings, Log, TEXT("SetWorldSettings: %s"), *FString::Join(Changes, TEXT(", ")));
	return Out;
}

// ============================================================
// SET GAME MODE
// ============================================================

FString FNwiroSettingsTools::SetGameMode(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	AWorldSettings* WS = GetCurrentWorldSettings();
	if (!WS) return TEXT("{\"success\": false, \"error\": \"No world settings\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetGameMode", "AI: Set Game Mode"), WS);
	TArray<FString> Changes;

	// Set GameMode class (accept both gameModeClass and gameMode)
	FString GMPath = Cmd->GetStringField(TEXT("gameModeClass"));
	if (GMPath.IsEmpty()) GMPath = Cmd->GetStringField(TEXT("gameMode"));
	if (!GMPath.IsEmpty())
	{
		UClass* GMClass = LoadClassFromPath(GMPath);
		if (GMClass && GMClass->IsChildOf(AGameModeBase::StaticClass()))
		{
			// SAFETY: if the agent passed a freshly-edited BP that hasn't been
			// recompiled, ClassConstructor is null and GetDefaultObject below
			// asserts in UObjectGlobals.cpp:3396, killing the editor. Same
			// problem as spawn_actor — apply the same compile-and-retry guard.
			auto IsClassConstructible = [](UClass* C)
			{
				return C && C->IsValidLowLevel() && C->ClassConstructor != nullptr
					&& C->ClassWithin != nullptr
					&& !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
			};
			if (!IsClassConstructible(GMClass))
			{
				if (UBlueprint* GMBPSrc = Cast<UBlueprint>(GMClass->ClassGeneratedBy))
				{
					FBlueprintEditorUtils::RefreshAllNodes(GMBPSrc);
					FKismetEditorUtilities::CompileBlueprint(GMBPSrc, EBlueprintCompileOptions::SkipGarbageCollection);
					FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
					if (GMBPSrc->GeneratedClass) GMClass = GMBPSrc->GeneratedClass;
				}
				if (!IsClassConstructible(GMClass))
				{
					return FString::Printf(TEXT("{\"success\":false,\"error\":\"GameMode class '%s' is not constructible (BP has compile errors or stale skeleton). Recompile the GameMode blueprint before set_game_mode.\"}"),
						*GMPath);
				}
			}
			auto CompileIfNeeded = [&IsClassConstructible](UClass*& C)
			{
				if (!C || IsClassConstructible(C)) return;
				if (UBlueprint* B = Cast<UBlueprint>(C->ClassGeneratedBy))
				{
					FBlueprintEditorUtils::RefreshAllNodes(B);
					FKismetEditorUtilities::CompileBlueprint(B, EBlueprintCompileOptions::SkipGarbageCollection);
					FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
					if (B->GeneratedClass) C = B->GeneratedClass;
				}
				if (!IsClassConstructible(C)) C = nullptr;
			};

			WS->DefaultGameMode = GMClass;
			Changes.Add(FString::Printf(TEXT("GameMode: %s"), *GMClass->GetName()));

			// Set DefaultPawnClass on the GameMode CDO
			FString PawnPath = Cmd->GetStringField(TEXT("defaultPawnClass"));
			if (!PawnPath.IsEmpty())
			{
				UClass* PawnClass = LoadClassFromPath(PawnPath);
				CompileIfNeeded(PawnClass);
				if (PawnClass)
				{
					AGameModeBase* GMCDO = GMClass->GetDefaultObject<AGameModeBase>();
					if (GMCDO)
					{
						GMCDO->DefaultPawnClass = PawnClass;
						GMCDO->Modify();
						GMCDO->MarkPackageDirty();

						// Compile so PIE picks up the change in this session.
						// Persistence to disk is handled by editor save flow (user Ctrl+S
						// or save-on-close prompt) so AI mutations can be cleanly undone
						// without an eager half-written .uasset on disk.
						UBlueprint* GMBP = Cast<UBlueprint>(GMClass->ClassGeneratedBy);
						if (GMBP)
						{
							Tx.AlsoModify(GMBP);
							FKismetEditorUtilities::CompileBlueprint(GMBP);
							GMBP->MarkPackageDirty();
						}

						Changes.Add(FString::Printf(TEXT("DefaultPawn: %s"), *PawnClass->GetName()));
					}
				}
			}

			// Set PlayerControllerClass on the GameMode CDO
			FString PCPath = Cmd->GetStringField(TEXT("playerControllerClass"));
			if (!PCPath.IsEmpty())
			{
				UClass* PCClass = LoadClassFromPath(PCPath);
				CompileIfNeeded(PCClass);
				if (PCClass)
				{
					AGameModeBase* GMCDO = GMClass->GetDefaultObject<AGameModeBase>();
					if (GMCDO)
					{
						GMCDO->PlayerControllerClass = PCClass;
						GMCDO->MarkPackageDirty();
						Changes.Add(FString::Printf(TEXT("PlayerController: %s"), *PCClass->GetName()));
					}
				}
			}

			// Set HUDClass
			FString HUDPath = Cmd->GetStringField(TEXT("hudClass"));
			if (!HUDPath.IsEmpty())
			{
				UClass* HUDClass = LoadClassFromPath(HUDPath);
				CompileIfNeeded(HUDClass);
				if (HUDClass)
				{
					AGameModeBase* GMCDO = GMClass->GetDefaultObject<AGameModeBase>();
					if (GMCDO)
					{
						GMCDO->HUDClass = HUDClass;
						GMCDO->MarkPackageDirty();
						Changes.Add(FString::Printf(TEXT("HUD: %s"), *HUDClass->GetName()));
					}
				}
			}

			// Set GameStateClass
			FString GSPath = Cmd->GetStringField(TEXT("gameStateClass"));
			if (!GSPath.IsEmpty())
			{
				UClass* GSClass = LoadClassFromPath(GSPath);
				CompileIfNeeded(GSClass);
				if (GSClass)
				{
					AGameModeBase* GMCDO = GMClass->GetDefaultObject<AGameModeBase>();
					if (GMCDO)
					{
						GMCDO->GameStateClass = GSClass;
						GMCDO->MarkPackageDirty();
						Changes.Add(FString::Printf(TEXT("GameState: %s"), *GSClass->GetName()));
					}
				}
			}

			// Set SpectatorClass
			FString SpecPath = Cmd->GetStringField(TEXT("spectatorClass"));
			if (!SpecPath.IsEmpty())
			{
				UClass* SpecClass = LoadClassFromPath(SpecPath);
				CompileIfNeeded(SpecClass);
				if (SpecClass)
				{
					AGameModeBase* GMCDO = GMClass->GetDefaultObject<AGameModeBase>();
					if (GMCDO)
					{
						GMCDO->SpectatorClass = SpecClass;
						GMCDO->MarkPackageDirty();
						Changes.Add(FString::Printf(TEXT("Spectator: %s"), *SpecClass->GetName()));
					}
				}
			}
		}
		else
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"GameMode class not found: %s\"}"), *GMPath);
		}
	}

	// Also set on world settings
	if (Cmd->HasField(TEXT("setOnWorldSettings")) && Cmd->GetBoolField(TEXT("setOnWorldSettings")))
	{
		WS->MarkPackageDirty();
		// TODO: WorldSettings lives in the LEVEL package; persisting requires UEditorLevelLibrary::SaveCurrentLevel.
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("changes"), FString::Join(Changes, TEXT(", ")));

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroSettings, Log, TEXT("SetGameMode: %s"), *FString::Join(Changes, TEXT(", ")));
	return Out;
}

// ============================================================
// GET PROJECT SETTINGS
// ============================================================

FString FNwiroSettingsTools::GetProjectSettings(const FString& JsonCommand)
{
	UGameMapsSettings* MapSettings = GetMutableDefault<UGameMapsSettings>();
	if (!MapSettings) return TEXT("{\"success\": false, \"error\": \"Cannot access GameMapsSettings\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("globalDefaultGameMode"), UGameMapsSettings::GetGlobalDefaultGameMode());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SET PROJECT SETTINGS
// ============================================================

FString FNwiroSettingsTools::SetProjectSettings(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	UGameMapsSettings* MapSettings = GetMutableDefault<UGameMapsSettings>();
	if (!MapSettings) return TEXT("{\"success\": false, \"error\": \"Cannot access GameMapsSettings\"}");

	TArray<FString> Changes;

	if (Cmd->HasField(TEXT("globalDefaultGameMode")))
	{
		FString GM = Cmd->GetStringField(TEXT("globalDefaultGameMode"));
		UGameMapsSettings::SetGlobalDefaultGameMode(GM);
		Changes.Add(FString::Printf(TEXT("GlobalGameMode: %s"), *GM));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("changes"), FString::Join(Changes, TEXT(", ")));

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroSettings, Log, TEXT("SetProjectSettings: %s"), *FString::Join(Changes, TEXT(", ")));
	return Out;
}

// ============================================================
// GET LEVEL ACTORS
// ============================================================

FString FNwiroSettingsTools::GetLevelActors(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		Cmd = MakeShareable(new FJsonObject());
	}

	FString ClassFilter = Cmd->GetStringField(TEXT("classFilter"));
	// Optional free-text query — matches against name, label, OR class.
	// Also accept the same string under "search" or "name" since the LLM tends
	// to invent both.
	FString Query;
	if (Cmd->HasField(TEXT("query")))   Query = Cmd->GetStringField(TEXT("query"));
	else if (Cmd->HasField(TEXT("search"))) Query = Cmd->GetStringField(TEXT("search"));
	else if (Cmd->HasField(TEXT("name")))   Query = Cmd->GetStringField(TEXT("name"));

	int32 MaxResults = Cmd->HasField(TEXT("maxResults")) ? (int32)Cmd->GetNumberField(TEXT("maxResults")) : 500;

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\": false, \"error\": \"No editor world\"}");

	TArray<TSharedPtr<FJsonValue>> ActorArr;
	int32 Count = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (Count >= MaxResults) break;
		AActor* Actor = *It;
		if (!Actor) continue;

		FString ClassName = Actor->GetClass()->GetName();
		FString ActorName = Actor->GetName();
		FString ActorLabel = Actor->GetActorLabel();

		if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (!Query.IsEmpty())
		{
			const bool bHit =
				ActorName.Contains(Query, ESearchCase::IgnoreCase) ||
				ActorLabel.Contains(Query, ESearchCase::IgnoreCase) ||
				ClassName.Contains(Query, ESearchCase::IgnoreCase);
			if (!bHit) continue;
		}

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("class"), ClassName);

		FVector Loc = Actor->GetActorLocation();
		Obj->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Loc.X, Loc.Y, Loc.Z));

		ActorArr.Add(MakeShareable(new FJsonValueObject(Obj)));
		Count++;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("actors"), ActorArr);
	Result->SetNumberField(TEXT("count"), ActorArr.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SPAWN ACTOR
// ============================================================

FString FNwiroSettingsTools::SpawnActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ClassName = Cmd->GetStringField(TEXT("class"));
	FString BlueprintPath = Cmd->GetStringField(TEXT("blueprint"));
	// Accept a static-mesh path directly. LLMs also spell this staticMeshPath or mesh.
	FString StaticMeshPath = Cmd->GetStringField(TEXT("staticMesh"));
	if (StaticMeshPath.IsEmpty()) StaticMeshPath = Cmd->GetStringField(TEXT("staticMeshPath"));
	if (StaticMeshPath.IsEmpty()) StaticMeshPath = Cmd->GetStringField(TEXT("mesh"));
	FString Label = Cmd->GetStringField(TEXT("label"));
	if (Label.IsEmpty()) Label = Cmd->GetStringField(TEXT("name"));

	// Support location:{x,y,z}, location:[x,y,z], flat x/y/z.
	double X = 0, Y = 0, Z = 0;
	if (Cmd->HasField(TEXT("location")))
	{
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		if (Cmd->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
		{
			X = (*LocArr)[0]->AsNumber();
			Y = (*LocArr)[1]->AsNumber();
			Z = (*LocArr)[2]->AsNumber();
		}
		else if (Cmd->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
		{
			(*LocObj)->TryGetNumberField(TEXT("x"), X); (*LocObj)->TryGetNumberField(TEXT("X"), X);
			(*LocObj)->TryGetNumberField(TEXT("y"), Y); (*LocObj)->TryGetNumberField(TEXT("Y"), Y);
			(*LocObj)->TryGetNumberField(TEXT("z"), Z); (*LocObj)->TryGetNumberField(TEXT("Z"), Z);
		}
	}
	if (Cmd->HasField(TEXT("x"))) X = Cmd->GetNumberField(TEXT("x"));
	if (Cmd->HasField(TEXT("y"))) Y = Cmd->GetNumberField(TEXT("y"));
	if (Cmd->HasField(TEXT("z"))) Z = Cmd->GetNumberField(TEXT("z"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UClass* ActorClass = nullptr;
	if (!BlueprintPath.IsEmpty())
		ActorClass = LoadClassFromPath(BlueprintPath);
	else if (!ClassName.IsEmpty())
	{
		ActorClass = FindFirstObject<UClass>(*ClassName);
		if (!ActorClass) ActorClass = FindFirstObject<UClass>(*(TEXT("A") + ClassName));
	}
	// An agent asked to place a mesh naturally reaches for spawn_actor and hands
	// it the asset path — as `staticMesh`, or as `class` because no mesh param
	// used to exist. Resolve that to a StaticMeshActor and remember the mesh so
	// it can be assigned after FinishSpawning, instead of failing.
	UStaticMesh* SpawnMesh = nullptr;
	if (!ActorClass)
	{
		const FString MeshPath = !StaticMeshPath.IsEmpty() ? StaticMeshPath : ClassName;
		if (!MeshPath.IsEmpty())
		{
			SpawnMesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
			if (!SpawnMesh) SpawnMesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
			if (SpawnMesh) ActorClass = AStaticMeshActor::StaticClass();
		}
	}

	// FAIL CLOSED. This previously fell back to AActor::StaticClass(), which
	// spawned a component-less, ROOT-less actor: it rendered nothing, and every
	// later SetActorLocation on it silently no-opped because there is no root to
	// move — while spawn_actor, transform_actor and get_actor_property all still
	// reported success:true. An unresolvable class must be an error the caller
	// can see, not an empty actor with a success receipt.
	if (!ActorClass)
	{
		const FString Requested = !BlueprintPath.IsEmpty() ? BlueprintPath
			: (!StaticMeshPath.IsEmpty() ? StaticMeshPath : ClassName);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Could not resolve '%s' to a spawnable Actor class, Blueprint, or StaticMesh asset. Pass class (an Actor class name such as 'StaticMeshActor'), blueprint (a /Game/... BP path), or staticMesh (a /Game/... static mesh path).\"}"),
			*Requested);
	}

	// SAFETY: an agent often calls spawn_actor on a BP it just edited but
	// never recompiled. The GeneratedClass exists but ClassConstructor is
	// null because the skeleton class wasn't built. UWorld::SpawnActor will
	// crash inside StaticConstructObject_Internal asserting
	// `InClass->ClassConstructor` (UObjectGlobals.cpp:3396). Detect that
	// state, try to compile the BP, and only then attempt the spawn. If it
	// still isn't constructible, return a graceful error instead of crashing
	// the editor.
	auto IsClassConstructible = [](UClass* C)
	{
		return C && C->IsValidLowLevel() && C->ClassConstructor != nullptr
			&& C->ClassWithin != nullptr
			&& !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
	};
	if (!IsClassConstructible(ActorClass))
	{
		UBlueprint* OwningBP = nullptr;
		if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(ActorClass))
			OwningBP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		if (!OwningBP && !BlueprintPath.IsEmpty())
		{
			OwningBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
			if (!OwningBP)
			{
				const FString WithExt = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
				OwningBP = LoadObject<UBlueprint>(nullptr, *WithExt);
			}
		}
		if (OwningBP)
		{
			FBlueprintEditorUtils::RefreshAllNodes(OwningBP);
			FKismetEditorUtilities::CompileBlueprint(OwningBP, EBlueprintCompileOptions::SkipGarbageCollection);
			FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
			if (OwningBP->GeneratedClass) ActorClass = OwningBP->GeneratedClass;
		}
	}

	// Only AActor subclasses can be spawned into a world. The agent often hands
	// spawn_actor a UserWidget blueprint (WBP_*) trying to "drop the UI in the
	// level" — but a UserWidget is NOT an Actor, so SpawnActorDeferred<AActor>
	// on its class HARD-CRASHES the editor (Custom Property List Not Initialized
	// / InClass->ClassConstructor assert at UObjectGlobals.cpp:3396). The guards
	// below check constructibility/CDO but none reject a non-Actor class, so a
	// widget class sails straight into the crash. This was the dominant
	// scenario-batch crash cause (7 of 10 overnight). Refuse cleanly instead.
	if (!ActorClass->IsChildOf(AActor::StaticClass()))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Class '%s' is not an Actor and cannot be spawned into the level. UserWidgets/objects are not spawnable — a UMG widget only needs to exist as an asset (then be added to the viewport at runtime via Create Widget + Add to Viewport), not spawned.\"}"), *ActorClass->GetName());

	if (!IsClassConstructible(ActorClass) || !ActorClass->GetDefaultObject(false))
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Class '%s' is not constructible (BP has compile errors or stale CDO). Call bp_get_compile_errors, fix the graph, then retry.\"}"),
			ActorClass ? *ActorClass->GetName() : TEXT("null"));
	}

	// CDO sanity: if root component's owner is not the CDO itself, SpawnActor's
	// internal FinishSpawning will hit `SceneRootComponent->GetOwner() == this`
	// assertion (Actor.cpp:3854) and crash UE.
	if (AActor* CDO = Cast<AActor>(ActorClass->GetDefaultObject(false)))
	{
		if (USceneComponent* CDORoot = CDO->GetRootComponent())
		{
			if (CDORoot->GetOwner() != CDO)
			{
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"Class '%s' has corrupted CDO root component (owner mismatch). Delete and recreate the blueprint, or run a fresh compile.\"}"),
					*ActorClass->GetName());
			}
		}
		for (UActorComponent* C : CDO->GetComponents())
		{
			if (C && C->GetOwner() != CDO)
			{
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"Class '%s' has corrupted CDO component '%s' (owner mismatch). Delete and recreate the blueprint, or run a fresh compile.\"}"),
					*ActorClass->GetName(), *C->GetName());
			}
		}
	}

	// SCS scene-root conflict guard. A parent with a NATIVE root must not also
	// have a standalone scene-component root in the Blueprint's SCS, else
	// FinishSpawning asserts SceneRootComponent->GetOwner()==this
	// (Actor.cpp:3854) and crashes UE. Detect and refuse cleanly.
	{
		USceneComponent* NativeRoot = nullptr;
		if (AActor* CDO = Cast<AActor>(ActorClass->GetDefaultObject(false)))
		{
			if (USceneComponent* Root = CDO->GetRootComponent())
			{
				if (Root->CreationMethod == EComponentCreationMethod::Native)
					NativeRoot = Root;
			}
		}
		// Collapse competing scene-root SCS nodes into one valid hierarchy.
		// Native root present (Character): nest every SCS scene root under it.
		// Plain Actor with 2+ scene roots: keep the first, nest the rest.
		UBlueprint* RepairBP = nullptr;
		bool bRepaired = false;
		USCS_Node* CanonicalRootNode = nullptr;
		for (UClass* C = ActorClass; C; C = C->GetSuperClass())
		{
			UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(C);
			if (!BPGC || !BPGC->SimpleConstructionScript) continue;
			USimpleConstructionScript* SCS = BPGC->SimpleConstructionScript;
			TArray<USCS_Node*> Roots = SCS->GetRootNodes();
			for (USCS_Node* Node : Roots)
			{
				if (!Node) continue;
				const bool bSceneComp = Node->ComponentClass
					&& Node->ComponentClass->IsChildOf(USceneComponent::StaticClass());
				if (!bSceneComp) continue;

				if (NativeRoot)
				{
					Node->SetParent(NativeRoot);
					bRepaired = true;
					if (!RepairBP) RepairBP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
				}
				else if (!CanonicalRootNode)
				{
					CanonicalRootNode = Node;
				}
				else
				{
					Node->SetParent(CanonicalRootNode);
					bRepaired = true;
					if (!RepairBP) RepairBP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
				}
			}
		}
		if (bRepaired && RepairBP)
		{
			FKismetEditorUtilities::CompileBlueprint(RepairBP);
			if (RepairBP->GeneratedClass) ActorClass = RepairBP->GeneratedClass;
			UEditorAssetLibrary::SaveLoadedAsset(RepairBP, false);
		}
	}

	FVector Location(X, Y, Z);
	// >>> NWIRO_PARITY BEGIN: spawn-actor-transaction
	// Every class/CDO check is done, so the transaction cannot be opened on a
	// rejected call. UWorld::SpawnActor dirties only an EXTERNAL package, so on
	// a classic .umap the new actor is invisible to the save-on-close prompt.
	// The editor's own Add Actor wraps the spawn in a transaction and calls
	// MarkPackageDirty afterwards; do the same.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SpawnActor", "AI: Spawn Actor"), /*no object: matches UEditorEngine::AddActor, so a World Partition map package stays clean*/ nullptr);
	// <<< NWIRO_PARITY END: spawn-actor-transaction
	AActor* NewActor = nullptr;
	{
		FTransform SpawnTM(FRotator::ZeroRotator, Location);
		NewActor = World->SpawnActorDeferred<AActor>(ActorClass, SpawnTM, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (NewActor) NewActor->FinishSpawning(SpawnTM);
	}
	if (!NewActor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"SpawnActor returned null for class '%s'\"}"), *ActorClass->GetName());

	// >>> NWIRO_PARITY BEGIN: spawn-actor-dirty
	NewActor->Modify();
	if (!Label.IsEmpty()) NewActor->SetActorLabel(Label);

	// Assign the mesh through the real setter, which rebuilds render state.
	if (SpawnMesh)
	{
		if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(NewActor))
		{
			SMA->GetStaticMeshComponent()->SetStaticMesh(SpawnMesh);
		}
	}

	// GetOutermost() follows the One File Per Actor package, so this dirties the
	// actor's own package on World Partition and the map package otherwise.
	NewActor->MarkPackageDirty();

	// Report what actually exists, not what was asked for: an actor with no root
	// cannot be moved by transform_actor, so say so here rather than letting the
	// caller discover it as a silent no-op several calls later.
	const bool bHasRoot = NewActor->GetRootComponent() != nullptr;
	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"class\":\"%s\",\"mesh\":\"%s\",\"hasRoot\":%s,\"location\":\"(%.0f,%.0f,%.0f)\",\"dirtyAfter\":%s}"),
		*NewActor->GetName(), *NewActor->GetClass()->GetName(),
		SpawnMesh ? *SpawnMesh->GetName() : TEXT(""),
		bHasRoot ? TEXT("true") : TEXT("false"),
		Location.X, Location.Y, Location.Z,
		NewActor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: spawn-actor-dirty
}

// ============================================================
// DELETE ACTOR
// ============================================================

FString FNwiroSettingsTools::DeleteActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("label"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && (Actor->GetName() == ActorName || Actor->GetActorLabel() == ActorName))
		{
			FString Name = Actor->GetName();
			// >>> NWIRO_PARITY BEGIN: delete-actor-transaction
			// The engine already Modify()s the actor and MarkPackageDirty()s it on the editor path
			// (LevelActor.cpp:872, 1010), but UWorld::RemoveActor records the level's actor list ONLY
			// when a transaction is open (World.cpp:2544). Without one the delete never reaches the undo
			// stack and the agent removes an actor the user cannot get back. The editor's own delete is
			// transacted by its command and refuses non-transactional actors (EditorActor.cpp:703).
			UPackage* const Package = Actor->GetOutermost();
			FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "DeleteActor", "AI: Delete Actor"), /*no object: DestroyActor Modify()s the actor itself*/ nullptr);
			Actor->Destroy();
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\",\"dirtyAfter\":%s}"), *Name,
				Package->IsDirty() ? TEXT("true") : TEXT("false"));
			// <<< NWIRO_PARITY END: delete-actor-transaction
		}
	}
	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);
}

// ============================================================
// TRANSFORM ACTOR
// ============================================================

FString FNwiroSettingsTools::TransformActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("label"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{ Actor = *It; break; }
	}
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// An actor with no root component cannot be moved: SetActorLocation/Rotation
	// silently do nothing, so a caller gets success:true and an actor that never
	// moved. Fail loudly instead.
	if (!Actor->GetRootComponent())
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor '%s' (class %s) has no root component, so it cannot be moved, rotated or scaled. It was most likely spawned without a valid class or mesh — respawn it with spawn_actor passing class, blueprint, or staticMesh.\"}"),
			*ActorName, *Actor->GetClass()->GetName());

	// >>> NWIRO_PARITY BEGIN: transform-actor-field-precheck
	// A call with no transform field mutates nothing. Refuse it BEFORE the transaction, because
	// Modify() dirties the package even when nothing changes — and say so, instead of the old
	// success:true with an empty change list.
	static const TCHAR* const TransformFields[] = {
		TEXT("location"), TEXT("x"), TEXT("y"), TEXT("z"),
		TEXT("rotation"), TEXT("pitch"), TEXT("yaw"), TEXT("roll"),
		TEXT("scale"), TEXT("scaleX"), TEXT("scaleY"), TEXT("scaleZ") };
	bool bAnyTransformField = false;
	for (const TCHAR* const Field : TransformFields)
	{
		if (Cmd->HasField(Field)) { bAnyTransformField = true; break; }
	}
	if (!bAnyTransformField)
		return TEXT("{\"success\":false,\"error\":\"No transform field given. Pass location or x/y/z, rotation or pitch/yaw/roll, or scale/scaleX/scaleY/scaleZ.\"}");
	// <<< NWIRO_PARITY END: transform-actor-field-precheck

	// >>> NWIRO_PARITY BEGIN: transform-actor-transaction
	// Validation is complete, so opening the transaction here cannot dirty the
	// package on a rejected call. SetActorLocation/Rotation/Scale3D neither
	// Modify nor dirty on their own, so without this the move is invisible to
	// the save-on-close prompt and Ctrl+Z does nothing. Actor->Modify() also
	// registers the root component, which is where the transform actually lives.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "TransformActor", "AI: Transform Actor"), Actor);
	// <<< NWIRO_PARITY END: transform-actor-transaction

	TArray<FString> Changes;

	// Parse location either as scalar x/y/z OR location:[x,y,z] OR location:{X,Y,Z}.
	{
		FVector Loc = Actor->GetActorLocation();
		bool bSet = false;
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		if (Cmd->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
		{
			Loc = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
			bSet = true;
		}
		else
		{
			const TSharedPtr<FJsonObject>* LocObj = nullptr;
			if (Cmd->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
			{
				double X = Loc.X, Y = Loc.Y, Z = Loc.Z;
				(*LocObj)->TryGetNumberField(TEXT("X"), X); (*LocObj)->TryGetNumberField(TEXT("x"), X);
				(*LocObj)->TryGetNumberField(TEXT("Y"), Y); (*LocObj)->TryGetNumberField(TEXT("y"), Y);
				(*LocObj)->TryGetNumberField(TEXT("Z"), Z); (*LocObj)->TryGetNumberField(TEXT("z"), Z);
				Loc = FVector(X, Y, Z); bSet = true;
			}
		}
		if (Cmd->HasField(TEXT("x"))) { Loc.X = Cmd->GetNumberField(TEXT("x")); bSet = true; }
		if (Cmd->HasField(TEXT("y"))) { Loc.Y = Cmd->GetNumberField(TEXT("y")); bSet = true; }
		if (Cmd->HasField(TEXT("z"))) { Loc.Z = Cmd->GetNumberField(TEXT("z")); bSet = true; }
		if (bSet)
		{
			Actor->SetActorLocation(Loc);
			Changes.Add(FString::Printf(TEXT("Location: (%.0f,%.0f,%.0f)"), Loc.X, Loc.Y, Loc.Z));
		}
	}

	// Rotation: scalar pitch/yaw/roll OR rotation:{Pitch,Yaw,Roll} OR rotation:[p,y,r].
	{
		FRotator Rot = Actor->GetActorRotation();
		bool bSet = false;
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		if (Cmd->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
		{
			Rot = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
			bSet = true;
		}
		else
		{
			const TSharedPtr<FJsonObject>* RotObj = nullptr;
			if (Cmd->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
			{
				double P = Rot.Pitch, Y = Rot.Yaw, R = Rot.Roll;
				(*RotObj)->TryGetNumberField(TEXT("Pitch"), P); (*RotObj)->TryGetNumberField(TEXT("pitch"), P);
				(*RotObj)->TryGetNumberField(TEXT("Yaw"), Y);   (*RotObj)->TryGetNumberField(TEXT("yaw"), Y);
				(*RotObj)->TryGetNumberField(TEXT("Roll"), R);  (*RotObj)->TryGetNumberField(TEXT("roll"), R);
				Rot = FRotator(P, Y, R); bSet = true;
			}
		}
		if (Cmd->HasField(TEXT("pitch"))) { Rot.Pitch = Cmd->GetNumberField(TEXT("pitch")); bSet = true; }
		if (Cmd->HasField(TEXT("yaw")))   { Rot.Yaw = Cmd->GetNumberField(TEXT("yaw")); bSet = true; }
		if (Cmd->HasField(TEXT("roll")))  { Rot.Roll = Cmd->GetNumberField(TEXT("roll")); bSet = true; }
		if (bSet)
		{
			Actor->SetActorRotation(Rot);
			Changes.Add(FString::Printf(TEXT("Rotation: (%.0f,%.0f,%.0f)"), Rot.Pitch, Rot.Yaw, Rot.Roll));
		}
	}

	if (Cmd->HasField(TEXT("scaleX")) || Cmd->HasField(TEXT("scaleY")) || Cmd->HasField(TEXT("scaleZ")) || Cmd->HasField(TEXT("scale")))
	{
		FVector Scale = Actor->GetActorScale3D();
		if (Cmd->HasField(TEXT("scale"))) { double S = Cmd->GetNumberField(TEXT("scale")); Scale = FVector(S, S, S); }
		if (Cmd->HasField(TEXT("scaleX"))) Scale.X = Cmd->GetNumberField(TEXT("scaleX"));
		if (Cmd->HasField(TEXT("scaleY"))) Scale.Y = Cmd->GetNumberField(TEXT("scaleY"));
		if (Cmd->HasField(TEXT("scaleZ"))) Scale.Z = Cmd->GetNumberField(TEXT("scaleZ"));
		Actor->SetActorScale3D(Scale);
		Changes.Add(FString::Printf(TEXT("Scale: (%.1f,%.1f,%.1f)"), Scale.X, Scale.Y, Scale.Z));
	}

	// >>> NWIRO_PARITY BEGIN: transform-actor-dirty
	// GetOutermost() follows the One File Per Actor package, so this dirties the
	// actor's own package on World Partition and the map package otherwise.
	Actor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"changes\":\"%s\",\"dirtyAfter\":%s}"),
		*Actor->GetName(), *FString::Join(Changes, TEXT(", ")),
		Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: transform-actor-dirty
}

// ============================================================
// GET ACTOR PROPERTY
// ============================================================

// Synthetic actor properties — names the LLM often asks for that are not
// FProperty fields, they map to Get*() functions on AActor.
static FString GetSyntheticActorPropertyValue(AActor* Actor, const FString& NameLower)
{
	if (NameLower == TEXT("actorlocation") || NameLower == TEXT("location")
		|| NameLower == TEXT("position") || NameLower == TEXT("worldposition")
		|| NameLower == TEXT("worldlocation"))
	{
		const FVector V = Actor->GetActorLocation();
		return FString::Printf(TEXT("(X=%.3f,Y=%.3f,Z=%.3f)"), V.X, V.Y, V.Z);
	}
	if (NameLower == TEXT("actorrotation") || NameLower == TEXT("rotation")
		|| NameLower == TEXT("worldrotation"))
	{
		const FRotator R = Actor->GetActorRotation();
		return FString::Printf(TEXT("(Pitch=%.3f,Yaw=%.3f,Roll=%.3f)"), R.Pitch, R.Yaw, R.Roll);
	}
	if (NameLower == TEXT("actorscale") || NameLower == TEXT("scale")
		|| NameLower == TEXT("scale3d") || NameLower == TEXT("actorscale3d")
		|| NameLower == TEXT("worldscale"))
	{
		const FVector S = Actor->GetActorScale3D();
		return FString::Printf(TEXT("(X=%.3f,Y=%.3f,Z=%.3f)"), S.X, S.Y, S.Z);
	}
	if (NameLower == TEXT("actorlabel") || NameLower == TEXT("label")
		|| NameLower == TEXT("displayname"))
	{
		return Actor->GetActorLabel();
	}
	if (NameLower == TEXT("classname") || NameLower == TEXT("class"))
	{
		return Actor->GetClass()->GetName();
	}
	return TEXT("");
}

static FProperty* FindPropLoose(UClass* Cls, const FString& In)
{
	if (!Cls || In.IsEmpty()) return nullptr;
	if (FProperty* P = Cls->FindPropertyByName(FName(*In))) return P;
	if (In.StartsWith(TEXT("b"), ESearchCase::CaseSensitive))
	{
		if (FProperty* P = Cls->FindPropertyByName(FName(*In.RightChop(1)))) return P;
	}
	else
	{
		if (FProperty* P = Cls->FindPropertyByName(FName(*(FString(TEXT("b")) + In)))) return P;
	}
	if (In.Contains(TEXT("_")))
	{
		FString Camel; bool bUpper = true;
		for (TCHAR C : In)
		{
			if (C == TEXT('_')) { bUpper = true; continue; }
			Camel += bUpper ? FChar::ToUpper(C) : C; bUpper = false;
		}
		if (FProperty* P = Cls->FindPropertyByName(FName(*Camel))) return P;
	}
	const FString LowerIn = In.ToLower();
	for (TFieldIterator<FProperty> It(Cls); It; ++It)
	{
		if (It->GetName().ToLower() == LowerIn) return *It;
	}
	return nullptr;
}

FString FNwiroSettingsTools::GetActorProperty(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	FString PropName = Cmd->GetStringField(TEXT("property"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{ Actor = *It; break; }
	}
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UObject* TargetObj = nullptr;
	FProperty* Prop = nullptr;

	FString CompPart, PropPart;
	if (PropName.Split(TEXT("."), &CompPart, &PropPart))
	{
		// Dot notation: ComponentName.PropertyName
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (Comp->GetName().Equals(CompPart, ESearchCase::IgnoreCase) ||
				Comp->GetClass()->GetName().Equals(CompPart, ESearchCase::IgnoreCase))
			{
				Prop = FindPropLoose(Comp->GetClass(), PropPart);
				if (Prop) { TargetObj = Comp; break; }
			}
		}
	}
	else
	{
		// 1) Synthetic actor-level properties (no dot, on the actor).
		const FString PropLower = PropName.ToLower();
		const FString SyntheticVal = GetSyntheticActorPropertyValue(Actor, PropLower);
		if (!SyntheticVal.IsEmpty())
		{
			return FString::Printf(TEXT("{\"success\":true,\"property\":\"%s\",\"value\":\"%s\",\"synthetic\":true}"),
				*PropName, *SyntheticVal.Replace(TEXT("\""), TEXT("\\\"")));
		}

		// 2) Reflected actor property with loose lookup.
		Prop = FindPropLoose(Actor->GetClass(), PropName);
		if (Prop) { TargetObj = Actor; }
		else
		{
			// 3) Fall back: search all components for a matching property.
			for (UActorComponent* Comp : Actor->GetComponents())
			{
				Prop = FindPropLoose(Comp->GetClass(), PropName);
				if (Prop) { TargetObj = Comp; break; }
			}
		}
	}

	if (!Prop || !TargetObj)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s (also tried synthetic getters ActorLocation/Rotation/Scale/Label/Class, dot-notation Component.Property, case/snake/b-prefix variants, and every component)\"}"), *PropName);

	FString ValueStr;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObj);
	Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, TargetObj, PPF_None);

	return FString::Printf(TEXT("{\"success\":true,\"property\":\"%s\",\"value\":\"%s\"}"), *Prop->GetName(), *ValueStr);
}

// ============================================================
// SET ACTOR PROPERTY
// ============================================================

// Parse "(X=1,Y=2,Z=3)" or "1 2 3" or "1,2,3" → FVector. Tolerant.
static bool ParseVectorLoose(const FString& In, FVector& Out)
{
	if (Out.InitFromString(In)) return true;
	FString S = In;
	S.ReplaceInline(TEXT("("), TEXT(" "));
	S.ReplaceInline(TEXT(")"), TEXT(" "));
	S.ReplaceInline(TEXT("{"), TEXT(" "));
	S.ReplaceInline(TEXT("}"), TEXT(" "));
	S.ReplaceInline(TEXT(","), TEXT(" "));
	S.ReplaceInline(TEXT("X="), TEXT(""));
	S.ReplaceInline(TEXT("Y="), TEXT(""));
	S.ReplaceInline(TEXT("Z="), TEXT(""));
	TArray<FString> Parts; S.ParseIntoArrayWS(Parts);
	if (Parts.Num() < 3) return false;
	Out.X = FCString::Atof(*Parts[0]);
	Out.Y = FCString::Atof(*Parts[1]);
	Out.Z = FCString::Atof(*Parts[2]);
	return true;
}

static bool TrySetSyntheticActorProperty(AActor* Actor, const FString& NameLower, const FString& Value)
{
	FVector V;
	if (NameLower == TEXT("actorlocation") || NameLower == TEXT("location")
		|| NameLower == TEXT("position") || NameLower == TEXT("worldposition")
		|| NameLower == TEXT("worldlocation"))
	{
		if (!ParseVectorLoose(Value, V)) return false;
		Actor->SetActorLocation(V); return true;
	}
	if (NameLower == TEXT("actorscale") || NameLower == TEXT("scale")
		|| NameLower == TEXT("scale3d") || NameLower == TEXT("actorscale3d")
		|| NameLower == TEXT("worldscale"))
	{
		if (!ParseVectorLoose(Value, V)) return false;
		Actor->SetActorScale3D(V); return true;
	}
	if (NameLower == TEXT("actorrotation") || NameLower == TEXT("rotation")
		|| NameLower == TEXT("worldrotation"))
	{
		FRotator R;
		if (R.InitFromString(Value)) { Actor->SetActorRotation(R); return true; }
		if (ParseVectorLoose(Value, V))
		{
			Actor->SetActorRotation(FRotator(V.Y, V.X, V.Z));
			return true;
		}
		return false;
	}
	if (NameLower == TEXT("actorlabel") || NameLower == TEXT("label")
		|| NameLower == TEXT("displayname"))
	{
		Actor->SetActorLabel(Value); return true;
	}
	return false;
}

FString FNwiroSettingsTools::SetActorProperty(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	FString PropName = Cmd->GetStringField(TEXT("property"));
	FString Value = Cmd->GetStringField(TEXT("value"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{ Actor = *It; break; }
	}
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UObject* TargetObj = nullptr;
	FProperty* Prop = nullptr;

	FString CompPart, PropPart;
	if (PropName.Split(TEXT("."), &CompPart, &PropPart))
	{
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (Comp->GetName().Equals(CompPart, ESearchCase::IgnoreCase) ||
				Comp->GetClass()->GetName().Equals(CompPart, ESearchCase::IgnoreCase))
			{
				Prop = FindPropLoose(Comp->GetClass(), PropPart);
				if (Prop) { TargetObj = Comp; break; }
			}
		}
	}
	else
	{
		// 1) Synthetic actor-level setters first.
		const FString PropLower = PropName.ToLower();
		if (TrySetSyntheticActorProperty(Actor, PropLower, Value))
		{
			Actor->Modify();
			Actor->MarkPackageDirty();
			// TODO: level actor mutation should save the LEVEL via UEditorLevelLibrary::SaveCurrentLevel.
			return FString::Printf(TEXT("{\"success\":true,\"property\":\"%s\",\"value\":\"%s\",\"synthetic\":true}"),
				*PropName, *Value.Replace(TEXT("\""), TEXT("\\\"")));
		}

		Prop = FindPropLoose(Actor->GetClass(), PropName);
		if (Prop) { TargetObj = Actor; }
		else
		{
			for (UActorComponent* Comp : Actor->GetComponents())
			{
				Prop = FindPropLoose(Comp->GetClass(), PropName);
				if (Prop) { TargetObj = Comp; break; }
			}
		}
	}

	if (!Prop || !TargetObj)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s (also tried synthetic setters ActorLocation/Rotation/Scale/Label, dot-notation Component.Property, case/snake/b-prefix variants, and every component)\"}"), *PropName);

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObj);
	if (Prop->ImportText_Direct(*Value, ValuePtr, TargetObj, PPF_None))
	{
		// >>> NWIRO_PARITY BEGIN: set-actor-property-dirty
		// The edited object must Modify() as well as the actor: on the component path the change lives
		// on the component, so without its own Modify() it never enters the transaction and undo
		// restores nothing. Gated because one product shipped without this line. Only these five lines
		// are shared - the lookup that resolved the object above is deliberately product-specific.
		TargetObj->Modify();
		FPropertyChangedEvent PropChangedEvent(Prop);
		TargetObj->PostEditChangeProperty(PropChangedEvent);
		Actor->Modify();
		Actor->MarkPackageDirty();
		// <<< NWIRO_PARITY END: set-actor-property-dirty
		// TODO: level actor mutation should save the LEVEL via UEditorLevelLibrary::SaveCurrentLevel.
		return FString::Printf(TEXT("{\"success\":true,\"property\":\"%s\",\"value\":\"%s\"}"), *PropName, *Value);
	}
	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to set %s\"}"), *PropName);
}

// ============================================================
// EXECUTE PYTHON
// ============================================================

FString FNwiroSettingsTools::ExecutePython(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	// TryGetStringField also coerces numbers/bools in UE; validate the JSON type first.
	FString Mode = TEXT("live");
	if (Cmd->HasField(TEXT("execution_mode"))
		&& (!Cmd->HasTypedField<EJson::String>(TEXT("execution_mode")) || !Cmd->TryGetStringField(TEXT("execution_mode"), Mode)))
		return TEXT("{\"success\":false,\"error\":\"execution_mode must be live or isolated.\"}");
	if (Mode == TEXT("isolated")) return NwiroIsolatedPython::Execute(Cmd);
	if (Mode != TEXT("live"))
		return TEXT("{\"success\":false,\"error\":\"execution_mode must be live or isolated.\"}");
	if (Cmd->HasField(TEXT("action")) || Cmd->HasField(TEXT("job_id")) || Cmd->HasField(TEXT("timeout_seconds")))
		return TEXT("{\"success\":false,\"error\":\"action, job_id and timeout_seconds require execution_mode=isolated.\"}");
	FString Code;
	if (!Cmd->HasTypedField<EJson::String>(TEXT("code")) || !Cmd->TryGetStringField(TEXT("code"), Code))
		return TEXT("{\"success\":false,\"error\":\"code must be a nonempty string.\"}");
	if (Code.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"code is required\"}");
	if (Code.Len() > NwiroIsolatedPython::MaxCodeCharacters)
		return TEXT("{\"success\":false,\"error\":\"code exceeds the 1048576-character limit.\"}");

	// Crash-pattern block: AssetTools.create_asset (via Python) hits a UE
	// engine bug where Factory's NewObject path crashes with null
	// ClassConstructor (UObjectGlobals.cpp:3396). Refuse and redirect to
	// dedicated MCP tools that bypass the Factory.
	if (Code.Contains(TEXT("AssetTools")) || Code.Contains(TEXT("AssetToolsHelpers")) || Code.Contains(TEXT("create_asset")))
	{
		return TEXT("{\"success\":false,\"error\":\"execute_python rejected: AssetTools.create_asset / AssetToolsHelpers crashes UE5 in this project (Factory NewObject null ClassConstructor). Use the dedicated MCP tools instead: create_blueprint (actors / gameplay BPs), create_widget_blueprint (UMG), create_anim_blueprint, create_material. They bypass the buggy Factory path.\"}");
	}
	if (Code.Contains(TEXT("EditorAssetLibrary.save_asset")) || Code.Contains(TEXT("editor_asset_library.save_asset")))
	{
		return TEXT("{\"success\":false,\"error\":\"execute_python rejected: EditorAssetLibrary.save_asset crashes UE on freshly created BPs (null CDO path). Use the dedicated save_asset tool after mutation; persistence is explicit.\"}");
	}
	// Keep the existing Python crash-pattern rejection. Use edit_blueprint
	// with its compile and no-op controls; persistence remains explicit.
	if (Code.Contains(TEXT("compile_blueprint")) || Code.Contains(TEXT("CompileBlueprint")))
	{
		return TEXT("{\"success\":false,\"error\":\"execute_python rejected: BlueprintEditorLibrary.compile_blueprint crashes UE5 in this project (Kismet2.cpp:804 CDO assertion). Use edit_blueprint for graph/component changes. Compilation follows its compile option and mutation/no-op gates; request compile:true explicitly when needed. Use the dedicated save_asset tool separately when persistence is intended.\"}");
	}

	// IPythonScriptPlugin::ExecPythonCommand can ONLY be called from the game
	// thread — calling it from the HTTP worker thread (where MCP dispatches
	// happen) crashes inside CPython with an access violation. Marshal the call
	// onto the game thread and block this worker until it completes.
	// We use ExecPythonCommandEx so the caller gets the captured stdout/stderr
	// (LogOutput) and the evaluated CommandResult back, not just a bool.
	struct FPyState { bool bSuccess = false; bool bAvailable = true; FString Output; FString CmdResult; };

	auto RunPython = [](const FString& InCode) -> FPyState
	{
		FPyState S;
		IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		if (!Python) { S.bAvailable = false; return S; }
		FPythonCommandEx Px;
		Px.Command = InCode;
		Px.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Px.Flags |= EPythonCommandFlags::Unattended;
		S.bSuccess = Python->ExecPythonCommandEx(Px);
		S.CmdResult = Px.CommandResult;
		for (const FPythonLogOutputEntry& E : Px.LogOutput)
		{
			S.Output += E.Output;
		}
		return S;
	};

	FPyState State;
	if (IsInGameThread())
	{
		State = RunPython(Code);
	}
	else
	{
		const double kPythonTimeoutSec = 60.0;
		TSharedRef<TPromise<FPyState>, ESPMode::ThreadSafe> Promise =
			MakeShared<TPromise<FPyState>, ESPMode::ThreadSafe>();
		TFuture<FPyState> Future = Promise->GetFuture();
		const auto Fence = MakeShared<FNwiroDispatchFence, ESPMode::ThreadSafe>();
		FString CodeCopy = Code;
		AsyncTask(ENamedThreads::GameThread, [Promise, Fence, RunPython, CodeCopy = MoveTemp(CodeCopy)]() mutable
		{
			Fence->ExecuteIfQueued([&] { Promise->SetValue(RunPython(CodeCopy)); });
		});
		if (Future.WaitFor(FTimespan::FromSeconds(kPythonTimeoutSec)))
		{
			State = Future.Get();
		}
		else
		{
			const bool bCancelled = Fence->CancelBeforeStart();
			if (bCancelled)
			{
				// A cancelled UE promise still needs a value before its final owner releases it.
				Promise->SetValue(FPyState());
				return FNwiroDispatchFence::TimeoutResult(true, kPythonTimeoutSec);
			}
			if (!Future.IsReady()) return FNwiroDispatchFence::TimeoutResult(false, kPythonTimeoutSec);
			State = Future.Get();
		}
	}

	if (!State.bAvailable)
		return TEXT("{\"success\":false,\"error\":\"PythonScriptPlugin not available\"}");

	TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject());
	Out->SetBoolField(TEXT("success"), State.bSuccess);
	Out->SetStringField(TEXT("output"), State.Output);
	if (!State.CmdResult.IsEmpty())
		Out->SetStringField(TEXT("result"), State.CmdResult);
	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Out, Writer);
	return OutStr;
}

// ============================================================
// DUPLICATE ACTOR
// ============================================================

FString FNwiroSettingsTools::DuplicateActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("label"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* SourceActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && (A->GetName() == ActorName || A->GetActorLabel() == ActorName))
		{
			SourceActor = A;
			break;
		}
	}
	if (!SourceActor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// Duplicate by spawning same class and copying transform
	FVector SpawnLoc = SourceActor->GetActorLocation();
	FRotator SpawnRot = SourceActor->GetActorRotation();
	FActorSpawnParameters SpawnParams;
	SpawnParams.Template = SourceActor;
	// >>> NWIRO_PARITY BEGIN: duplicate-actor-transaction
	// The source actor is resolved, so the transaction cannot be opened on a
	// rejected call. UWorld::SpawnActor dirties only an EXTERNAL package, so on
	// a classic .umap the duplicate is invisible to the save-on-close prompt.
	// The editor's own Add Actor wraps the spawn in a transaction and calls
	// MarkPackageDirty afterwards; do the same.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "DuplicateActor", "AI: Duplicate Actor"), /*no object: matches UEditorEngine::AddActor, so a World Partition map package stays clean*/ nullptr);
	AActor* NewActor = World->SpawnActor<AActor>(SourceActor->GetClass(), SpawnLoc, SpawnRot, SpawnParams);
	if (!NewActor)
		return TEXT("{\"success\":false,\"error\":\"Failed to duplicate actor\"}");

	NewActor->Modify();
	// <<< NWIRO_PARITY END: duplicate-actor-transaction

	// Copy scale from source
	NewActor->SetActorScale3D(SourceActor->GetActorScale3D());

	// Apply offset if specified
	double OffX = Cmd->HasField(TEXT("offsetX")) ? Cmd->GetNumberField(TEXT("offsetX")) : 100.0;
	double OffY = Cmd->HasField(TEXT("offsetY")) ? Cmd->GetNumberField(TEXT("offsetY")) : 0.0;
	double OffZ = Cmd->HasField(TEXT("offsetZ")) ? Cmd->GetNumberField(TEXT("offsetZ")) : 0.0;
	FVector Loc = NewActor->GetActorLocation();
	NewActor->SetActorLocation(Loc + FVector(OffX, OffY, OffZ));

	// Set label if specified
	FString NewLabel = Cmd->GetStringField(TEXT("newName"));
	if (!NewLabel.IsEmpty())
	{
		NewActor->SetActorLabel(NewLabel);
	}

	// >>> NWIRO_PARITY BEGIN: duplicate-actor-dirty
	// GetOutermost() follows the One File Per Actor package, so this dirties the
	// actor's own package on World Partition and the map package otherwise.
	NewActor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"label\":\"%s\",\"dirtyAfter\":%s}"),
		*NewActor->GetName(), *NewActor->GetActorLabel(),
		NewActor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: duplicate-actor-dirty
}

// ============================================================
// RENAME ACTOR
// ============================================================

FString FNwiroSettingsTools::RenameActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("label"));
	FString NewName = Cmd->GetStringField(TEXT("newName"));
	if (NewName.IsEmpty()) NewName = Cmd->GetStringField(TEXT("newLabel"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && (Actor->GetName() == ActorName || Actor->GetActorLabel() == ActorName))
		{
			const FString OldLabel = Actor->GetActorLabel();
			// >>> NWIRO_PARITY BEGIN: rename-actor-transaction
			// SetActorLabel Modify(bMarkDirty=true)s the actor, but only when the trimmed label really
			// differs AND passes ValidateActorName - an invalid name merely logs a warning
			// (ActorEditor.cpp:1239-1268). So the transaction is opened with no object and carries no
			// explicit MarkPackageDirty: the engine dirties exactly when it changed something, and a
			// rejected rename leaves an empty transaction the editor drops. Reading the label back keeps
			// a rejected rename from answering success with a name the actor does not have.
			const FString Wanted = NewName.TrimStartAndEnd();
			if (Wanted.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Missing 'newName' (or 'newLabel')\"}");
			FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "RenameActor", "AI: Rename Actor"), /*no object: SetActorLabel Modify()s only on a real change*/ nullptr);
			Actor->SetActorLabel(Wanted);
			const FString NowLabel = Actor->GetActorLabel();
			if (NowLabel != Wanted)
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"the editor rejected that label\",\"oldLabel\":\"%s\",\"label\":\"%s\"}"),
					*OldLabel, *NowLabel);
			return FString::Printf(TEXT("{\"success\":true,\"oldLabel\":\"%s\",\"newLabel\":\"%s\",\"changed\":%s,\"dirtyAfter\":%s}"),
				*OldLabel, *NowLabel,
				(NowLabel != OldLabel) ? TEXT("true") : TEXT("false"),
				Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
			// <<< NWIRO_PARITY END: rename-actor-transaction
		}
	}
	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);
}

// ============================================================
// ATTACH ACTOR
// ============================================================

FString FNwiroSettingsTools::AttachActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ChildName = Cmd->GetStringField(TEXT("child"));
	FString ParentName = Cmd->GetStringField(TEXT("parent"));
	FString SocketName = Cmd->GetStringField(TEXT("socketName"));
	FString Rule = Cmd->GetStringField(TEXT("rule"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* ChildActor = nullptr;
	AActor* ParentActor = nullptr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A) continue;
		if (!ChildActor && (A->GetName() == ChildName || A->GetActorLabel() == ChildName))
			ChildActor = A;
		if (!ParentActor && (A->GetName() == ParentName || A->GetActorLabel() == ParentName))
			ParentActor = A;
		if (ChildActor && ParentActor) break;
	}

	if (!ChildActor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Child actor not found: %s\"}"), *ChildName);
	if (!ParentActor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Parent actor not found: %s\"}"), *ParentName);

	FAttachmentTransformRules AttachRules = FAttachmentTransformRules::KeepWorldTransform;
	if (Rule.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
		AttachRules = FAttachmentTransformRules::KeepRelativeTransform;
	else if (Rule.Equals(TEXT("SnapToTarget"), ESearchCase::IgnoreCase))
		AttachRules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;

	FName Socket = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);

	// >>> NWIRO_PARITY BEGIN: attach-actor-dirty
	// Both actors are resolved, so the transaction cannot be opened on a
	// rejected call. AttachToActor never calls Modify (DetachFromActor does -
	// that asymmetry is why attachments vanished on a level switch), and the
	// parent's child list changes too, so both sides go into the transaction.
	// This mirrors UEditorEngine::ParentActors.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AttachActor", "AI: Attach Actor"), ChildActor);
	Tx.AlsoModifyNoDirty(ParentActor);   // engine parity: ParentActors Modify(false)s the parent

	ChildActor->AttachToActor(ParentActor, AttachRules, Socket);

	// The attachment is persisted on the child. GetOutermost() follows the One
	// File Per Actor package, so this dirties the child's own package on World
	// Partition and the map package otherwise.
	ChildActor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"child\":\"%s\",\"parent\":\"%s\",\"dirtyAfter\":%s}"),
		*ChildActor->GetActorLabel(), *ParentActor->GetActorLabel(),
		ChildActor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: attach-actor-dirty
}

// ============================================================
// DETACH ACTOR
// ============================================================

FString FNwiroSettingsTools::DetachActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("name"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("label"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("actor"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("child"));
	FString Rule = Cmd->GetStringField(TEXT("rule"));

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && (Actor->GetName() == ActorName || Actor->GetActorLabel() == ActorName))
		{
			FDetachmentTransformRules DetachRules = FDetachmentTransformRules::KeepWorldTransform;
			if (Rule.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
				DetachRules = FDetachmentTransformRules::KeepRelativeTransform;

			// >>> NWIRO_PARITY BEGIN: detach-actor-transaction
			// DetachFromComponent Modify()s the child and Modify(false)s the old parent
			// (SceneComponent.cpp:2378-2382; both rule presets pass bCallModify=true, EngineTypes.cpp:17),
			// so the child's package dirties, the parent's stays clean, and neither side is recorded
			// without an open transaction. An unattached actor detaches nothing, so it must not dirty.
			if (!Actor->GetAttachParentActor())
				return FString::Printf(TEXT("{\"success\":true,\"detached\":\"%s\",\"changed\":false,\"dirtyAfter\":%s}"),
					*Actor->GetActorLabel(), Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
			FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "DetachActor", "AI: Detach Actor"), /*no object: the engine Modify()s both sides with the right dirty flags*/ nullptr);
			Actor->DetachFromActor(DetachRules);
			return FString::Printf(TEXT("{\"success\":true,\"detached\":\"%s\",\"changed\":true,\"dirtyAfter\":%s}"),
				*Actor->GetActorLabel(), Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
			// <<< NWIRO_PARITY END: detach-actor-transaction
		}
	}
	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);
}

// ============================================================
// SELECT ACTOR
// ============================================================

FString FNwiroSettingsTools::SelectActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetEditorWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	// Support single name or array of names
	TArray<FString> Names;
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (Cmd->TryGetArrayField(TEXT("names"), NamesArr))
	{
		for (const auto& V : *NamesArr)
			Names.Add(V->AsString());
	}
	else
	{
		FString Name = Cmd->GetStringField(TEXT("name"));
		if (Name.IsEmpty()) Name = Cmd->GetStringField(TEXT("label"));
		if (!Name.IsEmpty()) Names.Add(Name);
	}

	if (Names.Num() == 0) return TEXT("{\"success\":false,\"error\":\"No actor name specified\"}");

	// Clear current selection
	GEditor->SelectNone(true, true, false);

	int32 Selected = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		for (const FString& N : Names)
		{
			if (Actor->GetName() == N || Actor->GetActorLabel() == N)
			{
				GEditor->SelectActor(Actor, true, true, true);
				Selected++;
				break;
			}
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"selectedCount\":%d}"), Selected);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroDispatchFenceSpec.inl"
#include "Tests/NwiroUnsavedWorkSpec.inl"
#endif
