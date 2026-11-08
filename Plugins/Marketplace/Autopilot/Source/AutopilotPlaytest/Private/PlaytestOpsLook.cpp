// How a thing is drawn, for `view` (its `look`) and `watch` with `drawn` (docs/protocol.md): its meshes, effects,
// lights, animation, texts and bars, and its visible body and own box.

#include "PlaytestOps.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/ProgressBar.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/TextRenderComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/UObjectIterator.h"

namespace PlaytestOps
{
	// --- drawn: how a thing looks ---------------------------------------------------------------------
	// What a player sees of a thing besides its shape and where it is: the meshes it is drawn with, the effects playing
	// on it (a fire, sparks, the muzzle flash of a weapon it holds), its lights, the animation it plays, whether it hangs
	// limp (a ragdoll), the texts and bars drawn on it (a name plate, a health bar), and whether it is highlighted (an
	// outline or an overlay). From its own parts, its child actors' and what is attached to it (a weapon in hand), never
	// from the player or what the player holds

	// Whether a part is drawn now for the player: registered, visible, not hidden in game (nor is its actor), no editor
	// helper, and not drawn for its owner alone (a character's first-person arms, which only its own camera shows; no
	// thing told of is the player's)
	bool Drawn(const USceneComponent* Part)
	{
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Part);
		return Part && Part->IsRegistered() && Part->IsVisible() && !Part->bHiddenInGame && !Part->IsEditorOnly()
			&& (!Part->GetOwner() || !Part->GetOwner()->IsHidden()) && !(Primitive && Primitive->bOnlyOwnerSee);
	}

	// Whether a part is drawn for a viewer's own camera (the player's view target: its pawn): registered, visible, not
	// hidden in game (nor is its actor), no editor helper; a part drawn for its owner alone (a gun's first-person copy)
	// only when the viewer owns it, directly or indirectly, and a part its owner doesn't see (the gun's third-person
	// copy, which the others see in the player's hands) only when it doesn't. How the renderer tells them apart. Nor a
	// part left out of the main pass, drawn only into scene captures, or hidden from this player's camera (its
	// controller's hidden actors and components)
	bool DrawnFor(const UPrimitiveComponent* Part, const AActor* Viewer, const APlayerController* PC)
	{
		if (!Part || !Part->IsRegistered() || !Part->IsVisible() || Part->bHiddenInGame || Part->IsEditorOnly()
			|| !Part->bRenderInMainPass || Part->bVisibleInSceneCaptureOnly)
		{
			return false;
		}
		const AActor* Owner = Part->GetOwner();
		if (Owner && Owner->IsHidden())
		{
			return false;
		}
		if (PC && ((Owner && PC->HiddenActors.Contains(Owner))
				|| PC->HiddenPrimitiveComponents.ContainsByPredicate(
					[Part](const TWeakObjectPtr<UPrimitiveComponent>& Hidden) { return Hidden.Get() == Part; })))
		{
			return false;
		}
		bool bViewersOwn = false;
		int32 Depth = 0;
		for (const AActor* Up = Owner; Up && !bViewersOwn && Depth < 16; Up = Up->GetOwner(), ++Depth)
		{
			bViewersOwn = Viewer && Up == Viewer;
		}
		return !(Part->bOnlyOwnerSee && !bViewersOwn) && !(Part->bOwnerNoSee && bViewersOwn);
	}

	// The player's: its pawn, what is attached to it (a weapon in hand, or the player riding on something), and what it
	// or its controller owns
	bool PlayersOwn(const AActor* Actor, const APawn* Pawn, const APlayerController* PC)
	{
		if (!Actor)
		{
			return false;
		}
		if (Pawn && (Actor == Pawn || Actor->IsAttachedTo(Pawn)))
		{
			return true;
		}
		int32 Depth = 0;
		for (const AActor* Owner = Actor->GetOwner(); Owner && Depth < 8; Owner = Owner->GetOwner(), ++Depth)
		{
			if (Owner == Pawn || Owner == PC)
			{
				return true;
			}
		}
		return false;
	}

	// --- a thing's body: what `view` lists a thing by and boxes it with ----------------------------------------------

	// A part of a thing's visible body: drawn for the player (see Drawn: no editor helpers, nor parts only their owner
	// sees), and not a collision shape such as a spawn capsule or a trigger volume, an effect (smoke, sparks, a muzzle
	// flash), nor a label drawn over it (a health bar or a name plate isn't its body)
	bool IsBodyPart(const UPrimitiveComponent* Part)
	{
		return Drawn(Part) && !Part->IsA(UShapeComponent::StaticClass()) && !Part->IsA(UFXSystemComponent::StaticClass())
			&& !Part->IsA(UWidgetComponent::StaticClass());
	}

	// The bounds of a thing's visible body, its child actors' included: around it in the world (Visible) and in its own
	// frame (Local). Neither is valid when nothing of it shows
	void BodyBounds(const AActor* Actor, FBox& Visible, FBox& Local)
	{
		Visible = FBox(ForceInit);
		Local = FBox(ForceInit);
		const FTransform& ActorTransform = Actor->GetActorTransform();
		TInlineComponentArray<UPrimitiveComponent*> Parts(Actor, true);
		for (const UPrimitiveComponent* Part : Parts)
		{
			if (IsBodyPart(Part))
			{
				Visible += Part->Bounds.GetBox();
				Local += Part->CalcBounds(Part->GetComponentTransform().GetRelativeTransform(ActorTransform)).GetBox();
			}
		}
	}

namespace
{
	// Whether a thing has a visible body of its own (its child actors' included): what `view` can list
	bool HasBody(const AActor* Actor)
	{
		TInlineComponentArray<UPrimitiveComponent*> Parts(Actor, true);
		return Parts.ContainsByPredicate([](const UPrimitiveComponent* Part) { return IsBodyPart(Part); });
	}
}

	// The own box of an actor from its parts' bounds in its own frame (Local), else the box around it (Origin,
	// Extent: square to the world)
	FPlaytestShape ShapeOf(const AActor* Actor, const FBox& Local, const FVector& Origin, const FVector& Extent)
	{
		FPlaytestShape Shape;
		Shape.Center = Origin;
		Shape.Half = Extent;
		if (Local.IsValid)
		{
			const FTransform& Transform = Actor->GetActorTransform();
			FVector LocalCenter, LocalExtent;
			Local.GetCenterAndExtents(LocalCenter, LocalExtent);
			Shape.Center = Transform.TransformPosition(LocalCenter);
			Shape.Half = LocalExtent * Transform.GetScale3D().GetAbs();
			const FQuat Turned = Transform.GetRotation();
			Shape.Axis[0] = Turned.GetAxisX();
			Shape.Axis[1] = Turned.GetAxisY();
			Shape.Axis[2] = Turned.GetAxisZ();
		}
		return Shape;
	}

	// --- lights -----------------------------------------------------------------------------------------------------

namespace
{
	// How bright a light shines on the world: 0 when it lights nothing (it doesn't light the world, or has no intensity)
	double LightBrightness(const ULightComponent* Light)
	{
		return Light->bAffectsWorld && Light->Intensity > 0.0f ? (double)Light->ComputeLightBrightness() : 0.0;
	}
}

	// The lights drawn by helpers: lit lights (not the sun's: no place) of actors with no visible body of their own (logic
	// placed beside what it lights: a charging pad's glow kept on its own actor) that hang from nothing with a body (that
	// thing's own light, told in its look), aren't the player's (a lamp it carries) and stay where they are (a shot's
	// glow flying past isn't at anything). Seen as the light of whatever thing they are at
	TArray<FPlacedLight> PlacedLights(const UWorld* World, const APawn* Pawn, const APlayerController* PC)
	{
		TArray<FPlacedLight> Out;
		TMap<const AActor*, bool> Bodies;  // whether an actor has a visible body (see HasBody), as found
		for (TObjectIterator<ULightComponent> It; It; ++It)
		{
			const ULightComponent* Light = *It;
			if (!Light || Light->GetWorld() != World || !Light->GetOwner() || Light->IsA(UDirectionalLightComponent::StaticClass())
				|| !Drawn(Light) || Light->GetOwner()->GetVelocity().Size() >= 10.0)
			{
				continue;
			}
			const double Brightness = LightBrightness(Light);
			bool bHelper = Brightness > 0.0;
			int32 Depth = 0;
			for (const AActor* Up = Light->GetOwner(); Up && bHelper && Depth < 16; Up = Up->GetAttachParentActor(), ++Depth)
			{
				const bool* Known = Bodies.Find(Up);
				const bool bBody = Known ? *Known : Bodies.Add(Up, HasBody(Up));
				bHelper = !bBody && !PlayersOwn(Up, Pawn, PC);
			}
			if (bHelper)
			{
				Out.Add({Light->GetComponentLocation(), Brightness});
			}
		}
		return Out;
	}

	// A light counted among a thing's own
	void AddLight(FDrawn& D, double Brightness)
	{
		++D.Lights;
		D.Brightness = FMath::Max(D.Brightness, Brightness);
	}

	// How a thing is drawn now. With bTexts, the texts and bars drawn on it too (a walk through its world-space widgets)
	FDrawn DrawnOf(AActor* Actor, const APawn* Pawn, const APlayerController* PC, bool bTexts)
	{
		FDrawn Out;
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
		auto AddText = [&Out](FString Text)
		{
			Text.ReplaceInline(TEXT("<br>"), TEXT(" "));  // (a text render component's line break)
			Text.ReplaceInline(TEXT("\r"), TEXT(" "));
			Text.ReplaceInline(TEXT("\n"), TEXT(" "));
			Text.TrimStartAndEndInline();
			Text = Text.Left(40);
			if (!Text.IsEmpty() && Out.Texts.Num() < 4 && !Out.Texts.Contains(Text))
			{
				Out.Texts.Add(Text);
			}
		};
		// Whether a widget shows: it and every panel it sits in are visible (not collapsed or hidden) and not see-through
		auto Shows = [](const UWidget* Widget)
		{
			for (; Widget; Widget = Widget->GetParent())
			{
				const ESlateVisibility Visibility = Widget->GetVisibility();
				if (Visibility == ESlateVisibility::Collapsed || Visibility == ESlateVisibility::Hidden || Widget->GetRenderOpacity() <= 0.0f)
				{
					return false;
				}
			}
			return true;
		};
		// The texts and bars of a widget drawn in the world, and of the widgets inside it. A bound text or bar reads what
		// its binding gives, as it is drawn (its own field keeps what it was set to last)
		TFunction<void(UUserWidget*, int32)> Walk;
		Walk = [&](UUserWidget* User, int32 Depth)
		{
			if (!User || !User->WidgetTree || Depth > 3 || !Shows(User))
			{
				return;
			}
			User->WidgetTree->ForEachWidget([&](UWidget* Widget)
			{
				if (!Widget || !Shows(Widget))
				{
					return;
				}
				if (UUserWidget* Inner = Cast<UUserWidget>(Widget))
				{
					Walk(Inner, Depth + 1);
				}
				else if (UTextBlock* Block = Cast<UTextBlock>(Widget))
				{
					AddText((Block->TextDelegate.IsBound() ? Block->TextDelegate.Execute() : Block->GetText()).ToString());
				}
				else if (UProgressBar* Bar = Cast<UProgressBar>(Widget))
				{
					const float Percent = Bar->PercentDelegate.IsBound() ? Bar->PercentDelegate.Execute() : Bar->GetPercent();
					if (Out.Bars.Num() < 4)
					{
						Out.Bars.Add(FMath::RoundToDouble(FMath::Clamp((double)Percent, 0.0, 1.0) * 100.0) / 100.0);
					}
				}
			});
		};
		TMap<FString, double> Parts;  // mesh asset name -> how big it is drawn (the biggest, of several)
		auto AddPart = [&Parts](const FString& Name, double Size)
		{
			double& Biggest = Parts.FindOrAdd(Name, Size);
			Biggest = FMath::Max(Biggest, Size);
		};
		TSet<const UActorComponent*> Done;
		for (int32 i = 0; i < Sources.Num(); ++i)
		{
			TInlineComponentArray<UActorComponent*> Components(Sources[i], true);  // (its child actors' too)
			// And the effects attached to its parts that the world owns (a pooled muzzle flash or impact), not to the
			// player's (when the player rides it)
			TArray<USceneComponent*> Children;
			if (USceneComponent* Root = Sources[i]->GetRootComponent())
			{
				Root->GetChildrenComponents(true, Children);
			}
			for (USceneComponent* Child : Children)
			{
				const USceneComponent* Holder = Child ? Child->GetAttachParent() : nullptr;
				if (Child && Child->IsA(UFXSystemComponent::StaticClass()) && !PlayersOwn(Child->GetOwner(), Pawn, PC)
					&& !(Holder && PlayersOwn(Holder->GetOwner(), Pawn, PC)))
				{
					Components.Add(Child);
				}
			}
			for (UActorComponent* Component : Components)
			{
				if (!Component || Done.Contains(Component))
				{
					continue;
				}
				Done.Add(Component);
				USceneComponent* Part = Cast<USceneComponent>(Component);
				if (!Drawn(Part))
				{
					continue;
				}
				if (UFXSystemComponent* Effect = Cast<UFXSystemComponent>(Part))
				{
					UFXSystemAsset* Asset = Effect->GetFXSystemAsset();
					if (Effect->IsActive() && Asset)
					{
						const double Scale = FMath::RoundToDouble(Effect->GetComponentScale().GetAbsMax() * 100.0) / 100.0;
						double& Largest = Out.Effects.FindOrAdd(Asset->GetName(), Scale);
						Largest = FMath::Max(Largest, Scale);
					}
				}
				else if (const ULightComponent* Light = Cast<ULightComponent>(Part))
				{
					const double Brightness = LightBrightness(Light);
					if (Brightness > 0.0)
					{
						AddLight(Out, Brightness);
					}
				}
				else if (const UTextRenderComponent* Label = Cast<UTextRenderComponent>(Part))
				{
					if (bTexts)
					{
						AddText(Label->Text.ToString());
					}
				}
				else if (UWidgetComponent* Widget = Cast<UWidgetComponent>(Part))
				{
					if (bTexts && Widget->GetWidgetSpace() == EWidgetSpace::World)
					{
						Walk(Widget->GetUserWidgetObject(), 0);
					}
				}
				else if (const UMeshComponent* Mesh = Cast<UMeshComponent>(Part))
				{
					if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Mesh))
					{
						if (Static->GetStaticMesh())
						{
							AddPart(Static->GetStaticMesh()->GetName(), Mesh->Bounds.SphereRadius);
						}
					}
					else if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Mesh))
					{
						if (const USkinnedAsset* Asset = Skinned->GetSkinnedAsset())
						{
							AddPart(Asset->GetName(), Mesh->Bounds.SphereRadius);
						}
					}
					if (Mesh->bRenderCustomDepth || Mesh->GetOverlayMaterial())
					{
						Out.bHighlighted = true;
					}
					if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Mesh))
					{
						const UAnimInstance* Anim = Skeletal->GetAnimInstance();
						if (const UAnimMontage* Montage = Anim ? Anim->GetCurrentActiveMontage() : nullptr)
						{
							Out.Animations.AddUnique(Montage->GetName());
						}
						// Limp: a ragdoll simulates (nearly) all its bodies; a ponytail or a cape only a few. Its own
						// meshes and its child actors' (art it spawned), not what is attached to it: a weapon it holds
						// falling isn't it falling
						if (i == 0 && Skeletal->IsAnySimulatingPhysics())
						{
							int32 Simulating = 0;
							for (const FBodyInstance* Body : Skeletal->Bodies)
							{
								if (Body && Body->IsInstanceSimulatingPhysics())
								{
									++Simulating;
								}
							}
							if (Simulating * 2 >= Skeletal->Bodies.Num())
							{
								Out.bLimp = true;
							}
						}
					}
				}
			}
		}
		Parts.GetKeys(Out.Parts);
		Out.Parts.Sort();
		Out.Biggest = Out.Parts;
		Out.Biggest.Sort([&Parts](const FString& A, const FString& B) { return Parts.FindChecked(A) > Parts.FindChecked(B); });
		Out.Biggest.SetNum(FMath::Min(Out.Biggest.Num(), 8));
		Out.Biggest.Sort();
		return Out;
	}

	// Counts among a thing's lights those placed at it (see PlacedLights): within its own box grown a little (see
	// FPlaytestShape::Holds). Not for a thing without a body (a helper itself, whose own lights are its look's already)
	void AddPlacedLights(FDrawn& D, const AActor* Actor, const TArray<FPlacedLight>& Placed)
	{
		if (Placed.Num() == 0)
		{
			return;
		}
		FBox Visible, Local;
		BodyBounds(Actor, Visible, Local);
		if (!Visible.IsValid)
		{
			return;
		}
		FVector Origin, Extent;
		Visible.GetCenterAndExtents(Origin, Extent);
		const FPlaytestShape Shape = ShapeOf(Actor, Local, Origin, Extent);
		for (const FPlacedLight& Light : Placed)
		{
			if (Shape.Holds(Light.At, PlacedLightReach))
			{
				AddLight(D, Light.Brightness);
			}
		}
	}

	// A thing's look as `view` tells it: only what it has (nothing for a plain box)
	TSharedPtr<FJsonObject> LookJson(const FDrawn& D)
	{
		TSharedRef<FJsonObject> Look = MakeShared<FJsonObject>();
		if (D.Biggest.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Parts;
			for (const FString& Part : D.Biggest)
			{
				Parts.Add(MakeShared<FJsonValueString>(Part));
			}
			Look->SetArrayField(TEXT("parts"), Parts);
		}
		if (D.Effects.Num() > 0)
		{
			TArray<FString> Names;
			D.Effects.GetKeys(Names);
			Names.Sort();
			TArray<TSharedPtr<FJsonValue>> Effects;
			for (int32 i = 0; i < Names.Num() && i < 6; ++i)
			{
				TSharedRef<FJsonObject> Effect = MakeShared<FJsonObject>();
				Effect->SetStringField(TEXT("asset"), Names[i]);
				Effect->SetNumberField(TEXT("scale"), D.Effects[Names[i]]);
				Effects.Add(Obj(Effect));
			}
			Look->SetArrayField(TEXT("effects"), Effects);
		}
		if (D.Lights > 0)
		{
			TSharedRef<FJsonObject> Lights = MakeShared<FJsonObject>();
			Lights->SetNumberField(TEXT("on"), D.Lights);
			Lights->SetNumberField(TEXT("brightness"), FMath::RoundToDouble(D.Brightness * 100.0) / 100.0);
			Look->SetObjectField(TEXT("lights"), Lights);
		}
		if (D.Animations.Num() > 0)
		{
			Look->SetStringField(TEXT("animation"), D.Animations[0]);
		}
		if (D.bLimp)
		{
			Look->SetBoolField(TEXT("limp"), true);
		}
		if (D.Texts.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Texts;
			for (const FString& Text : D.Texts)
			{
				Texts.Add(MakeShared<FJsonValueString>(Text));
			}
			Look->SetArrayField(TEXT("texts"), Texts);
		}
		if (D.Bars.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Bars;
			for (const double Bar : D.Bars)
			{
				Bars.Add(MakeShared<FJsonValueNumber>(Bar));
			}
			Look->SetArrayField(TEXT("bars"), Bars);
		}
		if (D.bHighlighted)
		{
			Look->SetBoolField(TEXT("highlighted"), true);
		}
		if (Look->Values.Num() == 0)
		{
			return nullptr;
		}
		return Look;
	}
}
