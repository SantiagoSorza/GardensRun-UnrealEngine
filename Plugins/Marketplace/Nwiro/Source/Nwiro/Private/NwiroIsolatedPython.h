// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Owns only explicitly requested isolated workers. Never waits on Python on the game thread.
namespace NwiroIsolatedPython
{
	// Both live and isolated execution use the advertised code-size limit.
	constexpr int32 MaxCodeCharacters = 1024 * 1024;

	FString Execute(const TSharedPtr<FJsonObject>& Command);
	void Shutdown();
	bool IsWorker();
}
