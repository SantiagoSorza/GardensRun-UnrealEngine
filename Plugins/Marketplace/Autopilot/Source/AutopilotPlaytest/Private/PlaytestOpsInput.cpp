// `input`, `move` and `key` (docs/protocol.md): the player's hands. Input actions fired by name (Enhanced Input, or
// a legacy action mapping), movement input added to the pawn, and keys, buttons, mouse movement and sticks sent as
// the devices would, held between frames until released or their lease runs out (TickKeys).

#include "PlaytestOps.h"

#include "EnhancedInputSubsystems.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputAction.h"
#include "Misc/App.h"
#include "Misc/EngineVersionComparison.h"
#include "PlaytestJson.h"
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
#include "InputKeyEventArgs.h"
#endif

namespace PlaytestOps
{
namespace
{
	// A tap (a quick press) stays down this long and across this many engine frames: input read once a frame
	// (Enhanced Input's triggers) misses a press and release that land in the same frame, as a slow frame allows
	constexpr double TapSeconds = 0.05;
	constexpr uint64 TapFrames = 2;

	bool TapDone(const FPending& P, double Now, uint64 StartFrame)
	{
		return Now - P.StartReal >= P.Seconds && GFrameCounter - StartFrame >= TapFrames;
	}

#if UE_VERSION_OLDER_THAN(5, 3, 0)
	// Before 5.3 Enhanced Input has no continuous injection: an action held is injected again every frame (TickKeys)
	// until released, the session ends or the brain goes
	struct FHeldAction
	{
		TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem;
		TWeakObjectPtr<UInputAction> Action;
		FInputActionValue Value;
	};
	TArray<FHeldAction> HeldActions;
#endif

	// A key event into the player controller, built as the FInputKeyParams overload of InputKey (deprecated in 5.6)
	// built it: no viewport, input device or timestamp. Amount is how far the key is down (an axis: its value)
	void SendInput(APlayerController* PC, const FKey& Key, EInputEvent Event, float Amount, float DeltaTime, int32 NumSamples)
	{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		FInputKeyParams Params;
		Params.Key = Key;
		Params.Event = Event;
		Params.Delta = FVector(Amount, 0.0, 0.0);
		Params.DeltaTime = DeltaTime;
		Params.NumSamples = NumSamples;
		PC->InputKey(Params);
#else
		FInputKeyEventArgs Args(nullptr, INPUTDEVICEID_NONE, Key, Amount, DeltaTime, NumSamples, 0u);
		Args.Event = Event;
		PC->InputKey(Args);
#endif
	}

	void SendKey(APlayerController* PC, const FKey& Key, EInputEvent Event)
	{
		if (PointerButton(PC, Key, Event))
		{
			return;  // (a mouse button over the game's UI at the pointer: a click there, as the mouse's)
		}
		SendInput(PC, Key, Event, Event == IE_Released ? 0.0f : 1.0f, 1.0f / 60.0f, 0);
	}

	// (IE_Pressed: the event the FInputKeyParams overload sent an axis value with)
	void SendAxis(APlayerController* PC, const FKey& Key, float Delta, float DeltaTime)
	{
		SendInput(PC, Key, IE_Pressed, Delta, DeltaTime, 1);
	}

	/** A legacy action mapping ("legacy:<Name>"): press its key through the player controller, as the
	 *  keyboard would (the game's action events fire from it); release it a moment later, or on "release". */
	void LegacyInput(UPlaytestSubsystem& Bridge, int64 Id, const FString& Name, const FString& Mode)
	{
		APlayerController* PC = PlayerController(Bridge.GetGameWorld());
		if (!PC)
		{
			Bridge.RespondError(Id, TEXT("No player controller"));
			return;
		}
		FKey Key;
		for (const FInputActionKeyMapping& Mapping : UInputSettings::GetInputSettings()->GetActionMappings())
		{
			if (Mapping.ActionName.ToString() == Name && (!Key.IsValid() || (Key.IsMouseButton() && !Mapping.Key.IsMouseButton())))
			{
				Key = Mapping.Key;  // a keyboard key before a mouse button, as a player would press
			}
		}
		if (!Key.IsValid())
		{
			Bridge.RespondError(Id, FString::Printf(TEXT("No input mapping named %s"), *Name));
			return;
		}
		if (Mode == TEXT("release"))
		{
			SendKey(PC, Key, IE_Released);
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
			return;
		}
		SendKey(PC, Key, IE_Pressed);
		if (Mode == TEXT("hold"))
		{
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
			return;
		}
		TWeakObjectPtr<APlayerController> WeakPC(PC);
		FPending Pending;
		Pending.Id = Id;
		Pending.Op = TEXT("input");
		Pending.StartReal = FPlatformTime::Seconds();
		Pending.Seconds = TapSeconds;
		Pending.Tick = [&Bridge, WeakPC, Key, StartFrame = GFrameCounter](FPending& P, double Now)
		{
			if (!TapDone(P, Now, StartFrame))
			{
				return false;
			}
			if (APlayerController* Current = WeakPC.Get())
			{
				SendKey(Current, Key, IE_Released);
			}
			Bridge.Respond(P.Id, Obj(MakeShared<FJsonObject>()));
			return true;
		};
		Bridge.AddPending(MoveTemp(Pending));
	}
}

	void Input(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = Bridge.GetGameWorld();
		if (StringField(Args, TEXT("action")).StartsWith(TEXT("legacy:")))
		{
			LegacyInput(Bridge, Id, StringField(Args, TEXT("action")).Mid(7), StringField(Args, TEXT("mode")));
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSubsystem(World);
		if (!Subsystem)
		{
			Bridge.RespondError(Id, TEXT("No local player with Enhanced Input"));
			return;
		}
		const FString Path = StringField(Args, TEXT("action"));
		UInputAction* Action = Cast<UInputAction>(PlaytestJson::ResolveObject(Path));
		if (!Action)
		{
			Bridge.RespondError(Id, Missing(Path));
			return;
		}
		const FString Mode = StringField(Args, TEXT("mode"));
		TSharedPtr<FJsonObject> ValueJson = ObjectField(Args, TEXT("value"));
		const FVector V = ValueJson.IsValid() ? VectorField(Args, TEXT("value")) : FVector(1, 0, 0);
		const FInputActionValue Value(Action->ValueType, V);
		const TArray<UInputModifier*> Modifiers;
		const TArray<UInputTrigger*> Triggers;
		if (Mode == TEXT("hold"))
		{
#if UE_VERSION_OLDER_THAN(5, 3, 0)
			HeldActions.RemoveAll([Action](const FHeldAction& Held) { return Held.Action.Get() == Action; });
			HeldActions.Add({Subsystem, Action, Value});
			Subsystem->InjectInputForAction(Action, Value, Modifiers, Triggers);
#else
			Subsystem->StartContinuousInputInjectionForAction(Action, Value, Modifiers, Triggers);
#endif
		}
		else if (Mode == TEXT("release"))
		{
#if UE_VERSION_OLDER_THAN(5, 3, 0)
			HeldActions.RemoveAll([Action](const FHeldAction& Held) { return Held.Action.Get() == Action; });
#else
			Subsystem->StopContinuousInputInjectionForAction(Action);
#endif
		}
		else
		{
			// A press: held a moment and across two frames, then let go (as a key's tap, and as Unity's and Godot's
			// actions): a trigger reading the action once a frame sees it go down and up
#if UE_VERSION_OLDER_THAN(5, 3, 0)
			HeldActions.RemoveAll([Action](const FHeldAction& Held) { return Held.Action.Get() == Action; });
			HeldActions.Add({Subsystem, Action, Value});
			Subsystem->InjectInputForAction(Action, Value, Modifiers, Triggers);
#else
			Subsystem->StartContinuousInputInjectionForAction(Action, Value, Modifiers, Triggers);
#endif
			TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> WeakSubsystem(Subsystem);
			TWeakObjectPtr<UInputAction> WeakAction(Action);
			FPending Pending;
			Pending.Id = Id;
			Pending.Op = TEXT("input");
			Pending.StartReal = FPlatformTime::Seconds();
			Pending.Seconds = TapSeconds;
			Pending.Tick = [&Bridge, WeakSubsystem, WeakAction, StartFrame = GFrameCounter](FPending& P, double Now)
			{
				if (!TapDone(P, Now, StartFrame))
				{
					return false;
				}
				UInputAction* Current = WeakAction.Get();
#if UE_VERSION_OLDER_THAN(5, 3, 0)
				HeldActions.RemoveAll([Current](const FHeldAction& Held) { return Held.Action.Get() == Current; });
#else
				if (UEnhancedInputLocalPlayerSubsystem* S = WeakSubsystem.Get())
				{
					if (Current)
					{
						S->StopContinuousInputInjectionForAction(Current);
					}
				}
#endif
				Bridge.Respond(P.Id, Obj(MakeShared<FJsonObject>()));
				return true;
			};
			Bridge.AddPending(MoveTemp(Pending));
			return;
		}
		Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
	}

	void Move(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = Bridge.GetGameWorld();
		APlayerController* PC = PlayerController(World);
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			Bridge.RespondError(Id, TEXT("No player pawn"));
			return;
		}
		// "via" (as Unity's): Unreal's move is the pawn's own movement input (its controller); the game's keys or stick
		// are the `key` op's to press
		const FString Via = StringField(Args, TEXT("via"));
		if (!Via.IsEmpty() && Via != TEXT("auto") && Via != TEXT("controller"))
		{
			Bridge.RespondError(Id, FString::Printf(TEXT("move can't go via %s here (it adds the pawn's movement input: "
				"via controller; press the game's keys or stick with `key`)"), *Via));
			return;
		}
		const FVector Direction = VectorField(Args, TEXT("direction"));
		const double Scale = FMath::Clamp(NumberField(Args, TEXT("scale"), 1.0), -1.0, 1.0);
		TWeakObjectPtr<APawn> WeakPawn(Pawn);
		const FVector Start = Pawn->GetActorLocation();
		FPending Pending;
		Pending.Id = Id;
		Pending.Op = TEXT("move");
		Pending.StartReal = FPlatformTime::Seconds();
		Pending.Seconds = NumberField(Args, TEXT("seconds"), 0.35);
		Pending.Tick = [&Bridge, WeakPawn, Direction, Scale, Start](FPending& P, double Now)
		{
			APawn* Current = WeakPawn.Get();
			if (Current && Now - P.StartReal < P.Seconds)
			{
				Current->AddMovementInput(Direction, (float)Scale, true);
				return false;
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const double Moved = Current ? FVector::Dist2D(Current->GetActorLocation(), Start) : 0.0;
			Result->SetField(TEXT("moved"), PlaytestJson::Number(FMath::RoundToDouble(Moved * 10) / 10));
			Result->SetStringField(TEXT("via"), TEXT("controller"));
			Bridge.Respond(P.Id, Obj(Result));
			return true;
		};
		Bridge.AddPending(MoveTemp(Pending));
	}

	// --- keys: the player's hands -------------------------------------------------------------------
	// Keyboard keys, mouse buttons, mouse movement and gamepad sticks go in as the devices would send them
	// (APlayerController::InputKey, see SendInput), so the game's own mappings, contexts, modifiers and sensitivity apply,
	// and a key does nothing while the game doesn't map it. A held key stays down, and a held axis keeps its
	// value every frame, until released, until the brain stops renewing it (the lease: a brain gone quiet
	// leaves nothing held), or until the session ends.

namespace
{
	struct FHeldKey
	{
		FKey Key;
		float Value = 1.0f;
		bool bAxis = false;
		bool bRate = false;  // an axis value per second (mouse movement): sent as Value * frame time each frame
		double Renewed = 0.0;
		double Until = 0.0;  // a timed hold: let go at this game time (world seconds), needing no renewal; 0: leased
	};
	TMap<FName, FHeldKey> HeldKeys;
	TWeakObjectPtr<APlayerController> KeysController;
	double KeyLeaseSeconds = 1.0;

	void ReleaseHeldKeys(APlayerController* PC)
	{
		if (PC)
		{
			for (const TPair<FName, FHeldKey>& Pair : HeldKeys)
			{
				if (!Pair.Value.bAxis)
				{
					SendKey(PC, Pair.Value.Key, IE_Released);
				}
			}
		}
		HeldKeys.Reset();
	}
}

	void KeyOp(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		APlayerController* PC = PlayerController(Bridge.GetGameWorld());
		const FString Mode = StringField(Args, TEXT("mode"));
		double Lease = 0.0;
		if (Args->TryGetNumberField(TEXT("lease"), Lease) && Lease > 0.0)
		{
			KeyLeaseSeconds = FMath::Clamp(Lease, 0.2, 10.0);
		}
		if (Mode == TEXT("release_all"))
		{
			ReleaseHeldKeys(PC == KeysController.Get() ? PC : nullptr);
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
			return;
		}
		if (!PC)
		{
			Bridge.RespondError(Id, TEXT("No player controller"));
			return;
		}
		if (KeysController.Get() != PC)
		{
			HeldKeys.Reset();  // a new session: nothing of the old one is held
			KeysController = PC;
		}
		const FKey Key(*StringField(Args, TEXT("key")));
		if (!Key.IsValid())
		{
			Bridge.RespondError(Id, FString::Printf(TEXT("Unknown key %s"), *StringField(Args, TEXT("key"))));
			return;
		}
		double Value = 1.0;
		Args->TryGetNumberField(TEXT("value"), Value);
		bool bRate = false;
		Args->TryGetBoolField(TEXT("rate"), bRate);
		const bool bAxis = Key.IsAxis1D();
		if (Mode == TEXT("release"))
		{
			if (const FHeldKey* Held = HeldKeys.Find(Key.GetFName()))
			{
				if (!Held->bAxis)
				{
					SendKey(PC, Key, IE_Released);
				}
				HeldKeys.Remove(Key.GetFName());
			}
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
			return;
		}
		if (Mode == TEXT("hold"))
		{
			FHeldKey* Held = HeldKeys.Find(Key.GetFName());
			if (!Held)
			{
				Held = &HeldKeys.Add(Key.GetFName());
				Held->Key = Key;
				if (!bAxis)
				{
					SendKey(PC, Key, IE_Pressed);  // goes down once and stays down
				}
			}
			Held->Value = (float)Value;
			Held->bAxis = bAxis;
			Held->bRate = bRate;
			Held->Renewed = FPlatformTime::Seconds();
			// A timed hold ("seconds"): the game lets go after exactly that much game time, however late the brain is
			double Seconds = 0.0;
			const UWorld* World = Bridge.GetGameWorld();
			Held->Until = Args->TryGetNumberField(TEXT("seconds"), Seconds) && Seconds > 0.0 && World
				? World->GetTimeSeconds() + Seconds : 0.0;
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
			return;
		}
		// A tap: an axis moves once; a key goes down, and up again a moment later, as a quick press does
		if (bAxis)
		{
			SendAxis(PC, Key, (float)Value, FApp::GetDeltaTime());
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
			return;
		}
		SendKey(PC, Key, IE_Pressed);
		TWeakObjectPtr<APlayerController> WeakPC(PC);
		FPending Pending;
		Pending.Id = Id;
		Pending.Op = TEXT("key");
		Pending.StartReal = FPlatformTime::Seconds();
		Pending.Seconds = TapSeconds;
		Pending.Tick = [&Bridge, WeakPC, Key, StartFrame = GFrameCounter](FPending& P, double Now)
		{
			if (!TapDone(P, Now, StartFrame))
			{
				return false;
			}
			APlayerController* Current = WeakPC.Get();
			if (Current && !HeldKeys.Contains(Key.GetFName()))  // (unless it is held now)
			{
				SendKey(Current, Key, IE_Released);
			}
			Bridge.Respond(P.Id, Obj(MakeShared<FJsonObject>()));
			return true;
		};
		Bridge.AddPending(MoveTemp(Pending));
	}

	void TickKeys(UPlaytestSubsystem& Bridge, float DeltaTime)
	{
		TickPointer(Bridge);
#if UE_VERSION_OLDER_THAN(5, 3, 0)
		for (int32 I = HeldActions.Num() - 1; I >= 0; --I)
		{
			UEnhancedInputLocalPlayerSubsystem* Subsystem = HeldActions[I].Subsystem.Get();
			const UInputAction* Action = HeldActions[I].Action.Get();
			if (!Bridge.IsConnected() || !Subsystem || !Action || Subsystem != InputSubsystem(Bridge.GetGameWorld()))
			{
				HeldActions.RemoveAt(I);
				continue;
			}
			Subsystem->InjectInputForAction(Action, HeldActions[I].Value, {}, {});
		}
#endif
		if (HeldKeys.Num() == 0)
		{
			return;
		}
		APlayerController* PC = PlayerController(Bridge.GetGameWorld());
		if (!Bridge.IsConnected() || !PC || PC != KeysController.Get())
		{
			ReleaseHeldKeys(PC && PC == KeysController.Get() ? PC : nullptr);  // the brain or the session is gone
			return;
		}
		const double Now = FPlatformTime::Seconds();
		const UWorld* World = Bridge.GetGameWorld();
		const double GameNow = World ? World->GetTimeSeconds() : 0.0;
		TArray<FName> Expired;
		for (TPair<FName, FHeldKey>& Pair : HeldKeys)
		{
			const FHeldKey& Held = Pair.Value;
			if (Held.Until > 0.0 ? GameNow >= Held.Until : Now - Held.Renewed > KeyLeaseSeconds)
			{
				Expired.Add(Pair.Key);
			}
			else if (Held.bAxis)
			{
				SendAxis(PC, Held.Key, Held.bRate ? Held.Value * DeltaTime : Held.Value, DeltaTime);
			}
		}
		for (const FName& Name : Expired)
		{
			const FHeldKey Held = HeldKeys.FindAndRemoveChecked(Name);
			if (!Held.bAxis)
			{
				SendKey(PC, Held.Key, IE_Released);
			}
		}
	}
}
