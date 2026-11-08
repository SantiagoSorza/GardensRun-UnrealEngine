#include "SPlaytestPolicyLog.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "HAL/PlatformApplicationMisc.h"
#include "AutopilotStyle.h"
#include "AutopilotWidgets.h"
#include "PlaytestInspect.h"
#include "PlaytestJson.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "AutopilotPolicyLog"

namespace
{
	constexpr double FollowRefreshSeconds = 0.25;  // while following, the step shown changes at most this often
	constexpr float VerticalBelow = 900.f;          // narrower: the step picked goes below the table, and the table loses columns
	constexpr float DetailWidth = 440.f;            // the step picked, beside the table

	const TCHAR* const Tags[] = { TEXT("SWITCH"), TEXT("START"), TEXT("KEEP"), TEXT("HELD"), TEXT("DROPPED"), TEXT("FAILED") };

	using AutopilotUI::Str;

	FString Prob(float P)
	{
		FString S = FString::Printf(TEXT("%.2f"), FMath::Clamp(P, 0.f, 1.f));
		return S.StartsWith(TEXT("0.")) ? S.Mid(1) : S;
	}

	FString Ms(double Seconds)
	{
		return Seconds >= 0 ? FString::Printf(TEXT("%.0f"), Seconds * 1000.0) : FString(TEXT("\u2014"));
	}

	FString NumberText(double V)
	{
		if (V == FMath::FloorToDouble(V) && FMath::Abs(V) < 1e12)
		{
			return FString::Printf(TEXT("%lld"), (long long)V);
		}
		return FString::Printf(TEXT("%.4g"), V);
	}

	/** "1 switch", "4 switches". */
	FString Count(int32 N, const TCHAR* One, const TCHAR* Many)
	{
		return FString::Printf(TEXT("%d %s"), N, N == 1 ? One : Many);
	}

	/** A JSON value on one line, as the observation shows it. */
	FString Compact(const TSharedPtr<FJsonValue>& V, int32 Depth = 0)
	{
		if (!V.IsValid() || V->IsNull())
		{
			return TEXT("null");
		}
		switch (V->Type)
		{
		case EJson::Boolean: return V->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number: return NumberText(V->AsNumber());
		case EJson::String: return V->AsString();
		case EJson::Array:
		{
			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Item : V->AsArray())
			{
				Items.Add(Compact(Item, Depth + 1));
			}
			const FString Joined = FString::Join(Items, TEXT(", "));
			return Depth ? TEXT("[") + Joined + TEXT("]") : Joined;
		}
		case EJson::Object:
		{
			TArray<FString> Items;
			for (const auto& Pair : V->AsObject()->Values)
			{
				Items.Add(FString(*Pair.Key) + TEXT(": ") + Compact(Pair.Value, Depth + 1));
			}
			const FString Joined = FString::Join(Items, TEXT(" · "));
			return Depth ? TEXT("{") + Joined + TEXT("}") : Joined;
		}
		default:
			return FString();
		}
	}

	TSharedPtr<FJsonObject> ObjectOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : nullptr;
	}

	/** A step's tag colors: SWITCH solid accent (ink), START info, KEEP neutral, HELD amber, DROPPED and FAILED red. */
	void TagColors(const FString& TagName, FLinearColor& Fg, FLinearColor& Bg, FLinearColor& Line)
	{
		EAutopilotTone Tone = EAutopilotTone::Neutral;
		if (TagName.StartsWith(TEXT("SWITCH")))
		{
			Fg = AutopilotColor::OnAccent();
			Bg = Line = AutopilotColor::Accent();
			return;
		}
		if (TagName.StartsWith(TEXT("START")))
		{
			Tone = EAutopilotTone::Info;
		}
		else if (TagName == TEXT("HELD"))
		{
			Tone = EAutopilotTone::Amber;
		}
		else if (TagName == TEXT("DROPPED") || TagName == TEXT("FAILED"))
		{
			Tone = EAutopilotTone::Red;
		}
		const FAutopilotTone& T = FAutopilotStyle::Tone(Tone);
		Fg = T.Fore;
		Bg = T.Fill;
		Line = T.Line;
	}

	/** A step's tag: 62 x 18 in the table (mono 9.5), or (bBig) 22 tall as wide as its word (mono 10.5), in the detail. */
	TSharedRef<SWidget> StepTag(const FString& TagName, bool bBig = false)
	{
		FLinearColor Fg, Bg, Line;
		TagColors(TagName, Fg, Bg, Line);
		return SNew(SBox)
			.WidthOverride(bBig ? FOptionalSize() : FOptionalSize(62.f))
			.HeightOverride(bBig ? 22.f : 18.f)
			[
				SNew(SBorder)
				.BorderImage(FAutopilotStyle::Box(Bg, Line))
				.Padding(FMargin(bBig ? 9.f : 0.f, 0.f))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Str(TagName))
					.Font(FAutopilotStyle::Font(EAutopilotFont::MonoBold, bBig ? 10.5f : 9.5f, 60))
					.ColorAndOpacity(FSlateColor(Fg))
				]
			];
	}

	/** A readable section of the observation: the ground, a rule round it, 8px corners, 10 x 12 inside. */
	TSharedRef<SWidget> Sec(const TSharedRef<SWidget>& Content)
	{
		return AutopilotUI::Box(AutopilotColor::Ground(), AutopilotColor::Rule(), AutopilotRadius::Plate, FMargin(12.f, 10.f), Content);
	}
}

// -- The replay strip --------------------------------------------------------------------------------------------

float SAutopilotReplay::XOf(int32 Index, float Width) const
{
	const TArray<TSharedPtr<FAutopilotStep>>& Steps = GetSteps();
	if (Steps.Num() < 2)
	{
		return 0.f;
	}
	const double T0 = Steps[0]->Call->GameTime;
	const double T1 = Steps.Last()->Call->GameTime;
	const double T = Steps[Index]->Call->GameTime;
	if (T0 >= 0 && T1 > T0 && T >= 0)
	{
		return (float)((T - T0) / (T1 - T0)) * Width;
	}
	return (float)Index / (float)(Steps.Num() - 1) * Width;  // (no game times: evenly)
}

FVector2D SAutopilotReplay::ComputeDesiredSize(float) const
{
	// The lane (16; narrow 14), the step marks under it (3 + 10, not when narrow), the game times (3 + 14)
	return FVector2D(200.0, bCompact ? 14.0 + 17.0 : 16.0 + 13.0 + 17.0);
}

int32 SAutopilotReplay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FSlateBrush* White = FAutopilotStyle::Brush("Autopilot.White");
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	const float Width = FMath::Max(1.f, Size.X - 2.f);
	const float Lane = bCompact ? 14.f : 16.f;
	const float Marks = Lane + 3.f;  // (the step marks' top)
	const float Times = bCompact ? Lane + 3.f : Marks + 13.f;
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();  // (its parent's opacity: SSettle paints a page coming unseen)
	auto Box = [&](float X, float Y, float W, float H, const FLinearColor& Color, int32 Layer)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(FVector2f(FMath::Max(W, 1.f), H), FSlateLayoutTransform(FVector2f(X, Y))),
			White, ESlateDrawEffect::None, Color * Tint);
	};
	// The lane
	Box(0.f, 0.f, Size.X, Lane, AutopilotColor::Hover(), LayerId);
	if (!GetSteps || !ColorOf)
	{
		return LayerId;
	}
	const TArray<TSharedPtr<FAutopilotStep>>& Steps = GetSteps();
	if (Steps.Num() == 0)
	{
		return LayerId;
	}
	const TSharedPtr<FAutopilotStep> Picked = GetSelected ? GetSelected() : nullptr;
	for (int32 i = 0; i < Steps.Num(); ++i)
	{
		const FPlaytestJevCall& Call = *Steps[i]->Call;
		const float X = XOf(i, Width);
		const float Next = i + 1 < Steps.Num() ? XOf(i + 1, Width) : FMath::Min(Size.X, X + 4.f);
		// What ran after this step
		const bool bNew = Call.Action == TEXT("started") || Call.Action == TEXT("switched");
		const FString Running = bNew || Call.NowDoing.IsEmpty() ? Call.Chosen : Call.NowDoing;
		if (!Running.IsEmpty())
		{
			Box(X, 0.f, Next - X, Lane, ColorOf(Running), LayerId + 1);
		}
		// A tick per step, a diamond per switch, a square per held one (not when narrow)
		if (!bCompact)
		{
			if (Call.Action == TEXT("switched"))
			{
				const FGeometry Turned = AllottedGeometry.MakeChild(FVector2f(7.f, 7.f), FSlateLayoutTransform(FVector2f(X - 3.5f, Marks + 1.5f)),
					FSlateRenderTransform(TQuat2<float>(0.25f * PI)), FVector2f(0.5f, 0.5f));
				FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2, Turned.ToPaintGeometry(), White, ESlateDrawEffect::None, AutopilotColor::Accent() * Tint);
			}
			else if (Call.Action == TEXT("kept_running"))
			{
				Box(X - 3.f, Marks + 2.f, 6.f, 6.f, AutopilotColor::Amber(), LayerId + 2);
			}
			else
			{
				Box(X, Marks, 1.5f, 10.f, AutopilotColor::LineHover(), LayerId + 1);
			}
		}
		if (Steps[i] == Picked)
		{
			Box(X - 1.f, -3.f, 2.f, (bCompact ? Lane : Marks + 10.f) + 6.f, AutopilotColor::Text(), LayerId + 3);
		}
	}
	// Game time, start and end
	const FSlateFontInfo Font = FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f);
	const double T0 = Steps[0]->Call->GameTime;
	const double T1 = Steps.Last()->Call->GameTime;
	if (T0 >= 0)
	{
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(FVector2f(80.f, 14.f), FSlateLayoutTransform(FVector2f(0.f, Times))),
			FString::Printf(TEXT("%.1fs"), T0), Font, ESlateDrawEffect::None, AutopilotColor::Muted() * Tint);
	}
	if (T1 >= 0 && Steps.Num() > 1)
	{
		const FString End = FString::Printf(TEXT("%.1fs"), T1);
		const float EndWidth = FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer()
			? (float)FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(End, Font).X : 40.f;
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(FVector2f(80.f, 14.f), FSlateLayoutTransform(FVector2f(FMath::Max(0.f, Size.X - EndWidth), Times))),
			End, Font, ESlateDrawEffect::None, AutopilotColor::Muted() * Tint);
	}
	return LayerId + 3;
}

FReply SAutopilotReplay::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!GetSteps || !OnPick)
	{
		return FReply::Unhandled();
	}
	const TArray<TSharedPtr<FAutopilotStep>>& Steps = GetSteps();
	if (Steps.Num() == 0)
	{
		return FReply::Unhandled();
	}
	const float X = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()).X;
	const float Width = FMath::Max(1.f, (float)MyGeometry.GetLocalSize().X - 2.f);
	int32 Best = 0;
	float BestDistance = TNumericLimits<float>::Max();
	for (int32 i = 0; i < Steps.Num(); ++i)
	{
		const float D = FMath::Abs(XOf(i, Width) - X);
		if (D < BestDistance)
		{
			BestDistance = D;
			Best = i;
		}
	}
	OnPick(Steps[Best]);
	return FReply::Handled();
}

// -- The policy log ----------------------------------------------------------------------------------------------

FString SPlaytestPolicyLog::TagOf(const FPlaytestJevCall& Call)
{
	if (Call.Failed())
	{
		return TEXT("FAILED");
	}
	if (Call.Action == TEXT("started"))
	{
		return TEXT("START");
	}
	if (Call.Action == TEXT("kept"))
	{
		return TEXT("KEEP");
	}
	if (Call.Action == TEXT("switched"))
	{
		return TEXT("SWITCH");
	}
	if (Call.Action == TEXT("kept_running"))
	{
		return TEXT("HELD");
	}
	if (Call.Action == TEXT("dropped"))
	{
		return TEXT("DROPPED");
	}
	return Call.Action.IsEmpty() ? FString(TEXT("STEP")) : Call.Action.ToUpper();
}

namespace
{
	/** "π(a | s)": a label's face and colour, never in caps. */
	TSharedRef<SWidget> PiLabel()
	{
		return SNew(STextBlock)
			.Text(LOCTEXT("Pi", "\u03c0(a | s)"))
			.Font(FAutopilotStyle::Label(10.f))
			.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()));
	}
}

void SPlaytestPolicyLog::Construct(const FArguments& InArgs)
{
	SAssignNew(Replay, SAutopilotReplay);
	Replay->GetSteps = [this]() -> const TArray<TSharedPtr<FAutopilotStep>>& { return StripSteps; };
	Replay->ColorOf = [this](const FString& Action) { return ColorOf(Action); };
	Replay->GetSelected = [this]() { return bFollow ? TSharedPtr<FAutopilotStep>() : SelectedStep; };
	Replay->OnPick = [this](const TSharedPtr<FAutopilotStep>& Step)
	{
		bFollow = false;  // (paused on it, the newest too: Follow live follows again)
		Select(Step);
	};

	auto Header = [](const FText& Text, bool bRight) -> TSharedRef<SWidget>
	{
		return SNew(SBox).HAlign(bRight ? HAlign_Right : HAlign_Left)[AutopilotUI::Label(Text)];
	};
	auto StripLabel = [this](const TAttribute<FText>& Text) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride_Lambda([this]() { return FOptionalSize(bVertical ? 84.f : 120.f); })[AutopilotUI::Label(Text)];
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[MakeFilterBar()]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		// Replay: what ran over the attempt's game time (the plate's fill, a rule under it)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAutopilotStyle::Brush("Autopilot.White"))
			.BorderBackgroundColor(FSlateColor(AutopilotColor::Plate()))
			.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bVertical ? FMargin(10.f, 8.f, 10.f, 6.f) : FMargin(16.f, 10.f, 16.f, 8.f); }))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 2.f, 0.f, 0.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							StripLabel(TAttribute<FText>::CreateLambda([this]()
							{
								const int32 Attempt = StripSteps.Num() ? StripSteps[0]->Attempt : 0;
								return Attempt > 0 ? FText::Format(LOCTEXT("RunningH", "H{0} · running"), Attempt) : LOCTEXT("Running", "Running");
							}))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
						[
							SNew(SBox)
							.Visibility_Lambda([this]() { return bVertical ? EVisibility::Collapsed : EVisibility::Visible; })
							[
								StripLabel(LOCTEXT("Steps", "Policy steps"))
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						Replay.ToSharedRef()
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				.Padding(TAttribute<FMargin>::CreateLambda([this]() { return FMargin(bVertical ? 84.f : 120.f, 2.f, 0.f, 0.f); }))
				[
					SAssignNew(Legend, SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(14.f, 4.f))
					.Visibility_Lambda([this]() { return bVertical ? EVisibility::Collapsed : EVisibility::Visible; })
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(Splitter, SSplitter)
			.Orientation(Orient_Horizontal)
			.PhysicalSplitterHandleSize(1.f)
			.Style(&FAppStyle::Get().GetWidgetStyle<FSplitterStyle>("Splitter"))
			+ SSplitter::Slot().Value(0.62f)
			[
				SNew(SVerticalBox)
				// The table's head: 28 tall (narrow 26), a rule under it
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox)
					.HeightOverride_Lambda([this]() { return FOptionalSize(bVertical ? 26.f : 28.f); })
					.VAlign(VAlign_Center)
					.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bVertical ? FMargin(12.f, 0.f, 10.f, 0.f) : FMargin(16.f, 0.f, 14.f, 0.f); }))
					[
						MakeColumns(Header(LOCTEXT("WallClock", "Wall clock"), false), Header(LOCTEXT("GameT", "Game t"), false), Header(LOCTEXT("StepNumber", "Step"), false),
							Header(LOCTEXT("CurrentAction", "Current action"), false), Header(LOCTEXT("Decision", "Decision"), false),
							Header(LOCTEXT("ActionProbabilities", "Action probabilities"), false), Header(LOCTEXT("Ms", "ms"), true))
					]
				]
				+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SAssignNew(List, SListView<TSharedPtr<FAutopilotStep>>)
					.ListItemsSource(&Rows)
					.OnGenerateRow(this, &SPlaytestPolicyLog::MakeRow)
					.SelectionMode(ESelectionMode::Single)
					.OnSelectionChanged_Lambda([this](TSharedPtr<FAutopilotStep> Step, ESelectInfo::Type How)
					{
						if (How == ESelectInfo::Direct || !Step.IsValid())
						{
							return;  // (selected by following)
						}
						bFollow = false;  // (paused on it, the newest too)
						SelectedStep = Step;
					})
				]
				+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule()]
				+ SVerticalBox::Slot().AutoHeight()
				.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bVertical ? FMargin(10.f, 6.f) : FMargin(16.f, 7.f); }))
				[
					SNew(STextBlock)
					.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f))
					.ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
					.AutoWrapText(true)
					.Text_Lambda([this]() { return Str(FooterText()); })
				]
			]
			+ SSplitter::Slot().Value(0.38f)
			[
				AutopilotUI::Fill(FSlateColor(AutopilotColor::Plate()), FMargin(0),
					// (the padding on a box in the slot: a scroll box's slot takes no padding that changes, Slate drops it)
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SBox).Padding(TAttribute<FMargin>::CreateLambda([this]() { return bVertical ? FMargin(12.f) : FMargin(18.f, 14.f); }))
						[
							SAssignNew(DetailSettle, AutopilotUI::SSettle)
						]
					])
			]
		]
	];
	ShowStep(nullptr);
}

TSharedRef<SWidget> SPlaytestPolicyLog::MakeColumns(const TSharedRef<SWidget>& Wall, const TSharedRef<SWidget>& Time, const TSharedRef<SWidget>& Number,
	const TSharedRef<SWidget>& Current, const TSharedRef<SWidget>& Decision, const TSharedRef<SWidget>& Probabilities, const TSharedRef<SWidget>& Milliseconds) const
{
	// Wide: wall clock 92, game t 52, step 50, current action 104, decision 176, the probabilities the rest, ms 30, 9 apart;
	// narrow: step 50, decision 150, the probabilities, ms 26, 8 apart
	auto Wide = [this]() { return bVertical ? EVisibility::Collapsed : EVisibility::Visible; };
	auto Gap = [this]() { return FMargin(0.f, 0.f, bVertical ? 8.f : 9.f, 0.f); };
	return SNew(SHorizontalBox)
	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(TAttribute<FMargin>::CreateLambda(Gap))[SNew(SBox).WidthOverride(92.f).Visibility_Lambda(Wide)[Wall]]
	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(TAttribute<FMargin>::CreateLambda(Gap))[SNew(SBox).WidthOverride(52.f).Visibility_Lambda(Wide)[Time]]
	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(TAttribute<FMargin>::CreateLambda(Gap))[SNew(SBox).WidthOverride(50.f)[Number]]
	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(TAttribute<FMargin>::CreateLambda(Gap))[SNew(SBox).WidthOverride(104.f).Visibility_Lambda(Wide)[Current]]
	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(TAttribute<FMargin>::CreateLambda(Gap))
	[
		SNew(SBox).WidthOverride_Lambda([this]() { return FOptionalSize(bVertical ? 150.f : 176.f); })[Decision]
	]
	+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(TAttribute<FMargin>::CreateLambda(Gap))[Probabilities]
	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride_Lambda([this]() { return FOptionalSize(bVertical ? 26.f : 30.f); })[Milliseconds]];
}

TSharedRef<SWidget> SPlaytestPolicyLog::MakeFilterBar()
{
	auto TagChip = [this](const FString& TagName)
	{
		return SNew(SBox)
		.Visibility_Lambda([this, TagName]()
		{
			return TagName != TEXT("FAILED") || TagCounts.FindRef(TagName) > 0 ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			AutopilotUI::Chip(TAttribute<FText>::CreateLambda([this, TagName]() { return Str(FString::Printf(TEXT("%s %d"), *TagName, TagCounts.FindRef(TagName))); }),
				TAttribute<ECheckBoxState>::CreateLambda([this, TagName]() { return HiddenTags.Contains(TagName) ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; }),
				FOnCheckStateChanged::CreateLambda([this, TagName](ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked)
					{
						HiddenTags.Remove(TagName);
					}
					else
					{
						HiddenTags.Add(TagName);
					}
					SyncedRevision = -1;
				}))
		];
	};
	TSharedRef<SWrapBox> Bar = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 6.f));
	Bar->AddSlot().VAlign(VAlign_Center)[AutopilotUI::Label(LOCTEXT("Attempt", "Policy"))];
	Bar->AddSlot().VAlign(VAlign_Center)[SAssignNew(AttemptChips, SHorizontalBox)];
	Bar->AddSlot().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f))
	[
		SNew(SBox).WidthOverride(1.f).HeightOverride(18.f).Visibility_Lambda([this]() { return bVertical ? EVisibility::Collapsed : EVisibility::Visible; })
		[
			AutopilotUI::Fill(FSlateColor(AutopilotColor::Line()))
		]
	];
	Bar->AddSlot().VAlign(VAlign_Center)[AutopilotUI::Label(LOCTEXT("Show", "Show"))];
	for (const TCHAR* TagName : Tags)
	{
		Bar->AddSlot().VAlign(VAlign_Center)[TagChip(TagName)];
	}
	Bar->AddSlot().FillEmptySpace(true).HAlign(HAlign_Right).VAlign(VAlign_Center)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(200.f).HeightOverride(24.f)
			[
				SNew(SEditableTextBox)
				.Style(&FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Small"))
				.HintText(LOCTEXT("Find", "Find an action…"))
				.AccessibleText(LOCTEXT("FindName", "Find an action"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Search = Text.ToString().TrimStartAndEnd();
					SyncedRevision = -1;
				})
			]
		]
		// Following the newest step, or paused on the one picked (Follow live)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return bFollow ? 0 : 1; })
			+ SWidgetSwitcher::Slot().VAlign(VAlign_Center)
			[
				AutopilotUI::Mono(LOCTEXT("Following", "● FOLLOWING"), 10.f, FSlateColor(AutopilotColor::Accent()), false, 100)
			]
			+ SWidgetSwitcher::Slot().VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
				[
					AutopilotUI::Mono(TAttribute<FText>::CreateLambda([this]()
					{
						return SelectedStep.IsValid() ? Str(FString::Printf(TEXT("PAUSED ON H%d #%lld"), SelectedStep->Attempt, SelectedStep->Call->Call)) : LOCTEXT("Paused", "PAUSED");
					}), 10.f, FSlateColor(AutopilotColor::Amber()), false, 100)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					AutopilotUI::Button(LOCTEXT("Live", "Follow live"), "Autopilot.Button", FOnClicked::CreateLambda([this]()
					{
						bFollow = true;
						Select(Rows.Num() ? Rows.Last() : nullptr);
						return FReply::Handled();
					}), true, nullptr, true)
				]
			]
		]
	];
	return SNew(SBox)
		.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bVertical ? FMargin(10.f, 8.f) : FMargin(16.f, 8.f); }))
		[
			Bar
		];
}

void SPlaytestPolicyLog::RebuildAttemptChips()
{
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	const int32 Attempts = R.IsValid() ? R->StepAttempts : 0;
	if (!AttemptChips.IsValid() || Attempts == ChipAttempts)
	{
		return;
	}
	ChipAttempts = Attempts;
	AttemptChips->ClearChildren();
	auto Count = [this](int32 Attempt)
	{
		const TSharedPtr<FAutopilotRun> Pinned = Run.Pin();
		int32 N = 0;
		if (Pinned.IsValid())
		{
			for (const TSharedPtr<FAutopilotStep>& Step : Pinned->Steps)
			{
				N += (Attempt == 0 || Step->Attempt == Attempt) ? 1 : 0;
			}
		}
		return N;
	};
	for (int32 Attempt = (Attempts > 1 ? 0 : 1); Attempt <= Attempts; ++Attempt)
	{
		AttemptChips->AddSlot().AutoWidth().Padding(AttemptChips->NumSlots() ? 6.f : 0.f, 0.f, 0.f, 0.f)
		[
			AutopilotUI::Chip(TAttribute<FText>::CreateLambda([Attempt, Count]()
			{
				return Str(Attempt == 0 ? FString::Printf(TEXT("All · %d"), Count(0)) : FString::Printf(TEXT("H%d · %d"), Attempt, Count(Attempt)));
			}),
			TAttribute<ECheckBoxState>::CreateLambda([this, Attempt]()
			{
				return AttemptFilter == Attempt || (Attempt == 1 && ChipAttempts == 1) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			}),
			FOnCheckStateChanged::CreateLambda([this, Attempt](ECheckBoxState)
			{
				AttemptFilter = Attempt;
				SyncedRevision = -1;
			}))
		];
	}
}

void SPlaytestPolicyLog::RebuildLegend()
{
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	// The actions that ran in the harness the strip shows (current or chosen), by their colours' order, at most 8
	TArray<TPair<int32, FString>> Ordered;
	if (R.IsValid())
	{
		for (const TSharedPtr<FAutopilotStep>& Step : StripSteps)
		{
			for (const FString& Name : { Step->Call->NowDoing, Step->Call->Chosen })
			{
				const int32* At = R->ActionColors.Find(Name);
				if (!Name.IsEmpty() && At && !Ordered.ContainsByPredicate([&Name](const TPair<int32, FString>& Item) { return Item.Value == Name; }))
				{
					Ordered.Add({ *At, Name });
				}
			}
		}
	}
	Ordered.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
	if (Ordered.Num() > 8)
	{
		Ordered.SetNum(8);
	}
	int32 N = Ordered.Num() * 1000;
	for (const TPair<int32, FString>& Item : Ordered)
	{
		N = N * 31 + Item.Key + 1;
	}
	if (!Legend.IsValid() || N == LegendCount)
	{
		return;
	}
	LegendCount = N;
	Legend->ClearChildren();
	if (!R.IsValid())
	{
		return;
	}
	auto Key = [](const TSharedRef<SWidget>& Mark, const FText& Name, const FLinearColor& Color)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)[Mark]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[AutopilotUI::Mono(Name, 10.f, FSlateColor(Color))];
	};
	auto Square = [](float Side, const FLinearColor& Color) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(Side).HeightOverride(Side)[AutopilotUI::Fill(FSlateColor(Color))];
	};
	for (const TPair<int32, FString>& Item : Ordered)
	{
		Legend->AddSlot()[Key(Square(8.f, FAutopilotStyle::ActionColor(Item.Key)), Str(Item.Value), AutopilotColor::Text3())];
	}
	Legend->AddSlot()[Key(Square(8.f, AutopilotColor::Amber()), LOCTEXT("HeldMark", "held"), AutopilotColor::Muted())];
	// (the switch's mark: a 7px diamond)
	TSharedRef<SWidget> Diamond = SNew(SBox).WidthOverride(8.f).HeightOverride(8.f).HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(6.f).HeightOverride(6.f)
			.RenderTransformPivot(FVector2D(0.5, 0.5))
			.RenderTransform(TOptional<FSlateRenderTransform>(FSlateRenderTransform(TQuat2<float>(0.25f * PI))))
			[
				AutopilotUI::Fill(FSlateColor(AutopilotColor::Accent()))
			]
		];
	Legend->AddSlot()[Key(Diamond, LOCTEXT("SwitchMark", "switch"), AutopilotColor::Muted())];
}

void SPlaytestPolicyLog::SetRun(const TSharedPtr<FAutopilotRun>& InRun)
{
	const int32 Id = InRun.IsValid() ? InRun->Id : INDEX_NONE;
	if (Id == RunId)
	{
		return;
	}
	RunId = Id;
	Run = InRun;
	SyncedRevision = -1;
	ChipAttempts = -1;
	LegendCount = -1;
	AttemptFilter = 0;
	HiddenTags.Reset();
	bFollow = true;
	SelectedStep.Reset();
	ShownStep.Reset();
	Rows.Reset();
	StripSteps.Reset();
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
	ShowStep(nullptr);
}

bool SPlaytestPolicyLog::Passes(const FAutopilotStep& Step) const
{
	if (AttemptFilter > 0 && Step.Attempt != AttemptFilter)
	{
		return false;
	}
	const FPlaytestJevCall& Call = *Step.Call;
	if (HiddenTags.Contains(TagOf(Call)))
	{
		return false;
	}
	if (!Search.IsEmpty() && !Call.NowDoing.Contains(Search) && !Call.Chosen.Contains(Search))
	{
		return false;
	}
	return true;
}

void SPlaytestPolicyLog::SyncRows()
{
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	SyncedRevision = R.IsValid() ? R->StepRevision : -1;
	RebuildAttemptChips();
	Rows.Reset();
	TagCounts.Reset();
	StripSteps.Reset();
	TArray<float> Latencies;
	if (R.IsValid())
	{
		// The replay strip: the attempt picked, else the picked step's, else the latest
		const int32 StripAttempt = AttemptFilter > 0 ? AttemptFilter
			: (!bFollow && SelectedStep.IsValid() ? SelectedStep->Attempt : R->StepAttempts);
		for (const TSharedPtr<FAutopilotStep>& Step : R->Steps)
		{
			if (Step->Attempt == StripAttempt)
			{
				StripSteps.Add(Step);
			}
			if (AttemptFilter > 0 && Step->Attempt != AttemptFilter)
			{
				continue;
			}
			TagCounts.FindOrAdd(TagOf(*Step->Call))++;
			if (!Step->Call->Failed() && Step->Call->Seconds() >= 0)
			{
				Latencies.Add((float)(Step->Call->Seconds() * 1000.0));
			}
			if (Passes(*Step))
			{
				Rows.Add(Step);
			}
		}
	}
	RebuildLegend();
	FooterKey.Empty();  // (the footer is made again: FooterText)
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
	if (bFollow)
	{
		Select(Rows.Num() ? Rows.Last() : nullptr);
	}
}

void SPlaytestPolicyLog::Select(const TSharedPtr<FAutopilotStep>& Step)
{
	SelectedStep = Step;
	if (!List.IsValid())
	{
		return;
	}
	if (Step.IsValid() && Rows.Contains(Step))
	{
		List->SetSelection(Step, ESelectInfo::Direct);
		List->RequestScrollIntoView(Step);
	}
	else
	{
		List->ClearSelection();
	}
}

void SPlaytestPolicyLog::StepBy(int32 Delta)
{
	const int32 At = Rows.IndexOfByKey(SelectedStep);
	const int32 To = At == INDEX_NONE ? (Delta < 0 ? Rows.Num() - 1 : 0) : At + Delta;
	if (Rows.IsValidIndex(To))
	{
		bFollow = false;
		Select(Rows[To]);
	}
}

void SPlaytestPolicyLog::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	const float Width = AllottedGeometry.GetLocalSize().X;
	const bool bWantVertical = Width < VerticalBelow;
	if (Splitter.IsValid() && bWantVertical != bVertical)
	{
		bVertical = bWantVertical;
		Splitter->SetOrientation(bVertical ? Orient_Vertical : Orient_Horizontal);
		Splitter->SlotAt(0).SetSizeValue(bVertical ? 0.45f : 0.62f);
		Splitter->SlotAt(1).SetSizeValue(bVertical ? 0.55f : 0.38f);
		if (Replay.IsValid())
		{
			Replay->bCompact = bVertical;
			Replay->Invalidate(EInvalidateWidgetReason::Layout);
		}
		ShownStep.Reset();  // (the step picked is laid out again for the other side)
	}
	if (Splitter.IsValid() && !bVertical && Width > DetailWidth * 1.5f && !bDetailSized)
	{
		// (the step picked: 440 wide, the table the rest; dragged after that, as dragged)
		bDetailSized = true;
		Splitter->SlotAt(0).SetSizeValue(1.f - DetailWidth / Width);
		Splitter->SlotAt(1).SetSizeValue(DetailWidth / Width);
	}
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	if (R.IsValid() && R->StepRevision != SyncedRevision)
	{
		SyncRows();
	}
	if ((SelectedStep != ShownStep || bRaw != bShownRaw) && (!bFollow || InCurrentTime - LastShown >= FollowRefreshSeconds))
	{
		LastShown = InCurrentTime;
		ShowStep(SelectedStep);
	}
}

FString SPlaytestPolicyLog::FooterText() const
{
	// How the steps shown went: "All harnesses: …" for All with no step picked, else the harness shown ("H2: 26 steps ·
	// median 84 ms · max 312 ms · 4 switches · 1 start · 1 held · 1 dropped")
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	const int32 Shown = AttemptFilter > 0 ? AttemptFilter : (!bFollow && SelectedStep.IsValid() ? SelectedStep->Attempt : 0);
	const FString Key = FString::Printf(TEXT("%d|%d|%d|%d"), SyncedRevision, Shown, Rows.Num(), R.IsValid() ? R->Steps.Num() : -1);
	if (Key == FooterKey)
	{
		return Footer;
	}
	FooterKey = Key;
	TMap<FString, int32> Counts;
	TArray<float> Latencies;
	int32 Steps = 0;
	if (R.IsValid())
	{
		for (const TSharedPtr<FAutopilotStep>& Step : R->Steps)
		{
			if (Shown > 0 && Step->Attempt != Shown)
			{
				continue;
			}
			++Steps;  // (every step of the harness shown, or of all: not only the rows the filters keep)
			Counts.FindOrAdd(TagOf(*Step->Call))++;
			if (!Step->Call->Failed() && Step->Call->Seconds() > 0)
			{
				Latencies.Add((float)(Step->Call->Seconds() * 1000.0));
			}
		}
	}
	Latencies.Sort();
	FString Text = FString::Printf(TEXT("%s: %s"), Shown > 0 ? *FString::Printf(TEXT("H%d"), Shown) : TEXT("All harnesses"), *Count(Steps, TEXT("step"), TEXT("steps")));
	if (Latencies.Num())
	{
		Text += FString::Printf(TEXT(" · median %.0f ms · max %.0f ms"), Latencies[Latencies.Num() / 2], Latencies.Last());
	}
	Text += TEXT(" · ") + Count(Counts.FindRef(TEXT("SWITCH")), TEXT("switch"), TEXT("switches"))
		+ TEXT(" · ") + Count(Counts.FindRef(TEXT("START")), TEXT("start"), TEXT("starts"))
		+ FString::Printf(TEXT(" · %d held · %d dropped"), Counts.FindRef(TEXT("HELD")), Counts.FindRef(TEXT("DROPPED")));
	if (Counts.FindRef(TEXT("FAILED")))
	{
		Text += FString::Printf(TEXT(" · %d failed"), Counts.FindRef(TEXT("FAILED")));
	}
	if (R.IsValid() && R->StepCount > R->Steps.Num())
	{
		Text += FString::Printf(TEXT(" · the oldest %d no longer kept"), R->StepCount - R->Steps.Num());
	}
	Footer = Text;
	return Footer;
}

FLinearColor SPlaytestPolicyLog::ColorOf(const FString& Action) const
{
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	return R.IsValid() ? R->ColorOf(Action) : AutopilotColor::Text2();
}

TSharedRef<SWidget> SPlaytestPolicyLog::MakeProbabilityBar(const FPlaytestJevCall& Call, int32 Most) const
{
	TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
	float Sum = 0.f;
	float Rest = 0.f;
	for (int32 i = 0; i < FMath::Min(Most, Call.Pick.Num()); ++i)
	{
		const float P = FMath::Clamp(Call.Pick[i].P, 0.f, 1.f);
		Sum += P;
		if (i < 2)
		{
			Bar->AddSlot().FillWidth(FMath::Max(P, 0.001f))[AutopilotUI::Fill(FSlateColor(ColorOf(Call.Pick[i].Name)))];
		}
		else
		{
			Rest += P;
		}
	}
	if (Rest > 0.f)
	{
		Bar->AddSlot().FillWidth(Rest)[AutopilotUI::Fill(FSlateColor(AutopilotColor::LineHover()))];  // (the rest: one #3a3e47 segment)
	}
	if (Sum < 1.f)
	{
		Bar->AddSlot().FillWidth(1.f - Sum)[SNullWidget::NullWidget];
	}
	return AutopilotUI::Fill(FSlateColor(AutopilotColor::Hover()), FMargin(0), SNew(SBox).HeightOverride(9.f)[Bar]);
}

TSharedRef<ITableRow> SPlaytestPolicyLog::MakeRow(TSharedPtr<FAutopilotStep> Step, const TSharedRef<STableViewBase>& Owner)
{
	// A step: 28 tall, mono 11, a #1d2026 rule under it; picked (paused on it): #2a1b10 and the 2px accent rule at its left
	const FPlaytestJevCall& Call = *Step->Call;
	const FString TagName = TagOf(Call);
	const bool bNowDoing = !Call.NowDoing.IsEmpty();
	FString Chosen = Call.Failed() ? FAutopilotSession::Scrub(Call.Error) : Call.Chosen;
	if (Chosen.IsEmpty())
	{
		Chosen = TEXT("—");
	}
	const bool bSwitch = TagName == TEXT("SWITCH");
	TSharedRef<SWidget> Probabilities = AutopilotUI::Mono(Str(TEXT("—")), 11.f, FSlateColor(AutopilotColor::Muted()));
	if (Call.Pick.Num())
	{
		Probabilities = MakeProbabilityBar(Call, 4);
	}
	TWeakPtr<FAutopilotStep> Weak = Step;
	auto Text = [](const FString& Words, const FLinearColor& Color, bool bMedium = false) -> TSharedRef<SWidget>
	{
		return SNew(STextBlock)
			.Text(Str(Words))
			.Font(FAutopilotStyle::Font(bMedium ? EAutopilotFont::MonoBold : EAutopilotFont::Mono, 11.f))
			.ColorAndOpacity(FSlateColor(Color))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
	};
	return SNew(STableRow<TSharedPtr<FAutopilotStep>>, Owner)
	.Style(&FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row.Hover"))
	.Padding(FMargin(0.f))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			AutopilotUI::Picked(TAttribute<bool>::CreateLambda([this, Weak]() { return !bFollow && Weak.Pin().IsValid() && Weak.Pin() == SelectedStep; }),
				SNew(SBox)
				.HeightOverride(27.f)
				.VAlign(VAlign_Center)
				.Padding(TAttribute<FMargin>::CreateLambda([this]() { return bVertical ? FMargin(12.f, 0.f, 10.f, 0.f) : FMargin(16.f, 0.f, 14.f, 0.f); }))
				[
					MakeColumns(
						Text(PlaytestInspect::ClockText(Call.WallTime), AutopilotColor::Muted()),
						Text(Call.GameTime >= 0 ? FString::Printf(TEXT("%.2fs"), Call.GameTime) : FString(TEXT("—")), AutopilotColor::Text2()),
						Text(FString::Printf(TEXT("H%d #%lld"), Step->Attempt, Call.Call), AutopilotColor::Muted()),
						Text(bNowDoing ? Call.NowDoing : FString(TEXT("—")), bNowDoing ? ColorOf(Call.NowDoing) : AutopilotColor::Muted()),
						SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)[StepTag(TagName)]
							+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(Str(Chosen))
								.ToolTipText(Str(Chosen))
								.Font(FAutopilotStyle::Font(bSwitch ? EAutopilotFont::MonoBold : EAutopilotFont::Mono, 11.f))
								.ColorAndOpacity(FSlateColor(Call.Failed() ? AutopilotColor::Red() : Call.Chosen.IsEmpty() ? AutopilotColor::Muted() : ColorOf(Call.Chosen)))
								.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							],
						Probabilities,
						SNew(SBox).HAlign(HAlign_Right)[Text(Ms(Call.Seconds()), AutopilotColor::Muted())])
				])
		]
		+ SVerticalBox::Slot().AutoHeight()[AutopilotUI::Rule(AutopilotColor::Hover())]
	];
}

TSharedPtr<FAutopilotStep> SPlaytestPolicyLog::PreviousInAttempt(const TSharedPtr<FAutopilotStep>& Step) const
{
	const TSharedPtr<FAutopilotRun> R = Run.Pin();
	if (!R.IsValid() || !Step.IsValid())
	{
		return nullptr;
	}
	const int32 At = R->Steps.IndexOfByKey(Step);
	return At > 0 && R->Steps[At - 1]->Attempt == Step->Attempt ? R->Steps[At - 1] : nullptr;
}

FString SPlaytestPolicyLog::FrameJson(const FPlaytestJevCall& Call) const
{
	const FString Frame = Call.Frame.Unpack();
	return Frame.IsEmpty() ? RawJson(Call) : Frame;  // (a step from before frames were kept: made from its parts)
}

FString SPlaytestPolicyLog::RawJson(const FPlaytestJevCall& Call) const
{
	// The frame as the agent service sent it, without the instructions it was given
	TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetNumberField(TEXT("call"), (double)Call.Call);
	if (Call.Step >= 0)
	{
		Frame->SetNumberField(TEXT("step"), Call.Step);
	}
	if (Call.GameTime >= 0)
	{
		Frame->SetNumberField(TEXT("t"), Call.GameTime);
	}
	Frame->SetNumberField(TEXT("wall"), Call.WallTime);
	if (!Call.Goal.IsEmpty())
	{
		Frame->SetStringField(TEXT("goal"), Call.Goal);
	}
	if (!Call.PlanStep.IsEmpty())
	{
		Frame->SetStringField(TEXT("plan_step"), Call.PlanStep);
	}
	if (Call.Failed())
	{
		Frame->SetStringField(TEXT("error"), FAutopilotSession::Scrub(Call.Error));
	}
	if (TSharedPtr<FJsonObject> Input = PlaytestJson::Parse(Call.Input.Unpack()))
	{
		if (TSharedPtr<FJsonObject> Questions = ObjectOf(Input, TEXT("questions")))
		{
			for (auto& Pair : Questions->Values)
			{
				const TSharedPtr<FJsonObject>* Question = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Question) && Question && Question->IsValid())
				{
					(*Question)->RemoveField(TEXT("instructions"));
				}
			}
		}
		Frame->SetObjectField(TEXT("input"), Input);
	}
	if (TSharedPtr<FJsonObject> Output = PlaytestJson::Parse(Call.Output.Unpack()))
	{
		Frame->SetObjectField(TEXT("output"), Output);
	}
	if (TSharedPtr<FJsonObject> Decided = PlaytestJson::Parse(Call.Decided.Unpack()))
	{
		if (TSharedPtr<FJsonObject> Seconds = ObjectOf(Decided, TEXT("seconds")))
		{
			TSharedPtr<FJsonValue> Policy = Seconds->TryGetField(TEXT("jev"));
			if (Policy.IsValid())
			{
				Seconds->RemoveField(TEXT("jev"));
				Seconds->SetField(TEXT("policy"), Policy);
			}
		}
		Frame->SetObjectField(TEXT("decided"), Decided);
	}
	return PlaytestInspect::Pretty(TSharedPtr<FJsonObject>(Frame));
}

void SPlaytestPolicyLog::ShowStep(const TSharedPtr<FAutopilotStep>& Step)
{
	ShownStep = Step;
	bShownRaw = bRaw;
	if (!DetailSettle.IsValid())
	{
		return;
	}
	// (made again, the step before shown until this one is laid out)
	SAssignNew(Detail, SVerticalBox);
	DetailSettle->SetContent(Detail.ToSharedRef());
	const float Gap = bVertical ? 10.f : 12.f;
	auto Add = [this](const TSharedRef<SWidget>& Widget, float Top)
	{
		Detail->AddSlot().AutoHeight().Padding(0.f, Detail->NumSlots() ? Top : 0.f, 0.f, 0.f)[Widget];
	};
	if (!Step.IsValid())
	{
		const TSharedPtr<FAutopilotRun> R = Run.Pin();
		Add(AutopilotUI::Body(R.IsValid() && R->Steps.Num() ? LOCTEXT("PickStep", "Pick a step to see what the policy observed and chose.")
			: LOCTEXT("NoSteps", "No policy steps yet."), 12.f, AutopilotColor::Muted()), 0.f);
		return;
	}
	const FPlaytestJevCall& Call = *Step->Call;
	const TSharedPtr<FJsonObject> Input = PlaytestJson::Parse(Call.Input.Unpack());
	const TSharedPtr<FJsonObject> Output = PlaytestJson::Parse(Call.Output.Unpack());
	const TSharedPtr<FJsonObject> State = ObjectOf(Input, TEXT("state"));
	const TSharedPtr<FJsonObject> Criteria = ObjectOf(ObjectOf(ObjectOf(Input, TEXT("questions")), TEXT("pick")), TEXT("criteria"));
	auto LabelOf = [&Criteria](const FString& Action)
	{
		FString Text;
		return Criteria.IsValid() && Criteria->TryGetStringField(Action, Text) ? Text : FString();
	};
	const FString TagName = TagOf(Call);
	const TMap<FString, FString> LongTags = { { TEXT("SWITCH"), TEXT("SWITCHED") }, { TEXT("START"), TEXT("STARTED") }, { TEXT("KEEP"), TEXT("KEPT") } };

	// Which step, what it did (its tag, what ran → what it chose), and what was running then
	TSharedRef<SVerticalBox> Head = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			AutopilotUI::Mono(Str(FString::Printf(TEXT("H%d · #%lld · %s · %s · %s ms"), Step->Attempt, Call.Call,
				Call.GameTime >= 0 ? *FString::Printf(TEXT("t=%.1fs"), Call.GameTime) : TEXT("t=\u2014"),
				*PlaytestInspect::ClockText(Call.WallTime), *Ms(Call.Seconds()))), 10.f, FSlateColor(AutopilotColor::Muted()))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
		[
			SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8.f, 4.f))
			+ SWrapBox::Slot().VAlign(VAlign_Center)[StepTag(LongTags.Contains(TagName) ? LongTags[TagName] : TagName, true)]
			+ SWrapBox::Slot().VAlign(VAlign_Center)
			[
				AutopilotUI::Mono(Str(Call.NowDoing.IsEmpty() ? FString(TEXT("(nothing running)")) : Call.NowDoing), 12.f,
					FSlateColor(Call.NowDoing.IsEmpty() ? AutopilotColor::Muted() : ColorOf(Call.NowDoing)))
			]
			+ SWrapBox::Slot().VAlign(VAlign_Center)[AutopilotUI::Mono(Str(TEXT("→")), 12.f, FSlateColor(AutopilotColor::Muted()))]
			+ SWrapBox::Slot().VAlign(VAlign_Center)
			[
				AutopilotUI::Mono(Str(Call.Chosen.IsEmpty() ? FString(TEXT("(none)")) : Call.Chosen), 12.f,
					FSlateColor(Call.Chosen.IsEmpty() ? AutopilotColor::Muted() : ColorOf(Call.Chosen)), true)
			]
		];
	if (!Call.NowDoing.IsEmpty())
	{
		// Action: "Walk to the gate along the left fence" (4.2 s)
		const FString Label = LabelOf(Call.NowDoing);
		FString Line = FString::Printf(TEXT("Action: \"%s\""), Label.IsEmpty() ? *Call.NowDoing : *Label);
		if (Call.NowDoingSeconds >= 0)
		{
			Line += FString::Printf(TEXT(" (%.1f s)"), Call.NowDoingSeconds);
		}
		Head->AddSlot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)[AutopilotUI::Body(Str(Line), 12.f, AutopilotColor::Text3())];
	}
	if (Call.Failed())
	{
		Head->AddSlot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
		[
			AutopilotUI::Body(Str(FString::Printf(TEXT("This step failed: %s"), *FAutopilotSession::Scrub(Call.Error))), 12.f, AutopilotColor::Red())
		];
	}
	Add(Head, 0.f);

	// The policy's output: pi(a | s), an action a row (its name, " · current", its bar, its probability)
	const float NameWidth = bVertical ? 130.f : 160.f;
	TSharedRef<SVerticalBox> Out = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 3.f)[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[AutopilotUI::Label(LOCTEXT("PolicyOutput", "Policy output · "))]
			+ SHorizontalBox::Slot().AutoWidth()[PiLabel()]
		];
	for (const FPlaytestPick& Pick : Call.Pick)
	{
		const bool bChosen = Pick.Name == Call.Chosen;
		const bool bCurrent = Pick.Name == Call.NowDoing;
		Out->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(SBox).WidthOverride(NameWidth).Clipping(EWidgetClipping::ClipToBounds).ToolTipText(Str(LabelOf(Pick.Name)))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock)
						.Text(Str(Pick.Name))
						.Font(FAutopilotStyle::Font(bChosen ? EAutopilotFont::MonoBold : EAutopilotFont::Mono, 11.5f))
						.ColorAndOpacity(FSlateColor(bChosen ? ColorOf(Pick.Name) : AutopilotColor::Text2()))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						AutopilotUI::Mono(bCurrent ? LOCTEXT("CurrentMark", " · current") : FText::GetEmpty(), 11.5f, FSlateColor(AutopilotColor::Muted()))
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				AutopilotUI::Fill(FSlateColor(AutopilotColor::Hover()), FMargin(0),
					SNew(SBox).HeightOverride(10.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(FMath::Max(Pick.P, 0.001f))[AutopilotUI::Fill(FSlateColor(ColorOf(Pick.Name)))]
						+ SHorizontalBox::Slot().FillWidth(FMath::Max(1.f - Pick.P, 0.f))[SNullWidget::NullWidget]
					])
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SBox).WidthOverride(34.f).HAlign(HAlign_Right)[AutopilotUI::Mono(Str(Prob(Pick.P)), 11.5f, FSlateColor(AutopilotColor::Text()))]
			]
		];
	}
	if (Call.Pick.Num() == 0)
	{
		Out->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)[AutopilotUI::Body(LOCTEXT("NoPick", "No action probabilities in this step."), 12.f, AutopilotColor::Muted())];
	}
	Add(Out, Gap);

	// The observation: Readable or Raw JSON, Copy step JSON, the step before and after
	Add(SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 6.f))
		+ SWrapBox::Slot().FillEmptySpace(true).VAlign(VAlign_Center)
		[
			AutopilotUI::Label(FText::Format(LOCTEXT("Observation", "Observation · #{0}"), FText::AsNumber(Call.Call, &FNumberFormattingOptions::DefaultNoGrouping())),
				FSlateColor(AutopilotColor::Accent()))
		]
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::Chip(LOCTEXT("Readable", "Readable"),
				TAttribute<ECheckBoxState>::CreateLambda([this]() { return bRaw ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; }),
				FOnCheckStateChanged::CreateLambda([this](ECheckBoxState) { bRaw = false; }))
		]
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::Chip(LOCTEXT("RawJson", "Raw JSON"),
				TAttribute<ECheckBoxState>::CreateLambda([this]() { return bRaw ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }),
				FOnCheckStateChanged::CreateLambda([this](ECheckBoxState) { bRaw = true; }))
		]
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::Button(LOCTEXT("CopyStep", "Copy step JSON"), "Autopilot.Button", FOnClicked::CreateLambda([this, Step]()
			{
				if (Step.IsValid())
				{
					FPlatformApplicationMisc::ClipboardCopy(*FrameJson(*Step->Call));
					FAutopilotSession::Get().Say(TEXT("Copied"));
				}
				return FReply::Handled();
			}), true, nullptr, true)
		]
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::IconButton(FAutopilotStyle::Brush("Autopilot.Back"), LOCTEXT("Previous", "Previous step"), "Autopilot.Button",
				FOnClicked::CreateLambda([this]() { StepBy(-1); return FReply::Handled(); }),
				TAttribute<bool>::CreateLambda([this, Step]() { return Rows.IndexOfByKey(Step) > 0; }), true)
		]
		+ SWrapBox::Slot().VAlign(VAlign_Center)
		[
			AutopilotUI::IconButton(FAutopilotStyle::Brush("Autopilot.Forward"), LOCTEXT("Next", "Next step"), "Autopilot.Button",
				FOnClicked::CreateLambda([this]() { StepBy(1); return FReply::Handled(); }),
				TAttribute<bool>::CreateLambda([this, Step]() { const int32 At = Rows.IndexOfByKey(Step); return At != INDEX_NONE && At < Rows.Num() - 1; }), true)
		], Gap);
	if (bRaw)
	{
		Add(SNew(SMultiLineEditableTextBox)
			.Style(&FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Code"))
			.IsReadOnly(true)
			.AutoWrapText(false)
			.AlwaysShowScrollbars(false)
			.Text(Str(FrameJson(Call))), Gap);
		return;
	}

	auto Section = [&Add](const FText& Title, const TSharedRef<SWidget>& Content, const TSharedPtr<SWidget>& Extra)
	{
		Add(Sec(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 7.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[AutopilotUI::Label(Title)]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Extra.IsValid() ? Extra.ToSharedRef() : SNullWidget::NullWidget]
			]
			+ SVerticalBox::Slot().AutoHeight()[Content]), 10.f);
	};
	const float KeyWidth = bVertical ? 110.f : 130.f;
	auto KeyValue = [KeyWidth](const FString& Key, const FString& Value, bool bChanged)
	{
		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(KeyWidth)
			[
				SNew(STextBlock).Text(Str(Key)).Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f)).ColorAndOpacity(FSlateColor(AutopilotColor::Muted()))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
		[
			SNew(STextBlock)
			.Text(Str(bChanged ? TEXT("● ") + Value : Value))
			.Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f))
			.ColorAndOpacity(FSlateColor(bChanged ? AutopilotColor::Accent() : AutopilotColor::Text2()))
			.AutoWrapText(true)
		];
	};

	// Task
	FString Task;
	if (!State.IsValid() || !State->TryGetStringField(TEXT("goal"), Task))
	{
		Task = Call.Goal;
	}
	if (!Task.IsEmpty())
	{
		Add(Sec(SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 2.f, 12.f, 0.f)[SNew(SBox).WidthOverride(50.f)[AutopilotUI::Label(LOCTEXT("Task", "Task"))]]
			+ SHorizontalBox::Slot().FillWidth(1.f)[AutopilotUI::Body(Str(Task), 13.f, AutopilotColor::Text2())]), 10.f);
	}

	// Environment state: what changed since the step before marked
	if (const TSharedPtr<FJsonObject> Situation = ObjectOf(State, TEXT("situation")))
	{
		TSharedPtr<FJsonObject> Before;
		const TSharedPtr<FAutopilotStep> Previous = PreviousInAttempt(Step);
		if (Previous.IsValid())
		{
			Before = ObjectOf(ObjectOf(PlaytestJson::Parse(Previous->Call->Input.Unpack()), TEXT("state")), TEXT("situation"));
		}
		TSharedRef<SVerticalBox> StateRows = SNew(SVerticalBox);
		int32 Changed = 0;
		for (const auto& Pair : Situation->Values)
		{
			const FString Value = Compact(Pair.Value);
			const TSharedPtr<FJsonValue> Old = Before.IsValid() ? Before->TryGetField(Pair.Key) : nullptr;
			const bool bChanged = Before.IsValid() && (!Old.IsValid() || Compact(Old) != Value);
			Changed += bChanged ? 1 : 0;
			StateRows->AddSlot().AutoHeight().Padding(0.f, 1.f)[KeyValue(FString(*Pair.Key), Value, bChanged)];
		}
		TSharedPtr<SWidget> Extra;
		if (Before.IsValid() && Changed > 0)
		{
			// (narrow: "● 3 changed"; none: nothing)
			const FString Since = bVertical ? FString::Printf(TEXT("● %d changed"), Changed)
				: FString::Printf(TEXT("● %d changed since #%lld"), Changed, Previous.IsValid() ? Previous->Call->Call : 0ll);
			Extra = AutopilotUI::Mono(Str(Since), 10.f, FSlateColor(AutopilotColor::Accent()));
		}
		Section(LOCTEXT("Environment", "Observation · environment state"), StateRows, Extra);
	}

	// Current action
	{
		FString Text = TEXT("none");
		if (!Call.NowDoing.IsEmpty())
		{
			Text = Call.NowDoingSeconds >= 0 ? FString::Printf(TEXT("%s for %.1f s"), *Call.NowDoing, Call.NowDoingSeconds) : Call.NowDoing;
		}
		Section(LOCTEXT("CurrentActionTitle", "Current action"),
			AutopilotUI::Mono(Str(Text), 11.5f, FSlateColor(Call.NowDoing.IsEmpty() ? AutopilotColor::Muted() : ColorOf(Call.NowDoing))), nullptr);
	}

	// Recent actions and how they went (success, failure)
	const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
	if (State.IsValid() && State->TryGetArrayField(TEXT("recent"), Recent) && Recent && Recent->Num())
	{
		TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
		for (const TSharedPtr<FJsonValue>& Item : *Recent)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(Entry) || !Entry)
			{
				continue;
			}
			FString Option;
			(*Entry)->TryGetStringField(TEXT("option"), Option);
			double Seconds = -1;
			(*Entry)->TryGetNumberField(TEXT("seconds"), Seconds);
			FString How;
			(*Entry)->TryGetStringField(TEXT("how"), How);
			const TSharedPtr<FJsonValue> Worked = (*Entry)->TryGetField(TEXT("worked"));
			const bool bKnown = Worked.IsValid() && Worked->Type == EJson::Boolean;
			const bool bWorked = bKnown && Worked->AsBool();
			Lines->AddSlot().AutoHeight().Padding(0.f, 1.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(110.f)
					[
						SNew(STextBlock).Text(Str(Option)).Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 11.f)).ColorAndOpacity(FSlateColor(ColorOf(Option)))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(44.f)[AutopilotUI::Mono(Str(Seconds >= 0 ? FString::Printf(TEXT("%.1f s"), Seconds) : FString(TEXT("\u2014"))), 11.f, FSlateColor(AutopilotColor::Text2()))]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(52.f)
					[
						AutopilotUI::Mono(Str(bKnown ? (bWorked ? TEXT("success") : TEXT("failure")) : TEXT("\u2014")), 11.f,
							FSlateColor(bKnown ? (bWorked ? AutopilotColor::Success() : AutopilotColor::Red()) : AutopilotColor::Muted()))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					AutopilotUI::Body(Str(How), 12.f, AutopilotColor::Text3())
				]
			];
		}
		Section(LOCTEXT("Recent", "Recent actions · outcomes"), Lines, nullptr);
	}

	// The action space: each action, its label, its facts and its probability; the chosen one #2a1b10 with the 2px accent
	// rule at its left (the others' rule transparent), its words in their colors, its probability in the accent
	if (const TSharedPtr<FJsonObject> Options = ObjectOf(State, TEXT("options")))
	{
		TSharedRef<SVerticalBox> Cards = SNew(SVerticalBox);
		for (const auto& Pair : Options->Values)
		{
			const FString Name(*Pair.Key);
			const FPlaytestPick* Pick = Call.Pick.FindByPredicate([&Name](const FPlaytestPick& P) { return P.Name == Name; });
			const bool bChosen = Name == Call.Chosen;
			const FString Label = LabelOf(Name);
			const FString Facts = Compact(Pair.Value);
			Cards->AddSlot().AutoHeight()
			[
				AutopilotUI::Picked(bChosen,
					SNew(SBox).Padding(FMargin(10.f, 7.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth()[AutopilotUI::Mono(Str(Name), 11.5f, FSlateColor(ColorOf(Name)), bChosen)]
								+ SHorizontalBox::Slot().AutoWidth()
								[
									AutopilotUI::Mono(Name == Call.NowDoing ? LOCTEXT("CurrentMark", " · current") : FText::GetEmpty(), 11.5f, FSlateColor(AutopilotColor::Muted()))
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f, 0.f, 0.f)
							[
								SNew(SBox).Visibility(Label.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
								[
									AutopilotUI::Body(Str(Label), 12.f, AutopilotColor::Text3())
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f, 0.f, 0.f)
							[
								SNew(SBox).Visibility(Facts.IsEmpty() || Facts == TEXT("null") ? EVisibility::Collapsed : EVisibility::Visible)
								[
									SNew(STextBlock).Text(Str(Facts)).Font(FAutopilotStyle::Font(EAutopilotFont::Mono, 10.f)).ColorAndOpacity(FSlateColor(AutopilotColor::Muted())).AutoWrapText(true)
								]
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(10.f, 0.f, 0.f, 0.f)
						[
							AutopilotUI::Mono(Str(Pick ? Prob(Pick->P) : FString(TEXT("\u2014"))), 14.f, FSlateColor(bChosen ? AutopilotColor::Accent() : AutopilotColor::Text3()), bChosen)
						]
					])
			];
		}
		Section(FText::Format(LOCTEXT("ActionSpace", "Action space · {0}"), Options->Values.Num()), Cards,
			PiLabel());
	}

	// Any other answers the policy gave
	if (const TSharedPtr<FJsonObject> Answers = ObjectOf(Output, TEXT("answers")))
	{
		TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
		int32 Count = 0;
		for (const auto& Pair : Answers->Values)
		{
			if (Pair.Key == TEXT("pick"))
			{
				continue;
			}
			++Count;
			Lines->AddSlot().AutoHeight().Padding(0.f, 1.f)[KeyValue(FString(*Pair.Key), Compact(Pair.Value), false)];
		}
		if (Count)
		{
			Section(LOCTEXT("OtherOutputs", "Other outputs"), Lines, nullptr);
		}
	}
}

#undef LOCTEXT_NAMESPACE
