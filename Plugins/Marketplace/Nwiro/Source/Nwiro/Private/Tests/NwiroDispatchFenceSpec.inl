// Copyright 2026 Nwiro. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "Async/Future.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroDispatchFenceProof,
	"Nwiro.Pro.Transport.DispatchFence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroDispatchFenceProof::RunTest(const FString& Parameters)
{
	// Deterministic scheduler interleavings, without sleeping or mutating assets.
	int32 Executions = 0;
	FNwiroDispatchFence Cancelled;
	TPromise<FString> CancelledPromise;
	TFuture<FString> CancelledFuture = CancelledPromise.GetFuture();
	const bool bCancelled = Cancelled.CancelBeforeStart();
	TestTrue(TEXT("queued cancellation wins"), bCancelled);
	const FString CancelledJson = FNwiroDispatchFence::TimeoutResult(bCancelled, 60);
	// UE promises must complete even if their queued callback is skipped.
	CancelledPromise.SetValue(CancelledJson);
	TestFalse(TEXT("late queued delivery never executes"), Cancelled.ExecuteIfQueued([&] { ++Executions; }));
	TestFalse(TEXT("duplicate queued delivery never executes"), Cancelled.ExecuteIfQueued([&] { ++Executions; }));
	TestEqual(TEXT("no side effect after cancellation"), Executions, 0);
	TestTrue(TEXT("cancelled promise is fulfilled"), CancelledFuture.IsReady());
	TSharedPtr<FJsonObject> Response;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CancelledFuture.Get()), Response);
	if (!TestNotNull(TEXT("cancelled result parses"), Response.Get())) return false;
	TestFalse(TEXT("cancelled result success false"), Response->GetBoolField(TEXT("success")));
	TestFalse(TEXT("cancelled execution did not begin"), Response->GetBoolField(TEXT("executionStarted")));
	TestTrue(TEXT("cancelled request can be retried"), Response->GetBoolField(TEXT("retrySafe")));
	TestEqual(TEXT("cancelled status exact"), Response->GetStringField(TEXT("executionStatus")), FString(TEXT("cancelled_before_start")));

	FNwiroDispatchFence Running;
	TPromise<FString> RunningPromise;
	TFuture<FString> RunningFuture = RunningPromise.GetFuture();
	FString Timeout;
	TestTrue(TEXT("dispatch begins once"), Running.ExecuteIfQueued([&]
	{
		++Executions;
		TestFalse(TEXT("running task cannot be pre-start cancelled"), Running.CancelBeforeStart());
		Timeout = FNwiroDispatchFence::TimeoutResult(false, 60);
		TestFalse(TEXT("running future remains pending"), RunningFuture.IsReady());
		RunningPromise.SetValue(TEXT("actual completed result"));
	}));
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Timeout), Response);
	if (!TestNotNull(TEXT("running timeout result parses"), Response.Get())) return false;
	TestTrue(TEXT("running execution began"), Response->GetBoolField(TEXT("executionStarted")));
	TestFalse(TEXT("running timeout is not safe to retry"), Response->GetBoolField(TEXT("retrySafe")));
	TestEqual(TEXT("running timeout outcome is unknown"), Response->GetStringField(TEXT("executionStatus")), FString(TEXT("outcome_unknown")));
	TestEqual(TEXT("late actual result still completes"), RunningFuture.Get(), FString(TEXT("actual completed result")));
	TestFalse(TEXT("completed task cannot be cancelled"), Running.CancelBeforeStart());
	TestFalse(TEXT("completed task cannot execute again"), Running.ExecuteIfQueued([&] { ++Executions; }));
	TestEqual(TEXT("running request executes exactly once"), Executions, 1);

	FNwiroDispatchFence Completed;
	TPromise<int32> CompletedPromise;
	TFuture<int32> CompletedFuture = CompletedPromise.GetFuture();
	TestTrue(TEXT("ordinary completion runs"), Completed.ExecuteIfQueued([&] { CompletedPromise.SetValue(31); }));
	TestTrue(TEXT("ready result wins timeout boundary"), CompletedFuture.IsReady());
	TestEqual(TEXT("actual result preserved at timeout boundary"), CompletedFuture.Get(), 31);
	TestFalse(TEXT("late cancellation cannot misreport no execution"), Completed.CancelBeforeStart());
	return true;
}
