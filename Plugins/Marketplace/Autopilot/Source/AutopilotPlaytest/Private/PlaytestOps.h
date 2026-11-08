// Shared by the request handlers (docs/protocol.md), one file per family of ops:
//
//   PlaytestOps.cpp           Handle: routes each request to its op's handler; small JSON and player helpers
//   PlaytestOpsRc.cpp         `rc`: Remote Control compatible calls, property reads and writes, descriptions
//   PlaytestOpsSession.cpp    `context`, `session`, `levels`, `pause`, `timestep`, `ping`
//   PlaytestOpsInput.cpp      `input`, `move`, `key` (and TickKeys: keys held between frames)
//   PlaytestOpsUI.cpp         `screen`, `ui`, `ui_act`
//   PlaytestOpsView.cpp       `view`: what the player sees and what its body feels
//   PlaytestOpsOpenings.cpp   view's `openings` and `ways_up`
//   PlaytestOpsSonar.cpp      view's `sonar`
//   PlaytestOpsLook.cpp       how a thing is drawn, and its visible body: for `view` and `watch`
//   PlaytestOpsWatch.cpp      `watch`: what changes over a stretch of game time
//   PlaytestMedia.cpp         `record` and `capture`: the game's picture (FPlaytestMedia, PlaytestInternal.h)
//
// Handle and TickKeys, which the subsystem calls, are declared in PlaytestInternal.h.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "PlaytestInternal.h"
#include "PlaytestSubsystem.h"

class AActor;
class APawn;
class APlayerController;
class UEnhancedInputLocalPlayerSubsystem;
class UPrimitiveComponent;
class USceneComponent;

namespace PlaytestOps
{
	using FPending = UPlaytestSubsystem::FPending;

	// --- helpers (PlaytestOps.cpp) -------------------------------------------------------------------------------

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Json, const FString& Name);
	FString StringField(const TSharedPtr<FJsonObject>& Json, const FString& Name);
	double NumberField(const TSharedPtr<FJsonObject>& Json, const FString& Name, double Default);
	FVector VectorField(const TSharedPtr<FJsonObject>& Json, const FString& Name);
	TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O);
	FString PathOf(const UObject* Object);
	/** Remote Control's wording for a missing object. */
	FString Missing(const FString& Path);
	/** A JSON field by name, case-insensitive. */
	TSharedPtr<FJsonValue> FindField(const TSharedPtr<FJsonObject>& Json, const FString& Name);
	APlayerController* PlayerController(UWorld* World);
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem(UWorld* World);
	/** {"X", "Y", "Z"}, each scaled and rounded to a whole number. */
	TSharedRef<FJsonObject> Vec(const FVector& V, double Scale = 1.0);

	// --- the ops' handlers ---------------------------------------------------------------------------------------

	void Rc(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Context(UPlaytestSubsystem& Bridge, int64 Id);
	void Session(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Levels(UPlaytestSubsystem& Bridge, int64 Id);
	void Pause(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Timestep(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Ping(UPlaytestSubsystem& Bridge, int64 Id);
	void Input(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Move(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void KeyOp(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	/** `pointer` (PlaytestOpsPointer.cpp): the mouse to a place on the game's view; the buttons pressed after click there. */
	void PointerOp(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	/** A mouse button going down or up while the pointer is set: true when it went to the game's UI under the pointer
	 *  (then not to the player controller). */
	bool PointerButton(APlayerController* PC, const FKey& Key, EInputEvent Event);
	/** Every frame: the pointer kept where it was put (forgotten once the brain or the session is gone). */
	void TickPointer(UPlaytestSubsystem& Bridge);
	void Screen(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void UiList(UPlaytestSubsystem& Bridge, int64 Id);
	void UiAct(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void View(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Watch(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);

	// --- how a thing looks (PlaytestOpsLook.cpp; see the comments there) ----------------------------------------

	bool Drawn(const USceneComponent* Part);
	bool DrawnFor(const UPrimitiveComponent* Part, const AActor* Viewer, const APlayerController* PC);
	bool PlayersOwn(const AActor* Actor, const APawn* Pawn, const APlayerController* PC);
	bool IsBodyPart(const UPrimitiveComponent* Part);
	void BodyBounds(const AActor* Actor, FBox& Visible, FBox& Local);

	// A thing's own box: its bounds in its own frame, turned and placed as it is. A tipped slab is a thin box along
	// the slope, not the tall box around it (whose space under the slab's raised end is empty)
	struct FPlaytestShape
	{
		FVector Center = FVector::ZeroVector;
		FVector Axis[3] = {FVector(1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, 1.0)};
		FVector Half = FVector::ZeroVector;  // half its size along each of its own axes

		// How far a point is from it: 0 inside it
		double Distance(const FVector& P) const
		{
			const FVector D = P - Center;
			double Sum = 0.0;
			for (int32 i = 0; i < 3; ++i)
			{
				const double Out = FMath::Max(0.0, FMath::Abs(FVector::DotProduct(D, Axis[i])) - Half[i]);
				Sum += Out * Out;
			}
			return FMath::Sqrt(Sum);
		}

		// Its axis turned most upward: its top face's
		int32 UpIndex() const
		{
			int32 Best = 0;
			for (int32 i = 1; i < 3; ++i)
			{
				if (FMath::Abs(Axis[i].Z) > FMath::Abs(Axis[Best].Z))
				{
					Best = i;
				}
			}
			return Best;
		}

		// Its top face's normal, pointing up
		FVector TopNormal() const
		{
			const int32 K = UpIndex();
			return Axis[K] * (Axis[K].Z >= 0.0 ? 1.0 : -1.0);
		}

		// Whether a point is at it: inside it grown on every side by a quarter of its size along that axis (by half its
		// size in all), at least Reach units
		bool Holds(const FVector& P, double Reach) const
		{
			const FVector D = P - Center;
			for (int32 i = 0; i < 3; ++i)
			{
				if (FMath::Abs(FVector::DotProduct(D, Axis[i])) > Half[i] + FMath::Max(Reach, Half[i] * 0.5))
				{
					return false;
				}
			}
			return true;
		}
	};

	FPlaytestShape ShapeOf(const AActor* Actor, const FBox& Local, const FVector& Origin, const FVector& Extent);

	// A light a game draws at a thing's place from a helper actor beside it: lit, where it is, how bright
	struct FPlacedLight
	{
		FVector At;
		double Brightness;
	};

	// How far a placed light may lie outside a thing's box and still be at it, at least (see FPlaytestShape::Holds)
	const double PlacedLightReach = 100.0;

	TArray<FPlacedLight> PlacedLights(const UWorld* World, const APawn* Pawn, const APlayerController* PC);

	// How a thing is drawn (DrawnOf)
	struct FDrawn
	{
		TArray<FString> Parts;  // the asset names of the meshes it is drawn with, sorted, each once
		TArray<FString> Biggest;  // the 8 of them drawn biggest, sorted (what `view` tells)
		TMap<FString, double> Effects;  // effects playing: asset name -> its scale (the largest, of several)
		int32 Lights = 0;  // lights on (its own, and those placed at it: see PlacedLights)
		double Brightness = 0.0;  // the brightest of them
		TArray<FString> Animations;  // animation montages playing, its own meshes' first
		bool bLimp = false;  // its own skeletal mesh hangs limp: most of its bodies simulate physics (a ragdoll)
		TArray<FString> Texts;  // texts drawn on it (up to 4, each up to 40 characters)
		TArray<double> Bars;  // how full the bars drawn on it are, 0..1 (up to 4)
		bool bHighlighted = false;  // a part drawn in the custom depth pass (an outline) or with an overlay material
	};

	void AddLight(FDrawn& D, double Brightness);
	FDrawn DrawnOf(AActor* Actor, const APawn* Pawn, const APlayerController* PC, bool bTexts);
	void AddPlacedLights(FDrawn& D, const AActor* Actor, const TArray<FPlacedLight>& Placed);
	TSharedPtr<FJsonObject> LookJson(const FDrawn& D);

	// --- the player's body probing the room (view's `openings`, `ways_up` and `sonar`) ----------------------------

	AActor* WholeOf(AActor* Actor);

	// The player's body as it probes the room: its collision (what stops it), its capsule, how high it steps and how
	// steep a floor it stands on
	struct FBodyProbe
	{
		UWorld* World = nullptr;
		FCollisionQueryParams Params;
		FCollisionResponseParams Responses;
		ECollisionChannel Channel = ECC_Pawn;
		double Radius = 0.0, HalfHeight = 0.0, StepHeight = 0.0, WalkableZ = 0.7;
		double Feet = 0.0;
		FVector Me = FVector::ZeroVector;  // the body's middle

		// The first thing along a line that stops the body (its own collision channel and responses)
		bool Line(const FVector& A, const FVector& B, FHitResult& Hit) const
		{
			return World->LineTraceSingleByChannel(Hit, A, B, Channel, Params, Responses) && Hit.bBlockingHit;
		}

		// Its own capsule, a hair thinner and lifted a step's height off the feet (what it steps up onto is the floor's
		// to tell, see Floor), swept from one place for the feet to another: whether something stops it on the way
		bool Sweep(const FVector& FromFeet, const FVector& ToFeet, FHitResult& Hit) const
		{
			const double R = FMath::Max(1.0, Radius - 2.0);
			const double Half = FMath::Max(R, HalfHeight - StepHeight * 0.5 - 2.0);
			const FVector Lift(0.0, 0.0, StepHeight + 2.0 + Half);
			return World->SweepSingleByChannel(Hit, FromFeet + Lift, ToFeet + Lift, FQuat::Identity, Channel,
					   FCollisionShape::MakeCapsule((float)R, (float)Half), Params, Responses)
				&& Hit.bBlockingHit;
		}

		// The floor under a place, straight down from `From` by `Depth`: how high it is and whether a body stands on it
		// (no steeper than the walkable slope)
		bool Floor(const FVector& From, double Depth, double& Z, bool& bWalkable) const
		{
			FHitResult Hit;
			if (!Line(From, From - FVector(0.0, 0.0, Depth), Hit))
			{
				return false;
			}
			Z = Hit.ImpactPoint.Z;
			bWalkable = Hit.ImpactNormal.Z >= WalkableZ;
			return true;
		}
	};

	void AddOpenings(FJsonObject& Body, const FBodyProbe& P, const FVector& Cam, double Yaw, double Fov, double Reach,
		bool bOpenings, bool bWaysUp, const TArray<TPair<AActor*, FPlaytestShape>>& Holders,
		const FCollisionQueryParams& Sight);

	const int32 SonarMaxTurns = 16;  // the most `sonar_turns` a view tries
	void AddSonar(FJsonObject& Body, const FBodyProbe& P, double Yaw, double Reach, const TArray<double>& Turns);
}
