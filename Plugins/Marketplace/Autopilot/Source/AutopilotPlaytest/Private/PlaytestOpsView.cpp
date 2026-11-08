// `view` (docs/protocol.md): what the player sees and what its body feels. Its `openings` and `ways_up` are in
// PlaytestOpsOpenings.cpp, its `sonar` in PlaytestOpsSonar.cpp, how things look in PlaytestOpsLook.cpp.

#include "PlaytestOps.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Info.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/UObjectIterator.h"

namespace PlaytestOps
{
	// --- view: what the player sees -------------------------------------------------------------------
	// Everything with a visible, colliding body inside the camera's frustum that a line of sight from the camera
	// reaches (center, top or nearest corner), nearest first: level geometry (floors excepted), props and
	// characters alike, with its world bounding box and its own box (turned as it is), where it is on screen and
	// how high it reaches against the player's feet; for a surface, how steep its top is, and for a slope which way it
	// rises and where its low and high ends are (each only when a line of sight from the camera reaches it). And what
	// the player's body feels: on the ground or not, what it stands on, what it is against (and so, of what is in view,
	// what stops it: nothing else is told of that, as a player learns it by walking into things), what is overhead
	// (only when that is in view, or the head is against it), its velocity.

	void View(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = Bridge.GetGameWorld();
		APlayerController* PC = PlayerController(World);
		APlayerCameraManager* CameraManager = PC ? PC->PlayerCameraManager : nullptr;
		if (!World || !CameraManager)
		{
			Bridge.RespondError(Id, TEXT("No player camera"));
			return;
		}
		double MaxDistance = 5000.0, MaxCount = 24.0, FloorSize = 4000.0, OverheadReach = 2000.0;
		Args->TryGetNumberField(TEXT("max_distance"), MaxDistance);
		Args->TryGetNumberField(TEXT("max_count"), MaxCount);
		Args->TryGetNumberField(TEXT("floor_size"), FloorSize);  // wider than this and flat: the ground, left out
		Args->TryGetNumberField(TEXT("overhead_reach"), OverheadReach);  // how far above the head something is overhead
		// Asked for: the openings the player sees round it and how far its body could go each way (`openings`), and
		// the ways up it sees (`ways_up`), within `openings_reach` (see AddOpenings). A game that never asks pays nothing
		bool bOpenings = false, bWaysUp = false;
		double OpeningsReach = 3000.0;
		Args->TryGetBoolField(TEXT("openings"), bOpenings);
		Args->TryGetBoolField(TEXT("ways_up"), bWaysUp);
		Args->TryGetNumberField(TEXT("openings_reach"), OpeningsReach);
		// Asked for: how far the body could walk each way before something stops it, and what is point blank ahead
		// (`sonar`, see AddSonar): out to `sonar_reach`, the ways `sonar_turns` (degrees right of where the view points).
		// A game that never asks pays nothing
		bool bSonar = false;
		double SonarReach = 1500.0;
		TArray<double> SonarTurns;
		Args->TryGetBoolField(TEXT("sonar"), bSonar);
		Args->TryGetNumberField(TEXT("sonar_reach"), SonarReach);
		SonarReach = FMath::Clamp(SonarReach, 50.0, 5000.0);
		const TArray<TSharedPtr<FJsonValue>>* TurnValues = nullptr;
		if (Args->TryGetArrayField(TEXT("sonar_turns"), TurnValues) && TurnValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *TurnValues)
			{
				double Turn = 0.0;
				if (Value.IsValid() && Value->TryGetNumber(Turn) && SonarTurns.Num() < SonarMaxTurns)
				{
					SonarTurns.AddUnique(FRotator::NormalizeAxis(Turn));
				}
			}
		}
		if (SonarTurns.IsEmpty())
		{
			SonarTurns = {0.0, -30.0, 30.0, -60.0, 60.0, -90.0, 90.0, -135.0, 135.0, 180.0};
		}
		// A look at these things alone (`only`: actor names), as the brain's hand looks at what it follows between
		// decisions: each listed by the same rules (drawn, in the frustum, in line of sight, within reach), with its
		// crosshair test, the middle of its body and its look, and nothing else of the scene (no other actor is looked
		// at, no top surfaces, no lights placed at it, nothing held, no body contacts): a look that costs the game next
		// to nothing, where the whole view costs it a good share of a frame
		TSet<FName> Only;
		const TArray<TSharedPtr<FJsonValue>>* OnlyNames = nullptr;
		if (Args->TryGetArrayField(TEXT("only"), OnlyNames) && OnlyNames)
		{
			for (const TSharedPtr<FJsonValue>& Name : *OnlyNames)
			{
				FString Text;
				if (Name.IsValid() && Name->TryGetString(Text) && !Text.IsEmpty())
				{
					Only.Add(FName(*Text));
				}
			}
		}
		const bool bOnly = OnlyNames != nullptr;

		const FVector Cam = CameraManager->GetCameraLocation();
		const FRotator Rot = CameraManager->GetCameraRotation();
		const FRotationMatrix Axes(Rot);
		const FVector Fwd = Axes.GetScaledAxis(EAxis::X), Right = Axes.GetScaledAxis(EAxis::Y), Up = Axes.GetScaledAxis(EAxis::Z);
		double Aspect = 16.0 / 9.0;
		if (World->GetGameViewport())
		{
			FVector2D Size;
			World->GetGameViewport()->GetViewportSize(Size);
			if (Size.X > 1 && Size.Y > 1)
			{
				Aspect = Size.X / Size.Y;
			}
		}
		const double TanH = FMath::Tan(FMath::DegreesToRadians(CameraManager->GetFOVAngle() * 0.5));
		const double TanV = TanH / Aspect;

		APawn* Pawn = PC->GetPawn();
		double Feet = Pawn ? Pawn->GetActorLocation().Z : Cam.Z;
		double BodyHeight = 180.0;  // how tall the player's body is (a person's, without a capsule to measure)
		ACharacter* Character = Cast<ACharacter>(Pawn);
		if (Character && Character->GetCapsuleComponent())
		{
			Feet -= Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			BodyHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0;
		}

		// Where a point is on screen: X right, Y up, -1..1 at the edges; false if behind the camera
		auto OnScreen = [&](const FVector& P, double& X, double& Y)
		{
			const FVector D = P - Cam;
			const double Ahead = FVector::DotProduct(D, Fwd);
			if (Ahead <= 1.0)
			{
				return false;
			}
			X = FVector::DotProduct(D, Right) / Ahead / TanH;
			Y = FVector::DotProduct(D, Up) / Ahead / TanV;
			return true;
		};

		// What a player sees as one thing: art a Blueprint spawns as a child actor (a crate's lid, a drone's rotor) is part
		// of the actor that spawned it, and goes by that one's name
		auto Whole = [](AActor* Actor)
		{
			for (int32 Depth = 0; Actor && Actor->IsChildActor() && Actor->GetParentActor() && Depth < 8; ++Depth)
			{
				Actor = Actor->GetParentActor();
			}
			return Actor;
		};
		// What the body stands on (see body)
		const UCharacterMovementComponent* Move = Character ? Character->GetCharacterMovement() : nullptr;
		AActor* StandingOn = Move && Move->IsMovingOnGround() ? Whole(Move->CurrentFloor.HitResult.GetActor()) : nullptr;

		FCollisionQueryParams Query(SCENE_QUERY_STAT(PlaytestView), false);
		if (Pawn)
		{
			Query.AddIgnoredActor(Pawn);
		}
		auto Sees = [&](const AActor* Actor, const FVector& P)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Cam, P, ECC_Visibility, Query))
			{
				return true;  // nothing in the way (the thing itself may not block the visibility channel)
			}
			return Whole(Hit.GetActor()) == Actor || (Hit.Distance >= FVector::Dist(Cam, P) - 5.0);
		};

		// What the player carries (attached to it: the gun in hand), which no sense of its own is stopped by
		TArray<AActor*> Carried;
		if (Pawn)
		{
			Pawn->GetAttachedActors(Carried, true, true);
		}
		// What the player's body is against now: its own collision shape, a hair smaller, swept TouchReach units out
		// sideways all round (as its moves are swept), stopped by what blocks it. Not the floor under it (what it
		// stands on is told apart), and nothing it doesn't touch: whether a thing stops the body is felt by walking
		// into it, not seen
		const double TouchReach = 10.0;
		TSet<const AActor*> Touched;
		TArray<TSharedPtr<FJsonValue>> Touching;
		UPrimitiveComponent* PlayerBody = Pawn ? Cast<UPrimitiveComponent>(Pawn->GetRootComponent()) : nullptr;
		// (a body that collides: what stops it is asked of its own channel and responses, for a full look or one of a few
		// things alike)
		const bool bBodyCollides = PlayerBody && PlayerBody->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
		const bool bTouchTested = !bOnly && bBodyCollides;
		if (bTouchTested)
		{
			FCollisionQueryParams Sweep(SCENE_QUERY_STAT(PlaytestTouch), false, Pawn);
			FCollisionResponseParams Responses;
			PlayerBody->InitSweepCollisionParams(Sweep, Responses);
			Sweep.AddIgnoredActors(Carried);
			const FCollisionShape BodyShape = PlayerBody->GetCollisionShape(-1.0f);
			const FVector From = PlayerBody->GetComponentLocation();
			// A capsule, box or sphere root gives its own shape, turned as the body is; any other root (a vehicle's
			// or a hover-bike's mesh) gives a box from its bounds, which are already lined up with the world:
			// turning that box with the body would make it reach past the body
			const bool bOwnShape = PlayerBody->IsA(UShapeComponent::StaticClass());
			const FQuat Turned = bOwnShape ? PlayerBody->GetComponentQuat() : FQuat::Identity;
			// Each contact with the way the body felt it most head-on: that sweep's way (`yaw`, and `turn` from the
			// view's) and the face's normal there (`normal`), so a wall's run is told apart from the way into it
			TMap<const AActor*, TPair<TSharedRef<FJsonObject>, double>> Felt;
			const double ViewYaw = CameraManager->GetCameraRotation().Yaw;
			for (int32 i = 0; i < 8; ++i)
			{
				const double Angle = i * UE_DOUBLE_PI / 4.0;
				const FVector Way(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
				const FVector To = From + Way * (TouchReach + 1.0);
				FHitResult Hit;
				if (World->SweepSingleByChannel(Hit, From, To, Turned, PlayerBody->GetCollisionObjectType(), BodyShape,
						Sweep, Responses)
					&& Hit.bBlockingHit && Hit.GetActor() && Hit.ImpactNormal.Z < 0.7)
				{
					AActor* Thing = Whole(Hit.GetActor());
					const FVector Face = FVector(Hit.ImpactNormal.X, Hit.ImpactNormal.Y, 0.0).GetSafeNormal();
					const double HeadOn = -FVector::DotProduct(Way, Face);
					TPair<TSharedRef<FJsonObject>, double>* Kept = Felt.Find(Thing);
					if (!Kept)
					{
						Touched.Add(Thing);
						TSharedRef<FJsonObject> Contact = MakeShared<FJsonObject>();
						Contact->SetStringField(TEXT("thing"), Thing->GetName());
#if WITH_EDITOR
						Contact->SetStringField(TEXT("label"), Thing->GetActorLabel());
#endif
						Touching.Add(MakeShared<FJsonValueObject>(Contact));
						Kept = &Felt.Add(Thing, TPair<TSharedRef<FJsonObject>, double>(Contact, -2.0));
					}
					if (HeadOn > Kept->Value)
					{
						Kept->Value = HeadOn;
						const double WayYaw = FMath::RadiansToDegrees(Angle);
						Kept->Key->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(FRotator::NormalizeAxis(WayYaw)));
						Kept->Key->SetNumberField(TEXT("turn"), FMath::RoundToDouble(FRotator::NormalizeAxis(WayYaw - ViewYaw)));
						TSharedRef<FJsonObject> Normal = MakeShared<FJsonObject>();
						Normal->SetNumberField(TEXT("X"), FMath::RoundToDouble(Hit.ImpactNormal.X * 100.0) / 100.0);
						Normal->SetNumberField(TEXT("Y"), FMath::RoundToDouble(Hit.ImpactNormal.Y * 100.0) / 100.0);
						Normal->SetNumberField(TEXT("Z"), FMath::RoundToDouble(Hit.ImpactNormal.Z * 100.0) / 100.0);
						Kept->Key->SetObjectField(TEXT("normal"), Normal);
					}
				}
			}
		}
		// Owned by the player, or by what it owns (a sight on the gun in hand)
		auto OwnedByPlayer = [Pawn, PC](const AActor* Actor)
		{
			int32 Depth = 0;
			for (const AActor* Owner = Actor ? Actor->GetOwner() : nullptr; Owner && Depth < 8; Owner = Owner->GetOwner(), ++Depth)
			{
				if (Owner == Pawn || Owner == PC)
				{
					return true;
				}
			}
			return false;
		};

		struct FSeen
		{
			AActor* Actor;
			FVector Origin, Extent;
			double Distance, X, Y;
			FPlaytestShape Shape;
			bool bOwned;
		};
		TArray<FSeen> Seen;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (bOnly && (!Actor || !Only.Contains(Actor->GetFName())))
			{
				continue;  // (a look at some things alone: see Only)
			}
			if (!Actor || Actor == Pawn || Actor->IsHidden() || (Pawn && Actor->IsAttachedTo(Pawn))
				|| Actor->IsA(AInfo::StaticClass()) || Actor->IsChildActor())
			{
				// the player, what is attached to it, the game's bookkeeping (world settings, game state), and art
				// spawned as a child actor (seen as part of what spawned it, see Whole)
				continue;
			}
			const bool bOwned = OwnedByPlayer(Actor);
			if (bOwned)
			{
				// The player's own things while it carries them (held, or fixed onto what it holds); once let go (a
				// thrown object, a dropped weapon, its own shot) they are out in the level, and say they are its own
				const AActor* Parent = Actor->GetAttachParentActor();
				if (Parent && (Parent == Pawn || Parent == PC || Parent == CameraManager || OwnedByPlayer(Parent)))
				{
					continue;
				}
			}
			// Its bounds from the parts a player sees, its child actors' included (see BodyBounds); around it in the world
			// and in its own frame
			FBox Visible, Local;
			BodyBounds(Actor, Visible, Local);
			if (!Visible.IsValid)
			{
				continue;
			}
			FVector Origin, Extent;
			Visible.GetCenterAndExtents(Origin, Extent);
			if (Extent.IsNearlyZero(1.0) || Extent.GetMax() > 50000.0
				|| (FMath::Max(Extent.X, Extent.Y) * 2.0 > FloorSize && Extent.Z * 2.0 < FloorSize * 0.25))
			{
				continue;  // nothing to see, something unbounded, or the ground itself
			}
			// How far it is: to its own box (under a tipped slab's raised end is not inside it)
			const FPlaytestShape Shape = ShapeOf(Actor, Local, Origin, Extent);
			const double Distance = Shape.Distance(Cam);
			if (Distance <= 0.0 && !Actor->IsA(APawn::StaticClass()))
			{
				continue;  // a box the camera is inside: nothing seen from outside (a volume, a dome around the level)
			}
			if (Distance > MaxDistance)
			{
				continue;
			}
			// In the frustum: its center, its top or one of its own box's corners on screen
			const int32 UpI = Shape.UpIndex();
			TArray<FVector> Points = {Shape.Center, Shape.Center + Shape.TopNormal() * (Shape.Half[UpI] * 0.9)};
			for (int32 i = 0; i < 8; ++i)
			{
				Points.Add(Shape.Center + Shape.Axis[0] * (Shape.Half.X * (i & 1 ? 0.95 : -0.95))
					+ Shape.Axis[1] * (Shape.Half.Y * (i & 2 ? 0.95 : -0.95))
					+ Shape.Axis[2] * (Shape.Half.Z * (i & 4 ? 0.95 : -0.95)));
			}
			const FVector* Inside = nullptr;
			double CX = 0.0, CY = 0.0;
			for (const FVector& P : Points)
			{
				double X, Y;
				if (OnScreen(P, X, Y) && FMath::Abs(X) <= 1.0 && FMath::Abs(Y) <= 1.0)
				{
					Inside = &P;
					break;
				}
			}
			if (!Inside)
			{
				continue;
			}
			// In sight: a line from the camera reaches its center, its top, or the corner found on screen
			if (!Sees(Actor, Shape.Center) && !Sees(Actor, Points[1]) && !Sees(Actor, *Inside))
			{
				continue;
			}
			if (!OnScreen(Shape.Center, CX, CY))
			{
				OnScreen(*Inside, CX, CY);
			}
			Seen.Add({Actor, Origin, Extent, Distance, CX, CY, Shape, bOwned});
		}
		Seen.Sort([](const FSeen& A, const FSeen& B) { return A.Distance < B.Distance; });

		// A place on a thing: where it is, how far to turn the view toward it (degrees, right positive), how far it is
		// from the player across the floor, and how high it is against the player's feet
		const FVector Me = Pawn ? Pawn->GetActorLocation() : Cam;
		auto Place = [&](const FVector& P)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetObjectField(TEXT("at"), Vec(P));
			const double Bearing = FMath::RadiansToDegrees(FMath::Atan2(P.Y - Cam.Y, P.X - Cam.X));
			O->SetNumberField(TEXT("turn"), FMath::RoundToDouble(FRotator::NormalizeAxis(Bearing - Rot.Yaw)));
			O->SetNumberField(TEXT("distance"), FMath::RoundToDouble(FVector::Dist2D(P, Me)));
			O->SetNumberField(TEXT("height_above_feet"), FMath::RoundToDouble(P.Z - Feet));
			return O;
		};
		// Its top surface: the face of its own box turned most upward, and how high the surface really is near each
		// of that face's four edges, straight down onto its own shapes (a wedge-shaped mesh and a tipped box alike;
		// the face itself where none is hit). Along the way it rises most: how steep it is (0 for a wall stood on its
		// side), and for a slope a player could walk up its low end (where its walkable surface meets the floor below
		// it: where it comes up out of that floor, or out of a wall its foot runs into), its high end (its top edge, or
		// where it comes out from under what it runs up into) and which way is uphill. Each end only where a line of
		// sight from the camera reaches it (see Surface): an end behind the slab itself (from under it) or behind
		// something else isn't seen, and isn't told
		const double SlopeMin = 5.0, SlopeMax = 50.0;
		const double SlopeEndInset = 15.0;  // units in from a top face's edge where its height is read (see Surface)
		// Where a slope's surface is open, as a body would stand on it (see Surface): the room a body takes over it, from
		// SlopeEndClear over the surface (the floor it meets lies at the surface, and isn't in that room) up to the body's
		// height, SlopeEndProbe units either side of the place, clear of everything that stops the body but the slope
		// itself and characters (a bot standing at its foot doesn't move where the slope begins). Found along the slope a
		// stride at a time (SlopeEndStride units across the floor, at most SlopeEndStrides strides: longer on a long one),
		// then between the last stride covered and the first open by halves, SlopeEndHalvings times
		const double SlopeEndClear = 2.0, SlopeEndProbe = 5.0, SlopeEndStride = 20.0;
		const int32 SlopeEndStrides = 128, SlopeEndHalvings = 5;
		// A slope's lane (see Surface): across it at these shares of the way up it, a body's room every SlopeLaneStep units
		// (at most SlopeLaneSamples places), each end of a run found to an eighth of a step (SlopeLaneHalvings halvings);
		// a drop past a side measured down to SlopeLaneDeep units at most, the floor below its foot to SlopeLaneBelow
		const double SlopeLaneStations[] = {0.25, 0.5, 0.75};
		const double SlopeLaneStep = 20.0, SlopeLaneDeep = 3000.0, SlopeLaneBelow = 1000.0;
		const int32 SlopeLaneSamples = 64, SlopeLaneHalvings = 3;
		FCollisionQueryParams Sight = Query;
		Sight.AddIgnoredActors(Carried);
		// Whether a place on a thing's top is in view: on screen (not behind the camera or off its edges), and a line
		// of sight from the camera reaches it, nothing in the way short of it (the thing itself included: its
		// underside hides its top from below). An end out of view is what the agent remembers of it, if anything
		auto SeesPoint = [&](const FVector& P)
		{
			const FVector Above = P + FVector(0.0, 0.0, 5.0);  // (just above the surface, which doesn't hide itself)
			double SX = 0.0, SY = 0.0;
			if (!OnScreen(Above, SX, SY) || FMath::Abs(SX) > 1.0 || FMath::Abs(SY) > 1.0)
			{
				return false;
			}
			FHitResult Hit;
			return !World->LineTraceSingleByChannel(Hit, Cam, Above, ECC_Visibility, Sight)
				|| Hit.Distance >= FVector::Dist(Cam, Above) - 10.0;
		};
		FCollisionQueryParams Down(SCENE_QUERY_STAT(PlaytestSurface), true);
		auto Surface = [&](FJsonObject& Item, const FSeen& S)
		{
			const FPlaytestShape& B = S.Shape;
			const int32 K = B.UpIndex();
			const FVector Top = B.Center + B.TopNormal() * B.Half[K];
			const double Above = S.Origin.Z + S.Extent.Z + 50.0, Below = S.Origin.Z - S.Extent.Z - 50.0;
			TInlineComponentArray<UPrimitiveComponent*> Shapes(S.Actor, true);  // (its child actors' too)
			auto HeightAt = [&](const FVector& P)
			{
				double Height = P.Z;
				bool bHit = false;
				for (UPrimitiveComponent* Shape : Shapes)
				{
					if (!Shape || !Shape->IsRegistered() || !Shape->IsVisible() || Shape->bHiddenInGame
						|| Shape->IsA(UFXSystemComponent::StaticClass()))
					{
						continue;
					}
					FHitResult Hit;
					if (Shape->LineTraceComponent(Hit, FVector(P.X, P.Y, Above), FVector(P.X, P.Y, Below), Down)
						&& (!bHit || Hit.ImpactPoint.Z > Height))
					{
						Height = Hit.ImpactPoint.Z;
						bHit = true;
					}
				}
				return Height;
			};
			bool bFound = false;
			double Steepest = 0.0;
			int32 Along = 0;  // the axis of its own box it rises along
			FVector LowEnd = Top, HighEnd = Top;
			for (int32 i = 0; i < 3; ++i)
			{
				if (i == K)
				{
					continue;
				}
				// Each end at the top face's own edge, a small fixed inset in (so the trace still lands on the surface,
				// never more than a quarter of the way in on a small thing): a share of the length would put a long
				// slope's high end well inside and below where it tops out
				const double Reach = FMath::Max(0.0, B.Half[i] - FMath::Min(SlopeEndInset, B.Half[i] * 0.25));
				FVector One = Top + B.Axis[i] * Reach, Other = Top - B.Axis[i] * Reach;
				One.Z = HeightAt(One);
				Other.Z = HeightAt(Other);
				const double Run = FVector::Dist2D(One, Other);
				if (Run < 1.0)
				{
					continue;
				}
				const double Slope = FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(One.Z - Other.Z), Run));
				if (!bFound || Slope > Steepest)
				{
					bFound = true;
					Steepest = Slope;
					Along = i;
					LowEnd = One.Z <= Other.Z ? One : Other;
					HighEnd = One.Z <= Other.Z ? Other : One;
				}
			}
			const FRotator Turned = S.Actor->GetActorRotation();
			const bool bTipped = FMath::Abs(Turned.Pitch) >= 1.0 || FMath::Abs(Turned.Roll) >= 1.0;
			if (!bFound || (Steepest < 1.0 && !bTipped))
			{
				return;  // level on top: nothing to tell
			}
			Item.SetNumberField(TEXT("top_slope_degrees"), FMath::RoundToDouble(Steepest));
			if (Steepest < SlopeMin || Steepest > SlopeMax || HighEnd.Z - LowEnd.Z < 10.0)
			{
				return;
			}
			// Each end along a line up the slope from its low edge of the top face to its high edge: the middle line
			// first, then a third and two thirds of the way out to either side, the first that has the end open and in
			// view (SeesPoint). The low end is the first place up from the low edge where the surface is open (see
			// SlopeEndClear: where it comes up out of the floor below it, or out of a wall its foot is sunk into, not
			// where its buried edge is), the high end the first down from the high edge (its top edge, or where it comes
			// out from under what it runs up into). Wherever the player stands: neither moves with it
			const int32 Across = 3 - K - Along;
			FCollisionQueryParams Beside(SCENE_QUERY_STAT(PlaytestSlopeEnd), false, Pawn);
			FCollisionResponseParams BodyStops;
			ECollisionChannel BodyChannel = ECC_Visibility;  // (no body to ask: what stops a line of sight)
			if (bBodyCollides)
			{
				PlayerBody->InitSweepCollisionParams(Beside, BodyStops);
				BodyChannel = PlayerBody->GetCollisionObjectType();
			}
			TArray<AActor*> Children;
			S.Actor->GetAllChildActors(Children, true);
			Beside.AddIgnoredActors(Carried);
			Beside.AddIgnoredActor(S.Actor);
			Beside.AddIgnoredActors(Children);
			// (a character, or something it holds or wears)
			auto OfCharacter = [&](AActor* A)
			{
				for (int32 Depth = 0; A && Depth < 8; ++Depth)
				{
					if (A->IsA(APawn::StaticClass()))
					{
						return true;
					}
					A = A->GetAttachParentActor() ? A->GetAttachParentActor() : A->GetOwner();
				}
				return false;
			};
			const double Room = FMath::Max(1.0, (BodyHeight - SlopeEndClear) * 0.5);
			const FCollisionShape Column = FCollisionShape::MakeBox(FVector(SlopeEndProbe, SlopeEndProbe, Room));
			// (drawn, or part of a thing with a visible body, as a wall whose collision is a hidden box: not a thing
			// nothing of which is drawn, as an invisible blocking volume, which the player doesn't see the slope meet)
			auto Shows = [&](const FOverlapResult& O)
			{
				if (Drawn(O.GetComponent()))
				{
					return true;
				}
				const AActor* A = O.GetActor();
				if (!A)
				{
					return false;
				}
				TInlineComponentArray<UPrimitiveComponent*> Parts(A, true);
				return Parts.ContainsByPredicate([](const UPrimitiveComponent* Part) { return IsBodyPart(Part); });
			};
			auto Open = [&](const FVector& P)
			{
				TArray<FOverlapResult> Over;
				World->OverlapMultiByChannel(Over, P + FVector(0.0, 0.0, SlopeEndClear + Room), FQuat::Identity,
					BodyChannel, Column, Beside, BodyStops);
				for (const FOverlapResult& O : Over)
				{
					if (O.bBlockingHit && Shows(O) && !OfCharacter(O.GetActor()))
					{
						return false;
					}
				}
				return true;
			};
			// The first open place on the slope from `From` (a place on one of its edges) toward `To` (that place's
			// fellow on the other edge), on its surface; false when none is
			auto FirstOpen = [&](const FVector& From, const FVector& To, FVector& Out)
			{
				auto At = [&](double T)
				{
					FVector P = FMath::Lerp(From, To, T);
					P.Z = HeightAt(P);
					return P;
				};
				const int32 Strides = FMath::Clamp(FMath::CeilToInt(FVector::Dist2D(From, To) / SlopeEndStride), 1,
					SlopeEndStrides);
				double Covered = 0.0;
				for (int32 i = 0; i <= Strides; ++i)
				{
					const double T = double(i) / Strides;
					if (!Open(At(T)))
					{
						Covered = T;
						continue;
					}
					double Hi = T;
					for (int32 h = 0; i > 0 && h < SlopeEndHalvings; ++h)
					{
						const double Mid = (Covered + Hi) * 0.5;
						(Open(At(Mid)) ? Hi : Covered) = Mid;
					}
					Out = At(Hi);
					return true;
				}
				return false;
			};
			auto EndAt = [&](const FVector& Edge, const FVector& Other, FVector& Out)
			{
				const double Shares[] = {0.0, 1.0 / 3.0, -1.0 / 3.0, 2.0 / 3.0, -2.0 / 3.0};
				for (const double Share : Shares)
				{
					const FVector Side = B.Axis[Across] * (B.Half[Across] * Share);
					FVector P;
					if (FirstOpen(Edge + Side, Other + Side, P) && SeesPoint(P))
					{
						Out = P;
						return true;
					}
				}
				return false;
			};
			FVector End = Top;
			if (EndAt(LowEnd, HighEnd, End))
			{
				Item.SetObjectField(TEXT("low_end"), Place(End));
			}
			if (EndAt(HighEnd, LowEnd, End))
			{
				Item.SetObjectField(TEXT("high_end"), Place(End));
			}
			// The lane a body walks up it (`lane`): across the slope at SlopeLaneStations of the way from its low edge to
			// its high edge, a body's room over its surface (see Open) every SlopeLaneStep units from one side of its top
			// face to the other (at most SlopeLaneSamples places); the longest run of open places in a row is where a
			// body walks there, each end of it found to within an eighth of a step. Its `width` is the narrowest run;
			// its middle line runs the way it rises through the middles of the runs (their mean offset across), with its
			// `foot` and `top` on that line (the first open place up from its low edge and down from its high edge along
			// it, see FirstOpen: places, each told only where seen, SeesPoint) and `middle`, its place halfway up. Past
			// each side (`left`, `right`, facing up it): a `wall` where a run stopped short of the face's edge, or a
			// body's room just past the edge isn't clear (something stands there); else the floor there, straight down:
			// a `drop` where it lies more than a step below the surface (`depth`: the deepest of the places, at most
			// SlopeLaneDeep), else `open` (floor about as high beside it). `below`: how far a body walks on from its foot
			// down its middle line, on the floor (`clear`), before something stops it (`wall`), the floor falls more
			// than a step (`drop`), a place on the way isn't seen (`unseen`: the floor beyond it isn't told) or
			// SlopeLaneBelow units (`reach`); told only with its foot, where that is seen. Only when a place on its
			// middle line is seen
			{
				const double StepUp = Move ? Move->MaxStepHeight : BodyHeight * 0.25;  // (a person's knee: a step)
				const double BodyRadius = Character && Character->GetCapsuleComponent()
					? Character->GetCapsuleComponent()->GetScaledCapsuleRadius() : BodyHeight * 0.2;
				const FVector Up2 = FVector(HighEnd.X - LowEnd.X, HighEnd.Y - LowEnd.Y, 0.0).GetSafeNormal();
				const FVector Right2(-Up2.Y, Up2.X, 0.0);
				// (the side the across axis points to, facing up it: +1 right)
				const double RightSign = FVector::DotProduct(B.Axis[Across], Right2) >= 0.0 ? 1.0 : -1.0;
				const double Edge = FMath::Max(0.0, B.Half[Across] - FMath::Min(SlopeEndInset, B.Half[Across] * 0.25));
				const int32 Samples = FMath::Clamp(FMath::CeilToInt(2.0 * Edge / SlopeLaneStep), 2, SlopeLaneSamples);
				struct FLaneSide
				{
					int32 By = 0;  // 0 open, 1 a wall, 2 a drop
					double Depth = 0.0;
				};
				// (the worse of two: a drop deeper than a step over a wall over open floor; of two drops, the deeper)
				auto Worse = [](FLaneSide& Kept, const FLaneSide& Here)
				{
					if ((Here.By == 2 && (Kept.By != 2 || Here.Depth > Kept.Depth)) || (Here.By == 1 && Kept.By == 0))
					{
						Kept = Here;
					}
				};
				// What lies past the top face's edge at the place Middle + across * Q (Sign: which way out)
				auto Past = [&](const FVector& Middle, double Q, double Sign)
				{
					FLaneSide Side;
					FVector P = Middle + B.Axis[Across] * Q;
					const double Z = HeightAt(P);
					P = Middle + B.Axis[Across] * (Sign * (B.Half[Across] + BodyRadius));
					P.Z = Z;
					if (!Open(P))
					{
						Side.By = 1;
						return Side;
					}
					FHitResult Floor;
					const FVector Start(P.X, P.Y, Z + StepUp);
					if (World->LineTraceSingleByChannel(Floor, Start, Start - FVector(0.0, 0.0, StepUp + SlopeLaneDeep),
							BodyChannel, Beside, BodyStops) && Floor.bBlockingHit)
					{
						const double Fall = Z - Floor.ImpactPoint.Z;
						if (Fall > StepUp)
						{
							Side.By = 2;
							Side.Depth = Fall;
						}
					}
					else
					{
						Side.By = 2;
						Side.Depth = SlopeLaneDeep;
					}
					return Side;
				};
				FLaneSide Sides[2];  // past the +across edge, past the -across edge
				double Narrowest = 0.0, Offsets = 0.0;
				int32 Runs = 0;
				bool bSeenLane = false;
				for (const double T : SlopeLaneStations)
				{
					const FVector Middle = FMath::Lerp(LowEnd, HighEnd, T);
					auto AtQ = [&](double Q)
					{
						FVector P = Middle + B.Axis[Across] * Q;
						P.Z = HeightAt(P);
						return P;
					};
					auto QOf = [&](int32 i) { return -Edge + 2.0 * Edge * i / Samples; };
					TArray<bool> Free;
					Free.SetNum(Samples + 1);
					for (int32 i = 0; i <= Samples; ++i)
					{
						Free[i] = Open(AtQ(QOf(i)));
					}
					int32 RunFrom = INDEX_NONE, RunTo = INDEX_NONE;
					for (int32 i = 0; i <= Samples;)
					{
						if (!Free[i])
						{
							++i;
							continue;
						}
						int32 j = i;
						while (j < Samples && Free[j + 1])
						{
							++j;
						}
						if (RunFrom == INDEX_NONE || j - i > RunTo - RunFrom)
						{
							RunFrom = i;
							RunTo = j;
						}
						i = j + 1;
					}
					if (RunFrom == INDEX_NONE)
					{
						continue;
					}
					// (between the last place open and the first covered, by halves)
					auto Halve = [&](double OpenQ, double CoveredQ)
					{
						for (int32 h = 0; h < SlopeLaneHalvings; ++h)
						{
							const double Mid = (OpenQ + CoveredQ) * 0.5;
							(Open(AtQ(Mid)) ? OpenQ : CoveredQ) = Mid;
						}
						return OpenQ;
					};
					const double Lo = RunFrom > 0 ? Halve(QOf(RunFrom), QOf(RunFrom - 1)) : -Edge;
					const double Hi = RunTo < Samples ? Halve(QOf(RunTo), QOf(RunTo + 1)) : Edge;
					Narrowest = Runs == 0 ? Hi - Lo : FMath::Min(Narrowest, Hi - Lo);
					Offsets += (Lo + Hi) * 0.5;
					++Runs;
					FLaneSide Wall;
					Wall.By = 1;
					Worse(Sides[0], RunTo < Samples ? Wall : Past(Middle, Edge, 1.0));
					Worse(Sides[1], RunFrom > 0 ? Wall : Past(Middle, -Edge, -1.0));
					bSeenLane = bSeenLane || SeesPoint(AtQ((Lo + Hi) * 0.5));
				}
				if (Runs > 0 && bSeenLane)
				{
					const FVector Shift = B.Axis[Across] * (Offsets / Runs);
					TSharedRef<FJsonObject> Lane = MakeShared<FJsonObject>();
					Lane->SetNumberField(TEXT("width"), FMath::RoundToDouble(Narrowest));
					FVector Halfway = FMath::Lerp(LowEnd, HighEnd, 0.5) + Shift;
					Halfway.Z = HeightAt(Halfway);
					Lane->SetObjectField(TEXT("middle"), Vec(Halfway));
					FVector Foot = LowEnd + Shift, TopP = HighEnd + Shift;
					const bool bFoot = FirstOpen(LowEnd + Shift, HighEnd + Shift, Foot) && SeesPoint(Foot);
					if (bFoot)
					{
						Lane->SetObjectField(TEXT("foot"), Place(Foot));
					}
					if (FirstOpen(HighEnd + Shift, LowEnd + Shift, TopP) && SeesPoint(TopP))
					{
						Lane->SetObjectField(TEXT("top"), Place(TopP));
					}
					auto SideOf = [](const FLaneSide& Kind)
					{
						TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
						O->SetStringField(TEXT("by"), Kind.By == 2 ? TEXT("drop") : Kind.By == 1 ? TEXT("wall") : TEXT("open"));
						if (Kind.By == 2)
						{
							O->SetNumberField(TEXT("depth"), FMath::RoundToDouble(Kind.Depth));
						}
						return O;
					};
					Lane->SetObjectField(TEXT("right"), SideOf(RightSign > 0.0 ? Sides[0] : Sides[1]));
					Lane->SetObjectField(TEXT("left"), SideOf(RightSign > 0.0 ? Sides[1] : Sides[0]));
					// From its foot (told: seen) down its middle line, on the floor: how far a body walks on, as far as the
					// eyes see the floor go on (a place not seen ends it: `unseen`)
					if (bFoot)
					{
						double Z = Foot.Z, Clear = SlopeLaneBelow;
						const TCHAR* By = TEXT("reach");
						for (double D = SlopeLaneStep; D <= SlopeLaneBelow; D += SlopeLaneStep)
						{
							FVector P = Foot - Up2 * D;
							FHitResult Floor;
							const FVector Start(P.X, P.Y, Z + StepUp);
							if (!World->LineTraceSingleByChannel(Floor, Start, Start - FVector(0.0, 0.0, 2.0 * StepUp + 1.0),
									BodyChannel, Beside, BodyStops) || !Floor.bBlockingHit)
							{
								// (no floor within a step: a drop, unless something stands there that the line started in; where
								// the floor ends seen: the eyes reach over the edge)
								P.Z = Z;
								Clear = D - SlopeLaneStep;
								By = !Open(P) ? TEXT("wall") : SeesPoint(P) ? TEXT("drop") : TEXT("unseen");
								break;
							}
							P.Z = Floor.ImpactPoint.Z;
							if (!Open(P))
							{
								Clear = D - SlopeLaneStep;
								By = TEXT("wall");
								break;
							}
							if (!SeesPoint(P))
							{
								Clear = D - SlopeLaneStep;
								By = TEXT("unseen");
								break;
							}
							Z = P.Z;
						}
						TSharedRef<FJsonObject> UnderFoot = MakeShared<FJsonObject>();
						UnderFoot->SetNumberField(TEXT("clear"), FMath::RoundToDouble(Clear));
						UnderFoot->SetStringField(TEXT("by"), By);
						Lane->SetObjectField(TEXT("below"), UnderFoot);
					}
					Item.SetObjectField(TEXT("lane"), Lane);
				}
			}
			TSharedRef<FJsonObject> Uphill = MakeShared<FJsonObject>();
			const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(HighEnd.Y - LowEnd.Y, HighEnd.X - LowEnd.X));
			Uphill->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Yaw));
			Uphill->SetNumberField(TEXT("turn"), FMath::RoundToDouble(FRotator::NormalizeAxis(Yaw - Rot.Yaw)));
			Item.SetObjectField(TEXT("uphill"), Uphill);
		};

		// The lights helpers draw at things' places (see PlacedLights), each counted for the listed thing it is at: whose
		// own box, grown a little (see FPlaytestShape::Holds), holds it; of several, the one whose box is nearest it
		const int32 ListedCount = FMath::Min(Seen.Num(), FMath::Max(0, (int32)MaxCount));
		TArray<FDrawn> PlacedAt;  // per listed thing, the placed lights at it
		PlacedAt.SetNum(ListedCount);
		for (const FPlacedLight& Light : bOnly ? TArray<FPlacedLight>() : PlacedLights(World, Pawn, PC))
		{
			int32 Best = INDEX_NONE;
			double Nearest = 0.0;
			for (int32 i = 0; i < ListedCount; ++i)
			{
				const double Off = Seen[i].Shape.Distance(Light.At);
				if (Seen[i].Shape.Holds(Light.At, PlacedLightReach) && (Best == INDEX_NONE || Off < Nearest))
				{
					Best = i;
					Nearest = Off;
				}
			}
			if (Best != INDEX_NONE)
			{
				AddLight(PlacedAt[Best], Light.Brightness);
			}
		}

		TArray<TSharedPtr<FJsonValue>> List;
		TSet<const AActor*> Listed;  // what the player is told it sees
		for (int32 Index = 0; Index < Seen.Num(); ++Index)
		{
			const FSeen& S = Seen[Index];
			if (List.Num() >= (int32)MaxCount)
			{
				break;
			}
			Listed.Add(S.Actor);
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), S.Actor->GetName());
#if WITH_EDITOR
			if (!S.Actor->GetActorLabel().IsEmpty() && S.Actor->GetActorLabel() != S.Actor->GetName())
			{
				Item->SetStringField(TEXT("label"), S.Actor->GetActorLabel());  // its name in the editor's outliner
			}
#endif
			Item->SetStringField(TEXT("path"), PathOf(S.Actor));
			Item->SetStringField(TEXT("class"), S.Actor->GetClass()->GetName());
			if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(S.Actor->GetRootComponent()))
			{
				if (Mesh->GetStaticMesh())
				{
					Item->SetStringField(TEXT("mesh"), Mesh->GetStaticMesh()->GetName());  // e.g. SM_Ramp, Cube
				}
			}
			if (FMath::Abs(S.X) > 1.0 || FMath::Abs(S.Y) > 1.0)
			{
				Item->SetBoolField(TEXT("partly_in_view"), true);  // its middle is off screen: big, or at the edge
			}
			Item->SetObjectField(TEXT("center"), Vec(S.Origin));
			Item->SetObjectField(TEXT("size"), Vec(S.Extent, 2.0));  // the bounding box's full width, depth, height
			const FRotator Tilt = S.Actor->GetActorRotation();
			if (FMath::Abs(Tilt.Pitch) >= 1.0 || FMath::Abs(Tilt.Roll) >= 1.0)
			{
				TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
				T->SetNumberField(TEXT("pitch"), FMath::RoundToDouble(Tilt.Pitch));
				T->SetNumberField(TEXT("roll"), FMath::RoundToDouble(Tilt.Roll));
				Item->SetObjectField(TEXT("tilt"), T);  // e.g. a ramp: a box tipped over
			}
			Item->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Tilt.Yaw));
			// Its own box, when that isn't the box around it (it is turned): its middle, its size along its own
			// axes, and how it is turned (its axes are the rotation's)
			if (!S.Shape.Center.Equals(S.Origin, 1.0) || !S.Shape.Half.Equals(S.Extent, 0.5))
			{
				TSharedRef<FJsonObject> Own = MakeShared<FJsonObject>();
				Own->SetObjectField(TEXT("center"), Vec(S.Shape.Center));
				Own->SetObjectField(TEXT("size"), Vec(S.Shape.Half, 2.0));
				TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
				Rotation->SetNumberField(TEXT("pitch"), FMath::RoundToDouble(Tilt.Pitch * 10.0) / 10.0);
				Rotation->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Tilt.Yaw * 10.0) / 10.0);
				Rotation->SetNumberField(TEXT("roll"), FMath::RoundToDouble(Tilt.Roll * 10.0) / 10.0);
				Own->SetObjectField(TEXT("rotation"), Rotation);
				Item->SetObjectField(TEXT("box"), Own);
			}
			if (Touched.Contains(S.Actor))
			{
				Item->SetBoolField(TEXT("blocks_player"), true);  // the player's body is against it: it stops the body
			}
			if (S.bOwned)
			{
				Item->SetBoolField(TEXT("owned_by_player"), true);  // the player's own, let go of (thrown, dropped, shot)
			}
			const double Speed = S.Actor->GetVelocity().Size();
			if (Speed >= 10.0)
			{
				Item->SetNumberField(TEXT("speed"), FMath::RoundToDouble(Speed));  // moving, this many units a second
			}
			Item->SetNumberField(TEXT("distance"), FMath::RoundToDouble(S.Distance));
			Item->SetNumberField(TEXT("screen_x"), FMath::RoundToDouble(S.X * 100.0) / 100.0);
			Item->SetNumberField(TEXT("screen_y"), FMath::RoundToDouble(S.Y * 100.0) / 100.0);
			Item->SetNumberField(TEXT("top_above_feet"), FMath::RoundToDouble(S.Origin.Z + S.Extent.Z - Feet));
			Item->SetNumberField(TEXT("bottom_above_feet"), FMath::RoundToDouble(S.Origin.Z - S.Extent.Z - Feet));
			if (!bOnly && !S.Actor->IsA(APawn::StaticClass()))
			{
				Surface(*Item, S);
			}
			// Whether the middle of the view (the crosshair) is on it, and on which part: the view's center line
			// against its own shapes (a character's physics bodies, a mesh; its child actors' too, but not a
			// character's: what it holds isn't its body), whatever collision channels the game uses, and not hidden
			// behind something else there. What a player sees of the crosshair over a thing
			{
				const FVector RayEnd = Cam + Fwd * (S.Distance + S.Extent.Size() * 2.0 + 100.0);
				FCollisionQueryParams Ray(SCENE_QUERY_STAT(PlaytestCrosshair), true);
				FHitResult Best;
				bool bOn = false, bAnyCollision = false;
				const bool bPawn = S.Actor->IsA(APawn::StaticClass());
				// Whether a shape stops anything (a body), not only overlaps (a trigger or a volume: the area round a
				// thing a game watches for whoever comes near, wider than what shows of it)
				auto Blocks = [](const UPrimitiveComponent& Part)
				{
					for (const uint8 Response : Part.GetCollisionResponseToChannels().EnumArray)
					{
						if ((ECollisionResponse)Response == ECR_Block)
						{
							return true;
						}
					}
					return false;
				};
				TInlineComponentArray<UPrimitiveComponent*> Shapes(S.Actor, !bPawn);
				for (int32 Pass = 0; Pass < 3 && !bOn; ++Pass)
				{
					// The parts a player sees first; only when none of them has a shape to test, the hidden ones that
					// stop something (e.g. a character whose mesh has no collision: its capsule; a crate drawn without
					// collision, its hidden box); and only when it has none of those either, a thing's hidden trigger or
					// volume (a pickup's sphere: the agent counts it only over what shows of the thing)
					if (Pass > 0 && bAnyCollision)
					{
						break;
					}
					for (UPrimitiveComponent* Shape : Shapes)
					{
						if (!Shape || !Shape->IsRegistered())
						{
							continue;
						}
						const bool bShown = Shape->IsVisible() && !Shape->bHiddenInGame;
						if ((bShown ? 0 : ((bPawn || Blocks(*Shape)) ? 1 : 2)) != Pass)
						{
							continue;
						}
						if (Shape->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
						{
							bAnyCollision = true;
						}
						FHitResult Hit;
						if (Shape->LineTraceComponent(Hit, Cam, RayEnd, Ray) && (!bOn || Hit.Distance < Best.Distance))
						{
							Best = Hit;
							bOn = true;
						}
					}
				}
				// The middle of its body as seen from here, for a character or creature (a skinned mesh): rays through
				// a grid over its box against its own shapes, and the middle of where they hit. Its box's center is
				// often off its body (a floating name tag, an arm held out), so aiming there misses
				if (bAnyCollision && Shapes.ContainsByPredicate([](const UPrimitiveComponent* Part)
						{ return Part && Part->IsA(USkinnedMeshComponent::StaticClass()); }))
				{
					const int32 N = 7;
					const double HalfW = FMath::Max(S.Extent.X, S.Extent.Y);
					FVector Sum = FVector::ZeroVector;
					int32 Hits = 0, Shown = 0;
					for (int32 i = 0; i < N; ++i)
					{
						for (int32 j = 0; j < N; ++j)
						{
							const FVector Through = S.Origin + Right * (((double)i / (N - 1) - 0.5) * 2.0 * HalfW)
								+ Up * (((double)j / (N - 1) - 0.5) * 2.0 * S.Extent.Z);
							const FVector End = Cam + (Through - Cam).GetSafeNormal() * (S.Distance + S.Extent.Size() * 2.0 + 100.0);
							FHitResult Nearest;
							bool bHit = false;
							for (UPrimitiveComponent* Shape : Shapes)
							{
								if (!Shape || !Shape->IsRegistered() || !Shape->IsVisible() || Shape->bHiddenInGame)
								{
									continue;
								}
								FHitResult Hit;
								if (Shape->LineTraceComponent(Hit, Cam, End, Ray) && (!bHit || Hit.Distance < Nearest.Distance))
								{
									Nearest = Hit;
									bHit = true;
								}
							}
							if (bHit)
							{
								++Hits;
								FHitResult Block;  // the part a player sees: not behind something else
								if (!(World->LineTraceSingleByChannel(Block, Cam, Nearest.ImpactPoint, ECC_Visibility, Query)
									  && Whole(Block.GetActor()) != S.Actor && Block.Distance < Nearest.Distance - 5.0))
								{
									Sum += Nearest.ImpactPoint;
									++Shown;
								}
							}
						}
					}
					if (Shown > 0)
					{
						Item->SetObjectField(TEXT("body_center"), Vec(Sum / Shown));  // the middle of what is seen of it
						if (Shown < Hits)
						{
							// how much of its body shows (the rest is behind something)
							Item->SetNumberField(TEXT("body_shown"), FMath::RoundToDouble(100.0 * Shown / Hits) / 100.0);
						}
					}
				}
				if (bOn)
				{
					FHitResult Block;
					const bool bHidden = World->LineTraceSingleByChannel(Block, Cam, Best.ImpactPoint, ECC_Visibility, Query)
						&& Whole(Block.GetActor()) != S.Actor && Block.Distance < Best.Distance - 5.0;
					if (!bHidden)
					{
						Item->SetBoolField(TEXT("crosshair_on"), true);
						if (Best.BoneName != NAME_None)
						{
							Item->SetStringField(TEXT("crosshair_part"), Best.BoneName.ToString());  // e.g. head, spine_03
						}
					}
				}
			}
			// How it is drawn (see DrawnOf), the lights placed at it among its lights, when anything of that shows
			FDrawn Drawing = DrawnOf(S.Actor, Pawn, PC, true);
			if (Index < PlacedAt.Num() && PlacedAt[Index].Lights > 0)
			{
				Drawing.Lights += PlacedAt[Index].Lights;
				Drawing.Brightness = FMath::Max(Drawing.Brightness, PlacedAt[Index].Brightness);
			}
			if (const TSharedPtr<FJsonObject> Look = LookJson(Drawing))
			{
				Item->SetObjectField(TEXT("look"), Look);
			}
			List.Add(MakeShared<FJsonValueObject>(Item));
		}

		// What the player holds, as its own camera shows it (`held`): the things fixed onto it, whoever owns them (onto
		// its body or one of its parts, a hand of its arms; or onto its controller or camera), each as the thing it is
		// part of (a sight on the gun: the gun), with the meshes of it drawn for the player's own view (see DrawnFor: a
		// game hiding the tool not in use, or drawing a gun's first-person copy for its owner alone, shows the player
		// just what is out). Each with its look (`parts`: the meshes drawn, biggest first), where it hangs on the player,
		// where those meshes are (their box's middle and size, how far the camera is from that box, where its middle is
		// on screen) and whether any of it is in the frustum. Nearest the camera first (by name among those as near);
		// none of them is in `visible`'s count
		TArray<TSharedPtr<FJsonValue>> Held;
		if (!bOnly)  // (a look at some things alone: see Only)
		{
			const AActor* Viewer = PC->GetViewTarget() ? PC->GetViewTarget() : Pawn;
			TArray<AActor*> Fixed = Carried;
			for (AActor* Base : {static_cast<AActor*>(PC), static_cast<AActor*>(CameraManager)})
			{
				TArray<AActor*> More;
				Base->GetAttachedActors(More, true, true);
				for (AActor* Actor : More)
				{
					Fixed.AddUnique(Actor);
				}
			}
			const TSet<AActor*> FixedSet(Fixed);
			struct FHeldPart
			{
				FString Asset;
				double Size;
			};
			struct FHeldThing
			{
				AActor* Root;
				TArray<FHeldPart> Parts;
				FBox Box;
				FName At;
			};
			// Where on the player a drawn mesh hangs: the socket or bone of the player's own part it is fixed onto, up
			// through what else of the held thing it is fixed onto (a sight on the gun in the hand: the hand's)
			auto HangsAt = [&FixedSet](const USceneComponent* Part)
			{
				for (int32 Depth = 0; Part && Part->GetAttachParent() && Depth < 32; Part = Part->GetAttachParent(), ++Depth)
				{
					const AActor* Below = Part->GetAttachParent()->GetOwner();
					if (Below != Part->GetOwner() && !FixedSet.Contains(Below))
					{
						return Part->GetAttachSocketName();
					}
				}
				return FName(NAME_None);
			};
			TArray<FHeldThing> Things;
			for (AActor* Actor : Fixed)
			{
				if (!Actor || Actor == Pawn || Actor == PC || Actor == CameraManager || Actor->IsA(AController::StaticClass())
					|| Actor->IsA(AInfo::StaticClass()) || Actor->IsA(APlayerCameraManager::StaticClass()))
				{
					continue;  // (the player's own bookkeeping: its controller, camera, state)
				}
				// The thing it is part of: up the things it is fixed onto, while those are the player's too
				AActor* Root = Actor;
				for (int32 Depth = 0; Depth < 8; ++Depth)
				{
					AActor* Onto = Root->GetAttachParentActor();
					if (!Onto || Onto == Root || !FixedSet.Contains(Onto) || Onto->IsA(AController::StaticClass())
						|| Onto->IsA(APlayerCameraManager::StaticClass()))
					{
						break;
					}
					Root = Onto;
				}
				TArray<FHeldPart> Parts;
				FBox Box(ForceInit);
				FName At = NAME_None;
				TInlineComponentArray<UMeshComponent*> Meshes(Actor, true);  // (its child actors' too)
				for (const UMeshComponent* Mesh : Meshes)
				{
					if (!DrawnFor(Mesh, Viewer, PC))
					{
						continue;
					}
					FString Asset;
					if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Mesh))
					{
						Asset = Static->GetStaticMesh() ? Static->GetStaticMesh()->GetName() : FString();
					}
					else if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Mesh))
					{
						Asset = Skinned->GetSkinnedAsset() ? Skinned->GetSkinnedAsset()->GetName() : FString();
					}
					if (Asset.IsEmpty())
					{
						continue;
					}
					Parts.Add({Asset, Mesh->Bounds.SphereRadius});
					Box += Mesh->Bounds.GetBox();
					if (At.IsNone())
					{
						At = HangsAt(Mesh);
					}
				}
				if (Parts.Num() == 0)
				{
					continue;  // nothing of it shows to the player now (put away, or drawn for the others only)
				}
				FHeldThing* Thing = Things.FindByPredicate([Root](const FHeldThing& T) { return T.Root == Root; });
				if (!Thing)
				{
					Thing = &Things.Add_GetRef({Root, {}, FBox(ForceInit), NAME_None});
				}
				Thing->Parts.Append(Parts);
				Thing->Box += Box;
				if (Thing->At.IsNone() || Actor == Root)
				{
					Thing->At = At.IsNone() ? Thing->At : At;
				}
			}
			// (by name among those as near: the same order every look)
			Things.Sort([](const FHeldThing& A, const FHeldThing& B) { return A.Root->GetName() < B.Root->GetName(); });
			TArray<TPair<double, TSharedPtr<FJsonValue>>> Sorted;
			for (FHeldThing& Thing : Things)
			{
				// Its meshes, each once at its biggest drawn size, biggest first (up to 8)
				TMap<FString, double> Sizes;
				for (const FHeldPart& Part : Thing.Parts)
				{
					double& Kept = Sizes.FindOrAdd(Part.Asset, 0.0);
					Kept = FMath::Max(Kept, Part.Size);
				}
				TArray<FString> Names;
				Sizes.GetKeys(Names);
				Names.Sort([&Sizes](const FString& A, const FString& B)
					{ return Sizes[A] != Sizes[B] ? Sizes[A] > Sizes[B] : A < B; });
				Names.SetNum(FMath::Min(Names.Num(), 8));
				TArray<TSharedPtr<FJsonValue>> PartsJson;
				for (const FString& Name : Names)
				{
					PartsJson.Add(MakeShared<FJsonValueString>(Name));
				}
				TSharedRef<FJsonObject> Look = MakeShared<FJsonObject>();
				Look->SetArrayField(TEXT("parts"), PartsJson);
				FVector Center, Extent;
				Thing.Box.GetCenterAndExtents(Center, Extent);
				const double Distance = FMath::Sqrt(Thing.Box.ComputeSquaredDistanceToPoint(Cam));
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Thing.Root->GetName());
				Item->SetStringField(TEXT("path"), PathOf(Thing.Root));
				Item->SetStringField(TEXT("class"), Thing.Root->GetClass()->GetName());
				Item->SetObjectField(TEXT("look"), Look);
				if (!Thing.At.IsNone())
				{
					Item->SetStringField(TEXT("attached_at"), Thing.At.ToString());  // e.g. hand_l: in a hand
				}
				Item->SetObjectField(TEXT("center"), Vec(Center));
				Item->SetObjectField(TEXT("size"), Vec(Extent, 2.0));
				Item->SetNumberField(TEXT("distance"), FMath::RoundToDouble(Distance));
				double X = 0.0, Y = 0.0;
				if (OnScreen(Center, X, Y))
				{
					Item->SetNumberField(TEXT("screen_x"), FMath::RoundToDouble(X * 100.0) / 100.0);
					Item->SetNumberField(TEXT("screen_y"), FMath::RoundToDouble(Y * 100.0) / 100.0);
				}
				// In the frustum: its middle or a corner of its box on screen
				bool bInView = false;
				for (int32 i = -1; i < 8 && !bInView; ++i)
				{
					const FVector P = i < 0 ? Center
						: Center + FVector(i & 1 ? Extent.X : -Extent.X, i & 2 ? Extent.Y : -Extent.Y, i & 4 ? Extent.Z : -Extent.Z);
					double PX, PY;
					bInView = OnScreen(P, PX, PY) && FMath::Abs(PX) <= 1.0 && FMath::Abs(PY) <= 1.0;
				}
				Item->SetBoolField(TEXT("in_view"), bInView);
				Sorted.Add({Distance, MakeShared<FJsonValueObject>(Item)});
			}
			Sorted.StableSort([](const TPair<double, TSharedPtr<FJsonValue>>& A, const TPair<double, TSharedPtr<FJsonValue>>& B)
				{ return A.Key < B.Key; });
			for (const TPair<double, TSharedPtr<FJsonValue>>& Entry : Sorted)
			{
				Held.Add(Entry.Value);
			}
		}

		// The scene's light as a player sees day turn to night: how bright the sun is, the brightest directional light
		// that lights the world (a moon light too, when it outshines the sun)
		TSharedRef<FJsonObject> Scene = MakeShared<FJsonObject>();
		double Sun = -1.0;
		for (TObjectIterator<UDirectionalLightComponent> It; It && !bOnly; ++It)
		{
			const UDirectionalLightComponent* Light = *It;
			if (Light && Light->GetWorld() == World && Drawn(Light) && Light->bAffectsWorld)
			{
				Sun = FMath::Max(Sun, (double)Light->ComputeLightBrightness());
			}
		}
		if (Sun >= 0.0)
		{
			Scene->SetNumberField(TEXT("sun"), FMath::RoundToDouble(Sun * 100.0) / 100.0);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("visible"), List);
		Result->SetArrayField(TEXT("held"), Held);
		Result->SetObjectField(TEXT("scene"), Scene);
		Result->SetBoolField(TEXT("crosshair_tested"), true);  // each thing says crosshair_on when the crosshair is on it
		Result->SetBoolField(TEXT("oriented"), true);  // each thing's box is its own (or it says `box`), its top measured
		// Whether effects play at all: a game run without rendering (-nullrhi) never starts one (Niagara and Cascade don't
		// activate then), so no look tells an effect and no watch sees one start, whatever a player would see
		Result->SetBoolField(TEXT("effects_play"), FApp::CanEverRender());
		TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
		Camera->SetObjectField(TEXT("location"), Vec(Cam));
		Camera->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(Rot.Yaw * 100.0) / 100.0);
		Camera->SetNumberField(TEXT("pitch"), FMath::RoundToDouble(FRotator::NormalizeAxis(Rot.Pitch) * 100.0) / 100.0);
		Camera->SetNumberField(TEXT("fov"), FMath::RoundToDouble(CameraManager->GetFOVAngle()));
		Result->SetObjectField(TEXT("camera"), Camera);
		if (bOnly)
		{
			// The names looked at alone, as asked (a plugin from before `only` answers with the whole view, without it)
			TArray<TSharedPtr<FJsonValue>> Asked;
			for (const FName& Name : Only)
			{
				Asked.Add(MakeShared<FJsonValueString>(Name.ToString()));
			}
			Result->SetArrayField(TEXT("only"), Asked);
		}
		// What the player's body feels
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		if (Pawn)
		{
			Body->SetObjectField(TEXT("velocity"), Vec(Pawn->GetVelocity()));
			Body->SetNumberField(TEXT("feet_z"), FMath::RoundToDouble(Feet));
			// What the body is against (see Touched), when it can tell
			if (bTouchTested)
			{
				Body->SetArrayField(TEXT("touching"), Touching);
			}
			// What is overhead: the first thing straight above the head (it stands under a slab, a ledge, a roof),
			// and how much room there is up to it. Only what the player perceives is overhead: a thing in view (it
			// sees itself under it), or one the head or the body is against; not whatever a trace straight up meets
			// while the view is elsewhere
			FVector Head = Pawn->GetActorLocation();
			if (Character && Character->GetCapsuleComponent())
			{
				Head.Z += Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			}
			FCollisionQueryParams Upward(SCENE_QUERY_STAT(PlaytestOverhead), false);
			Upward.AddIgnoredActor(Pawn);
			Upward.AddIgnoredActors(Carried);
			FHitResult Hit;
			const AActor* Above = !bOnly && OverheadReach > 0.0 && World->LineTraceSingleByChannel(Hit, Head,
				Head + FVector(0.0, 0.0, OverheadReach), ECC_Visibility, Upward) ? Whole(Hit.GetActor()) : nullptr;
			if (Above && (Listed.Contains(Above) || Touched.Contains(Above) || Hit.Distance <= TouchReach))
			{
				TSharedRef<FJsonObject> Overhead = MakeShared<FJsonObject>();
				Overhead->SetStringField(TEXT("thing"), Above->GetName());
#if WITH_EDITOR
				Overhead->SetStringField(TEXT("label"), Above->GetActorLabel());
#endif
				Overhead->SetNumberField(TEXT("clearance"), FMath::RoundToDouble(Hit.Distance));
				Body->SetObjectField(TEXT("overhead"), Overhead);
			}
			// With openings or ways up asked for: what is straight above the head, whatever is in view (a ceiling over
			// a player inside a house is over it, though the house's walls round the camera leave it out of the list)
			if (Above && (bOpenings || bWaysUp))
			{
				TSharedRef<FJsonObject> Under = MakeShared<FJsonObject>();
				Under->SetStringField(TEXT("thing"), Above->GetName());
				Under->SetNumberField(TEXT("clearance"), FMath::RoundToDouble(Hit.Distance));
				Body->SetObjectField(TEXT("under"), Under);
			}
			if (!bOnly && (bOpenings || bWaysUp || bSonar) && Character && Character->GetCapsuleComponent() && Move
				&& PlayerBody)
			{
				FBodyProbe Probe;
				Probe.World = World;
				Probe.Params = FCollisionQueryParams(SCENE_QUERY_STAT(PlaytestOpenings), false, Pawn);
				Probe.Params.AddIgnoredActors(Carried);
				PlayerBody->InitSweepCollisionParams(Probe.Params, Probe.Responses);
				Probe.Channel = PlayerBody->GetCollisionObjectType();
				Probe.Radius = Character->GetCapsuleComponent()->GetScaledCapsuleRadius();
				Probe.HalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				Probe.StepHeight = Move->MaxStepHeight;
				Probe.WalkableZ = Move->GetWalkableFloorZ();
				Probe.Feet = Feet;
				Probe.Me = Pawn->GetActorLocation();
				TArray<TPair<AActor*, FPlaytestShape>> Holders;
				for (const FSeen& S : Seen)
				{
					Holders.Add(TPair<AActor*, FPlaytestShape>(S.Actor, S.Shape));
				}
				if (bOpenings || bWaysUp)
				{
					AddOpenings(*Body, Probe, Cam, Rot.Yaw, CameraManager->GetFOVAngle(), FMath::Max(100.0, OpeningsReach),
						bOpenings, bWaysUp, Holders, Sight);
				}
				if (bSonar)
				{
					AddSonar(*Body, Probe, Rot.Yaw, SonarReach, SonarTurns);
				}
			}
		}
		if (Move)
		{
			Body->SetBoolField(TEXT("on_ground"), Move->IsMovingOnGround());
			Body->SetBoolField(TEXT("falling"), Move->IsFalling());
			Body->SetBoolField(TEXT("crouched"), Move->IsCrouching());
			if (StandingOn)
			{
				Body->SetStringField(TEXT("standing_on"), StandingOn->GetName());
#if WITH_EDITOR
				Body->SetStringField(TEXT("standing_on_label"), StandingOn->GetActorLabel());
#endif
				const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Move->CurrentFloor.HitResult.ImpactNormal.Z, -1.0, 1.0)));
				Body->SetNumberField(TEXT("floor_slope_degrees"), FMath::RoundToDouble(Slope));
			}
		}
		Result->SetObjectField(TEXT("body"), Body);
		Bridge.Respond(Id, Obj(Result));
	}
}
