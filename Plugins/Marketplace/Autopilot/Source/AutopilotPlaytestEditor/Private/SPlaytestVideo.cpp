#include "SPlaytestVideo.h"

#include "AutopilotSession.h"
#include "AutopilotStyle.h"
#include "AutopilotWidgets.h"
#include "FileMediaSource.h"
#include "HAL/PlatformProcess.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "UObject/Package.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SLeafWidget.h"

#define LOCTEXT_NAMESPACE "Autopilot"

namespace
{
	/** The recording's button: the file shown, selected, in the computer's file manager (its name on each system). */
	FText ShowInFolderText()
	{
#if PLATFORM_WINDOWS
		return LOCTEXT("ShowInExplorer", "Show in File Explorer");
#elif PLATFORM_MAC
		return LOCTEXT("ShowInFinder", "Show in Finder");
#else
		return LOCTEXT("ShowInFolder", "Show in folder");
#endif
	}

	/** The screen: the well (a rule round it, 8px corners), the video's frame fitted in it whole (letterboxed) once it has
	 *  one, a play mark in its middle until then (a warning mark, amber, when it can't play). */
	class SPlaytestVideoScreen : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SPlaytestVideoScreen) {}
			SLATE_ARGUMENT(const FSlateBrush*, Brush)
			/** The video's frame size in pixels (0: no frame yet). */
			SLATE_ATTRIBUTE(FVector2D, VideoSize)
			/** The recording can't play here, or is gone: the warning mark in place of the play mark. */
			SLATE_ATTRIBUTE(bool, Unplayable)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Brush = InArgs._Brush;
			VideoSize = InArgs._VideoSize;
			Unplayable = InArgs._Unplayable;
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect, FSlateWindowElementList& Out,
			int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const override
		{
			// (its color passed as the tint: MakeBox draws a brush white unless told its tint, as SBorder does)
			const FSlateBrush* Well = FAutopilotStyle::Box(AutopilotColor::Well(), AutopilotColor::Rule(), AutopilotRadius::Plate);
			FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(), Well, ESlateDrawEffect::None,
				Well->GetTint(Style) * Style.GetColorAndOpacityTint());
			const FVector2D Video = VideoSize.Get();
			const FVector2f Room = Geometry.GetLocalSize();
			if (Brush && Video.X > 0.0 && Video.Y > 0.0 && Room.X > 2.f && Room.Y > 2.f)
			{
				const FVector2f Inside(Room.X - 2.f, Room.Y - 2.f);
				const float Scale = FMath::Min(Inside.X / (float)Video.X, Inside.Y / (float)Video.Y);
				const FVector2f Size((float)Video.X * Scale, (float)Video.Y * Scale);
				const FVector2f Offset((Room.X - Size.X) * 0.5f, (Room.Y - Size.Y) * 0.5f);
				FSlateDrawElement::MakeBox(Out, LayerId + 1, Geometry.MakeChild(Size, FSlateLayoutTransform(Offset)).ToPaintGeometry(), Brush,
					ESlateDrawEffect::None, Brush->GetTint(Style) * Style.GetColorAndOpacityTint());
			}
			else if (const FSlateBrush* Mark = FAutopilotStyle::Brush(Unplayable.Get(false) ? "Autopilot.Warning" : "Autopilot.Screen"))
			{
				const FVector2f Side(30.f, 30.f);
				FSlateDrawElement::MakeBox(Out, LayerId + 1, Geometry.MakeChild(Side, FSlateLayoutTransform((Room - Side) * 0.5f)).ToPaintGeometry(), Mark,
					ESlateDrawEffect::None, (Unplayable.Get(false) ? AutopilotColor::Amber() : FLinearColor::White) * Style.GetColorAndOpacityTint());
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(16.0, 9.0);  // (the box round it sets its size)
		}

	private:
		const FSlateBrush* Brush = nullptr;
		TAttribute<FVector2D> VideoSize;
		TAttribute<bool> Unplayable;
	};
}

void SPlaytestVideo::Construct(const FArguments& InArgs)
{
	Path = InArgs._Path;
	Seconds = InArgs._Seconds;
	if (InArgs._Open && !Path.IsEmpty())
	{
		// The player, its source and the texture its frames go to: transient, kept alive by AddReferencedObjects
		Player = NewObject<UMediaPlayer>(GetTransientPackage(), NAME_None, RF_Transient);
		Player->PlayOnOpen = false;  // (nothing plays until Play is pressed)
		Player->SetLooping(false);
		Texture = NewObject<UMediaTexture>(GetTransientPackage(), NAME_None, RF_Transient);
		Texture->AutoClear = true;
		Texture->ClearColor = FLinearColor::Black;
		Texture->SetMediaPlayer(Player);
		Texture->UpdateResource();
		Source = NewObject<UFileMediaSource>(GetTransientPackage(), NAME_None, RF_Transient);
		Source->SetFilePath(Path);
		bCantPlay = !Player->OpenSource(Source);  // (no player for it in this engine: the file is still shown in its folder)
		Brush.SetResourceObject(Texture);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.ImageSize = FVector2D(16.0, 9.0);
	}

	bGone = InArgs._Gone;
	auto ShowInFolder = [this]()
	{
		return AutopilotUI::IconButton(FAutopilotStyle::Brush("Autopilot.Folder"), ShowInFolderText(), "Autopilot.Button.Quiet",
			FOnClicked::CreateLambda([this]()
		{
			if (FPaths::FileExists(Path))
			{
				// (File Explorer and Finder select the file; on Linux the file manager opens its folder)
				FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(Path));
			}
			return FReply::Handled();
		}), !bGone);
	};
	// The controls, 10 apart: Play / Pause (outlined), the time bar (2px: the rule, the accent as far as it has played; a
	// small handle to scrub), the time (mono 11), Show in File Explorer (quiet, the folder)
	auto Played = [this]() { const double L = Length(); return L > 0.0 ? (float)FMath::Clamp(Now() / L, 0.0, 1.0) : 0.f; };
	static FSliderStyle Slider = []()
	{
		FSliderStyle Style = FAppStyle::Get().GetWidgetStyle<FSliderStyle>("Slider");
		const FSlateRoundedBoxBrush Handle(AutopilotColor::Accent(), 0.f, FVector2f(8.f, 8.f));
		Style.SetNormalBarImage(FSlateNoResource()).SetHoveredBarImage(FSlateNoResource()).SetDisabledBarImage(FSlateNoResource())
			.SetNormalThumbImage(Handle).SetHoveredThumbImage(Handle).SetDisabledThumbImage(FSlateNoResource()).SetBarThickness(2.f);
		return Style;
	}();
	TSharedRef<SHorizontalBox> Controls = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
		[
			// Play / Pause: the triangle, or the two bars while it plays
			AutopilotUI::IconButton(TAttribute<const FSlateBrush*>::CreateLambda([this]() { return FAutopilotStyle::Brush(IsPlaying() ? "Autopilot.Pause" : "Autopilot.Play"); }),
				TAttribute<FText>::CreateLambda([this]() { return IsPlaying() ? LOCTEXT("PauseVideo", "Pause") : LOCTEXT("PlayVideo", "Play"); }),
				"Autopilot.Button", FOnClicked::CreateSP(this, &SPlaytestVideo::OnPlay),
				TAttribute<bool>::CreateLambda([this]() { return Player != nullptr && !CantPlay(); }))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SOverlay)
			+ SOverlay::Slot().VAlign(VAlign_Center)
			[
				SNew(SBox).HeightOverride(2.f)
				[
					AutopilotUI::Fill(FSlateColor(AutopilotColor::Rule()), FMargin(0.f),
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(TAttribute<float>::CreateLambda([Played]() { return FMath::Max(Played(), 0.0001f); }))
						[
							AutopilotUI::Fill(FSlateColor(AutopilotColor::Accent()))
						]
						+ SHorizontalBox::Slot().FillWidth(TAttribute<float>::CreateLambda([Played]() { return FMath::Max(1.f - Played(), 0.0001f); })))
				]
			]
			+ SOverlay::Slot().VAlign(VAlign_Center)
			[
				SNew(SSlider)
				.Style(&Slider)
				.IsEnabled_Lambda([this]() { return Player != nullptr && Length() > 0.0; })
				.Value_Lambda(Played)
				.OnValueChanged(this, &SPlaytestVideo::OnScrub)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
		[
			AutopilotUI::Mono(TAttribute<FText>::CreateSP(this, &SPlaytestVideo::TimeText), 11.f, FSlateColor(AutopilotColor::Text3()))
		];
	Controls->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)[ShowInFolder()];  // (an icon: it fits a narrow tab too)
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.HeightOverride(this, &SPlaytestVideo::ScreenHeight)
			[
				SNew(SPlaytestVideoScreen)
				.Brush(&Brush)
				.Unplayable_Lambda([this]() { return bGone || (Player && CantPlay()); })
				.VideoSize(this, &SPlaytestVideo::VideoSize)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			Controls
		];
	Box->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
	[
		SNew(STextBlock)
		.Visibility_Lambda([this]() { return Player && CantPlay() ? EVisibility::Visible : EVisibility::Collapsed; })
		.Text_Lambda([this]()
		{
			// (no player plugin for an MP4 on in this project: the editor's media players are plugins a project can turn off)
			return bCantPlay
				? FText::Format(LOCTEXT("VideoNoPlayer", "This recording can’t play here: this project has no video player plugin on (Electra Player or WMF Media, in Edit > Plugins). {0} (the button beside the time) and open it from there."),
					ShowInFolderText())
				: FText::Format(LOCTEXT("VideoError", "This recording can’t play here: {0} (the button beside the time) and open it from there."),
					ShowInFolderText());
		})
		.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
		.ColorAndOpacity(FSlateColor(AutopilotColor::Amber()))
		.AutoWrapText(true)
	];
	Box->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
	[
		SNew(STextBlock)
		.Visibility(bGone ? EVisibility::Visible : EVisibility::Collapsed)
		.Text(LOCTEXT("VideoGone", "The recording isn\u2019t there any more."))
		.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f))
		.ColorAndOpacity(FSlateColor(AutopilotColor::Amber()))
		.AutoWrapText(true)
	];
	ChildSlot[Box];
}

SPlaytestVideo::~SPlaytestVideo()
{
	// The run shown changed or the tab closed: the player lets go of the file (the objects go at the next collection)
	if (UObjectInitialized() && Player && IsValid(Player))
	{
		Player->Close();
	}
	if (UObjectInitialized() && Texture && IsValid(Texture))
	{
		Texture->SetMediaPlayer(nullptr);
	}
	Brush.SetResourceObject(nullptr);
}

void SPlaytestVideo::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	Width = AllottedGeometry.GetLocalSize().X;
	if (Texture && Texture->GetWidth() > 1 && Texture->GetHeight() > 1)
	{
		Brush.ImageSize = FVector2D((double)Texture->GetWidth(), (double)Texture->GetHeight());
	}
}

void SPlaytestVideo::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Player);
	Collector.AddReferencedObject(Source);
	Collector.AddReferencedObject(Texture);
}

bool SPlaytestVideo::CantPlay() const
{
	return bCantPlay || (Player && Player->HasError());
}

bool SPlaytestVideo::IsPlaying() const
{
	return Player && Player->IsPlaying();
}

void SPlaytestVideo::Pause()
{
	if (IsPlaying())
	{
		Player->Pause();
	}
}

FReply SPlaytestVideo::OnPlay()
{
	if (!Player)
	{
		return FReply::Handled();
	}
	if (Player->IsPlaying())
	{
		Player->Pause();
		return FReply::Handled();
	}
	if (Player->IsClosed())
	{
		// (closed at its end, or it never opened): open again, playing once open
		Player->PlayOnOpen = true;
		bCantPlay = !Player->OpenSource(Source);
	}
	else
	{
		const double L = Length();
		if (L > 0.0 && Now() >= L - 0.05)
		{
			Player->Rewind();  // (at the end: from the start again)
		}
		Player->Play();
	}
	if (!bTimer)
	{
		bTimer = true;
		RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SPlaytestVideo::WhilePlaying));
	}
	return FReply::Handled();
}

EActiveTimerReturnType SPlaytestVideo::WhilePlaying(double InCurrentTime, float InDeltaTime)
{
	if (Player && (Player->IsPlaying() || Player->IsPreparing() || Player->IsBuffering() || Player->IsConnecting()))
	{
		return EActiveTimerReturnType::Continue;
	}
	bTimer = false;
	return EActiveTimerReturnType::Stop;
}

void SPlaytestVideo::OnScrub(float Value)
{
	const double L = Length();
	if (Player && L > 0.0)
	{
		Player->Seek(FTimespan::FromSeconds(FMath::Clamp((double)Value, 0.0, 1.0) * L));
	}
}

double SPlaytestVideo::Now() const
{
	return Player ? FMath::Max(0.0, Player->GetTime().GetTotalSeconds()) : 0.0;
}

double SPlaytestVideo::Length() const
{
	const double Known = Player ? Player->GetDuration().GetTotalSeconds() : 0.0;
	return Known > 0.0 ? Known : FMath::Max(0.0, Seconds);
}

FText SPlaytestVideo::TimeText() const
{
	return AutopilotUI::Str(FString::Printf(TEXT("%s / %s"), *FAutopilotSession::ClockShort(Now()), *FAutopilotSession::Clock(Length())));
}

FVector2D SPlaytestVideo::VideoSize() const
{
	if (!Player || CantPlay())
	{
		return FVector2D::ZeroVector;  // (the play mark, never the texture's own black: it has no frame to show)
	}
	const FIntPoint Track = Player->GetVideoTrackDimensions(INDEX_NONE, INDEX_NONE);
	if (Track.X > 0 && Track.Y > 0)
	{
		return FVector2D((double)Track.X, (double)Track.Y);
	}
	// (a player that doesn't say its track's: the frames' once there are some, beyond the empty texture's few pixels)
	return Texture && Texture->GetWidth() > 16 && Texture->GetHeight() > 16 ? FVector2D((double)Texture->GetWidth(), (double)Texture->GetHeight())
		: FVector2D::ZeroVector;
}

FOptionalSize SPlaytestVideo::ScreenHeight() const
{
	// The width it was last given, in the video's shape (16:9 until it is known): the video fills it, no bars
	const FVector2D Video = VideoSize();
	const double Shape = Video.X > 0.0 && Video.Y > 0.0 ? Video.Y / Video.X : 9.0 / 16.0;
	return FOptionalSize(Width > 0.f ? (float)(Width * Shape) : 230.f);
}

#undef LOCTEXT_NAMESPACE
