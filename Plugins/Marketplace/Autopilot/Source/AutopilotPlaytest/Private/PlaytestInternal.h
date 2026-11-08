#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/OutputDevice.h"
#include "UObject/WeakObjectPtr.h"

class UPlaytestSubsystem;
class UWorld;
class AActor;
class UClass;
class UObject;

DECLARE_LOG_CATEGORY_EXTERN(LogPlaytest, Log, All);

namespace PlaytestOps
{
	/** Handle a request. Returns true if it responded (or registered a pending response). */
	void Handle(UPlaytestSubsystem& Bridge, int64 Id, const FString& Op, const TSharedPtr<FJsonObject>& Args);

	/** Every frame: keys held through the `key` op stay down, held axes send their value, leases run out. */
	void TickKeys(UPlaytestSubsystem& Bridge, float DeltaTime);

	/** How the player's inputs reach the game: context's and hello's `input` (PlaytestOpsSession.cpp). */
	TSharedRef<FJsonObject> InputInfo();
}

/** The game's code for a brain that can't read this machine's disk (docs/protocol.md: `codex_manifest`, `codex_read`). */
namespace PlaytestCodex
{
	/** `codex_manifest`: runs the Blueprint export as a child process when needed, then lists and hashes the files. */
	void Manifest(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	/** `codex_read`: the listed files asked for, as text. */
	void Read(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args);
	/** Whether the Blueprint export is running, and how far it is: files exported, of all it exports (both 0 until it
	 *  has counted them). Game thread. */
	bool ExportProgress(int32& OutDone, int32& OutTotal);
	/** Ends the export running, if any (the run was stopped): the manifest waiting for it answers with an error. */
	void StopExport();
	/** Ends an export still running (the bridge is going away). */
	void Shutdown();
}

namespace PlaytestScreen
{
	struct FShownText
	{
		FString Key;
		FString Text;
	};
	/** Every visible text in the world's game viewport: UMG text keyed <UserWidget>.<TextBlock>, other
	 *  Slate text by its place in the widget tree (or its tag). */
	TArray<FShownText> Read(UWorld* World);

	/**
	 * Without a renderer (-nullrhi), Slate paints nothing, and UMG widgets only tick while they are
	 * painted: a HUD that updates its text on its widget's Tick would stay frozen. When nothing renders,
	 * tick the world's constructed user widgets here instead. Does nothing when the game renders.
	 */
	void TickUnpainted(UWorld* World, float DeltaTime);
}

namespace PlaytestUI
{
	/**
	 * The interactive elements on screen: UMG buttons, check boxes, combo boxes, sliders and text inputs, and
	 * user widgets whose Blueprint handles the mouse or drag and drop itself (e.g. inventory slots). Each
	 * is {"id", "kind", "name", "owner", "label" (texts in it), "enabled", ...}; ids stay valid until the
	 * next listing.
	 */
	TArray<TSharedPtr<FJsonValue>> List(UWorld* World);

	/**
	 * Act on an element by id: {"id", "action": "click" | "right_click" | "double_click" | "set" | "drag",
	 * "to" (drag: the element to drop on), "option" (combo box), "value" (slider), "text" (text input)}.
	 * Events go straight to the widgets, so this works headless too. Returns "" or what went wrong.
	 */
	FString Act(UWorld* World, const TSharedPtr<FJsonObject>& Args);

	/**
	 * The id of the element at a pixel of the game's view (Size: the view's size in pixels), the topmost there, as
	 * the game's widgets are laid out over the view (computed here: a game that doesn't render lays nothing out);
	 * "" when none. For the pointer's clicks on a game that doesn't render (PlaytestOpsPointer.cpp).
	 */
	FString ElementAt(UWorld* World, const FVector2D& Pixel, const FVector2D& Size);
}

class FPlaytestMediaRender;
class SViewport;
class SWindow;

/**
 * `record` and `capture` (docs/protocol.md, "Recording, stills and usage"): the game's picture, UI included, read back
 * from the GPU without waiting for it and encoded off the game thread (PlaytestMedia.cpp).
 */
class FPlaytestMedia
{
public:
	explicit FPlaytestMedia(UPlaytestSubsystem& InBridge);
	~FPlaytestMedia();

	/** Whether this game can capture its picture at all (it renders: not -nullrhi, not a server). Hello's "media". */
	static bool CanCapture();

	void Record(int64 Id, const TSharedPtr<FJsonObject>& Args);
	void Capture(int64 Id, const TSharedPtr<FJsonObject>& Args);
	/** Every tick (game thread): sends what is encoded, answers stills, paces the recording. */
	void Tick();
	void Shutdown();

private:
	struct FJobs;

	/** "" and the game viewport's rectangle in its window's back buffer, or why there is no picture ("no picture: ..."). */
	FString FindPicture(FIntRect& OutRect, const SWindow*& OutWindow);
	/** Hooks the render thread's side to Slate's presents (once). */
	bool Hook();
	bool Start(bool bStill, int64 Number, int32 MaxDim, int32 Quality, FString& OutError);
	void StopRecording();

	UPlaytestSubsystem& Bridge;
	TUniquePtr<FJobs> Jobs;
	TSharedPtr<FPlaytestMediaRender, ESPMode::ThreadSafe> Render;
	FDelegateHandle BackBufferHandle;
	TWeakPtr<SViewport> ViewportWidget;  // the viewport last seen, and its window
	TWeakPtr<SWindow> ViewportWindow;

	bool bRecording = false;
	double Interval = 0.2;
	int32 RecordMaxDim = 960;
	int32 RecordQuality = 75;
	double NextFrame = 0.0;  // FPlatformTime; 0: at once
	int64 NextN = 0;
	int64 FramesSent = 0;
	int32 RecordSerial = 0;  // the connection the recording was started on
	int32 Generation = 0;    // bumped at each start and stop: older frames in flight are dropped
};

/** Captures engine errors, ensure failures and Blueprint runtime errors from the log (any thread). */
class FPlaytestLogCapture : public FOutputDevice
{
public:
	FPlaytestLogCapture();
	virtual ~FPlaytestLogCapture() override;
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	struct FLine
	{
		FString Verbosity;
		FString Category;
		FString Message;
	};
	TArray<FLine> Take();
	bool bEnabled = false;

private:
	FCriticalSection Lock;
	TArray<FLine> Lines;
	int32 Dropped = 0;
};

/** Frame-accurate checks that run in the engine, independent of the brain's step rate. */
class FPlaytestDetectors
{
public:
	explicit FPlaytestDetectors(UPlaytestSubsystem& InBridge);

	void Configure(const TSharedPtr<FJsonObject>& Args);
	void Reset();
	void Tick(UWorld* World, double RealDelta);
	void OnLogLines(const TArray<FPlaytestLogCapture::FLine>& Lines, double GameTime);

	bool bEnabled = true;
	double TeleportFactor = 3.0;
	double TeleportMin = 1000.0;
	double LongFallSeconds = 8.0;
	double HitchSeconds = 0.75;
	double GrowthSeconds = 120.0;
	double HudEvery = 0.5;
	double PropertyEvery = 0.5;

private:
	void Report(const FString& Detector, const FString& Severity, const FString& Summary, const FString& Signature,
		double GameTime, const AActor* Actor = nullptr, const TSharedPtr<FJsonObject>& Details = nullptr);
	void CheckActors(UWorld* World, double GameTime);
	void CheckPawns(UWorld* World, double GameTime, double WorldDelta);
	void CheckGrowth(UWorld* World, double GameTime);
	void CheckProperties(UWorld* World, double GameTime);
	void CheckHud(UWorld* World, double GameTime);
	bool IsProjectClass(const UClass* Class) const;

	/** One numeric property CheckProperties reads on each object of a class: its name, its ClampMin/ClampMax, and its
	 *  Max partner (MaxHealth for Health, HealthMax for Health), each found once per class. */
	struct FPropertyCheck
	{
		const FProperty* Property = nullptr;
		FString Name;
		FString Min;  // ClampMin as written ("" none)
		FString Max;
		TOptional<double> MinValue;
		TOptional<double> MaxValue;
		const FProperty* MaxProperty = nullptr;
		FString MaxName;
	};
	/** What CheckProperties reads on the objects of one class (none: not the game's own class, or no numbers). */
	struct FClassChecks
	{
		TArray<FPropertyCheck> Properties;
	};
	/** A class's checks, made on its first object of this sweep. They hold FProperty pointers, which a Blueprint
	 *  compile frees: kept for one sweep only (one call, so nothing compiles in between). */
	const FClassChecks& ChecksOf(const UClass* Class, TMap<const UClass*, FClassChecks>& Plans, TMap<const UClass*, bool>& Project) const;
	bool IsProjectClassCached(const UClass* Class, TMap<const UClass*, bool>& Project) const;
	void CheckObjectProperties(const FClassChecks& Checks, UObject* Object, const AActor* Actor, double GameTime);

	UPlaytestSubsystem& Bridge;
	TMap<FString, int32> Seen;  // signature -> times seen
	TWeakObjectPtr<UWorld> World;
	FDelegateHandle ActorDestroyedHandle;  // on World (removed by Reset)
	double SessionStartReal = 0;
	double LastGameTime = -1;
	double NextActorCheck = 0;
	double NextGrowthSample = 0;
	double NextPropertyCheck = 0;
	double NextHudCheck = 0;

	struct FPawnTrack
	{
		FVector Location = FVector::ZeroVector;
		int32 Frames = 0;
		double FallingSince = -1;
	};
	TMap<TWeakObjectPtr<AActor>, FPawnTrack> Pawns;
	TArray<TMap<FString, int32>> GrowthSamples;
	TSet<FString> ProjectPackages;
};
