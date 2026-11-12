// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include <atomic>

// The timeout and game thread compete for one queued request. Cancellation
// never interrupts running Unreal/Python code or claims its mutations rolled back.
class FNwiroDispatchFence
{
	enum class EState : uint8 { Queued, Running, Completed, Cancelled };
	std::atomic<EState> State{EState::Queued};

public:
	bool CancelBeforeStart()
	{
		EState Expected = EState::Queued;
		return State.compare_exchange_strong(Expected, EState::Cancelled);
	}

	template <typename F>
	bool ExecuteIfQueued(F&& Execute)
	{
		EState Expected = EState::Queued;
		if (!State.compare_exchange_strong(Expected, EState::Running)) return false;
		Execute();
		State.store(EState::Completed);
		return true;
	}

	static FString TimeoutResult(bool bCancelledBeforeStart, double Seconds)
	{
		const auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetBoolField(TEXT("executionStarted"), !bCancelledBeforeStart);
		Result->SetBoolField(TEXT("retrySafe"), bCancelledBeforeStart);
		Result->SetStringField(TEXT("executionStatus"),
			bCancelledBeforeStart ? TEXT("cancelled_before_start") : TEXT("outcome_unknown"));
		Result->SetStringField(TEXT("error"), bCancelledBeforeStart
			? FString::Printf(TEXT("Dispatch timed out after %.0fs before execution began. This queued request was cancelled and will not execute."), Seconds)
			: FString::Printf(TEXT("Dispatch timed out after %.0fs after execution began. Work may still be running or may complete; its outcome is unknown. Do not automatically retry. Inspect the affected state before deciding what to do next."), Seconds));
		FString Json;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}
};
