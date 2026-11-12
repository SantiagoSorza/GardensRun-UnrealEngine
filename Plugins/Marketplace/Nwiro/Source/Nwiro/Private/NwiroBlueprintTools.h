// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

namespace NwiroEnhancedInputContract { struct FPreparedActions; }

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UK2Node;
struct FNwiroPreparedVariable;
struct FNwiroPreparedFunction;
struct FNwiroDispatcherMutation;

// Result from any blueprint edit operation
struct FNwiroBPResult
{
	bool bSuccess = false;
	FString Message;
	TSharedPtr<FJsonObject> Data;

	// Non-fatal, per-unit notes. Merged into the response's existing top-level warnings[],
	// not a ledger bucket: the only producer (add_components) has no ledger, and the two
	// handlers that do have one produce no warnings, so the bucket would always be empty.
	TArray<FString> Warnings;

	/**
	 * Level-2 acceptance: what happened to each REQUEST UNIT, machine-readable.
	 *
	 * `bSuccess` answers whether a per-unit verdict could be produced at all; it does
	 * NOT say whether the requested work was applied. That distinction has never had a
	 * field — until now it survived only as English inside `Message`, e.g.
	 * "Connected 0 pin pair(s). Errors: Node not found in 'castSubsys.Object'", which
	 * nothing downstream can branch on.
	 *
	 * Shape: { op, requestUnits, created[], modified[], removed[], skipped[],
	 * errors[] } with one entry per request unit across the buckets, never one per
	 * artifact mutation. NULL on handlers not yet migrated — edit_blueprint emits an
	 * `operations[]` entry only for handlers that set it, so a partially migrated
	 * surface reads as "this op does not report yet" rather than "this op did nothing".
	 *
	 * Mirror of the Integration Kit's FNwiroIKBPResult::Level2. The two plugins keep
	 * deliberately forked copies and must stay structurally identical.
	 */
	TSharedPtr<FJsonObject> Level2;

	static FNwiroBPResult Ok(const FString& Msg)
	{
		FNwiroBPResult R;
		R.bSuccess = true;
		R.Message = Msg;
		return R;
	}

	static FNwiroBPResult Fail(const FString& Msg)
	{
		FNwiroBPResult R;
		R.bSuccess = false;
		R.Message = Msg;
		return R;
	}
};

// Tracks node references within an editing session (ref name -> node GUID)
struct FNwiroNodeRef
{
	FGuid NodeGuid;
	FString GraphName;

	// Which Blueprint this binding belongs to, and the bare ref it was stored under.
	// The map key is BlueprintPath|Ref; these two make the value self-describing so
	// lookups can compare a ref without parsing the key apart.
	FString BlueprintPath;
	FString Ref;
};

/**
 * Core blueprint manipulation logic for Nwiro.
 * All methods are static, called from UNwiroBridge.
 * Returns JSON strings for communication with the web UI.
 */
class FNwiroBlueprintTools
{
public:
	// ===== Bridge-facing API (returns JSON) =====

	// Search blueprints by name/path. Returns JSON array.
	static FString FindBlueprints(const FString& SearchTerm);

	// Render a Blueprint's graph (event graph or named graph) to PNG via SGraphEditor + FWidgetRenderer.
	static FString RenderBlueprintGraph(const FString& JsonCommand);
	// Auto-arrange a graph's nodes (headless layered layout) so they don't overlap.
	static FString OrganizeBlueprintNodes(const FString& JsonCommand);

	// Read blueprint structure (variables, components, functions, graphs). Returns JSON.
	// When ArgsJson contains a "graph" field, returns full node/pin data for that function graph.
	static FString ReadBlueprint(const FString& ArgsJson);

	// Main edit entry point. Accepts JSON with operations. Returns JSON result.
	static FString EditBlueprint(const FString& JsonCommand);

	// Create a new blueprint asset. Returns JSON with the created blueprint path.
	static FString CreateBlueprint(const FString& JsonCommand);

	// Search available node types for blueprint graphs. Returns JSON.
	static FString FindBlueprintNodes(const FString& Query, const FString& BlueprintPath);

	// Delete a blueprint asset.
	static FString DeleteBlueprint(const FString& JsonCommand);

	// Duplicate a blueprint asset.
	static FString DuplicateBlueprint(const FString& JsonCommand);

	// Rename a blueprint asset.
	static FString RenameBlueprint(const FString& JsonCommand);


	// Clear all user-created nodes from a blueprint graph (keeps events)
	static FString ClearGraph(const FString& JsonCommand);

	// Standalone wrappers for edit_blueprint sub-operations
	static FString DeleteNode(const FString& JsonCommand);
	static FString CreateFunctionGraph(const FString& JsonCommand);
	static FString AddInterface(const FString& JsonCommand);
	static FString RemoveInterface(const FString& JsonCommand);
	static FString CreateEventDispatcher(const FString& JsonCommand);
	static FString ReparentBlueprint(const FString& JsonCommand);
	static FString RemoveComponent(const FString& JsonCommand);
	static FString EditComponent(const FString& JsonCommand);

	// Clear session node references
	static void ClearNodeRefs();

	// ===== Class Resolution =====
	// Public so file-scope helpers (e.g. ResolveInterfaceClass) can reuse it.
	static UClass* FindClassByName(const FString& Name);

private:
	static bool ValidateBlueprintRequest(UBlueprint* BP, const TSharedPtr<FJsonObject>& Cmd, const FString& GraphName, const NwiroEnhancedInputContract::FPreparedActions& PreparedActions);

	// ===== Asset Loading =====
	static UBlueprint* LoadBP(const FString& PathOrName, FString* OutError = nullptr);

	// ===== Graph Helpers =====
	static UEdGraph* FindGraph(UBlueprint* BP, const FString& GraphName);
	// OutAmbiguity, when supplied, receives a candidate list if the ref matches more
	// than one node by title. In that case the function returns nullptr rather than
	// guessing; an empty OutAmbiguity alongside nullptr means simply not found.
	static UEdGraphNode* FindNodeByRef(UEdGraph* Graph, const FString& Ref,
		FString* OutAmbiguity = nullptr);
	// Null + nonempty OutAmbiguity is refusal, never a missing-pin fallback.
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName,
		EEdGraphPinDirection Dir = EGPD_MAX, FString* OutAmbiguity = nullptr);
	static void StoreNodeRef(const FString& Ref, UEdGraphNode* Node, const FString& GraphName);

	// ===== Type Parsing =====
	static FEdGraphPinType ParsePinType(const TSharedPtr<FJsonObject>& TypeObj);

	// ===== Edit Operations =====
	static FNwiroBPResult DoAddVariables(UBlueprint* BP, const TArray<FNwiroPreparedVariable>& Items);
	static FNwiroBPResult DoRemoveVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoRenameVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoAddComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoRemoveComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoSetComponentProperties(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoAddFunctions(UBlueprint* BP, const TArray<FNwiroPreparedFunction>& Items);
	static FNwiroBPResult DoRemoveFunctions(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoAddCustomEvents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoAddEventDispatchers(
		UBlueprint* BP,
		const TArray<TSharedPtr<FJsonValue>>& Items,
		TArray<FNwiroDispatcherMutation>* OutMutations = nullptr);
	static FNwiroBPResult DoAddInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoRemoveInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoReparent(UBlueprint* BP, const FString& NewParentClass);
	static FNwiroBPResult DoAddNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items, const NwiroEnhancedInputContract::FPreparedActions& PreparedActions);
	static FNwiroBPResult DoRemoveNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoConnectPins(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoBreakConnections(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroBPResult DoSetPinDefaults(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);

	// ===== Component Property Helper =====
	// Applies one key/value pair to a component template.
	// Handles special asset-loading cases, direct reflection, and deep struct reflection.
	// Returns true if the property was successfully set.
	static bool ApplyComponentProperty(UActorComponent* CompTemplate, const FString& Key, const FString& Value, const FString& CompName, TArray<FString>* OutErrors = nullptr, TArray<FString>* OutWarnings = nullptr);

	// ===== Serialization Helpers =====
	static TSharedPtr<FJsonObject> SerializeVariable(UBlueprint* BP, const FName& VarName);
	static TSharedPtr<FJsonObject> SerializeComponent(const UActorComponent* Comp, const FName& VarName);
	static TSharedPtr<FJsonObject> SerializeGraph(UEdGraph* Graph, bool bIncludeNodes);
	static TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node);
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	// ===== Node Ref Storage =====
	static TMap<FString, FNwiroNodeRef> NodeRefs;
};
