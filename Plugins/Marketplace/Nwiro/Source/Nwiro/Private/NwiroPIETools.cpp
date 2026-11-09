// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroPIETools.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "AIController.h"
#include "BrainComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "Navigation/PathFollowingComponent.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroPIE, Log, All);

// ============================================================
// HELPERS
// ============================================================

static UWorld* GetPIEWorld()
{
	if (!GEditor) return nullptr;
	return GEditor->PlayWorld;
}

static AActor* FindPIEActor(UWorld* World, const FString& Name)
{
	// Empty name would Contains-match the first actor — refuse it.
	if (!World || Name.IsEmpty()) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && (A->GetName() == Name || A->GetActorLabel() == Name)) return A;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && (A->GetName().Contains(Name, ESearchCase::IgnoreCase) ||
			A->GetActorLabel().Contains(Name, ESearchCase::IgnoreCase)))
		{
			return A;
		}
	}
	return nullptr;
}

static AAIController* FindAIController(UWorld* World, const FString& ActorName)
{
	AActor* Actor = FindPIEActor(World, ActorName);
	if (!Actor) return nullptr;

	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		if (AAIController* Existing = Cast<AAIController>(Pawn->GetController()))
		{
			return Existing;
		}
		// AutoPossessAI defaults to NotSpawned on C++ Character — spawn the
		// pawn's default controller so the LLM's "set blackboard on this
		// pawn" request can succeed.
		Pawn->SpawnDefaultController();
		if (AAIController* Spawned = Cast<AAIController>(Pawn->GetController()))
		{
			return Spawned;
		}
		return nullptr;
	}

	if (AAIController* AIC = Cast<AAIController>(Actor))
	{
		return AIC;
	}

	return nullptr;
}

// Serialise with the condensed writer: dynamic strings are escaped, so a name cannot break the JSON or
// inject fields, and "success":false keeps the no-space spelling that substring failure checks look for.
static FString ToCondensedJson(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

// FindAIController() returns nullptr for the player's pawn: it already has a PlayerController, so
// SpawnDefaultController() does nothing. Say so instead of the generic "not found", so the caller can
// switch tools (PlayerHint names the alternative where one exists). IsPlayerControlled() also covers
// remote players' pawns in a client PIE world, whose PlayerController is not local; the cast covers a
// possessed pawn without a PlayerState.
static FString AIControllerNotFoundError(UWorld* World, const FString& ActorName, const TCHAR* ToolName, const TCHAR* PlayerHint = TEXT(""))
{
	AActor* Actor = FindPIEActor(World, ActorName);
	const APawn* Pawn = Cast<APawn>(Actor);
	const bool bPlayerControlled = Pawn
		? Pawn->IsPlayerControlled() || Cast<APlayerController>(Pawn->GetController()) != nullptr
		: Cast<APlayerController>(Actor) != nullptr;

	TSharedRef<FJsonObject> Reply = MakeShared<FJsonObject>();
	Reply->SetBoolField(TEXT("success"), false);
	Reply->SetStringField(TEXT("error"), bPlayerControlled
		? FString::Printf(TEXT("'%s' is %s; %s only works on AI-controlled pawns.%s"),
			*Actor->GetName(), Pawn ? TEXT("player-controlled") : TEXT("a player controller"), ToolName, PlayerHint)
		: FString::Printf(TEXT("AI Controller not found for: %s"), *ActorName));
	return ToCondensedJson(Reply);
}

#define PIE_CHECK() \
	UWorld* PIEWorld = GetPIEWorld(); \
	if (!PIEWorld) return TEXT("{\"success\":false,\"error\":\"PIE not running. Use play_in_editor first.\"}");

// ============================================================
// PIE TELEPORT ACTOR
// ============================================================

FString FNwiroPIETools::PIETeleportActor(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	double X = Cmd->GetNumberField(TEXT("x"));
	double Y = Cmd->GetNumberField(TEXT("y"));
	double Z = Cmd->GetNumberField(TEXT("z"));

	AActor* Actor = FindPIEActor(PIEWorld, ActorName);
	if (!Actor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PIE actor not found: %s\"}"), *ActorName);

	const FVector NewLoc(X, Y, Z);

	// 3-tier fallback chain (mirrors IK plugin).
	bool bSuccess = Actor->TeleportTo(NewLoc, Actor->GetActorRotation());
	const TCHAR* Method = TEXT("TeleportTo");
	if (!bSuccess)
	{
		bSuccess = Actor->SetActorLocation(NewLoc, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		Method = TEXT("SetActorLocation(noSweep)");
	}
	if (!bSuccess)
	{
		if (USceneComponent* Root = Actor->GetRootComponent())
		{
			const EComponentMobility::Type OldMobility = Root->Mobility;
			if (OldMobility != EComponentMobility::Movable)
			{
				Root->SetMobility(EComponentMobility::Movable);
			}
			Root->SetWorldLocation(NewLoc, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			bSuccess = Root->GetComponentLocation().Equals(NewLoc, 0.01f);
			Method = OldMobility == EComponentMobility::Movable
				? TEXT("RootComponent.SetWorldLocation")
				: TEXT("RootComponent.SetWorldLocation(+Mobility=Movable)");
		}
	}

	return FString::Printf(TEXT("{\"success\":%s,\"actor\":\"%s\",\"position\":\"(%.0f, %.0f, %.0f)\",\"method\":\"%s\"}"),
		bSuccess ? TEXT("true") : TEXT("false"), *Actor->GetName(), X, Y, Z, Method);
}

// ============================================================
// PIE SPAWN ACTOR
// ============================================================

FString FNwiroPIETools::PIESpawnActor(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ClassName = Cmd->GetStringField(TEXT("class"));
	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	if (Cmd->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
	{
		X = (*LocArr)[0]->AsNumber();
		Y = (*LocArr)[1]->AsNumber();
		Z = (*LocArr)[2]->AsNumber();
	}
	FString DesiredLabel = Cmd->GetStringField(TEXT("name"));
	if (DesiredLabel.IsEmpty()) DesiredLabel = Cmd->GetStringField(TEXT("label"));

	UClass* ActorClass = FindObject<UClass>(nullptr, *ClassName);
	if (!ActorClass)
		ActorClass = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ClassName));
	if (!ActorClass)
		ActorClass = AActor::StaticClass();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	FVector SpawnLoc(X, Y, Z);
	AActor* NewActor = PIEWorld->SpawnActor(ActorClass, &SpawnLoc, &FRotator::ZeroRotator, Params);

	if (!NewActor)
		return TEXT("{\"success\":false,\"error\":\"Failed to spawn in PIE\"}");

	if (!DesiredLabel.IsEmpty()) NewActor->SetActorLabel(DesiredLabel);

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"label\":\"%s\",\"class\":\"%s\"}"),
		*NewActor->GetName(), *NewActor->GetActorLabel(), *ActorClass->GetName());
}

// ============================================================
// PIE DESTROY ACTOR
// ============================================================

FString FNwiroPIETools::PIEDestroyActor(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("name"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("label"));
	AActor* Actor = FindPIEActor(PIEWorld, ActorName);
	if (!Actor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PIE actor not found: %s\"}"), *ActorName);

	FString Name = Actor->GetName();
	Actor->Destroy();
	return FString::Printf(TEXT("{\"success\":true,\"destroyed\":\"%s\"}"), *Name);
}

// ============================================================
// PIE GET PROPERTY
// ============================================================

FString FNwiroPIETools::PIEGetProperty(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString PropName = Cmd->GetStringField(TEXT("property"));

	AActor* Actor = FindPIEActor(PIEWorld, ActorName);
	if (!Actor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PIE actor not found: %s\"}"), *ActorName);

	FProperty* Prop = Actor->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s\"}"), *PropName);

	FString Value;
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);
	Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, Actor, PPF_None);

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"property\":\"%s\",\"value\":\"%s\"}"),
		*Actor->GetName(), *PropName, *Value);
}

// ============================================================
// PIE SET PROPERTY
// ============================================================

FString FNwiroPIETools::PIESetProperty(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString PropName = Cmd->GetStringField(TEXT("property"));
	FString Value = Cmd->GetStringField(TEXT("value"));

	AActor* Actor = FindPIEActor(PIEWorld, ActorName);
	if (!Actor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PIE actor not found: %s\"}"), *ActorName);

	FProperty* Prop = Actor->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s\"}"), *PropName);

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);
	if (Prop->ImportText_Direct(*Value, ValuePtr, Actor, PPF_None))
		return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"property\":\"%s\",\"value\":\"%s\"}"), *Actor->GetName(), *PropName, *Value);

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to set %s\"}"), *PropName);
}

// ============================================================
// PIE SET BLACKBOARD KEY
// ============================================================

FString FNwiroPIETools::PIESetBlackboardKey(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString KeyName = Cmd->GetStringField(TEXT("key"));
	FString KeyType = Cmd->GetStringField(TEXT("type")).ToLower();

	AAIController* AIC = FindAIController(PIEWorld, ActorName);
	if (!AIC)
		return AIControllerNotFoundError(PIEWorld, ActorName, TEXT("pie_set_blackboard_key"));

	UBlackboardComponent* BB = AIC->GetBlackboardComponent();
	if (!BB)
		return TEXT("{\"success\":false,\"error\":\"No Blackboard component on AI Controller\"}");

	FName Key(*KeyName);

	if (KeyType == TEXT("bool"))
	{
		BB->SetValueAsBool(Key, Cmd->GetBoolField(TEXT("value")));
	}
	else if (KeyType == TEXT("int"))
	{
		BB->SetValueAsInt(Key, (int32)Cmd->GetNumberField(TEXT("value")));
	}
	else if (KeyType == TEXT("float"))
	{
		BB->SetValueAsFloat(Key, (float)Cmd->GetNumberField(TEXT("value")));
	}
	else if (KeyType == TEXT("string"))
	{
		BB->SetValueAsString(Key, Cmd->GetStringField(TEXT("value")));
	}
	else if (KeyType == TEXT("vector"))
	{
		FVector Vec(
			Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0,
			Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0,
			Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0
		);
		BB->SetValueAsVector(Key, Vec);
	}
	else if (KeyType == TEXT("object"))
	{
		FString ObjName = Cmd->GetStringField(TEXT("value"));
		UObject* Obj = FindPIEActor(PIEWorld, ObjName);
		BB->SetValueAsObject(Key, Obj);
	}
	else
	{
		// Auto-detect: try bool, then float, then string
		if (Cmd->HasField(TEXT("value")))
		{
			double NumVal;
			if (Cmd->TryGetNumberField(TEXT("value"), NumVal))
				BB->SetValueAsFloat(Key, (float)NumVal);
			else
			{
				bool BoolVal;
				if (Cmd->TryGetBoolField(TEXT("value"), BoolVal))
					BB->SetValueAsBool(Key, BoolVal);
				else
					BB->SetValueAsString(Key, Cmd->GetStringField(TEXT("value")));
			}
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"key\":\"%s\"}"), *ActorName, *KeyName);
}

// ============================================================
// PIE GET BLACKBOARD KEY
// ============================================================

FString FNwiroPIETools::PIEGetBlackboardKey(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString KeyName = Cmd->GetStringField(TEXT("key"));

	AAIController* AIC = FindAIController(PIEWorld, ActorName);
	if (!AIC)
		return AIControllerNotFoundError(PIEWorld, ActorName, TEXT("pie_get_blackboard_key"));

	UBlackboardComponent* BB = AIC->GetBlackboardComponent();
	if (!BB)
		return TEXT("{\"success\":false,\"error\":\"No Blackboard component\"}");

	FName Key(*KeyName);

	// Try to read and return as string
	FString Value;

	// Check key type from blackboard data
	const FBlackboard::FKey KeyID = BB->GetKeyID(Key);
	if (KeyID == FBlackboard::InvalidKey)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blackboard key not found: %s\"}"), *KeyName);

	const FBlackboardEntry* Entry = BB->GetBlackboardAsset() ? BB->GetBlackboardAsset()->GetKey(KeyID) : nullptr;
	if (Entry && Entry->KeyType)
	{
		FString TypeName = Entry->KeyType->GetClass()->GetName();

		if (TypeName.Contains(TEXT("Bool")))
			Value = BB->GetValueAsBool(Key) ? TEXT("true") : TEXT("false");
		else if (TypeName.Contains(TEXT("Float")))
			Value = FString::SanitizeFloat(BB->GetValueAsFloat(Key));
		else if (TypeName.Contains(TEXT("Int")))
			Value = FString::FromInt(BB->GetValueAsInt(Key));
		else if (TypeName.Contains(TEXT("String")))
			Value = BB->GetValueAsString(Key);
		else if (TypeName.Contains(TEXT("Vector")))
			Value = BB->GetValueAsVector(Key).ToString();
		else if (TypeName.Contains(TEXT("Object")))
		{
			UObject* Obj = BB->GetValueAsObject(Key);
			Value = Obj ? Obj->GetName() : TEXT("None");
		}
		else
			Value = TEXT("(unknown type)");
	}

	return FString::Printf(TEXT("{\"success\":true,\"key\":\"%s\",\"value\":\"%s\"}"), *KeyName, *Value);
}

// ============================================================
// PIE MOVE AI TO
// ============================================================

FString FNwiroPIETools::PIEMoveAITo(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	double X = Cmd->GetNumberField(TEXT("x"));
	double Y = Cmd->GetNumberField(TEXT("y"));
	double Z = Cmd->GetNumberField(TEXT("z"));
	float AcceptanceRadius = Cmd->HasField(TEXT("acceptanceRadius")) ? (float)Cmd->GetNumberField(TEXT("acceptanceRadius")) : 50.0f;

	AAIController* AIC = FindAIController(PIEWorld, ActorName);
	if (!AIC)
		return AIControllerNotFoundError(PIEWorld, ActorName, TEXT("pie_move_ai_to"), TEXT(" Use pie_teleport_actor on the player's pawn to reposition it."));

	EPathFollowingRequestResult::Type Result = AIC->MoveToLocation(FVector(X, Y, Z), AcceptanceRadius);

	FString ResultStr;
	switch (Result)
	{
	case EPathFollowingRequestResult::RequestSuccessful: ResultStr = TEXT("Moving"); break;
	case EPathFollowingRequestResult::AlreadyAtGoal: ResultStr = TEXT("AlreadyAtGoal"); break;
	default:
	{
		// A refused move request must not read as success: the agent would believe the pawn is walking.
		TSharedRef<FJsonObject> Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("success"), false);
		Reply->SetStringField(TEXT("error"), TEXT("The AIController rejected the move request. Possible causes: no navigation data (NavMesh) for this pawn, no usable path to the target, an invalid target, or a pawn without a movement component. Check get_navigation_info, or run build_navigation."));
		Reply->SetStringField(TEXT("actor"), ActorName);
		Reply->SetStringField(TEXT("result"), TEXT("Failed"));
		Reply->SetStringField(TEXT("target"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), X, Y, Z));
		return ToCondensedJson(Reply);
	}
	}

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"result\":\"%s\",\"target\":\"(%.0f, %.0f, %.0f)\"}"),
		*ActorName, *ResultStr, X, Y, Z);
}

// ============================================================
// PIE STOP AI
// ============================================================

FString FNwiroPIETools::PIEStopAI(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));

	AAIController* AIC = FindAIController(PIEWorld, ActorName);
	if (!AIC)
		return AIControllerNotFoundError(PIEWorld, ActorName, TEXT("pie_stop_ai"));

	AIC->StopMovement();

	// Also stop behavior tree if running
	UBrainComponent* Brain = AIC->GetBrainComponent();
	if (Brain)
	{
		Brain->StopLogic(TEXT("MCP PIE Stop"));
	}

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"message\":\"AI stopped\"}"), *ActorName);
}

// ============================================================
// PIE GET GAME STATE
// ============================================================

FString FNwiroPIETools::PIEGetGameState(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("timeSeconds"), PIEWorld->GetTimeSeconds());
	Result->SetBoolField(TEXT("isPaused"), PIEWorld->IsPaused());

	// Player info
	APlayerController* PC = PIEWorld->GetFirstPlayerController();
	if (PC)
	{
		APawn* Pawn = PC->GetPawn();
		if (Pawn)
		{
			FVector Loc = Pawn->GetActorLocation();
			FRotator Rot = Pawn->GetActorRotation();
			Result->SetStringField(TEXT("playerPosition"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Loc.X, Loc.Y, Loc.Z));
			Result->SetStringField(TEXT("playerRotation"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Rot.Pitch, Rot.Yaw, Rot.Roll));
			Result->SetStringField(TEXT("playerClass"), Pawn->GetClass()->GetName());

			// Health if Character
			if (ACharacter* Char = Cast<ACharacter>(Pawn))
			{
				Result->SetStringField(TEXT("playerName"), Char->GetName());
			}
		}
	}

	// Actor count
	int32 ActorCount = 0;
	int32 PawnCount = 0;
	for (TActorIterator<AActor> It(PIEWorld); It; ++It)
	{
		ActorCount++;
		if (Cast<APawn>(*It)) PawnCount++;
	}
	Result->SetNumberField(TEXT("actorCount"), ActorCount);
	Result->SetNumberField(TEXT("pawnCount"), PawnCount);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// PIE LIST ACTORS
// ============================================================

FString FNwiroPIETools::PIEListActors(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(R, Cmd);

	FString ClassFilter = Cmd.IsValid() ? Cmd->GetStringField(TEXT("classFilter")) : TEXT("");
	int32 Limit = Cmd.IsValid() && Cmd->HasField(TEXT("limit")) ? (int32)Cmd->GetNumberField(TEXT("limit")) : 100;

	TArray<TSharedPtr<FJsonValue>> Actors;
	for (TActorIterator<AActor> It(PIEWorld); It; ++It)
	{
		if (Actors.Num() >= Limit) break;

		AActor* A = *It;
		if (!ClassFilter.IsEmpty() && !A->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			continue;

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), A->GetName());
		Obj->SetStringField(TEXT("label"), A->GetActorLabel());
		Obj->SetStringField(TEXT("class"), A->GetClass()->GetName());
		FVector Loc = A->GetActorLocation();
		Obj->SetNumberField(TEXT("x"), Loc.X);
		Obj->SetNumberField(TEXT("y"), Loc.Y);
		Obj->SetNumberField(TEXT("z"), Loc.Z);
		Actors.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("actors"), Actors);
	Result->SetNumberField(TEXT("count"), Actors.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// PIE CONSOLE COMMAND
// ============================================================

FString FNwiroPIETools::PIEConsoleCommand(const FString& JsonCommand)
{
	PIE_CHECK();

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Command = Cmd->GetStringField(TEXT("command"));
	if (Command.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'command'\"}");

	// Block dangerous commands
	FString CmdLower = Command.ToLower();
	if (CmdLower.Contains(TEXT("exit")) || CmdLower.Contains(TEXT("quit")))
		return TEXT("{\"success\":false,\"error\":\"Command blocked for safety\"}");

	GEngine->Exec(PIEWorld, *Command);
	return FString::Printf(TEXT("{\"success\":true,\"command\":\"%s\"}"), *Command);
}
