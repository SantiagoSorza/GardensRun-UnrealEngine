#pragma once

#include "CoreMinimal.h"
#include "AutopilotSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Views/SListView.h"

class SHorizontalBox;
class SSplitter;
class SVerticalBox;
class SWrapBox;
namespace AutopilotUI { class SSettle; }

/** The replay strip: which action ran over an attempt's game time, a tick per step, a mark per switch. */
class SAutopilotReplay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SAutopilotReplay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs) {}

	TFunction<const TArray<TSharedPtr<FAutopilotStep>>&()> GetSteps;
	TFunction<FLinearColor(const FString&)> ColorOf;
	TFunction<TSharedPtr<FAutopilotStep>()> GetSelected;
	TFunction<void(const TSharedPtr<FAutopilotStep>&)> OnPick;

	/** Narrow: a lower lane, no step marks under it. */
	bool bCompact = false;

	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	float XOf(int32 Index, float Width) const;
};

/**
 * A run's policy log: a table of its policy steps (newest at the bottom, following live until a row is picked),
 * filtered by attempt and by what the step did (start, keep, switch, held, dropped), with a replay strip above it;
 * beside it (below it when narrow) the step picked: what it chose, π(a | s) for every action, and the observation
 * it was given (readable, or as raw JSON without the instructions).
 */
class SPlaytestPolicyLog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPlaytestPolicyLog) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void SetRun(const TSharedPtr<FAutopilotRun>& InRun);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** What the step did, as its tag: START, KEEP, SWITCH, HELD, DROPPED (FAILED when the step failed). */
	static FString TagOf(const FPlaytestJevCall& Call);

private:
	TSharedRef<SWidget> MakeFilterBar();
	TSharedRef<SWidget> MakeColumns(const TSharedRef<SWidget>& Wall, const TSharedRef<SWidget>& Time, const TSharedRef<SWidget>& Number,
		const TSharedRef<SWidget>& Current, const TSharedRef<SWidget>& Decision, const TSharedRef<SWidget>& Probabilities, const TSharedRef<SWidget>& Ms) const;
	TSharedRef<ITableRow> MakeRow(TSharedPtr<FAutopilotStep> Step, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeProbabilityBar(const FPlaytestJevCall& Call, int32 Most) const;
	void RebuildAttemptChips();
	void RebuildLegend();
	void SyncRows();
	bool Passes(const FAutopilotStep& Step) const;
	void Select(const TSharedPtr<FAutopilotStep>& Step);
	void ShowStep(const TSharedPtr<FAutopilotStep>& Step);
	TSharedPtr<FAutopilotStep> PreviousInAttempt(const TSharedPtr<FAutopilotStep>& Step) const;
	FString RawJson(const FPlaytestJevCall& Call) const;
	/** What Copy step JSON copies and Raw JSON shows: the frame as received (else RawJson). */
	FString FrameJson(const FPlaytestJevCall& Call) const;
	FLinearColor ColorOf(const FString& Action) const;
	void StepBy(int32 Delta);

	TWeakPtr<FAutopilotRun> Run;
	int32 RunId = INDEX_NONE;
	int32 SyncedRevision = -1;

	TArray<TSharedPtr<FAutopilotStep>> Rows;       // passing the filters
	TArray<TSharedPtr<FAutopilotStep>> StripSteps; // the attempt shown in the replay strip
	TSharedPtr<SListView<TSharedPtr<FAutopilotStep>>> List;
	TSharedPtr<SHorizontalBox> AttemptChips;
	int32 ChipAttempts = -1;
	TSharedPtr<SWrapBox> Legend;
	int32 LegendCount = -1;
	TSharedPtr<SVerticalBox> Detail;  // the step shown: made again for each, in DetailSettle (shown once laid out: no flash)
	TSharedPtr<AutopilotUI::SSettle> DetailSettle;
	TSharedPtr<SSplitter> Splitter;
	bool bVertical = false;
	bool bDetailSized = false;  // (the step picked given its 440 once, when wide)
	TSharedPtr<SAutopilotReplay> Replay;

	int32 AttemptFilter = 0;  // 0: every attempt
	TSet<FString> HiddenTags;
	FString Search;
	TMap<FString, int32> TagCounts;  // in the attempts shown
	mutable FString Footer;
	mutable FString FooterKey;  // (what Footer was made for)
	/** The footer's words (made again when what it counts changes). */
	FString FooterText() const;

	TSharedPtr<FAutopilotStep> SelectedStep;
	TSharedPtr<FAutopilotStep> ShownStep;
	bool bFollow = true;
	bool bRaw = false;
	bool bShownRaw = false;
	double LastShown = 0.0;
};
