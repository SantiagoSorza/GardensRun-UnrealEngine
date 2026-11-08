#include "PlaytestInternal.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/Geometry.h"
#include "Layout/SlateRect.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Slate/SObjectWidget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace PlaytestScreen
{
namespace
{
	FString WidgetName(const UUserWidget* Widget)
	{
		// WBP_PlayerHUD_C_0 -> WBP_PlayerHUD: the widget Blueprint's name, without the instance suffix
		FString Name = Widget->GetName();
		int32 Index = Name.Find(TEXT("_C_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Index != INDEX_NONE)
		{
			Name = Name.Left(Index);
		}
		return Name;
	}

	/** A text found on screen, in the order texts are painted (a later one is drawn over an earlier one), with the
	 *  rectangle it is painted in when its place is known. */
	struct FFoundText
	{
		FString Key;
		FString Text;
		FSlateRect Rect;
		bool bPlaced = false;
	};

	/** A child widget by its index among its parent's children, and where it is laid out (null: not known). */
	struct FChildPlace
	{
		int32 Index;
		const FGeometry* Geometry;
	};

	/** Whether a text painted over another hides it: it covers at least half of it (or lies in the same rectangle,
	 *  as when nothing measured either). */
	bool Covers(const FSlateRect& Above, const FSlateRect& Below)
	{
		if (Above == Below)
		{
			return true;
		}
		const float Wide = FMath::Min(Above.Right, Below.Right) - FMath::Max(Above.Left, Below.Left);
		const float High = FMath::Min(Above.Bottom, Below.Bottom) - FMath::Max(Above.Top, Below.Top);
		return Wide > 0.f && High > 0.f && Wide * High >= 0.5f * Below.GetArea();
	}

	/** Where the game viewport is, to lay out what is in it from: as last painted, else (nothing paints without a
	 *  renderer) the viewport's size. */
	FGeometry ViewportGeometry(UGameViewportClient* Viewport, const TSharedRef<SViewport>& Root)
	{
		const FGeometry& Painted = Root->GetPaintSpaceGeometry();
		if (Painted.GetLocalSize().X > 0.f && Painted.GetLocalSize().Y > 0.f)
		{
			return Painted;
		}
		FVector2D Size(0.0, 0.0);
		Viewport->GetViewportSize(Size);
		if (Size.X <= 0.0 || Size.Y <= 0.0)
		{
			Size = FVector2D(1920.0, 1080.0);  // any screen will do: only which texts lie over which is told from it
		}
		return FGeometry::MakeRoot(FVector2f(Size), FSlateLayoutTransform());
	}

	struct FWalker
	{
		TMap<const SWidget*, FString> Named;
		TArray<FFoundText> Found;

		void Add(const FString& Key, const FString& Text, const FGeometry* Geometry)
		{
			FFoundText& Item = Found.AddDefaulted_GetRef();
			Item.Key = Key;
			Item.Text = Text;
			if (Geometry)
			{
				Item.Rect = Geometry->GetRenderBoundingRect();
				Item.bPlaced = true;
			}
		}

		/** The texts as a player sees them, each under its own key (Key, Key#2, ...): of copies of one text painted
		 *  over each other (a widget added to the screen again each time the game starts over, the new copy on top of
		 *  the old), only the top one. */
		TArray<FShownText> Shown() const
		{
			TArray<FShownText> Out;
			TSet<FString> Keys;
			for (int32 i = 0; i < Found.Num(); ++i)
			{
				const FFoundText& Item = Found[i];
				bool bHidden = false;
				for (int32 j = i + 1; j < Found.Num() && !bHidden; ++j)
				{
					bHidden = Item.bPlaced && Found[j].bPlaced && Found[j].Key == Item.Key && Covers(Found[j].Rect, Item.Rect);
				}
				if (bHidden)
				{
					continue;
				}
				FString Unique = Item.Key;
				for (int32 n = 2; Keys.Contains(Unique); ++n)
				{
					Unique = FString::Printf(TEXT("%s#%d"), *Item.Key, n);
				}
				Keys.Add(Unique);
				Out.Add({Unique, Item.Text});
			}
			return Out;
		}

		void Walk(const TSharedRef<SWidget>& Widget, const FGeometry* Geometry, const FString& Path, const FString& Owner, int32 Depth)
		{
			if (Depth > 64 || !Widget->GetVisibility().IsVisible() || Widget->GetRenderOpacity() <= 0.f)
			{
				return;
			}
			static const FName TextBlockType(TEXT("STextBlock"));
			static const FName ObjectWidgetType(TEXT("SObjectWidget"));
			const FName Type = Widget->GetType();
			FString NextPath = Path;
			FString NextOwner = Owner;
			if (Type == TextBlockType)
			{
				const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
				if (!Text.TrimStartAndEnd().IsEmpty())
				{
					if (const FString* Name = Named.Find(&Widget.Get()))
					{
						Add(*Name, Text, Geometry);
					}
					else if (Widget->GetTag() != NAME_None)
					{
						Add(FString::Printf(TEXT("%s.%s"), *Owner, *Widget->GetTag().ToString()), Text, Geometry);
					}
					else
					{
						// Where the widget was created in code (e.g. MyPlayerController.cpp(85)) names it better
						// than its place in the tree, and stays the same when the layout around it changes
						const FString Location = Widget->GetReadableLocation();
						if (!Location.IsEmpty() && Location.Contains(TEXT("(")))
						{
							Add(FString::Printf(TEXT("%s.%s"), *Owner, *Location), Text, Geometry);
						}
						else
						{
							Add(Path, Text, Geometry);
						}
					}
				}
			}
			else if (Type == ObjectWidgetType)
			{
				if (UUserWidget* User = StaticCastSharedRef<SObjectWidget>(Widget)->GetWidgetObject())
				{
					NextOwner = WidgetName(User);
					NextPath = NextOwner;
				}
			}
			FChildren* Children = Widget->GetChildren();
			if (!Children)
			{
				return;
			}
			// The children in the order they are painted, each where it is: as the widget lays out those it shows (a
			// canvas by their z-order), then any others, with no place known
			FArrangedChildren Arranged(EVisibility::Visible);
			if (Geometry)
			{
				Widget->ArrangeChildren(*Geometry, Arranged);
			}
			TMap<const SWidget*, int32> Index;
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				Index.Add(&Children->GetChildAt(i).Get(), i);
			}
			TArray<FChildPlace> Order;
			TSet<int32> Placed;
			for (const FArrangedWidget& Child : Arranged.GetInternalArray())
			{
				const int32* At = Index.Find(&Child.Widget.Get());
				if (At && !Placed.Contains(*At))
				{
					Placed.Add(*At);
					Order.Add({*At, &Child.Geometry});
				}
			}
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				if (!Placed.Contains(i))
				{
					Order.Add({i, nullptr});
				}
			}
			for (const FChildPlace& Child : Order)
			{
				Walk(Children->GetChildAt(Child.Index), Child.Geometry, FString::Printf(TEXT("%s.%d"), *NextPath, Child.Index),
					NextOwner, Depth + 1);
			}
		}
	};
}

TArray<FShownText> Read(UWorld* World)
{
	FWalker Walker;
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (!Viewport)
	{
		return {};
	}
	TSharedPtr<SViewport> Root = Viewport->GetGameViewportWidget();
	if (!Root.IsValid())
	{
		return {};
	}
	// UMG text blocks by name: <UserWidget>.<TextBlock>
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* User = *It;
		if (!IsValid(User) || User->GetWorld() != World || !User->WidgetTree)
		{
			continue;
		}
		const FString Owner = WidgetName(User);
		User->WidgetTree->ForEachWidget([&Walker, &Owner](UWidget* Child)
		{
			if (UTextBlock* Text = Cast<UTextBlock>(Child))
			{
				TSharedPtr<SWidget> Cached = Text->GetCachedWidget();
				if (Cached.IsValid())
				{
					Walker.Named.Add(Cached.Get(), FString::Printf(TEXT("%s.%s"), *Owner, *Text->GetName()));
				}
			}
		});
	}
	const FGeometry RootGeometry = ViewportGeometry(Viewport, Root.ToSharedRef());
	Walker.Walk(Root.ToSharedRef(), &RootGeometry, TEXT("Slate"), TEXT("Slate"), 0);
	return Walker.Shown();
}

void TickUnpainted(UWorld* World, float DeltaTime)
{
	static const bool bRenders = FApp::CanEverRender() && !FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
	if (bRenders || !World)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* User = *It;
		if (!IsValid(User) || User->GetWorld() != World)
		{
			continue;
		}
		TSharedPtr<SWidget> Cached = User->GetCachedWidget();
		if (Cached.IsValid())
		{
			Cached->Tick(FGeometry(), Now, DeltaTime);  // SObjectWidget::Tick runs the widget's NativeTick / Event Tick
		}
	}
	// Slate evaluates bound attributes (a panel's visibility or a text bound to a Blueprint function) in the
	// prepass before painting. Nothing paints here, so run the prepass: otherwise every binding keeps its
	// design-time value (e.g. every menu panel reads as open).
	if (UGameViewportClient* Viewport = World->GetGameViewport())
	{
		if (TSharedPtr<SViewport> Root = Viewport->GetGameViewportWidget())
		{
			Root->SlatePrepass(1.f);
		}
	}
}
}
