// The Autopilot tab's look: Aura's "The Flatness" (dark only). Flat fills, 1px rules, one amber accent, no shadows, no
// gradients; 8px corners on plates (cards, the feed, the composer, wells, the recording, menus), square controls (buttons,
// tags, inputs, tabs, alerts, bars), a pill only for the switch. Its two fonts (IBM Plex Sans 400/500/600, IBM Plex Mono
// 400/500; JetBrains Mono for the geometric marks they lack), bundled under Resources/Fonts with their SIL Open Font
// Licenses, its brushes and widget styles, as one FSlateStyleSet. Sizes are the design's px: fonts are given in px and
// turned into Slate's points here (px x 0.75), layout sizes are Slate units (1 px at 100% scale).

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/ISlateStyle.h"

#define AUTOPILOT_COLOR(Name, Hex) \
	inline const FLinearColor& Name() { static const FLinearColor Color(FColor::FromHex(TEXT(Hex))); return Color; }

namespace AutopilotColor
{
	// Surfaces and lines
	AUTOPILOT_COLOR(Ground, "121316")     // the page; the history's odd rows
	AUTOPILOT_COLOR(Rail, "0f1013")       // the history rail; its even rows
	AUTOPILOT_COLOR(Plate, "181a1e")      // cards, the feed, findings
	AUTOPILOT_COLOR(Popover, "15171b")    // a floating menu (the narrow history list)
	AUTOPILOT_COLOR(Hover, "1d2026")      // hover, the pressed gear, the neutral tag
	AUTOPILOT_COLOR(RowActive, "2a1b10")  // the picked row (with a 2px accent rule at its left), the accent tag
	AUTOPILOT_COLOR(Well, "0d0e11")       // the prompt, inputs, the recording, evidence
	AUTOPILOT_COLOR(Rule, "24262b")       // dividers: under headers, round plates, above footers
	AUTOPILOT_COLOR(Line, "2c2f36")       // control outlines: buttons, inputs, the composer; the switch off
	AUTOPILOT_COLOR(LineHover, "3a3e47")  // a check box's outline, the switch hovered
	// Text
	AUTOPILOT_COLOR(Text, "e6e6e8")
	AUTOPILOT_COLOR(Text2, "d3d5da")      // reading text: reasons, prompts, evidence
	AUTOPILOT_COLOR(Text3, "a4a8b0")      // row text, feed lines, secondary lines
	AUTOPILOT_COLOR(Muted, "8b8f98")      // muted, micro-labels
	AUTOPILOT_COLOR(Neutral, "c9ccd2")    // the neutral tag's words
	// The accent
	AUTOPILOT_COLOR(Accent, "f0873a")
	AUTOPILOT_COLOR(AccentHover, "f59b57")
	AUTOPILOT_COLOR(AccentRest, "c4762f")  // (the send button at rest)
	AUTOPILOT_COLOR(AccentLine, "6b4122")  // the accent tag's outline
	AUTOPILOT_COLOR(OnAccent, "141517")    // ink on any fill
	AUTOPILOT_COLOR(Focus, "ffb066")       // the focus ring
	AUTOPILOT_COLOR(StepDone, "6b4122")    // a setup step done
	// Verdicts and states: the color, its words on its surface, its surface, its outline
	AUTOPILOT_COLOR(Success, "74b078")
	AUTOPILOT_COLOR(SuccessText, "b6ddb9")
	AUTOPILOT_COLOR(SuccessFill, "16281a")
	AUTOPILOT_COLOR(SuccessLine, "3d6b42")
	AUTOPILOT_COLOR(Red, "e26a62")
	AUTOPILOT_COLOR(RedText, "f0b2ac")
	AUTOPILOT_COLOR(RedFill, "2c1715")
	AUTOPILOT_COLOR(RedLine, "7a3b36")
	AUTOPILOT_COLOR(RedHover, "ea8079")
	AUTOPILOT_COLOR(Amber, "e8a94e")
	AUTOPILOT_COLOR(AmberText, "f6dcae")
	AUTOPILOT_COLOR(AmberFill, "2a2114")
	AUTOPILOT_COLOR(AmberLine, "7a5c2a")
	AUTOPILOT_COLOR(Info, "7aa5d8")
	AUTOPILOT_COLOR(InfoText, "bcd4ee")
	AUTOPILOT_COLOR(InfoFill, "161f2b")
	AUTOPILOT_COLOR(InfoLine, "3a5578")
	// The policy log's other action colors
	AUTOPILOT_COLOR(Violet, "a08ad8")
	AUTOPILOT_COLOR(Teal, "5fb3b3")
	AUTOPILOT_COLOR(Knob, "ffffff")        // the switch's knob
}

#undef AUTOPILOT_COLOR

/** A state's colors: a tag (words, surface, outline), an alert, a solid block (the verdict). */
enum class EAutopilotTone : uint8 { Success, Red, Amber, Info, Accent, Neutral };

struct FAutopilotTone
{
	FLinearColor Solid;  // the verdict block's fill, a dot
	FLinearColor Fore;   // a tag's words
	FLinearColor Fill;   // a tag's or an alert's surface
	FLinearColor Line;   // a tag's or an alert's outline
	FLinearColor Text;   // an alert's words
};

namespace AutopilotRadius
{
	constexpr float Plate = 8.f;  // plates: cards, the feed, the composer, wells, the recording, menus
}

enum class EAutopilotFont : uint8
{
	Body,        // IBM Plex Sans Regular (400)
	BodyMedium,  // IBM Plex Sans Medium (500): buttons, tabs, titles
	BodySemi,    // IBM Plex Sans SemiBold (600): setup titles, the verdict
	Mono,        // IBM Plex Mono Regular: numbers, times, ids, micro-labels
	MonoBold,    // IBM Plex Mono Medium: tags
};

class FAutopilotStyle
{
public:
	static void Initialize();
	static void Shutdown();
	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	/** A font of the theme at Px (the design's pixels: turned into Slate points), with letter spacing in thousandths of an
	 *  em (0.1em: 100). Falls back to the editor's font when the bundled one is missing. */
	static FSlateFontInfo Font(EAutopilotFont Face, float Px, int32 LetterSpacing = 0);
	/** The micro-label: IBM Plex Mono 10px, 0.1em (the text is shown upper case by the widget). */
	static FSlateFontInfo Label(float Px = 10.f) { return Font(EAutopilotFont::Mono, Px, 100); }

	static const FSlateBrush* Brush(const FName& Name);
	/** The activity feed's icon for a kind, by the name FAutopilotFeedLine::Icon gives it: the editor's own brush
	 *  (FAppStyle) where it has one of the meaning (a magnifying glass, a pencil, an eye, a circular arrow), the plugin's
	 *  SVG for the rest (a robot, binoculars, a flask, a gamepad, a speech bubble, a flag, a megaphone, a notebook); null for a name
	 *  no brush fits (the feed shows a letter then). White, to be tinted by the line's kind color, 12 px. */
	static const FSlateBrush* ActivityIcon(const FString& Icon);
	/** A box of Fill with a Width-px Outline (transparent: none) and corners of Radius (0: square, a control; 8: a plate). */
	static const FSlateBrush* Box(const FLinearColor& Fill, const FLinearColor& Outline, float Radius = 0.f, float Width = 1.f);

	/** A state's colors. */
	static const FAutopilotTone& Tone(EAutopilotTone Tone);

	/** A categorical color for the Index-th action of a run (accent, info, amber, success, violet, red, teal, light;
	 *  then the same again). */
	static FLinearColor ActionColor(int32 Index);
	/** The bundled fonts were found (else the editor's font is used, and the tab says so once). */
	static bool FontsFound();
};
