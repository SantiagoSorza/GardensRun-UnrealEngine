// `pointer` (as the Unity and Godot plugins answer it): the mouse goes to a place on the game's view without moving
// across to it, and the mouse buttons pressed after it click there. The place is the game viewport's own cursor (what
// GetMousePosition, GetHitResultUnderCursor, DeprojectMousePositionToWorld and the player controller's click events
// read), set as a mouse event over the view sets it: the desktop's cursor stays where it is. A button pressed while
// the game's UI (UMG) is under the pointer goes to that UI as the mouse's click would (Slate routes it); anywhere else
// it goes to the player controller as before, which reads the place set here.

#include "PlaytestOps.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Input/Events.h"
#include "Layout/WidgetPath.h"
#include "Misc/App.h"
#include "PlaytestInternal.h"
#include "PlaytestJson.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace PlaytestOps
{
namespace
{
	struct FPointerState
	{
		bool bSet = false;
		FVector2D Pixel = FVector2D::ZeroVector;  // in the game's view: pixels from its top left
		TWeakObjectPtr<UGameViewportClient> Client;
		TSet<FKey> Pressed;  // the mouse buttons down (as a pointer event lists them)
		TSet<FKey> UiDown;   // those a press sent to the game's UI: let go there too
		TMap<FKey, FString> ElementDown;  // a game that doesn't render: the UI element a button went down on (by id)
		bool bVirtual = false;  // a game that doesn't render: the pointer is on a view of HeadlessSize
	};
	FPointerState Pointer;
	const FIntPoint HeadlessSize(1280, 720);  // a game that never renders: the view's size for the mouse

	FSceneViewport* SceneViewport(UGameViewportClient* Client)
	{
		return Client ? Client->GetGameViewport() : nullptr;
	}

	FInputDeviceId Device()
	{
		return IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	}

	/** Where a pixel of the game's view is on the desktop, as Slate places its events; false when the view isn't laid
	 *  out (a game that doesn't render: nothing can be under the pointer but the world). */
	bool Absolute(UGameViewportClient* Client, const FVector2D& Pixel, FVector2D& Out)
	{
		TSharedPtr<SViewport> Widget = Client ? Client->GetGameViewportWidget() : nullptr;
		if (!Widget.IsValid())
		{
			return false;
		}
		const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
		const FVector2D Size(Geometry.GetLocalSize());
		if (Size.X < 1.0 || Size.Y < 1.0)
		{
			return false;
		}
		const double Scale = Geometry.Scale > 0.f ? Geometry.Scale : 1.0;
		Out = FVector2D(Geometry.LocalToAbsolute(FVector2D(Pixel.X / Scale, Pixel.Y / Scale)));
		return true;
	}

	/** The game reads the mouse at the pointer: a mouse move over the view, at the pointer, with no movement (the view's
	 *  own geometry, so it keeps it; FSceneViewport scales the place by its DPI scale: undone here, so the cursor lands on
	 *  the pixel). Only when the view's cursor isn't there already (a move of the desktop's mouse over the view moved it):
	 *  a view holding the mouse puts the desktop's cursor back at each move. */
	void Apply(bool bForce = false)
	{
		FSceneViewport* View = SceneViewport(Pointer.Client.Get());
		if (!Pointer.bSet || !View)
		{
			return;
		}
		const FIntPoint Want(FMath::RoundToInt(Pointer.Pixel.X), FMath::RoundToInt(Pointer.Pixel.Y));
		FIntPoint Now;
		View->GetMousePos(Now, true);
		if (Now == Want && !bForce)
		{
			return;
		}
		const FGeometry Geometry = View->GetCachedGeometry();
		const double Scale = Geometry.Scale > 0.f ? Geometry.Scale : 1.0;
		const FVector2D Abs(Geometry.LocalToAbsolute(FVector2D(Pointer.Pixel.X / Scale, Pointer.Pixel.Y / Scale)));
		const FPointerEvent Event(Device(), FSlateApplication::CursorPointerIndex, Abs, Abs, Pointer.Pressed, EKeys::Invalid, 0.f,
			FModifierKeysState());
		View->OnMouseMove(Geometry, Event);
	}

	/** The game's UI under the pointer (a widget inside the view's widget, past it): its path; false for the world. */
	bool UiUnder(UGameViewportClient* Client, const FVector2D& Abs)
	{
		TSharedPtr<SViewport> ViewWidget = Client ? Client->GetGameViewportWidget() : nullptr;
		if (!ViewWidget.IsValid())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const FWidgetPath Path = Slate.LocateWindowUnderMouse(Abs, Slate.GetInteractiveTopLevelWindows(), false, 0);
		if (!Path.IsValid())
		{
			return false;
		}
		for (int32 i = 0; i < Path.Widgets.Num(); ++i)
		{
			if (&Path.Widgets[i].Widget.Get() == ViewWidget.Get())
			{
				return i < Path.Widgets.Num() - 1;  // (the view itself the deepest: the world)
			}
		}
		return false;  // (not over the game's view: nothing of the game's there)
	}

	FPointerEvent ButtonEvent(const FVector2D& Abs, const FKey& Button)
	{
		return FPointerEvent(Device(), FSlateApplication::CursorPointerIndex, Abs, Abs, Pointer.Pressed, Button, 0.f, FModifierKeysState());
	}

	void Forget()
	{
		Pointer.bSet = false;
		Pointer.Client.Reset();
		Pointer.Pressed.Reset();
		Pointer.UiDown.Reset();
		Pointer.ElementDown.Reset();
		Pointer.bVirtual = false;
	}

	/** A game that doesn't render: a button going up over the game's UI acts on the element under the pointer as
	 *  `ui_act` does (a click; a right click; down on one element and up on another: a drag from it onto that one). */
	bool ElementButton(APlayerController* PC, const FKey& Key, EInputEvent Event)
	{
		UWorld* World = PC->GetWorld();
		FSceneViewport* View = SceneViewport(Pointer.Client.Get());
		if (!World || !View)
		{
			return false;
		}
		const FVector2D Size = Pointer.bVirtual ? FVector2D(HeadlessSize) : FVector2D(View->GetSizeXY());
		if (Event == IE_Pressed)
		{
			const FString Id = PlaytestUI::ElementAt(World, Pointer.Pixel, Size);
			if (Id.IsEmpty())
			{
				return false;
			}
			Pointer.ElementDown.Add(Key, Id);
			return true;
		}
		FString Down;
		if (!Pointer.ElementDown.RemoveAndCopyValue(Key, Down))
		{
			return false;
		}
		const FString Up = PlaytestUI::ElementAt(World, Pointer.Pixel, Size);
		TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("id"), Down);
		if (!Up.IsEmpty() && Up != Down && Key == EKeys::LeftMouseButton)
		{
			Args->SetStringField(TEXT("action"), TEXT("drag"));
			Args->SetStringField(TEXT("to"), Up);
		}
		else if (Up == Down)
		{
			Args->SetStringField(TEXT("action"), Key == EKeys::RightMouseButton ? TEXT("right_click") : TEXT("click"));
		}
		else
		{
			return true;  // (let go off the element it went down on: no click, as a mouse's)
		}
		const FString Error = PlaytestUI::Act(World, Args);
		if (!Error.IsEmpty())
		{
			UE_LOG(LogPlaytest, Log, TEXT("[Autopilot] pointer: %s"), *Error);
		}
		return true;
	}
}

	void PointerOp(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = Bridge.GetGameWorld();
		APlayerController* PC = PlayerController(World);
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		FSceneViewport* View = SceneViewport(Client);
		if (!PC || !View)
		{
			Bridge.RespondError(Id, TEXT("No game view"));
			return;
		}
		FIntPoint Size = View->GetSizeXY();
		// A game that never renders (-nullrhi) has a view of no size (and can't be given one: its picture can't be
		// made). The pointer is then on a view of HeadlessSize (16:9, as `view` takes such a view to be): its UI is laid
		// out over that for the clicks, but the game itself can't pick the world under the mouse (no view to deproject)
		const bool bVirtual = (Size.X < 1 || Size.Y < 1) && !FApp::CanEverRender();
		if (bVirtual)
		{
			Size = HeadlessSize;
		}
		if (Size.X < 1 || Size.Y < 1)
		{
			Bridge.RespondError(Id, TEXT("The game's view has no size: nothing to point at"));
			return;
		}
		const double W = Size.X, H = Size.Y;
		FVector2D Pixel;
		const TSharedPtr<FJsonObject> WorldPoint = ObjectField(Args, TEXT("world"));
		const FString ObjectPath = StringField(Args, TEXT("object"));
		if (WorldPoint.IsValid() || !ObjectPath.IsEmpty())
		{
			FVector At;
			if (WorldPoint.IsValid())
			{
				At = VectorField(Args, TEXT("world"));
			}
			else
			{
				UObject* Object = PlaytestJson::ResolveObject(ObjectPath);
				AActor* Actor = Cast<AActor>(Object);
				if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Object))
				{
					At = Primitive->Bounds.Origin;
				}
				else if (const USceneComponent* Component = Cast<USceneComponent>(Object))
				{
					At = Component->GetComponentLocation();
				}
				else if (Actor)
				{
					FBox Visible(ForceInit), Local(ForceInit);
					BodyBounds(Actor, Visible, Local);
					At = Visible.IsValid ? Visible.GetCenter() : Actor->GetActorLocation();  // (its drawn box's middle)
				}
				else
				{
					Bridge.RespondError(Id, Missing(ObjectPath));
					return;
				}
			}
			FVector2D Screen;
			if (bVirtual)
			{
				// Where `view` puts it: the camera's field of view over a 16:9 view
				const APlayerCameraManager* Camera = PC->PlayerCameraManager;
				const FVector D = At - (Camera ? Camera->GetCameraLocation() : FVector::ZeroVector);
				const FRotationMatrix Axes(Camera ? Camera->GetCameraRotation() : FRotator::ZeroRotator);
				const double Ahead = FVector::DotProduct(D, Axes.GetScaledAxis(EAxis::X));
				const double TanH = FMath::Tan(FMath::DegreesToRadians((Camera ? Camera->GetFOVAngle() : 90.0) * 0.5));
				const double TanV = TanH / (W / H);
				if (Ahead <= 1.0)
				{
					Bridge.RespondError(Id, TEXT("That is behind the camera"));
					return;
				}
				const double X = FVector::DotProduct(D, Axes.GetScaledAxis(EAxis::Y)) / (Ahead * TanH);
				const double Y = FVector::DotProduct(D, Axes.GetScaledAxis(EAxis::Z)) / (Ahead * TanV);
				Screen = FVector2D((X + 1.0) / 2.0 * W, (1.0 - Y) / 2.0 * H);
			}
			else if (!PC->ProjectWorldLocationToScreen(At, Screen, true))
			{
				Bridge.RespondError(Id, TEXT("That is behind the camera"));
				return;
			}
			Pixel = Screen;
		}
		else if (Args->HasField(TEXT("screen_x")) || Args->HasField(TEXT("screen_y")))
		{
			// -1..1 from the middle, y up (the numbers `view` gives a thing)
			Pixel = FVector2D((NumberField(Args, TEXT("screen_x"), 0.0) + 1.0) / 2.0 * W, (1.0 - NumberField(Args, TEXT("screen_y"), 0.0)) / 2.0 * H);
		}
		else
		{
			Bridge.RespondError(Id, TEXT("pointer needs screen_x and screen_y, world or object"));
			return;
		}
		if (Pointer.Client.Get() != Client)
		{
			Forget();  // (another view: a new session)
		}
		const FVector2D Before = Pointer.Pixel;
		const bool bWasSet = Pointer.bSet;
		Pointer.bSet = true;
		Pointer.Client = Client;
		Pointer.Pixel = Pixel;
		Pointer.bVirtual = bVirtual;
		Apply(true);
		// A button down on the game's UI (a slider, a drag): the mouse moves there, as it would held
		FVector2D Abs, AbsBefore;
		if (Pointer.UiDown.Num() && Absolute(Client, Pixel, Abs))
		{
			if (!bWasSet || !Absolute(Client, Before, AbsBefore))
			{
				AbsBefore = Abs;
			}
			const FPointerEvent Move(Device(), FSlateApplication::CursorPointerIndex, Abs, AbsBefore, Pointer.Pressed, EKeys::Invalid,
				0.f, FModifierKeysState());
			FSlateApplication::Get().ProcessMouseMoveEvent(Move);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("screen_x"), FMath::RoundToDouble((Pixel.X / W * 2.0 - 1.0) * 1000.0) / 1000.0);
		Result->SetNumberField(TEXT("screen_y"), FMath::RoundToDouble((1.0 - Pixel.Y / H * 2.0) * 1000.0) / 1000.0);
		Result->SetNumberField(TEXT("x"), FMath::RoundToDouble(Pixel.X));
		Result->SetNumberField(TEXT("y"), FMath::RoundToDouble(H - Pixel.Y));  // (from the bottom left, as Unity's)
		Result->SetBoolField(TEXT("on_screen"), Pixel.X >= 0.0 && Pixel.X <= W && Pixel.Y >= 0.0 && Pixel.Y <= H);
		if (bVirtual)
		{
			Result->SetStringField(TEXT("note"), TEXT("This game doesn't render (headless): a click at the pointer reaches its UI, "
				"but the game can't pick the world under the mouse. A world click needs the game played in the editor."));
		}
		Bridge.Respond(Id, Obj(Result));
	}

	bool PointerButton(APlayerController* PC, const FKey& Key, EInputEvent Event)
	{
		if (!Key.IsMouseButton() || !Pointer.bSet || !PC || (Event != IE_Pressed && Event != IE_Released))
		{
			return false;
		}
		UGameViewportClient* Client = Pointer.Client.Get();
		if (!Client || !PC->GetWorld() || PC->GetWorld()->GetGameViewport() != Client)
		{
			return false;
		}
		Apply();
		if (Event == IE_Pressed)
		{
			Pointer.Pressed.Add(Key);
		}
		else
		{
			Pointer.Pressed.Remove(Key);
		}
		FVector2D Abs;
		const bool bLaidOut = Absolute(Client, Pointer.Pixel, Abs);
		if (Event == IE_Released && Pointer.UiDown.Contains(Key))
		{
			Pointer.UiDown.Remove(Key);
			if (bLaidOut)
			{
				FSlateApplication::Get().ProcessMouseButtonUpEvent(ButtonEvent(Abs, Key));
			}
			return true;
		}
		if (Event == IE_Pressed && bLaidOut && UiUnder(Client, Abs))
		{
			FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, ButtonEvent(Abs, Key));
			Pointer.UiDown.Add(Key);
			return true;
		}
		if (!bLaidOut && !FApp::CanEverRender() && ElementButton(PC, Key, Event))
		{
			return true;  // (a game that doesn't render: its UI as `ui_act` acts on it)
		}
		return false;  // the player controller takes it (a click in the world reads the cursor set above)
	}

	void TickPointer(UPlaytestSubsystem& Bridge)
	{
		if (!Pointer.bSet)
		{
			return;
		}
		UWorld* World = Bridge.GetGameWorld();
		if (!Bridge.IsConnected() || !World || World->GetGameViewport() != Pointer.Client.Get())
		{
			// The brain or the session is gone: a button the game's UI holds goes up there, and the pointer is forgotten
			FVector2D Abs;
			if (Pointer.UiDown.Num() && Pointer.Client.IsValid() && Absolute(Pointer.Client.Get(), Pointer.Pixel, Abs))
			{
				for (const FKey& Key : TSet<FKey>(Pointer.UiDown))
				{
					Pointer.Pressed.Remove(Key);
					FSlateApplication::Get().ProcessMouseButtonUpEvent(ButtonEvent(Abs, Key));
				}
			}
			Forget();
			return;
		}
		Apply();  // (every frame: a mouse event of the desktop's over the view moves it back)
	}
}
