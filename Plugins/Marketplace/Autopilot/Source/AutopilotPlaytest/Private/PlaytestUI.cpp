#include "PlaytestInternal.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/Slider.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/DragAndDrop.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "Layout/ArrangedChildren.h"
#include "InputCoreTypes.h"
#include "Slate/SObjectWidget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace PlaytestUI
{
namespace
{
	/** An interactive element found at the last listing, to act on by its id. */
	struct FElement
	{
		FString Id;
		FString Kind;      // button, checkbox, combo, slider, text_input, widget
		FString Name;      // the UMG widget's name (e.g. BtnSinglePlayerMode)
		FString Owner;     // the user widget it belongs to (e.g. UI_MainMenu)
		FString Label;     // visible texts inside it
		bool bEnabled = true;
		TWeakObjectPtr<UWidget> Widget;
		TWeakPtr<SWidget> Slate;
		TWeakPtr<SObjectWidget> Handler;  // the nearest user widget that takes mouse and drag events
		TWeakObjectPtr<UButton> Button;   // a user widget's own single button (e.g. an inventory slot's): clicks go to it
	};

	TArray<FElement> Last;

	FString ShortName(const UObject* Object)
	{
		FString Name = Object->GetName();
		const int32 Index = Name.Find(TEXT("_C_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		return Index != INDEX_NONE ? Name.Left(Index) : Name;
	}

	/** Whether a user widget's class (a Blueprint) handles the mouse or drag and drop itself: then it is
	 *  something to click or drag, like an inventory slot, even without a button in it. */
	bool HandlesInput(const UUserWidget* User)
	{
		static const FName Handlers[] = {
			TEXT("OnMouseButtonDown"), TEXT("OnMouseButtonUp"), TEXT("OnMouseButtonDoubleClick"),
			TEXT("OnDragDetected"), TEXT("OnDrop"), TEXT("OnPreviewMouseButtonDown"),
		};
		const UClass* Class = User->GetClass();
		for (const FName& Name : Handlers)
		{
			const UFunction* Function = Class->FindFunctionByName(Name);
			if (Function && Function->GetOuterUClass() != UUserWidget::StaticClass())
			{
				return true;
			}
		}
		return false;
	}

	/** The visible texts in a widget, then the names of the images in it (an icon says what an item slot
	 *  holds when no text does, e.g. [T_Icon_Stone]). */
	void Texts(const TSharedRef<SWidget>& Widget, TArray<FString>& Out, const TMap<const SWidget*, UWidget*>& Native,
		TArray<FString>& Icons, int32 Depth = 0)
	{
		if (Depth > 32 || Out.Num() >= 4 || !Widget->GetVisibility().IsVisible())
		{
			return;
		}
		if (UWidget* const* W = Native.Find(&Widget.Get()))
		{
			if (const UImage* Image = Cast<UImage>(*W))
			{
				const UObject* Resource = Image->GetBrush().GetResourceObject();
				const FString Name = Resource ? Resource->GetName() : FString();
				if (!Name.IsEmpty() && Icons.Num() < 2 && !Icons.Contains(Name))
				{
					Icons.Add(Name);
				}
			}
		}
		static const FName TextBlockType(TEXT("STextBlock"));
		if (Widget->GetType() == TextBlockType)
		{
			const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString().TrimStartAndEnd();
			if (!Text.IsEmpty() && !Out.Contains(Text))
			{
				Out.Add(Text.Left(60));
			}
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				Texts(Children->GetChildAt(i), Out, Native, Icons, Depth + 1);
			}
		}
	}

	struct FLister
	{
		TMap<const SWidget*, UWidget*> Native;  // Slate widget -> the UMG widget that made it
		TArray<FElement> Out;
		TSet<FString> Ids;

		FString UniqueId(const FString& Base)
		{
			FString Id = Base;
			for (int32 i = 2; Ids.Contains(Id); ++i)
			{
				Id = FString::Printf(TEXT("%s#%d"), *Base, i);
			}
			Ids.Add(Id);
			return Id;
		}

		void Add(const TSharedRef<SWidget>& Slate, UWidget* Widget, const FString& Kind, const FString& Owner,
			const TSharedPtr<SObjectWidget>& Handler)
		{
			FElement E;
			E.Kind = Kind;
			E.Name = Widget ? ShortName(Widget) : Slate->GetTypeAsString();
			E.Owner = Owner;
			E.Id = UniqueId(Owner.IsEmpty() || Owner == E.Name ? E.Name : Owner + TEXT(".") + E.Name);
			TArray<FString> Found, Icons;
			Texts(Slate, Found, Native, Icons);
			for (const FString& Icon : Icons)
			{
				Found.Add(FString::Printf(TEXT("[%s]"), *Icon));
			}
			E.Label = FString::Join(Found, TEXT(" / "));
			E.bEnabled = Slate->IsEnabled() && (!Widget || Widget->GetIsEnabled());
			E.Widget = Widget;
			E.Slate = Slate;
			E.Handler = Handler;
			Out.Add(MoveTemp(E));
		}

		/** A button directly inside a user widget that is itself an element (e.g. an inventory slot) is that
		 *  element's button rather than an element of its own: one entry per slot, clicked through its button,
		 *  right-clicked and dragged through the widget. */
		bool Merge(UWidget* W, const FString& Owner, const TSharedPtr<SObjectWidget>& Handler)
		{
			UButton* Button = Cast<UButton>(W);
			if (!Button || !Handler.IsValid())
			{
				return false;
			}
			for (int32 i = Out.Num() - 1; i >= 0; --i)
			{
				FElement& E = Out[i];
				if (E.Slate.Pin() == StaticCastSharedPtr<SWidget>(Handler))
				{
					if (E.Button.IsValid() || !E.Widget.IsValid() || Owner != ShortName(E.Widget.Get()))
					{
						return false;
					}
					E.Button = Button;
					return true;
				}
			}
			return false;
		}

		void Walk(const TSharedRef<SWidget>& Widget, const FString& Owner, const TSharedPtr<SObjectWidget>& Handler, int32 Depth)
		{
			if (Depth > 96 || !Widget->GetVisibility().IsVisible() || Widget->GetRenderOpacity() <= 0.f)
			{
				return;
			}
			static const FName ObjectWidgetType(TEXT("SObjectWidget"));
			FString NextOwner = Owner;
			TSharedPtr<SObjectWidget> NextHandler = Handler;
			if (Widget->GetType() == ObjectWidgetType)
			{
				TSharedRef<SObjectWidget> Object = StaticCastSharedRef<SObjectWidget>(Widget);
				if (UUserWidget* User = Object->GetWidgetObject())
				{
					if (HandlesInput(User))
					{
						Add(Widget, User, TEXT("widget"), Owner, Object);
						NextHandler = Object;
					}
					NextOwner = ShortName(User);
				}
			}
			else if (UWidget** Found = Native.Find(&Widget.Get()))
			{
				UWidget* W = *Found;
				const TCHAR* Kind =
					W->IsA<UButton>() ? TEXT("button") :
					W->IsA<UCheckBox>() ? TEXT("checkbox") :
					W->IsA<UComboBoxString>() ? TEXT("combo") :
					W->IsA<USlider>() ? TEXT("slider") :
					(W->IsA<UEditableTextBox>() || W->IsA<UEditableText>()) ? TEXT("text_input") : nullptr;
				if (Kind && Merge(W, Owner, Handler))
				{
					Kind = nullptr;  // the button is its user widget's own: clicking the widget clicks it
				}
				if (Kind)
				{
					Add(Widget, W, Kind, Owner, Handler);
				}
			}
			if (FChildren* Children = Widget->GetChildren())
			{
				for (int32 i = 0; i < Children->Num(); ++i)
				{
					Walk(Children->GetChildAt(i), NextOwner, NextHandler, Depth + 1);
				}
			}
		}
	};

	FPointerEvent Pointer(const FGeometry& Geometry, const FKey& Button, bool bDown)
	{
		const FVector2D At = Geometry.GetAbsolutePositionAtCoordinates(FVector2D(0.5, 0.5));
		TSet<FKey> Pressed;
		if (bDown)
		{
			Pressed.Add(Button);
		}
		return FPointerEvent(0, At, At, Pressed, Button, 0.f, FModifierKeysState());
	}

	/** The element as the screen is now: listed afresh, so an element that was hidden or disabled since
	 *  (e.g. a "+" button that disappears when no points are left) can't be acted on, as for a player. */
	const FElement* Find(UWorld* World, const FString& Id)
	{
		List(World);
		for (const FElement& E : Last)
		{
			if (E.Id == Id && E.Slate.IsValid())
			{
				return &E;
			}
		}
		return nullptr;
	}

	/** Mouse events straight to the widget (and the user widget handling them): headless games paint
	 *  nothing, so Slate can't route a pointer by position, but widgets take the events all the same. */
	void Mouse(const FElement& E, const FKey& Button, bool bDouble)
	{
		TSharedPtr<SWidget> Target = E.Handler.IsValid() && E.Kind != TEXT("button") ? TSharedPtr<SWidget>(E.Handler.Pin()) : E.Slate.Pin();
		if (!Target.IsValid())
		{
			return;
		}
		const FGeometry Geometry = Target->GetCachedGeometry();
		if (bDouble)
		{
			Target->OnMouseButtonDoubleClick(Geometry, Pointer(Geometry, Button, true));
		}
		else
		{
			Target->OnMouseButtonDown(Geometry, Pointer(Geometry, Button, true));
		}
		Target->OnMouseButtonUp(Geometry, Pointer(Geometry, Button, false));
	}
}

TArray<TSharedPtr<FJsonValue>> List(UWorld* World)
{
	FLister Lister;
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	TSharedPtr<SViewport> Root = Viewport ? Viewport->GetGameViewportWidget() : nullptr;
	TArray<TSharedPtr<FJsonValue>> Out;
	if (!Root.IsValid())
	{
		Last.Reset();
		return Out;
	}
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* User = *It;
		if (!IsValid(User) || User->GetWorld() != World || !User->WidgetTree)
		{
			continue;
		}
		User->WidgetTree->ForEachWidget([&Lister](UWidget* Child)
		{
			if (TSharedPtr<SWidget> Cached = Child ? Child->GetCachedWidget() : nullptr)
			{
				Lister.Native.Add(Cached.Get(), Child);
			}
		});
	}
	Lister.Walk(Root.ToSharedRef(), FString(), nullptr, 0);
	Last = Lister.Out;
	for (const FElement& E : Last)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("id"), E.Id);
		Item->SetStringField(TEXT("kind"), E.Kind);
		Item->SetStringField(TEXT("name"), E.Name);
		Item->SetStringField(TEXT("owner"), E.Owner);
		Item->SetStringField(TEXT("label"), E.Label);
		Item->SetBoolField(TEXT("enabled"), E.bEnabled);
		UWidget* W = E.Widget.Get();
		if (const UCheckBox* Check = Cast<UCheckBox>(W))
		{
			Item->SetBoolField(TEXT("checked"), Check->IsChecked());
		}
		else if (const UComboBoxString* Combo = Cast<UComboBoxString>(W))
		{
			Item->SetStringField(TEXT("selected"), Combo->GetSelectedOption());
			TArray<TSharedPtr<FJsonValue>> Options;
			for (int32 i = 0; i < Combo->GetOptionCount() && i < 20; ++i)
			{
				Options.Add(MakeShared<FJsonValueString>(Combo->GetOptionAtIndex(i)));
			}
			Item->SetArrayField(TEXT("options"), Options);
		}
		else if (const USlider* Slider = Cast<USlider>(W))
		{
			Item->SetNumberField(TEXT("value"), Slider->GetValue());
			Item->SetNumberField(TEXT("min"), Slider->GetMinValue());
			Item->SetNumberField(TEXT("max"), Slider->GetMaxValue());
		}
		else if (const UEditableTextBox* Box = Cast<UEditableTextBox>(W))
		{
			// What is typed in it now, and its hint (as Unity's and Godot's text inputs)
			Item->SetStringField(TEXT("text"), Box->GetText().ToString());
			Item->SetStringField(TEXT("placeholder"), Box->GetHintText().ToString());
		}
		else if (const UEditableText* Edit = Cast<UEditableText>(W))
		{
			Item->SetStringField(TEXT("text"), Edit->GetText().ToString());
			Item->SetStringField(TEXT("placeholder"), Edit->GetHintText().ToString());
		}
		Out.Add(MakeShared<FJsonValueObject>(Item));
	}
	return Out;
}

FString ElementAt(UWorld* World, const FVector2D& Pixel, const FVector2D& Size)
{
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	TSharedPtr<SViewport> Root = Viewport ? Viewport->GetGameViewportWidget() : nullptr;
	if (!Root.IsValid() || Size.X < 1.0 || Size.Y < 1.0)
	{
		return FString();
	}
	List(World);
	TMap<const SWidget*, int32> Listed;  // each element's widget -> its index in Last
	for (int32 i = 0; i < Last.Num(); ++i)
	{
		if (TSharedPtr<SWidget> Slate = Last[i].Slate.Pin())
		{
			Listed.Add(Slate.Get(), i);
		}
	}
	// Laid out as Slate would over a view of that size (desired sizes first), the topmost element under the pixel:
	// the last one met in paint order
	Root->SlatePrepass(1.f);
	int32 Found = INDEX_NONE;
	TFunction<void(const TSharedRef<SWidget>&, const FGeometry&, int32)> Visit;
	Visit = [&](const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, int32 Depth)
	{
		if (Depth > 96 || !Widget->GetVisibility().IsVisible() || Widget->GetRenderOpacity() <= 0.f)
		{
			return;
		}
		if (const int32* Index = Listed.Find(&Widget.Get()))
		{
			if (Geometry.IsUnderLocation(Pixel))
			{
				Found = *Index;
			}
		}
		FArrangedChildren Arranged(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Arranged);
		for (int32 i = 0; i < Arranged.Num(); ++i)
		{
			Visit(Arranged[i].Widget, Arranged[i].Geometry, Depth + 1);
		}
	};
	Visit(Root.ToSharedRef(), FGeometry::MakeRoot(Size, FSlateLayoutTransform()), 0);
	return Found != INDEX_NONE ? Last[Found].Id : FString();
}

FString Act(UWorld* World, const TSharedPtr<FJsonObject>& Args)
{
	FString Id, Action = TEXT("click");
	Args->TryGetStringField(TEXT("id"), Id);
	Args->TryGetStringField(TEXT("action"), Action);
	const FElement* Found = Find(World, Id);
	if (!Found)
	{
		return FString::Printf(TEXT("no element %s on screen"), *Id);
	}
	const FElement E = *Found;  // a listing below may replace Last
	if (!E.bEnabled)
	{
		return FString::Printf(TEXT("%s is disabled"), *Id);
	}
	UWidget* W = E.Widget.Get();
	if (Action == TEXT("click"))
	{
		UButton* Own = E.Button.Get();
		if (UButton* Button = Own ? Own : Cast<UButton>(W))
		{
			// As a real click: pressed, released, clicked
			Button->OnPressed.Broadcast();
			Button->OnReleased.Broadcast();
			TSharedPtr<SWidget> Slate = Own ? Own->GetCachedWidget() : E.Slate.Pin();
			if (Slate.IsValid() && Slate->GetType() == FName(TEXT("SButton")))
			{
				StaticCastSharedPtr<SButton>(Slate)->SimulateClick();
			}
			else
			{
				Button->OnClicked.Broadcast();
			}
		}
		else if (UCheckBox* Check = Cast<UCheckBox>(W))
		{
			const bool bChecked = !Check->IsChecked();
			Check->SetIsChecked(bChecked);
			Check->OnCheckStateChanged.Broadcast(bChecked);
		}
		else if (UComboBoxString* Combo = Cast<UComboBoxString>(W))
		{
			FString Option;
			if (!Args->TryGetStringField(TEXT("option"), Option) && Combo->GetOptionCount() > 0)
			{
				Option = Combo->GetOptionAtIndex((Combo->GetSelectedIndex() + 1) % Combo->GetOptionCount());
			}
			if (Combo->FindOptionIndex(Option) == INDEX_NONE)
			{
				return FString::Printf(TEXT("%s has no option %s"), *Id, *Option);  // (as Unity's and Godot's)
			}
			Combo->SetSelectedOption(Option);
		}
		else
		{
			Mouse(E, EKeys::LeftMouseButton, false);
		}
	}
	else if (Action == TEXT("right_click"))
	{
		Mouse(E, EKeys::RightMouseButton, false);
	}
	else if (Action == TEXT("double_click"))
	{
		Mouse(E, EKeys::LeftMouseButton, true);
	}
	else if (Action == TEXT("set"))
	{
		if (UCheckBox* Check = Cast<UCheckBox>(W))
		{
			// {"value": true|false}: checked or not (as Unity's and Godot's), whatever it was
			bool bValue = !Check->IsChecked();
			Args->TryGetBoolField(TEXT("value"), bValue);
			if (bValue != Check->IsChecked())
			{
				Check->SetIsChecked(bValue);
				Check->OnCheckStateChanged.Broadcast(bValue);
			}
		}
		else if (UComboBoxString* Combo = Cast<UComboBoxString>(W))
		{
			// {"option"} (or {"text"}): that option picked
			FString Option;
			if (!Args->TryGetStringField(TEXT("option"), Option))
			{
				Args->TryGetStringField(TEXT("text"), Option);
			}
			if (Combo->FindOptionIndex(Option) == INDEX_NONE)
			{
				return FString::Printf(TEXT("%s has no option %s"), *Id, *Option);
			}
			Combo->SetSelectedOption(Option);
		}
		else if (USlider* Slider = Cast<USlider>(W))
		{
			const float Value = (float)Args->GetNumberField(TEXT("value"));
			Slider->SetValue(Value);
			Slider->OnValueChanged.Broadcast(Value);
		}
		else if (UEditableTextBox* Box = Cast<UEditableTextBox>(W))
		{
			const FText Text = FText::FromString(Args->GetStringField(TEXT("text")));
			Box->SetText(Text);
			Box->OnTextChanged.Broadcast(Text);
			Box->OnTextCommitted.Broadcast(Text, ETextCommit::OnEnter);
		}
		else if (UEditableText* Edit = Cast<UEditableText>(W))
		{
			const FText Text = FText::FromString(Args->GetStringField(TEXT("text")));
			Edit->SetText(Text);
			Edit->OnTextChanged.Broadcast(Text);
			Edit->OnTextCommitted.Broadcast(Text, ETextCommit::OnEnter);
		}
		else
		{
			return FString::Printf(TEXT("%s takes no value"), *Id);
		}
	}
	else if (Action == TEXT("drag"))
	{
		FString ToId;
		Args->TryGetStringField(TEXT("to"), ToId);
		const FElement* ToFound = Find(World, ToId);
		if (!ToFound)
		{
			return FString::Printf(TEXT("no element %s on screen to drop on"), *ToId);
		}
		const FElement To = *ToFound;
		TSharedPtr<SObjectWidget> Source = E.Handler.Pin();
		TSharedPtr<SObjectWidget> Target = To.Handler.Pin();
		if (!Source.IsValid() || !Target.IsValid())
		{
			return TEXT("drag and drop needs widgets that handle it on both ends");
		}
		// As Slate does it: press on the source, a drag is detected, then enter, over and drop on the target
		const FGeometry From = Source->GetCachedGeometry();
		Source->OnMouseButtonDown(From, Pointer(From, EKeys::LeftMouseButton, true));
		const FReply Reply = Source->OnDragDetected(From, Pointer(From, EKeys::LeftMouseButton, true));
		TSharedPtr<FDragDropOperation> Operation = Reply.GetDragDropContent();
		if (!Operation.IsValid())
		{
			Source->OnMouseButtonUp(From, Pointer(From, EKeys::LeftMouseButton, false));
			return FString::Printf(TEXT("%s can't be dragged"), *Id);
		}
		const FGeometry Onto = Target->GetCachedGeometry();
		const FDragDropEvent Event(Pointer(Onto, EKeys::LeftMouseButton, false), Operation);
		Target->OnDragEnter(Onto, Event);
		Target->OnDragOver(Onto, Event);
		const bool bHandled = Target->OnDrop(Onto, Event).IsEventHandled();
		Operation->OnDrop(bHandled, Pointer(Onto, EKeys::LeftMouseButton, false));
		if (!bHandled)
		{
			return FString::Printf(TEXT("%s didn't take the drop"), *ToId);
		}
	}
	else
	{
		return FString::Printf(TEXT("unknown action %s"), *Action);
	}
	return FString();
}
}
