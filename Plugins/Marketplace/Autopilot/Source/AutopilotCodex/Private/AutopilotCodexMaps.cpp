// The AutopilotCodex commandlet's maps (maps.txt): one line per placed actor, with where it is and what was set on it.

#include "AutopilotCodexInternal.h"

#include "Components/SceneComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"

namespace AutopilotCodex
{
	FString Vec(const FVector& V)
	{
		return FString::Printf(TEXT("(%d, %d, %d)"), FMath::RoundToInt(V.X), FMath::RoundToInt(V.Y), FMath::RoundToInt(V.Z));
	}

	bool IsProjectClass(const UClass* C)
	{
		for (const UClass* K = C; K; K = K->GetSuperClass())
		{
			if (Cast<UBlueprintGeneratedClass>(K))
			{
				return true;
			}
		}
		return false;
	}

	FString ActorDetails(AActor* A)
	{
		FString S;
		if (USceneComponent* Root = A->GetRootComponent())
		{
			S += TEXT(" at ") + Vec(Root->GetRelativeLocation());
			const FRotator R = Root->GetRelativeRotation();
			if (!R.IsNearlyZero(1.0))
			{
				S += FString::Printf(TEXT(" rot (%d, %d, %d)"), FMath::RoundToInt(R.Pitch), FMath::RoundToInt(R.Yaw), FMath::RoundToInt(R.Roll));
			}
			const FVector Sc = Root->GetRelativeScale3D();
			if (!Sc.Equals(FVector::OneVector, 0.01))
			{
				S += TEXT(" scale ") + TrimFloats(FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), Sc.X, Sc.Y, Sc.Z));
			}
			if (Root->GetAttachParent() && Root->GetAttachParent()->GetOwner() && Root->GetAttachParent()->GetOwner() != A)
			{
				S += TEXT(" attached to ") + Root->GetAttachParent()->GetOwner()->GetActorLabel();
			}
		}
		if (A->Tags.Num() > 0)
		{
			TArray<FString> T;
			for (const FName& N : A->Tags)
			{
				T.Add(N.ToString());
			}
			S += TEXT(" tags [") + FString::Join(T, TEXT(", ")) + TEXT("]");
		}
		if (IsProjectClass(A->GetClass()))
		{
			TArray<FString> Props;
			FDiffOptions O;
			O.Max = MaxActorProps;
			O.MaxLen = 120;
			O.bInstanceEditableOnly = true;
			O.bBlueprintDeclaredOnly = true;
			DiffProps(A, A->GetClass()->GetDefaultObject(), Props, O);
			if (Props.Num() > 0)
			{
				S += TEXT("  ") + JoinProps(Props);
			}
		}
		return S;
	}
}
