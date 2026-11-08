#include "AutopilotStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/CompositeFont.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"

namespace
{
	TSharedPtr<FSlateStyleSet> StyleSet;
	FString FontDir;
	bool bFontsFound = false;
	TMap<uint8, TSharedPtr<const FCompositeFont>> Fonts;  // one per face, loaded when first drawn
	const TCHAR* SymbolsFile = TEXT("JetBrainsMono-Regular.ttf");  // (only for the geometric marks the two faces lack)

	/** Slate's points for the design's px (Slate draws fonts at 96 dpi: 1 pt = 4/3 px). */
	constexpr float PointsPerPx = 0.75f;

	const TCHAR* FontFile(EAutopilotFont Face)
	{
		switch (Face)
		{
		case EAutopilotFont::Body: return TEXT("IBMPlexSans-Regular.ttf");
		case EAutopilotFont::BodyMedium: return TEXT("IBMPlexSans-Medium.ttf");
		case EAutopilotFont::BodySemi: return TEXT("IBMPlexSans-SemiBold.ttf");
		case EAutopilotFont::Mono: return TEXT("IBMPlexMono-Regular.ttf");
		case EAutopilotFont::MonoBold: return TEXT("IBMPlexMono-Medium.ttf");
		}
		return TEXT("IBMPlexSans-Regular.ttf");
	}

	FSlateFontInfo FallbackFont(EAutopilotFont Face, float Size)
	{
		switch (Face)
		{
		case EAutopilotFont::BodyMedium:
		case EAutopilotFont::BodySemi:
		case EAutopilotFont::MonoBold:
			return FCoreStyle::GetDefaultFontStyle("Bold", Size);
		case EAutopilotFont::Mono:
			return FCoreStyle::GetDefaultFontStyle("Mono", Size);
		default:
			return FCoreStyle::GetDefaultFontStyle("Regular", Size);
		}
	}

	/** A color at Alpha of its own (a control disabled: 0.45). */
	FLinearColor Faded(const FLinearColor& Color, float Alpha = 0.45f)
	{
		return FLinearColor(Color.R, Color.G, Color.B, Color.A * Alpha);
	}

	/** A square box: Fill, with a 1px Outline (transparent: none). */
	FSlateRoundedBoxBrush Square(const FLinearColor& Fill, const FLinearColor& Outline, float Width = 1.f)
	{
		return FSlateRoundedBoxBrush(Fill, 0.f, Outline, Width);
	}

	/** A square button (every control is square): its fill and outline at rest and hovered, its words' colors; disabled,
	 *  the rest look at 45%. */
	FButtonStyle SquareButton(const FLinearColor& Fill, const FLinearColor& Outline, const FLinearColor& HoverFill, const FLinearColor& HoverOutline,
		const FLinearColor& Text, const FLinearColor& HoverText)
	{
		FButtonStyle Style;
		Style.SetNormal(Square(Fill, Outline))
			.SetHovered(Square(HoverFill, HoverOutline))
			.SetPressed(Square(HoverFill, HoverOutline))
			.SetDisabled(Square(Faded(Fill), Faded(Outline)))
			.SetNormalForeground(FSlateColor(Text))
			.SetHoveredForeground(FSlateColor(HoverText))
			.SetPressedForeground(FSlateColor(HoverText))
			.SetDisabledForeground(FSlateColor(Faded(Text)))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));
		return Style;
	}

	FCheckBoxStyle Toggle(const FSlateBrush& Off, const FSlateBrush& OffHover, const FSlateBrush& On, const FSlateBrush& OnHover,
		const FLinearColor& OffText, const FLinearColor& HoverText, const FLinearColor& OnText, const FMargin& Padding)
	{
		FCheckBoxStyle Style;
		Style.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
			.SetUncheckedImage(Off)
			.SetUncheckedHoveredImage(OffHover)
			.SetUncheckedPressedImage(OffHover)
			.SetCheckedImage(On)
			.SetCheckedHoveredImage(OnHover)
			.SetCheckedPressedImage(OnHover)
			.SetUndeterminedImage(Off)
			.SetUndeterminedHoveredImage(OffHover)
			.SetUndeterminedPressedImage(OffHover)
			.SetForegroundColor(FSlateColor(OffText))
			.SetHoveredForegroundColor(FSlateColor(HoverText))
			.SetPressedForegroundColor(FSlateColor(HoverText))
			.SetCheckedForegroundColor(FSlateColor(OnText))
			.SetCheckedHoveredForegroundColor(FSlateColor(OnText))
			.SetCheckedPressedForegroundColor(FSlateColor(OnText))
			.SetPadding(Padding);
		return Style;
	}

	/** Brushes made for a fill, an outline and a radius (AutopilotUI's plates, tags, alerts): kept until shutdown. */
	TMap<FString, TSharedPtr<FSlateBrush>> Boxes;
}

void FAutopilotStyle::Initialize()
{
	if (StyleSet.IsValid())
	{
		return;
	}
	StyleSet = MakeShared<FSlateStyleSet>(TEXT("AutopilotStyle"));
	FString Root;
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Autopilot")))
	{
		Root = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"));
	}
	StyleSet->SetContentRoot(Root);
	FontDir = FPaths::Combine(Root, TEXT("Fonts"));
	bFontsFound = FPaths::FileExists(FPaths::Combine(FontDir, FontFile(EAutopilotFont::Body)))
		&& FPaths::FileExists(FPaths::Combine(FontDir, FontFile(EAutopilotFont::BodyMedium)))
		&& FPaths::FileExists(FPaths::Combine(FontDir, FontFile(EAutopilotFont::Mono)));

	FSlateStyleSet& S = *StyleSet;
	const FString Icons = FPaths::Combine(Root, TEXT("Icons"));
	auto Icon = [&Icons](const TCHAR* File, float Px) { return new FSlateVectorImageBrush(FPaths::Combine(Icons, File), FVector2f(Px, Px)); };

	// The logo: a game controller (outline, d-pad, two buttons), in the accent
	S.Set("Autopilot.Icon", Icon(TEXT("Autopilot.svg"), 16.f));
	S.Set("Autopilot.Icon.Small", Icon(TEXT("Autopilot.svg"), 14.f));     // (the history's header)
	S.Set("Autopilot.Icon.Toolbar", Icon(TEXT("Autopilot.svg"), 20.f));
	S.Set("Autopilot.Icon.Large", Icon(TEXT("Autopilot.svg"), 20.f));     // (the setup's and the removal's titles)
	S.Set("Autopilot.Check", Icon(TEXT("Check.svg"), 18.f));              // (the agent service ready)
	S.Set("Autopilot.Chevron", Icon(TEXT("Chevron.svg"), 10.f));          // (the narrow history button)
	S.Set("Autopilot.Screen", Icon(TEXT("Screen.svg"), 30.f));            // (the recording before it plays)
	S.Set("Autopilot.Warning", Icon(TEXT("Warning.svg"), 30.f));          // (the recording that can't play here, or is gone)
	// Icons drawn white, tinted by the button they are on (its foreground): Settings, the recording's Play and Pause, a
	// run's Run again (a circular arrow), Copy report (a clipboard) and the folder (Run folder, Show in File Explorer)
	S.Set("Autopilot.Gear", Icon(TEXT("Gear.svg"), 14.f));
	S.Set("Autopilot.Play", Icon(TEXT("Play.svg"), 12.f));
	S.Set("Autopilot.Pause", Icon(TEXT("Pause.svg"), 12.f));
	S.Set("Autopilot.Folder", Icon(TEXT("Folder.svg"), 14.f));
	S.Set("Autopilot.Back", Icon(TEXT("Back.svg"), 12.f));         // (the policy log's previous step)
	S.Set("Autopilot.Forward", Icon(TEXT("Forward.svg"), 12.f));   // (and next)
	S.Set("Autopilot.Retry", Icon(TEXT("Retry.svg"), 14.f));
	S.Set("Autopilot.Clipboard", Icon(TEXT("Clipboard.svg"), 14.f));
	// The activity feed's kind icons the editor has none of the meaning for (ActivityIcon; white, 12 px, tinted by the
	// line's kind color): a robot (the agent's words), binoculars (searched), a flask (tried in the game), a gamepad
	// (played the harness), a speech bubble (the user steered), a flag (the report), a megaphone (an alert), a notebook
	// (the refiner edited the game's memory)
	S.Set("Autopilot.Feed.Robot", Icon(TEXT("Robot.svg"), 12.f));
	S.Set("Autopilot.Feed.Binoculars", Icon(TEXT("Binoculars.svg"), 12.f));
	S.Set("Autopilot.Feed.Flask", Icon(TEXT("Flask.svg"), 12.f));
	S.Set("Autopilot.Feed.Gamepad", Icon(TEXT("Gamepad.svg"), 12.f));
	S.Set("Autopilot.Feed.Bubble", Icon(TEXT("Bubble.svg"), 12.f));
	S.Set("Autopilot.Feed.Flag", Icon(TEXT("Flag.svg"), 12.f));
	S.Set("Autopilot.Feed.Megaphone", Icon(TEXT("Megaphone.svg"), 12.f));
	S.Set("Autopilot.Feed.Notebook", Icon(TEXT("Notebook.svg"), 12.f));

	// Fills (tinted by the widget: SBorder's BorderBackgroundColor, SImage's ColorAndOpacity)
	S.Set("Autopilot.White", new FSlateColorBrush(FLinearColor::White));
	S.Set("Autopilot.Round", new FSlateRoundedBoxBrush(FLinearColor::White, 6.f));  // (the switch's knob: a circle)
	S.Set("Autopilot.Pill", new FSlateRoundedBoxBrush(FLinearColor::White, 8.f));   // (the switch's track)

	// The feed's activity lines (SRichTextBlock): the words in the text's color, the result after them quieter
	S.Set("Autopilot.Feed.Text", FTextBlockStyle().SetFont(Font(EAutopilotFont::Mono, 11.5f)).SetColorAndOpacity(FSlateColor(AutopilotColor::Text())));
	S.Set("Autopilot.Feed.Quiet", FTextBlockStyle().SetFont(Font(EAutopilotFont::Mono, 11.5f)).SetColorAndOpacity(FSlateColor(AutopilotColor::Muted())));

	// Buttons, all square: primary (the accent), default (outlined), quiet, danger (red words), danger filled, and on (the
	// gear while the Settings page is open)
	const FLinearColor None = FLinearColor::Transparent;
	S.Set("Autopilot.Button.Primary", SquareButton(AutopilotColor::Accent(), AutopilotColor::Accent(), AutopilotColor::AccentHover(), AutopilotColor::AccentHover(),
		AutopilotColor::OnAccent(), AutopilotColor::OnAccent()));
	S.Set("Autopilot.Button", SquareButton(None, AutopilotColor::Line(), AutopilotColor::Hover(), AutopilotColor::Line(),
		AutopilotColor::Text(), AutopilotColor::Text()));
	S.Set("Autopilot.Button.Quiet", SquareButton(None, None, AutopilotColor::Hover(), AutopilotColor::Hover(),
		AutopilotColor::Muted(), AutopilotColor::Text()));
	S.Set("Autopilot.Button.Danger", SquareButton(None, None, AutopilotColor::RedFill(), AutopilotColor::RedFill(),
		AutopilotColor::Red(), AutopilotColor::Red()));
	S.Set("Autopilot.Button.DangerFill", SquareButton(AutopilotColor::Red(), AutopilotColor::Red(), AutopilotColor::RedHover(), AutopilotColor::RedHover(),
		AutopilotColor::OnAccent(), AutopilotColor::OnAccent()));
	S.Set("Autopilot.Button.On", SquareButton(AutopilotColor::Hover(), AutopilotColor::Line(), AutopilotColor::Hover(), AutopilotColor::Line(),
		AutopilotColor::Accent(), AutopilotColor::Accent()));
	// (the narrow history's button: the rail's fill, the control outline)
	S.Set("Autopilot.Button.History", SquareButton(AutopilotColor::Rail(), AutopilotColor::Line(), AutopilotColor::Hover(), AutopilotColor::Line(),
		AutopilotColor::Text(), AutopilotColor::Text()));
	// (words as a button: the connect card's Advanced; nothing drawn round them)
	S.Set("Autopilot.Button.Bare", SquareButton(None, None, None, None, AutopilotColor::Muted(), AutopilotColor::Text()));

	// Toggles: chips (the policy log's filters: on, the accent tag's colors) and the panel's tabs (their 2px underline
	// is drawn by the tab: AutopilotUI::Tab)
	S.Set("Autopilot.Chip", Toggle(Square(None, AutopilotColor::Line()), Square(AutopilotColor::Hover(), AutopilotColor::Line()),
		Square(AutopilotColor::RowActive(), AutopilotColor::AccentLine()), Square(AutopilotColor::RowActive(), AutopilotColor::AccentLine()),
		AutopilotColor::Muted(), AutopilotColor::Text(), AutopilotColor::Accent(), FMargin(9.f, 0.f)));
	S.Set("Autopilot.PanelTab", Toggle(FSlateNoResource(), FSlateNoResource(), FSlateNoResource(), FSlateNoResource(),
		AutopilotColor::Muted(), AutopilotColor::Text(), AutopilotColor::Text(), FMargin(0.f)));
	// (a check box or switch drawn by AutopilotUI: the whole row clickable, nothing drawn round it)
	S.Set("Autopilot.Bare", Toggle(FSlateNoResource(), FSlateNoResource(), FSlateNoResource(), FSlateNoResource(),
		AutopilotColor::Text(), AutopilotColor::Text(), AutopilotColor::Text(), FMargin(0.f)));

	// Text fields: a square well with the control outline, the focus ring when focused (13px Plex Sans, 28 tall)
	{
		FEditableTextBoxStyle Box = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
		Box.SetBackgroundImageNormal(Square(AutopilotColor::Well(), AutopilotColor::Line()))
			.SetBackgroundImageHovered(Square(AutopilotColor::Well(), AutopilotColor::Line()))
			.SetBackgroundImageFocused(Square(AutopilotColor::Well(), AutopilotColor::Focus(), 2.f))
			.SetBackgroundImageReadOnly(Square(AutopilotColor::Well(), AutopilotColor::Line()))
			.SetFont(Font(EAutopilotFont::Body, 13.f))
			.SetForegroundColor(FSlateColor(AutopilotColor::Text()))
			.SetFocusedForegroundColor(FSlateColor(AutopilotColor::Text()))
			.SetReadOnlyForegroundColor(FSlateColor(AutopilotColor::Text2()))
			.SetBackgroundColor(FSlateColor(FLinearColor::White))
			.SetPadding(FMargin(9.f, 5.f));
		S.Set("Autopilot.TextBox", Box);
		FEditableTextBoxStyle Mono = Box;
		Mono.SetFont(Font(EAutopilotFont::Mono, 12.f)).SetPadding(FMargin(9.f, 6.f));
		S.Set("Autopilot.TextBox.Mono", Mono);
		// (inside a plate that is the box: the composer's prompt, the steer box)
		FEditableTextBoxStyle Bare = Box;
		Bare.SetBackgroundImageNormal(FSlateNoResource())
			.SetBackgroundImageHovered(FSlateNoResource())
			.SetBackgroundImageFocused(FSlateNoResource())
			.SetBackgroundImageReadOnly(FSlateNoResource())
			.SetPadding(FMargin(0.f));
		S.Set("Autopilot.TextBox.Bare", Bare);
		// (the policy log's search: 24 tall, mono 11)
		FEditableTextBoxStyle Small = Mono;
		Small.SetFont(Font(EAutopilotFont::Mono, 11.f)).SetPadding(FMargin(9.f, 3.f));
		S.Set("Autopilot.TextBox.Small", Small);
		// (a step's raw JSON: a code box, the well with the rule round it, 8px corners)
		FEditableTextBoxStyle Code = Mono;
		const FSlateRoundedBoxBrush CodeBox(AutopilotColor::Well(), AutopilotRadius::Plate, AutopilotColor::Rule(), 1.f);
		Code.SetBackgroundImageNormal(CodeBox).SetBackgroundImageHovered(CodeBox).SetBackgroundImageFocused(CodeBox).SetBackgroundImageReadOnly(CodeBox)
			.SetFont(Font(EAutopilotFont::Mono, 11.f))
			.SetReadOnlyForegroundColor(FSlateColor(AutopilotColor::Text2()))
			.SetPadding(FMargin(11.f, 9.f));
		S.Set("Autopilot.TextBox.Code", Code);
	}
	// Number fields (Duration, Minutes, Time limit): a mono well, its number at the right, no arrows
	{
		FSpinBoxStyle Spin = FAppStyle::Get().GetWidgetStyle<FSpinBoxStyle>("SpinBox");
		Spin.SetBackgroundBrush(Square(AutopilotColor::Well(), AutopilotColor::Line()))
			.SetHoveredBackgroundBrush(Square(AutopilotColor::Well(), AutopilotColor::Line()))
			.SetActiveFillBrush(FSlateNoResource())
			.SetInactiveFillBrush(FSlateNoResource())
			.SetArrowsImage(FSlateNoResource())
			.SetForegroundColor(FSlateColor(AutopilotColor::Text()))
			.SetTextPadding(FMargin(9.f, 0.f));
		S.Set("Autopilot.SpinBox", Spin);
	}
	// Dropdowns (the setup's Coding Agent and LLM provider): a field; their list a menu (the popover, its outline)
	{
		FComboBoxStyle Combo = FAppStyle::Get().GetWidgetStyle<FComboBoxStyle>("ComboBox");
		FComboButtonStyle ComboButton = Combo.ComboButtonStyle;
		FButtonStyle Field = SquareButton(AutopilotColor::Well(), AutopilotColor::Line(), AutopilotColor::Well(), AutopilotColor::LineHover(),
			AutopilotColor::Text(), AutopilotColor::Text());
		ComboButton.SetButtonStyle(Field)
			.SetMenuBorderBrush(FSlateRoundedBoxBrush(AutopilotColor::Popover(), AutopilotRadius::Plate, AutopilotColor::Line(), 1.f))
			.SetMenuBorderPadding(FMargin(1.f))
			.SetContentPadding(FMargin(9.f, 5.f));
		Combo.SetComboButtonStyle(ComboButton).SetContentPadding(FMargin(9.f, 5.f));
		S.Set("Autopilot.ComboBox", Combo);
	}

	// List rows: no stripes (the history draws its own), a neutral hover, the picked one RowActive (the row draws its
	// 2px accent rule itself: AutopilotUI::Picked); its words keep their color
	{
		FTableRowStyle Row = FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
		const FSlateColorBrush Selected(AutopilotColor::RowActive());
		const FSlateColorBrush Hovered(AutopilotColor::Hover());
		Row.SetEvenRowBackgroundBrush(FSlateNoResource())
			.SetOddRowBackgroundBrush(FSlateNoResource())
			.SetEvenRowBackgroundHoveredBrush(Hovered)
			.SetOddRowBackgroundHoveredBrush(Hovered)
			.SetActiveBrush(Selected)
			.SetActiveHoveredBrush(Selected)
			.SetInactiveBrush(Selected)
			.SetInactiveHoveredBrush(Selected)
			.SetActiveHighlightedBrush(Selected)
			.SetInactiveHighlightedBrush(Selected)
			.SetSelectorFocusedBrush(FSlateNoResource())
			.SetTextColor(FSlateColor(AutopilotColor::Text2()))
			.SetSelectedTextColor(FSlateColor(AutopilotColor::Text()));
		S.Set("Autopilot.Row", Row);
		// (the feed's lines: nothing at all on hover or selection)
		FTableRowStyle Plain = Row;
		Plain.SetEvenRowBackgroundHoveredBrush(FSlateNoResource()).SetOddRowBackgroundHoveredBrush(FSlateNoResource());
		S.Set("Autopilot.Row.Plain", Plain);
		// (the policy log's steps: a hover, the picked one drawn by the row)
		FTableRowStyle HoverOnly = Plain;
		const FSlateNoResource NoBrush;
		HoverOnly.SetEvenRowBackgroundHoveredBrush(Hovered).SetOddRowBackgroundHoveredBrush(Hovered)
			.SetActiveBrush(NoBrush).SetActiveHoveredBrush(Hovered).SetInactiveBrush(NoBrush).SetInactiveHoveredBrush(Hovered)
			.SetActiveHighlightedBrush(NoBrush).SetInactiveHighlightedBrush(NoBrush);
		S.Set("Autopilot.Row.Hover", HoverOnly);
	}

	FSlateStyleRegistry::RegisterSlateStyle(S);
}

void FAutopilotStyle::Shutdown()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
		Fonts.Reset();
		Boxes.Reset();
	}
}

const ISlateStyle& FAutopilotStyle::Get()
{
	check(StyleSet.IsValid());
	return *StyleSet;
}

FName FAutopilotStyle::GetStyleSetName()
{
	return TEXT("AutopilotStyle");
}

FSlateFontInfo FAutopilotStyle::Font(EAutopilotFont Face, float Px, int32 LetterSpacing)
{
	const float Size = Px * PointsPerPx;
	if (!bFontsFound)
	{
		FSlateFontInfo Fallback = FallbackFont(Face, Size);
		Fallback.LetterSpacing = LetterSpacing;
		return Fallback;
	}
	TSharedPtr<const FCompositeFont>& Composite = Fonts.FindOrAdd((uint8)Face);
	if (!Composite.IsValid())
	{
		TSharedRef<FStandaloneCompositeFont> Made = MakeShared<FStandaloneCompositeFont>(FName(FontFile(Face)), FPaths::Combine(FontDir, FontFile(Face)),
			EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
		// The marks the tab draws that IBM Plex Sans and Mono don't have (● ◀ ▶ ◂ ▸ ▾): from JetBrains Mono
		const FString Symbols = FPaths::Combine(FontDir, SymbolsFile);
		if (FPaths::FileExists(Symbols))
		{
			FCompositeSubFont& Sub = Made->SubTypefaces.AddDefaulted_GetRef();
			Sub.CharacterRanges.Add(FInt32Range(0x25A0, 0x2600));  // Geometric Shapes
			Sub.Typeface.AppendFont(TEXT("Symbols"), Symbols, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
		}
		Composite = Made;
	}
	FSlateFontInfo Info(Composite, Size);
	Info.LetterSpacing = LetterSpacing;
	return Info;
}

bool FAutopilotStyle::FontsFound()
{
	return bFontsFound;
}

const FSlateBrush* FAutopilotStyle::Brush(const FName& Name)
{
	return StyleSet.IsValid() ? StyleSet->GetBrush(Name) : FAppStyle::GetNoBrush();
}

const FSlateBrush* FAutopilotStyle::ActivityIcon(const FString& Icon)
{
	// The editor's brushes where one means the same (UE 5.8's Starship core style: a magnifying glass, a pencil, an eye,
	// a circular arrow); the plugin's SVGs (above) for the rest. An editor brush missing in some other engine: null,
	// and the feed shows the kind's letter
	static const TMap<FString, FName> Editor = {
		{ TEXT("magnifier"), FName("Icons.Search") }, { TEXT("pencil"), FName("Icons.Edit") },
		{ TEXT("eye"), FName("Icons.Visible") }, { TEXT("circular-arrow"), FName("Icons.Refresh") },
	};
	static const TMap<FString, FName> Own = {
		{ TEXT("robot"), FName("Autopilot.Feed.Robot") }, { TEXT("binoculars"), FName("Autopilot.Feed.Binoculars") },
		{ TEXT("flask"), FName("Autopilot.Feed.Flask") }, { TEXT("gamepad"), FName("Autopilot.Feed.Gamepad") },
		{ TEXT("speech-bubble"), FName("Autopilot.Feed.Bubble") }, { TEXT("flag"), FName("Autopilot.Feed.Flag") },
		{ TEXT("megaphone"), FName("Autopilot.Feed.Megaphone") }, { TEXT("notebook"), FName("Autopilot.Feed.Notebook") },
	};
	if (const FName* Name = Editor.Find(Icon))
	{
		return FAppStyle::Get().GetOptionalBrush(*Name, nullptr, nullptr);
	}
	if (const FName* Name = Own.Find(Icon))
	{
		return StyleSet.IsValid() ? StyleSet->GetOptionalBrush(*Name, nullptr, nullptr) : nullptr;
	}
	return nullptr;
}

const FSlateBrush* FAutopilotStyle::Box(const FLinearColor& Fill, const FLinearColor& Outline, float Radius, float Width)
{
	const FString Key = Fill.ToString() + Outline.ToString() + FString::Printf(TEXT("%.1f|%.1f"), Radius, Width);
	TSharedPtr<FSlateBrush>& Made = Boxes.FindOrAdd(Key);
	if (!Made.IsValid())
	{
		Made = MakeShared<FSlateRoundedBoxBrush>(Fill, Radius, Outline, Outline.A > 0.f ? Width : 0.f);
	}
	return Made.Get();
}

const FAutopilotTone& FAutopilotStyle::Tone(EAutopilotTone Which)
{
	static const FAutopilotTone Tones[] = {
		{ AutopilotColor::Success(), AutopilotColor::Success(), AutopilotColor::SuccessFill(), AutopilotColor::SuccessLine(), AutopilotColor::SuccessText() },
		{ AutopilotColor::Red(), AutopilotColor::Red(), AutopilotColor::RedFill(), AutopilotColor::RedLine(), AutopilotColor::RedText() },
		{ AutopilotColor::Amber(), AutopilotColor::Amber(), AutopilotColor::AmberFill(), AutopilotColor::AmberLine(), AutopilotColor::AmberText() },
		{ AutopilotColor::Info(), AutopilotColor::Info(), AutopilotColor::InfoFill(), AutopilotColor::InfoLine(), AutopilotColor::InfoText() },
		{ AutopilotColor::Accent(), AutopilotColor::Accent(), AutopilotColor::RowActive(), AutopilotColor::AccentLine(), AutopilotColor::Accent() },
		{ AutopilotColor::Muted(), AutopilotColor::Neutral(), AutopilotColor::Hover(), AutopilotColor::Line(), AutopilotColor::Neutral() },
	};
	return Tones[FMath::Clamp((int32)Which, 0, (int32)UE_ARRAY_COUNT(Tones) - 1)];
}

FLinearColor FAutopilotStyle::ActionColor(int32 Index)
{
	static const FLinearColor Palette[] = {
		AutopilotColor::Accent(), AutopilotColor::Info(), AutopilotColor::Amber(), AutopilotColor::Success(), AutopilotColor::Violet(),
		AutopilotColor::Red(), AutopilotColor::Teal(), AutopilotColor::Neutral(),
	};
	constexpr int32 Count = UE_ARRAY_COUNT(Palette);
	return Palette[FMath::Max(0, Index) % Count];  // (the eight again, in order)
}
