// The Autopilot tab laid out at several sizes, without drawing it (AutopilotPlaytest.Editor.Layout): every page is laid out
// as Slate would, and no widget may be placed past the tab's left or right edge (a row that doesn't wrap or stack,
// a fixed width too wide), no stage name is given less room than it needs, and the composer's hint fits its box. Pages
// taller than the tab are fine: they scroll. Also: the kind of run the composer's words ask for (Editor.RunKind), and the
// end of a run: the memory line, the recording kept and copied, or why there is none (Editor.RunEnd); steering only while the game plays, its
// Rollout (Editor.Steer, and the Live page's steer row in Editor.Layout).

#include "Settings/EditorExperimentalSettings.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Layout/ArrangedChildren.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PlaytestAccount.h"
#include "PlaytestAura.h"
#include "PlaytestJson.h"
#include "PlaytestPreferences.h"
#include "PlaytestSettings.h"
#include "AutopilotSession.h"
#include "AutopilotStyle.h"
#include "AutopilotWidgets.h"
#include "Rendering/SlateRenderer.h"
#include "SPlaytestPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FTabSize
	{
		float Width;
		float Height;
	};
	int32 Walked = 0;  // widgets laid out, all pages (said at the end)
	const FTabSize TabSizes[] = { { 320.f, 900.f }, { 360.f, 640.f }, { 560.f, 480.f }, { 900.f, 400.f }, { 1400.f, 900.f } };

	/** Every widget laid out under Widget (in Geometry) that sits past Left..Right: "path: from..to". */
	void Walk(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, float Left, float Right, const FString& Path, TArray<FString>& Out)
	{
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children, true);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			const FArrangedWidget& Child = Children[i];
			++Walked;
			const FVector2D Position = FVector2D(Child.Geometry.GetAbsolutePosition());
			const FVector2D Size = FVector2D(Child.Geometry.GetAbsoluteSize());
			const FString Here = Path + TEXT("/") + Child.Widget->GetTypeAsString();
			if (Position.X < Left - 1.0 || Position.X + Size.X > Right + 1.0)
			{
				Out.Add(FString::Printf(TEXT("%s: x %.0f..%.0f, the tab 0..%.0f (wants %.0f wide)"), *Here, Position.X, Position.X + Size.X, Right,
					Child.Widget->GetDesiredSize().X));
				continue;  // (what is inside it is past too)
			}
			Walk(Child.Widget, Child.Geometry, Left, Right, Here, Out);
		}
	}

	/** Whether any widget under Widget (Widget too) is a button. */
	bool HasButton(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == FName(TEXT("SButton")))
		{
			return true;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (HasButton(Children->GetChildAt(i)))
			{
				return true;
			}
		}
		return false;
	}

	/** Where Target is laid out under Widget (in Geometry), into Out; false when it isn't shown. */
	bool Find(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, const SWidget* Target, FGeometry& Out)
	{
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children, true);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			const FArrangedWidget& Child = Children[i];
			if (&Child.Widget.Get() == Target)
			{
				Out = Child.Geometry;
				return true;
			}
			if (Find(Child.Widget, Child.Geometry, Target, Out))
			{
				return true;
			}
		}
		return false;
	}

	/** Where a text saying What is laid out inside Within (under Widget, in Geometry), into Out and OutText; false when none. */
	bool FindText(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, const FString& What, const SWidget* Within, FGeometry& Out,
		TSharedPtr<SWidget>& OutText, bool bInside = false)
	{
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children, true);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			const FArrangedWidget& Child = Children[i];
			const bool bHere = bInside || &Child.Widget.Get() == Within;
			if (bHere && Child.Widget->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(Child.Widget)->GetText().ToString() == What)
			{
				Out = Child.Geometry;
				OutText = Child.Widget;
				return true;
			}
			if (FindText(Child.Widget, Child.Geometry, What, Within, Out, OutText, bHere))
			{
				return true;
			}
		}
		return false;
	}

	/** Where the button named Tip (its tooltip and accessible text: AutopilotUI::IconButton) is laid out under Widget, in
	 *  Geometry, into Out; false when none. */
	bool FindNamedButton(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, const FString& Tip, FGeometry& Out)
	{
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children, true);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			const FArrangedWidget& Child = Children[i];
			if (Child.Widget->GetType() == FName(TEXT("SButton")) && Child.Widget->GetAccessibleText().ToString() == Tip)
			{
				Out = Child.Geometry;
				return true;
			}
			if (FindNamedButton(Child.Widget, Child.Geometry, Tip, Out))
			{
				return true;
			}
		}
		return false;
	}

	/** A folder of its own under Saved/Automation (autopilot.env for the panel: never the user's), removed when done. */
	struct FScratchDir
	{
		FString Path;
		FScratchDir()
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Autopilot"),
				FGuid::NewGuid().ToString()));
			IFileManager::Get().MakeDirectory(*Path, true);
		}
		~FScratchDir()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};

	/** The words of every text laid out (and shown) under Widget, in Geometry. */
	void Texts(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, TArray<FString>& Out)
	{
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children, true);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			const FArrangedWidget& Child = Children[i];
			if (Child.Widget->GetType() == FName(TEXT("STextBlock")))
			{
				Out.Add(StaticCastSharedRef<STextBlock>(Child.Widget)->GetText().ToString());
			}
			Texts(Child.Widget, Child.Geometry, Out);
		}
	}

	/** The words Widget shows at Width x Height (laid out as LayOut does), one line. */
	FString Shown(const TSharedRef<SWidget>& Widget, float Width, float Height)
	{
		const FGeometry Root = FGeometry::MakeRoot(FVector2D(Width, Height), FSlateLayoutTransform());
		Widget->SlatePrepass(1.f);
		Widget->SlatePrepass(1.f);
		TArray<FString> Out;
		Texts(Widget, Root, Out);
		return FString::Join(Out, TEXT(" | "));
	}

	/** How many times Words says What. */
	int32 Count(const FString& Words, const TCHAR* What)
	{
		int32 N = 0;
		for (int32 At = Words.Find(What); At != INDEX_NONE; At = Words.Find(What, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			++N;
		}
		return N;
	}

	/** Widget laid out at Width x Height (twice: a page builds some of itself on its first tick). */
	TArray<FString> LayOut(const TSharedRef<SWidget>& Widget, float Width, float Height, const TFunction<void(const FGeometry&)>& Tick = nullptr)
	{
		const FGeometry Root = FGeometry::MakeRoot(FVector2D(Width, Height), FSlateLayoutTransform());
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			if (Tick)
			{
				Tick(Root);
			}
			Widget->SlatePrepass(1.f);
		}
		TArray<FString> Out;
		Walk(Widget, Root, 0.f, Width, Widget->GetTypeAsString(), Out);
		return Out;
	}

	/** A finished run with long words everywhere. */
	TSharedPtr<FAutopilotRun> LongRun()
	{
		TSharedPtr<FAutopilotRun> Run = MakeShared<FAutopilotRun>();
		Run->Id = 987654;
		Run->Kind = TEXT("verify");
		Run->Prompt = TEXT("Verify that opening the heavy iron door on the second floor of the abandoned lighthouse lets the player into the lantern room without a key");
		Run->Started = FDateTime::Now();
		Run->bDone = true;
		Run->Verdict = TEXT("INCONCLUSIVE");
		Run->Reason = TEXT("The agent reached the door three times but the interaction prompt never appeared, so it could not tell whether the door opens without the key or not at all.");
		Run->Evidence = { TEXT("Walked to BP_IronDoor_C_2 at (1520, -340, 412) and pressed Interact (E) five times: no prompt, no state change in bIsOpen"),
			TEXT("A_very_long_identifier_without_any_spaces_that_cannot_wrap_anywhere_BP_LighthouseLanternRoomDoor_C_7.bIsLockedForever") };
		Run->AgentNotes = { TEXT("The stairs to the second floor clip through the wall near the window; the player can stand inside the wall for a second.") };
		Run->Changes = { TEXT("Set BP_Player_C_0.Inventory.Keys to 0 before the attempt") };
		TSharedPtr<FAutopilotFinding> Engine = MakeShared<FAutopilotFinding>();
		Engine->Severity = TEXT("error");
		Engine->Summary = TEXT("Accessed None trying to read property DoorMesh in BP_IronDoor_C::OnInteract");
		Engine->Detector = TEXT("log_error");
		Engine->Actor = TEXT("BP_IronDoor_C_2");
		Engine->Count = 14;
		Engine->bHasWhere = true;
		Engine->Where = FVector(1520.0, -340.0, 412.0);
		Engine->GameTime = 83.4;
		Run->Findings.Add(Engine);
		FPlaytestModelUsage Llm;
		Llm.Kind = TEXT("llm");
		Llm.Model = TEXT("claude-opus-5-5");
		Llm.Calls = 120;
		Llm.Input = 1234567;
		Llm.Output = 45678;
		Llm.CacheRead = 9876543;
		Llm.CacheWrite = 123456;
		FPlaytestModelUsage Decision;
		Decision.Kind = TEXT("decision");
		Decision.Model = TEXT("decision-model-with-a-long-name-2026-10");
		Decision.Calls = 23100;
		Decision.Input = 145000000;
		Decision.Output = 462000;
		Run->Usage = { Llm, Decision };
		// Billed through Aura: a long sum
		Run->Bill.bKnown = true;
		Run->Bill.bBilled = true;
		Run->Bill.Usd = 1234567.891;
		return Run;
	}

	/** A run through BYOK (no bill), or through Aura billed below a cent; Id set. */
	TSharedPtr<FAutopilotRun> OtherRun(int32 Id, bool bBilled)
	{
		TSharedPtr<FAutopilotRun> Run = LongRun();
		Run->Id = Id;
		Run->Kind = TEXT("goal");
		Run->Bill = FPlaytestRunBill();
		if (bBilled)
		{
			Run->Bill.bKnown = true;
			Run->Bill.bBilled = true;
			Run->Bill.Usd = 0.003;
		}
		return Run;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotPanelLayoutTest, "AutopilotPlaytest.Editor.Layout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotPanelLayoutTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
	{
		AddWarning(TEXT("No Slate renderer here (text can't be measured): the layout test is skipped"));
		return true;
	}
	// The check itself: a fixed width too wide, and words that don't wrap, are both found (text is measured here)
	TestEqual(TEXT("self-check: a 500 px box in 320 px"), LayOut(SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(500.f)], 320.f, 200.f).Num(), 1);
	TestEqual(TEXT("self-check: words that don't wrap in 320 px"), LayOut(SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()[SNew(STextBlock).Text(FText::FromString(TEXT("These words are far too long to fit in a narrow tab without wrapping anywhere at all")))],
		320.f, 200.f).Num(), 1);
	TestEqual(TEXT("self-check: the same words wrapping"), LayOut(SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("These words are far too long to fit in a narrow tab without wrapping anywhere at all")))],
		320.f, 200.f).Num(), 0);
	int32 Reported = 0;
	auto Check = [this, &Reported](const FString& Page, const FTabSize& Size, const TArray<FString>& Overflows)
	{
		for (const FString& Overflow : Overflows)
		{
			if (++Reported <= 40)
			{
				AddError(FString::Printf(TEXT("%s at %.0fx%.0f: %s"), *Page, Size.Width, Size.Height, *Overflow));
			}
		}
	};

	// The run lists' picked row: #2a1b10 (and the 2px accent rule it draws), its words in the text's own color
	const FTableRowStyle& RowStyle = FAutopilotStyle::Get().GetWidgetStyle<FTableRowStyle>("Autopilot.Row");
	TestTrue(TEXT("picked row: #2a1b10"), RowStyle.ActiveBrush.TintColor.GetSpecifiedColor().Equals(AutopilotColor::RowActive()) && RowStyle.InactiveBrush.TintColor.GetSpecifiedColor().Equals(AutopilotColor::RowActive()));
	TestTrue(TEXT("picked row: #2a1b10, as the design's"), AutopilotColor::RowActive().Equals(FLinearColor(FColor::FromHex(TEXT("2a1b10")))));
	TestEqual(TEXT("the picked row's rule: 2px"), AutopilotUI::PickedBar, 2.f);
	TestEqual(TEXT("picked row: its words in the text's color"), RowStyle.SelectedTextColor.GetSpecifiedColor(), AutopilotColor::Text());
	TestFalse(TEXT("hovered row: no accent"), RowStyle.EvenRowBackgroundHoveredBrush.TintColor.GetSpecifiedColor().Equals(AutopilotColor::Accent()));

	FScratchDir KeysDir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(KeysDir.Path);
	Panel->bOpenVideos = false;  // (recordings laid out without a player: no media here)
	auto TickPanel = [&Panel](const FGeometry& Root) { Panel->Tick(Root, 0.0, 0.016f); };
	const TSharedPtr<FAutopilotRun> Run = LongRun();
	Run->Recording = FPaths::Combine(KeysDir.Path, TEXT("recording.mp4"));  // (a file that is there: the result shows it)
	FFileHelper::SaveStringToFile(TEXT("not a video"), *Run->Recording);
	Run->RecordingSeconds = 123.4;
	// The memory row under the run history: Reset memory… never over the words after Memory (they end before it, clipped)
	Panel->TestMemoryLine = TEXT("1234 runs · and words long enough to run under the button, past its end");
	for (const float Width : { 300.f, 240.f, 360.f })
	{
		const TSharedRef<SWidget> Row = Panel->MakeSaveData();
		const FGeometry RowRoot = FGeometry::MakeRoot(FVector2D(Width, 40.f), FSlateLayoutTransform());
		Row->SlatePrepass(1.f);
		FGeometry Button, Words;
		TSharedPtr<SWidget> ButtonText, WordsText;
		const bool bButton = FindText(Row, RowRoot, TEXT("Reset memory…"), nullptr, Button, ButtonText, true);
		const bool bWords = FindText(Row, RowRoot, Panel->TestMemoryLine.GetValue(), nullptr, Words, WordsText, true);
		TestTrue(*FString::Printf(TEXT("%.0f: the memory row shows Reset memory… and its words"), Width), bButton && bWords);
		if (bButton && bWords)
		{
			const double ButtonLeft = Button.GetAbsolutePosition().X;
			TestTrue(*FString::Printf(TEXT("%.0f: Reset memory… inside the row, its words whole"), Width),
				ButtonLeft + Button.GetAbsoluteSize().X <= Width + 1.0 && Button.GetLocalSize().X + 1.0 >= ButtonText->GetDesiredSize().X);
			TestTrue(*FString::Printf(TEXT("%.0f: the words after Memory end before Reset memory…"), Width),
				Words.GetAbsolutePosition().X + Words.GetAbsoluteSize().X + 4.0 <= ButtonLeft);
			TestEqual(*FString::Printf(TEXT("%.0f: the words after Memory clipped to their own room"), Width),
				WordsText->GetClipping(), EWidgetClipping::ClipToBounds);
		}
	}
	Panel->TestMemoryLine.Reset();
	for (const FTabSize& Size : TabSizes)
	{
		// The whole tab: the setup's steps, then the tab itself (not connected: the connect card)
		Panel->Prefs.bOnboarded = true;
		Panel->bSetupOpen = true;
		const TCHAR* StepNames[] = { TEXT("setup 1 account"), TEXT("setup 2 agent service"), TEXT("setup 3 privacy settings") };
		// (each step's title fills the column beside the logo: its width is the column's, whatever the step holds)
		const TCHAR* StepTitles[] = { TEXT("Welcome to Autopilot"), TEXT("Set up the agent service"), TEXT("Privacy settings") };
		TArray<double> TitleWidths;
		// (step 1's BYOK card folded, as it opens; then shown, its fields measured below)
		Panel->SetupStep = SPlaytestPanel::StepAccount;
		Panel->bKeysShown = false;
		Check(TEXT("setup 1 account, BYOK folded"), Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
		Panel->bKeysShown = true;
		for (int32 Step = SPlaytestPanel::StepAccount; Step <= SPlaytestPanel::StepPrivacy; ++Step)
		{
			Panel->SetupStep = Step;
			Panel->SetupAccess = TEXT("aura");
			Panel->SetupAccountWord = TEXT("AURA APP");
			Check(StepNames[Step], Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
			FGeometry TitleAt;
			TSharedPtr<SWidget> TitleText;
			if (FindText(Panel, FGeometry::MakeRoot(FVector2D(Size.Width, Size.Height), FSlateLayoutTransform()), StepTitles[Step], nullptr,
				TitleAt, TitleText, true))
			{
				TitleWidths.Add(TitleAt.GetLocalSize().X);
			}
			if (Step == SPlaytestPanel::StepPrivacy)
			{
				TestEqual(*FString::Printf(TEXT("%.0f: every setup step's title found"), Size.Width), TitleWidths.Num(), 3);
				for (const double Width : TitleWidths)
				{
					TestTrue(*FString::Printf(TEXT("%.0f: every setup step the same width (%.1f, %.1f)"), Size.Width, Width, TitleWidths[0]),
						FMath::Abs(Width - TitleWidths[0]) <= 1.0);
				}
			}
			if (Step != SPlaytestPanel::StepAccount)
			{
				continue;
			}
			// BYOK: the decision key full width, the Coding Agent's dropdown full width under it, then the LLM key's row:
			// the provider's dropdown (140 px) then the key, inside the tab; each key's hint fits its box
			const FGeometry Root = FGeometry::MakeRoot(FVector2D(Size.Width, Size.Height), FSlateLayoutTransform());
			FGeometry Decision, Agent, Combo, Key;
			const bool bDecision = Find(Panel, Root, Panel->TypeSafeKeyBox.Get(), Decision);
			const bool bAgent = Find(Panel, Root, Panel->CodingAgentCombo.Get(), Agent);
			const bool bCombo = Find(Panel, Root, Panel->LlmProviderCombo.Get(), Combo);
			const bool bKey = Find(Panel, Root, Panel->LlmKeyBox.Get(), Key);
			TestTrue(*FString::Printf(TEXT("%.0f: the keys and both dropdowns shown"), Size.Width), bDecision && bAgent && bCombo && bKey);
			if (bDecision && bAgent && bCombo && bKey)
			{
				const FVector2D DecisionAt = FVector2D(Decision.GetAbsolutePosition()), DecisionSize = FVector2D(Decision.GetAbsoluteSize());
				const FVector2D AgentAt = FVector2D(Agent.GetAbsolutePosition()), AgentSize = FVector2D(Agent.GetAbsoluteSize());
				const FVector2D ComboAt = FVector2D(Combo.GetAbsolutePosition()), ComboSize = FVector2D(Combo.GetAbsoluteSize());
				const FVector2D KeyAt = FVector2D(Key.GetAbsolutePosition()), KeySize = FVector2D(Key.GetAbsoluteSize());
				TestEqual(*FString::Printf(TEXT("%.0f: the dropdown 140 px wide"), Size.Width), ComboSize.X, 140.0, 0.5);
				TestTrue(*FString::Printf(TEXT("%.0f: the key right of the dropdown"), Size.Width), KeyAt.X >= ComboAt.X + ComboSize.X);
				TestTrue(*FString::Printf(TEXT("%.0f: on one row"), Size.Width), KeyAt.Y < ComboAt.Y + ComboSize.Y && ComboAt.Y < KeyAt.Y + KeySize.Y);
				TestTrue(*FString::Printf(TEXT("%.0f: inside the tab"), Size.Width), ComboAt.X >= 0.0 && KeyAt.X + KeySize.X <= Size.Width + 1.0);
				TestTrue(*FString::Printf(TEXT("%.0f: the decision key above, full width"), Size.Width), DecisionAt.Y + DecisionSize.Y <= AgentAt.Y
					&& FMath::Abs(DecisionAt.X - ComboAt.X) <= 1.0 && FMath::Abs(DecisionAt.X + DecisionSize.X - (KeyAt.X + KeySize.X)) <= 1.0);
				TestTrue(*FString::Printf(TEXT("%.0f: the Coding Agent between, full width"), Size.Width), AgentAt.Y + AgentSize.Y <= ComboAt.Y
					&& FMath::Abs(AgentAt.X - DecisionAt.X) <= 1.0 && FMath::Abs(AgentAt.X + AgentSize.X - (DecisionAt.X + DecisionSize.X)) <= 1.0);
				const FEditableTextBoxStyle& BoxStyle = FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Mono");
				const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
				auto Fits = [&](const TCHAR* What, const SEditableTextBox& Box, double BoxWidth)
				{
					const double Room = BoxWidth - BoxStyle.Padding.Left - BoxStyle.Padding.Right - 2.0;  // (and the caret)
					const double Wants = Measure->Measure(Box.GetHintText(), BoxStyle.TextStyle.Font).X;
					TestTrue(*FString::Printf(TEXT("%.0f: the %s key's hint fits (%.0f of %.0f px)"), Size.Width, What, Wants, Room), Wants <= Room);
				};
				Fits(TEXT("decision"), *Panel->TypeSafeKeyBox, DecisionSize.X);
				Fits(TEXT("LLM"), *Panel->LlmKeyBox, KeySize.X);
			}
			Panel->TestAccount = FPlaytestAccount();  // (nothing known: not this editor's agent service's, if it has one)
			const FString SetupWords = Shown(Panel, Size.Width, Size.Height);
			TestTrue(*FString::Printf(TEXT("%.0f: the agent says OpenCode"), Size.Width), SetupWords.Contains(TEXT("| OpenCode |")));
			TestTrue(*FString::Printf(TEXT("%.0f: the dropdown says Anthropic"), Size.Width), SetupWords.Contains(TEXT("| Anthropic |")));
			TestTrue(*FString::Printf(TEXT("%.0f: the Coding Agent's label, between the keys"), Size.Width),
				SetupWords.Contains(TEXT("Decision provider API key")) && SetupWords.Contains(TEXT("Coding Agent")) && SetupWords.Contains(TEXT("LLM provider API key"))
				&& SetupWords.Find(TEXT("Decision provider API key")) < SetupWords.Find(TEXT("Coding Agent"))
				&& SetupWords.Find(TEXT("Coding Agent")) < SetupWords.Find(TEXT("LLM provider API key")));
			TestEqual(*FString::Printf(TEXT("%.0f: the decision key's hint"), Size.Width), Panel->TypeSafeKeyBox->GetHintText().ToString(), FString(TEXT("Optional")));
			TestEqual(*FString::Printf(TEXT("%.0f: the LLM key's hint"), Size.Width), Panel->LlmKeyBox->GetHintText().ToString(), FString(TEXT("Optional")));
			// Each agent: laid out inside the tab, the provider it takes shown (locked for Claude Code and Codex); OpenCode
			// with its longest provider name, which fits the dropdown whole
			struct FAgentCase { const TCHAR* Agent; const TCHAR* Provider; const TCHAR* AgentName; const TCHAR* ProviderName; bool bLocked; };
			const FAgentCase Cases[] = { { TEXT("claude"), TEXT(""), TEXT("Claude Code"), TEXT("Anthropic"), true },
				{ TEXT("codex"), TEXT(""), TEXT("Codex"), TEXT("OpenAI"), true }, { TEXT("opencode"), TEXT("opencode"), TEXT("OpenCode"), TEXT("OpenCode Zen"), false } };
			for (const FAgentCase& Case : Cases)
			{
				Panel->PickedCodingAgent = Case.Agent;
				Panel->PickedLlmProvider = Case.Provider;
				Check(FString::Printf(TEXT("setup 1 account (%s)"), Case.AgentName), Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
				const FString Words = Shown(Panel, Size.Width, Size.Height);
				TestTrue(*FString::Printf(TEXT("%.0f %s: its name and provider shown"), Size.Width, Case.AgentName),
					Words.Contains(FString::Printf(TEXT("| %s |"), Case.AgentName)) && Words.Contains(FString::Printf(TEXT("| %s |"), Case.ProviderName)));
				TestTrue(*FString::Printf(TEXT("%.0f %s: the provider locked or not"), Size.Width, Case.AgentName), !Panel->LlmProviderCombo->IsEnabled() == Case.bLocked);
				FGeometry Name;
				TSharedPtr<SWidget> NameText;
				if (FindText(Panel, Root, Case.ProviderName, Panel->LlmProviderCombo.Get(), Name, NameText))
				{
					TestTrue(*FString::Printf(TEXT("%.0f %s: %s fits the dropdown (%.0f of %.0f px)"), Size.Width, Case.AgentName, Case.ProviderName,
						NameText->GetDesiredSize().X, Name.GetLocalSize().X), Name.GetLocalSize().X + 0.5 >= NameText->GetDesiredSize().X);
				}
				else
				{
					AddError(FString::Printf(TEXT("%.0f %s: %s not found in the dropdown"), Size.Width, Case.AgentName, Case.ProviderName));
				}
			}
			Panel->PickedCodingAgent.Empty();
			Panel->PickedLlmProvider.Empty();
			Panel->TestAccount.Reset();
		}
		// Step 4, First run: waiting for the connection and its memory_state, then the Explore run offered (memory empty)
		Panel->SetupStep = SPlaytestPanel::StepFirstRun;
		Panel->TestConnected = false;
		Panel->TestMemory = SPlaytestPanel::EMemoryView::Unknown;
		Check(TEXT("setup 4 first run (waiting)"), Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
		Panel->TestConnected = true;
		Panel->TestMemory = SPlaytestPanel::EMemoryView::Empty;
		Check(TEXT("setup 4 first run (an Explore run offered)"), Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
		{
			const FString Words = Shown(Panel, Size.Width, Size.Height);
			TestTrue(*FString::Printf(TEXT("%.0f: step 4 offers Explore and finish, and Skip and finish"), Size.Width),
				Words.Contains(TEXT("Explore and finish")) && Words.Contains(TEXT("Skip and finish")) && Words.Contains(TEXT("Recommended")));
		}
		Panel->TestConnected.Reset();
		Panel->TestMemory.Reset();
		Panel->bSetupOpen = false;
		Panel->SetupStep = SPlaytestPanel::StepAccount;
		Check(TEXT("tab (connect card)"), Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
		{
			// The gear beside New run (the rail's, or the narrow bar's): one shown, inside the tab, given its width; no
			// Rerun setup outside the Settings page
			const FGeometry TabRoot = FGeometry::MakeRoot(FVector2D(Size.Width, Size.Height), FSlateLayoutTransform());
			int32 Gears = 0;
			for (const TSharedPtr<SButton>& Gear : Panel->SettingsButtons)
			{
				FGeometry At;
				if (Gear.IsValid() && Find(Panel, TabRoot, Gear.Get(), At))
				{
					++Gears;
					const double Left = At.GetAbsolutePosition().X;
					TestTrue(*FString::Printf(TEXT("%.0f: the gear inside the tab"), Size.Width), Left >= -1.0 && Left + At.GetAbsoluteSize().X <= Size.Width + 1.0);
					TestTrue(*FString::Printf(TEXT("%.0f: the gear given its width"), Size.Width), At.GetLocalSize().X + 1.0 >= Gear->GetDesiredSize().X);
				}
			}
			TestEqual(*FString::Printf(TEXT("%.0f: one gear shown"), Size.Width), Gears, 1);
			TestFalse(*FString::Printf(TEXT("%.0f: no Rerun setup outside Settings"), Size.Width), Shown(Panel, Size.Width, Size.Height).Contains(TEXT("Rerun setup")));
		}
		Panel->bConfirmReset = true;
		Check(TEXT("tab (Reset memory asked)"), Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
		Panel->bConfirmReset = false;
		// Autopilot removed from this computer (or being removed): in place of the rest
		struct FRemovedCase { EPlaytestUninstallState State; const TCHAR* Page; const TCHAR* Says; };
		const FRemovedCase RemovedCases[] = {
			{ EPlaytestUninstallState::Removing, TEXT("tab (removing)"), TEXT("Uninstalling Autopilot from this computer…") },
			{ EPlaytestUninstallState::Removed, TEXT("tab (removed)"), TEXT("Uninstallation successful") },
			{ EPlaytestUninstallState::RemovedButSome, TEXT("tab (removed but for some files)"), TEXT("Uninstallation successful") },
			{ EPlaytestUninstallState::Failed, TEXT("tab (couldn't remove)"), TEXT("Couldn’t uninstall Autopilot from this computer.") } };
		for (const FRemovedCase& Case : RemovedCases)
		{
			Panel->TestUninstall = Case.State;
			Check(Case.Page, Size, LayOut(Panel, Size.Width, Size.Height, TickPanel));
			TestTrue(*FString::Printf(TEXT("%.0f: %s says %s"), Size.Width, Case.Page, Case.Says), Shown(Panel, Size.Width, Size.Height).Contains(Case.Says));
		}
		Panel->TestUninstall.Reset();

		// The run area's pages, at the width the tab gives them
		Panel->PanelWidth = Size.Width;
		Panel->bNarrow = Size.Width < 700.f;
		const float Main = Panel->MainWidth();
		{
			TSharedRef<SWidget> Composer = Panel->MakeComposer();
			Check(TEXT("composer"), Size, LayOut(Composer, Main, Size.Height));
			// The box's hint fits it: every word within its width, its lines within its height
			const FGeometry ComposerRoot = FGeometry::MakeRoot(FVector2D(Main, Size.Height), FSlateLayoutTransform());
			FGeometry BoxAt;
			if (Find(Composer, ComposerRoot, Panel->PromptBox.Get(), BoxAt))
			{
				const FEditableTextBoxStyle& BoxStyle = FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Bare");
				const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
				const FSlateFontInfo& Font = BoxStyle.TextStyle.Font;
				const double Room = BoxAt.GetLocalSize().X - BoxStyle.Padding.Left - BoxStyle.Padding.Right - 2.0;  // (and the caret)
				const FString Hint = Panel->PromptBox->GetHintText().ToString();
				TestTrue(*FString::Printf(TEXT("%.0f: the prompt five lines high (%.0f px)"), Size.Width, BoxAt.GetLocalSize().Y), BoxAt.GetLocalSize().Y + 0.5 >= 5.0 * 13.0 * 1.55);
				TestEqual(TEXT("the composer's hint"), Hint, FString(TEXT("Describe a claim to verify, a goal to reach, or what to test")));
				TArray<FString> Words;
				Hint.ParseIntoArray(Words, TEXT(" "));
				int32 Lines = 1;
				FString Line;
				for (const FString& Word : Words)
				{
					TestTrue(*FString::Printf(TEXT("%.0f: the hint's \"%s\" fits the box"), Size.Width, *Word), Measure->Measure(Word, Font).X <= Room);
					const FString Longer = Line.IsEmpty() ? Word : Line + TEXT(" ") + Word;
					if (!Line.IsEmpty() && Measure->Measure(Longer, Font).X > Room)
					{
						++Lines;
						Line = Word;
					}
					else
					{
						Line = Longer;
					}
				}
				const double Height = Lines * (double)Measure->GetMaxCharacterHeight(Font);
				const double Inner = BoxAt.GetLocalSize().Y - BoxStyle.Padding.Top - BoxStyle.Padding.Bottom;
				TestTrue(*FString::Printf(TEXT("%.0f: the hint's %d line(s) fit the box (%.0f of %.0f px)"), Size.Width, Lines, Height, Inner), Height <= Inner);
			}
			else
			{
				AddError(FString::Printf(TEXT("%.0f: the composer's box not shown"), Size.Width));
			}
			// No kind guessed as the words are typed (the decision model reads it once the run starts): no tag, the Time
			// limit for every kind; no explaining lines
			for (const TCHAR* Text : { TEXT(""), TEXT("Verify that doors open"), TEXT("Explore the game for bugs: the farm"), TEXT("Reach the castle gate") })
			{
				Panel->PromptBox->SetText(FText::FromString(Text));
				Check(FString::Printf(TEXT("composer (\"%s\")"), Text), Size, LayOut(Composer, Main, Size.Height));
				const FString Says = FString(TEXT("| ")) + Shown(Composer, Main, Size.Height) + TEXT(" |");
				for (const TCHAR* Tag : { TEXT("Verify"), TEXT("Explore"), TEXT("Goal") })
				{
					TestFalse(*FString::Printf(TEXT("%.0f \"%s\": no %s tag"), Size.Width, Text, Tag), Says.Contains(FString::Printf(TEXT("| %s |"), Tag)));
				}
				TestTrue(*FString::Printf(TEXT("%.0f \"%s\": the Time limit"), Size.Width, Text), Says.Contains(TEXT("| Time limit |")));
				TestFalse(*FString::Printf(TEXT("%.0f \"%s\": no Duration"), Size.Width, Text), Says.Contains(TEXT("| Duration |")));
				TestFalse(*FString::Printf(TEXT("%.0f \"%s\": no explaining lines"), Size.Width, Text),
					Says.Contains(TEXT("Autopilot remembers")) || Says.Contains(TEXT("Runs as")) || Says.Contains(TEXT("Start with Verify")));
			}
			Panel->PromptBox->SetText(FText::GetEmpty());
		}
		{
			// A run going, at the rollout on its twelfth harness (the longest name), current: every stage's name has all the
			// room it needs (none cut or ellipsized), at every width
			FAutopilotSession& LiveSession = FAutopilotSession::Get();
			const TSharedPtr<FAutopilotRun> Going = OtherRun(987657, false);
			Going->bDone = false;
			Going->Stage = EAutopilotStage::Rollout;
			Going->Attempt = 12;
			LiveSession.Runs.Add(Going);  // (no history file: never saved)
			Panel->SelectedId = Going->Id;
			TSharedRef<SWidget> Live = Panel->MakeLive();
			Check(TEXT("live"), Size, LayOut(Live, Main, Size.Height));
			TestEqual(TEXT("seven stage names"), Panel->StageLabels.Num(), 7);
			const FGeometry LiveRoot = FGeometry::MakeRoot(FVector2D(Main, Size.Height), FSlateLayoutTransform());
			for (const TSharedPtr<STextBlock>& Label : Panel->StageLabels)
			{
				FGeometry At;
				const FString Name = Label->GetText().ToString();
				if (Find(Live, LiveRoot, Label.Get(), At))
				{
					const FVector2D Wants = Label->GetDesiredSize();
					TestTrue(*FString::Printf(TEXT("%.0f: stage \"%s\" whole (%.1f of %.1f px wide)"), Size.Width, *Name, At.GetLocalSize().X, Wants.X),
						At.GetLocalSize().X + 0.5 >= Wants.X);
					TestTrue(*FString::Printf(TEXT("%.0f: stage \"%s\" whole (%.1f of %.1f px high)"), Size.Width, *Name, At.GetLocalSize().Y, Wants.Y),
						At.GetLocalSize().Y + 0.5 >= Wants.Y);
				}
				else
				{
					AddError(FString::Printf(TEXT("%.0f: stage \"%s\" not shown"), Size.Width, *Name));
				}
			}
			TestTrue(*FString::Printf(TEXT("%.0f: the rollout's harness"), Size.Width), Shown(Live, Main, Size.Height).Contains(TEXT("Rollout · H12")));
			TestTrue(*FString::Printf(TEXT("%.0f: harness attempts"), Size.Width), Shown(Live, Main, Size.Height).Contains(TEXT("Harness attempts")));
			{
				// The attempt squares: one row, left to right (never stacked, never past the row), 10 px each, 4 apart: the
				// newest that fit, at most 24, the current one last; the count after them says them all (12 and 30 attempts)
				const TSharedPtr<FAutopilotRun> Attempting = Panel->Selected();
				const int32 WasAttempt = Attempting.IsValid() ? Attempting->Attempt : 0;
				for (const int32 Attempts : { WasAttempt, 30 })
				{
					if (Attempting.IsValid())
					{
						Attempting->Attempt = Attempts;
					}
					LayOut(Live, Main, Size.Height);
					const int32 Squares = Panel->AttemptSquaresShown();
					TestTrue(*FString::Printf(TEXT("%.0f, %d attempts: squares shown (%d)"), Size.Width, Attempts, Squares),
						Squares >= 1 && Squares <= FMath::Min(Attempts, 24));
					if (Size.Width >= 900.f)
					{
						TestEqual(*FString::Printf(TEXT("%.0f, %d attempts: all that may show"), Size.Width, Attempts), Squares, FMath::Min(Attempts, 24));
					}
					double LastRight = -1.0, FirstTop = 0.0;
					for (int32 i = 0; i < Panel->AttemptSquares.Num(); ++i)
					{
						FGeometry At;
						const bool bShown = Find(Live, LiveRoot, Panel->AttemptSquares[i].Get(), At);
						if (i >= Squares)
						{
							TestFalse(*FString::Printf(TEXT("%.0f, %d attempts: square %d collapsed"), Size.Width, Attempts, i + 1), bShown);
							continue;
						}
						if (!bShown)
						{
							AddError(FString::Printf(TEXT("%.0f, %d attempts: square %d not shown"), Size.Width, Attempts, i + 1));
							break;
						}
						const double Left = At.GetAbsolutePosition().X, Top = At.GetAbsolutePosition().Y;
						if (i == 0)
						{
							FirstTop = Top;
						}
						TestTrue(*FString::Printf(TEXT("%.0f, %d attempts: square %d on the first one's line"), Size.Width, Attempts, i + 1), FMath::Abs(Top - FirstTop) < 0.5);
						TestTrue(*FString::Printf(TEXT("%.0f, %d attempts: square %d right of the one before"), Size.Width, Attempts, i + 1), i == 0 || Left >= LastRight + 3.5);
						TestTrue(*FString::Printf(TEXT("%.0f, %d attempts: square %d 10 px wide"), Size.Width, Attempts, i + 1), FMath::Abs(At.GetLocalSize().X - 10.0) < 0.5);
						LastRight = Left + At.GetLocalSize().X;
					}
					FGeometry CountAt;
					const bool bCount = Find(Live, LiveRoot, Panel->AttemptCount.Get(), CountAt);
					TestTrue(*FString::Printf(TEXT("%.0f, %d attempts: the count after the squares, inside the row"), Size.Width, Attempts),
						bCount && CountAt.GetAbsolutePosition().X >= LastRight && CountAt.GetAbsolutePosition().X + CountAt.GetLocalSize().X <= Main - 16.f + 0.5);
					TestTrue(*FString::Printf(TEXT("%.0f, %d attempts: the count says them all"), Size.Width, Attempts),
						Shown(Panel->AttemptCount.ToSharedRef(), 100.f, 40.f) == FString::FromInt(Attempts));
				}
				if (Attempting.IsValid())
				{
					Attempting->Attempt = WasAttempt;
				}
			}
			// The steer row (as when the agent service takes steer): words can be typed at every stage, in the same place,
			// its one hint fitting the box (every word within its width, its lines within its height); Steer pressable at
			// the Rollout only; words typed are kept from stage to stage
			Panel->TestSteers = true;
			struct FSteerCase { EAutopilotStage Stage; const TCHAR* Name; bool bOpen; };
			const FSteerCase SteerCases[] = { { EAutopilotStage::Rollout, TEXT("Rollout"), true }, { EAutopilotStage::Launch, TEXT("Launch"), false },
				{ EAutopilotStage::Analyze, TEXT("Analyze"), false }, { EAutopilotStage::Probe, TEXT("Probe"), false },
				{ EAutopilotStage::Build, TEXT("Build"), false }, { EAutopilotStage::Results, TEXT("Process result"), false },
				{ EAutopilotStage::Report, TEXT("Report"), false }, { EAutopilotStage::Rollout, TEXT("Rollout again"), true } };
			TOptional<FVector2D> SteerAtFirst;
			for (const FSteerCase& Case : SteerCases)
			{
				Going->Stage = Case.Stage;
				Panel->SteerBox->SetText(FText::GetEmpty());
				Check(FString::Printf(TEXT("live (steer, %s)"), Case.Name), Size, LayOut(Live, Main, Size.Height));
				FGeometry BoxAt;
				if (!Find(Live, LiveRoot, Panel->SteerBox.Get(), BoxAt))
				{
					AddError(FString::Printf(TEXT("%.0f %s: the steer box not shown"), Size.Width, Case.Name));
					continue;
				}
				const FVector2D At = FVector2D(BoxAt.GetAbsolutePosition());
				if (!SteerAtFirst.IsSet())
				{
					SteerAtFirst = At;
				}
				TestTrue(*FString::Printf(TEXT("%.0f %s: the steer box in the same place"), Size.Width, Case.Name), At.Equals(SteerAtFirst.GetValue(), 0.5));
				TestTrue(*FString::Printf(TEXT("%.0f %s: the steer box takes words"), Size.Width, Case.Name), Panel->SteerBox->IsEnabled());
				const FString Hint = Panel->SteerBox->GetHintText().ToString();
				TestEqual(*FString::Printf(TEXT("%.0f %s: the steer hint"), Size.Width, Case.Name), Hint, FString(TEXT("Send a steering prompt to Autopilot")));
				const FEditableTextBoxStyle& BoxStyle = FAutopilotStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Autopilot.TextBox.Bare");
				const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
				const FSlateFontInfo& Font = BoxStyle.TextStyle.Font;
				const double Room = BoxAt.GetLocalSize().X - BoxStyle.Padding.Left - BoxStyle.Padding.Right - 2.0;  // (and the caret)
				TArray<FString> Words;
				Hint.ParseIntoArray(Words, TEXT(" "));
				int32 Lines = 1;
				FString Line;
				for (const FString& Word : Words)
				{
					TestTrue(*FString::Printf(TEXT("%.0f %s: the steer hint's \"%s\" fits the box"), Size.Width, Case.Name, *Word), Measure->Measure(Word, Font).X <= Room);
					const FString Longer = Line.IsEmpty() ? Word : Line + TEXT(" ") + Word;
					if (!Line.IsEmpty() && Measure->Measure(Longer, Font).X > Room)
					{
						++Lines;
						Line = Word;
					}
					else
					{
						Line = Longer;
					}
				}
				const double Height = Lines * (double)Measure->GetMaxCharacterHeight(Font);
				const double Inner = BoxAt.GetLocalSize().Y - BoxStyle.Padding.Top - BoxStyle.Padding.Bottom;
				TestTrue(*FString::Printf(TEXT("%.0f %s: the steer hint's %d line(s) fit the box (%.0f of %.0f px)"), Size.Width, Case.Name, Lines, Height, Inner),
					Height <= Inner + 0.5);
				// Words typed: Steer pressable at the Rollout only, and kept (never sent, never cleared) at the other stages
				Panel->SteerBox->SetText(FText::FromString(TEXT("go round the rocks to the left first")));
				LayOut(Live, Main, Size.Height);  // (a widget's enabled state is read again as it is laid out)
				TestEqual(*FString::Printf(TEXT("%.0f %s: Steer pressable"), Size.Width, Case.Name), Panel->SteerButton->IsEnabled(), Case.bOpen);
				if (!Case.bOpen)
				{
					const int32 Lines0 = Going->Feed.Num();
					Panel->OnSteer();
					TestEqual(*FString::Printf(TEXT("%.0f %s: the words kept"), Size.Width, Case.Name), Panel->SteerBox->GetText().ToString(),
						FString(TEXT("go round the rocks to the left first")));
					TestEqual(*FString::Printf(TEXT("%.0f %s: nothing sent"), Size.Width, Case.Name), Going->Feed.Num(), Lines0);
				}
			}
			Panel->SteerBox->SetText(FText::GetEmpty());
			Panel->TestSteers.Reset();
			LiveSession.Runs.Remove(Going);
			Panel->SelectedId = INDEX_NONE;
		}
		Panel->ResultBox = SNew(SVerticalBox);
		Panel->RebuildResult(Run);
		TestTrue(*FString::Printf(TEXT("%.0f: the result shows its recording"), Size.Width), Panel->Video.IsValid());
		Check(TEXT("result"), Size, LayOut(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height));
		{
			// The run's id line, and at its right, on the same row, Run again, Copy report and Run folder: icons named by
			// their tooltips; the prompt close under the row
			const FGeometry ResultRoot = FGeometry::MakeRoot(FVector2D(Main - 32.f, Size.Height), FSlateLayoutTransform());
			FGeometry IdAt;
			TSharedPtr<SWidget> IdText;
			const bool bId = FindText(Panel->ResultBox.ToSharedRef(), ResultRoot, Run->IdLine(), nullptr, IdAt, IdText, true);
			TestTrue(*FString::Printf(TEXT("%.0f: the run's id line shown"), Size.Width), bId);
			double Left = IdAt.GetAbsolutePosition().X + IdAt.GetAbsoluteSize().X - 1.0;
			for (const TCHAR* Name : { TEXT("Run again"), TEXT("Copy report"), TEXT("Run folder") })
			{
				FGeometry At;
				const bool bButton = FindNamedButton(Panel->ResultBox.ToSharedRef(), ResultRoot, Name, At);
				TestTrue(*FString::Printf(TEXT("%.0f: %s, an icon by the id line"), Size.Width, Name), bButton);
				if (bId && bButton)
				{
					const double Middle = At.GetAbsolutePosition().Y + At.GetAbsoluteSize().Y / 2.0;
					TestTrue(*FString::Printf(TEXT("%.0f: %s on the id line's row"), Size.Width, Name),
						Middle >= IdAt.GetAbsolutePosition().Y - 4.0 && Middle <= IdAt.GetAbsolutePosition().Y + IdAt.GetAbsoluteSize().Y + 4.0);
					TestTrue(*FString::Printf(TEXT("%.0f: %s after it, at the right"), Size.Width, Name), At.GetAbsolutePosition().X >= Left);
					Left = At.GetAbsolutePosition().X + At.GetAbsoluteSize().X - 1.0;
				}
			}
			TestTrue(*FString::Printf(TEXT("%.0f: the last icon at the right edge"), Size.Width), Left >= Main - 32.f - 4.0);
			TestFalse(*FString::Printf(TEXT("%.0f: no text buttons for them"), Size.Width),
				Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height).Contains(TEXT("Run again")));
		}
		{
			// A run whose recording's file is gone: the player kept, nothing to press, and why
			const TSharedPtr<FAutopilotRun> NoVideo = OtherRun(987658, false);
			NoVideo->bDone = true;
			NoVideo->Recording = FPaths::Combine(KeysDir.Path, TEXT("gone.mp4"));
			Panel->RebuildResult(NoVideo);
			TestTrue(*FString::Printf(TEXT("%.0f: a recording gone: the player kept"), Size.Width), Panel->Video.IsValid());
			TestTrue(*FString::Printf(TEXT("%.0f: a recording gone: said"), Size.Width),
				Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height).Contains(TEXT("The recording isn’t there any more.")));
			// A run without a recording: nothing for it, no empty box
			NoVideo->Recording.Empty();
			Panel->RebuildResult(NoVideo);
			TestFalse(*FString::Printf(TEXT("%.0f: no recording, no player"), Size.Width), Panel->Video.IsValid());
			TestFalse(*FString::Printf(TEXT("%.0f: nothing said, no line"), Size.Width),
				Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height).Contains(TEXT("No recording")));

			// Why there is none, when the result said: one muted line that wraps inside the tab (never while the run
			// goes on, never with a player)
			NoVideo->Recording.Reset();
			NoVideo->NoRecording = TEXT("the video couldn't be made (ffmpeg exited with code 3221225477 while it encoded the frames): its frames are in C:/Users/someone/AppData/Local/Autopilot/projects/a-project-with-a-long-name/runs/20261015-101500/frames");
			const FString Line = TEXT("No recording: ") + NoVideo->NoRecording;
			Panel->RebuildResult(NoVideo);
			Check(TEXT("result (no recording, why)"), Size, LayOut(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height));
			TestEqual(*FString::Printf(TEXT("%.0f: why there is no recording, once"), Size.Width),
				Count(Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height), *Line), 1);
NoVideo->bDone = false;
			Panel->RebuildResult(NoVideo);
			TestFalse(*FString::Printf(TEXT("%.0f: not while the run goes on"), Size.Width),
				Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height).Contains(TEXT("No recording")));
			NoVideo->bDone = true;
			Run->NoRecording = NoVideo->NoRecording;
			Panel->RebuildResult(Run);
			TestFalse(*FString::Printf(TEXT("%.0f: never with a player"), Size.Width),
				Panel->Video.IsValid() && Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height).Contains(TEXT("No recording")));
			Run->NoRecording.Reset();

			// The findings: their header with some (as before), no section at all with none
			const FString Words = Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height);
			TestTrue(*FString::Printf(TEXT("%.0f: the engine findings' label"), Size.Width), Words.Contains(TEXT("Engine findings · 1")));
			TestTrue(*FString::Printf(TEXT("%.0f: the agent's finding"), Size.Width), Words.Contains(TEXT("| Found |")) && Words.Contains(TEXT("Agent finding")));
			TestTrue(*FString::Printf(TEXT("%.0f: how many times, at the finding's right"), Size.Width), Words.Contains(TEXT("\u00d714")));
			NoVideo->AgentNotes.Reset();
			NoVideo->Findings.Reset();
			Panel->RebuildResult(NoVideo);
			Check(TEXT("result (no findings)"), Size, LayOut(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height));
			const FString NoneWords = Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height);
			TestFalse(*FString::Printf(TEXT("%.0f: no findings, no header"), Size.Width), NoneWords.Contains(TEXT("Findings")));
			TestFalse(*FString::Printf(TEXT("%.0f: no findings, no placeholder"), Size.Width), NoneWords.Contains(TEXT("· none")));

			// The result's alerts: each as said, once, before the reason, wrapping inside the tab; none: nothing
			const FString Alert = TEXT("Autopilot couldn't look at the game's picture: the agent's LLM provider has no model that looks at pictures (DeepSeek's models can't look at pictures). A claim about how something looks (an animation, an effect, the UI) can't be settled without it: choose an LLM provider whose models look at pictures (Anthropic, OpenAI, Google or OpenRouter).");
			NoVideo->Alerts = { Alert };
			Panel->RebuildResult(NoVideo);
			Check(TEXT("result (alert)"), Size, LayOut(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height));
			const FString AlertWords = Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height);
			TestEqual(*FString::Printf(TEXT("%.0f: the alert, once"), Size.Width), Count(AlertWords, *Alert), 1);
			TestTrue(*FString::Printf(TEXT("%.0f: the alert before the reason"), Size.Width),
				AlertWords.Find(Alert) >= 0 && AlertWords.Find(Alert) < AlertWords.Find(NoVideo->Reason));
			NoVideo->Alerts.Reset();
			Panel->RebuildResult(NoVideo);
			TestFalse(*FString::Printf(TEXT("%.0f: no alerts, nothing"), Size.Width),
				Shown(Panel->ResultBox.ToSharedRef(), Main - 32.f, Size.Height).Contains(TEXT("couldn't look")));
			Panel->RebuildResult(Run);
		}
		TSharedRef<SWidget> SettingsPage = Panel->MakeSettings();
		Panel->RebuildAccount();
		Panel->RebuildSettings();
		Check(TEXT("settings"), Size, LayOut(SettingsPage, Main, Size.Height));
		Panel->bConfirmSignOut = true;
		Panel->Prefs.Access = TEXT("aura");
		Panel->RebuildAccount();
		Check(TEXT("settings (sign out asked)"), Size, LayOut(SettingsPage, Main, Size.Height));
		Panel->bConfirmSignOut = false;
		Panel->Prefs.Access = TEXT("own_keys");
		Panel->RebuildAccount();
		Check(TEXT("settings (BYOK)"), Size, LayOut(SettingsPage, Main, Size.Height));
		Panel->bConfirmRemove = true;
		Check(TEXT("settings (remove asked)"), Size, LayOut(SettingsPage, Main, Size.Height));
		TestTrue(*FString::Printf(TEXT("%.0f: remove asked: Delete Autopilot's memories"), Size.Width),
			Shown(SettingsPage, Main, Size.Height).Contains(TEXT("[Optional] Delete Autopilot’s memories for all projects in this machine across all editors. I understand that this action is destructive and deletes Autopilot’s memories for my entire team.")));
		Panel->bConfirmRemove = false;
		Check(TEXT("usage run row"), Size, LayOut(Panel->MakeUsageRunRow(Run), Main - 32.f, Size.Height));
		{
			// A usage row: plain (nothing to click or hover, no hand)
			const TSharedRef<SWidget> Row = Panel->MakeUsageRunRow(Run);
			TestFalse(*FString::Printf(TEXT("%.0f: usage row: no button in it"), Size.Width), HasButton(Row));
			TestFalse(*FString::Printf(TEXT("%.0f: usage row: no hand"), Size.Width), Row->OnCursorQuery(FGeometry(), FPointerEvent()).IsEventHandled() && Row->OnCursorQuery(FGeometry(), FPointerEvent()).GetCursorType() == EMouseCursor::Hand);
			TestFalse(*FString::Printf(TEXT("%.0f: usage row: takes no clicks"), Size.Width),
				Row->OnMouseButtonDown(FGeometry(), FPointerEvent()).IsEventHandled());
		}

		// The Settings page with runs of its own: one billed through Aura (a long sum), one BYOK, one below a cent; the
		// account: Aura with this period's bill (long), then BYOK with keys saved, then BYOK with none
		FAutopilotSession& Session = FAutopilotSession::Get();
		const int32 Before = Session.Runs.Num();
		const TSharedPtr<FAutopilotRun> Byok = OtherRun(987655, false);
		Byok->Usage.Add(FPlaytestModelUsage());  // (a BYOK run: tokens used, nothing billed)
		const TSharedPtr<FAutopilotRun> Cent = OtherRun(987656, true);
		Session.Runs.Append({ Run, Byok, Cent });  // (no history file: never saved)
		++Session.UsageRevision;
		FPlaytestAccount Aura;
		Aura.bKnown = true;
		Aura.Mode = TEXT("aura");
		Aura.bSignedIn = true;
		Aura.Email = TEXT("a.very.long.address.for.someone@a-studio-with-a-very-long-domain-name.example.com");
		Aura.Bill.bKnown = true;
		Aura.Bill.Usd = 1234567.891;
		Aura.Bill.Runs = 123456;
		Aura.Bill.From = TEXT("2026-12-01");
		Aura.Bill.To = TEXT("2027-01-31");
		Panel->TestAccount = Aura;
		Panel->Prefs.Access = TEXT("aura");
		Panel->RebuildAccount();
		Panel->RebuildSettings();
		Check(TEXT("settings (Aura, billed)"), Size, LayOut(SettingsPage, Main, Size.Height));
		const FString AuraWords = Shown(SettingsPage, Main, Size.Height);
		TestTrue(*FString::Printf(TEXT("%.0f: the billing period, its days and its bill"), Size.Width),
			AuraWords.Contains(TEXT("Billing period (Dec 1, 2026 – Jan 31, 2027): | $1,234,567.89 USD")));
		TestFalse(*FString::Printf(TEXT("%.0f: no selected run's usage"), Size.Width), AuraWords.Contains(TEXT("Selected run")) || AuraWords.Contains(TEXT("SELECTED RUN")));
		TestFalse(*FString::Printf(TEXT("%.0f: no model usage table"), Size.Width), AuraWords.Contains(TEXT("claude-opus-5-5")) || AuraWords.Contains(TEXT("Cache read"))
			|| AuraWords.Contains(TEXT("LLM tokens")));
		TestTrue(*FString::Printf(TEXT("%.0f: what sharing sends (as the setup says it)"), Size.Width), AuraWords.Contains(
			TEXT("Enabling this sends recordings, screenshots, and agent traces and memories to the developers of Autopilot.")));
		TestFalse(*FString::Printf(TEXT("%.0f: no code export sent"), Size.Width), AuraWords.Contains(TEXT("code export")));
		TestFalse(*FString::Printf(TEXT("%.0f: no time limit (each run's is the composer's)"), Size.Width),
			AuraWords.Contains(TEXT("Time limit")) || AuraWords.Contains(TEXT("| min |")));
		TestTrue(*FString::Printf(TEXT("%.0f: the agent service line"), Size.Width), !AuraWords.Contains(TEXT("started by Autopilot")) && !AuraWords.Contains(TEXT("started outside")));
		TestTrue(*FString::Printf(TEXT("%.0f: this project's runs: Time, Prompt, Billed"), Size.Width), AuraWords.Contains(TEXT("| Time | Prompt | Billed |")));

		TestTrue(*FString::Printf(TEXT("%.0f: this run's bill"), Size.Width), AuraWords.Contains(TEXT("| $1,234,567.89 |")));
		TestTrue(*FString::Printf(TEXT("%.0f: below a cent"), Size.Width), AuraWords.Contains(TEXT("| < $0.01 |")));
		TestTrue(*FString::Printf(TEXT("%.0f: a BYOK run: no bill"), Size.Width), AuraWords.Contains(TEXT("| —")));
		TestTrue(*FString::Printf(TEXT("%.0f: the project's bill"), Size.Width), AuraWords.Contains(TEXT("Billed by Aura | $1,234,567.89")));
		TestFalse(*FString::Printf(TEXT("%.0f: no billed-to-your-plan note"), Size.Width), AuraWords.Contains(TEXT("billed to your Aura plan")));
		Aura.Bill = FPlaytestPeriodBill();  // (Aura couldn't say: nothing)
		Panel->TestAccount = Aura;
		Panel->RebuildAccount();
		TestFalse(*FString::Printf(TEXT("%.0f: no period bill"), Size.Width), Shown(SettingsPage, Main, Size.Height).Contains(TEXT("Billing period")));
		// (The billed run's row still says its bill)
		TestEqual(*FString::Printf(TEXT("%.0f: the bills as before"), Size.Width), Count(Shown(SettingsPage, Main, Size.Height), TEXT("| $1,234,567.89 |")),
			Count(AuraWords, TEXT("| $1,234,567.89 |")));
		// Out of credits: Aura's words on the account card, with Add credits, and over the composer; none named where to
		// add them: the words alone; credits again: neither
		const FString NoCredits = TEXT("Your team is out of Aura credits. Add credits at https://aura.example.com/account/pricing to keep playtesting.");
		Aura.OutOfCredits = NoCredits;
		Aura.AddCreditsUrl = TEXT("https://aura.example.com/account/pricing");
		Panel->TestAccount = Aura;
		Panel->RebuildAccount();
		Check(TEXT("settings (Aura, out of credits)"), Size, LayOut(SettingsPage, Main, Size.Height));
		{
			const FString Card = Shown(SettingsPage, Main, Size.Height);
			TestTrue(*FString::Printf(TEXT("%.0f: out of credits: Aura's words on the card"), Size.Width), Card.Contains(NoCredits));
			// (Aura's words say "Add credits at …" too: the button is a second)
			TestEqual(*FString::Printf(TEXT("%.0f: out of credits: Add credits on the card"), Size.Width), Count(Card, TEXT("Add credits")), 2);
			const TSharedRef<SWidget> Composer = Panel->MakeComposer();
			Check(TEXT("composer (out of credits)"), Size, LayOut(Composer, Main, Size.Height));
			const FString Over = Shown(Composer, Main, Size.Height);
			TestTrue(*FString::Printf(TEXT("%.0f: out of credits: Aura's words over the composer"), Size.Width), Over.Contains(NoCredits));
			TestEqual(*FString::Printf(TEXT("%.0f: out of credits: Add credits over the composer"), Size.Width), Count(Over, TEXT("Add credits")), 2);
		}
		Aura.AddCreditsUrl.Reset();
		Panel->TestAccount = Aura;
		Panel->RebuildAccount();
		{
			const FString Card = Shown(SettingsPage, Main, Size.Height);
			const FString Over = Shown(Panel->MakeComposer(), Main, Size.Height);
			TestTrue(*FString::Printf(TEXT("%.0f: nowhere named: the words"), Size.Width), Card.Contains(NoCredits) && Over.Contains(NoCredits));
			TestTrue(*FString::Printf(TEXT("%.0f: nowhere named: no Add credits"), Size.Width), Count(Card, TEXT("Add credits")) == 1 && Count(Over, TEXT("Add credits")) == 1);
		}
		Aura.OutOfCredits.Reset();
		Panel->TestAccount = Aura;
		Panel->RebuildAccount();
		TestFalse(*FString::Printf(TEXT("%.0f: credits again: nothing said"), Size.Width),
			Shown(SettingsPage, Main, Size.Height).Contains(NoCredits) || Shown(Panel->MakeComposer(), Main, Size.Height).Contains(NoCredits));
		FPlaytestAccount Keys;
		Keys.bKnown = true;
		Keys.Mode = TEXT("own_keys");
		Keys.DecisionKey = TEXT("••••a91f");
		Keys.LlmKey = TEXT("••••3c2e");
		Panel->TestAccount = Keys;
		Panel->Prefs.Access = TEXT("own_keys");
		Panel->RebuildAccount();
		Check(TEXT("settings (BYOK, keys saved)"), Size, LayOut(SettingsPage, Main, Size.Height));
		const FString KeysWords = Shown(SettingsPage, Main, Size.Height);
		TestTrue(*FString::Printf(TEXT("%.0f: BYOK tag"), Size.Width), KeysWords.Contains(TEXT("BYOK")));
		TestTrue(*FString::Printf(TEXT("%.0f: the keys saved (an older agent service: OpenCode, Anthropic)"), Size.Width),
			KeysWords.Contains(TEXT("Saved on this computer: Decision provider ••••a91f · OpenCode (Anthropic) ••••3c2e")));
		Keys.LlmProvider = TEXT("openai");
		Keys.bLlmProviderSent = true;
		Panel->TestAccount = Keys;
		Panel->RebuildAccount();
		Check(TEXT("settings (BYOK, OpenAI)"), Size, LayOut(SettingsPage, Main, Size.Height));
		TestTrue(*FString::Printf(TEXT("%.0f: the keys saved (OpenAI)"), Size.Width),
			Shown(SettingsPage, Main, Size.Height).Contains(TEXT("Saved on this computer: Decision provider ••••a91f · OpenCode (OpenAI) ••••3c2e")));
		// Each agent, and OpenCode's other providers (one the tab doesn't know: its id)
		struct FKeysCase { const TCHAR* Agent; const TCHAR* Provider; const TCHAR* Says; };
		const FKeysCase KeysCases[] = { { TEXT("claude"), TEXT("anthropic"), TEXT("Claude Code (Anthropic) ••••3c2e") },
			{ TEXT("codex"), TEXT("openai"), TEXT("Codex (OpenAI) ••••3c2e") },
			{ TEXT("opencode"), TEXT("openrouter"), TEXT("OpenCode (OpenRouter) ••••3c2e") },
			{ TEXT("opencode"), TEXT("opencode"), TEXT("OpenCode (OpenCode Zen) ••••3c2e") },
			{ TEXT("opencode"), TEXT("google"), TEXT("OpenCode (Gemini) ••••3c2e") },
			{ TEXT("opencode"), TEXT("some-new-provider"), TEXT("OpenCode (some-new-provider) ••••3c2e") } };
		for (const FKeysCase& Case : KeysCases)
		{
			Keys.CodingAgent = Case.Agent;
			Keys.bCodingAgentSent = true;
			Keys.LlmProvider = Case.Provider;
			Panel->TestAccount = Keys;
			Panel->RebuildAccount();
			Check(FString::Printf(TEXT("settings (BYOK, %s)"), Case.Says), Size, LayOut(SettingsPage, Main, Size.Height));
			TestTrue(*FString::Printf(TEXT("%.0f: the keys saved (%s)"), Size.Width, Case.Says),
				Shown(SettingsPage, Main, Size.Height).Contains(FString(TEXT("Saved on this computer: Decision provider ••••a91f · ")) + Case.Says));
		}
		Keys.CodingAgent = TEXT("opencode");
		Keys.LlmProvider = TEXT("anthropic");
		Panel->TestAccount = Keys;
		Panel->RebuildAccount();
		TestTrue(*FString::Printf(TEXT("%.0f: the keys saved (Anthropic)"), Size.Width),
			Shown(SettingsPage, Main, Size.Height).Contains(TEXT("OpenCode (Anthropic) ••••3c2e")));
		Keys.DecisionKey.Empty();
		Keys.LlmKey.Empty();
		Panel->TestAccount = Keys;
		Panel->RebuildAccount();
		Check(TEXT("settings (BYOK, no keys)"), Size, LayOut(SettingsPage, Main, Size.Height));
		const FString NoKeysWords = Shown(SettingsPage, Main, Size.Height);
		TestFalse(*FString::Printf(TEXT("%.0f: no keys note"), Size.Width), NoKeysWords.Contains(TEXT("Keys are read")) || NoKeysWords.Contains(TEXT("Saved on this computer")));
		TestFalse(*FString::Printf(TEXT("%.0f: no Own keys"), Size.Width), NoKeysWords.Contains(TEXT("Own keys"), ESearchCase::CaseSensitive));
		TestTrue(*FString::Printf(TEXT("%.0f: BYOK: Your own keys"), Size.Width), NoKeysWords.Contains(TEXT("| Your own keys |")));
		TestEqual(*FString::Printf(TEXT("%.0f: BYOK: one Rerun setup"), Size.Width), Count(NoKeysWords, TEXT("Rerun setup")), 1);
		TestFalse(*FString::Printf(TEXT("%.0f: BYOK: no Run setup again, no Change sign-in"), Size.Width),
			NoKeysWords.Contains(TEXT("Run setup again")) || NoKeysWords.Contains(TEXT("Change sign-in")));
		Check(TEXT("usage run row (billed)"), Size, LayOut(Panel->MakeUsageRunRow(Run), Main - 32.f, Size.Height));
		Check(TEXT("usage run row (below a cent)"), Size, LayOut(Panel->MakeUsageRunRow(Cent), Main - 32.f, Size.Height));
		Session.Runs.SetNum(Before);
		++Session.UsageRevision;
		Panel->TestAccount.Reset();
		Panel->Prefs.Access.Empty();
	}
	Panel->Video.Reset();
	AddInfo(FString::Printf(TEXT("%d widgets laid out over %d sizes"), Walked, (int32)UE_ARRAY_COUNT(TabSizes)));
	if (Reported > 40)
	{
		AddError(FString::Printf(TEXT("... and %d more"), Reported - 40));
	}
	return true;
}

namespace
{
	/** Every text under Widget (shown or not), in order. */
	void AllTexts(const TSharedRef<SWidget>& Widget, TArray<FString>& Out)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			Out.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			AllTexts(Children->GetChildAt(i), Out);
		}
	}

	FString AllWords(const TSharedRef<SWidget>& Widget)
	{
		TArray<FString> Out;
		AllTexts(Widget, Out);
		return FString::Join(Out, TEXT(" | "));
	}
}

// The gear beside New run opens the Settings page and closes it; Rerun setup, the Settings page's one button (in place of
// Run setup again and Change sign-in), opens the first-run setup at its first step; while a run goes it is disabled,
// saying why. BYOK is the mode's one name (AutopilotPlaytest.Editor.RerunSetup).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotRerunSetupTest, "AutopilotPlaytest.Editor.RerunSetup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotRerunSetupTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the Rerun setup test is skipped"));
		return true;
	}
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->Prefs.bOnboarded = true;
	Panel->TestRunning = false;
	auto LaidOut = [&Panel]()
	{
		Panel->SlatePrepass(1.f);
		Panel->SlatePrepass(1.f);  // (a widget's enabled state is read when it is laid out)
	};

	// The gears (the rail's and the narrow bar's: quiet, and accent-filled while the page is open): icons named Settings,
	// each opening the Settings page, and again closing it; no Rerun setup outside the page
	TestTrue(TEXT("the gears"), Panel->SettingsButtons.Num() >= 2);
	for (const TSharedPtr<SButton>& Gear : Panel->SettingsButtons)
	{
		TestTrue(TEXT("a gear"), Gear.IsValid());
		if (!Gear.IsValid())
		{
			continue;
		}
		TestEqual(TEXT("a gear: no words"), AllWords(Gear.ToSharedRef()), FString());
		TestTrue(TEXT("a gear: its tooltip (Settings)"), Gear->GetToolTip().IsValid());
		Panel->bSettingsPage = false;
		Gear->SimulateClick();
		TestTrue(TEXT("a gear: the Settings page opens"), Panel->bSettingsPage);
		Gear->SimulateClick();
		TestFalse(TEXT("a gear again: it closes"), Panel->bSettingsPage);
	}
	LaidOut();
	TestFalse(TEXT("no Rerun setup outside the Settings page"), Shown(Panel, 1000.f, 700.f).Contains(TEXT("Rerun setup")));
	TestFalse(TEXT("the tip isn't the busy one"), Panel->RerunSetupTip().ToString() == TEXT("Finish or stop the run first"));
	// ... and BYOK is what the setup's first step calls the user's own keys; its privacy step says what sharing sends
	const FString SetupWords = AllWords(Panel->MakeSetup());
	TestTrue(TEXT("setup: BYOK"), SetupWords.Contains(TEXT("BYOK")));
	TestFalse(TEXT("setup: no Own keys"), SetupWords.Contains(TEXT("Own keys"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("setup: no Run setup again"), SetupWords.Contains(TEXT("Run setup again")));
	// (a step's words are read while it is the one shown: its switcher's, once laid out)
	auto StepWords = [&Panel](int32 Step)
	{
		Panel->SetupStep = Step;
		const TSharedRef<SWidget> Setup = Panel->MakeSetup();
		Setup->SlatePrepass(1.f);
		Setup->SlatePrepass(1.f);
		return AllWords(Setup);
	};
	const FString PrivacyWords = StepWords(SPlaytestPanel::StepPrivacy);
	Panel->SetupStep = SPlaytestPanel::StepAccount;
	TestTrue(TEXT("setup: what sharing sends"), PrivacyWords.Contains(TEXT("Enabling this sends recordings, screenshots, and agent traces and memories to the developers of Autopilot. For runs from your own Claude Code or Codex, the agent traces are its conversation during the run.")));
	TestFalse(TEXT("setup: no code export sent"), PrivacyWords.Contains(TEXT("export of its code")));
	FString EverySteps;
	for (int32 Step = SPlaytestPanel::StepAccount; Step <= SPlaytestPanel::StepFirstRun; ++Step)
	{
		EverySteps += StepWords(Step) + TEXT(" | ");
	}
	Panel->SetupStep = SPlaytestPanel::StepAccount;
	for (const TCHAR* Gone : { TEXT("Choose how to run it:"), TEXT("Autopilot learns your game as it plays"), TEXT("Not sure yet?"),
		TEXT("Leave empty if"), TEXT("The agent service runs the agent on this computer"), TEXT("Recommended, not required") })
	{
		TestFalse(*FString::Printf(TEXT("setup: no \"%s\""), Gone), EverySteps.Contains(Gone));
	}
	Panel->bSetupOpen = false;

	// The Settings page: one Rerun setup, whatever the account; no Run setup again, no Change sign-in; Sign out stays
	TSharedRef<SWidget> SettingsPage = Panel->MakeSettings();
	struct FCase { const TCHAR* What; const TCHAR* Mode; bool bSignedIn; const TCHAR* Access; const TCHAR* Also; };
	const FCase Cases[] = {
		{ TEXT("aura"), TEXT("aura"), true, TEXT("aura"), TEXT("Sign out") },
		{ TEXT("BYOK"), TEXT("own_keys"), false, TEXT("own_keys"), TEXT("BYOK") },
		{ TEXT("signed out"), TEXT("aura"), false, TEXT("aura"), TEXT("Sign in again") },
		{ TEXT("not set up"), TEXT("none"), false, TEXT(""), TEXT("Not set up") },
	};
	for (const FCase& Case : Cases)
	{
		FPlaytestAccount Account;
		Account.bKnown = true;
		Account.Mode = Case.Mode;
		Account.bSignedIn = Case.bSignedIn;
		Account.Email = Case.bSignedIn ? TEXT("dev@studio.test") : TEXT("");
		Panel->TestAccount = Account;
		Panel->Prefs.Access = Case.Access;
		Panel->AccountShownKey.Empty();
		Panel->RebuildAccount();
		const FString Words = AllWords(SettingsPage);
		TestEqual(*FString::Printf(TEXT("%s: one Rerun setup"), Case.What), Count(Words, TEXT("Rerun setup")), 1);
		TestFalse(*FString::Printf(TEXT("%s: no Run setup again"), Case.What), Words.Contains(TEXT("Run setup again")));
		TestFalse(*FString::Printf(TEXT("%s: no Change sign-in"), Case.What), Words.Contains(TEXT("Change sign-in")));
		TestFalse(*FString::Printf(TEXT("%s: no Own keys"), Case.What), Words.Contains(TEXT("Own keys"), ESearchCase::CaseSensitive));
		TestTrue(*FString::Printf(TEXT("%s: %s"), Case.What, Case.Also), Words.Contains(Case.Also));
		const TSharedPtr<SButton> Button = Panel->SettingsRerunSetup.Pin();
		TestTrue(*FString::Printf(TEXT("%s: the button"), Case.What), Button.IsValid());
		if (!Button.IsValid())
		{
			continue;
		}
		Panel->bSetupOpen = false;
		Panel->SetupStep = SPlaytestPanel::StepService;
		Button->SimulateClick();
		TestTrue(*FString::Printf(TEXT("%s: the setup opens"), Case.What), Panel->bSetupOpen);
		TestEqual(*FString::Printf(TEXT("%s: at its first step"), Case.What), Panel->SetupStep, (int32)SPlaytestPanel::StepAccount);
		Panel->bSetupOpen = false;
		Panel->TestRunning = true;
		SettingsPage->SlatePrepass(1.f);
		TestFalse(*FString::Printf(TEXT("%s: a run going: disabled"), Case.What), Button->IsEnabled());
		Button->SimulateClick();
		TestFalse(*FString::Printf(TEXT("%s: a run going: nothing opens"), Case.What), Panel->bSetupOpen);
		Panel->TestRunning = false;
	}
	Panel->TestAccount.Reset();
	return true;
}

namespace
{
	TSharedPtr<SButton> FindButton(const TSharedRef<SWidget>& Widget, const FString& Words);
	TSharedPtr<SButton> FindTagged(const TSharedRef<SWidget>& Widget, const FName& Tag);

	/** The check box under Widget whose words start with Words (null when none). */
	TSharedPtr<SCheckBox> FindCheckBox(const TSharedRef<SWidget>& Widget, const FString& Words)
	{
		if (Widget->GetType() == FName(TEXT("SCheckBox")) && AllWords(Widget).StartsWith(Words))
		{
			return StaticCastSharedRef<SCheckBox>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (TSharedPtr<SCheckBox> Found = FindCheckBox(Children->GetChildAt(i), Words))
			{
				return Found;
			}
		}
		return nullptr;
	}
}

// Remove Autopilot: the Settings page's last section asks in place (Delete Autopilot's memories, unticked each time it
// asks), not while a run goes; once asked, what it did takes the tab's place, the setup's too, until Set up again (the
// setup from its first step) or, when it couldn't, Cancel (AutopilotPlaytest.Editor.Remove).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotRemoveTest, "AutopilotPlaytest.Editor.Remove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotRemoveTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the Remove test is skipped"));
		return true;
	}
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->Prefs.bOnboarded = true;
	Panel->TestRunning = false;
	const FGeometry Root = FGeometry::MakeRoot(FVector2D(900.f, 700.f), FSlateLayoutTransform());
	auto Ticked = [&Panel, &Root]()
	{
		Panel->Tick(Root, 0.0, 0.016f);
		Panel->SlatePrepass(1.f);
	};

	// The Settings page's last section
	TSharedRef<SWidget> SettingsPage = Panel->MakeSettings();
	Panel->RebuildSettings();
	const FString Words = Shown(SettingsPage, 600.f, 2000.f);
	TestTrue(TEXT("settings: Uninstall Autopilot, last"), Words.Contains(TEXT("Uninstall Autopilot")) && Words.Find(TEXT("Uninstall Autopilot")) > Words.Find(TEXT("Share runs to improve Autopilot")));
	TestFalse(TEXT("settings: no paragraph before it is asked"), Words.Contains(TEXT("Removes the agent service")));
	TestTrue(TEXT("settings: Uninstall"), Words.Contains(TEXT("| Uninstall |")) || Words.EndsWith(TEXT("| Uninstall")));
	TestFalse(TEXT("settings: not asked yet"), Words.Contains(TEXT("Uninstall Autopilot from this computer?")));
	Panel->bConfirmRemove = true;
	const FString Asked = Shown(SettingsPage, 600.f, 2000.f);
	TestTrue(TEXT("asked in place"), Asked.Contains(TEXT("Uninstall Autopilot from this computer?"))
		&& Asked.Contains(TEXT("[Optional] Delete Autopilot’s memories for all projects in this machine across all editors. I understand that this action is destructive and deletes Autopilot’s memories for my entire team.")));
	TestFalse(TEXT("asked: no Keep Autopilot's memories"), Asked.Contains(TEXT("Keep Autopilot's memories")));
	TestTrue(TEXT("asked: what it removes"), Asked.Contains(TEXT("Removes the agent service, your keys, your Aura sign-in and your setup choices, for every project.")));
	TestFalse(TEXT("asked: no line about data already sent"), Asked.Contains(TEXT("already sent")));
	TestTrue(TEXT("asked: Remove and Cancel"), Asked.Contains(TEXT("| Uninstall |")) && Asked.Contains(TEXT("Cancel")));
	TestFalse(TEXT("delete: unticked at first"), Panel->bRemoveDeletesMemory);
	TSharedPtr<SCheckBox> Delete = FindCheckBox(SettingsPage, TEXT("[Optional] Delete Autopilot’s memories for all projects"));
	TestTrue(TEXT("delete: its check box"), Delete.IsValid() && !Delete->IsChecked());
	if (Delete.IsValid())
	{
		Delete->ToggleCheckedState();
		TestTrue(TEXT("delete: ticked"), Panel->bRemoveDeletesMemory && Delete->IsChecked());
	}
	// (asked: the confirmation, a button of its own, tagged; never pressed here: it would uninstall this computer's agent
	// service for real (OnRemove does nothing from a test, but the test doesn't count on it))
	const TSharedPtr<SButton> Confirm = FindTagged(SettingsPage, TEXT("Autopilot.Uninstall.Confirm"));
	// (Cancel, then asked again: unticked again. A switcher's children are its page shown, as last laid out: laid out
	// again, then the Uninstall that asks, by its tag)
	Panel->bConfirmRemove = false;
	SettingsPage->SlatePrepass(1.f);
	TSharedPtr<SButton> AskAgain = FindTagged(SettingsPage, TEXT("Autopilot.Uninstall.Ask"));
	TestTrue(TEXT("Uninstall: its button"), AskAgain.IsValid() && AllWords(AskAgain.ToSharedRef()) == TEXT("Uninstall"));
	TestTrue(TEXT("Uninstall: the confirmation another button"), Confirm.IsValid() && Confirm != AskAgain);
	if (AskAgain.IsValid())
	{
		AskAgain->SimulateClick();
		TestTrue(TEXT("asked again"), Panel->bConfirmRemove);
		TestFalse(TEXT("asked again: unticked"), Panel->bRemoveDeletesMemory || (Delete.IsValid() && Delete->IsChecked()));
	}
	Panel->bConfirmRemove = false;
	TestTrue(TEXT("no run going: it can be asked"), Panel->CanChangeAccount());
	Panel->TestRunning = true;
	TestFalse(TEXT("a run going: it can't"), Panel->CanChangeAccount());
	Panel->OnRemove();
	TestFalse(TEXT("a run going: nothing removed"), Panel->IsRemovedShown());
	Panel->TestRunning = false;

	// What the tab shows while and after it removes, in place of the rest and of the setup
	struct FCase { EPlaytestUninstallState State; const TCHAR* Says; const TCHAR* Buttons[3]; };
	const FCase Cases[] = {
		{ EPlaytestUninstallState::Removing, TEXT("Uninstalling Autopilot from this computer…"), { nullptr } },
		{ EPlaytestUninstallState::Removed, TEXT("Uninstallation successful"), { TEXT("Set up again"), TEXT("View logs"), nullptr } },
		{ EPlaytestUninstallState::RemovedButSome, TEXT("Uninstallation successful"), { TEXT("View logs"), TEXT("Set up again"), nullptr } },
		{ EPlaytestUninstallState::Failed, TEXT("Couldn’t uninstall Autopilot from this computer."), { TEXT("View logs"), TEXT("Try again"), TEXT("Cancel") } } };
	Panel->Prefs.bOnboarded = false;  // (its choices are gone: the setup would show, but this does)
	for (const FCase& Case : Cases)
	{
		Panel->TestUninstall = Case.State;
		Ticked();
		const FString Removed = Shown(Panel, 900.f, 700.f);
		TestTrue(*FString::Printf(TEXT("%s: shown"), Case.Says), Removed.Contains(Case.Says));
		// (removed: what is left to remove by hand, the plugin in each project)
		const bool bDone = Case.State == EPlaytestUninstallState::Removed || Case.State == EPlaytestUninstallState::RemovedButSome;
		TestEqual(*FString::Printf(TEXT("%s: what is left to do"), Case.Says),
			Removed.Contains(TEXT("To finish, close the editor and delete Plugins/Autopilot/ from each project that has it.")), bDone);
		TestFalse(*FString::Printf(TEXT("%s: not the setup"), Case.Says), Removed.Contains(TEXT("Decide later")));
		TestFalse(*FString::Printf(TEXT("%s: no Open log"), Case.Says), Removed.Contains(TEXT("Open log")));
		for (const TCHAR* Button : Case.Buttons)
		{
			if (Button)
			{
				TestTrue(*FString::Printf(TEXT("%s: %s"), Case.Says, Button), Removed.Contains(Button));
			}
		}
	}
	// Couldn't: Cancel goes back to the tab as it is now (here, the setup: its choices went)
	Panel->OnRemoveCancel();
	TestFalse(TEXT("cancel: not shown"), Panel->IsRemovedShown());
	TestFalse(TEXT("cancel: the setup not opened"), Panel->bSetupOpen);
	// Removed: Set up again opens the setup at its first step
	Panel->TestUninstall = EPlaytestUninstallState::Removed;
	Ticked();
	Panel->bSettingsPage = true;
	Panel->SetupStep = SPlaytestPanel::StepPrivacy;
	Panel->OnSetUpAgain();
	TestFalse(TEXT("set up again: not shown"), Panel->IsRemovedShown());
	TestTrue(TEXT("set up again: the setup opens"), Panel->bSetupOpen && Panel->IsSetupShown());
	TestEqual(TEXT("set up again: at its first step"), Panel->SetupStep, (int32)SPlaytestPanel::StepAccount);
	TestFalse(TEXT("set up again: not the Settings page"), Panel->bSettingsPage);
	Panel->bSetupOpen = false;
	return true;
}

// The setup's LLM provider dropdown with OpenCode: where it starts (the account frame's provider, else autopilot.env's, else
// Anthropic), a pick over both; the key's hint is "Optional" whichever it is (AutopilotPlaytest.Editor.LlmProvider).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotLlmProviderTest, "AutopilotPlaytest.Editor.LlmProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotLlmProviderTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the LLM provider test is skipped"));
		return true;
	}
	FScratchDir Dir;
	auto Hint = [](const TSharedRef<SPlaytestPanel>& Panel) { return Panel->LlmKeyBox->GetHintText().ToString(); };
	auto Frame = [](const TCHAR* Json)
	{
		FPlaytestAccount Account;
		PlaytestAccount::ParseAccount(PlaytestJson::Parse(Json), Account);
		return Account;
	};
	const FString LlmHint = TEXT("Optional");  // (whichever provider: the dropdown names it)

	// Nothing known: Anthropic
	TSharedRef<SPlaytestPanel> Fresh = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Fresh->TestAccount = FPlaytestAccount();  // (nothing known: not this editor's agent service's, if it has one)
	TestEqual(TEXT("nothing known: anthropic"), Fresh->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestEqual(TEXT("nothing known: its hint"), Hint(Fresh), LlmHint);
	TestEqual(TEXT("six options"), Fresh->LlmProviderItems.Num(), 6);
	// The account frame's provider
	Fresh->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"llm_provider\": \"openai\"}"));
	TestEqual(TEXT("the frame's: openai"), Fresh->SetupLlmProvider(), FString(TEXT("openai")));
	TestEqual(TEXT("the frame's: its hint"), Hint(Fresh), LlmHint);
	Fresh->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"opencode\", \"llm_provider\": \"deepseek\"}"));
	TestEqual(TEXT("the frame's: deepseek"), Fresh->SetupLlmProvider(), FString(TEXT("deepseek")));
	Fresh->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"llm_provider\": \"mistral\"}"));
	TestEqual(TEXT("the frame's unknown: anthropic"), Fresh->SetupLlmProvider(), FString(TEXT("anthropic")));

	// autopilot.env's (read when the panel is made, and when the setup opens)
	PlaytestPreferences::WriteKeys(PlaytestPreferences::SetupKeys(FString(), TEXT("opencode"), TEXT("openai"), FString()), Dir.Path);
	TSharedRef<SPlaytestPanel> Saved = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Saved->TestAccount = FPlaytestAccount();
	TestEqual(TEXT("autopilot.env's: openai"), Saved->SetupLlmProvider(), FString(TEXT("openai")));
	TestEqual(TEXT("autopilot.env's: its hint"), Hint(Saved), LlmHint);
	Saved->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\"}"));
	TestEqual(TEXT("an older agent service: autopilot.env's"), Saved->SetupLlmProvider(), FString(TEXT("openai")));
	Saved->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"llm_provider\": \"anthropic\"}"));
	TestEqual(TEXT("the frame's over autopilot.env's"), Saved->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestEqual(TEXT("the frame's over autopilot.env's: its hint"), Hint(Saved), LlmHint);

	// Picked in the dropdown: that one, whatever the frame says
	Saved->LlmProviderCombo->SetSelectedItem(Saved->LlmProviderItems[1]);
	TestEqual(TEXT("picked openai"), Saved->SetupLlmProvider(), FString(TEXT("openai")));
	TestEqual(TEXT("picked openai: its hint"), Hint(Saved), LlmHint);
	Saved->LlmProviderCombo->SetSelectedItem(Saved->LlmProviderItems[0]);
	TestEqual(TEXT("picked anthropic"), Saved->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestEqual(TEXT("picked anthropic: its hint"), Hint(Saved), LlmHint);
	Saved->LlmProviderCombo->SetSelectedItem(Saved->LlmProviderItems[5]);
	TestEqual(TEXT("picked gemini"), Saved->SetupLlmProvider(), FString(TEXT("google")));
	return true;
}

// The setup's Coding Agent dropdown: where it starts (the account frame's agent, else autopilot.env's, else OpenCode), the
// LLM provider it locks (Claude Code: Anthropic, Codex: OpenAI), OpenCode's own provider kept across switches, and
// what "Use my keys" writes for each (AutopilotPlaytest.Editor.CodingAgent).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotCodingAgentTest, "AutopilotPlaytest.Editor.CodingAgent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotCodingAgentTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the Coding Agent test is skipped"));
		return true;
	}
	FScratchDir Dir;
	auto Frame = [](const TCHAR* Json)
	{
		FPlaytestAccount Account;
		PlaytestAccount::ParseAccount(PlaytestJson::Parse(Json), Account);
		return Account;
	};
	auto Locked = [](const TSharedRef<SPlaytestPanel>& Panel)
	{
		// (a widget's enabled state is read when it is laid out: the setup's step 1 shown, then laid out)
		Panel->bSetupOpen = true;
		Panel->SetupStep = SPlaytestPanel::StepAccount;
		Panel->SlatePrepass(1.f);
		Panel->SlatePrepass(1.f);  // (twice: the page shown is picked on the first)
		return !Panel->LlmProviderCombo->IsEnabled();
	};
	auto Item = [](const TArray<TSharedPtr<FString>>& Items, const TCHAR* Id)
	{
		const TSharedPtr<FString>* Found = Items.FindByPredicate([Id](const TSharedPtr<FString>& It) { return *It == Id; });
		return Found ? *Found : TSharedPtr<FString>();
	};
	// What "Use my keys" would write into an empty autopilot.env, keys given
	auto Writes = [](const TSharedRef<SPlaytestPanel>& Panel)
	{
		return PlaytestPreferences::MergeKeys(FString(), PlaytestPreferences::SetupKeys(TEXT("ts"), Panel->SetupCodingAgent(), Panel->SetupLlmProvider(), TEXT("sk")));
	};

	// Nothing known: OpenCode, Anthropic, the provider open
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->TestAccount = FPlaytestAccount();  // (nothing known: not this editor's agent service's, if it has one)
	TestEqual(TEXT("three agents"), Panel->CodingAgentItems.Num(), 3);
	TestEqual(TEXT("nothing known: opencode"), Panel->SetupCodingAgent(), FString(TEXT("opencode")));
	TestEqual(TEXT("nothing known: anthropic"), Panel->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestFalse(TEXT("opencode: the provider open"), Locked(Panel));
	TestEqual(TEXT("opencode writes"), Writes(Panel),
		FString(TEXT("TYPESAFE_API_KEY=ts\nAUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=anthropic\nANTHROPIC_API_KEY=sk\n")));

	// The account frame's agent: its provider shown, locked
	Panel->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"claude\", \"llm_provider\": \"anthropic\"}"));
	TestEqual(TEXT("the frame's: claude"), Panel->SetupCodingAgent(), FString(TEXT("claude")));
	TestEqual(TEXT("claude: anthropic"), Panel->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestTrue(TEXT("claude: the provider locked"), Locked(Panel));
	TestEqual(TEXT("claude writes (no OpenCode provider)"), Writes(Panel), FString(TEXT("TYPESAFE_API_KEY=ts\nAUTOPILOT_AGENT=claude\nANTHROPIC_API_KEY=sk\n")));
	Panel->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"codex\", \"llm_provider\": \"openai\"}"));
	TestEqual(TEXT("the frame's: codex"), Panel->SetupCodingAgent(), FString(TEXT("codex")));
	TestEqual(TEXT("codex: openai"), Panel->SetupLlmProvider(), FString(TEXT("openai")));
	TestTrue(TEXT("codex: the provider locked"), Locked(Panel));
	TestEqual(TEXT("codex writes (no OpenCode provider)"), Writes(Panel), FString(TEXT("TYPESAFE_API_KEY=ts\nAUTOPILOT_AGENT=codex\nOPENAI_API_KEY=sk\n")));
	Panel->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"aider\", \"llm_provider\": \"openrouter\"}"));
	TestEqual(TEXT("the frame's unknown agent: opencode"), Panel->SetupCodingAgent(), FString(TEXT("opencode")));
	Panel->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"opencode\", \"llm_provider\": \"mistral\"}"));
	TestEqual(TEXT("the frame's unknown provider: anthropic"), Panel->SetupLlmProvider(), FString(TEXT("anthropic")));

	// Switching: from Codex (the frame's) to OpenCode, a provider picked for it, to Claude Code and back
	Panel->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"codex\", \"llm_provider\": \"openai\"}"));
	Panel->CodingAgentCombo->SetSelectedItem(Item(Panel->CodingAgentItems, TEXT("opencode")));
	TestEqual(TEXT("picked opencode"), Panel->SetupCodingAgent(), FString(TEXT("opencode")));
	TestEqual(TEXT("opencode after codex: nothing saved, anthropic"), Panel->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestFalse(TEXT("picked opencode: open"), Locked(Panel));
	Panel->LlmProviderCombo->SetSelectedItem(Item(Panel->LlmProviderItems, TEXT("openrouter")));
	TestEqual(TEXT("picked openrouter"), Panel->SetupLlmProvider(), FString(TEXT("openrouter")));
	TestEqual(TEXT("openrouter writes"), Writes(Panel),
		FString(TEXT("TYPESAFE_API_KEY=ts\nAUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=openrouter\nOPENROUTER_API_KEY=sk\n")));
	Panel->CodingAgentCombo->SetSelectedItem(Item(Panel->CodingAgentItems, TEXT("claude")));
	TestEqual(TEXT("switched to claude: anthropic"), Panel->SetupLlmProvider(), FString(TEXT("anthropic")));
	TestTrue(TEXT("switched to claude: locked"), Locked(Panel));
	Panel->LlmProviderCombo->SetSelectedItem(Item(Panel->LlmProviderItems, TEXT("deepseek")));  // (locked: not OpenCode's pick)
	TestEqual(TEXT("claude: still anthropic"), Panel->SetupLlmProvider(), FString(TEXT("anthropic")));
	Panel->CodingAgentCombo->SetSelectedItem(Item(Panel->CodingAgentItems, TEXT("codex")));
	TestEqual(TEXT("switched to codex: openai"), Panel->SetupLlmProvider(), FString(TEXT("openai")));
	Panel->CodingAgentCombo->SetSelectedItem(Item(Panel->CodingAgentItems, TEXT("opencode")));
	TestEqual(TEXT("back to opencode: openrouter again"), Panel->SetupLlmProvider(), FString(TEXT("openrouter")));
	TestFalse(TEXT("back to opencode: open"), Locked(Panel));

	// A key typed for one provider is never saved as another's: the box empties when the provider changes, whatever
	// changed it, and keeps the key when it doesn't
	auto Typed = [&Panel]() { return Panel->LlmKeyBox->GetText().ToString(); };
	Panel->FollowLlmProvider();
	Panel->LlmKeyBox->SetText(FText::FromString(TEXT("sk-or-typed")));
	Panel->LlmProviderCombo->SetSelectedItem(Item(Panel->LlmProviderItems, TEXT("openrouter")));
	TestEqual(TEXT("same provider picked: the key kept"), Typed(), FString(TEXT("sk-or-typed")));
	Panel->LlmProviderCombo->SetSelectedItem(Item(Panel->LlmProviderItems, TEXT("openai")));
	TestTrue(TEXT("another provider picked: emptied"), Typed().IsEmpty());
	Panel->LlmKeyBox->SetText(FText::FromString(TEXT("sk-openai-typed")));
	Panel->CodingAgentCombo->SetSelectedItem(Item(Panel->CodingAgentItems, TEXT("codex")));
	TestEqual(TEXT("to codex, still openai: the key kept"), Typed(), FString(TEXT("sk-openai-typed")));
	Panel->CodingAgentCombo->SetSelectedItem(Item(Panel->CodingAgentItems, TEXT("claude")));
	TestTrue(TEXT("to claude code (anthropic): emptied"), Typed().IsEmpty());
	Panel->LlmKeyBox->SetText(FText::FromString(TEXT("sk-ant-typed")));
	Panel->PickedCodingAgent.Empty();
	Panel->PickedLlmProvider.Empty();
	Panel->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"opencode\", \"llm_provider\": \"deepseek\"}"));
	Panel->FollowLlmProvider();  // (as the tab's tick does)
	TestTrue(TEXT("an account frame's provider: emptied"), Typed().IsEmpty());

	// autopilot.env's agent (read when the panel is made, and when the setup opens); the frame's over it
	PlaytestPreferences::WriteKeys(PlaytestPreferences::SetupKeys(FString(), TEXT("opencode"), TEXT("google"), FString()), Dir.Path);
	PlaytestPreferences::WriteKeys(PlaytestPreferences::SetupKeys(FString(), TEXT("codex"), FString(), FString()), Dir.Path);
	TSharedRef<SPlaytestPanel> Saved = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Saved->TestAccount = FPlaytestAccount();
	TestEqual(TEXT("autopilot.env's: codex"), Saved->SetupCodingAgent(), FString(TEXT("codex")));
	TestEqual(TEXT("autopilot.env's codex: openai"), Saved->SetupLlmProvider(), FString(TEXT("openai")));
	Saved->CodingAgentCombo->SetSelectedItem(Item(Saved->CodingAgentItems, TEXT("opencode")));
	TestEqual(TEXT("to opencode: autopilot.env's provider for it"), Saved->SetupLlmProvider(), FString(TEXT("google")));
	Saved->PickedCodingAgent.Empty();
	Saved->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"claude\", \"llm_provider\": \"anthropic\"}"));
	TestEqual(TEXT("the frame's over autopilot.env's"), Saved->SetupCodingAgent(), FString(TEXT("claude")));
	Saved->TestAccount = Frame(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\"}"));
	TestEqual(TEXT("an older agent service: autopilot.env's"), Saved->SetupCodingAgent(), FString(TEXT("codex")));
	return true;
}

// A run's bill (the agent service's `bill`, through Aura): kept on the run going, saved with the run history and read
// back; a run saved before bills loads with none; the project's bill sums the runs billed (AutopilotPlaytest.Editor.RunBill).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotRunBillTest, "AutopilotPlaytest.Editor.RunBill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotRunBillTest::RunTest(const FString& Parameters)
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	const int32 Before = Session.Runs.Num();
	int32 BilledBefore = 0;
	const double BillBefore = Session.ProjectBill(BilledBefore);

	// The bill frame comes while the run is still going (before its result)
	TSharedPtr<FAutopilotRun> Live = OtherRun(987700, false);
	Live->bDone = false;
	Session.Runs.Add(Live);  // (no history file: never saved)
	Session.OnBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"usd\": 0.593177, \"billed\": true}")));
	TestTrue(TEXT("kept on the run going"), Live->Bill.Shows());
	TestEqual(TEXT("its sum"), Live->Bill.Usd, 0.593177, 1e-9);
	TestEqual(TEXT("its line"), PlaytestAccount::BilledLine(Live->Bill.Usd), FString(TEXT("Billed $0.59")));
	Session.OnBill(PlaytestJson::Parse(TEXT("{\"type\": \"usage\", \"usd\": 9}")));
	TestEqual(TEXT("not a bill: unchanged"), Live->Bill.Usd, 0.593177, 1e-9);
	Live->bDone = true;

	// Saved and read back
	const FString Text = PlaytestJson::Write(FAutopilotSession::RunToJson(*Live));
	const TSharedPtr<FAutopilotRun> Back = FAutopilotSession::RunFromJson(PlaytestJson::Parse(Text));
	TestTrue(TEXT("read back"), Back.IsValid());
	if (Back.IsValid())
	{
		TestTrue(TEXT("read back: billed"), Back->Bill.Shows());
		TestEqual(TEXT("read back: sum"), Back->Bill.Usd, 0.593177, 1e-9);
		TestEqual(TEXT("read back: usage too"), Back->Usage.Num(), Live->Usage.Num());
	}
	// A BYOK run: no bill saved, none read
	const TSharedPtr<FAutopilotRun> Byok = OtherRun(987701, false);
	const TSharedRef<FJsonObject> ByokJson = FAutopilotSession::RunToJson(*Byok);
	TestFalse(TEXT("BYOK: nothing saved"), ByokJson->HasField(TEXT("bill")));
	// A run saved before bills (no "bill" field) loads fine, with none
	const TSharedPtr<FAutopilotRun> Old = FAutopilotSession::RunFromJson(PlaytestJson::Parse(TEXT(
		"{\"version\": 1, \"kind\": \"verify\", \"prompt\": \"The door opens\", \"started\": \"2026-10-01T12:00:00.000Z\", \"elapsed\": 42,"
		" \"done\": true, \"verdict\": \"CONFIRMED\", \"usage\": {\"final\": true, \"models\": [{\"kind\": \"llm\", \"model\": \"m\", \"calls\": 1, \"input\": 10, \"output\": 2}]}}")));
	TestTrue(TEXT("old run loads"), Old.IsValid());
	if (Old.IsValid())
	{
		TestFalse(TEXT("old run: no bill"), Old->Bill.bKnown);
		TestEqual(TEXT("old run: its usage"), Old->Usage.Num(), 1);
	}
	// Billed later (billed false): kept, not shown, not summed
	const TSharedPtr<FAutopilotRun> Later = OtherRun(987702, false);
	Later->Bill = PlaytestAccount::BillFromJson(PlaytestJson::Parse(TEXT("{\"usd\": 5, \"billed\": false, \"reason\": \"later\"}")));
	TestTrue(TEXT("billed later: kept"), Later->Bill.bKnown);
	TestEqual(TEXT("billed later: saved"), PlaytestAccount::BillFromJson(FAutopilotSession::RunToJson(*Later)->GetObjectField(TEXT("bill"))).Reason, FString(TEXT("later")));

	// The project's bill: the runs billed, summed
	const TSharedPtr<FAutopilotRun> Cent = OtherRun(987703, true);
	Session.Runs.Append({ Byok, Later, Cent });
	int32 Billed = 0;
	const double Sum = Session.ProjectBill(Billed);
	TestEqual(TEXT("runs billed"), Billed - BilledBefore, 2);
	TestEqual(TEXT("summed"), Sum - BillBefore, 0.593177 + 0.003, 1e-9);
	TestEqual(TEXT("shown"), TEXT("Billed by Aura ") + PlaytestAccount::Money(Sum - BillBefore), FString(TEXT("Billed by Aura $0.60")));
	Session.Runs.SetNum(Before);
	++Session.UsageRevision;
	return true;
}

// The composer's start frame (its words as typed, kind "auto": the decision model reads which kind of run they ask for),
// the first-run card's (an exploration asked for as one), and a run's kind once the agent service says what was read
// (run_kind), with a test's result shown as GAME TESTED (AutopilotPlaytest.Editor.RunKind).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotRunKindTest, "AutopilotPlaytest.Editor.RunKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotRunKindTest::RunTest(const FString& Parameters)
{
	// The start frames (docs/protocol.md, Runs)
	FString Prompt;
	TSharedRef<FJsonObject> Frame = FAutopilotSession::RunFrame(TEXT(" Lvl_HordeArea test this game and find all the bugs "), 25,
		TEXT("Hordes come every 2 minutes"), Prompt);
	TestEqual(TEXT("composer: kind auto"), Frame->GetStringField(TEXT("kind")), FString(TEXT("auto")));
	TestEqual(TEXT("composer: the words, as typed"), Frame->GetStringField(TEXT("prompt")), FString(TEXT("Lvl_HordeArea test this game and find all the bugs")));
	TestEqual(TEXT("composer: shown as typed"), Prompt, FString(TEXT("Lvl_HordeArea test this game and find all the bugs")));
	TestEqual(TEXT("composer: its Time limit"), (int32)Frame->GetNumberField(TEXT("time_limit_minutes")), 25);
	TestEqual(TEXT("composer: the Design Brief setting"), Frame->GetStringField(TEXT("design_brief")), FString(TEXT("Hordes come every 2 minutes")));
	for (const TCHAR* Field : { TEXT("claim"), TEXT("goal"), TEXT("brief"), TEXT("minutes"), TEXT("steps") })
	{
		TestFalse(*FString::Printf(TEXT("composer: no %s"), Field), Frame->HasField(Field));
	}
	Frame = FAutopilotSession::RunFrame(TEXT("Reach the test chamber"), 0, TEXT("  "), Prompt);
	TestFalse(TEXT("composer, no time limit: none sent"), Frame->HasField(TEXT("time_limit_minutes")));
	TestFalse(TEXT("composer, no design brief: none sent"), Frame->HasField(TEXT("design_brief")));
	Frame = FAutopilotSession::ExploreFrame(10, TEXT("Crops grow after 3 waterings"));
	TestEqual(TEXT("first run: an exploration"), Frame->GetStringField(TEXT("kind")), FString(TEXT("explore")));
	TestEqual(TEXT("first run: its minutes"), (int32)Frame->GetNumberField(TEXT("minutes")), 10);
	TestEqual(TEXT("first run: the Design Brief setting"), Frame->GetStringField(TEXT("brief")), FString(TEXT("Crops grow after 3 waterings")));
	TestFalse(TEXT("first run, no design brief: none"), FAutopilotSession::ExploreFrame(10, FString())->HasField(TEXT("brief")));

	// A run of words the decision model reads: its kind blank until run_kind says it; a test's result is GAME TESTED,
	// an exploration's EXPLORED
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going in this editor: the run_kind part is skipped"));
		return true;
	}
	struct FCase { const TCHAR* Kind; bool bTested; const TCHAR* Verdict; };
	const FCase Cases[] = { { TEXT("explore"), true, TEXT("GAME TESTED") }, { TEXT("explore"), false, TEXT("EXPLORED") }, { TEXT("goal"), false, TEXT("GOAL MISSED") } };
	for (const FCase& Case : Cases)
	{
		const int32 Before = Session.GetAgentStartedRunId();
		Session.OnRunStarted(PlaytestJson::Parse(TEXT("{\"kind\": \"auto\", \"prompt\": \"Lvl_HordeArea test this game\", \"time_limit_minutes\": 20, \"by\": \"claude-code\"}")));
		TSharedPtr<FAutopilotRun> Run = Session.GetAgentStartedRunId() != Before ? Session.FindRun(Session.GetAgentStartedRunId()) : nullptr;
		TestTrue(TEXT("an auto run: started"), Run.IsValid());
		if (!Run.IsValid())
		{
			return true;
		}
		Run->HistoryFile.Empty();  // (never saved)
		TestEqual(TEXT("an auto run: its words"), Run->ShownPrompt(), FString(TEXT("Lvl_HordeArea test this game")));
		TestEqual(TEXT("an auto run: no kind yet"), Run->Kind, FString());
		Session.OnRunKind(PlaytestJson::Parse(TEXT("{\"type\": \"run_kind\", \"kind\": \"nonsense\"}")));
		TestEqual(TEXT("a kind no run has: none"), Run->Kind, FString());
		Session.OnRunKind(PlaytestJson::Parse(*FString::Printf(TEXT("{\"type\": \"run_kind\", \"kind\": \"%s\", \"tested\": %s}"),
			Case.Kind, Case.bTested ? TEXT("true") : TEXT("false"))));
		TestEqual(*FString::Printf(TEXT("%s: the kind read"), Case.Verdict), Run->Kind, FString(Case.Kind));
		FString Result = TEXT("{\"kind\": \"goal\", \"reached\": false, \"notes\": [\"Jumped through a wall\"]");
		Result += Case.bTested ? TEXT(", \"tested\": true}") : TEXT("}");
		Session.OnResult(PlaytestJson::Parse(Result), FString());
		TestEqual(*FString::Printf(TEXT("%s: its result"), Case.Verdict), Run->Verdict, FString(Case.Verdict));
		const TSharedPtr<FAutopilotRun> Saved = FAutopilotSession::RunFromJson(FAutopilotSession::RunToJson(*Run));
		TestTrue(*FString::Printf(TEXT("%s: kept in the history"), Case.Verdict), Saved.IsValid() && Saved->Verdict == Case.Verdict);
		TestTrue(*FString::Printf(TEXT("%s: its tone"), Case.Verdict), Run->Tone() == (Case.bTested || FCString::Strcmp(Case.Kind, TEXT("explore")) == 0 ? EAutopilotTone::Info : EAutopilotTone::Red));
		Run->bDone = true;
		Session.Runs.Remove(Run);
		Session.Unsaved.Remove(Run->Id);
	}
	return true;
}

// The end of a run: "Saving what it learned to memory" is part of the report (the stage bar never goes back); the result's
// memory.updated adds the memory line last in the feed; its recording is kept, copied into the run's folder off the game
// thread, and saved with the run (AutopilotPlaytest.Editor.RunEnd).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotRunEndTest, "AutopilotPlaytest.Editor.RunEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotRunEndTest::RunTest(const FString& Parameters)
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going in this editor: the run end test is skipped"));
		return true;
	}
	const int32 Before = Session.Runs.Num();
	FScratchDir Dir;

	// The stage: at the results, then saving to memory: the report (and nothing moves it back)
	TSharedPtr<FAutopilotRun> Live = OtherRun(987800, false);
	Live->bDone = false;
	Live->Stage = EAutopilotStage::Results;
	Session.Runs.Add(Live);  // (no history file: never saved)
	EAutopilotStage Stage = EAutopilotStage::None;
	int32 Attempt = 0;
	bool bNow = false;
	TestEqual(TEXT("saving: shown as said"), FAutopilotSession::Translate(TEXT("Saving what it learned to memory"), Stage, Attempt, bNow),
		FString(TEXT("Saving what it learned to memory")));
	TestTrue(TEXT("saving: the report"), Stage == EAutopilotStage::Report);
	Session.OnStatus(TEXT("Saving what it learned to memory"));
	TestTrue(TEXT("saving: the run at its report"), Live->Stage == EAutopilotStage::Report);
	Session.OnStatus(TEXT("Looking at the results"));
	TestTrue(TEXT("saving: never back"), Live->Stage == EAutopilotStage::Report);

	// The result: the memory line last, the recording kept (the agent service's, then the copy in the run's folder)
	const FString Video = FPaths::Combine(Dir.Path, TEXT("brain-recording.mp4"));
	FFileHelper::SaveStringToFile(TEXT("an mp4"), *Video);
	const FString ReportDir = FPaths::Combine(Dir.Path, TEXT("20261014-120000"));
	IFileManager::Get().MakeDirectory(*ReportDir, true);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("verdict"), TEXT("confirmed"));
	TSharedRef<FJsonObject> Memory = MakeShared<FJsonObject>();
	Memory->SetBoolField(TEXT("updated"), true);
	Memory->SetBoolField(TEXT("notes"), true);
	Memory->SetBoolField(TEXT("lib"), false);
	Result->SetObjectField(TEXT("memory"), Memory);
	TSharedRef<FJsonObject> Recording = MakeShared<FJsonObject>();
	Recording->SetStringField(TEXT("video"), Video);
	Recording->SetNumberField(TEXT("seconds"), 123.4);
	Result->SetObjectField(TEXT("recording"), Recording);
	Session.OnResult(Result, ReportDir);
	TestTrue(TEXT("done"), Live->bDone);
	TestTrue(TEXT("at its report"), Live->Stage == EAutopilotStage::Report);
	TestTrue(TEXT("the memory line last"), Live->Feed.Num() >= 2 && Live->Feed.Last()->Kind == EAutopilotFeedKind::Memory
		&& Live->Feed.Last()->Text == FAutopilotSession::MemoryLine);
	TestTrue(TEXT("the result's line before it"), Live->Feed.Num() >= 2 && Live->Feed[Live->Feed.Num() - 2]->Kind == EAutopilotFeedKind::Result);
	TestTrue(TEXT("the result's line: Result: Confirmed"), Live->Feed.Num() >= 2 && Live->Feed[Live->Feed.Num() - 2]->Text == TEXT("Result: Confirmed"));
	TestTrue(TEXT("the copied report: Duration"), Live->ReportMarkdown().Contains(TEXT("**Duration:** ")) && !Live->ReportMarkdown().Contains(TEXT("Took ")));
	TestEqual(TEXT("the memory line's words"), FString(FAutopilotSession::MemoryLine), FString(TEXT("Memory updated")));
	// The run history's line: its harness attempts (one: attempt)
	TestTrue(TEXT("history: attempts"), Live->HistoryLine().EndsWith(FString::Printf(TEXT(" · %d attempts"), Live->Attempts())) || Live->Attempts() == 1);
	FAutopilotRun One;
	One.Attempt = 1;
	TestTrue(TEXT("history: one attempt"), One.HistoryLine().EndsWith(TEXT(" · 1 attempt")));
	One.Attempt = 2;
	TestTrue(TEXT("history: two attempts"), One.HistoryLine().EndsWith(TEXT(" · 2 attempts")));
	TestEqual(TEXT("its length"), Live->RecordingSeconds, 123.4, 1e-9);
	const FString Copy = FPaths::Combine(ReportDir, TEXT("recording.mp4"));
	TestTrue(TEXT("the agent service's recording until copied"), Live->Recording == Video || Live->Recording == Copy);
	// (the copy is made on a pool thread; the run takes it on the game thread)
	const double Until = FPlatformTime::Seconds() + 10.0;
	while (Live->Recording != Copy && FPlatformTime::Seconds() < Until)
	{
		FPlatformProcess::Sleep(0.02f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	TestEqual(TEXT("the copy in the run's folder"), Live->Recording, Copy);
	TestTrue(TEXT("the copy made"), FPaths::FileExists(Copy));

	// Saved and read back: the recording and the memory line
	const TSharedPtr<FAutopilotRun> Back = FAutopilotSession::RunFromJson(PlaytestJson::Parse(PlaytestJson::Write(FAutopilotSession::RunToJson(*Live))));
	TestTrue(TEXT("read back"), Back.IsValid());
	if (Back.IsValid())
	{
		TestEqual(TEXT("read back: the recording"), Back->Recording, Copy);
		TestEqual(TEXT("read back: its length"), Back->RecordingSeconds, 123.4, 1e-9);
		TestTrue(TEXT("read back: the memory line"), Back->Feed.Num() && Back->Feed.Last()->Kind == EAutopilotFeedKind::Memory);
	}

	// A result that changed nothing remembered, and recorded nothing: neither
	TSharedPtr<FAutopilotRun> Plain = OtherRun(987801, false);
	Plain->bDone = false;
	Session.Runs.Add(Plain);
	Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("reached"), true);
	Memory->SetBoolField(TEXT("updated"), false);
	Result->SetObjectField(TEXT("memory"), Memory);
	Session.OnResult(Result, FPaths::Combine(Dir.Path, TEXT("20261014-120100")));
	TestTrue(TEXT("plain: done"), Plain->bDone);
	TestFalse(TEXT("plain: no memory line"), Plain->Feed.ContainsByPredicate([](const TSharedPtr<FAutopilotFeedLine>& L) { return L->Kind == EAutopilotFeedKind::Memory; }));
	TestTrue(TEXT("plain: no recording"), Plain->Recording.IsEmpty());
	TestFalse(TEXT("plain: none saved"), FAutopilotSession::RunToJson(*Plain)->HasField(TEXT("recording")));
	TestTrue(TEXT("plain (memory, no recording, no reason): nothing said"), Plain->NoRecording.IsEmpty());
	TestFalse(TEXT("plain: no reason saved"), FAutopilotSession::RunToJson(*Plain)->HasField(TEXT("no_recording")));
	TestTrue(TEXT("recorded: nothing said"), Live->NoRecording.IsEmpty());
	// A run saved before recordings loads with none
	const TSharedPtr<FAutopilotRun> Old = FAutopilotSession::RunFromJson(PlaytestJson::Parse(TEXT(
		"{\"version\": 2, \"kind\": \"goal\", \"prompt\": \"Reach the gate\", \"started\": \"2026-10-01T12:00:00.000Z\", \"elapsed\": 42, \"done\": true, \"verdict\": \"GOAL HIT\"}")));
	TestTrue(TEXT("old run: no recording"), Old.IsValid() && Old->Recording.IsEmpty());
	TestTrue(TEXT("old run: nothing said of it"), Old.IsValid() && Old->NoRecording.IsEmpty());

	// Why a result has no recording: as sent; else nothing (an older agent service too)
	auto Why = [](const TCHAR* Json) { return FAutopilotSession::NoRecordingWhy(PlaytestJson::Parse(Json)); };
	TestEqual(TEXT("why: as sent"), Why(TEXT("{\"verdict\": \"confirmed\", \"memory\": {\"updated\": false}, \"no_recording\": \"the game can't be recorded here: it runs without rendering\"}")),
		FString(TEXT("the game can't be recorded here: it runs without rendering")));
	TestEqual(TEXT("why: as sent, without memory too"), Why(TEXT("{\"verdict\": \"confirmed\", \"no_recording\": \"no picture came from the game while it played\"}")),
		FString(TEXT("no picture came from the game while it played")));
	TestEqual(TEXT("why: an older agent service: nothing"), Why(TEXT("{\"verdict\": \"confirmed\"}")), FString());
	TestEqual(TEXT("why: memory without either: nothing"), Why(TEXT("{\"verdict\": \"confirmed\", \"memory\": {\"updated\": true}}")), FString());
	TestEqual(TEXT("why: a recording: nothing"), Why(TEXT("{\"verdict\": \"confirmed\", \"recording\": {\"video\": \"C:/r.mp4\", \"seconds\": 3}}")), FString());
	TestEqual(TEXT("why: a recording without memory: nothing"),
		Why(TEXT("{\"verdict\": \"confirmed\", \"recording\": {\"video\": \"C:/r.mp4\"}, \"no_recording\": \"ignored\"}")), FString());

	// Decided when the result comes, saved with the run, read back after a restart
	TSharedPtr<FAutopilotRun> Unrecorded = OtherRun(987802, false);
	Unrecorded->bDone = false;
	Session.Runs.Add(Unrecorded);
	Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("reached"), true);
	Result->SetObjectField(TEXT("memory"), Memory);
	Result->SetStringField(TEXT("no_recording"), TEXT("recording couldn't start: no picture: the game's window is minimized or hidden"));
	Session.OnResult(Result, FPaths::Combine(Dir.Path, TEXT("20261014-120200")));
	TestEqual(TEXT("no recording: why, as sent"), Unrecorded->NoRecording,
		FString(TEXT("recording couldn't start: no picture: the game's window is minimized or hidden")));
	const TSharedPtr<FAutopilotRun> UnrecordedBack = FAutopilotSession::RunFromJson(PlaytestJson::Parse(PlaytestJson::Write(FAutopilotSession::RunToJson(*Unrecorded))));
	TestTrue(TEXT("no recording: read back"), UnrecordedBack.IsValid() && UnrecordedBack->NoRecording == Unrecorded->NoRecording
		&& UnrecordedBack->Recording.IsEmpty());
	TSharedPtr<FAutopilotRun> Older1 = OtherRun(987803, false);
	Older1->bDone = false;
	Session.Runs.Add(Older1);
	Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("verdict"), TEXT("confirmed"));
	Session.OnResult(Result, FPaths::Combine(Dir.Path, TEXT("20261014-120300")));
	TestTrue(TEXT("older agent service: nothing said"), Older1->NoRecording.IsEmpty());

	// The result's alerts: kept as said, saved with the run, read back; none: none, and none saved
	const FString Alert = TEXT("Autopilot couldn't look at the game's picture: the game runs without a picture (headless: nothing is rendered). A claim about how something looks (an animation, an effect, the UI) can't be settled without it: run the game with its window shown (not headless, not minimized).");
	TSharedPtr<FAutopilotRun> Alerted = OtherRun(987804, false);
	Alerted->bDone = false;
	Session.Runs.Add(Alerted);
	Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("verdict"), TEXT("INCONCLUSIVE"));
	TArray<TSharedPtr<FJsonValue>> Alerts;
	Alerts.Add(MakeShared<FJsonValueString>(Alert));
	Result->SetArrayField(TEXT("alerts"), Alerts);
	Session.OnResult(Result, FPaths::Combine(Dir.Path, TEXT("20261014-120400")));
	TestTrue(TEXT("alerts: as sent"), Alerted->Alerts.Num() == 1 && Alerted->Alerts[0] == Alert);
	const TSharedPtr<FAutopilotRun> AlertedBack = FAutopilotSession::RunFromJson(PlaytestJson::Parse(PlaytestJson::Write(FAutopilotSession::RunToJson(*Alerted))));
	TestTrue(TEXT("alerts: read back"), AlertedBack.IsValid() && AlertedBack->Alerts == Alerted->Alerts);
	TestTrue(TEXT("alerts: in the copied report"), Alerted->ReportMarkdown().Contains(TEXT("> ") + Alert));
	{
		// The copied report's findings, as Unity writes them: the agent's first, then the engine's with severity and detector
		FAutopilotRun Found;
		Found.AgentNotes.Add(TEXT("The shop door stays shut"));
		const TSharedRef<FAutopilotFinding> Engine = MakeShared<FAutopilotFinding>();
		Engine->Source = TEXT("engine");
		Engine->Severity = TEXT("error");
		Engine->Detector = TEXT("log_error");
		Engine->Summary = TEXT("Accessed None");
		Engine->Count = 3;
		Engine->bDismissed = true;
		const TSharedRef<FAutopilotFinding> Agent = MakeShared<FAutopilotFinding>();
		Agent->Source = TEXT("agent");
		Agent->Summary = TEXT("The orb falls through the floor");
		Found.Findings.Add(Engine);
		Found.Findings.Add(Agent);
		const FString Report = Found.ReportMarkdown();
		TestTrue(TEXT("report: an agent note"), Report.Contains(TEXT("- [agent] The shop door stays shut\n")));
		TestTrue(TEXT("report: an agent finding, with the agent's"), Report.Contains(TEXT("- [agent] The orb falls through the floor\n"))
			&& Report.Find(TEXT("[agent] The orb")) < Report.Find(TEXT("[engine")));
		TestTrue(TEXT("report: an engine finding"), Report.Contains(TEXT("- [engine · error · log_error] Accessed None (×3) (not a bug)\n")));
	}
	TestTrue(TEXT("no alerts: none"), Older1->Alerts.IsEmpty());
	TestFalse(TEXT("no alerts: none saved"), FAutopilotSession::RunToJson(*Older1)->HasField(TEXT("alerts")));
	TestTrue(TEXT("old run: no alerts"), Old.IsValid() && Old->Alerts.IsEmpty());

	Session.Runs.SetNum(Before);
	++Session.UsageRevision;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotBackgroundTest, "AutopilotPlaytest.Editor.Background",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotBackgroundTest::RunTest(const FString& Parameters)
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid() || Session.LastPlayed >= 0.0)
	{
		AddWarning(TEXT("A run is going in this editor: the background test is skipped"));
		return true;
	}
	// The engine asks the session every frame whether the editor must keep its pace
	TestTrue(TEXT("asked by the engine"), GEditor && GEditor->ShouldDisableCPUThrottlingDelegates.ContainsByPredicate(
		[&Session](const UEditorEngine::FShouldDisableCPUThrottling& D) { return D.GetHandle() == Session.ThrottleHandle; }));
	const bool bUser = GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
	const int32 Before = Session.Runs.Num();
	TestFalse(TEXT("no run: the editor may slow down"), Session.KeepsPlaying());

	TSharedPtr<FAutopilotRun> Live = OtherRun(987900, false);
	Live->bDone = false;
	Session.Runs.Add(Live);
	TestTrue(TEXT("a run: it keeps its pace"), Session.KeepsPlaying());
	TestTrue(TEXT("a run, asked again: still"), Session.KeepsPlaying());

	// The run ends: still a while (a reconnect is not the end), then the editor may slow down again
	Live->bDone = true;
	TestTrue(TEXT("just ended: still"), Session.KeepsPlaying());
	Session.LastPlayed -= 3600.0;
	TestFalse(TEXT("a while after: it may slow down"), Session.KeepsPlaying());
	TestFalse(TEXT("a while after, asked again: still may"), Session.KeepsPlaying());
	TestEqual(TEXT("the user's preference untouched"), GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground,
		bUser);
	Session.Runs.SetNum(Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotSteerTest, "AutopilotPlaytest.Editor.Steer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotSteerTest::RunTest(const FString& Parameters)
{
	// The rule: a run takes the user's words only while it plays (its Rollout)
	struct FStageCase { EAutopilotStage Stage; const TCHAR* Name; bool bSteers; };
	const FStageCase Stages[] = { { EAutopilotStage::None, TEXT("none yet"), false }, { EAutopilotStage::Launch, TEXT("Launch"), false },
		{ EAutopilotStage::Analyze, TEXT("Analyze"), false }, { EAutopilotStage::Probe, TEXT("Probe"), false },
		{ EAutopilotStage::Build, TEXT("Build"), false }, { EAutopilotStage::Rollout, TEXT("Rollout"), true },
		{ EAutopilotStage::Results, TEXT("Process result"), false }, { EAutopilotStage::Report, TEXT("Report"), false } };
	for (const FStageCase& Case : Stages)
	{
		FAutopilotRun Run;
		Run.Stage = Case.Stage;
		TestEqual(*FString::Printf(TEXT("going, %s"), Case.Name), FAutopilotSession::SteersAt(Run), Case.bSteers);
		Run.bDone = true;
		TestFalse(*FString::Printf(TEXT("done, %s"), Case.Name), FAutopilotSession::SteersAt(Run));
	}

	// A run's status lines, one after another: open from "Trying the harness (run N)" (and its "Now: ..." lines) to the next stage
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going in this editor: the status line part of the steer test is skipped"));
		return true;
	}
	const int32 Before = Session.Runs.Num();
	TSharedPtr<FAutopilotRun> Live = OtherRun(987900, false);
	Live->bDone = false;
	Live->Stage = EAutopilotStage::None;
	Live->Feed.Reset();
	Session.Runs.Add(Live);  // (no history file: never saved)
	struct FLineCase { const TCHAR* Line; bool bSteers; };
	const FLineCase Lines[] = { { TEXT("Starting the game"), false }, { TEXT("Reading the game's code"), false },
		{ TEXT("Trying things in the game"), false }, { TEXT("Writing the harness"), false },
		{ TEXT("Trying the harness (run 1)"), true }, { TEXT("Now: walking to the gate"), true },
		{ TEXT("Looking at the results"), false }, { TEXT("Now: a late action"), false },
		{ TEXT("Trying the harness (run 2)"), true }, { TEXT("Now: jumping over the fence"), true },
		{ TEXT("Looking at the results"), false }, { TEXT("Saving what it learned to memory"), false },
		{ TEXT("Trying the harness (run 3)"), false } };
	for (const FLineCase& Case : Lines)
	{
		Session.OnStatus(Case.Line);
		TestEqual(*FString::Printf(TEXT("after \"%s\""), Case.Line), FAutopilotSession::SteersAt(*Live), Case.bSteers);
		if (!Case.bSteers)
		{
			// (refused outside the Rollout, whatever the connection: nothing sent, nothing in the feed)
			const int32 Feed = Live->Feed.Num();
			TestFalse(*FString::Printf(TEXT("after \"%s\": SteerRun refused"), Case.Line), Session.SteerRun(TEXT("go left")));
			TestEqual(*FString::Printf(TEXT("after \"%s\": nothing in the feed"), Case.Line), Live->Feed.Num(), Feed);
		}
	}
	// "Now: ..." before any stage: playing
	TSharedPtr<FAutopilotRun> Early = OtherRun(987901, false);
	Early->bDone = false;
	Early->Stage = EAutopilotStage::None;
	Live->bDone = true;
	Session.Runs.Add(Early);
	Session.OnStatus(TEXT("Now: looking around"));
	TestTrue(TEXT("a Now line before any stage: playing"), FAutopilotSession::SteersAt(*Early));
	Early->bDone = true;
	TestFalse(TEXT("done: never"), FAutopilotSession::SteersAt(*Early));

	Session.Runs.SetNum(Before);
	return true;
}

namespace
{
	/** Whether a text saying What is under Widget with it and everything above it visible (attributes read, nothing laid out). */
	bool ShownText(const TSharedRef<SWidget>& Widget, const FString& What)
	{
		if (!Widget->GetVisibility().IsVisible())
		{
			return false;
		}
		if (Widget->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == What)
		{
			return true;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (ShownText(Children->GetChildAt(i), What))
			{
				return true;
			}
		}
		return false;
	}

	/** The button under Widget tagged Tag (SWidget::SetTag; null when none). */
	TSharedPtr<SButton> FindTagged(const TSharedRef<SWidget>& Widget, const FName& Tag)
	{
		if (Widget->GetType() == FName(TEXT("SButton")) && Widget->GetTag() == Tag)
		{
			return StaticCastSharedRef<SButton>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (TSharedPtr<SButton> Found = FindTagged(Children->GetChildAt(i), Tag))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** The button under Widget whose words are exactly Words (null when none). */
	TSharedPtr<SButton> FindButton(const TSharedRef<SWidget>& Widget, const FString& Words)
	{
		if (Widget->GetType() == FName(TEXT("SButton")) && AllWords(Widget) == Words)
		{
			return StaticCastSharedRef<SButton>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (TSharedPtr<SButton> Found = FindButton(Children->GetChildAt(i), Words))
			{
				return Found;
			}
		}
		return nullptr;
	}
}

// The setup's Aura card, the Aura app installed or not: Create Account first (the primary), then Sign in. Explore's
// minutes: 10 unless set. Step 4, First run: Next on step 3 and the stepper's 4th step unless the game's memory holds
// something; waiting for the connection and its memory_state (Skip); an Explore run offered when the memory is empty
// (Explore and finish sends what the composer would for "Explore", Skip and finish); closed by itself when the memory holds
// something (AutopilotPlaytest.Editor.FirstRun).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotFirstRunTest, "AutopilotPlaytest.Editor.FirstRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotFirstRunTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the First run test is skipped"));
		return true;
	}
	using EMemoryView = SPlaytestPanel::EMemoryView;
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->Prefs.bOnboarded = true;
	Panel->TestRunning = false;

	// Explore's minutes
	TestEqual(TEXT("Explore's default minutes"), GetDefault<UPlaytestSettings>()->DefaultExploreMinutes, 10);
	TestEqual(TEXT("the tab starts with them"), Panel->Minutes, GetDefault<UPlaytestSettings>()->DefaultExploreMinutes);
	// The time limit: the composer's starts with the one last set in it
	TestEqual(TEXT("the default time limit"), GetDefault<UPlaytestSettings>()->RunTimeLimitMinutes, 30);
	TestEqual(TEXT("the composer starts with it"), Panel->TimeLimit, GetDefault<UPlaytestSettings>()->RunTimeLimitMinutes);

	// The Aura card
	for (const bool bApp : { true, false })
	{
		Panel->TestAuraApp = bApp;
		TArray<FString> Words;
		AllTexts(Panel->MakeAuraCard(), Words);
		const int32 Create = Words.Find(TEXT("Create Account"));
		const int32 SignIn = Words.Find(TEXT("Sign in"));
		TestTrue(TEXT("both buttons"), Create != INDEX_NONE && SignIn != INDEX_NONE);
		TestTrue(TEXT("Create Account first"), Create < SignIn);
		TestFalse(TEXT("no Use Aura app"), Words.Contains(TEXT("Use Aura app")));
	}
	Panel->TestAuraApp.Reset();
	// Create Account's next page: finish in the browser, then Sign in (only flag set: no browser opened from a test)
	Panel->bCreatingAccount = true;
	{
		TArray<FString> Words;
		AllTexts(Panel->MakeAuraCard(), Words);
		TestTrue(TEXT("creating: Finish creating your Aura account"), Words.Contains(TEXT("Finish creating your Aura account")));
		TestTrue(TEXT("creating: then sign in"), Words.Contains(TEXT("Once you have created your account, sign in and start using Autopilot")));
		TestTrue(TEXT("creating: Sign in"), Words.Contains(TEXT("Sign in")));
		TestFalse(TEXT("creating: no Create Account"), Words.Contains(TEXT("Create Account")));
	}
	Panel->bCreatingAccount = false;
	// The account creation page's server: autopilot.env's, else the download's settings.env, else Aura's
	{
		FScratchDir Service;
		TestEqual(TEXT("sign-up page: Aura's"), PlaytestAura::SignUpUrl(Dir.Path, Service.Path), FString(TEXT("https://app.tryaura.dev/auth/signup")));
		FFileHelper::SaveStringToFile(TEXT("AUTOPILOT_AURA_SERVER=https://beta.example/\n"), *FPaths::Combine(Service.Path, TEXT("settings.env")));
		TestEqual(TEXT("sign-up page: the download's"), PlaytestAura::SignUpUrl(Dir.Path, Service.Path), FString(TEXT("https://beta.example/auth/signup")));
		FFileHelper::SaveStringToFile(TEXT("AUTOPILOT_AURA_SERVER=https://own.example\n"), *FPaths::Combine(Dir.Path, TEXT("autopilot.env")));
		TestEqual(TEXT("sign-up page: autopilot.env's"), PlaytestAura::SignUpUrl(Dir.Path, Service.Path), FString(TEXT("https://own.example/auth/signup")));
		IFileManager::Get().Delete(*FPaths::Combine(Dir.Path, TEXT("autopilot.env")));
	}

	// The keys card while the sign-in goes on: not shown (Cancel brings it back); a sign-in that needs Try again: shown.
	// (Only the flag is set: no sign-in is sent from a test)
	{
		const TSharedRef<SWidget> Keys = Panel->MakeKeysCard();
		auto KeysShown = [&Keys]()
		{
			Keys->SlatePrepass(1.f);
			return Keys->GetVisibility() != EVisibility::Collapsed && AllWords(Keys).Contains(TEXT("Bring your own keys (BYOK)"));
		};
		TestTrue(TEXT("keys card: shown before the sign-in"), KeysShown());
		Panel->bSignInWanted = true;
		const SPlaytestPanel::ESignInView View = Panel->SignInView();
		const bool bGoing = View == SPlaytestPanel::ESignInView::Preparing || View == SPlaytestPanel::ESignInView::Waiting;
		AddInfo(FString::Printf(TEXT("the sign-in's view here: %d"), (int32)View));
		TestEqual(TEXT("keys card: hidden while the sign-in goes on (shown when it needs Try again)"), KeysShown(), !bGoing);
		Panel->bSignInWanted = false;  // (Cancel's part that matters here)
		TestTrue(TEXT("keys card: back after Cancel"), KeysShown());
	}

	// Step 3: Next (and the stepper's 4th step) unless the memory holds something
	TSharedRef<SWidget> Setup = Panel->MakeSetup();
	// (what the setup shows, its words and which step, is read when it is laid out)
	auto LaidOut = [&Setup]()
	{
		Setup->SlatePrepass(1.f);
		Setup->SlatePrepass(1.f);
	};
	Panel->bSetupOpen = true;
	Panel->SetupStep = SPlaytestPanel::StepPrivacy;
	struct FCase { const TCHAR* What; EMemoryView Memory; bool bFirstRun; };
	const FCase Cases[] = { { TEXT("memory not known"), EMemoryView::Unknown, true }, { TEXT("memory empty"), EMemoryView::Empty, true },
		{ TEXT("memory holds something"), EMemoryView::Something, false } };
	for (const FCase& Case : Cases)
	{
		Panel->TestMemory = Case.Memory;
		LaidOut();
		TArray<FString> Words;
		AllTexts(Setup, Words);
		TestEqual(*FString::Printf(TEXT("%s: Next"), Case.What), Words.Contains(TEXT("Next")), Case.bFirstRun);
		TestEqual(*FString::Printf(TEXT("%s: Finish"), Case.What), Words.Contains(TEXT("Finish")), !Case.bFirstRun);
		TestEqual(*FString::Printf(TEXT("%s: the stepper's 4 FIRST RUN"), Case.What), ShownText(Setup, TEXT("4 FIRST RUN")), Case.bFirstRun);
	}

	// Step 4: waiting for the connection, then for its memory_state; Skip
	Panel->SetupStep = SPlaytestPanel::StepFirstRun;
	Panel->TestMemory = EMemoryView::Unknown;
	Panel->TestConnected = false;
	LaidOut();
	TestTrue(TEXT("step 4: its title"), ShownText(Setup, TEXT("Let Autopilot learn your game")));
	TestTrue(TEXT("step 4, not connected: says so"), ShownText(Setup, TEXT("Connecting to the agent service…")));
	TestFalse(TEXT("step 4, not connected: no Explore yet"), ShownText(Setup, TEXT("Explore and finish")));
	Panel->TestConnected = true;
	LaidOut();
	TestTrue(TEXT("step 4, connected: checking the memory"), ShownText(Setup, TEXT("Checking what Autopilot remembers about this game…")));
	TestFalse(TEXT("step 4, memory not known: no Explore yet"), ShownText(Setup, TEXT("Explore and finish")));
	TSharedPtr<SButton> Skip = FindButton(Setup, TEXT("Skip"));
	TestTrue(TEXT("step 4, waiting: Skip"), Skip.IsValid());
	if (Skip.IsValid())
	{
		Skip->SimulateClick();
		TestFalse(TEXT("Skip: the setup closes"), Panel->IsSetupShown());
	}

	// Step 4, the memory empty: the Explore run offered
	Panel->bSetupOpen = true;
	Panel->SetupStep = SPlaytestPanel::StepFirstRun;
	Panel->TestMemory = EMemoryView::Empty;
	LaidOut();
	for (const TCHAR* Shown : { TEXT("Explore your game"), TEXT("Recommended"), TEXT("Explore and finish"), TEXT("Skip and finish"), TEXT("Minutes") })
	{
		TestTrue(*FString::Printf(TEXT("step 4, memory empty: %s"), Shown), ShownText(Setup, Shown));
	}
	TestFalse(TEXT("step 4, memory empty: not waiting"), ShownText(Setup, TEXT("Checking what Autopilot remembers about this game…")));
	TSharedPtr<SButton> Explore = FindButton(Setup, TEXT("Explore and finish"));
	TSharedPtr<SButton> SkipFinish = FindButton(Setup, TEXT("Skip and finish"));
	TestTrue(TEXT("step 4: its buttons"), Explore.IsValid() && SkipFinish.IsValid());
	if (Explore.IsValid() && SkipFinish.IsValid())
	{
		Panel->bTestStarts = true;
		// ... not while a run goes
		Panel->TestRunning = true;
		Explore->SimulateClick();
		TestFalse(TEXT("a run going: nothing started"), Panel->TestStartedFrame.IsValid());
		TestTrue(TEXT("a run going: the setup stays"), Panel->IsSetupShown());
		Panel->TestRunning = false;
		// ... Explore and finish: the composer's start frame for "Explore" with the minutes, and the setup closed
		Explore->SimulateClick();
		TestTrue(TEXT("Explore and finish: started"), Panel->TestStartedFrame.IsValid());
		if (Panel->TestStartedFrame.IsValid())
		{
			TestEqual(TEXT("Explore and finish: kind"), Panel->TestStartedFrame->GetStringField(TEXT("kind")), FString(TEXT("explore")));
			TestEqual(TEXT("Explore and finish: minutes"), (int32)Panel->TestStartedFrame->GetNumberField(TEXT("minutes")), 10);
		}
		TestFalse(TEXT("Explore and finish: the setup closes"), Panel->IsSetupShown());
		// ... Skip and finish
		Panel->bSetupOpen = true;
		Panel->SetupStep = SPlaytestPanel::StepFirstRun;
		Panel->TestStartedFrame.Reset();
		SkipFinish->SimulateClick();
		TestFalse(TEXT("Skip and finish: the setup closes"), Panel->IsSetupShown());
		TestFalse(TEXT("Skip and finish: nothing started"), Panel->TestStartedFrame.IsValid());
		Panel->bTestStarts = false;
	}

	// The memory holds something: step 4 closes by itself
	Panel->bSetupOpen = true;
	Panel->SetupStep = SPlaytestPanel::StepFirstRun;
	Panel->TestMemory = EMemoryView::Something;
	Panel->Tick(FGeometry::MakeRoot(FVector2D(900.f, 700.f), FSlateLayoutTransform()), 0.0, 0.016f);
	TestFalse(TEXT("memory holds something: step 4 skipped"), Panel->IsSetupShown());

	Panel->TestMemory.Reset();
	Panel->TestConnected.Reset();
	return true;
}

// The connect card: no Connect to press (the tab connects by itself once set up), "Agent service", not connected, how it
// starts (autopilot serve, or the Aura app), each state's words and buttons (Disconnected: Reconnect), Advanced; no
// Aura line, no Export project index (AutopilotPlaytest.Editor.Connect).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotConnectTest, "AutopilotPlaytest.Editor.Connect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotConnectTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the connect card test is skipped"));
		return true;
	}
	using EConnectView = SPlaytestPanel::EConnectView;
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	TSharedRef<SWidget> Card = Panel->MakeConnectCard();
	struct FCase { EConnectView View; const TCHAR* Name; const TCHAR* Says; const TCHAR* Buttons[3]; };
	const FCase Cases[] = {
		{ EConnectView::Connecting, TEXT("connecting"), TEXT("Connecting to the agent service…"), { TEXT("Stop") } },
		{ EConnectView::Disconnected, TEXT("disconnected"), nullptr, { TEXT("Reconnect") } },
		{ EConnectView::Starting, TEXT("starting"), TEXT("Starting the agent service…"), { TEXT("Stop") } },
		{ EConnectView::Installing, TEXT("installing"), TEXT("Installing the agent service…"), { TEXT("Stop") } },
		{ EConnectView::NotInstalled, TEXT("not installed"), TEXT("The agent service isn\u2019t installed on this computer."), { TEXT("Install"), TEXT("Stop") } },
		{ EConnectView::NoInstaller, TEXT("no installer"), TEXT("Set Agent service command under Advanced, or run autopilot serve."), { TEXT("Try again"), TEXT("Stop") } },
		{ EConnectView::Unreachable, TEXT("unreachable"), TEXT("Can\u2019t reach the agent service, retrying"), { TEXT("Try now"), TEXT("Stop") } },
		{ EConnectView::Failed, TEXT("failed"), nullptr, { TEXT("Try again"), TEXT("Stop") } },
		{ EConnectView::Refused, TEXT("refused"), nullptr, { TEXT("Try again") } },
		{ EConnectView::Aura, TEXT("Aura not running"), nullptr, { TEXT("Try now"), TEXT("Stop") } } };
	UPlaytestSettings* Settings = GetMutableDefault<UPlaytestSettings>();
	const bool bWasAura = Settings->bUseWithAura;
	for (const FCase& Case : Cases)
	{
		Settings->bUseWithAura = Case.View == EConnectView::Aura;  // (not saved: put back below)
		Panel->TestConnectView = Case.View;
		Panel->RebuildConnect();
		const FString Words = FString(TEXT("| ")) + Shown(Card, 520.f, 800.f) + TEXT(" |");
		TestTrue(*FString::Printf(TEXT("%s: Agent service, not connected"), Case.Name), Words.Contains(TEXT("| Agent service |")) && Words.Contains(TEXT("| Not connected |")));
		if (Case.View == EConnectView::Aura)
		{
			TestTrue(TEXT("Aura: the Aura app runs it"), Words.Contains(TEXT("Start the Aura app: it runs the agent service.")));
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: start it with autopilot serve"), Case.Name),
				Words.Contains(TEXT("| Start the agent service with |")) && Words.Contains(TEXT("| autopilot serve |")));
		}
		if (Case.Says)
		{
			TestTrue(*FString::Printf(TEXT("%s: says %s"), Case.Name, Case.Says), Words.Contains(Case.Says));
		}
		for (const TCHAR* Button : Case.Buttons)
		{
			if (Button)
			{
				TestTrue(*FString::Printf(TEXT("%s: %s"), Case.Name, Button), FindButton(Card, Button).IsValid());
			}
		}
		TestTrue(*FString::Printf(TEXT("%s: Advanced"), Case.Name), FindButton(Card, TEXT("Advanced")).IsValid());
		TestFalse(*FString::Printf(TEXT("%s: no Connect to press"), Case.Name), FindButton(Card, TEXT("Connect")).IsValid());
		for (const TCHAR* Gone : { TEXT("Use with Aura"), TEXT("Get Aura"), TEXT("Export project index"), TEXT("Open log"), TEXT("Editor Preferences") })
		{
			TestFalse(*FString::Printf(TEXT("%s: no %s"), Case.Name, Gone), Words.Contains(Gone));
		}
	}
	Settings->bUseWithAura = bWasAura;
	Panel->TestConnectView.Reset();
	return true;
}

// The status line: what just happened, the main column's last row (the copies, why something was refused), for 8 s; the
// Feed tab's steer box saying the same words as the Live page's; Back to the last run on the connect card when a run was
// shown before (AutopilotPlaytest.Editor.StatusLine).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotStatusLineTest, "AutopilotPlaytest.Editor.StatusLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotStatusLineTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the status line test is skipped"));
		return true;
	}
	FAutopilotSession& Session = FAutopilotSession::Get();
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->Prefs.bOnboarded = true;

	// Said, and gone after 8 s
	Session.Say(TEXT("Copied"));
	TestEqual(TEXT("said"), Session.StatusLine(), FString(TEXT("Copied")));
	Session.StatusAt -= 9.0;
	TestTrue(TEXT("gone after 8 s"), Session.StatusLine().IsEmpty());
	// (the copies and Run again aren't pressed here: they would take the user's clipboard, or start a run on an agent
	// service this editor may be connected to)
	const TSharedPtr<FAutopilotRun> Run = OtherRun(988000, false);
	Run->bDone = true;
	Run->Verdict = TEXT("GOAL HIT");
	// The line itself: shown with words, nothing without
	const TSharedRef<SWidget> Line = Panel->MakeStatusLine();
	Session.Say(TEXT("Showing BP_IronDoor_C_2"));
	Line->SlatePrepass(1.f);
	TestTrue(TEXT("the line shown"), Line->GetVisibility() == EVisibility::Visible && Shown(Line, 400.f, 40.f).Contains(TEXT("Showing BP_IronDoor_C_2")));
	Session.StatusAt -= 9.0;
	Line->SlatePrepass(1.f);
	TestTrue(TEXT("the line hidden"), Line->GetVisibility() == EVisibility::Collapsed);

	// The steer boxes: one set of words
	Panel->MakeLive();
	Panel->MakeSteerBar(true);
	TestTrue(TEXT("both steer boxes"), Panel->SteerBox.IsValid() && Panel->FeedSteerBox.IsValid());
	if (Panel->SteerBox.IsValid() && Panel->FeedSteerBox.IsValid())
	{
		Panel->FeedSteerBox->SetText(FText::FromString(TEXT("go left")));
		TestEqual(TEXT("the words, whichever box"), Panel->SteerText(), FString(TEXT("go left")));
		TestEqual(TEXT("the steer hint, at any stage"), Panel->FeedSteerBox->GetHintText().ToString(), FString(TEXT("Send a steering prompt to Autopilot")));
	}

	// Back to the last run: the connect card, disconnected, after a run was shown
	Session.Runs.Add(Run);  // (no history file: never saved)
	Panel->SelectedId = Run->Id;
	Panel->Select(nullptr);
	TestEqual(TEXT("the run shown before"), Panel->PreviousRunId, Run->Id);
	TSharedRef<SWidget> Card = Panel->MakeConnectCard();
	Panel->TestConnectView = SPlaytestPanel::EConnectView::Disconnected;
	Panel->RebuildConnect();
	TSharedPtr<SButton> Back = FindButton(Card, TEXT("Back to the last run"));
	TestTrue(TEXT("Back to the last run"), Back.IsValid());
	if (Back.IsValid())
	{
		Back->SimulateClick();
		TestEqual(TEXT("Back to the last run: that run"), Panel->SelectedId, Run->Id);
	}
	Panel->TestConnectView = SPlaytestPanel::EConnectView::Connecting;
	Panel->RebuildConnect();
	TestFalse(TEXT("not while connecting"), FindButton(Card, TEXT("Back to the last run")).IsValid());
	Panel->TestConnectView.Reset();
	Session.Runs.Remove(Run);
	Session.StatusAt = -1.0;
	return true;
}

namespace
{
	/** Every button under Widget whose words are Words, in order. */
	void FindButtons(const TSharedRef<SWidget>& Widget, const FString& Words, TArray<TSharedPtr<SButton>>& Out)
	{
		if (Widget->GetType() == FName(TEXT("SButton")) && AllWords(Widget) == Words)
		{
			Out.Add(StaticCastSharedRef<SButton>(Widget));
			return;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			FindButtons(Children->GetChildAt(i), Words, Out);
		}
	}
}

// A run the user's own Claude Code or Codex started (`run_started`): a run of the tab's, shown as one started from the
// prompt box is (from the Settings page too), its words the run's, with where it was started under them (Claude Code,
// Codex, any other coding agent), live and once done, and kept with it in the history; one while a run goes is not the
// tab's; the first-run setup, when open, stays. The Settings page's card "Autopilot in Claude Code & Codex" (after
// Data sharing, before Uninstall): its words, each command with Copy; and what sharing sends says what the agent traces
// are for those runs (AutopilotPlaytest.Editor.RunStarted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotRunStartedTest, "AutopilotPlaytest.Editor.RunStarted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotRunStartedTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the run started test is skipped"));
		return true;
	}
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going in this editor: the run started test is skipped"));
		return true;
	}
	const int32 RunsBefore = Session.Runs.Num();
	const int32 AgentRunBefore = Session.AgentStartedRunId;
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->Prefs.bOnboarded = true;
	Panel->bOpenVideos = false;
	TArray<TSharedPtr<FAutopilotRun>> Made;
	// The runs this test made: never saved, gone from the session after it
	auto Forget = [&Session, &Made]()
	{
		for (const TSharedPtr<FAutopilotRun>& Run : Made)
		{
			Session.Runs.Remove(Run);
			Session.Unsaved.Remove(Run->Id);
		}
		Made.Reset();
	};
	auto Started = [&Session, &Made](const TCHAR* Json) -> TSharedPtr<FAutopilotRun>
	{
		const int32 Before = Session.GetAgentStartedRunId();
		Session.OnRunStarted(PlaytestJson::Parse(Json));
		if (Session.GetAgentStartedRunId() == Before)
		{
			return nullptr;
		}
		TSharedPtr<FAutopilotRun> Run = Session.FindRun(Session.GetAgentStartedRunId());
		if (Run.IsValid())
		{
			Run->HistoryFile.Empty();  // (never saved)
			Made.Add(Run);
		}
		return Run;
	};

	struct FCase { const TCHAR* What; const TCHAR* Json; const TCHAR* Kind; const TCHAR* Words; const TCHAR* Origin; };
	const FCase Cases[] = {
		{ TEXT("Claude Code"), TEXT("{\"kind\": \"verify\", \"claim\": \"Verify that the door opens with the red key\", \"time_limit_minutes\": 15, \"by\": \"claude-code\"}"),
			TEXT("verify"), TEXT("Verify that the door opens with the red key"), TEXT("Started from Claude Code") },
		{ TEXT("Codex"), TEXT("{\"kind\": \"goal\", \"goal\": \"Light any one lantern\", \"by\": \"codex\"}"),
			TEXT("goal"), TEXT("Light any one lantern"), TEXT("Started from Codex") },
		{ TEXT("another agent"), TEXT("{\"kind\": \"explore\", \"brief\": \"the market stall\", \"minutes\": 5, \"by\": \"opencode\"}"),
			TEXT("explore"), TEXT("Explore: the market stall"), TEXT("Started from your coding agent") },
	};
	for (const FCase& Case : Cases)
	{
		// (shown wherever the tab was: here the Settings page)
		Panel->bSettingsPage = true;
		Panel->SelectedId = INDEX_NONE;
		TSharedPtr<FAutopilotRun> Run = Started(Case.Json);
		TestTrue(*FString::Printf(TEXT("%s: a run of the tab's"), Case.What), Run.IsValid());
		if (!Run.IsValid())
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: going"), Case.What), !Run->bDone && Session.GetActiveRun() == Run);
		TestEqual(*FString::Printf(TEXT("%s: its kind"), Case.What), Run->Kind, FString(Case.Kind));
		TestEqual(*FString::Printf(TEXT("%s: its words"), Case.What), Run->ShownPrompt(), FString(Case.Words));
		TestEqual(*FString::Printf(TEXT("%s: where it was started"), Case.What), Run->OriginLine(), FString(Case.Origin));
		TestTrue(*FString::Printf(TEXT("%s: Run again sends it as a start frame, without who started it"), Case.What),
			Run->Frame.IsValid() && !Run->Frame->HasField(TEXT("by")) && Run->Frame->GetStringField(TEXT("kind")) == Case.Kind);
		TestTrue(*FString::Printf(TEXT("%s: its feed says it started"), Case.What), Run->Feed.Num() > 0 && Run->Feed[0]->Text == TEXT("Run started"));
		Panel->SyncRuns();
		TestEqual(*FString::Printf(TEXT("%s: shown"), Case.What), Panel->SelectedId, Run->Id);
		TestFalse(*FString::Printf(TEXT("%s: the Settings page left for it"), Case.What), Panel->bSettingsPage);
		TestEqual(*FString::Printf(TEXT("%s: live"), Case.What), Panel->ViewIndex(), (int32)SPlaytestPanel::ViewLive);
		const FString Live = Shown(Panel->MakeLive(), 900.f, 600.f);
		TestTrue(*FString::Printf(TEXT("%s: live, its words and where it was started"), Case.What), Live.Contains(Case.Words) && Live.Contains(Case.Origin));

		// Another while it goes: not the tab's (the agent service runs one at a time)
		const int32 Count = Session.Runs.Num();
		TestFalse(*FString::Printf(TEXT("%s: another while it goes: nothing"), Case.What),
			Started(TEXT("{\"kind\": \"goal\", \"goal\": \"Something else\", \"by\": \"codex\"}")).IsValid());
		TestEqual(*FString::Printf(TEXT("%s: no run added"), Case.What), Session.Runs.Num(), Count);

		// Done: the result says it too, and the history keeps it
		Run->bDone = true;
		Run->EndSeconds = FPlatformTime::Seconds();
		Run->Verdict = TEXT("CONFIRMED");
		Run->Reason = TEXT("The door opened.");
		Panel->RebuildResult(Run);
		const FString Result = Shown(Panel->ResultBox.ToSharedRef(), 900.f, 1600.f);
		TestTrue(*FString::Printf(TEXT("%s: the result, where it was started under its words"), Case.What),
			Result.Contains(FString(Case.Words) + TEXT(" | ") + Case.Origin));
		const TSharedPtr<FAutopilotRun> Saved = FAutopilotSession::RunFromJson(FAutopilotSession::RunToJson(*Run));
		TestTrue(*FString::Printf(TEXT("%s: kept in the history"), Case.What), Saved.IsValid() && Saved->OriginLine() == Case.Origin);
	}

	// A run started here says nothing of where; a frame with no `by` is a coding agent's all the same
	TestEqual(TEXT("a run started here: no line"), OtherRun(987900, false)->OriginLine(), FString());
	TSharedPtr<FAutopilotRun> NoBy = Started(TEXT("{\"kind\": \"verify\", \"claim\": \"Verify that it rains\"}"));
	TestTrue(TEXT("no by: your coding agent"), NoBy.IsValid() && NoBy->OriginLine() == TEXT("Started from your coding agent"));
	if (NoBy.IsValid())
	{
		NoBy->bDone = true;
	}

	// The first-run setup, open: it stays (the run is shown once it closes)
	Panel->Prefs.bOnboarded = false;
	Panel->SetupStep = SPlaytestPanel::StepPrivacy;
	TSharedPtr<FAutopilotRun> UnderSetup = Started(TEXT("{\"kind\": \"goal\", \"goal\": \"Reach the gate\", \"by\": \"claude-code\"}"));
	Panel->SyncRuns();
	TestTrue(TEXT("setup open: the run started"), UnderSetup.IsValid());
	TestTrue(TEXT("setup open: still shown, at its step"), Panel->IsSetupShown() && Panel->SetupStep == SPlaytestPanel::StepPrivacy);
	TestEqual(TEXT("setup open: the run is the one under it"), Panel->SelectedId, UnderSetup.IsValid() ? UnderSetup->Id : -1);
	if (UnderSetup.IsValid())
	{
		UnderSetup->bDone = true;
	}
	Panel->Prefs.bOnboarded = true;
	Forget();
	TestEqual(TEXT("the session's runs as they were"), Session.Runs.Num(), RunsBefore);
	Session.AgentStartedRunId = AgentRunBefore;

	// The Settings page: the card after Data sharing and the Research agent, before Uninstall
	TSharedRef<SWidget> SettingsPage = Panel->MakeSettings();
	Panel->RebuildSettings();
	const FString Words = Shown(SettingsPage, 600.f, 3000.f);
	const int32 Share = Words.Find(TEXT("Share runs to improve Autopilot"));
	const int32 Research = Words.Find(TEXT("Research agent"), ESearchCase::CaseSensitive);
	const int32 Title = Words.Find(TEXT("Autopilot in Claude Code & Codex"), ESearchCase::IgnoreCase);
	const int32 Uninstall = Words.Find(TEXT("Uninstall Autopilot"));
	TestTrue(TEXT("settings: Data sharing, then the Research agent"), Share != INDEX_NONE && Research > Share);
	TestTrue(TEXT("settings: the card, after the Research agent and before Uninstall"), Title > Research && Uninstall > Title);
	TestTrue(TEXT("settings: what the research agent does, and its cost"), Words.Contains(TEXT("Enabling the research agent allows Autopilot to achieve more difficult goals with fewer attempts, but at a higher cost.")));
	TestTrue(TEXT("settings: what sharing sends, and for these runs"), Words.Contains(TEXT("Enabling this sends recordings, screenshots, and agent traces and memories to the developers of Autopilot. For runs from your own Claude Code or Codex, the agent traces are its conversation during the run.")));
	const TCHAR* CardWords[] = {
		TEXT("One-time CLI setup to run Autopilot through Claude Code or Codex using your Claude or ChatGPT subscriptions, respectively."),
		TEXT("autopilot connect claude"),
		TEXT("autopilot connect codex"),
	};
	int32 After = Title;
	for (const TCHAR* Line : CardWords)
	{
		const int32 At = Words.Find(Line, ESearchCase::CaseSensitive, ESearchDir::FromStart, FMath::Max(0, After));
		TestTrue(*FString::Printf(TEXT("settings: \"%s\", in order"), Line), At != INDEX_NONE && At > After && At < Uninstall);
		After = At;
	}
	TestFalse(TEXT("settings: nothing under the commands"), Words.Contains(TEXT("Then ask it")) || Words.Contains(TEXT("Runs still use")));
	// Each command's Copy copies it (here not to the clipboard: the user's is left alone)
	const TSharedRef<SWidget> Card = Panel->MakeCodingAgents();
	Shown(Card, 600.f, 600.f);
	TArray<TSharedPtr<SButton>> Copies;
	FindButtons(Card, TEXT("Copy"), Copies);
	TestEqual(TEXT("card: a Copy for each command"), Copies.Num(), 2);
	TArray<FString> Copied;
	Panel->TestCopied = [&Copied](const FString& Text) { Copied.Add(Text); };
	for (const TSharedPtr<SButton>& Copy : Copies)
	{
		Copy->SimulateClick();
	}
	Panel->TestCopied = nullptr;
	TestEqual(TEXT("card: Copy, the command beside it"), FString::Join(Copied, TEXT(" / ")), FString(TEXT("autopilot connect claude / autopilot connect codex")));
	return true;
}

namespace
{
	/** The first widget of Type under Widget (itself included), depth first; nullptr when none. */
	TSharedPtr<SWidget> FirstOfType(const TSharedRef<SWidget>& Widget, const TCHAR* Type)
	{
		if (Widget->GetType() == FName(Type))
		{
			return Widget;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (TSharedPtr<SWidget> Found = FirstOfType(Children->GetChildAt(i), Type))
			{
				return Found;
			}
		}
		return nullptr;
	}

	bool SameFont(const FSlateFontInfo& A, const FSlateFontInfo& B)
	{
		// (Size is an int32 before Unreal 5.3)
		return A.FontObject == B.FontObject && A.TypefaceFontName == B.TypefaceFontName && FMath::IsNearlyEqual(float(A.Size), float(B.Size))
			&& A.LetterSpacing == B.LetterSpacing;
	}
}

// The Settings page's sharing, research agent and Claude Code & Codex plates are built alike: the name (the mono
// micro-label), what it does (in the sharing text's font: Body 12), then what changes it (the switch alone, its name
// the tooltip; the two commands with Copy), 6 px under the name and 10 px under the words
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotSettingCardsTest, "AutopilotPlaytest.Editor.SettingCards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotSettingCardsTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("No Slate here: the SettingCards test is skipped"));
		return true;
	}
	FScratchDir Dir;
	TSharedRef<SPlaytestPanel> Panel = SNew(SPlaytestPanel).KeysDir(Dir.Path);
	Panel->Prefs.bOnboarded = true;
	Panel->Prefs.bShareData = false;
	Panel->Prefs.ResearchChoice = false;
	struct FCase
	{
		const TCHAR* Name;
		TSharedRef<SWidget> Card;
		FString Title;
		FString Subtitle;
		FString Control;  // (the control's words, " | " between)
	};
	FCase Cases[] = {
		{ TEXT("sharing"), Panel->MakeDataSharing(), TEXT("Share runs to improve Autopilot"),
			TEXT("Enabling this sends recordings, screenshots, and agent traces and memories to the developers of Autopilot. For runs from your own Claude Code or Codex, the agent traces are its conversation during the run."),
			TEXT("Data sharing off") },
		{ TEXT("research agent"), Panel->MakeResearchAgent(), TEXT("Research agent"),
			TEXT("Enabling the research agent allows Autopilot to achieve more difficult goals with fewer attempts, but at a higher cost."),
			TEXT("Research agent off") },
		{ TEXT("Claude Code & Codex"), Panel->MakeCodingAgents(), TEXT("Autopilot in Claude Code & Codex"),
			TEXT("One-time CLI setup to run Autopilot through Claude Code or Codex using your Claude or ChatGPT subscriptions, respectively."),
			TEXT("autopilot connect claude | Copy | autopilot connect codex | Copy") },
	};
	const FSlateFontInfo TitleFont = FAutopilotStyle::Label();
	const FSlateFontInfo WordsFont = FAutopilotStyle::Font(EAutopilotFont::Body, 12.f);
	for (FCase& Case : Cases)
	{
		const FGeometry Root = FGeometry::MakeRoot(FVector2D(600.f, 600.f), FSlateLayoutTransform());
		Case.Card->SlatePrepass(1.f);
		Case.Card->SlatePrepass(1.f);
		const TSharedPtr<SWidget> Column = FirstOfType(Case.Card, TEXT("SVerticalBox"));
		FGeometry ColumnAt;
		if (!TestTrue(*FString::Printf(TEXT("%s: a column"), Case.Name), Column.IsValid() && Find(Case.Card, Root, Column.Get(), ColumnAt)))
		{
			continue;
		}
		FArrangedChildren Rows(EVisibility::Visible);
		Column->ArrangeChildren(ColumnAt, Rows, true);
		if (!TestEqual(*FString::Printf(TEXT("%s: name, words, control"), Case.Name), Rows.Num(), 3))
		{
			continue;
		}
		const bool bTexts = Rows[0].Widget->GetType() == FName(TEXT("STextBlock")) && Rows[1].Widget->GetType() == FName(TEXT("STextBlock"));
		if (!TestTrue(*FString::Printf(TEXT("%s: its name and words are texts"), Case.Name), bTexts))
		{
			continue;
		}
		const TSharedRef<STextBlock> Title = StaticCastSharedRef<STextBlock>(Rows[0].Widget);
		const TSharedRef<STextBlock> Words = StaticCastSharedRef<STextBlock>(Rows[1].Widget);
		TestEqual(*FString::Printf(TEXT("%s: its name"), Case.Name), Title->GetText().ToString(), Case.Title);
		TestEqual(*FString::Printf(TEXT("%s: what it does"), Case.Name), Words->GetText().ToString(), Case.Subtitle);
#if !UE_VERSION_OLDER_THAN(5, 3, 0)  // (Unreal 5.2's STextBlock keeps its font private)
		TestTrue(*FString::Printf(TEXT("%s: its name in the mono micro-label"), Case.Name), SameFont(Title->GetFont(), TitleFont));
		TestTrue(*FString::Printf(TEXT("%s: in the sharing text's font"), Case.Name), SameFont(Words->GetFont(), WordsFont));
#endif
		const float NameBottom = Rows[0].Geometry.GetAbsolutePosition().Y + Rows[0].Geometry.GetAbsoluteSize().Y;
		const float WordsTop = Rows[1].Geometry.GetAbsolutePosition().Y;
		const float WordsBottom = WordsTop + Rows[1].Geometry.GetAbsoluteSize().Y;
		const float ControlTop = Rows[2].Geometry.GetAbsolutePosition().Y;
		TestTrue(*FString::Printf(TEXT("%s: 6 px under the name (%.1f)"), Case.Name, WordsTop - NameBottom), FMath::IsNearlyEqual(WordsTop - NameBottom, 6.f, 0.5f));
		TestTrue(*FString::Printf(TEXT("%s: the control 10 px under the words (%.1f)"), Case.Name, ControlTop - WordsBottom),
			FMath::IsNearlyEqual(ControlTop - WordsBottom, 10.f, 0.5f));
		TArray<FString> ControlWords;
		Texts(Rows[2].Widget, Rows[2].Geometry, ControlWords);
		TestEqual(*FString::Printf(TEXT("%s: the control"), Case.Name), FString::Join(ControlWords, TEXT(" | ")), Case.Control);
	}
	// The sharing switch: its name is its tooltip; beside it, 8 px to the pill's right and centred on it, whether sharing
	// is on, in the switch's words (the body face, 13), flipping the moment it is toggled
	const TSharedPtr<SWidget> Toggle = FirstOfType(Cases[0].Card, TEXT("SCheckBox"));
	if (TestTrue(TEXT("sharing: a switch"), Toggle.IsValid()))
	{
		const TSharedPtr<IToolTip> Tip = Toggle->GetToolTip();
		TestTrue(TEXT("sharing: the switch's tooltip, its name"), Tip.IsValid() && AllWords(Tip->AsWidget()).Contains(TEXT("Share runs to improve Autopilot")));
		TestEqual(TEXT("sharing: off, its words"), AllWords(Toggle.ToSharedRef()), FString(TEXT("Data sharing off")));
		const TSharedPtr<SWidget> Label = FirstOfType(Toggle.ToSharedRef(), TEXT("STextBlock"));
		const TSharedPtr<SWidget> Pill = FirstOfType(Toggle.ToSharedRef(), TEXT("SBox"));  // (the pill's 30 x 16)
		const FGeometry Root = FGeometry::MakeRoot(FVector2D(600.f, 600.f), FSlateLayoutTransform());
		Cases[0].Card->SlatePrepass(1.f);
		FGeometry LabelAt, PillAt;
		if (TestTrue(TEXT("sharing: the pill and its words laid out"), Label.IsValid() && Pill.IsValid()
			&& Find(Cases[0].Card, Root, Label.Get(), LabelAt) && Find(Cases[0].Card, Root, Pill.Get(), PillAt)))
		{
			const float Gap = LabelAt.GetAbsolutePosition().X - (PillAt.GetAbsolutePosition().X + PillAt.GetAbsoluteSize().X);
			const float Off = (LabelAt.GetAbsolutePosition().Y + LabelAt.GetAbsoluteSize().Y / 2.f)
				- (PillAt.GetAbsolutePosition().Y + PillAt.GetAbsoluteSize().Y / 2.f);
			TestTrue(*FString::Printf(TEXT("sharing: the words 8 px right of the pill (%.1f)"), Gap), FMath::IsNearlyEqual(Gap, 8.f, 0.5f));
			TestTrue(*FString::Printf(TEXT("sharing: the words centred on the pill (%.1f)"), Off), FMath::Abs(Off) <= 1.f);
#if !UE_VERSION_OLDER_THAN(5, 3, 0)
			TestTrue(TEXT("sharing: the words in the switch's face"), SameFont(StaticCastSharedPtr<STextBlock>(Label)->GetFont(),
				FAutopilotStyle::Font(EAutopilotFont::BodyMedium, 13.f)));
#endif
		}
		Panel->Prefs.bShareData = true;
		TestEqual(TEXT("sharing: on, its words"), AllWords(Toggle.ToSharedRef()), FString(TEXT("Data sharing on")));
		Panel->Prefs.bShareData = false;
		TestEqual(TEXT("sharing: off again"), AllWords(Toggle.ToSharedRef()), FString(TEXT("Data sharing off")));
	}
	// The research agent's switch: the same, in its own words
	const TSharedPtr<SWidget> ResearchToggle = FirstOfType(Cases[1].Card, TEXT("SCheckBox"));
	if (TestTrue(TEXT("research agent: a switch"), ResearchToggle.IsValid()))
	{
		const TSharedPtr<IToolTip> Tip = ResearchToggle->GetToolTip();
		TestTrue(TEXT("research agent: the switch's tooltip, its name"), Tip.IsValid() && AllWords(Tip->AsWidget()).Contains(TEXT("Research agent")));
		TestEqual(TEXT("research agent: off, its words"), AllWords(ResearchToggle.ToSharedRef()), FString(TEXT("Research agent off")));
		Panel->Prefs.ResearchChoice = true;
		TestEqual(TEXT("research agent: on, its words"), AllWords(ResearchToggle.ToSharedRef()), FString(TEXT("Research agent on")));
		// Never chosen: what the agent service does without a choice, off in the Aura app's (Use with Aura) and on in the
		// user's own, also when it runs through Aura (signed in)
		UPlaytestSettings* Settings = GetMutableDefault<UPlaytestSettings>();
		const bool bWasAura = Settings->bUseWithAura;
		Panel->Prefs.ResearchChoice.Reset();
		Panel->Prefs.Access = TEXT("aura");
		Settings->bUseWithAura = false;  // (not saved: put back below)
		TestEqual(TEXT("research agent: the plugin's own agent service, signed in to Aura: on"), AllWords(ResearchToggle.ToSharedRef()), FString(TEXT("Research agent on")));
		Settings->bUseWithAura = true;
		TestEqual(TEXT("research agent: Use with Aura: off"), AllWords(ResearchToggle.ToSharedRef()), FString(TEXT("Research agent off")));
		Panel->Prefs.ResearchChoice = true;
		TestEqual(TEXT("research agent: the choice beats the Aura app's default"), AllWords(ResearchToggle.ToSharedRef()), FString(TEXT("Research agent on")));
		Settings->bUseWithAura = bWasAura;
	}
	return true;
}

// The activity feed (docs/protocol.md, `activity`): each frame a line of the run's feed under its stage (which drives the
// stage track), a call and its result one line (the result's words after the call's, two folds), a frame seen already
// (the feed sent again after a reconnect) changing nothing, a result whose call isn't here a line of its own; the Feed
// tab's items: a heading per stretch of one stage with its count; the folds' words; what the hello asks for
// (activity_since: the live run's last id, nothing for a finished run); a run saved and read back with its events.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotActivityTest, "AutopilotPlaytest.Editor.Activity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotActivityTest::RunTest(const FString& Parameters)
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going in this editor: the activity feed test is skipped"));
		return true;
	}
	TSharedPtr<FAutopilotRun> Live = OtherRun(987950, false);
	Live->bDone = false;
	Live->Stage = EAutopilotStage::None;
	Live->Feed.Reset();
	Live->LastActivityId = 0;
	Live->StartSeconds = FPlatformTime::Seconds();  // (the plugin's clock: the lines' times count from here)
	Session.Runs.Add(Live);  // (no history file: never saved)
	auto Frame = [](int32 Id, double T, const TCHAR* Stage, const TCHAR* Kind, const TCHAR* Text, const TCHAR* Detail, int32 Ends)
	{
		TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("type"), TEXT("activity"));
		F->SetNumberField(TEXT("id"), Id);
		F->SetNumberField(TEXT("t"), T);
		F->SetStringField(TEXT("stage"), Stage);
		F->SetStringField(TEXT("kind"), Kind);
		F->SetStringField(TEXT("text"), Text);
		if (Detail)
		{
			F->SetStringField(TEXT("detail"), Detail);
		}
		else
		{
			F->SetField(TEXT("detail"), MakeShared<FJsonValueNull>());
		}
		if (Ends > 0)
		{
			F->SetNumberField(TEXT("ends"), Ends);
		}
		else
		{
			F->SetField(TEXT("ends"), MakeShared<FJsonValueNull>());
		}
		return TSharedPtr<FJsonObject>(F);
	};

	Session.OnActivity(Frame(1, 2.0, TEXT("analyze"), TEXT("read"), TEXT("Read codex/README.md"), TEXT("codex/README.md"), 0));
	TestEqual(TEXT("a read: one line"), Live->Feed.Num(), 1);
	TestTrue(TEXT("its stage: Analyze, from the frame"), Live->Stage == EAutopilotStage::Analyze);
	if (Live->Feed.Num() == 1)
	{
		TestTrue(TEXT("an activity line, id 1, a magnifying glass"), Live->Feed[0]->IsActivity() && Live->Feed[0]->ActivityId == 1
			&& Live->Feed[0]->Stage == EAutopilotStage::Analyze && Live->Feed[0]->Icon() == TEXT("magnifier") && Live->Feed[0]->Glyph() == TEXT("R"));
		TestEqual(TEXT("its title"), Live->Feed[0]->Title(), FString(TEXT("Read a file")));
		// One clock per feed: the line's time is the plugin's (the run just started here: about 0), the frame's t kept apart
		TestTrue(TEXT("its time: the plugin's clock, not the frame's t"), Live->Feed[0]->Elapsed >= 0.0 && Live->Feed[0]->Elapsed < 30.0
			&& FMath::IsNearlyEqual(Live->Feed[0]->AgentT, 2.0));
		TestTrue(TEXT("a short path: nothing to fold"), Live->Feed[0]->DetailLabel().IsEmpty());
	}
	Session.OnActivity(Frame(2, 83.4, TEXT("probe"), TEXT("probe"), TEXT("Tried 12 lines in the game"), TEXT("print(1)\nprint(2)"), 0));
	Session.OnActivity(Frame(3, 84.6, TEXT("probe"), TEXT("probe"), TEXT("Got 3 lines back in 1.2 s"), TEXT("1\n2\n3"), 2));
	TestEqual(TEXT("a probe and its result: one line"), Live->Feed.Num(), 2);
	TestTrue(TEXT("its stage: Probe"), Live->Stage == EAutopilotStage::Probe);
	if (Live->Feed.Num() == 2)
	{
		const FAutopilotFeedLine& Probe = *Live->Feed[1];
		TestEqual(TEXT("the call's words"), Probe.Text, FString(TEXT("Tried 12 lines in the game")));
		TestEqual(TEXT("the result's words on it"), Probe.Result, FString(TEXT("Got 3 lines back in 1.2 s")));
		TestEqual(TEXT("the result's id"), Probe.ResultId, 3);
		TestEqual(TEXT("the result's detail"), Probe.ResultDetail, FString(TEXT("1\n2\n3")));
		TestEqual(TEXT("a flask"), Probe.Icon(), FString(TEXT("flask")));
		TestEqual(TEXT("the fold: the code"), Probe.DetailLabel(), FString(TEXT("the code")));
		TestEqual(TEXT("the fold: the output"), Probe.ResultLabel(), FString(TEXT("the output")));
		TestEqual(TEXT("the fold's control, closed"), FAutopilotFeedLine::FoldWords(Probe.DetailLabel(), false), FString(TEXT("+ inspect the code")));
		TestEqual(TEXT("the fold's control, open"), FAutopilotFeedLine::FoldWords(Probe.DetailLabel(), true), FString(TEXT("– hide the code")));
		TestEqual(TEXT("the result's fold, closed"), FAutopilotFeedLine::FoldWords(Probe.ResultLabel(), false), FString(TEXT("+ inspect the output")));
		TestTrue(TEXT("the probe's time: the plugin's clock"), Probe.Elapsed >= 0.0 && Probe.Elapsed < 30.0 && FMath::IsNearlyEqual(Probe.AgentT, 83.4));
	}
	TestEqual(TEXT("the last id: 3"), Live->LastActivityId, 3);
	// The feed sent again (a reconnect): nothing changes
	Session.OnActivity(Frame(2, 83.4, TEXT("probe"), TEXT("probe"), TEXT("Tried 12 lines in the game"), TEXT("print(1)\nprint(2)"), 0));
	Session.OnActivity(Frame(3, 84.6, TEXT("probe"), TEXT("probe"), TEXT("Got 3 lines back in 1.2 s"), TEXT("1\n2\n3"), 2));
	Session.OnActivity(Frame(1, 2.0, TEXT("analyze"), TEXT("read"), TEXT("Read codex/README.md"), TEXT("codex/README.md"), 0));
	TestEqual(TEXT("sent again: the same two lines"), Live->Feed.Num(), 2);
	TestTrue(TEXT("sent again: the stage stays"), Live->Stage == EAutopilotStage::Probe);
	TestEqual(TEXT("sent again: 3 events counted as skipped (the log says so)"), Live->ReplaySkipped, 3);
	// A result whose call never came here: a line of its own, at the stage it says
	Session.OnActivity(Frame(5, 90.0, TEXT("play"), TEXT("episode"), TEXT("Episode 1: 60 s of play (status done)"), TEXT("The agent walked to the gate.\nIt opened."), 4));
	TestEqual(TEXT("an orphan result: its own line"), Live->Feed.Num(), 3);
	TestTrue(TEXT("play: the Rollout, steerable"), Live->Stage == EAutopilotStage::Rollout && FAutopilotSession::SteersAt(*Live));
	if (Live->Feed.Num() == 3)
	{
		TestEqual(TEXT("a gamepad"), Live->Feed[2]->Icon(), FString(TEXT("gamepad")));
		TestEqual(TEXT("its fold: what was asked"), Live->Feed[2]->DetailLabel(), FString(TEXT("what was asked")));
	}
	TestEqual(TEXT("a new event: the skipped count said and cleared"), Live->ReplaySkipped, 0);
	// What the hello asks for while it goes: the feed from its last id
	TestEqual(TEXT("activity_since: the live run's last id"), Session.ActivitySince(), 5);
	// A status line still drives the stage track (an older agent service sends no activity frames)
	Session.OnStatus(TEXT("Looking at the results"));
	TestTrue(TEXT("a status line: Results"), Live->Stage == EAutopilotStage::Results);
	TestEqual(TEXT("the status line in the feed, at its stage"), Live->Feed.Num(), 4);
	if (Live->Feed.Num() == 4)
	{
		TestTrue(TEXT("its stage: Results"), Live->Feed[3]->Stage == EAutopilotStage::Results && !Live->Feed[3]->IsActivity());
	}

	// The Feed tab's items: a heading before each stage's lines, with how many
	TArray<TSharedPtr<FAutopilotFeedLine>> Items;
	SPlaytestPanel::GroupFeed(Live->Feed, Items);
	TestEqual(TEXT("grouped: 4 headings and 4 lines"), Items.Num(), 8);
	if (Items.Num() == 8)
	{
		const TCHAR* Headings[] = { TEXT("Analyze"), TEXT("Probe"), TEXT("Play"), TEXT("Results") };
		for (int32 i = 0; i < 4; ++i)
		{
			const FAutopilotFeedLine& H = *Items[i * 2];
			TestTrue(*FString::Printf(TEXT("heading %d: %s, 1 line"), i, Headings[i]), H.Kind == EAutopilotFeedKind::Heading && H.Text == Headings[i] && H.Attempt == 1);
			TestTrue(*FString::Printf(TEXT("line %d under it"), i), Items[i * 2 + 1] == Live->Feed[i]);
		}
	}
	// Two lines of one stage: one heading, 2
	Session.OnActivity(Frame(6, 95.0, TEXT("results"), TEXT("text"), TEXT("The gate opened on the second try"), TEXT("The gate opened on the second try.\nThe first press came too early."), 0));
	Items.Reset();
	SPlaytestPanel::GroupFeed(Live->Feed, Items);
	TestEqual(TEXT("grouped again: 4 headings and 5 lines"), Items.Num(), 9);
	if (Items.Num() == 9)
	{
		TestTrue(TEXT("Results: 2 lines"), Items[6]->Kind == EAutopilotFeedKind::Heading && Items[6]->Attempt == 2);
		TestEqual(TEXT("the agent's words: a robot"), Items[8]->Icon(), FString(TEXT("robot")));
		TestEqual(TEXT("their title"), Items[8]->Title(), FString(TEXT("What the agent said")));
		TestEqual(TEXT("their fold: the whole message"), Items[8]->DetailLabel(), FString(TEXT("the whole message")));
	}
	// An icon, a title and a letter per kind, the same meanings as the other Autopilot UIs; a kind this build doesn't know: none
	{
		const TCHAR* Kinds[] = { TEXT("text"), TEXT("read"), TEXT("search"), TEXT("write"), TEXT("probe"), TEXT("episode"), TEXT("look"),
			TEXT("restart"), TEXT("steer"), TEXT("report"), TEXT("alert"), TEXT("refine") };
		const TCHAR* Icons[] = { TEXT("robot"), TEXT("magnifier"), TEXT("binoculars"), TEXT("pencil"), TEXT("flask"), TEXT("gamepad"), TEXT("eye"),
			TEXT("circular-arrow"), TEXT("speech-bubble"), TEXT("flag"), TEXT("megaphone"), TEXT("notebook") };
		const TCHAR* Glyphs[] = { TEXT("?"), TEXT("R"), TEXT("S"), TEXT("W"), TEXT("P"), TEXT("E"), TEXT("L"), TEXT("*"), TEXT(">"), TEXT("!"), TEXT("!"), TEXT("M") };
		for (int32 I = 0; I < UE_ARRAY_COUNT(Kinds); ++I)
		{
			FAutopilotFeedLine Line;
			Line.Kind = EAutopilotFeedKind::Activity;
			Line.ActivityKind = Kinds[I];
			TestEqual(FString::Printf(TEXT("%s: its icon"), Kinds[I]), Line.Icon(), FString(Icons[I]));
			TestEqual(FString::Printf(TEXT("%s: its letter"), Kinds[I]), Line.Glyph(), FString(Glyphs[I]));
			TestFalse(FString::Printf(TEXT("%s: a title"), Kinds[I]), Line.Title().IsEmpty());
		}
		FAutopilotFeedLine Dream;
		Dream.ActivityKind = TEXT("dream");
		TestTrue(TEXT("an unknown kind: no icon, no title, the dot"), Dream.Icon().IsEmpty() && Dream.Title().IsEmpty() && Dream.Glyph() == TEXT("·"));
	}
	// The refiner's edit to the game's memory: a notebook, its fold the edit and its evidence
	Session.OnActivity(Frame(7, 96.0, TEXT("results"), TEXT("refine"), TEXT("Added to advice.md after episode 2: Open the crate straight away"),
		TEXT("+ Open the crate straight away\nEvidence: episode 2 reached the gate at 41 s"), 0));
	TestEqual(TEXT("a refine: one more line"), Live->Feed.Num(), 6);
	if (Live->Feed.Num() == 6)
	{
		TestEqual(TEXT("a notebook"), Live->Feed[5]->Icon(), FString(TEXT("notebook")));
		TestEqual(TEXT("its title"), Live->Feed[5]->Title(), FString(TEXT("Refined the game's memory")));
		TestEqual(TEXT("its letter"), Live->Feed[5]->Glyph(), FString(TEXT("M")));
		TestEqual(TEXT("its fold: the edit and its evidence"), Live->Feed[5]->DetailLabel(), FString(TEXT("the edit and its evidence")));
	}

	// Done: saved and read back with its events; nothing asked for any more
	Live->bDone = true;
	Live->EndSeconds = Live->StartSeconds + 100.0;
	Live->Verdict = TEXT("GOAL HIT");
	TestEqual(TEXT("activity_since: nothing for a finished run"), Session.ActivitySince(), -1);
	const TSharedPtr<FAutopilotRun> Back = FAutopilotSession::RunFromJson(FAutopilotSession::RunToJson(*Live));
	if (TestTrue(TEXT("read back"), Back.IsValid()))
	{
		TestEqual(TEXT("read back: the lines"), Back->Feed.Num(), Live->Feed.Num());
		TestEqual(TEXT("read back: the last id"), Back->LastActivityId, 7);
		if (Back->Feed.Num() >= 3)
		{
			const FAutopilotFeedLine& Probe = *Back->Feed[1];
			TestTrue(TEXT("read back: the probe, its result and folds"), Probe.IsActivity() && Probe.ActivityId == 2 && Probe.ResultId == 3
				&& Probe.Result == TEXT("Got 3 lines back in 1.2 s") && Probe.Detail == TEXT("print(1)\nprint(2)") && Probe.ResultDetail == TEXT("1\n2\n3")
				&& Probe.ActivityKind == TEXT("probe") && Probe.Stage == EAutopilotStage::Probe);
			TestTrue(TEXT("read back: a status line's stage"), Back->Feed[3]->Stage == EAutopilotStage::Results);
		}
		TestTrue(TEXT("read back: the same again changes nothing"), Back->FindActivity(3).IsValid() && Back->FindActivity(2) == Back->FindActivity(3));
	}
	return true;
}

// Reset memory done: the runs ended leave the tab, their files moved to Saved/Autopilot/Runs/.reset-backup/<time>/ (never
// listed again, still counted on the Settings page); a run going stays; a reset that failed changes nothing. In a
// folder of its own, not the project's history.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotResetRunsTest, "AutopilotPlaytest.Editor.ResetRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotResetRunsTest::RunTest(const FString& Parameters)
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going: skipped"));
		return true;
	}
	Session.SaveHistory();  // (the project's runs so far: in the project's folder)
	const TArray<TSharedPtr<FAutopilotRun>> RunsBefore = Session.Runs;
	const TArray<TSharedPtr<FAutopilotRun>> SetAsideBefore = Session.SetAside;
	const TArray<TSharedPtr<FAutopilotRun>> RetiredBefore = Session.Retired;
	const bool bLoadedBefore = Session.bHistoryLoaded;
	const bool bLedgerBefore = Session.bLedgerLoaded;
	const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("AutopilotResetRunsTest"), FGuid::NewGuid().ToString());
	FAutopilotSession::HistoryDirOverride = Dir;
	Session.Runs.Reset();
	Session.SetAside.Reset();
	Session.Retired.Reset();
	Session.bHistoryLoaded = true;
	Session.bLedgerLoaded = true;

	auto Make = [&Session](const TCHAR* Prompt, bool bDone, double Usd)
	{
		TSharedPtr<FAutopilotRun> Run = Session.NewRun(TEXT("verify"), Prompt);
		Run->bDone = bDone;
		Run->Bill.bKnown = Usd > 0.0;
		Run->Bill.bBilled = Usd > 0.0;
		Run->Bill.Usd = Usd;
		Session.Unsaved.Add(Run->Id);
		return Run;
	};
	const TSharedPtr<FAutopilotRun> A = Make(TEXT("Verify the gate opens"), true, 0.25);
	const TSharedPtr<FAutopilotRun> B = Make(TEXT("Verify the bridge holds"), true, 0.5);
	const TSharedPtr<FAutopilotRun> Going = Make(TEXT("Reach the tower"), false, 0.0);
	Session.SaveHistory();
	TestTrue(TEXT("saved: each run's file"), IFileManager::Get().FileExists(*FPaths::Combine(Dir, A->HistoryFile))
		&& IFileManager::Get().FileExists(*FPaths::Combine(Dir, Going->HistoryFile)));
	B->Prompt = TEXT("Verify the bridge holds up");  // (changed since it was saved: moved as it is now)
	Session.Unsaved.Add(B->Id);

	Session.OnMemoryCleared(false);
	TestEqual(TEXT("not reset: the runs as they were"), Session.GetRuns().Num(), 3);

	Session.OnMemoryCleared(true);
	TestTrue(TEXT("reset: only the run going is listed"), Session.GetRuns().Num() == 1 && Session.GetRuns()[0] == Going);
	TArray<FString> Left;
	IFileManager::Get().FindFiles(Left, *FPaths::Combine(Dir, TEXT("*.json")), true, false);
	TestTrue(TEXT("reset: its file the only one left"), Left.Num() == 1 && Left[0] == Going->HistoryFile);
	TArray<FString> Resets;
	IFileManager::Get().FindFiles(Resets, *FPaths::Combine(Dir, FAutopilotSession::ResetBackup, TEXT("*")), false, true);
	const FString Backup = Resets.Num() == 1 ? FPaths::Combine(Dir, FAutopilotSession::ResetBackup, Resets[0]) : FString();
	FString BText;
	TestTrue(TEXT("reset: the runs ended kept in .reset-backup/<time>/"), Resets.Num() == 1
		&& IFileManager::Get().FileExists(*FPaths::Combine(Backup, A->HistoryFile))
		&& FFileHelper::LoadFileToString(BText, *FPaths::Combine(Backup, B->HistoryFile)) && BText.Contains(TEXT("holds up")));
	int32 Billed = 0;
	const double Bill = Session.ProjectBill(Billed);
	TestTrue(TEXT("reset: the Settings page still counts them"), Session.GetUsageRuns().Num() == 3 && Billed == 2
		&& FMath::IsNearlyEqual(Bill, 0.75));

	// The next editor session: the run going listed (ended, the editor closed), the others only counted
	Session.Runs.Reset();
	Session.SetAside.Reset();
	Session.bHistoryLoaded = false;
	Session.LoadHistory();
	TestTrue(TEXT("next session: only the run going listed"), Session.GetRuns().Num() == 1 && Session.GetRuns()[0]->Prompt == TEXT("Reach the tower"));
	Billed = 0;
	TestTrue(TEXT("next session: the runs set aside still counted"), Session.SetAside.Num() == 2
		&& FMath::IsNearlyEqual(Session.ProjectBill(Billed), 0.75) && Billed == 2);

	Session.Unsaved.Reset();
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Dir), false, true);
	FAutopilotSession::HistoryDirOverride.Empty();
	Session.Runs = RunsBefore;
	Session.SetAside = SetAsideBefore;
	Session.Retired = RetiredBefore;
	Session.bHistoryLoaded = bLoadedBefore;
	Session.bLedgerLoaded = bLedgerBefore;
	++Session.UsageRevision;
	Session.Changed();
	return true;
}

// The usage ledger: a run whose history file goes past MaxSavedRuns keeps its usage and bill for the Settings page
// (counted once while it is still listed this session), in the next editor session too, up to MaxUsageRuns; runs with
// neither aren't kept. As Unity's and Godot's usage ledgers. In a folder of its own, not the project's history.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotUsageLedgerTest, "AutopilotPlaytest.Editor.UsageLedger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotUsageLedgerTest::RunTest(const FString& Parameters)
{
	FAutopilotSession& Session = FAutopilotSession::Get();
	if (Session.GetActiveRun().IsValid())
	{
		AddWarning(TEXT("A run is going: skipped"));
		return true;
	}
	Session.SaveHistory();  // (the project's runs so far: in the project's folder)
	const TArray<TSharedPtr<FAutopilotRun>> RunsBefore = Session.Runs;
	const TArray<TSharedPtr<FAutopilotRun>> SetAsideBefore = Session.SetAside;
	const TArray<TSharedPtr<FAutopilotRun>> RetiredBefore = Session.Retired;
	const bool bLoadedBefore = Session.bHistoryLoaded;
	const bool bLedgerBefore = Session.bLedgerLoaded;
	const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("AutopilotUsageLedgerTest"), FGuid::NewGuid().ToString());
	FAutopilotSession::HistoryDirOverride = Dir;
	Session.Runs.Reset();
	Session.SetAside.Reset();
	Session.Retired.Reset();
	Session.bHistoryLoaded = true;
	Session.bLedgerLoaded = false;  // (no ledger file in the new folder: an empty one read)

	const int32 Total = FAutopilotSession::MaxSavedRuns + 5;
	for (int32 i = 0; i < Total; ++i)
	{
		TSharedPtr<FAutopilotRun> Run = Session.NewRun(TEXT("explore"), FString::Printf(TEXT("Explore %d"), i));
		Run->Started = FDateTime(2026, 1, 1) + FTimespan::FromMinutes(i);  // (each its own start)
		Run->RunId = Run->Started.ToString(TEXT("%Y%m%d-%H%M%S"));
		Run->bDone = true;
		if (i % 5 != 4)  // (every fifth: no bill, never in the ledger)
		{
			Run->Bill.bKnown = true;
			Run->Bill.bBilled = true;
			Run->Bill.Usd = 0.5;
		}
		Session.Unsaved.Add(Run->Id);
	}
	// (the first 5 went past MaxSavedRuns as the later ones began: 4 of them billed)
	TestEqual(TEXT("ledger: the billed runs past the history"), Session.Retired.Num(), 4);
	TestTrue(TEXT("ledger: written"), IFileManager::Get().FileExists(*FAutopilotSession::UsageLedgerPath()));
	int32 Billed = 0;
	double Bill = Session.ProjectBill(Billed);
	const int32 BilledAll = Total - Total / 5;
	TestTrue(TEXT("this session: each billed run counted once"), Billed == BilledAll && FMath::IsNearlyEqual(Bill, 0.5 * BilledAll));
	Session.SaveHistory();

	// The next editor session: 50 runs listed, the 4 before them still counted
	Session.Runs.Reset();
	Session.Retired.Reset();
	Session.bHistoryLoaded = false;
	Session.bLedgerLoaded = false;
	Session.LoadHistory();
	TestEqual(TEXT("next session: the newest runs listed"), Session.GetRuns().Num(), FAutopilotSession::MaxSavedRuns);
	Billed = 0;
	Bill = Session.ProjectBill(Billed);
	TestTrue(TEXT("next session: every billed run still counted"), Billed == BilledAll && FMath::IsNearlyEqual(Bill, 0.5 * BilledAll));
	TestTrue(TEXT("next session: the oldest counted"), Session.GetUsageRuns().Num() > 0
		&& Session.GetUsageRuns()[0]->Prompt == TEXT("Explore 0"));

	// Up to MaxUsageRuns: the oldest go
	Session.Retired.Reset();
	for (int32 i = 0; i < FAutopilotSession::MaxUsageRuns + 3; ++i)
	{
		TSharedPtr<FAutopilotRun> Run = MakeShared<FAutopilotRun>();
		Run->Started = FDateTime(2025, 1, 1) + FTimespan::FromMinutes(i);
		Run->RunId = FString::Printf(TEXT("old-%d"), i);
		Run->Bill.bKnown = true;
		Run->Bill.Usd = 0.01;
		Session.Retired.Add(Run);
	}
	TSharedPtr<FAutopilotRun> Newest = MakeShared<FAutopilotRun>();
	Newest->Started = FDateTime(2025, 6, 1);
	Newest->RunId = TEXT("newest");
	Newest->Bill.bKnown = true;
	Session.Retire(Newest);
	TestTrue(TEXT("cap: MaxUsageRuns kept, the oldest gone"), Session.Retired.Num() == FAutopilotSession::MaxUsageRuns
		&& Session.Retired[0]->RunId == TEXT("old-4") && Session.Retired.Last()->RunId == TEXT("newest"));

	Session.Unsaved.Reset();
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Dir), false, true);
	FAutopilotSession::HistoryDirOverride.Empty();
	Session.Runs = RunsBefore;
	Session.SetAside = SetAsideBefore;
	Session.Retired = RetiredBefore;
	Session.bHistoryLoaded = bLoadedBefore;
	Session.bLedgerLoaded = bLedgerBefore;
	++Session.UsageRevision;
	Session.Changed();
	return true;
}

// Blueprint Break on Exceptions held off while Autopilot plays (the game goes on past an Accessed None; the run reports
// it), the user's setting back after; a setting already off stays off, and nothing is saved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotExceptionBreaksTest, "AutopilotPlaytest.Editor.ExceptionBreaks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotExceptionBreaksTest::RunTest(const FString& Parameters)
{
	UEditorExperimentalSettings* Settings = GetMutableDefault<UEditorExperimentalSettings>();
	const bool bBefore = Settings->bBreakOnExceptions;
	FAutopilotSession::ReleaseExceptionBreaks();  // (none held before)
	Settings->bBreakOnExceptions = true;
	FAutopilotSession::HoldExceptionBreaks();
	TestFalse(TEXT("held: off while Autopilot plays"), Settings->bBreakOnExceptions);
	FAutopilotSession::HoldExceptionBreaks();  // (a restart: held once)
	FAutopilotSession::ReleaseExceptionBreaks();
	TestTrue(TEXT("released: the user's setting back"), Settings->bBreakOnExceptions);
	Settings->bBreakOnExceptions = false;
	FAutopilotSession::HoldExceptionBreaks();
	FAutopilotSession::ReleaseExceptionBreaks();
	TestFalse(TEXT("off already: stays off"), Settings->bBreakOnExceptions);
	Settings->bBreakOnExceptions = bBefore;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutopilotSettleTest, "AutopilotPlaytest.Editor.Settle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutopilotSettleTest::RunTest(const FString& Parameters)
{
	// What a settling box lays out (shown, or coming unseen), as Walk sees it
	const FGeometry Root = FGeometry::MakeRoot(FVector2D(300.f, 200.f), FSlateLayoutTransform());
	auto Laid = [&Root](const TSharedRef<SWidget>& Widget)
	{
		TSet<const SWidget*> Out;
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Root, Children, true);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			const FArrangedWidget& Box = Children[i];
			FChildren* Inside = Box.Widget->GetChildren();
			Out.Add(Inside && Inside->Num() ? &Inside->GetChildAt(0).Get() : &Box.Widget.Get());
		}
		return Out;
	};
	auto Tall = [](float Height) { return SNew(SBox).WidthOverride(100.f).HeightOverride(Height); };

	// Not on screen (a page not shown, a test): a content given shows at once
	TSharedRef<AutopilotUI::SSettle> Settle = SNew(AutopilotUI::SSettle);
	TSharedRef<SBox> A = Tall(40.f);
	Settle->SetContent(A);
	Settle->SlatePrepass(1.f);
	TestEqual(TEXT("off screen: shown at once"), Laid(Settle).Num(), 1);
	TestTrue(TEXT("off screen: the content given"), Laid(Settle).Contains(&A.Get()));
	TestEqual(TEXT("off screen: its size"), FVector2D(Settle->GetDesiredSize()).Y, 40.0);

	// On screen: the one before stays shown (its size too) while the next is laid out unseen
	Settle->TestPainted();
	TSharedRef<SBox> B = Tall(90.f);
	Settle->SetContent(B);
	Settle->SlatePrepass(1.f);
	TSet<const SWidget*> Now = Laid(Settle);
	TestEqual(TEXT("coming: both laid out"), Now.Num(), 2);
	TestTrue(TEXT("coming: the one before shown"), Now.Contains(&A.Get()));
	TestTrue(TEXT("coming: the next laid out unseen"), Now.Contains(&B.Get()));
	TestEqual(TEXT("coming: the size the one before has"), FVector2D(Settle->GetDesiredSize()).Y, 40.0);

	// Painted once unseen at a size that holds: it takes the other's place
	Settle->TestPainted();
	Settle->SlatePrepass(1.f);
	Now = Laid(Settle);
	TestEqual(TEXT("settled: one laid out"), Now.Num(), 1);
	TestTrue(TEXT("settled: the next shown"), Now.Contains(&B.Get()));
	TestEqual(TEXT("settled: its size"), FVector2D(Settle->GetDesiredSize()).Y, 90.0);

	// Given again and again while one settles (no paint between): the last given shows, the others gone
	Settle->TestPainted();
	TSharedRef<SBox> C = Tall(10.f);
	TSharedRef<SBox> D = Tall(20.f);
	Settle->SetContent(C);
	Settle->SlatePrepass(1.f);
	Settle->SetContent(D);
	Settle->SlatePrepass(1.f);
	TestFalse(TEXT("replaced while coming: gone"), Laid(Settle).Contains(&C.Get()));
	Settle->TestPainted();
	Settle->SlatePrepass(1.f);
	TestTrue(TEXT("replaced while coming: the last given shows"), Laid(Settle).Contains(&D.Get()) && Laid(Settle).Num() == 1);

	// Pages (WidgetIndex): switched on screen, the page before stays until the next is laid out; back before it settles,
	// nothing changes
	int32 Index = 0;
	TSharedRef<SBox> P0 = Tall(30.f);
	TSharedRef<SBox> P1 = Tall(60.f);
	TSharedRef<AutopilotUI::SSettle> Pages = SNew(AutopilotUI::SSettle)
		.WidgetIndex_Lambda([&Index]() { return Index; })
		.Pages({ P0, P1 });
	Pages->SlatePrepass(1.f);
	TestTrue(TEXT("pages: the first shown"), Laid(Pages).Contains(&P0.Get()) && Laid(Pages).Num() == 1);
	Pages->TestPainted();
	Index = 1;
	Pages->SlatePrepass(1.f);
	TestTrue(TEXT("pages: switched, the first still shown"), Laid(Pages).Contains(&P0.Get()) && Laid(Pages).Num() == 2);
	Index = 0;
	Pages->SlatePrepass(1.f);
	TestTrue(TEXT("pages: back before it settled, as it was"), Laid(Pages).Contains(&P0.Get()) && Laid(Pages).Num() == 1);
	Index = 1;
	Pages->SlatePrepass(1.f);
	Pages->TestPainted();
	Pages->SlatePrepass(1.f);
	TestTrue(TEXT("pages: the second shown once laid out"), Laid(Pages).Contains(&P1.Get()) && Laid(Pages).Num() == 1);
	TestEqual(TEXT("pages: its size"), FVector2D(Pages->GetDesiredSize()).Y, 60.0);
	return true;
}

#endif
