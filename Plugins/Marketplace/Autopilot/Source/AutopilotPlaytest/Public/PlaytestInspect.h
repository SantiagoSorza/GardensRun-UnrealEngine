// What the brain does, as the editor's Autopilot tab shows it in its policy log (docs/protocol.md: `inspect`, `jev`, `memory`,
// `memories`): every Jev call (what it was sent, what it answered, what the step decided, how long it took) and the
// memories the agent forms. Parsed off the game thread (a call can be a large message), kept by UPlaytestSubsystem.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Text kept compressed (a long run keeps thousands of Jev calls), unpacked when shown. */
struct AUTOPILOTPLAYTEST_API FPlaytestPackedText
{
	void Pack(const FString& Text);
	FString Unpack() const;
	int64 GetPackedSize() const { return Data.Num(); }

private:
	TArray<uint8> Data;
	int32 RawSize = 0;  // UTF-8 bytes before compression
	bool bCompressed = false;
};

/** One action's probability in a call's answer (`output.answers.pick`). */
struct AUTOPILOTPLAYTEST_API FPlaytestPick
{
	FString Name;
	float P = 0.f;
};

/** One Jev call as the brain reported it (`jev`). */
struct AUTOPILOTPLAYTEST_API FPlaytestJevCall
{
	int64 Call = 0;            // the brain's count of calls
	int32 Step = -1;
	double GameTime = -1.0;    // game seconds; < 0 when unknown
	double WallTime = 0.0;     // Unix seconds (the brain's clock) when the step began
	double JevSeconds = -1.0;  // the call itself; < 0 when not measured
	double DecideSeconds = -1.0;  // the step's whole decision (the call and the code around it)
	double PerceiveSeconds = -1.0;  // the step's look at the game before it
	FString Kind;              // "play", or "defend" (fighting back while a test watches)
	FString Goal;
	FString PlanStep;
	int32 PlanAt = 0;          // which plan step (from 1) of how many; 0 when not told
	int32 PlanSteps = 0;
	FString Summary;           // what the step did, in one line
	int32 Dropped = 0;         // calls the brain dropped just before this one (a slow connection)
	bool bTruncated = false;   // long parts were cut short by the brain
	FString Error;             // the call failed (the model API's error, a timeout, max_tokens): its error; "" otherwise
	// What the step chose, for the editor's policy log (parsed with the rest, off the game thread)
	FString NowDoing;          // input.state.now_doing.option: the option running when the call was made; "" when none
	double NowDoingSeconds = -1.0;  // ... for how long (now_doing.for_seconds); < 0 when not told
	FString Action;            // the verb of decided.summary ("<action>: <option>"): started, kept, switched, kept_running, dropped
	FString Chosen;            // decided.chosen[0], else the option named by decided.summary; "" when none
	TArray<FPlaytestPick> Pick;  // output.answers.pick, the most likely first
	FString InputSummary;      // a few lines on the input (its state's fields, the questions), shown for a large one
	FPlaytestPackedText Input;    // the state and questions as sent, as readable JSON
	FPlaytestPackedText Output;   // every answer
	FPlaytestPackedText Decided;  // what the step decided, and why
	FPlaytestPackedText Frame;    // the frame as received, its questions' instructions taken out (Copy step JSON)

	bool Failed() const { return !Error.IsEmpty(); }
	/** The duration shown for the call: the call itself, else the step's decision. */
	double Seconds() const { return JevSeconds >= 0 ? JevSeconds : DecideSeconds; }
	int64 GetPackedSize() const { return Input.GetPackedSize() + Output.GetPackedSize() + Decided.GetPackedSize(); }
};

/** One memory (`memory`, or one of `memories`). */
struct AUTOPILOTPLAYTEST_API FPlaytestMemoryItem
{
	int64 Id = 0;             // order of arrival in this editor session
	double GameTime = -1.0;   // when it formed, in game seconds; < 0 when unknown
	double WallTime = 0.0;    // Unix seconds: when it formed, or its file's time (kept)
	FString Change;           // kept (in memory when the tab asked), formed, changed, forgotten
	FString Kind;             // control, thing, run, knowledge, plan, test, lesson, note, class label, not a bug, ...
	FString Title;
	FString Key;              // the same memory from one event to the next
	FString File;             // its file in memory/
	FString Source;           // play (learned while playing, not saved yet) or file
	bool bTruncated = false;
	FPlaytestPackedText Content;  // readable JSON, or text (notes); kept compressed, as the Jev calls' panes are

	/** About what keeping it costs (see UPlaytestSubsystem::MaxMemoryBytes). */
	int64 GetPackedSize() const
	{
		return Content.GetPackedSize() + (int64)(Change.Len() + Kind.Len() + Title.Len() + Key.Len() + File.Len() + Source.Len()) * sizeof(TCHAR) + 128;
	}
};

/** How long Jev calls take, for the tab's header (see PlaytestInspect::AddToStats). */
struct AUTOPILOTPLAYTEST_API FPlaytestJevStats
{
	int32 Count = 0;            // calls received in this editor session (the kept ones and those let go of)
	int32 Failed = 0;           // ... of them that failed (not in the durations below)
	int32 Timed = 0;            // ... of them with a duration
	double Sum = 0.0;           // their seconds
	int32 DecideTimed = 0;
	double DecideSum = 0.0;     // the whole decisions' seconds (the call and the code around it)
	double Last = -1.0;         // the latest call's seconds
	double RecentAverage = 0.0; // over the latest RecentWindow calls
	double RecentMax = 0.0;
	int32 Dropped = 0;          // calls the brain dropped (never received)

	static constexpr int32 RecentWindow = 50;

	double Average() const { return Timed ? Sum / Timed : 0.0; }
	double DecideAverage() const { return DecideTimed ? DecideSum / DecideTimed : 0.0; }
};

namespace PlaytestInspect
{
	/** The questions of the call before, for filling in those a call names as unchanged (`questions_same`). */
	struct FQuestionMemo
	{
		TArray<FString> Order;
		TMap<FString, TSharedPtr<FJsonValue>> Questions;
		void Reset() { Order.Reset(); Questions.Reset(); }
	};

	/** The "type" of a message whose first field it is ({"type": "jev", ...}) without parsing the rest; "" otherwise. */
	AUTOPILOTPLAYTEST_API FString PeekType(const FString& Text);

	/** Whether messages of this type are parsed off the game thread (jev, memory, memories). */
	AUTOPILOTPLAYTEST_API bool IsInspectType(const FString& Type);

	/** JSON as indented text (two spaces), numbers as short as reads back exactly, short lists on one line. */
	AUTOPILOTPLAYTEST_API FString Pretty(const TSharedPtr<FJsonValue>& Value);
	AUTOPILOTPLAYTEST_API FString Pretty(const TSharedPtr<FJsonObject>& Object);

	/** A `jev` message as a call; questions named unchanged are filled in from Memo (then updated). */
	AUTOPILOTPLAYTEST_API TSharedPtr<FPlaytestJevCall> ParseJev(const TSharedPtr<FJsonObject>& Message, FQuestionMemo& Memo);

	/** A `memory` message, or an entry of `memories`. */
	AUTOPILOTPLAYTEST_API TSharedPtr<FPlaytestMemoryItem> ParseMemory(const TSharedPtr<FJsonObject>& Entry);

	/** Count the newest call (Calls.Last()) into Stats; the recent figures over the latest calls kept. A failed call is
	 *  counted (Failed) but not timed. */
	AUTOPILOTPLAYTEST_API void AddToStats(FPlaytestJevStats& Stats, const TArray<TSharedPtr<FPlaytestJevCall>>& Calls);

	/** Unix seconds as local time of day, HH:MM:SS.mmm. */
	AUTOPILOTPLAYTEST_API FString ClockText(double UnixSeconds);
}
