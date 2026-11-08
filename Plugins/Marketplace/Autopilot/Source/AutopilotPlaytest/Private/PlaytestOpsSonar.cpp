// view's `sonar` (docs/protocol.md): how far the player's body could walk each way before something stops it,
// found by sweeping its own capsule.

#include "PlaytestOps.h"

namespace PlaytestOps
{
	// --- sonar: how far the body could walk each way before something stops it (view's `sonar`) -------------------
	// What a player feels out round it as it walks, like a hand on a wall or a cane ahead: its own capsule (a hair thinner,
	// lifted a step's height off the feet, see FBodyProbe::Sweep) swept out each way from where it stands, turned from
	// where the view points (`turns`, degrees right; by default ahead, either side by 30, 60, 90 and 135, and behind), up
	// to `Reach`. Along the way lines down onto the floor a body's width apart feel what the feet would walk on: where the
	// floor steps up or down within a step (stairs, a ramp, a kerb) the sweep goes on from the new floor; where it falls
	// away more than a step, that stretch is felt again in shorter steps, each from the floor the one before found (stairs
	// or a ramp going down are walked down), and where one of them finds no floor within a step down it is a drop; a floor
	// steeper than the body stands on stops it. What the sweep meets: a step up (the floor just short of it is up to a step
	// higher, or the face met is walkable floor rising) is walked up, anything else stops the body (`wall`: what it is, the
	// face's normal there, to slide along). Each way: how far the body's middle gets (`clear`), what stops it (`by`:
	// `reach` when nothing does), how far it rose or fell on the way (`rise`). And point blank ahead: a line straight
	// ahead from the body at its knees' and its chest's height, how far past the body's side it meets something that
	// isn't floor it walks up (within SonarPointBlankWidths body widths). Never the level's navigation data: only what a
	// body finds by trying. A sweep goes the whole way left at first, and again after a stretch where the floor stayed
	// level; after the floor changed (a ramp, stairs: it changes again within a body's width or so) only
	// SonarSweepWidths body widths at a time, so uneven ground costs short sweeps, not the whole reach again and again
	// A change of the floor's height along the way, as a share of the step height, that starts the sweep afresh from the
	// new floor (less is the floor's own unevenness)
	const double SonarLevelShare = 0.25;
	const double SonarPointBlankWidths = 1.5;  // body widths ahead of the body the point-blank lines reach
	const double SonarDropHeights = 10.0;  // body heights down a drop is measured
	const double SonarSweepWidths = 4.0;  // body widths a sweep goes on uneven ground (see above)
	// Sweeps a look makes at most, shared among its ways (each way at least SonarMinGoes: a way that runs out over uneven
	// ground ends as far as it was felt), and the shorter steps per body width a stretch is felt again in where the floor
	// fell away more than a step
	const int32 SonarSweeps = 256, SonarMinGoes = 8, SonarSubSteps = 4;

	void AddSonar(FJsonObject& Body, const FBodyProbe& P, double Yaw, double Reach, const TArray<double>& Turns)
	{
		const double Height = P.HalfHeight * 2.0;
		const double Knee = FMath::Max(P.StepHeight + 5.0, Height * 0.28), Chest = FMath::Max(Knee + 10.0, Height * 0.62);
		const double Pace = FMath::Max(20.0, P.Radius * 2.0);  // the floor felt a body's width apart
		const double Level = FMath::Max(1.0, P.StepHeight * SonarLevelShare);
		auto DirOf = [](double Degrees)
		{
			const double R = FMath::DegreesToRadians(Degrees);
			return FVector(FMath::Cos(R), FMath::Sin(R), 0.0);
		};
		auto NormalOf = [](const FVector& N)
		{
			const FVector Flat = FVector(N.X, N.Y, 0.0).GetSafeNormal();
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("X"), FMath::RoundToDouble(Flat.X * 100.0) / 100.0);
			O->SetNumberField(TEXT("Y"), FMath::RoundToDouble(Flat.Y * 100.0) / 100.0);
			return O;
		};
		// Whether the floor at a place (x, y) the sweep from feet at Z went over is one the feet walk on: no lower than a
		// step down (else a drop). Felt from the middle of the swept capsule there (room the sweep found free: a ramp
		// rising under it is met, not started under)
		const double Middle = P.StepHeight + 2.0 + FMath::Max(FMath::Max(1.0, P.Radius - 2.0),
			P.HalfHeight - P.StepHeight * 0.5 - 2.0);
		auto FloorAt = [&](const FVector& At, double Z, double& FZ, bool& bWalk)
		{
			return P.Floor(FVector(At.X, At.Y, Z + Middle), Middle + P.StepHeight + 5.0, FZ, bWalk)
				&& FZ >= Z - P.StepHeight - 1.0;
		};
		TArray<TSharedPtr<FJsonValue>> Ways;
		// (the sweeps of the look shared among its ways)
		const int32 Goes = FMath::Max(SonarMinGoes, SonarSweeps / FMath::Max(1, Turns.Num()));
		const double Stretch = SonarSweepWidths * Pace;
		for (const double Turn : Turns)
		{
			const FVector Dir = DirOf(Yaw + Turn);
			FVector From(P.Me.X, P.Me.Y, 0.0);
			double Z = P.Feet, Travelled = 0.0, Clear = Reach;
			FString By = TEXT("reach");
			AActor* Thing = nullptr;
			FVector Normal = FVector::ZeroVector;
			double Drop = -1.0;
			bool bEnded = false, bLong = true;  // (bLong: the floor level so far, the next sweep goes the whole way)
			for (int32 Go = 0; Go < Goes && !bEnded; ++Go)
			{
				const double Left = Reach - Travelled;
				if (Left < 1.0)
				{
					Clear = Reach;
					bEnded = true;
					break;
				}
				FHitResult Hit;
				const FVector Start(From.X, From.Y, Z);
				const double Span = bLong ? Left : FMath::Min(Left, Stretch);
				const bool bHit = P.Sweep(Start, Start + Dir * Span, Hit);
				const double Open = bHit ? FMath::Max(0.0, (double)Hit.Distance) : Span;
				// The floor along the clear stretch, a body's width apart and at its end
				bool bAgain = false, bDone = false;
				for (double S = FMath::Min(Pace, Open); S > 0.5;)
				{
					const FVector At = From + Dir * S;
					double FZ = Z;
					bool bWalk = false;
					if (!FloorAt(At, Z, FZ, bWalk))
					{
						// More than a step below the floor last felt: that stretch again in shorter steps, each felt from
						// the floor the one before found (stairs, a ramp going down), else a drop where a short step finds
						// no floor within a step down
						double SubZ = Z, Good = FMath::Max(0.0, S - Pace);
						const double Sub = Pace / SonarSubSteps;
						bool bFell = false, bSteep = false;
						for (int32 k = 1; k <= SonarSubSteps; ++k)
						{
							const double T = FMath::Min(S, FMath::Max(0.0, S - Pace) + Sub * k);
							double TZ = SubZ;
							bool bTW = false;
							if (!FloorAt(From + Dir * T, SubZ, TZ, bTW))
							{
								bFell = true;
								break;
							}
							if (!bTW)
							{
								bSteep = true;
								break;
							}
							SubZ = TZ;
							Good = T;
						}
						if (!bFell && !bSteep)
						{
							From = At;
							Travelled += S;
							Z = SubZ;
							bAgain = true;
							break;
						}
						Clear = Travelled + Good;
						By = bFell ? TEXT("drop") : TEXT("steep");
						FHitResult Down;
						const FVector Over = From + Dir * FMath::Min(S, Good + Sub) + FVector(0.0, 0.0, SubZ);
						if (bFell && P.Line(Over, Over - FVector(0.0, 0.0, SonarDropHeights * Height), Down))
						{
							Drop = SubZ - Down.ImpactPoint.Z;
						}
						bDone = true;
						break;
					}
					if (!bWalk)
					{
						Clear = Travelled + FMath::Max(0.0, S - Pace);
						By = TEXT("steep");
						bDone = true;
						break;
					}
					if (FMath::Abs(FZ - Z) > Level)
					{
						// A step up or down, a ramp: on from here at the new floor
						From = At;
						Travelled += S;
						Z = FZ;
						bAgain = true;
						break;
					}
					S = S >= Open - 0.5 ? 0.0 : FMath::Min(S + Pace, Open);
				}
				if (bDone)
				{
					bEnded = true;
					break;
				}
				if (bAgain)
				{
					bLong = false;
					continue;
				}
				if (!bHit)
				{
					if (Span >= Left - 0.5)
					{
						Clear = Reach;
						bEnded = true;
						break;
					}
					// (a short sweep that met nothing over level floor: on from its end, the rest in one go)
					From += Dir * Open;
					Travelled += Open;
					bLong = true;
					continue;
				}
				// Met something: walkable floor rising (a ramp, a slope), or a step up whose next riser the body's sweep
				// met (the floor just short of it a step higher at most), else what stops the body
				const FVector Contact = Hit.ImpactPoint;
				double FZ = Z;
				bool bWalk = false, bUp = false;
				const FVector Short = FVector(Contact.X, Contact.Y, 0.0) - Dir * 2.0;
				if (Hit.ImpactNormal.Z >= P.WalkableZ)
				{
					// (the floor itself, rising under the sweep: on up it from where it met it)
					bUp = P.Floor(FVector(Short.X, Short.Y, Contact.Z + 5.0), Contact.Z + 6.0 - Z, FZ, bWalk) && bWalk
						&& FZ > Z + 2.0;
				}
				else
				{
					bUp = P.Floor(FVector(Short.X, Short.Y, Contact.Z), Contact.Z - Z + 1.0, FZ, bWalk) && bWalk
						&& FZ > Z + 2.0 && FZ <= Z + P.StepHeight + 1.0;
				}
				if (bUp && !Hit.bStartPenetrating)
				{
					From = FVector(Hit.Location.X, Hit.Location.Y, 0.0);
					Travelled += Open;
					Z = FZ;
					bLong = false;
					continue;
				}
				Clear = Travelled + Open;
				By = TEXT("wall");
				Thing = WholeOf(Hit.GetActor());
				Normal = Hit.ImpactNormal;
				bEnded = true;
				break;
			}
			if (!bEnded)
			{
				Clear = Travelled;  // (the sweeps ran out: as far as it felt, `reach`)
			}
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("turn"), FMath::RoundToDouble(FRotator::NormalizeAxis(Turn)));
			O->SetNumberField(TEXT("clear"), FMath::RoundToDouble(FMath::Clamp(Clear, 0.0, Reach)));
			O->SetStringField(TEXT("by"), By);
			if (Thing)
			{
				O->SetStringField(TEXT("thing"), Thing->GetName());
			}
			if (By == TEXT("wall") && !FVector(Normal.X, Normal.Y, 0.0).IsNearlyZero())
			{
				O->SetObjectField(TEXT("normal"), NormalOf(Normal));
			}
			if (Drop >= 0.0)
			{
				O->SetNumberField(TEXT("drop"), FMath::RoundToDouble(Drop));
			}
			if (FMath::Abs(Z - P.Feet) > Level)
			{
				O->SetNumberField(TEXT("rise"), FMath::RoundToDouble(Z - P.Feet));
			}
			Ways.Add(MakeShared<FJsonValueObject>(O));
		}
		TSharedRef<FJsonObject> Sonar = MakeShared<FJsonObject>();
		Sonar->SetNumberField(TEXT("reach"), FMath::RoundToDouble(Reach));
		Sonar->SetArrayField(TEXT("ways"), Ways);
		// Point blank ahead: how far past the body's side a line at its knees and one at its chest meet something (not
		// floor the body walks up: a ramp rising ahead met at the knees is no wall)
		TSharedRef<FJsonObject> Ahead = MakeShared<FJsonObject>();
		const FVector Way = DirOf(Yaw);
		const double Near = P.Radius * (1.0 + 2.0 * SonarPointBlankWidths);
		double Nearest = TNumericLimits<double>::Max();
		AActor* Met = nullptr;
		FVector MetNormal = FVector::ZeroVector;
		const TPair<const TCHAR*, double> Heights[] = {{TEXT("knee"), Knee}, {TEXT("chest"), Chest}};
		for (const TPair<const TCHAR*, double>& Line : Heights)
		{
			const FVector From(P.Me.X, P.Me.Y, P.Feet + Line.Value);
			FHitResult Hit;
			if (P.Line(From, From + Way * Near, Hit) && Hit.ImpactNormal.Z < P.WalkableZ)
			{
				Ahead->SetNumberField(Line.Key, FMath::RoundToDouble(FMath::Max(0.0, (double)Hit.Distance - P.Radius)));
				if (Hit.Distance < Nearest)
				{
					Nearest = Hit.Distance;
					Met = WholeOf(Hit.GetActor());
					MetNormal = FVector(Hit.ImpactNormal.X, Hit.ImpactNormal.Y, 0.0);
				}
			}
			else
			{
				Ahead->SetField(Line.Key, MakeShared<FJsonValueNull>());
			}
		}
		if (Met)
		{
			Ahead->SetStringField(TEXT("thing"), Met->GetName());
		}
		if (!MetNormal.IsNearlyZero())
		{
			Ahead->SetObjectField(TEXT("normal"), NormalOf(MetNormal));
		}
		Ahead->SetNumberField(TEXT("within"), FMath::RoundToDouble(Near - P.Radius));
		Sonar->SetObjectField(TEXT("point_blank"), Ahead);
		Body.SetObjectField(TEXT("sonar"), Sonar);
	}
}
