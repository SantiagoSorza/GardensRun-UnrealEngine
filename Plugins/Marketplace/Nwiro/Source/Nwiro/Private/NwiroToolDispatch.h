// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Tool dispatch — routes tool names to C++ implementations.
 * Called directly from Bridge::ExecuteTool.
 */
class FNwiroToolDispatch
{
public:
	static FString DispatchTool(const FString& ToolName, const FString& ArgsJson);

	/** Tool defs the user has ENABLED — the only list a model may ever see. */
	static FString GetEnabledToolDefinitionsJson();

	/** Full tool inventory including user-disabled namespaced tools, each def
	 *  carrying `"disabled":true|false`. Settings UI only — never for a model. */
	static FString GetToolDefinitionsForSettingsJson();
};
