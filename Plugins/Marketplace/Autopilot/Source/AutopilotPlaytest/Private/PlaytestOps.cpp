// The brain's requests (docs/protocol.md), handled on the game thread. Handle routes each request to its op's
// handler; the handlers live in PlaytestOps*.cpp, one file per family of ops (see PlaytestOps.h). This file also
// holds the small JSON and player helpers they share.

#include "PlaytestOps.h"

#include "Engine/LocalPlayer.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace PlaytestOps
{
	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Json, const FString& Name)
	{
		const TSharedPtr<FJsonObject>* Out = nullptr;
		if (Json.IsValid() && Json->TryGetObjectField(Name, Out) && Out)
		{
			return *Out;
		}
		return nullptr;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Json, const FString& Name)
	{
		FString Out;
		if (Json.IsValid())
		{
			Json->TryGetStringField(Name, Out);
		}
		return Out;
	}

	double NumberField(const TSharedPtr<FJsonObject>& Json, const FString& Name, double Default)
	{
		double Out = Default;
		if (Json.IsValid())
		{
			Json->TryGetNumberField(Name, Out);
		}
		return Out;
	}

	FVector VectorField(const TSharedPtr<FJsonObject>& Json, const FString& Name)
	{
		const TSharedPtr<FJsonObject> V = ObjectField(Json, Name);
		return FVector(NumberField(V, TEXT("X"), 0), NumberField(V, TEXT("Y"), 0), NumberField(V, TEXT("Z"), 0));
	}

	TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O)
	{
		return MakeShared<FJsonValueObject>(O);
	}

	FString PathOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	FString Missing(const FString& Path)
	{
		return FString::Printf(TEXT("Object %s does not exist"), *Path);
	}

	/** A JSON field by name, case-insensitive. */
	TSharedPtr<FJsonValue> FindField(const TSharedPtr<FJsonObject>& Json, const FString& Name)
	{
		if (!Json.IsValid())
		{
			return nullptr;
		}
		if (TSharedPtr<FJsonValue> Exact = Json->TryGetField(Name))
		{
			return Exact;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json->Values)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pair.Value;
			}
		}
		return nullptr;
	}

	APlayerController* PlayerController(UWorld* World)
	{
		// The local player's: input goes to the player on this machine (the server learns it from that player's
		// controller). In a server's world the first controller can stand in for a remote player, which takes no input.
		if (!World)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC && PC->IsLocalController())
			{
				return PC;
			}
		}
		return nullptr;
	}

	UEnhancedInputLocalPlayerSubsystem* InputSubsystem(UWorld* World)
	{
		APlayerController* PC = PlayerController(World);
		ULocalPlayer* Player = PC ? PC->GetLocalPlayer() : nullptr;
		return Player ? Player->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	}

	TSharedRef<FJsonObject> Vec(const FVector& V, double Scale)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("X"), FMath::RoundToDouble(V.X * Scale));
		O->SetNumberField(TEXT("Y"), FMath::RoundToDouble(V.Y * Scale));
		O->SetNumberField(TEXT("Z"), FMath::RoundToDouble(V.Z * Scale));
		return O;
	}

	void Handle(UPlaytestSubsystem& Bridge, int64 Id, const FString& Op, const TSharedPtr<FJsonObject>& Args)
	{
		if (Op == TEXT("key"))
		{
			KeyOp(Bridge, Id, Args);
		}
		else if (Op == TEXT("pointer"))
		{
			PointerOp(Bridge, Id, Args);
		}
		else if (Op == TEXT("view"))
		{
			View(Bridge, Id, Args);
		}
		else if (Op == TEXT("rc"))
		{
			Rc(Bridge, Id, Args);
		}
		else if (Op == TEXT("context"))
		{
			Context(Bridge, Id);
		}
		else if (Op == TEXT("session"))
		{
			Session(Bridge, Id, Args);
		}
		else if (Op == TEXT("levels"))
		{
			Levels(Bridge, Id);
		}
		else if (Op == TEXT("pause"))
		{
			Pause(Bridge, Id, Args);
		}
		else if (Op == TEXT("input"))
		{
			Input(Bridge, Id, Args);
		}
		else if (Op == TEXT("move"))
		{
			Move(Bridge, Id, Args);
		}
		else if (Op == TEXT("watch"))
		{
			Watch(Bridge, Id, Args);
		}
		else if (Op == TEXT("screen"))
		{
			Screen(Bridge, Id, Args);
		}
		else if (Op == TEXT("ui"))
		{
			UiList(Bridge, Id);
		}
		else if (Op == TEXT("ui_act"))
		{
			UiAct(Bridge, Id, Args);
		}
		else if (Op == TEXT("timestep"))
		{
			Timestep(Bridge, Id, Args);
		}
		else if (Op == TEXT("detectors"))
		{
			Bridge.GetDetectors().Configure(Args);
			Bridge.Respond(Id, Obj(MakeShared<FJsonObject>()));
		}
		else if (Op == TEXT("ping"))
		{
			Ping(Bridge, Id);
		}
		else if (Op == TEXT("record"))
		{
			Bridge.GetMedia().Record(Id, Args);
		}
		else if (Op == TEXT("capture"))
		{
			Bridge.GetMedia().Capture(Id, Args);  // (answered once the picture is read back and encoded)
		}
		else if (Op == TEXT("codex_manifest"))
		{
			PlaytestCodex::Manifest(Bridge, Id, Args);
		}
		else if (Op == TEXT("codex_read"))
		{
			PlaytestCodex::Read(Bridge, Id, Args);
		}
		else
		{
			Bridge.RespondError(Id, FString::Printf(TEXT("unknown op %s"), *Op));
		}
	}
}
