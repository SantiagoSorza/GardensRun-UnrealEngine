// Small building blocks of the Autopilot tab (The Flatness): micro-labels, mono and body text, square buttons and tags,
// plates, rules, the switch and the check box, the spinner. Sizes in the design's px.

#pragma once

#include "CoreMinimal.h"
#include "AutopilotStyle.h"
#include "CoreGlobals.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Children.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SPanel.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Text/STextBlock.h"

namespace AutopilotUI
{
	/** Body text's line height: 1.55 of its size (IBM Plex Sans's own is 1.3). */
	constexpr float BodyLineHeight = 1.55f / 1.3f;
	/** A button's height: 28 (small: 24); an icon button is square. */
	constexpr float ButtonHeight = 28.f;
	constexpr float SmallButtonHeight = 24.f;

	/** Text shown as is, not localized. */
	inline FText Str(const FString& S)
	{
		return FText::FromString(S);
	}

	/** A tooltip as the design draws it: the popover, its outline, 8px corners, 6 x 9 inside, 12px reading text. */
	inline TSharedRef<SToolTip> Tip(const TAttribute<FText>& Text)
	{
		return SNew(SToolTip)
			.Text(Text)
			.BorderImage(FAutopilotStyle::Box(AutopilotColor::Popover(), AutopilotColor::Line(), AutopilotRadius::Plate))
			.TextMargin(FMargin(9.f, 6.f))
			.Font(FAutopilotStyle::Font(EAutopilotFont::Body, 12.f));
	}

	/** A micro-label: IBM Plex Mono 10px, 0.1em, upper case, muted. */
	inline TSharedRef<STextBlock> Label(const TAttribute<FText>& Text, const TAttribute<FSlateColor>& Color = FSlateColor(AutopilotColor::Muted()), float Px = 10.f)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAutopilotStyle::Label(Px))
			.ColorAndOpacity(Color)
			.TransformPolicy(ETextTransformPolicy::ToUpper);
	}

	inline TSharedRef<STextBlock> Mono(const TAttribute<FText>& Text, float Px = 11.5f, const TAttribute<FSlateColor>& Color = FSlateColor(AutopilotColor::Text3()),
		bool bMedium = false, int32 LetterSpacing = 0)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAutopilotStyle::Font(bMedium ? EAutopilotFont::MonoBold : EAutopilotFont::Mono, Px, LetterSpacing))
			.ColorAndOpacity(Color);
	}

	inline TSharedRef<STextBlock> Body(const TAttribute<FText>& Text, float Px = 13.f, const FLinearColor& Color = AutopilotColor::Text(), bool bWrap = true,
		EAutopilotFont Face = EAutopilotFont::Body)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAutopilotStyle::Font(Face, Px))
			.ColorAndOpacity(FSlateColor(Color))
			.LineHeightPercentage(BodyLineHeight)
			.AutoWrapText(bWrap);
	}

	/** A plain fill of one color, round Content. */
	inline TSharedRef<SBorder> Fill(const TAttribute<FSlateColor>& Color, const FMargin& Padding = FMargin(0),
		const TSharedRef<SWidget>& Content = SNullWidget::NullWidget)
	{
		return SNew(SBorder)
			.BorderImage(FAutopilotStyle::Brush("Autopilot.White"))
			.BorderBackgroundColor(Color)
			.Padding(Padding)
			[
				Content
			];
	}

	/** A box: Fill with a 1px Outline (transparent: none), corners of Radius, round Content. */
	inline TSharedRef<SBorder> Box(const FLinearColor& FillColor, const FLinearColor& Outline, float Radius, const FMargin& Padding,
		const TSharedRef<SWidget>& Content = SNullWidget::NullWidget)
	{
		return SNew(SBorder)
			.BorderImage(FAutopilotStyle::Box(FillColor, Outline, Radius))
			.Padding(Padding)
			[
				Content
			];
	}

	/** A plate: #181a1e, a 1px rule round it, 8px corners (Outline: a state's, e.g. the red of a confirmation). */
	inline TSharedRef<SBorder> Plate(const FMargin& Padding, const TSharedRef<SWidget>& Content, const FLinearColor& Outline = AutopilotColor::Rule(),
		const FLinearColor& FillColor = AutopilotColor::Plate())
	{
		return Box(FillColor, Outline, AutopilotRadius::Plate, Padding, Content);
	}

	/** A 1px rule across (#24262b), or Color. */
	inline TSharedRef<SWidget> Rule(const FLinearColor& Color = AutopilotColor::Rule(), float Thickness = 1.f)
	{
		return SNew(SBox).HeightOverride(Thickness)[Fill(FSlateColor(Color))];
	}

	/** A 1px rule down. */
	inline TSharedRef<SWidget> VRule(const FLinearColor& Color = AutopilotColor::Rule())
	{
		return SNew(SBox).WidthOverride(1.f)[Fill(FSlateColor(Color))];
	}

	/** A square button, 28 tall (bSmall: 24, 11.5px words): Style is Autopilot.Button (outlined), Autopilot.Button.Primary,
	 *  .Quiet, .Danger (red words), .DangerFill or .On. Words in sentence case, 12px Plex Sans Medium; PadX: its side padding
	 *  (-1: 12, small 9). */
	inline TSharedRef<SButton> Button(const TAttribute<FText>& Text, const FName& Style, FOnClicked OnClicked,
		const TAttribute<bool>& Enabled = true, const FSlateBrush* Icon = nullptr, bool bSmall = false, float PadX = -1.f)
	{
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		if (Icon)
		{
			Content->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SImage).Image(Icon).ColorAndOpacity(FSlateColor::UseForeground())
			];
		}
		Content->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Text)
			.Font(FAutopilotStyle::Font(EAutopilotFont::BodyMedium, bSmall ? 11.5f : 12.f))
		];
		return SNew(SButton)
			.ButtonStyle(&FAutopilotStyle::Get().GetWidgetStyle<FButtonStyle>(Style))
			.ContentPadding(FMargin(PadX >= 0.f ? PadX : (bSmall ? 9.f : 12.f), 0.f))
			.IsEnabled(Enabled)
			.OnClicked(OnClicked)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.HeightOverride((bSmall ? SmallButtonHeight : ButtonHeight) - 2.f)  // (inside its 1px outline)
				.VAlign(VAlign_Center)
				[
					Content
				]
			];
	}

	/** A square button showing only an icon (Icon: an Autopilot.* brush drawn white, tinted by the button), 28 x 28 (small: 24),
	 *  named by Tip (its tooltip and what a screen reader says). */
	inline TSharedRef<SButton> IconButton(const TAttribute<const FSlateBrush*>& Icon, const TAttribute<FText>& Tip, const FName& Style,
		FOnClicked OnClicked, const TAttribute<bool>& Enabled = true, bool bSmall = false)
	{
		const float Side = (bSmall ? SmallButtonHeight : ButtonHeight) - 2.f;
		return SNew(SButton)
			.ButtonStyle(&FAutopilotStyle::Get().GetWidgetStyle<FButtonStyle>(Style))
			.ContentPadding(FMargin(0.f))
			.IsEnabled(Enabled)
			.OnClicked(OnClicked)
			.ToolTip(AutopilotUI::Tip(Tip))
			.AccessibleText(Tip)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(Side)
				.HeightOverride(Side)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SImage).Image(Icon).ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}

	/** A tag: a word, 18 tall, in a state's colors (its surface, a 1px outline of its own, mono 10px 500 upper case, 0.08em). */
	inline TSharedRef<SWidget> Tag(const TAttribute<FText>& Text, const TAttribute<EAutopilotTone>& Tone)
	{
		return SNew(SBox)
			.HeightOverride(18.f)
			.VAlign(VAlign_Fill)
			[
				SNew(SBorder)
				.BorderImage_Lambda([Tone]()
				{
					const FAutopilotTone& T = FAutopilotStyle::Tone(Tone.Get());
					return FAutopilotStyle::Box(T.Fill, T.Line);
				})
				.Padding(FMargin(6.f, 0.f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Text)
					.Font(FAutopilotStyle::Font(EAutopilotFont::MonoBold, 10.f, 80))
					.ColorAndOpacity_Lambda([Tone]() { return FSlateColor(FAutopilotStyle::Tone(Tone.Get()).Fore); })
					.TransformPolicy(ETextTransformPolicy::ToUpper)
				]
			];
	}

	/** A toggle chip (the policy log's filters, Readable / Raw JSON): 24 tall, mono 10.5 500; on: the accent tag's colors. */
	inline TSharedRef<SCheckBox> Chip(const TAttribute<FText>& Text, const TAttribute<ECheckBoxState>& State, FOnCheckStateChanged OnChanged)
	{
		return SNew(SCheckBox)
			.Style(&FAutopilotStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Autopilot.Chip"))
			.IsChecked(State)
			.OnCheckStateChanged(OnChanged)
			[
				SNew(SBox)
				.HeightOverride(22.f)  // (24 with its outline)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Text)
					.Font(FAutopilotStyle::Font(EAutopilotFont::MonoBold, 10.5f, 60))
				]
			];
	}

	/** The width of the accent rule at a picked row's left edge. */
	constexpr float PickedBar = 2.f;

	/**
	 * A row that can be the one picked (a run in a list): picked, #2a1b10 behind it and a 2px accent rule at its left edge;
	 * else Background (its stripe). Its words keep their own colors.
	 */
	inline TSharedRef<SWidget> Picked(const TAttribute<bool>& IsPicked, const TSharedRef<SWidget>& Content,
		const TAttribute<FLinearColor>& Background = FLinearColor::Transparent)
	{
		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderImage(FAutopilotStyle::Brush("Autopilot.White"))
				.BorderBackgroundColor_Lambda([IsPicked, Background]()
				{
					return FSlateColor(IsPicked.Get() ? AutopilotColor::RowActive() : Background.Get());
				})
				.Padding(0)
				[
					Content
				]
			]
			+ SOverlay::Slot().HAlign(HAlign_Left)
			[
				SNew(SBox)
				.WidthOverride(PickedBar)
				.Visibility_Lambda([IsPicked]() { return IsPicked.Get() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[
					Fill(FSlateColor(AutopilotColor::Accent()))
				]
			];
	}

	/** An alert: a square box in a state's surface and outline, 9 x 11 inside. */
	inline TSharedRef<SWidget> Alert(EAutopilotTone Tone, const TSharedRef<SWidget>& Content)
	{
		const FAutopilotTone& T = FAutopilotStyle::Tone(Tone);
		return Box(T.Fill, T.Line, 0.f, FMargin(11.f, 9.f), Content);
	}

	/** The switch (a pill: off #2c2f36, on the accent, a white knob) and its words Gap px to its right, the whole row
	 * clickable. */
	inline TSharedRef<SCheckBox> Switch(TFunction<bool()> IsOn, TFunction<void(bool)> OnChanged, const TSharedRef<SWidget>& Words,
		float Gap = 10.f)
	{
		TSharedRef<SCheckBox> Box = SNew(SCheckBox)
			.Style(&FAutopilotStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Autopilot.Bare"))
			.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([OnChanged](ECheckBoxState State) { OnChanged(State == ECheckBoxState::Checked); });
		TWeakPtr<SCheckBox> Weak = Box;
		Box->SetContent(
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, Gap, 0.f)
			[
				SNew(SBox).WidthOverride(30.f).HeightOverride(16.f)
				[
					SNew(SBorder)
					.BorderImage(FAutopilotStyle::Brush("Autopilot.Pill"))
					.BorderBackgroundColor_Lambda([IsOn, Weak]()
					{
						const TSharedPtr<SCheckBox> B = Weak.Pin();
						return FSlateColor(IsOn() ? AutopilotColor::Accent() : B.IsValid() && B->IsHovered() ? AutopilotColor::LineHover() : AutopilotColor::Line());
					})
					.Padding(FMargin(2.f))
					.VAlign(VAlign_Center)
					[
						// (the knob at the left when off, at the right when on)
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpacer).Visibility_Lambda([IsOn]() { return IsOn() ? EVisibility::Visible : EVisibility::Collapsed; })
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SBox).WidthOverride(12.f).HeightOverride(12.f)
							[
								SNew(SImage).Image(FAutopilotStyle::Brush("Autopilot.Round")).ColorAndOpacity(FSlateColor(AutopilotColor::Knob()))
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpacer).Visibility_Lambda([IsOn]() { return IsOn() ? EVisibility::Collapsed : EVisibility::Visible; })
						]
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				Words
			]);
		return Box;
	}

	/** A check box (14 x 14, square, filled red when ticked: a destructive choice) and its words, the whole row clickable. */
	inline TSharedRef<SCheckBox> Check(TFunction<bool()> IsOn, TFunction<void(bool)> OnChanged, const TSharedRef<SWidget>& Words)
	{
		return SNew(SCheckBox)
			.Style(&FAutopilotStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Autopilot.Bare"))
			.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([OnChanged](ECheckBoxState State) { OnChanged(State == ECheckBoxState::Checked); })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 2.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(14.f).HeightOverride(14.f)
					[
						SNew(SBorder)
						.BorderImage_Lambda([IsOn]()
						{
							return IsOn() ? FAutopilotStyle::Box(AutopilotColor::Red(), AutopilotColor::Red()) : FAutopilotStyle::Box(AutopilotColor::Well(), AutopilotColor::LineHover());
						})
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
				[
					Words
				]
			];
	}

	/** The spinner: a 12px square, 2px #3a5578 with its top #7aa5d8, turning. */
	class SSpinner : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSpinner) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateLambda([](double, float) { return EActiveTimerReturnType::Continue; }));
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect, FSlateWindowElementList& Out,
			int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const override
		{
			const float Angle = (float)FMath::Fmod(FPlatformTime::Seconds(), 1.0) * 2.f * PI;
			const FVector2f Size(12.f, 12.f);
			const FVector2f Offset = (FVector2f(Geometry.GetLocalSize()) - Size) * 0.5f;
			const FGeometry Turned = Geometry.MakeChild(Size, FSlateLayoutTransform(Offset), FSlateRenderTransform(TQuat2<float>(Angle)), FVector2f(0.5f, 0.5f));
			// (in its parent's opacity, as Slate's own widgets are: SSettle paints what is coming unseen)
			FSlateDrawElement::MakeBox(Out, LayerId, Turned.ToPaintGeometry(), FAutopilotStyle::Box(FLinearColor::Transparent, AutopilotColor::InfoLine(), 0.f, 2.f),
				ESlateDrawEffect::None, Style.GetColorAndOpacityTint());
			FSlateDrawElement::MakeBox(Out, LayerId + 1, Turned.ToPaintGeometry(FVector2f(12.f, 2.f), FSlateLayoutTransform()), FAutopilotStyle::Brush("Autopilot.White"),
				ESlateDrawEffect::None, AutopilotColor::Info() * Style.GetColorAndOpacityTint());
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(12.0, 12.0);
		}
	};

	/**
	 * Content that changes without a flash. Slate sizes a wrap box and wrapping words by the frame before, so a widget shown
	 * the frame it is made (or the first frame its page is) draws once wrong, its row wrapped at 100px or its words on one
	 * line, and jumps the next. Here the new content (SetContent, or the page of Pages that WidgetIndex picks) is laid out
	 * and painted unseen while what it replaces stays shown, until its size holds; then it takes that one's place. Not on
	 * screen (a page not shown, a test), a change shows at once.
	 */
	class SSettle : public SPanel
	{
	public:
		SLATE_BEGIN_ARGS(SSettle) {}
			/** The page shown, of Pages; unset: the content given last. */
			SLATE_ATTRIBUTE(int32, WidgetIndex)
			SLATE_ARGUMENT(TArray<TSharedRef<SWidget>>, Pages)
		SLATE_END_ARGS()

		SSettle() : Children(this) {}

		void Construct(const FArguments& InArgs)
		{
			WidgetIndex = InArgs._WidgetIndex;
			for (const TSharedRef<SWidget>& Page : InArgs._Pages)
			{
				Wrap(Page);
			}
			Shown = Target();  // (the first: as it comes)
		}

		/** Content in place of what is shown, once it is laid out. */
		void SetContent(const TSharedRef<SWidget>& Content)
		{
			if (!IsOnScreen())
			{
				// (nothing to flash: the one before goes now, not at the next layout)
				Shown.Reset();
				Incoming.Reset();
			}
			DropOld(false);  // (a content given before that never showed goes too)
			Wrap(Content);
			if (!Shown.IsValid())
			{
				Shown = Pages.Last();
			}
		}

		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
		{
			DropOld(true);
		}

		virtual FChildren* GetChildren() override
		{
			return &Children;
		}

		/** (a test's: as if painted on screen this frame, what is coming too) */
		void TestPainted() const
		{
			PaintedFrame = GFrameCounter + 1;
			Paints += Incoming.IsValid() ? 1 : 0;
		}

	protected:
		virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
		{
			// (the prepass: the pages' sizes are this frame's, and so is the change shown)
			Settle(LayoutScaleMultiplier);
			return Shown.IsValid() ? FVector2D(Shown->GetDesiredSize()) : FVector2D::ZeroVector;
		}

		virtual void OnArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const override
		{
			for (const TSharedRef<SBox>& Page : Pages)
			{
				// (this frame's, not the box's: that one is kept from the prepass, before a change was shown)
				const EVisibility PageVisibility = VisibilityOf(&Page.Get());
				if (ArrangedChildren.Accepts(PageVisibility))
				{
					ArrangedChildren.AddWidget(PageVisibility, AllottedGeometry.MakeChild(Page, FVector2D(AllottedGeometry.GetLocalSize()), FSlateLayoutTransform()));
				}
			}
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
			int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			PaintedFrame = GFrameCounter + 1;
			FArrangedChildren Arranged(EVisibility::Visible);
			ArrangeChildren(AllottedGeometry, Arranged);
			FArrangedChildren Seen(EVisibility::Visible);
			FArrangedChildren Unseen(EVisibility::Visible);
			for (int32 i = 0; i < Arranged.Num(); ++i)
			{
				(&Arranged[i].Widget.Get() == Incoming.Get() ? Unseen : Seen).AddWidget(Arranged[i]);
			}
			int32 Layer = LayerId;
			if (Unseen.Num())
			{
				// (nearly 0: what is drawn is invisible, but nothing is skipped for being transparent)
				Layer = PaintArrangedChildren(Args, Unseen, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId,
					FWidgetStyle(InWidgetStyle).BlendOpacity(0.001f), bParentEnabled);
				++Paints;
			}
			return FMath::Max(Layer, PaintArrangedChildren(Args, Seen, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled));
		}

	private:
		/** Painted this many times unseen, its size not settled, the new content shows anyway. */
		static constexpr int32 MostPaints = 8;

		TSharedPtr<SBox> Target() const
		{
			if (WidgetIndex.IsSet())
			{
				const int32 Index = WidgetIndex.Get();
				return Pages.IsValidIndex(Index) ? TSharedPtr<SBox>(Pages[Index]) : nullptr;
			}
			return Pages.Num() ? TSharedPtr<SBox>(Pages.Last()) : nullptr;
		}

		/** Painted this frame or one of the last two. */
		bool IsOnScreen() const
		{
			return PaintedFrame > 0 && GFrameCounter + 1 - PaintedFrame <= 2;
		}

		/** Shown: its own; settling: seen by nothing; else collapsed (not laid out). */
		EVisibility VisibilityOf(const SBox* Page) const
		{
			return Shown.Get() == Page ? EVisibility::SelfHitTestInvisible : Incoming.Get() == Page ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
		}

		void Wrap(const TSharedRef<SWidget>& Content)
		{
			TSharedRef<SBox> Box = SNew(SBox)[Content];
			const SBox* Raw = &Box.Get();
			Box->SetVisibility(TAttribute<EVisibility>::CreateLambda([this, Raw]() { return VisibilityOf(Raw); }));
			Pages.Add(Box);
			Children.Add(Box);
		}

		/** Contents given that are neither shown, settling (bKeepIncoming) nor the last given, gone; pages stay. */
		void DropOld(bool bKeepIncoming)
		{
			if (WidgetIndex.IsSet())
			{
				return;
			}
			for (int32 i = Pages.Num() - 1; i >= 0; --i)
			{
				const TSharedRef<SBox> Page = Pages[i];
				if (&Page.Get() != Shown.Get() && (!bKeepIncoming || (&Page.Get() != Incoming.Get() && i != Pages.Num() - 1)))
				{
					Pages.RemoveAt(i);
					Children.Remove(Page);
				}
			}
		}

		void Settle(float LayoutScaleMultiplier) const
		{
			const TSharedPtr<SBox> Want = Target();
			if (Want == Shown)
			{
				Incoming.Reset();  // (back to what is shown before the other settled)
				return;
			}
			if (!IsOnScreen() || !Want.IsValid())
			{
				Show(Want, LayoutScaleMultiplier);
				return;
			}
			if (Want != Incoming)
			{
				// (laid out now, painted unseen this frame; a content given while another settled keeps its count of
				// paints, so contents given every frame still show)
				if (!Incoming.IsValid())
				{
					Paints = 0;
				}
				Incoming = Want;
				Want->SlatePrepass(LayoutScaleMultiplier);
				Settling = FVector2D(Want->GetDesiredSize());
				return;
			}
			const FVector2D Size(Incoming->GetDesiredSize());
			if ((Paints >= 1 && Size.Equals(Settling, 0.5)) || Paints >= MostPaints)
			{
				Show(Incoming, LayoutScaleMultiplier, true);
				return;
			}
			Settling = Size;
		}

		void Show(const TSharedPtr<SBox>& Page, float LayoutScaleMultiplier, bool bLaidOut = false) const
		{
			Shown = Page;
			Incoming.Reset();
			Paints = 0;
			if (Page.IsValid() && !bLaidOut)
			{
				Page->SlatePrepass(LayoutScaleMultiplier);  // (it was collapsed: its size is this frame's now)
			}
		}

		TAttribute<int32> WidgetIndex;
		TArray<TSharedRef<SBox>> Pages;  // (each content in a box of its own, its visibility this widget's)
		TSlotlessChildren<SBox> Children;
		mutable TSharedPtr<SBox> Shown;
		mutable TSharedPtr<SBox> Incoming;  // being laid out unseen
		mutable FVector2D Settling = FVector2D::ZeroVector;  // its size at the prepass before
		mutable int32 Paints = 0;  // how many times it was painted unseen
		mutable uint64 PaintedFrame = 0;  // GFrameCounter + 1 when last painted (0: never)
	};

	/** Content at 45% while bGreyed: a button that can't act now but still says why when pressed (the status line). */
	inline TSharedRef<SWidget> Greyed(const TAttribute<bool>& bGreyed, const TSharedRef<SWidget>& Content)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetNoBrush())
			.Padding(0.f)
			.ColorAndOpacity_Lambda([bGreyed]() { return FLinearColor(1.f, 1.f, 1.f, bGreyed.Get() ? 0.45f : 1.f); })
			[
				Content
			];
	}

	/** A finding's severity as a state. */
	inline EAutopilotTone SeverityTone(const FString& Severity)
	{
		if (Severity == TEXT("error") || Severity == TEXT("bug") || Severity == TEXT("fatal"))
		{
			return EAutopilotTone::Red;
		}
		if (Severity == TEXT("warning"))
		{
			return EAutopilotTone::Amber;
		}
		return EAutopilotTone::Info;
	}
}
