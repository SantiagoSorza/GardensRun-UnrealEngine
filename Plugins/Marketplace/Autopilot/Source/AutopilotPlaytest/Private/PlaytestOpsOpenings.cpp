// view's `openings` and `ways_up` (docs/protocol.md): where a player sees a way through a wall, or up to a floor
// above, found as its own body would find them.

#include "PlaytestOps.h"

namespace PlaytestOps
{
	// --- openings and ways up: where a player sees a way through a wall, or up to a floor above (view's `openings`,
	// `ways_up`) ---------------------------------------------------------------------------------------------------
	// What a player sees of the room round it, as its eyes and its own body tell it: lines from where it stands across
	// the camera's field of view at its chest's and its knees' height, stopped by what stops its body; its own capsule
	// swept to see whether it gets somewhere; lines down onto the floor to feel the steps. Never the level's navigation
	// data: only what a body could find by looking and trying

	// What is seen as one thing (see View's Whole): art a Blueprint spawns as a child actor is part of what spawned it
	AActor* WholeOf(AActor* Actor)
	{
		for (int32 Depth = 0; Actor && Actor->IsChildActor() && Actor->GetParentActor() && Depth < 8; ++Depth)
		{
			Actor = Actor->GetParentActor();
		}
		return Actor;
	}

	// Openings: across the field of view (camera yaw `Yaw`, `Fov` degrees wide), a line every degree from where the body
	// stands at its knees' and its chest's height (each stopped by what stops the body, up to `Reach`). An opening is a
	// run of lines that go deeper than the lines on both sides of it (by at least OpeningDeeper units, or a share of how
	// far those are), open at both heights (not a window, nor a low wall), bounded on each side by something standing
	// as high as the chest (a wall or a door's frame, not a table in the room beyond), every line through it going on
	// past the line between its sides by half a body's width at least. Each side is found to within a few units by more
	// lines between the last that stops short and the first through, and taken onto the wall's face where a side's line
	// met the jamb's side face set back in the wall (seen at a slant): its middle and width on the face, the way through
	// it square to the face (`yaw`). Each opening is then tried as a body would: the floor at it and on each side
	// (`floor_step`: the biggest step between them, a body stepping up no higher than it does; the floor no steeper than
	// it stands on), its own capsule swept straight from where it stands to just past it (`reachable_straight`, else
	// `blocked_at`, how far along, and `blocked_by`) and through it alone, on its middle line or a little to either side
	// (`passable`, with the floor: its middle where the capsule passes), what is overhead just past it or whose box holds
	// the place (`into`: a doorway into a house), and what its sides are part of (`wall`). Only openings a line of sight
	// from the camera reaches; the
	// nearest MaxOpenings. Also how far the body could go each OpeningSector degrees across the view at each height
	// (`free`). Ways up (when asked): every WayUpEvery degrees, lines down onto the floor every WayUpPace units out from
	// the body, as far as a body walks on it (up steps no higher than it steps, no steeper than it stands on, not
	// through a wall at its knees or its chest, nor off a drop): where the floor rises by WayUpRise or more from where it
	// begins rising (a staircase, a ramp up), its foot and its top; the nearest MaxWaysUp, one per foot
	const double OpeningRayDegrees = 1.0, OpeningDeeper = 150.0, OpeningDeeperShare = 0.3, OpeningSector = 15.0;
	const double OpeningSameHeight = 60.0;  // units: a side's knee and chest lines stop this close: one surface standing up
	const double OpeningFlat = 8.0;  // units off the line between two points a third may lie and the three be one face
	// An opening at least this share of the body's half width across as seen (a slant hides part of a doorway behind its
	// jamb: whether the body gets through is the capsule's to tell), and how far aside its middle line the capsule is
	// tried at
	const double OpeningSeenShare = 1.2, OpeningAside = 12.0;
	const int32 MaxOpenings = 5, OpeningsTried = 8, OpeningEdgeSteps = 3;
	const double WayUpEvery = 5.0, WayUpPace = 40.0, WayUpReach = 1500.0, WayUpRise = 100.0, WayUpFootApart = 150.0;
	const int32 MaxWaysUp = 3;
	// A staircase seen from the side: how far round a tread the floor is felt for the way it rises (a tread or two of
	// a flight, no more than a step up or down), and how many treads one way across the view tries
	const double WayUpProbe = 40.0;
	const int32 WayUpSideProbes = 2;
	// A way up's lane (see AddOpenings): across it at these shares of the way from its foot to its top, every
	// WayUpLaneStep units out each side up to WayUpLaneReach (a flight is a few body widths across at most: past that,
	// open floor), each end found to an eighth of a step (WayUpLaneHalvings halvings); a drop measured down to
	// WayUpLaneDeep at most, the floor below its foot to WayUpLaneBelow
	const double WayUpLaneStations[] = {1.0 / 3.0, 2.0 / 3.0};
	const double WayUpLaneStep = 20.0, WayUpLaneReach = 600.0, WayUpLaneDeep = 3000.0, WayUpLaneBelow = 1000.0;
	const int32 WayUpLaneHalvings = 3;

	void AddOpenings(FJsonObject& Body, const FBodyProbe& P, const FVector& Cam, double Yaw, double Fov, double Reach,
		bool bOpenings, bool bWaysUp, const TArray<TPair<AActor*, FPlaytestShape>>& Holders,
		const FCollisionQueryParams& Sight)
	{
		UWorld* World = P.World;
		const double Height = P.HalfHeight * 2.0;
		const double Knee = FMath::Max(P.StepHeight + 5.0, Height * 0.28), Chest = FMath::Max(Knee + 10.0, Height * 0.62);
		const int32 N = FMath::Clamp(FMath::FloorToInt(Fov / OpeningRayDegrees) + 1, 3, 361);
		const double Left = Yaw - Fov * 0.5;
		auto DirOf = [](double Degrees)
		{
			const double R = FMath::DegreesToRadians(Degrees);
			return FVector(FMath::Cos(R), FMath::Sin(R), 0.0);
		};
		auto Turn = [Yaw](const FVector& From, const FVector& To)
		{
			return FMath::RoundToDouble(FRotator::NormalizeAxis(
				FMath::RadiansToDegrees(FMath::Atan2(To.Y - From.Y, To.X - From.X)) - Yaw));
		};
		// How far a line at that height goes from the body before something stops it (Reach: nothing does)
		auto Depth = [&](double Degrees, double Above, AActor** Thing)
		{
			const FVector From(P.Me.X, P.Me.Y, P.Feet + Above);
			FHitResult Hit;
			if (P.Line(From, From + DirOf(Degrees) * Reach, Hit))
			{
				if (Thing)
				{
					*Thing = WholeOf(Hit.GetActor());
				}
				return (double)Hit.Distance;
			}
			if (Thing)
			{
				*Thing = nullptr;
			}
			return Reach;
		};
		struct FRay
		{
			double Yaw = 0.0, Knee = 0.0, Chest = 0.0;
			AActor* Thing = nullptr;
			double Open() const { return FMath::Min(Knee, Chest); }
		};
		TArray<FRay> Rays;
		Rays.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FRay& Ray = Rays[i];
			Ray.Yaw = FMath::Min(Left + i * OpeningRayDegrees, Yaw + Fov * 0.5);
			AActor* KneeThing = nullptr;
			AActor* ChestThing = nullptr;
			Ray.Knee = Depth(Ray.Yaw, Knee, &KneeThing);
			Ray.Chest = Depth(Ray.Yaw, Chest, &ChestThing);
			Ray.Thing = ChestThing ? ChestThing : KneeThing;
		}
		auto Deeper = [](double Near) { return FMath::Max(OpeningDeeper, OpeningDeeperShare * Near); };
		// A side: something stands there as high as the chest (both lines stop about as far)
		auto Standing = [&](const FRay& Ray)
		{
			return Ray.Knee < Reach && Ray.Chest < Reach && FMath::Abs(Ray.Knee - Ray.Chest) <= OpeningSameHeight;
		};
		auto PointOf = [&](double Degrees, double Distance)
		{
			return FVector(P.Me.X, P.Me.Y, 0.0) + DirOf(Degrees) * Distance;
		};

		if (bOpenings)
		{
			struct FGap
			{
				FVector Middle, Along, Out;  // its middle on the wall's face (z 0), the way along the wall, the way through
				double Width = 0.0, Beyond = 0.0, Distance = 0.0;
				AActor* Wall = nullptr;
				AActor* Near = nullptr;  // what each side's lines stopped on: the side met first across the view ...
				AActor* Far = nullptr;   // ... and the other (a gap between a house and a crate beside it: two things)
			};
			TArray<FGap> Gaps;
			// The way a wall runs at a side of a gap, from that side's line and two more beyond it (away from the gap,
			// `Step` -1 or 1) where they lie on one straight face (the middle one within a few units of the line between
			// the others); zero where they don't (a corner, the jamb's side face seen at a slant)
			auto WallWay = [&](int32 Side, int32 Step)
			{
				const int32 A = Side + Step, B = Side + 2 * Step;
				if (A < 0 || B < 0 || A >= N || B >= N || !Standing(Rays[A]) || !Standing(Rays[B]))
				{
					return FVector::ZeroVector;
				}
				const FVector P0 = PointOf(Rays[Side].Yaw, Rays[Side].Open()), P1 = PointOf(Rays[A].Yaw, Rays[A].Open());
				const FVector P2 = PointOf(Rays[B].Yaw, Rays[B].Open());
				const FVector Run = P2 - P0;
				const double Length = Run.Size2D();
				if (Length < 10.0 || FMath::Abs(FVector::CrossProduct(Run / Length, P1 - P0).Z) > OpeningFlat)
				{
					return FVector::ZeroVector;
				}
				return Run / Length;
			};
			// Each side to within a few units: more lines between the last one stopped at the side and the first through
			auto Refine = [&](double NearYaw, double FarYaw, double NearDepth, FVector& Side)
			{
				for (int32 s = 0; s < OpeningEdgeSteps; ++s)
				{
					const double Mid = (NearYaw + FarYaw) * 0.5;
					const double Open = FMath::Min(Depth(Mid, Knee, nullptr), Depth(Mid, Chest, nullptr));
					if (Open <= NearDepth + Deeper(NearDepth) * 0.5)
					{
						NearYaw = Mid;
						NearDepth = Open;
					}
					else
					{
						FarYaw = Mid;
					}
				}
				Side = PointOf(NearYaw, NearDepth);
			};
			for (int32 i = 0; i + 1 < N; ++i)
			{
				if (!Standing(Rays[i]) || Rays[i + 1].Open() - Rays[i].Open() < Deeper(Rays[i].Open()))
				{
					continue;  // no side here: the next line doesn't go much deeper
				}
				for (int32 k = i + 2; k < N; ++k)
				{
					if (!Standing(Rays[k]) || Rays[k - 1].Open() - Rays[k].Open() < Deeper(Rays[k].Open()))
					{
						continue;  // not the other side yet
					}
					const FVector L = PointOf(Rays[i].Yaw, Rays[i].Open()), R = PointOf(Rays[k].Yaw, Rays[k].Open());
					FVector Across = R - L;
					const double Chord = Across.Size2D();
					if (Chord < 1.0)
					{
						break;
					}
					FVector Out(-Across.Y / Chord, Across.X / Chord, 0.0);
					const FVector Middle = (L + R) * 0.5;
					if (FVector::DotProduct(Out, Middle - FVector(P.Me.X, P.Me.Y, 0.0)) < 0.0)
					{
						Out = -Out;
					}
					double Beyond = TNumericLimits<double>::Max();
					for (int32 j = i + 1; j < k; ++j)
					{
						Beyond = FMath::Min(Beyond, FVector::DotProduct(PointOf(Rays[j].Yaw, Rays[j].Open()) - L, Out));
					}
					if (Beyond < P.Radius || Chord < OpeningSeenShare * P.Radius)
					{
						break;  // something stands in the gap, or it is far too narrow for the body: no opening from here
					}
					FVector Near, Far;
					Refine(Rays[i].Yaw, Rays[i + 1].Yaw, Rays[i].Open(), Near);
					Refine(Rays[k].Yaw, Rays[k - 1].Yaw, Rays[k].Open(), Far);
					// Its sides on the wall's face: seen at a slant, one side's line meets the jamb's side face set back
					// in the wall; that point taken back onto the face (along the wall as the other side's face runs)
					// is where that side is. With neither face straight, the line between the sides
					FGap Gap;
					Gap.Along = WallWay(i, -1);
					if (!Gap.Along.IsZero())
					{
						Far = Near + Gap.Along * FVector::DotProduct(Far - Near, Gap.Along);
					}
					else if (!(Gap.Along = WallWay(k, 1)).IsZero())
					{
						Near = Far + Gap.Along * FVector::DotProduct(Near - Far, Gap.Along);
					}
					Across = Far - Near;
					Gap.Width = Across.Size2D();
					if (Gap.Width >= OpeningSeenShare * P.Radius)
					{
						Gap.Along = Across / Gap.Width;
						Gap.Out = FVector(-Gap.Along.Y, Gap.Along.X, 0.0);
						Gap.Middle = (Near + Far) * 0.5;
						if (FVector::DotProduct(Gap.Out, Gap.Middle - FVector(P.Me.X, P.Me.Y, 0.0)) < 0.0)
						{
							Gap.Out = -Gap.Out;
						}
						Gap.Beyond = Beyond;
						Gap.Wall = Rays[i].Thing ? Rays[i].Thing : Rays[k].Thing;
						Gap.Near = Rays[i].Thing;
						Gap.Far = Rays[k].Thing;
						Gap.Distance = FVector::Dist2D(Gap.Middle, P.Me);
						Gaps.Add(Gap);
					}
					i = k - 1;  // (on past its far side)
					break;
				}
			}
			Gaps.Sort([](const FGap& A, const FGap& B) { return A.Distance < B.Distance; });
			TArray<TSharedPtr<FJsonValue>> Openings;
			for (int32 g = 0; g < Gaps.Num() && g < OpeningsTried && Openings.Num() < MaxOpenings; ++g)
			{
				const FGap& Gap = Gaps[g];
				const FVector Out = Gap.Out;
				const double Width = Gap.Width;
				FVector Middle = Gap.Middle;
				// The floor before it, in it and just past it, felt from the chest's height down (it is open there)
				const double Top = P.Feet + Chest, Down = Chest + 600.0;
				const double BeforeOut = P.Radius + 40.0, AfterOut = P.Radius + 60.0;
				double ZBefore = P.Feet, ZIn = P.Feet, ZAfter = P.Feet;
				bool WBefore = false, WIn = false, WAfter = false;
				const FVector Before0 = Middle - Out * BeforeOut, After0 = Middle + Out * AfterOut;
				const bool bFloor = P.Floor(FVector(Before0.X, Before0.Y, Top), Down, ZBefore, WBefore)
					&& P.Floor(FVector(Middle.X, Middle.Y, Top), Down, ZIn, WIn)
					&& P.Floor(FVector(After0.X, After0.Y, Top), Down, ZAfter, WAfter);
				// Seen from the eyes: a line of sight from the camera reaches it at the chest's height
				const FVector Seen(Middle.X, Middle.Y, (bFloor ? ZIn : P.Feet) + Chest);
				FHitResult Block;
				if (World->LineTraceSingleByChannel(Block, Cam, Seen, ECC_Visibility, Sight)
					&& Block.Distance < FVector::Dist(Cam, Seen) - 30.0)
				{
					continue;  // hidden from the eyes
				}
				const double Step = bFloor ? FMath::Max(FMath::Abs(ZIn - ZBefore), FMath::Abs(ZAfter - ZIn)) : -1.0;
				const bool bFloorOk = bFloor && WBefore && WIn && WAfter && Step <= P.StepHeight + 1.0;
				// Through it alone: the capsule swept from just before it to just past it, on its middle line, else a
				// little to either side (where its sides were found is good to a few units); its middle where it passes
				FHitResult Hit;
				bool bPasses = false;
				for (const double Aside : {0.0, OpeningAside, -OpeningAside, 2.0 * OpeningAside, -2.0 * OpeningAside})
				{
					const FVector Line = Gap.Middle + Gap.Along * Aside;
					const FVector From = Line - Out * BeforeOut, To = Line + Out * AfterOut;
					if (bFloorOk && !P.Sweep(FVector(From.X, From.Y, ZBefore), FVector(To.X, To.Y, ZAfter), Hit))
					{
						bPasses = true;
						Middle = Line;
						break;
					}
				}
				const FVector After = Middle + Out * AfterOut;
				const FVector MeFeet(P.Me.X, P.Me.Y, P.Feet);
				const FVector AfterFeet(After.X, After.Y, bFloor ? ZAfter : P.Feet);
				const bool bReach = !P.Sweep(MeFeet, AfterFeet, Hit);
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetObjectField(TEXT("at"), Vec(FVector(Middle.X, Middle.Y, bFloor ? ZIn : P.Feet)));
				O->SetNumberField(TEXT("turn"), Turn(P.Me, Middle));
				O->SetNumberField(TEXT("distance"), FMath::RoundToDouble(FVector::Dist2D(Middle, P.Me)));
				O->SetNumberField(TEXT("width"), FMath::RoundToDouble(Width));
				O->SetNumberField(TEXT("yaw"), FMath::RoundToDouble(FMath::RadiansToDegrees(FMath::Atan2(Out.Y, Out.X))));
				O->SetNumberField(TEXT("depth_beyond"), FMath::RoundToDouble(FMath::Min(Gap.Beyond, Reach)));
				if (bFloor)
				{
					O->SetNumberField(TEXT("floor_step"), FMath::RoundToDouble(Step));
					O->SetNumberField(TEXT("floor_above_feet"), FMath::RoundToDouble(ZIn - P.Feet));
				}
				O->SetBoolField(TEXT("reachable_straight"), bReach);
				if (!bReach)
				{
					O->SetNumberField(TEXT("blocked_at"), FMath::RoundToDouble(FVector::Dist2D(Hit.Location, P.Me)));
					if (AActor* By = WholeOf(Hit.GetActor()))
					{
						O->SetStringField(TEXT("blocked_by"), By->GetName());
					}
				}
				O->SetBoolField(TEXT("passable"), bPasses);
				// What it leads into: what is overhead just past it, else the smallest thing in view whose box holds
				// the place a body's width in from its sides (a house with no roof over that part; a box's frames and
				// eaves reach a little past its walls, so a gap beside it isn't in it)
				const FVector Inside(After.X, After.Y, (bFloor ? ZAfter : P.Feet) + Height);
				FHitResult Over;
				AActor* Into = World->LineTraceSingleByChannel(Over, Inside, Inside + FVector(0.0, 0.0, 2000.0),
					ECC_Visibility, Sight) ? WholeOf(Over.GetActor()) : nullptr;
				if (!Into)
				{
					double Smallest = TNumericLimits<double>::Max();
					for (const TPair<AActor*, FPlaytestShape>& Holder : Holders)
					{
						const FPlaytestShape& S = Holder.Value;
						const FVector D = Inside - FVector(0.0, 0.0, Height * 0.5) - S.Center;
						const int32 Up = S.UpIndex();
						bool bIn = S.Half[Up] * 2.0 >= Height * 2.0;
						for (int32 a = 0; a < 3 && bIn; ++a)
						{
							bIn = FMath::Abs(FVector::DotProduct(D, S.Axis[a]))
								<= S.Half[a] + (a == Up ? 10.0 : -2.0 * P.Radius);
						}
						if (bIn && S.Half.X * S.Half.Y * S.Half.Z < Smallest)
						{
							Smallest = S.Half.X * S.Half.Y * S.Half.Z;
							Into = Holder.Key;
						}
					}
				}
				if (Into)
				{
					O->SetStringField(TEXT("into"), Into->GetName());
				}
				if (Gap.Wall)
				{
					O->SetStringField(TEXT("wall"), Gap.Wall->GetName());
				}
				if (Gap.Near && Gap.Far)
				{
					// What each of its sides is part of (the same thing twice for a doorway in one wall)
					TArray<TSharedPtr<FJsonValue>> Sides;
					Sides.Add(MakeShared<FJsonValueString>(Gap.Near->GetName()));
					Sides.Add(MakeShared<FJsonValueString>(Gap.Far->GetName()));
					O->SetArrayField(TEXT("sides"), Sides);
				}
				Openings.Add(MakeShared<FJsonValueObject>(O));
			}
			Body.SetArrayField(TEXT("openings"), Openings);
			// How far the body could go across each part of the view, at its knees and at its chest
			TArray<TSharedPtr<FJsonValue>> Free;
			for (double From = 0.0; From < Fov - 0.5; From += OpeningSector)
			{
				double AtKnee = Reach, AtChest = Reach;
				for (const FRay& Ray : Rays)
				{
					const double Off = Ray.Yaw - Left;
					if (Off >= From - 0.01 && Off < From + OpeningSector - 0.01)
					{
						AtKnee = FMath::Min(AtKnee, Ray.Knee);
						AtChest = FMath::Min(AtChest, Ray.Chest);
					}
				}
				TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
				// (its middle: the last part may be narrower)
				S->SetNumberField(TEXT("turn"), FMath::RoundToDouble((From + FMath::Min(From + OpeningSector, Fov)) * 0.5 - Fov * 0.5));
				S->SetNumberField(TEXT("knee"), FMath::RoundToDouble(AtKnee / 10.0) * 10.0);
				S->SetNumberField(TEXT("chest"), FMath::RoundToDouble(AtChest / 10.0) * 10.0);
				Free.Add(MakeShared<FJsonValueObject>(S));
			}
			Body.SetArrayField(TEXT("free"), Free);
		}

		if (bWaysUp)
		{
			struct FWayUp
			{
				FVector Foot, TopAt;
				double Rise = 0.0, FootDistance = 0.0, TopDistance = 0.0, Degrees = 0.0;
				bool bSide = false;  // seen from the side (see SideWayUp)
				bool bReach = true;  // its foot reached straight by the body's own capsule (told for those seen so)
				// Its lane (see Lanes below), when found: how wide, what lies past each side facing up it (0 open, 1 a
				// wall, 2 a drop, and how deep) and how far the floor goes on below its foot (and what ends it)
				bool bLane = false;
				double Width = 0.0, RightDepth = 0.0, LeftDepth = 0.0, Clear = 0.0;
				int32 RightBy = 0, LeftBy = 0, BelowBy = 0;
			};
			TArray<FWayUp> Found;
			// The floor under a place as a body walking from a floor `FromZ` high finds it: where it is (lines down from
			// a step over that floor) and that it stands on it
			auto FloorAt = [&](const FVector& At, double FromZ, double& Z)
			{
				bool bWalkable = false;
				return P.Floor(FVector(At.X, At.Y, FromZ + P.StepHeight + 5.0), P.StepHeight * 2.0 + 65.0, Z, bWalkable)
					&& bWalkable;
			};
			// From a tread along `Way` (Sign 1: while the floor rises; -1: while it falls), a step at most every
			// WayUpPace, no wall at the knees, no floor too steep, until it levels off: where it ends (going down, the
			// floor where it levelled off, the foot; going up, the last tread up, the top)
			auto March = [&](const FVector& From, double FromZ, const FVector& Way, double Sign)
			{
				FVector Last(From.X, From.Y, FromZ), End = Last;
				double Z = FromZ;
				int32 Level = 0;
				for (double Gone = WayUpPace; Gone <= WayUpReach && Level < 2; Gone += WayUpPace)
				{
					const FVector Next = FVector(From.X, From.Y, 0.0) + Way * Gone;
					FHitResult Hit;
					double F = Z;
					if (P.Line(FVector(Last.X, Last.Y, Z + Knee), FVector(Next.X, Next.Y, Z + Knee), Hit)
						|| !FloorAt(Next, Z, F) || FMath::Abs(F - Z) > P.StepHeight + 1.0)
					{
						break;  // a wall, a drop, a floor too steep to stand on or a step too high
					}
					if ((F - Z) * Sign > 3.0)
					{
						Level = 0;
						End = FVector(Next.X, Next.Y, F);
					}
					else if (++Level == 1 && Sign < 0.0)
					{
						End = FVector(Next.X, Next.Y, F);
					}
					Z = F;
					Last = FVector(Next.X, Next.Y, F);
				}
				return End;
			};
			// Seen from the side: a staircase rising across the view, its foot off to one side of where the body stands
			// (out of reach of the march straight out along a way). Lines down from the eyes' height every WayUpPace out
			// along the way `Degrees`, as far as its chest line went (`SightTo`: the first thing as high as the chest; a
			// tread lower than that is seen over), find a tread the eyes see: a floor a body stands on, higher than it
			// steps up. The floor a WayUpProbe round it, up a step one way and down a step the other, tells the way it
			// rises (every 45 degrees, then halfway on either side of the best). Marched down that way to where it
			// levels off on the body's own floor (its foot) and up to where it levels off (its top), as a body walks it;
			// its foot tried with the body's own capsule swept straight from where it stands (`bReach`). A staircase
			// found already in this look isn't marched again; at most WayUpSideProbes treads tried per way
			auto SideWayUp = [&](double Degrees, double SightTo, FWayUp& Way)
			{
				const FVector Dir = DirOf(Degrees);
				const double To = FMath::Min3(WayUpReach, Reach, SightTo - P.Radius);
				int32 Probes = 0;
				for (double Out = WayUpPace; Out <= To && Probes < WayUpSideProbes; Out += WayUpPace)
				{
					const FVector At = FVector(P.Me.X, P.Me.Y, 0.0) + Dir * Out;
					double Z = 0.0;
					bool bWalkable = false;
					if (!P.Floor(FVector(At.X, At.Y, Cam.Z), Cam.Z - P.Feet + P.StepHeight + 5.0, Z, bWalkable)
						|| !bWalkable || Z < P.Feet + P.StepHeight + 5.0)
					{
						continue;  // the body's own floor or a step up from it, a drop, nothing to stand on
					}
					const FVector Tread(At.X, At.Y, Z);
					for (const FWayUp& K : Found)
					{
						if (Z <= K.TopAt.Z + 5.0 && FMath::PointDistToSegment(FVector(Tread.X, Tread.Y, 0.0),
								FVector(K.Foot.X, K.Foot.Y, 0.0), FVector(K.TopAt.X, K.TopAt.Y, 0.0)) <= WayUpFootApart)
						{
							return false;  // (one found already)
						}
					}
					FHitResult Block;
					const FVector Seen = Tread + FVector(0.0, 0.0, 5.0);
					if (World->LineTraceSingleByChannel(Block, Cam, Seen, ECC_Visibility, Sight)
						&& Block.Distance < FVector::Dist(Cam, Seen) - 20.0)
					{
						continue;  // hidden from the eyes
					}
					++Probes;
					double Best = 0.0, BestYaw = 0.0;
					auto Try = [&](double Toward)
					{
						const FVector W = DirOf(Toward);
						double ZUp = 0.0, ZDown = 0.0;
						if (FloorAt(Tread + W * WayUpProbe, Z, ZUp) && FloorAt(Tread - W * WayUpProbe, Z, ZDown))
						{
							const double Rise = ZUp - Z, Drop = Z - ZDown;
							if (Rise > 3.0 && Rise <= P.StepHeight + 1.0 && Drop > 3.0 && Drop <= P.StepHeight + 1.0
								&& FMath::Min(Rise, Drop) > Best)
							{
								Best = FMath::Min(Rise, Drop);
								BestYaw = Toward;
							}
						}
					};
					for (int32 d = 0; d < 8; ++d)
					{
						Try(d * 45.0);
					}
					if (Best <= 0.0)
					{
						continue;  // level round it (a table, a crate's top): no tread
					}
					const double Coarse = BestYaw;
					Try(Coarse - 22.5);
					Try(Coarse + 22.5);
					const FVector Up = DirOf(BestYaw);
					const FVector Foot = March(Tread, Z, -Up, -1.0), Top = March(Tread, Z, Up, 1.0);
					if (FMath::Abs(Foot.Z - P.Feet) > P.StepHeight + 5.0 || Top.Z - Foot.Z < WayUpRise)
					{
						continue;  // not up from the body's own floor, or too little of a way up
					}
					FHitResult Hit;
					Way = FWayUp();
					Way.Foot = Foot;
					Way.TopAt = Top;
					Way.Rise = Top.Z - Foot.Z;
					Way.FootDistance = FVector::Dist2D(Foot, P.Me);
					Way.TopDistance = FVector::Dist2D(Top, P.Me);
					Way.Degrees = FMath::RadiansToDegrees(FMath::Atan2(Foot.Y - P.Me.Y, Foot.X - P.Me.X));
					Way.bSide = true;
					Way.bReach = !P.Sweep(FVector(P.Me.X, P.Me.Y, P.Feet), Foot, Hit);
					return true;
				}
				return false;
			};
			for (double Off = 0.0; Off <= Fov + 0.01; Off += WayUpEvery)
			{
				const double Degrees = Left + Off;
				const FVector Dir = DirOf(Degrees);
				// While the floor stays as high as the feet, the knee and chest lines across the view (the nearest one
				// to this way) tell how far the body goes; once it rises, lines at its knees and chest from the floor
				// reached
				const int32 Nearest = FMath::Clamp(FMath::RoundToInt(Off / OpeningRayDegrees), 0, N - 1);
				const double Flat = Rays[Nearest].Open();
				double Z = P.Feet, FootZ = P.Feet, FootDistance = -1.0;
				FVector Last(P.Me.X, P.Me.Y, P.Feet), Foot = Last;
				double Rise = 0.0, TopDistance = 0.0;
				FVector TopAt = Last;
				int32 Level = 0;  // samples in a row with no rise since it rose
				for (double Out = WayUpPace; Out <= FMath::Min(WayUpReach, Reach); Out += WayUpPace)
				{
					const FVector Next = FVector(P.Me.X, P.Me.Y, 0.0) + Dir * Out;
					// (the line across the view at the feet's own level tells that nothing stops the body up to here)
					const bool bClear = Z <= P.Feet + 5.0 && Z >= P.Feet - 5.0 && Out <= Flat - P.Radius;
					FHitResult Hit;
					if (!bClear && (P.Line(FVector(Last.X, Last.Y, Z + Knee), FVector(Next.X, Next.Y, Z + Knee), Hit)
									|| P.Line(FVector(Last.X, Last.Y, Z + Chest), FVector(Next.X, Next.Y, Z + Chest), Hit)))
					{
						break;  // a wall at the knees or the chest, from the floor reached
					}
					double Floor = Z;
					bool bWalkable = false;
					if (!P.Floor(FVector(Next.X, Next.Y, Z + P.StepHeight + 5.0), P.StepHeight * 2.0 + 65.0, Floor, bWalkable)
						|| !bWalkable || Floor > Z + P.StepHeight + 1.0 || Floor < Z - P.StepHeight - 1.0)
					{
						break;  // a drop, a floor too steep to stand on, or a step too high
					}
					if (Floor > Z + 3.0)
					{
						if (FootDistance < 0.0)
						{
							FootDistance = Out - WayUpPace;
							Foot = FVector(Last.X, Last.Y, Z);
							FootZ = Z;
						}
						Level = 0;
					}
					else if (FootDistance >= 0.0 && ++Level >= 2 && Z - FootZ >= WayUpRise)
					{
						break;  // on top: level again
					}
					else if (FootDistance >= 0.0 && Level >= 3 && Z - FootZ < WayUpRise)
					{
						FootDistance = -1.0;  // a small rise that levelled off (a kerb, a sill): not a way up
						Level = 0;
					}
					Z = Floor;
					Last = FVector(Next.X, Next.Y, Z);
					if (FootDistance >= 0.0 && Z - FootZ > Rise)
					{
						Rise = Z - FootZ;
						TopDistance = Out;
						TopAt = Last;
					}
				}
				if (FootDistance < 0.0 || Rise < WayUpRise)
				{
					// None straight out along this way: one across it, seen from the side
					FWayUp Side;
					if (SideWayUp(Degrees, Rays[Nearest].Chest, Side))
					{
						Found.Add(Side);
					}
					continue;
				}
				// Seen from the eyes: a line of sight reaches its foot (just above the floor there)
				FHitResult Block;
				const FVector FootSeen = Foot + FVector(0.0, 0.0, 30.0);
				if (World->LineTraceSingleByChannel(Block, Cam, FootSeen, ECC_Visibility, Sight)
					&& Block.Distance < FVector::Dist(Cam, FootSeen) - 30.0)
				{
					continue;
				}
				Found.Add({Foot, TopAt, Rise, FootDistance, TopDistance, Degrees});
			}
			Found.Sort([](const FWayUp& A, const FWayUp& B) { return A.FootDistance < B.FootDistance; });
			TArray<FWayUp> Kept;
			for (const FWayUp& Way : Found)
			{
				FWayUp* Same = Kept.FindByPredicate([&Way](const FWayUp& K)
					{ return FVector::Dist2D(K.Foot, Way.Foot) <= WayUpFootApart; });
				if (!Same)
				{
					if (Kept.Num() < MaxWaysUp)
					{
						Kept.Add(Way);
					}
				}
				else if (Way.Rise > Same->Rise)
				{
					*Same = Way;  // (one foot: the line up it that rose most)
				}
			}
			// The lane up each way kept (`lane`): across it at WayUpLaneStations of the way from its foot to its top, from
			// the tread there out each side every WayUpLaneStep units, the floor as a body walks across it (no wall at
			// its knees, up or down no more than a step from the last place, a floor it stands on) to where it stops: a
			// wall (a wall at the knees, a step up higher than a body steps, a floor too steep to stand on), the floor
			// falling more than a step (a drop: how deep, straight down to what is below, at most WayUpLaneDeep) or
			// WayUpLaneReach units out (open floor), each end found to within an eighth of a step. The middle of each run
			// lies on the flight's middle line whichever way across it the run went (the middle of a chord across a
			// strip with straight sides): the line through the two middles is its middle line, down which its foot and
			// up which its top are marched again (see March), so a staircase seen from different places is told on one
			// line; its width the narrowest run, square to that line; and how far a body walks on from its foot down
			// that line before the floor stops it (as across it, up to WayUpLaneBelow), as far as the eyes see the floor
			// go on there (a place no line of sight reaches, as for a tread seen from the side, ends it: unseen). The
			// runs across it are walked as its foot and top are marched (see March): a body's walk across a staircase a
			// tread of which the eyes saw, a few body widths each side of it (no farther than WayUpLaneReach), as a
			// player sees a flight's sides from where it stands; the floor below its foot, out from it, only as seen.
			// Kept as found, with no lane, where a run isn't found or that line's foot isn't on the body's floor or its
			// top doesn't rise WayUpRise
			auto Lanes = [&](FWayUp& Way)
			{
				// (a place on the floor the eyes see: a line of sight reaches just over it, as a tread seen from the side)
				auto SeenFloor = [&](const FVector& At)
				{
					FHitResult Block;
					const FVector Over = At + FVector(0.0, 0.0, 5.0);
					return !World->LineTraceSingleByChannel(Block, Cam, Over, ECC_Visibility, Sight)
						|| Block.Distance >= FVector::Dist(Cam, Over) - 20.0;
				};
				// From a tread (At, its floor FromZ high) out along Dir: how far a body walks on, at most Most; By: what
				// stopped it (0 nothing, 1 a wall, 2 a drop, DropDepth deep, 3 a place the eyes don't see, with bSight)
				auto Across = [&](const FVector& At, double FromZ, const FVector& Dir, double Most, int32& By,
					double& DropDepth, bool bSight)
				{
					double Z = FromZ, Good = 0.0;
					By = 0;
					DropDepth = 0.0;
					// (whether the body walks on Out along from where it got to, the floor there, and why not)
					auto Try = [&](double Out, double& NewZ, int32& Why, double& Fall)
					{
						const FVector From2 = FVector(At.X, At.Y, 0.0) + Dir * Good;
						const FVector To2 = FVector(At.X, At.Y, 0.0) + Dir * Out;
						FHitResult Hit;
						if (P.Line(FVector(From2.X, From2.Y, Z + Knee), FVector(To2.X, To2.Y, Z + Knee), Hit))
						{
							Why = 1;
							return false;
						}
						double F = Z;
						bool bWalk = false;
						if (P.Floor(FVector(To2.X, To2.Y, Z + P.StepHeight + 5.0), P.StepHeight * 2.0 + 10.0, F, bWalk)
							&& F >= Z - P.StepHeight - 1.0)
						{
							if (F > Z + P.StepHeight + 1.0 || !bWalk)
							{
								Why = 1;
								return false;
							}
							if (bSight && !SeenFloor(FVector(To2.X, To2.Y, F)))
							{
								Why = 3;
								return false;
							}
							NewZ = F;
							return true;
						}
						double Low = Z;
						bool bLowWalk = false;
						// (a drop, where the floor ends seen: the eyes reach over its edge)
						Why = bSight && !SeenFloor(FVector(To2.X, To2.Y, Z)) ? 3 : 2;
						Fall = P.Floor(FVector(To2.X, To2.Y, Z), WayUpLaneDeep, Low, bLowWalk) ? Z - Low : WayUpLaneDeep;
						return false;
					};
					for (double Out = WayUpLaneStep; Out <= Most + 0.01; Out += WayUpLaneStep)
					{
						double NewZ = Z, Fall = 0.0;
						int32 Why = 0;
						if (!Try(Out, NewZ, Why, Fall))
						{
							double Hi = Out;
							for (int32 h = 0; h < WayUpLaneHalvings; ++h)
							{
								const double Mid = (Good + Hi) * 0.5;
								double MidZ = Z, MidFall = 0.0;
								int32 MidWhy = 0;
								if (Try(Mid, MidZ, MidWhy, MidFall))
								{
									Good = Mid;
									Z = MidZ;
								}
								else
								{
									Hi = Mid;
								}
							}
							By = Why;
							DropDepth = Fall;
							return Good;
						}
						Good = Out;
						Z = NewZ;
					}
					return Most;
				};
				const FVector Rough = FVector(Way.TopAt.X - Way.Foot.X, Way.TopAt.Y - Way.Foot.Y, 0.0).GetSafeNormal();
				if (Rough.IsNearlyZero())
				{
					return;
				}
				const FVector Side(-Rough.Y, Rough.X, 0.0);  // (its right, facing up it the way it was found)
				FVector Middles[2];
				double Zs[2] = {0.0, 0.0}, Chords[2] = {0.0, 0.0};
				int32 Bys[2][2] = {{0, 0}, {0, 0}};  // [station][0: along Side, 1: against it]
				double Depths[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
				for (int32 k = 0; k < 2; ++k)
				{
					const double T = WayUpLaneStations[k];
					const FVector At = FMath::Lerp(Way.Foot, Way.TopAt, T);
					double Z = 0.0;
					if (!FloorAt(At, FMath::Lerp(Way.Foot.Z, Way.TopAt.Z, T), Z))
					{
						return;
					}
					const double Out = Across(At, Z, Side, WayUpLaneReach, Bys[k][0], Depths[k][0], false);
					const double Back = Across(At, Z, -Side, WayUpLaneReach, Bys[k][1], Depths[k][1], false);
					Middles[k] = FVector(At.X, At.Y, 0.0) + Side * ((Out - Back) * 0.5);
					Chords[k] = Out + Back;
					Zs[k] = Z;
				}
				FVector Up2 = FVector(Middles[1].X - Middles[0].X, Middles[1].Y - Middles[0].Y, 0.0);
				Up2 = Up2.Size() < P.Radius ? Rough : Up2.GetSafeNormal();
				const double Square = FMath::Abs(Side.X * Up2.Y - Side.Y * Up2.X);  // (how square the runs went across it)
				const FVector Foot = March(FVector(Middles[0].X, Middles[0].Y, Zs[0]), Zs[0], -Up2, -1.0);
				const FVector TopAt = March(FVector(Middles[1].X, Middles[1].Y, Zs[1]), Zs[1], Up2, 1.0);
				if (FMath::Abs(Foot.Z - Way.Foot.Z) > P.StepHeight + 5.0 || TopAt.Z - Foot.Z < WayUpRise)
				{
					return;
				}
				Way.Foot = Foot;
				Way.TopAt = TopAt;
				Way.Rise = TopAt.Z - Foot.Z;
				Way.FootDistance = FVector::Dist2D(Foot, P.Me);
				Way.TopDistance = FVector::Dist2D(TopAt, P.Me);
				Way.Degrees = FMath::RadiansToDegrees(FMath::Atan2(Foot.Y - P.Me.Y, Foot.X - P.Me.X));
				Way.bLane = true;
				Way.Width = FMath::Min(Chords[0], Chords[1]) * Square;
				// (the worse of the two runs each side: a drop over a wall over open floor; of two drops, the deeper)
				const bool bSideRight = Side.X * -Up2.Y + Side.Y * Up2.X >= 0.0;  // (Side lies to the right facing up)
				for (int32 j = 0; j < 2; ++j)
				{
					int32 Worst = 0;
					double Deepest = 0.0;
					for (int32 k = 0; k < 2; ++k)
					{
						if ((Bys[k][j] == 2 && (Worst != 2 || Depths[k][j] > Deepest)) || (Bys[k][j] == 1 && Worst == 0))
						{
							Worst = Bys[k][j];
							Deepest = Depths[k][j];
						}
					}
					const bool bRight = (j == 0) == bSideRight;
					(bRight ? Way.RightBy : Way.LeftBy) = Worst;
					(bRight ? Way.RightDepth : Way.LeftDepth) = Deepest;
				}
				double Unused = 0.0;
				Way.Clear = Across(Foot, Foot.Z, -Up2, WayUpLaneBelow, Way.BelowBy, Unused, true);
			};
			for (FWayUp& Way : Kept)
			{
				Lanes(Way);
			}
			TArray<TSharedPtr<FJsonValue>> Ways;
			for (const FWayUp& Way : Kept)
			{
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetNumberField(TEXT("turn"), FMath::RoundToDouble(FRotator::NormalizeAxis(Way.Degrees - Yaw)));
				O->SetNumberField(TEXT("distance_to_foot"), FMath::RoundToDouble(Way.FootDistance));
				O->SetNumberField(TEXT("rises_by"), FMath::RoundToDouble(Way.Rise));
				O->SetNumberField(TEXT("top_distance"), FMath::RoundToDouble(Way.TopDistance));
				O->SetObjectField(TEXT("foot"), Vec(Way.Foot));
				O->SetObjectField(TEXT("top"), Vec(Way.TopAt));
				if (Way.bSide)
				{
					O->SetStringField(TEXT("seen_from"), TEXT("side"));
					O->SetBoolField(TEXT("reachable_straight"), Way.bReach);
				}
				if (Way.bLane)
				{
					auto SideOf = [](int32 Kind, double Deep)
					{
						TSharedRef<FJsonObject> Told = MakeShared<FJsonObject>();
						Told->SetStringField(TEXT("by"), Kind == 2 ? TEXT("drop") : Kind == 1 ? TEXT("wall") : TEXT("open"));
						if (Kind == 2)
						{
							Told->SetNumberField(TEXT("depth"), FMath::RoundToDouble(Deep));
						}
						return Told;
					};
					TSharedRef<FJsonObject> Lane = MakeShared<FJsonObject>();
					Lane->SetNumberField(TEXT("width"), FMath::RoundToDouble(Way.Width));
					Lane->SetObjectField(TEXT("right"), SideOf(Way.RightBy, Way.RightDepth));
					Lane->SetObjectField(TEXT("left"), SideOf(Way.LeftBy, Way.LeftDepth));
					TSharedRef<FJsonObject> UnderFoot = MakeShared<FJsonObject>();
					UnderFoot->SetNumberField(TEXT("clear"), FMath::RoundToDouble(Way.Clear));
					UnderFoot->SetStringField(TEXT("by"), Way.BelowBy == 3 ? TEXT("unseen") : Way.BelowBy == 2
						? TEXT("drop") : Way.BelowBy == 1 ? TEXT("wall") : TEXT("reach"));
					Lane->SetObjectField(TEXT("below"), UnderFoot);
					O->SetObjectField(TEXT("lane"), Lane);
				}
				// What it is inside: what is overhead at its foot
				const FVector Head = Way.Foot + FVector(0.0, 0.0, Height);
				FHitResult Over;
				if (World->LineTraceSingleByChannel(Over, Head, Head + FVector(0.0, 0.0, 2000.0), ECC_Visibility, Sight))
				{
					if (AActor* Inside = WholeOf(Over.GetActor()))
					{
						O->SetStringField(TEXT("inside"), Inside->GetName());
					}
				}
				Ways.Add(MakeShared<FJsonValueObject>(O));
			}
			Body.SetArrayField(TEXT("ways_up"), Ways);
		}
	}
}
