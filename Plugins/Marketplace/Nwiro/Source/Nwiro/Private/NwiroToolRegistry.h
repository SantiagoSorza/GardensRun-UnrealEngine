// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Host-side hub that aggregates every EXTERNAL tool source into the tool
 * dispatch layer. Mirrored in Nwiro Integration Kit (FNwiroIKToolRegistry).
 *
 * Sources, each landing in its own assigned namespace:
 *   - plugin.<id>.<tool>   INwiroToolProvider modular features        (Phase 1)
 *   - plugin.<id>.<tool>   UFUNCTION(meta=(NwiroTool=...)) reflection  (Phase 2)
 *   - ue.<toolset>.<tool>  Epic 5.8 native UToolsetDefinition import   (Phase 3)
 *
 * Two integration points used by tool dispatch:
 *   - AppendEnabledProviderDefs / AppendAllProviderDefsWithState: append the
 *     external, namespaced defs to the tools JSON array (reserved-namespace
 *     guards, dedup). The Enabled variant applies the user-disabled-namespace
 *     filter and is the ONLY one that may feed a model; the WithState variant
 *     serializes everything (each def carrying `"disabled":true|false`) and
 *     exists solely for the settings UI, so disabling a source never hides
 *     its own re-enable toggle.
 *   - TryDispatch: route a namespaced call to the right source, on the game
 *     thread, returning a JSON result.
 *
 * The advertised set is FROZEN at session start so the cached tool-name prefix
 * stays byte-stable across turns.
 */
/** Outcome of starting a ue.* (Epic ToolsetRegistry) execution. */
enum class ENwiroProEpicStart : uint8
{
	NotEpic,    // caller must fall through to its own path
	Completed,  // OutResult valid NOW; OnResult NOT invoked (never both)
	Deferred,   // OnResult fires exactly once, later, on the game thread
};

class FNwiroToolRegistry
{
public:
	/** Start a ue.* call without ever blocking the game thread.
	 *
	 *  Must be called ON the game thread. Pass a null OnResult when the call
	 *  site cannot await a late result — it then never returns Deferred and
	 *  answers with a structured "async required" error instead. The engine
	 *  operation is tracked either way, because it cannot be cancelled.
	 *
	 *  Completion, timeout and shutdown all settle through one exactly-once
	 *  gate, so OnResult fires at most once on every path. */
	static ENwiroProEpicStart BeginEpicTool(const FString& ToolName, const FString& ArgsJson,
	                                        TFunction<void(FString)> OnResult, FString& OutResult);

	/** Settle every still-open response with `Reason` and unregister tickers.
	 *  Game thread only; call while the delivery channel is still alive. */
	static void CancelPendingEpicCalls(const FString& Reason);

	/** In-flight ue.* operations — for shutdown assertions and tests. */
	static int32 NumPendingEpicCalls();

	/** Append the external, namespaced tool defs the user has ENABLED to `J`
	 *  (the in-progress tools JSON array body). Adds a separating comma only
	 *  when needed and never leaves a trailing comma. No-op when nothing
	 *  external is registered. Every model-facing consumer uses this. */
	static void AppendEnabledProviderDefs(FString& J);

	/** Append ALL external tool defs to `J`, including user-disabled ones, each
	 *  def carrying a `"disabled":true|false` field. For the settings UI only —
	 *  never feed this list to a model. */
	static void AppendAllProviderDefsWithState(FString& J);

	/** If `ToolName` is namespaced (plugin./ue./mcp.), run it and write the JSON
	 *  result into OutResult, returning true. Returns false otherwise so the
	 *  caller continues to its own fallback. Rejects user-disabled namespaces. */
	static bool TryDispatch(const FString& ToolName, const FString& ArgsJson, FString& OutResult);

	// ─── End-user tool-source controls (settings) ───────────────────────────
	/** Namespaces (e.g. "plugin.acme", "ue") the user has switched off. Disabled
	 *  namespaces never appear in tools/list and are rejected in dispatch.
	 *  On Pro the authoritative disabled set lives app-side; this mirror provides
	 *  defense-in-depth + a single resolution point. */
	static void SetDisabledNamespaces(const TArray<FString>& Namespaces);
	static TArray<FString> GetDisabledNamespaces();

	/** Re-resolve all sources (call when providers (un)register or the user
	 *  toggles a source). The next session start re-freezes. */
	static void Invalidate();
};
