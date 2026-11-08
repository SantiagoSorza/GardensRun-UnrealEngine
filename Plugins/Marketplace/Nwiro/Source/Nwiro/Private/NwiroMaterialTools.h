// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Json.h"

class UMaterial;
class UMaterialInterface;
class UMaterialInstanceConstant;
class UMaterialExpression;
class UMaterialFunction;

// Result from material edit operations.
//
// `bSuccess` reflects whether the helper itself ran without an internal error.
// Per-item failures (ref not found, pin not found, property not applied) populate
// `Skipped` and `Errors` instead — the helper still returns bSuccess=true. The
// new-schema aggregator computes its own success rule from Errors+Skipped
// (success = both empty); the legacy aggregator uses bSuccess and folds
// Modified/Skipped into messages[] so legacy success semantics don't change.
// See `Design Documents/material-function-tools-new-schema-migration.md` §4.2.
struct FNwiroMatResult
{
	bool bSuccess = false;
	FString Message;
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	// Per-item created records for addExpressions/addInputs/addOutputs.
	// Each entry is a JSON object: { op, userName (or userRef for legacy), assignedRef, type }
	TArray<TSharedPtr<FJsonValue>> Created;
	// Per-item modified records for setMetadata/disconnect/clearGraph/remove*.
	// Each entry is a JSON object: { op, ... } stamped by the producing helper.
	TArray<TSharedPtr<FJsonValue>> Modified;
	// Per-item skipped records for requested mutations that did not happen
	// (ref not found, pin not found, property not applied). Each Skipped entry
	// pairs with an Errors entry carrying the full machine-readable diagnostic.
	TArray<TSharedPtr<FJsonValue>> Skipped;

	static FNwiroMatResult Ok(const FString& Msg) { FNwiroMatResult R; R.bSuccess = true; R.Message = Msg; return R; }
	static FNwiroMatResult Fail(const FString& Msg) { FNwiroMatResult R; R.bSuccess = false; R.Message = Msg; return R; }
};

/**
 * Core material manipulation logic for Nwiro.
 * All methods are static, called from UNwiroBridge.
 * Returns JSON strings for communication with the web UI.
 */
class FNwiroMaterialTools
{
public:
	// ===== Material Instance Operations (existing) =====
	static FString FindMaterials(const FString& JsonCommand);
	static FString InspectMaterial(const FString& JsonCommand);
	static FString CreateMaterialInstance(const FString& JsonCommand);
	static FString UpdateMaterialInstance(const FString& JsonCommand);
	static FString DeleteMaterialInstance(const FString& JsonCommand);
	static FString ApplyMaterial(const FString& JsonCommand);
	static FString ListMaterialSlots(const FString& JsonCommand);

	// ===== Master Material Operations (NEW) =====
	static FString CreateMaterial(const FString& JsonCommand);
	static FString EditMaterial(const FString& JsonCommand);
	static FString InspectMaterialGraph(const FString& JsonCommand);
	static FString DeleteMaterial(const FString& JsonCommand);
	static FString FindTextures(const FString& JsonCommand);
	static void ClearExpressionRefs();

	// Standalone wrappers
	static FString SetMaterialProperty(const FString& JsonCommand);
	static FString EditMaterialInstance(const FString& JsonCommand);
	static FString DeleteExpression(const FString& JsonCommand);

	// ===== Material Function Operations (Policy 1.1 strict) =====
	static FString FindMaterialFunctions(const FString& JsonCommand);
	static FString CreateMaterialFunction(const FString& JsonCommand);
	static FString InspectMaterialFunction(const FString& JsonCommand);
	static FString EditMaterialFunction(const FString& JsonCommand);
	static FString DeleteMaterialFunction(const FString& JsonCommand);

private:
	// ===== Asset Loading Helpers =====
	static UMaterialInterface* LoadMaterialByPath(const FString& AssetPath);
	static UMaterialInstanceConstant* LoadMIByPath(const FString& AssetPath);
	static UMaterial* LoadMasterMaterial(const FString& PathOrName);

	// ===== Parameter Setting Helpers =====
	// Switches: static switch parameters [{ name, value }], checked against the instance's known
	// static switches (the engine setter accepts unknown names silently); after them the static
	// permutation is refreshed once. OutWarnings collects a missing texture or an unknown switch.
	static void ApplyParametersToMI(
		UMaterialInstanceConstant* MI,
		const TArray<TSharedPtr<FJsonValue>>& Scalars,
		const TArray<TSharedPtr<FJsonValue>>& Vectors,
		const TArray<TSharedPtr<FJsonValue>>& Textures,
		const TArray<TSharedPtr<FJsonValue>>& Switches = TArray<TSharedPtr<FJsonValue>>(),
		TArray<FString>* OutWarnings = nullptr
	);
	/** Remove the instance's own override of each named texture parameter (parent's texture shows
	 *  again); an unknown name is a warning. */
	static void ClearTextureOverrides(UMaterialInstanceConstant* MI, const TArray<TSharedPtr<FJsonValue>>& Names, TArray<FString>& OutWarnings);

	// ===== Master Material Helpers =====
	static UClass* ResolveExpressionClass(const FString& ClassName);
	static bool ResolveMaterialProperty(const FString& PropName, int32& OutProp);
	// `OutUnknownPropertyKeys` (G1/G9 fallback): when a key in `Props` doesn't
	// resolve to any UProperty on the expression class, its bare name is pushed
	// here so the caller can emit a `PROPERTY_NOT_FOUND` + matching `skipped[]`
	// pair with the right field-path prefix (top-level vs nested `properties{}`).
	//
	// `OutAssetMissPaths` (G14): when a property tries to load a referenced asset
	// (texture, UObject*) and the asset isn't found, the key→path mapping is pushed
	// here so the new-schema caller can emit policy-shaped `NOT_FOUND` + `skipped[]`.
	// Legacy callers (master-material `DoAddExpressions`) leave this nullptr and
	// continue to receive the legacy `ASSET_NOT_FOUND` MakeDiagnostic entry in
	// `OutErrors` for backward compatibility.
	static void SetExpressionProperties(UMaterialExpression* Expr, const TSharedPtr<FJsonObject>& Props, TArray<FString>* OutWarnings = nullptr, TArray<TSharedPtr<FJsonValue>>* OutErrors = nullptr, TArray<FString>* OutUnknownPropertyKeys = nullptr, TMap<FString, FString>* OutAssetMissPaths = nullptr);
	static void SetMaterialProperties(UMaterial* Mat, const TSharedPtr<FJsonObject>& Props);
	static TSharedPtr<FJsonObject> SerializeExpression(UMaterialExpression* Expr);

	// ===== Edit Sub-Operations =====
	static FNwiroMatResult DoAddExpressions(UMaterial* Mat, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroMatResult DoConnectExpressions(UMaterial* Mat, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroMatResult DoRemoveExpressions(UMaterial* Mat, const TArray<TSharedPtr<FJsonValue>>& Items);

	// ===== Material Function Helpers =====
	static UMaterialFunction* LoadMaterialFunction(const FString& PathOrName);
	// Internal: produces a legacy-shaped inspect JSON. The public InspectMaterialFunction
	// wraps this and reshapes the response into a policy 1.1 envelope. Kept as a
	// helper so the rich graph-walking code lives in one place.
	static FString InspectMaterialFunctionRaw(const FString& JsonCommand);
	static FNwiroMatResult DoAddExpressionsToFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items, const TCHAR* ItemNoun = TEXT("expression"));
	static FNwiroMatResult DoConnectExpressionsInFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroMatResult DoRemoveExpressionsFromFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items);

	// Factored from the original inline blocks per §4.2. Each helper
	// stamps `op` on records it produces and populates Modified/Skipped/Errors.
	// bSuccess reflects helper-level execution (always true today); per-item
	// failures live in Skipped+Errors.
	static FNwiroMatResult DoRemoveInputsFromFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroMatResult DoRemoveOutputsFromFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroMatResult DoDisconnectInFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroMatResult DoSetMetadataOnFunction(UMaterialFunction* Func, const TSharedPtr<FJsonObject>& Meta);
	static FNwiroMatResult DoClearFunctionGraph(UMaterialFunction* Func);

	// ===== Serialization Helpers =====
	static TSharedRef<FJsonObject> SerializeMaterialEntry(const FAssetData& Asset);

	// ===== Expression Ref Storage =====
	static TMap<FString, TWeakObjectPtr<UMaterialExpression>> ExpressionRefs;
};
