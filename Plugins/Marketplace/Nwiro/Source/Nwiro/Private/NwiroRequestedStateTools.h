// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

// Loaded authored-state observation only. No load, CDO, compile or save fallback.
class FNwiroRequestedStateTools
{
public:
	static FString ObserveBlueprintState(const FString& JsonCommand);
	static FString ObserveInputActionState(const FString& JsonCommand);
	// Two JSON tool definitions, each followed by a comma for the existing builder.
	static FString DefinitionsJson();
};
