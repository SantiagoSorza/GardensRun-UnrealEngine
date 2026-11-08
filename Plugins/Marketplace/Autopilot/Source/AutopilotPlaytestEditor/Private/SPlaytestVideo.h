// A run's recording in its result: a screen the tab's width and the video's shape (16:9 until the video's is known),
// Play or Pause, a scrub bar, the time and the length, and the file shown in its folder (File Explorer, Finder, the file
// manager). Played with the Media Framework, by whichever player plugin the project has on (Electra Player, WMF
// Media): a UMediaPlayer on a UFileMediaSource, drawn through a brush whose resource is a UMediaTexture. The widget keeps them alive (FGCObject) and closes the player when it goes (the run shown changes,
// the tab closes). Nothing plays until Play is pressed.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Types/SlateStructs.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class UFileMediaSource;
class UMediaPlayer;
class UMediaTexture;

class SPlaytestVideo : public SCompoundWidget, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SPlaytestVideo)
		: _Seconds(-1.0)
		, _Open(true)
		, _Gone(false)
		{}
		/** The recording (an MP4). */
		SLATE_ARGUMENT(FString, Path)
		/** Its length as the run's result said (-1: not said), shown until the player knows it. */
		SLATE_ARGUMENT(double, Seconds)
		/** Open it in a player (false: laid out only, the layout test's). */
		SLATE_ARGUMENT(bool, Open)
		/** The recording's file isn't there any more: the player kept, nothing to press, and it says so. */
		SLATE_ARGUMENT(bool, Gone)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SPlaytestVideo();

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SPlaytestVideo"); }

	const FString& GetPath() const { return Path; }
	bool IsPlaying() const;
	void Pause();

private:
	FReply OnPlay();
	/** It can't be played here (no media player in this engine for it, or it failed). */
	bool CantPlay() const;
	void OnScrub(float Value);
	/** Seconds into the recording, and its length (the player's once it knows, else the result's; 0: unknown). */
	double Now() const;
	double Length() const;
	FText TimeText() const;
	/** The video's frame size in pixels: its track's once opened, else its frames' (0: not known, or it can't play). */
	FVector2D VideoSize() const;
	/** The screen's height for the width it was last given: the video's shape (16:9 until it is known). */
	FOptionalSize ScreenHeight() const;
	/** Keeps Slate drawing at full rate while it plays (its frames change), and stops once it doesn't. */
	EActiveTimerReturnType WhilePlaying(double InCurrentTime, float InDeltaTime);

	FString Path;
	double Seconds = -1.0;
	float Width = 0.f;  // as last laid out
	bool bGone = false;
	bool bTimer = false;
	bool bCantPlay = false;
	TObjectPtr<UMediaPlayer> Player;
	TObjectPtr<UFileMediaSource> Source;
	TObjectPtr<UMediaTexture> Texture;
	FSlateBrush Brush;
};
