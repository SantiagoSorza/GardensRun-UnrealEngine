// `watch` (docs/protocol.md): what turns, moves, changes size or changes how it is drawn over a stretch of game
// time, sampled every frame. Answers when the stretch is over.

#include "PlaytestOps.h"

#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/WidgetComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Particles/ParticleSystemComponent.h"
#include "PlaytestJson.h"

namespace PlaytestOps
{
namespace
{
	struct FWatched
	{
		TWeakObjectPtr<USceneComponent> Component;
		FString Path;  // the name it is reported by
		int32 Thing = INDEX_NONE;  // the watched thing it is a part of, sampled in that one's frame (see FWatchedLook);
		// none: sampled against what it is attached to (its relative transform)
		bool bSampled = false;
		FQuat Rotation = FQuat::Identity;
		FVector Location = FVector::ZeroVector;
		FVector Scale = FVector::OneVector;
		TMap<FString, double> Found;  // what -> game time first seen
	};

	// A thing watched for how it is drawn: each change from one frame to the next, the first time it is seen. And the
	// frame its parts' motion is sampled in: its place as it is now, turned as it stood when the watch began, so the
	// whole thing going along isn't a part moving while a part turning or moving against where it stood is, even when
	// what moves it is a hidden root the art hangs under (a mesh the game shakes, a child actor's art on it). A pawn's
	// heading follows it too: a character or a vehicle turning round is the whole thing's motion (`view` tells its yaw);
	// its rocking or tipping isn't (a drone rocking when rammed)
	struct FWatchedLook
	{
		TWeakObjectPtr<AActor> Actor;
		FString Path;
		bool bSampled = false;
		FDrawn Last;
		bool bFramed = false;
		FQuat StartRotation = FQuat::Identity;  // how it was turned when the watch began
		double StartYaw = 0.0;  // its heading then
		struct FChange
		{
			double T;
			FString What, Asset;
		};
		TArray<FChange> Found;

		void Note(double T, const TCHAR* What, const FString& Asset)
		{
			if (!Found.ContainsByPredicate([What, &Asset](const FChange& C) { return C.What == What && C.Asset == Asset; }))
			{
				Found.Add({T, What, Asset});
			}
		}
	};

	// How far one rotation is turned from another, in degrees (the angle between them, whichever way)
	double TurnedBetween(const FQuat& A, const FQuat& B)
	{
		const double Dot = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
		return FMath::RadiansToDegrees(2.0 * FMath::Acos(FMath::Min(1.0, FMath::Abs(Dot))));
	}

	// Whether a part is carried by a skeleton: attached, itself or what it hangs from (up to the thing's root), to a
	// bone or socket of a skinned mesh, so it sways with each step the animation takes (a weapon in a hand): `look`
	// tells that as the animation, not as the part moving
	bool OnSkeleton(const USceneComponent* Part, const USceneComponent* Root)
	{
		int32 Depth = 0;
		for (const USceneComponent* C = Part; C && C != Root && Depth < 32; C = C->GetAttachParent(), ++Depth)
		{
			const USceneComponent* Parent = C->GetAttachParent();
			if (Parent && C->GetAttachSocketName() != NAME_None && Parent->IsA(USkinnedMeshComponent::StaticClass()))
			{
				return true;
			}
		}
		return false;
	}

	// The parts of a thing watched for motion. Its root, unless that is a collision shape (a character's capsule, a
	// trigger box: what moves of it shows in its meshes), an effect, a label or a widget, or is neither drawn nor carries
	// anything drawn (a hidden mesh the game shakes, the art hanging under it, is watched). And up to Count of the
	// visible meshes it is drawn with, the biggest first: its own, its child actors' and those of the actors attached to
	// it (not the player's), never a widget drawn over it, nor one a skeleton carries (see OnSkeleton)
	TArray<USceneComponent*> WatchedParts(AActor* Actor, int32 Count, const APawn* Pawn, const APlayerController* PC)
	{
		TArray<USceneComponent*> Out;
		USceneComponent* Root = Actor->GetRootComponent();
		TArray<AActor*> Sources = {Actor};
		TArray<AActor*> Attached;
		Actor->GetAttachedActors(Attached, true, true);
		for (AActor* Other : Attached)
		{
			if (Other && !Other->IsHidden() && !PlayersOwn(Other, Pawn, PC))
			{
				Sources.AddUnique(Other);
			}
		}
		TArray<UMeshComponent*> Meshes;
		for (const AActor* Source : Sources)
		{
			TInlineComponentArray<UMeshComponent*> Own(Source, true);  // (its child actors' too)
			for (UMeshComponent* Mesh : Own)
			{
				if (Mesh && Mesh != Root && Drawn(Mesh) && !Mesh->IsA(UWidgetComponent::StaticClass()) && !OnSkeleton(Mesh, Root))
				{
					Meshes.AddUnique(Mesh);
				}
			}
		}
		if (Root && Root->IsRegistered() && !Root->IsA(UShapeComponent::StaticClass())
			&& !Root->IsA(UFXSystemComponent::StaticClass()) && !Root->IsA(UWidgetComponent::StaticClass())
			&& !Root->IsA(UTextRenderComponent::StaticClass())
			&& (Drawn(Root) || Meshes.ContainsByPredicate([Root](const UMeshComponent* Mesh) { return Mesh->IsAttachedTo(Root); })))
		{
			Out.Add(Root);
		}
		Meshes.Sort([](const UMeshComponent& A, const UMeshComponent& B) { return A.Bounds.SphereRadius > B.Bounds.SphereRadius; });
		for (int32 i = 0; i < Meshes.Num() && i < Count; ++i)
		{
			Out.Add(Meshes[i]);
		}
		return Out;
	}

	// The name a watched part of a thing goes by ('<thing path>.<part>'): its own path, when it is the thing's own; a
	// child actor's goes by the thing's child actor component it hangs from (to the thing's Blueprint the art it spawns
	// is that one component, named by its maker; each name told once); an attached actor's by that actor's name
	FString PartPath(const AActor* Thing, const USceneComponent* Part)
	{
		const AActor* Below = Part->GetOwner();
		for (int32 Depth = 0; Below && Below != Thing && Depth < 16; ++Depth)
		{
			if (const UChildActorComponent* Holder = Below->IsChildActor() ? Below->GetParentComponent() : nullptr)
			{
				if (Holder->GetOwner() == Thing)
				{
					return PathOf(Holder);
				}
				Below = Holder->GetOwner();
				continue;
			}
			const AActor* Up = Below->GetAttachParentActor();
			if (Up == Thing)
			{
				return PathOf(Thing) + TEXT(".") + Below->GetName();
			}
			Below = Up;
		}
		return PathOf(Part);
	}
}

	void Watch(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		TArray<FWatched> Watched;
		TArray<FWatchedLook> Looks;
		bool bDrawn = false;
		Args->TryGetBoolField(TEXT("drawn"), bDrawn);
		const int32 PartCount = FMath::Clamp(FMath::RoundToInt32(NumberField(Args, TEXT("parts"), 3.0)), 0, 16);
		const APlayerController* Player = PlayerController(Bridge.GetGameWorld());
		const APawn* PlayerPawn = Player ? Player->GetPawn() : nullptr;
		auto Add = [&Watched](USceneComponent* Component, const FString& Path, int32 Thing)
		{
			FWatched W;
			W.Component = Component;
			W.Path = Path;
			W.Thing = Thing;
			Watched.Add(MoveTemp(W));
		};
		const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
		if (Args->TryGetArrayField(TEXT("objects"), Objects) && Objects)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Objects)
			{
				const FString Path = Value->AsString();
				UObject* Object = PlaytestJson::ResolveObject(Path);
				AActor* Actor = Cast<AActor>(Object);
				if (bDrawn && Actor)
				{
					// A thing: the parts of it a player sees move, by their own names (see PartPath), sampled in its
					// frame, and how it is drawn
					for (USceneComponent* Part : WatchedParts(Actor, PartCount, PlayerPawn, Player))
					{
						Add(Part, PartPath(Actor, Part), Looks.Num());
					}
					FWatchedLook L;
					L.Actor = Actor;
					L.Path = Path;
					Looks.Add(MoveTemp(L));
					continue;
				}
				USceneComponent* Component = Cast<USceneComponent>(Object);
				if (!Component && Actor)
				{
					Component = Actor->GetRootComponent();
				}
				if (Component)
				{
					Add(Component, Path, INDEX_NONE);
				}
			}
		}
		const double Turned = NumberField(Args, TEXT("turned"), 1.0);
		const double MovedLimit = NumberField(Args, TEXT("moved"), 2.0);
		const double Resized = NumberField(Args, TEXT("resized"), 0.02);
		TSharedPtr<TArray<FWatched>> State = MakeShared<TArray<FWatched>>(MoveTemp(Watched));
		TSharedPtr<TArray<FWatchedLook>> LookState = MakeShared<TArray<FWatchedLook>>(MoveTemp(Looks));
		FPending Pending;
		Pending.Id = Id;
		Pending.Op = TEXT("watch");
		Pending.StartReal = FPlatformTime::Seconds();
		Pending.Seconds = NumberField(Args, TEXT("seconds"), 0.6);
		Pending.Tick = [&Bridge, State, LookState, Turned, MovedLimit, Resized, bDrawn](FPending& P, double Now)
		{
			const double GameTime = FMath::Max(0.0, Bridge.GameTime());
			const UWorld* World = Bridge.GetGameWorld();
			const APlayerController* PC = PlayerController(Bridge.GetGameWorld());
			const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			// The lights helpers draw at things' places, each counted for whichever watched thing it is at
			const TArray<FPlacedLight> Placed = LookState->Num() > 0 ? PlacedLights(World, Pawn, PC) : TArray<FPlacedLight>();
			// Each watched thing's frame now (see FWatchedLook)
			struct FThingFrame
			{
				bool bValid = false;
				FQuat Rotation = FQuat::Identity;
				FVector Location = FVector::ZeroVector;
			};
			TArray<FThingFrame> Frames;
			Frames.SetNum(LookState->Num());
			for (int32 Index = 0; Index < LookState->Num(); ++Index)
			{
				FWatchedLook& L = (*LookState)[Index];
				AActor* Actor = L.Actor.Get();
				if (!Actor)
				{
					continue;
				}
				const FTransform& Transform = Actor->GetActorTransform();
				if (!L.bFramed)
				{
					L.bFramed = true;
					L.StartRotation = Transform.GetRotation();
					L.StartYaw = Transform.Rotator().Yaw;
				}
				FThingFrame& Frame = Frames[Index];
				Frame.bValid = true;
				Frame.Location = Transform.GetLocation();
				Frame.Rotation = L.StartRotation;
				if (Actor->IsA(APawn::StaticClass()))
				{
					const double Heading = FRotator::NormalizeAxis(Transform.Rotator().Yaw - L.StartYaw);
					Frame.Rotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(Heading)) * L.StartRotation;
				}
				FDrawn D = DrawnOf(Actor, Pawn, PC, false);
				AddPlacedLights(D, Actor, Placed);
				if (L.bSampled)
				{
					for (const FString& Part : D.Parts)
					{
						if (!L.Last.Parts.Contains(Part))
						{
							L.Note(GameTime, TEXT("part shown"), Part);
						}
					}
					for (const FString& Part : L.Last.Parts)
					{
						if (!D.Parts.Contains(Part))
						{
							L.Note(GameTime, TEXT("part hidden"), Part);
						}
					}
					for (const TPair<FString, double>& Effect : D.Effects)
					{
						if (!L.Last.Effects.Contains(Effect.Key))
						{
							L.Note(GameTime, TEXT("effect started"), Effect.Key);
						}
					}
					for (const TPair<FString, double>& Effect : L.Last.Effects)
					{
						if (!D.Effects.Contains(Effect.Key))
						{
							L.Note(GameTime, TEXT("effect stopped"), Effect.Key);
						}
					}
					if (D.Lights != L.Last.Lights)
					{
						L.Note(GameTime, D.Lights > L.Last.Lights ? TEXT("light on") : TEXT("light off"), FString());
					}
					for (const FString& Animation : D.Animations)
					{
						if (!L.Last.Animations.Contains(Animation))
						{
							L.Note(GameTime, TEXT("animation started"), Animation);
						}
					}
					for (const FString& Animation : L.Last.Animations)
					{
						if (!D.Animations.Contains(Animation))
						{
							L.Note(GameTime, TEXT("animation ended"), Animation);
						}
					}
					if (D.bLimp && !L.Last.bLimp)
					{
						L.Note(GameTime, TEXT("went limp"), FString());
					}
				}
				L.bSampled = true;
				L.Last = MoveTemp(D);
			}
			for (FWatched& W : *State)
			{
				USceneComponent* C = W.Component.Get();
				if (!C)
				{
					continue;
				}
				FQuat R = C->GetRelativeRotation().Quaternion();
				FVector L = C->GetRelativeLocation();
				FVector S = C->GetRelativeScale3D();
				if (W.Thing != INDEX_NONE)
				{
					// A part of a watched thing: where it is in the world, in that thing's frame (in world units), and
					// its size in the world
					const FThingFrame& Frame = Frames[W.Thing];
					if (!Frame.bValid)
					{
						continue;
					}
					R = Frame.Rotation.Inverse() * C->GetComponentQuat();
					L = Frame.Rotation.UnrotateVector(C->GetComponentLocation() - Frame.Location);
					S = C->GetComponentScale();
				}
				if (!W.bSampled)
				{
					W.bSampled = true;
					W.Rotation = R;
					W.Location = L;
					W.Scale = S;
					continue;
				}
				if (TurnedBetween(R, W.Rotation) > Turned && !W.Found.Contains(TEXT("turned")))
				{
					W.Found.Add(TEXT("turned"), GameTime);
				}
				if (FVector::Dist(L, W.Location) > MovedLimit && !W.Found.Contains(TEXT("moved")))
				{
					W.Found.Add(TEXT("moved"), GameTime);
				}
				const FVector Base(FMath::Max(FMath::Abs(W.Scale.X), 1e-3), FMath::Max(FMath::Abs(W.Scale.Y), 1e-3), FMath::Max(FMath::Abs(W.Scale.Z), 1e-3));
				const FVector Rel = (S - W.Scale).GetAbs() / Base;
				if (Rel.GetMax() > Resized && !W.Found.Contains(TEXT("changed size")))
				{
					W.Found.Add(TEXT("changed size"), GameTime);
				}
			}
			if (Now - P.StartReal < P.Seconds)
			{
				return false;
			}
			struct FChange
			{
				double T;
				FString Object;
				FString What;
				FString Asset;
				bool bLook;  // a change of how it is drawn (it says which asset), not a part's motion
			};
			TArray<FChange> Changes;
			for (const FWatched& W : *State)
			{
				for (const TPair<FString, double>& F : W.Found)
				{
					// Parts that go by one name (a child actor's meshes, see PartPath): each change once, the first time
					FChange* Same = Changes.FindByPredicate([&W, &F](const FChange& C) { return C.Object == W.Path && C.What == F.Key; });
					if (Same)
					{
						Same->T = FMath::Min(Same->T, F.Value);
						continue;
					}
					Changes.Add({F.Value, W.Path, F.Key, FString(), false});
				}
			}
			for (const FWatchedLook& L : *LookState)
			{
				for (const FWatchedLook::FChange& F : L.Found)
				{
					Changes.Add({F.T, L.Path, F.What, F.Asset, true});
				}
			}
			Changes.Sort([](const FChange& A, const FChange& B) { return A.T < B.T; });
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FChange& C : Changes)
			{
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetField(TEXT("t"), PlaytestJson::Number(C.T));
				O->SetStringField(TEXT("object"), C.Object);
				O->SetStringField(TEXT("what"), C.What);
				if (C.bLook)
				{
					O->SetStringField(TEXT("asset"), C.Asset);
				}
				Out.Add(Obj(O));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("changes"), Out);
			if (bDrawn)
			{
				Result->SetBoolField(TEXT("drawn"), true);  // things watched as drawn (an older plugin watched their roots)
			}
			Bridge.Respond(P.Id, Obj(Result));
			return true;
		};
		Bridge.AddPending(MoveTemp(Pending));
	}
}
