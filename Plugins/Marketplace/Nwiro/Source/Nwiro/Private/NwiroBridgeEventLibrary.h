// Copyright 2026 Nwiro. All Rights Reserved.

// Nwiro — static bridge-event entry points exposed to Python.
//
// UE Python can call static UFUNCTIONs on a UBlueprintFunctionLibrary
// directly as `unreal.LibraryName.method_name(args)`. Instance UFUNCTIONs
// on UObjects require the live instance, which is awkward to obtain for
// UNwiroBridge from Python. This library is a thin static facade over
// UNwiroBridge::Instance->PushEvent for the events Python needs to fire.
//
// Currently exposes:
//   EmitHdriResult — used at the end of the HDRI bake script emitted by
//                    pythonEmitter.ts. Fires `hdri_result` keyed by the
//                    promptId inside the JSON.
//   EmitLandscapeResult — used at the end of the LandscapeTool placement script
//                    emitted by extensions/landscape/pythonEmitter.ts. Fires
//                    `landscape_result` keyed by the promptId inside the JSON.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NwiroBridgeEventLibrary.generated.h"

UCLASS()
class UNwiroBridgeEventLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Fire an `hdri_result` event up to the frontend with the given JSON
	 * payload. The payload's shape is owned by pythonEmitter.ts and
	 * includes `promptId` so the frontend can correlate to the in-flight
	 * bake without any extra bridge state.
	 *
	 * Routes through UNwiroBridge::Instance->PushEvent — same dispatch
	 * mechanism the existing `fal_import` and other events use.
	 *
	 * Returns:
	 *   true  if the event was dispatched to the bridge.
	 *   false if the bridge singleton was not available (e.g. very early
	 *         editor startup, or running outside a normal session). The
	 *         bake script prints a `[HDRI_RESULT_BRIDGE_UNAVAILABLE]`
	 *         diagnostic to LogPython in that case.
	 *
	 * Safe to call from the game thread; PushEvent itself marshals to
	 * the WebBrowser channel internally.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nwiro|Bridge")
	static bool EmitHdriResult(const FString& JsonString);

	/**
	 * Fire a `landscape_result` event up to the frontend with the given JSON
	 * payload. Mirror of EmitHdriResult: the payload's shape is owned by
	 * extensions/landscape/pythonEmitter.ts and includes `promptId` so the
	 * frontend can correlate to the in-flight placement without extra bridge
	 * state.
	 *
	 * Returns:
	 *   true  if the event was dispatched to the bridge.
	 *   false if the bridge singleton was not available. The placement script
	 *         prints a `[LANDSCAPE_RESULT_BRIDGE_UNAVAILABLE]` diagnostic to
	 *         LogPython in that case.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nwiro|Bridge")
	static bool EmitLandscapeResult(const FString& JsonString);
};
