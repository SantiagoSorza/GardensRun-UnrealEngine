// `context`, `session`, `pause`, `timestep` and `ping` (docs/protocol.md): what is running and the game's controls,
// starting, stopping and restarting play, pausing, a fixed time step, and the clocks.

#include "PlaytestOps.h"
#include "Misc/EngineVersionComparison.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "UObject/UObjectIterator.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "PlaytestJson.h"
#include "PlaytestLevels.h"

namespace PlaytestOps
{
namespace
{
	/** The game's controls: Enhanced Input actions mapped for the player ({"name", "action": asset path, "keys",
	 *  "digital"}), and legacy action and axis mappings from the Input settings ({"name", "action":
	 *  "legacy:<Name>" or "legacy-axis:<Name>", "keys", "digital"}). */
	TArray<TSharedPtr<FJsonValue>> Controls(APlayerController* PC)
	{
		TMap<FString, TSharedPtr<FJsonObject>> ByAction;
		TArray<FString> Order;
		auto Entry = [&ByAction, &Order](const FString& Action, const FString& Name, bool bDigital)
		{
			if (TSharedPtr<FJsonObject>* Found = ByAction.Find(Action))
			{
				return *Found;
			}
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("action"), Action);
			Item->SetBoolField(TEXT("digital"), bDigital);
			Item->SetArrayField(TEXT("keys"), TArray<TSharedPtr<FJsonValue>>());
			ByAction.Add(Action, Item);
			Order.Add(Action);
			return Item;
		};
		auto AddKey = [](const TSharedPtr<FJsonObject>& Item, const FKey& Key)
		{
			TArray<TSharedPtr<FJsonValue>> Keys = Item->GetArrayField(TEXT("keys"));
			const FString Name = Key.GetFName().ToString();
			for (const TSharedPtr<FJsonValue>& K : Keys)
			{
				if (K->AsString() == Name)
				{
					return;
				}
			}
			Keys.Add(MakeShared<FJsonValueString>(Name));
			Item->SetArrayField(TEXT("keys"), Keys);
		};
		const auto AddMapping = [&](const FEnhancedActionKeyMapping& Mapping)
		{
			if (const UInputAction* Action = Mapping.Action)
			{
				AddKey(Entry(PathOf(Action), Action->GetName(), Action->ValueType == EInputActionValueType::Boolean), Mapping.Key);
			}
		};
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		// The player's own mappings, as Enhanced Input built them (with the player's remapped keys)
		if (UEnhancedPlayerInput* Enhanced = PC ? Cast<UEnhancedPlayerInput>(PC->PlayerInput) : nullptr)
		{
			for (const FEnhancedActionKeyMapping& Mapping : Enhanced->GetEnhancedActionMappingsView())
			{
				AddMapping(Mapping);
			}
		}
#else
		// Before 5.8 those aren't public: the mapping contexts applied to the player, as authored
		ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
		if (UEnhancedInputLocalPlayerSubsystem* Input = LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr)
		{
			for (TObjectIterator<UInputMappingContext> It; It; ++It)
			{
				if (Input->HasMappingContext(*It))
				{
					for (const FEnhancedActionKeyMapping& Mapping : It->GetMappings())
					{
						AddMapping(Mapping);
					}
				}
			}
		}
#endif
		for (const FInputActionKeyMapping& Mapping : UInputSettings::GetInputSettings()->GetActionMappings())
		{
			const FString Name = Mapping.ActionName.ToString();
			AddKey(Entry(TEXT("legacy:") + Name, Name, true), Mapping.Key);
		}
		// Axis mappings too: not pressed as actions, but their keys are what moves and looks
		for (const FInputAxisKeyMapping& Mapping : UInputSettings::GetInputSettings()->GetAxisMappings())
		{
			const FString Name = Mapping.AxisName.ToString();
			AddKey(Entry(TEXT("legacy-axis:") + Name, Name, false), Mapping.Key);
		}
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Action : Order)
		{
			Out.Add(MakeShared<FJsonValueObject>(ByAction[Action]));
		}
		return Out;
	}

	/** The server's copy of a client's player controller (the one with the same player id), in the server's world */
	APlayerController* ServerController(UWorld* Server, const APlayerController* PC)
	{
		if (!Server || !PC || !PC->PlayerState)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = Server->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* Other = It->Get();
			if (Other && Other->PlayerState && Other->PlayerState->GetPlayerId() == PC->PlayerState->GetPlayerId())
			{
				return Other;
			}
		}
		return nullptr;
	}

	/** How the game being played is networked (docs/protocol.md, `context`): "standalone", "listen_server" (the player
	 *  hosts it) or "client". A client's says where the server's world is (in this process, the editor's dedicated
	 *  server; "" when it runs elsewhere) and the server's own copies of the player's controller and pawn: what holds
	 *  the truth about health, damage and deaths, and the copies a change sticks on. */
	TSharedRef<FJsonObject> Net(UPlaytestSubsystem& Bridge, UWorld* World, APlayerController* PC)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		const ENetMode Mode = World ? World->GetNetMode() : NM_Standalone;
		Out->SetStringField(TEXT("mode"), Mode == NM_Client ? TEXT("client") : Mode == NM_ListenServer ? TEXT("listen_server")
			: Mode == NM_DedicatedServer ? TEXT("dedicated_server") : TEXT("standalone"));
		if (Mode != NM_Client)
		{
			return Out;
		}
		UWorld* Server = Bridge.GetServerWorld();
		APlayerController* ServerPC = ServerController(Server, PC);
		int32 Clients = 0;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World() && Context.World()->GetNetMode() == NM_Client)
			{
				++Clients;
			}
		}
		Out->SetStringField(TEXT("server_world"), PathOf(Server));
		Out->SetStringField(TEXT("server_controller"), PathOf(ServerPC));
		Out->SetStringField(TEXT("server_character"), PathOf(ServerPC ? ServerPC->GetPawn() : nullptr));
		Out->SetNumberField(TEXT("clients"), Clients);
		return Out;
	}
}

	TSharedRef<FJsonObject> InputInfo()
	{
		// How the player's inputs reach the game (as Unity's context `input`): keys and buttons through the player
		// controller, Enhanced Input actions by path, the legacy mappings as legacy:<Name>; the pointer; `move` adds
		// movement input to the pawn (its controller); no notes (Unity's say how its input handling is set up)
		TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("handling"), TEXT("enhanced_input"));
		Input->SetBoolField(TEXT("keys"), true);
		Input->SetBoolField(TEXT("actions"), true);
		Input->SetBoolField(TEXT("pointer"), true);
		Input->SetStringField(TEXT("move"), TEXT("controller"));
		Input->SetArrayField(TEXT("notes"), TArray<TSharedPtr<FJsonValue>>());
		return Input;
	}

	void Context(UPlaytestSubsystem& Bridge, int64 Id)
	{
		UWorld* World = Bridge.GetGameWorld();
		APlayerController* PC = PlayerController(World);
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("world"), PathOf(World));
		Result->SetStringField(TEXT("character"), PathOf(Pawn));
		Result->SetStringField(TEXT("controller"), PathOf(PC));
		Result->SetStringField(TEXT("input_subsystem"), PathOf(InputSubsystem(World)));
		Result->SetStringField(TEXT("camera_manager"), PC ? PathOf(PC->PlayerCameraManager) : FString());
		Result->SetStringField(TEXT("game_name"), FApp::GetProjectName());
		Result->SetStringField(TEXT("host"), GIsEditor ? TEXT("editor") : TEXT("game"));
		Result->SetBoolField(TEXT("session_running"), World != nullptr);
		Result->SetBoolField(TEXT("paused"), World && World->IsPaused());
		Result->SetField(TEXT("game_time"), PlaytestJson::Number(World ? World->GetTimeSeconds() : 0.0));
		Result->SetArrayField(TEXT("controls"), Controls(PC));
		Result->SetObjectField(TEXT("net"), Net(Bridge, World, PC));
		Result->SetObjectField(TEXT("input"), InputInfo());
		if (!World)
		{
			const FString Problem = Bridge.GetPlayProblem();
			if (!Problem.IsEmpty())
			{
				Result->SetStringField(TEXT("problem"), Problem);
			}
		}
		Bridge.Respond(Id, Obj(Result));
	}

namespace
{
	/** {"running", "scene": the level playing (its package name), as Unity's and Godot's answers}. */
	TSharedPtr<FJsonValue> Running(bool bRunning, const UWorld* World)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("running"), bRunning);
		Result->SetStringField(TEXT("scene"), bRunning && World ? PlaytestLevels::PackageOf(World) : FString());
		return Obj(Result);
	}

	/** Before the level is reopened: take off the viewport what the game put there, so the reopened level starts with
	 *  the screen a game started afresh has. A UMG widget leaves with its world, but one the game added to the viewport
	 *  as plain Slate stays across a level change and would go on showing over the reopened level (a death message from
	 *  before the restart). The game's software cursors aren't on-screen content: they are put back. */
	void RemoveGameWidgets(UWorld* World)
	{
		UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
		if (!Viewport)
		{
			return;
		}
		TArray<TPair<EMouseCursor::Type, TSharedPtr<SWidget>>> Cursors;
		for (int32 C = 0; C < static_cast<int32>(EMouseCursor::TotalCursorCount); ++C)
		{
			const EMouseCursor::Type Type = static_cast<EMouseCursor::Type>(C);
			if (TSharedPtr<SWidget> Cursor = Viewport->GetSoftwareCursorWidget(Type))
			{
				Cursors.Emplace(Type, Cursor);
			}
		}
		Viewport->RemoveAllViewportWidgets();
		for (const TPair<EMouseCursor::Type, TSharedPtr<SWidget>>& Cursor : Cursors)
		{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
			Viewport->AddSoftwareCursorFromSlateWidget(Cursor.Key, Cursor.Value);
#else
			Viewport->SetSoftwareCursorWidget(Cursor.Key, Cursor.Value);
#endif
		}
	}
}

	void Levels(UPlaytestSubsystem& Bridge, int64 Id)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Listed;
		for (const FString& Level : PlaytestLevels::List())
		{
			Listed.Add(MakeShared<FJsonValueString>(Level));
		}
		Out->SetArrayField(TEXT("levels"), Listed);
		// (in a game, a start plays the level it is in: the open one)
		const FString Playing = PlaytestLevels::PackageOf(Bridge.GetGameWorld());
		const FString Open = PlaytestLevels::EditorLevel();
		Out->SetStringField(TEXT("open"), Open.IsEmpty() ? Playing : Open);
		Out->SetStringField(TEXT("playing"), Playing);
		Bridge.Respond(Id, Obj(Out));
	}

	void Session(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		const FString Action = StringField(Args, TEXT("action"));
		UWorld* World = Bridge.GetGameWorld();
		if (Action == TEXT("status") || Action.IsEmpty())
		{
			// loading: the play session is still starting (asked for, or its world not playing yet): the agent service
			// waits it out before its time for reading the game counts
			TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetBoolField(TEXT("running"), World != nullptr);
			Status->SetBoolField(TEXT("loading"), (World && !World->HasBegunPlay())
				|| (Bridge.SessionRequested && Bridge.SessionRequested()));
			Bridge.Respond(Id, Obj(Status));
			return;
		}
		// A level that can't play (none or several by that name): 404 before anything changes, as Unity's and Godot's
		FString Map = StringField(Args, TEXT("map"));
		FString Package, Error;
		if (!Map.IsEmpty() && (Action == TEXT("start") || Action == TEXT("restart")) && !PlaytestLevels::Resolve(Map, Package, Error))
		{
			Bridge.RespondError(Id, Error, 404);
			return;
		}
		if (Bridge.SessionHandler)
		{
			TWeakObjectPtr<UPlaytestSubsystem> Weak(&Bridge);
			Bridge.SessionHandler(Action, Map, [Weak, Id](bool bRunning, const FString& Error)
			{
				if (UPlaytestSubsystem* B = Weak.Get())
				{
					if (Error.IsEmpty())
					{
						B->Respond(Id, Running(bRunning, B->GetGameWorld()));
					}
					else
					{
						B->RespondError(Id, Error, 500);
					}
				}
			});
			return;
		}
		// A game build: there is always a session; restarting reopens the map the game was launched with (or the one
		// asked for: "map"), never the one it travelled to since (a match that ended moves the server on to the next
		// map), with what the game put on the viewport taken off first (see RemoveGameWidgets). A start asking for
		// another level than the one playing goes there, as a restart in it
		const bool bSwitch = Action == TEXT("start") && World && !Package.IsEmpty() && Package != PlaytestLevels::PackageOf(World);
		if ((Action != TEXT("restart") && !bSwitch) || !World)
		{
			Bridge.Respond(Id, Running(World != nullptr, World));
			return;
		}
		if (!Package.IsEmpty())
		{
			Map = Package;
		}
		if (Map.IsEmpty())
		{
			Map = Bridge.GetLaunchMap();
		}
		if (Map.IsEmpty())
		{
			Map = UGameplayStatics::GetCurrentLevelName(World, true);  // (no world seen to begin play yet)
		}
		TWeakObjectPtr<UWorld> Old(World);
		RemoveGameWidgets(World);
		UGameplayStatics::OpenLevel(World, FName(*Map));
		FPending Pending;
		Pending.Id = Id;
		Pending.Op = TEXT("session");
		Pending.StartReal = FPlatformTime::Seconds();
		Pending.Tick = [&Bridge, Old](FPending& P, double Now)
		{
			UWorld* Current = Bridge.GetGameWorld();
			if (Current && Current != Old.Get() && Current->HasBegunPlay())
			{
				Bridge.Respond(P.Id, Running(true, Current));
				return true;
			}
			if (Now - P.StartReal > 30.0)
			{
				Bridge.RespondError(P.Id, TEXT("Timed out reopening the map"), 500);
				return true;
			}
			return false;
		};
		Bridge.AddPending(MoveTemp(Pending));
	}

	void Pause(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = Bridge.GetGameWorld();
		if (!World)
		{
			Bridge.RespondError(Id, TEXT("No game is running"));
			return;
		}
		bool bPaused = false;
		Args->TryGetBoolField(TEXT("paused"), bPaused);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (World->GetNetMode() != NM_Client)
		{
			UGameplayStatics::SetGamePaused(World, bPaused);
			Result->SetBoolField(TEXT("paused"), World->IsPaused());
			Bridge.Respond(Id, Obj(Result));
			return;
		}
		// A client can't pause the game: its server does, for every player, as the player's own controller there
		// would (it also sends the pause to the clients at once: a paused server's clock no longer brings their update)
		FString Why;
		UWorld* Server = Bridge.GetServerWorld();
		APlayerController* Pauser = ServerController(Server, PlayerController(World));
		if (!Server)
		{
			Why = TEXT("the game's server runs in another process: it can't be paused from here");
		}
		else if (!Pauser)
		{
			Why = TEXT("the player hasn't joined the server yet");
		}
		else if (Server->IsPaused() != bPaused && !Pauser->SetPause(bPaused))
		{
			Why = bPaused ? TEXT("the game doesn't allow pausing (its game mode refused)") : TEXT("the game wouldn't unpause");
		}
		const bool bNow = Server ? Server->IsPaused() : World->IsPaused();
		Result->SetBoolField(TEXT("paused"), bNow);
		if (!Why.IsEmpty() && bNow != bPaused)
		{
			Result->SetStringField(TEXT("why"), Why);
		}
		Bridge.Respond(Id, Obj(Result));
	}

	void Timestep(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		bool bFixed = false;
		Args->TryGetBoolField(TEXT("fixed"), bFixed);
		const double Fps = FMath::Max(1.0, NumberField(Args, TEXT("fps"), 30.0));
		FApp::SetUseFixedTimeStep(bFixed);
		if (bFixed)
		{
			FApp::SetFixedDeltaTime(1.0 / Fps);
		}
		Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
	}

	void Ping(UPlaytestSubsystem& Bridge, int64 Id)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetField(TEXT("game_time"), PlaytestJson::Number(FMath::Max(0.0, Bridge.GameTime())));
		Result->SetField(TEXT("wall_time"), PlaytestJson::Number((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalSeconds()));
		Bridge.Respond(Id, Obj(Result));
	}
}
