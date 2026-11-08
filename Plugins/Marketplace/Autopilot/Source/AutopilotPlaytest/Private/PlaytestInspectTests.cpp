// Automation tests of the inspector's message handling (Session Frontend > Automation: AutopilotPlaytest.Inspect).

#include "Misc/AutomationTest.h"
#include "PlaytestInspect.h"
#include "PlaytestJson.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestInspectPeekTest, "AutopilotPlaytest.Inspect.PeekType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestInspectPeekTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("type first"), PlaytestInspect::PeekType(TEXT("{\"type\": \"jev\", \"call\": 1}")), FString(TEXT("jev")));
	TestEqual(TEXT("no spaces"), PlaytestInspect::PeekType(TEXT("{\"type\":\"memory\"}")), FString(TEXT("memory")));
	TestEqual(TEXT("type not first"), PlaytestInspect::PeekType(TEXT("{\"call\": 1, \"type\": \"jev\"}")), FString());
	TestEqual(TEXT("not an object"), PlaytestInspect::PeekType(TEXT("[1, 2]")), FString());
	TestTrue(TEXT("jev is parsed off the game thread"), PlaytestInspect::IsInspectType(TEXT("memories")));
	TestFalse(TEXT("requests are not"), PlaytestInspect::IsInspectType(TEXT("request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestInspectJevTest, "AutopilotPlaytest.Inspect.Jev",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestInspectJevTest::RunTest(const FString& Parameters)
{
	PlaytestInspect::FQuestionMemo Memo;
	const TSharedPtr<FJsonObject> First = PlaytestJson::Parse(TEXT(
		"{\"type\": \"jev\", \"call\": 1, \"step\": 4, \"t\": 12.5, \"wall\": 1790000000.25, \"kind\": \"play\","
		" \"goal\": \"Repair the drone\", \"plan_step\": \"Fly to the pad\", \"seconds\": {\"jev\": 0.21, \"decide\": 0.24},"
		" \"input\": {\"state\": {\"speed\": 0.15}, \"questions\": {\"Move\": {\"q\": \"Which way?\"}, \"Press\": {\"q\": \"What?\"}}},"
		" \"output\": {\"answers\": {\"Move\": {\"forward\": 0.7}}}, \"decided\": {\"summary\": \"KeyWAction\", \"chosen\": [\"KeyWAction\"]},"
		" \"truncated\": false, \"dropped\": 2}"));
	const TSharedPtr<FPlaytestJevCall> Call = PlaytestInspect::ParseJev(First, Memo);
	if (!TestTrue(TEXT("parsed"), Call.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("call"), Call->Call, (int64)1);
	TestEqual(TEXT("step"), Call->Step, 4);
	TestEqual(TEXT("seconds"), Call->Seconds(), 0.21);
	TestEqual(TEXT("summary"), Call->Summary, FString(TEXT("KeyWAction")));
	TestEqual(TEXT("dropped"), Call->Dropped, 2);
	TestTrue(TEXT("input pretty"), Call->Input.Unpack().Contains(TEXT("\"speed\": 0.15")));
	TestTrue(TEXT("output"), Call->Output.Unpack().Contains(TEXT("\"forward\": 0.7")));

	// The next call names Press as unchanged: filled in from the first
	const TSharedPtr<FJsonObject> Second = PlaytestJson::Parse(TEXT(
		"{\"type\": \"jev\", \"call\": 2, \"input\": {\"state\": {}, \"questions\": {\"Move\": {\"q\": \"Which way now?\"}}},"
		" \"questions_same\": [\"Press\"], \"questions_order\": [\"Move\", \"Press\"], \"decided\": {\"chosen\": [\"RepairAction\"]}}"));
	const TSharedPtr<FPlaytestJevCall> Next = PlaytestInspect::ParseJev(Second, Memo);
	const FString Input = Next->Input.Unpack();
	TestTrue(TEXT("unchanged question filled in"), Input.Contains(TEXT("\"What?\"")));
	TestTrue(TEXT("changed question as sent"), Input.Contains(TEXT("Which way now?")));
	TestTrue(TEXT("in the brain's order"), Input.Find(TEXT("Which way now?")) < Input.Find(TEXT("What?")));
	TestEqual(TEXT("summary from chosen"), Next->Summary, FString(TEXT("RepairAction")));

	// Stats
	TArray<TSharedPtr<FPlaytestJevCall>> Calls = {Call};
	FPlaytestJevStats Stats;
	PlaytestInspect::AddToStats(Stats, Calls);
	Next->JevSeconds = 0.41;
	Calls.Add(Next);
	PlaytestInspect::AddToStats(Stats, Calls);
	TestEqual(TEXT("count"), Stats.Count, 2);
	TestTrue(TEXT("average"), FMath::IsNearlyEqual(Stats.Average(), 0.31));
	TestEqual(TEXT("last"), Stats.Last, 0.41);
	TestEqual(TEXT("max"), Stats.RecentMax, 0.41);
	TestEqual(TEXT("dropped"), Stats.Dropped, 2);

	// A call that failed: its error, counted apart from the times
	const TSharedPtr<FJsonObject> Third = PlaytestJson::Parse(TEXT(
		"{\"type\": \"jev\", \"call\": 3, \"step\": null, \"error\": \"TypeSafeError: cut off at max_tokens\","
		" \"seconds\": {\"jev\": 30.0}, \"input\": {\"state\": {\"speed\": 0.1}, \"questions\": {\"Move\": {\"q\": \"Which way?\"}}},"
		" \"output\": {}, \"decided\": {\"summary\": \"Jev call failed: TypeSafeError: cut off at max_tokens\"}}"));
	const TSharedPtr<FPlaytestJevCall> Failed = PlaytestInspect::ParseJev(Third, Memo);
	TestTrue(TEXT("failed"), Failed->Failed());
	TestEqual(TEXT("no step"), Failed->Step, -1);
	TestTrue(TEXT("summary of the input"), Failed->InputSummary.Contains(TEXT("Questions (1): Move")));
	Calls.Add(Failed);
	PlaytestInspect::AddToStats(Stats, Calls);
	TestEqual(TEXT("failed counted"), Stats.Failed, 1);
	TestEqual(TEXT("not timed"), Stats.Last, 0.41);
	TestEqual(TEXT("max without the failure"), Stats.RecentMax, 0.41);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestInspectMemoryTest, "AutopilotPlaytest.Inspect.Memory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestInspectMemoryTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FPlaytestMemoryItem> Item = PlaytestInspect::ParseMemory(PlaytestJson::Parse(TEXT(
		"{\"type\": \"memory\", \"change\": \"formed\", \"kind\": \"plan\", \"title\": \"Repair the drone\","
		" \"key\": \"plans_drones.json/Repair the drone\", \"file\": \"plans_drones.json\", \"source\": \"file\","
		" \"t\": 30.5, \"wall\": 1790000000, \"content\": {\"steps\": [\"Fly to it\", \"Press repair\"]}}")));
	TestEqual(TEXT("kind"), Item->Kind, FString(TEXT("plan")));
	TestEqual(TEXT("change"), Item->Change, FString(TEXT("formed")));
	TestTrue(TEXT("content"), Item->Content.Unpack().Contains(TEXT("Press repair")));
	const TSharedPtr<FPlaytestMemoryItem> Note = PlaytestInspect::ParseMemory(PlaytestJson::Parse(TEXT(
		"{\"kind\": \"note\", \"title\": \"Landing\", \"content\": \"Slow down first.\"}")));
	TestEqual(TEXT("text as it is"), Note->Content.Unpack(), FString(TEXT("Slow down first.")));
	TestEqual(TEXT("listed memories are kept ones"), Note->Change, FString(TEXT("kept")));

	FPlaytestPackedText Packed;
	FString Long;
	for (int32 i = 0; i < 500; ++i)
	{
		Long += FString::Printf(TEXT("drone %d hovers\n"), i);
	}
	Packed.Pack(Long);
	TestTrue(TEXT("compressed"), Packed.GetPackedSize() < Long.Len());
	TestEqual(TEXT("round trip"), Packed.Unpack(), Long);
	const TSharedPtr<FJsonValue> Number = MakeShared<FJsonValueNumber>(0.15);
	TestEqual(TEXT("numbers as short as they read back"), PlaytestInspect::Pretty(Number), FString(TEXT("0.15")));
	return true;
}

#endif
