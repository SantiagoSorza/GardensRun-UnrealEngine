// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroBlueprintTools.h"
#include "NwiroEnhancedInputPreparedActions.h"
#include "NwiroVariableEditabilityAdmission.h"
#include "NwiroComponentRequestAdmission.h"
#include "NwiroTransactionHelper.h"
#include "NwiroAssetGuard.h"
#include "HAL/IConsoleManager.h"   // CVarStrictConnectPins — see the ledger block below
// Graph rendering deps (for RenderBlueprintGraph)
#include "GraphEditor.h"
#include "EdGraph/EdGraph.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Editor.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"
#include "TextureResource.h"
#include "EdGraphNode_Comment.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
// Common UMG component classes — used as fallback owners for CallFunction
// resolution when the agent omits `target` (e.g. SetPercent → UProgressBar).
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/SpinBox.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ComboBoxString.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "KismetCompiler.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputKey.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Timeline.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Self.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Select.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_EnhancedInputAction.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "Editor.h"
#include "Json.h"
#include "EditorAssetLibrary.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"

#include "NwiroBlueprintBatchAdmission.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroBP, Log, All);

TMap<FString, FNwiroNodeRef> FNwiroBlueprintTools::NodeRefs;

struct FNwiroDispatcherParamSpec
{
	FName Name;
	FEdGraphPinType Type;
};

struct FNwiroDispatcherMutation
{
	FString RequestedName;
	FName Name;
	TArray<FNwiroDispatcherParamSpec> Params;
	TObjectPtr<UEdGraph> Graph = nullptr;
	bool bMemberCreated = false;
	bool bCreated = false;
	bool bRolledBack = false;
	bool bRollbackVerified = false;
	bool bSignatureGraphPresent = false;
	bool bSkeletonSignatureValid = false;
	TOptional<bool> GeneratedSignatureValid;
	FString ErrorCode;
	FString Error;
};

namespace NwiroDispatcherPrivate
{
	static FString PinBaseName(const FEdGraphPinType& Type)
	{
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Byte
			&& !Type.PinSubCategoryObject.IsValid()) return TEXT("byte");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Byte) return TEXT("enum");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Int) return TEXT("int");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Real)
			return Type.PinSubCategory == UEdGraphSchema_K2::PC_Double
				? TEXT("double") : TEXT("float");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_String) return TEXT("string");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Name) return TEXT("name");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Text) return TEXT("text");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Object) return TEXT("object");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Class) return TEXT("class");
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Struct) return TEXT("struct");
		return Type.PinCategory.ToString();
	}

	static FString PinContainerName(const FEdGraphPinType& Type)
	{
		switch (Type.ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		default: return TEXT("none");
		}
	}

	// Reflected UFunction array/set parameters can acquire ABI-only
	// const/reference flags. Those flags are not part of the Blueprint payload
	// type requested by the caller, so validate the semantic pin shape here.
	static bool ArePayloadTypesEquivalent(
		const FEdGraphPinType& Actual,
		const FEdGraphPinType& Expected)
	{
		return Actual.PinCategory == Expected.PinCategory
			&& Actual.PinSubCategory == Expected.PinSubCategory
			&& Actual.PinSubCategoryObject == Expected.PinSubCategoryObject
			&& Actual.ContainerType == Expected.ContainerType;
	}

	static TSharedRef<FJsonObject> ParamToJson(const FNwiroDispatcherParamSpec& Param)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Param.Name.ToString());
		Json->SetStringField(TEXT("type"), PinBaseName(Param.Type));
		if (UObject* Subtype = Param.Type.PinSubCategoryObject.Get())
		{
			Json->SetStringField(TEXT("subtype"), Subtype->GetPathName());
		}
		Json->SetStringField(TEXT("container"), PinContainerName(Param.Type));
		return Json;
	}

	static TSharedRef<FJsonObject> MutationToJson(const FNwiroDispatcherMutation& Mutation)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"),
			Mutation.Name.IsNone() ? Mutation.RequestedName : Mutation.Name.ToString());
		Json->SetBoolField(TEXT("created"), Mutation.bCreated);
		Json->SetBoolField(
			TEXT("rolledBack"), Mutation.bRolledBack && Mutation.bRollbackVerified);
		Json->SetBoolField(TEXT("signatureGraphPresent"), Mutation.bSignatureGraphPresent);
		const bool bSignatureValid = !Mutation.bRolledBack
			&& (Mutation.GeneratedSignatureValid.IsSet()
				? Mutation.GeneratedSignatureValid.GetValue()
				: Mutation.bSkeletonSignatureValid);
		Json->SetBoolField(TEXT("signatureValid"), bSignatureValid);
		Json->SetBoolField(TEXT("skeletonSignatureValid"), Mutation.bSkeletonSignatureValid);
		if (Mutation.GeneratedSignatureValid.IsSet())
		{
			Json->SetBoolField(TEXT("generatedSignatureValid"),
				Mutation.GeneratedSignatureValid.GetValue());
		}
		else
		{
			Json->SetField(TEXT("generatedSignatureValid"), MakeShared<FJsonValueNull>());
		}
		TArray<TSharedPtr<FJsonValue>> Params;
		for (const FNwiroDispatcherParamSpec& Param : Mutation.Params)
		{
			Params.Add(MakeShared<FJsonValueObject>(ParamToJson(Param)));
		}
		Json->SetArrayField(TEXT("params"), Params);
		if (!Mutation.ErrorCode.IsEmpty())
		{
			Json->SetStringField(TEXT("errorCode"), Mutation.ErrorCode);
		}
		if (!Mutation.Error.IsEmpty())
		{
			Json->SetStringField(TEXT("error"), Mutation.Error);
		}
		return Json;
	}

	static void SetDispatcherArray(
		const TSharedRef<FJsonObject>& Result,
		const TArray<FNwiroDispatcherMutation>& Mutations)
	{
		TArray<TSharedPtr<FJsonValue>> JsonMutations;
		for (const FNwiroDispatcherMutation& Mutation : Mutations)
		{
			JsonMutations.Add(MakeShared<FJsonValueObject>(MutationToJson(Mutation)));
		}
		Result->SetArrayField(TEXT("dispatchers"), JsonMutations);
	}

	static FBPVariableDescription* FindDirectVariable(UBlueprint* BP, const FName Name)
	{
		if (!BP) return nullptr;
		return BP->NewVariables.FindByPredicate(
			[Name](const FBPVariableDescription& Variable)
			{
				return Variable.VarName == Name;
			});
	}

	static UEdGraph* FindSignatureGraph(const UBlueprint* BP, const FName Name, int32* OutCount = nullptr)
	{
		int32 Count = 0;
		UEdGraph* Found = nullptr;
		if (BP)
		{
			for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
			{
				if (Graph && Graph->GetFName() == Name)
				{
					Found = Graph;
					++Count;
				}
			}
		}
		if (OutCount) *OutCount = Count;
		return Found;
	}

	static UK2Node_FunctionEntry* FindOnlyEntry(UEdGraph* Graph)
	{
		if (!Graph) return nullptr;
		TArray<UK2Node_FunctionEntry*> Entries;
		Graph->GetNodesOfClass(Entries);
		return Entries.Num() == 1 ? Entries[0] : nullptr;
	}

	static FMulticastDelegateProperty* FindOwnedDelegateProperty(UClass* Class, const FName Name)
	{
		if (!Class) return nullptr;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (It->GetFName() == Name)
			{
				return CastField<FMulticastDelegateProperty>(*It);
			}
		}
		return nullptr;
	}

	static UFunction* FindOwnedSignatureFunction(UClass* Class, const FName DispatcherName)
	{
		if (!Class) return nullptr;
		const FName SignatureName(
			*(DispatcherName.ToString() + HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX));
		return FindObjectFast<UFunction>(Class, SignatureName);
	}
}

// Node refs are scoped to the Blueprint that owns them. The map was keyed by the bare
// ref, which is global across every Blueprint: creating `printOne` in Blueprint B
// destroyed the binding Blueprint A was using. A's caller was then told "Node not
// found" with no hint the ref had been rebound elsewhere, so it rebuilt a node that
// already existed. Nothing returned the WRONG node — every consumer checks the guid is
// in the graph it was handed — but a ref that silently dies is its own defect.
static FString NodeRefOwnerKey(const UEdGraph* Graph)
{
	const UBlueprint* OwnerBP = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
	return OwnerBP ? OwnerBP->GetPathName() : FString();
}
static FString MakeNodeRefKey(const UEdGraph* Graph, const FString& Ref)
{
	return NodeRefOwnerKey(Graph) + TEXT("|") + Ref;
}

// Normalize a property key from LLM input: strip underscores, lowercase.
// Handles static_mesh / StaticMesh / staticMesh → "staticmesh"
// ── Level-2 acceptance ledger ───────────────────────────────────────────────
//
// Mirror of the Integration Kit's FNwiroIKUnitLedger. The two plugins keep
// deliberately forked copies and MUST stay structurally identical — a caller that
// learns the shape from one product must be able to rely on it in the other.
//
// What happened to each REQUEST UNIT, machine-readable. `success` answers whether a
// per-unit verdict could be produced; it does not say whether the work was applied,
// and that distinction has never had a field — it survived only as English inside the
// message string, which nothing downstream can branch on.
//
// The invariant a caller may rely on, unconditionally:
//   created + modified + removed + skipped + errors == requestUnits
// One entry per request unit, NEVER one per artifact mutation.
struct FNwiroUnitLedger
{
	TArray<TSharedPtr<FJsonValue>> Created, Modified, Removed, Skipped, Errors;

	/** One diagnostic per REJECTED unit, in the policy 1.1 diagnostic shape. */
	// `Allowed` lists values accepted **for the component named by `Field`**, which is why
	// Field can be finer than the request key: connect_pins takes `to` as a compound
	// "Node.Pin", so a node failure reports "to.node" and a pin failure "to.pin". Every
	// entry must be usable verbatim in that component.
	//
	// Emitted only when non-empty — an always-present empty array reads as "nothing is
	// allowed" rather than "no enumerable replacement exists".
	void AddError(int32 Unit, const FString& Op, const FString& Code,
		const FString& Field, const FString& Received, const FString& Message,
		const TArray<FString>& Allowed = TArray<FString>())
	{
		TSharedRef<FJsonObject> D = MakeShareable(new FJsonObject());
		D->SetStringField(TEXT("code"), Code);
		D->SetStringField(TEXT("op"), Op);
		D->SetNumberField(TEXT("unit"), Unit);
		D->SetStringField(TEXT("field"), Field);
		D->SetStringField(TEXT("received"), Received);
		D->SetStringField(TEXT("message"), Message);
		if (Allowed.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> A;
			for (const FString& V : Allowed) A.Add(MakeShareable(new FJsonValueString(V)));
			D->SetArrayField(TEXT("allowed"), A);
		}
		Errors.Add(MakeShareable(new FJsonValueObject(D)));
	}

	/**
	 * One entry per APPLIED unit. `asRequested:false` means the unit landed but is not
	 * what was asked for — a subtype that fell back, a coerced value. Neither an error
	 * nor a clean create, and before this field it reported as a clean success.
	 */
	static TSharedPtr<FJsonValue> MakeUnit(int32 Unit, const FString& Op,
		const FString& Name, bool bAsRequested,
		const FString& Requested = FString(), const FString& Resolved = FString())
	{
		TSharedRef<FJsonObject> U = MakeShareable(new FJsonObject());
		U->SetStringField(TEXT("op"), Op);
		U->SetNumberField(TEXT("unit"), Unit);
		U->SetStringField(TEXT("name"), Name);
		U->SetBoolField(TEXT("asRequested"), bAsRequested);
		if (!Requested.IsEmpty()) { U->SetStringField(TEXT("requested"), Requested); }
		if (!Resolved.IsEmpty())  { U->SetStringField(TEXT("resolved"), Resolved); }
		return MakeShareable(new FJsonValueObject(U));
	}

	// Deliberately no `complete` field: exhaustiveness holds by construction and a consumer
	// can check it directly. The check is on INDICES, not a sum — the buckets' `unit`
	// values must cover 0..N-1 exactly once. sum(buckets) == requestUnits is weaker and
	// passes even when one unit is duplicated and another missing.
	// Request-unit indices already named by an entry in any bucket. The reconcile loops
	// below pad what is missing and MUST pad by index, not by a running count: a count
	// matches only when every unaccounted unit sits at the tail, and otherwise duplicates
	// one index while leaving another unnamed. The sum invariant balances either way.
	TSet<int32> AccountedUnits() const
	{
		TSet<int32> Seen;
		const TArray<TSharedPtr<FJsonValue>>* Buckets[] = { &Created, &Modified, &Removed, &Skipped, &Errors };
		for (const TArray<TSharedPtr<FJsonValue>>* Bucket : Buckets)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Bucket)
			{
				const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
				double UnitValue = 0.0;
				if (Obj.IsValid() && Obj->TryGetNumberField(TEXT("unit"), UnitValue))
				{
					Seen.Add(static_cast<int32>(UnitValue));
				}
			}
		}
		return Seen;
	}

	TSharedPtr<FJsonObject> Build(const FString& Op, int32 RequestUnits) const
	{
		TSharedRef<FJsonObject> B = MakeShareable(new FJsonObject());
		B->SetStringField(TEXT("op"), Op);
		B->SetNumberField(TEXT("requestUnits"), RequestUnits);
		B->SetArrayField(TEXT("created"),  Created);
		B->SetArrayField(TEXT("modified"), Modified);
		B->SetArrayField(TEXT("removed"),  Removed);
		B->SetArrayField(TEXT("skipped"),  Skipped);
		B->SetArrayField(TEXT("errors"),   Errors);
		return B;
	}
};

/**
 * PRO ONLY — the Integration Kit applies the strict rule with no CVar. This one is an
 * escape hatch, not a staging gate: set it to 0 to restore the legacy always-Ok
 * verdict without a rebuild.
 *
 * This is the one change in the port that alters `success`, and in Pro a success:false
 * reaches the cloud API's errorHint() path, so the flip was measured on the Kit first.
 * The fact that de-risked it: re-issuing an ALREADY-MADE connection still returns
 * success:true (TryCreateConnection reports true for an existing link), so the rule is
 * idempotent and a replayed script cannot turn a no-op into a false failure.
 *
 * It also fails a PARTIAL batch, not just zero-of-N. modified[] still names the units
 * that landed, so success:false here does not mean nothing happened.
 */
static TAutoConsoleVariable<int32> CVarStrictConnectPins(
	TEXT("nwiro.ConnectPins.StrictAccounting"), 1,
	TEXT("When 1 (default), connect_pins reports success:false if applied < requested. ")
	TEXT("Set 0 to restore the legacy always-Ok verdict. The level-2 ledger is emitted either way."),
	ECVF_Default);

// Did a level-2 ledger record any unit that actually landed? skipped[] and errors[] are
// accounted-for but NOT applied, so they must not count as a mutation.
static bool LedgerAppliedAnything(const TSharedPtr<FJsonObject>& Level2)
{
	if (!Level2.IsValid()) return false;
	const TCHAR* AppliedBuckets[] = { TEXT("created"), TEXT("modified"), TEXT("removed") };
	for (const TCHAR* Bucket : AppliedBuckets)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Level2->TryGetArrayField(Bucket, Arr) && Arr && Arr->Num() > 0) return true;
	}
	return false;
}

static FString NormalizeKey(const FString& Key)
{
	FString Result = Key.Replace(TEXT("_"), TEXT(""));
	return Result.ToLower();
}

// Returns the value of the first JSON field whose normalized key matches NormalizeKey(WantedKey).
// Handles actor_class / actorClass / ActorClass / ACTOR_CLASS etc. transparently.
static FString GetFieldNormalized(const TSharedPtr<FJsonObject>& Obj, const FString& WantedKey)
{
	if (!Obj.IsValid()) return TEXT("");
	FString NWanted = NormalizeKey(WantedKey);
	for (auto& Pair : Obj->Values)
	{
		const FString Key(*Pair.Key);
		if (NormalizeKey(Key) == NWanted)
		{
			FString Out;
			if (Pair.Value->TryGetString(Out)) return Out;
		}
	}
	return TEXT("");
}

namespace NwiroDispatcherPrivate
{
	static UClass* ResolveClassSubtype(const FString& Requested)
	{
		if (Requested.IsEmpty()) return nullptr;
		const TArray<FString> Names = {
			Requested,
			Requested.StartsWith(TEXT("U")) ? Requested : TEXT("U") + Requested
		};
		for (const FString& Name : Names)
		{
			if (UClass* Class = FindFirstObject<UClass>(*Name)) return Class;
		}
		if (UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *Requested))
			return Class;
		for (const TCHAR* Module : {TEXT("Engine"), TEXT("CoreUObject"), TEXT("UMG")})
		{
			if (UClass* Class = StaticLoadClass(
				UObject::StaticClass(), nullptr,
				*FString::Printf(TEXT("/Script/%s.%s"), Module, *Requested)))
			{
				return Class;
			}
		}
		return nullptr;
	}

	static UScriptStruct* ResolveStructSubtype(const FString& Requested)
	{
		if (Requested.IsEmpty()) return nullptr;
		const TArray<FString> Names = {
			Requested,
			Requested.StartsWith(TEXT("F")) ? Requested : TEXT("F") + Requested
		};
		for (const FString& Name : Names)
		{
			if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Name)) return Struct;
		}
		return LoadObject<UScriptStruct>(nullptr, *Requested);
	}

	static UEnum* ResolveEnumSubtype(const FString& Requested)
	{
		if (Requested.IsEmpty()) return nullptr;
		const TArray<FString> Names = {
			Requested,
			Requested.StartsWith(TEXT("E")) ? Requested : TEXT("E") + Requested
		};
		for (const FString& Name : Names)
		{
			if (UEnum* Enum = FindFirstObject<UEnum>(*Name)) return Enum;
		}
		return LoadObject<UEnum>(nullptr, *Requested);
	}

	static bool ResolveBaseType(
		const FString& RequestedBase,
		const FString& RequestedSubtype,
		FEdGraphPinType& OutType,
		FString& OutError)
	{
		const FString Base = RequestedBase.ToLower();
		if (Base == TEXT("bool") || Base == TEXT("boolean"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		else if (Base == TEXT("byte"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		else if (Base == TEXT("int") || Base == TEXT("integer") || Base == TEXT("int32"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
		else if (Base == TEXT("int64"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		else if (Base == TEXT("float"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		}
		else if (Base == TEXT("double") || Base == TEXT("real"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Base == TEXT("string") || Base == TEXT("fstring"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
		else if (Base == TEXT("name") || Base == TEXT("fname"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
		else if (Base == TEXT("text") || Base == TEXT("ftext"))
			OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
		else if (Base == TEXT("vector") || Base == TEXT("fvector"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (Base == TEXT("rotator") || Base == TEXT("frotator"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (Base == TEXT("transform") || Base == TEXT("ftransform"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		}
		else if (Base == TEXT("linearcolor") || Base == TEXT("color"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		}
		else if (Base == TEXT("object") || Base == TEXT("class"))
		{
			UClass* Class = ResolveClassSubtype(RequestedSubtype);
			if (!Class)
			{
				OutError = FString::Printf(
					TEXT("unresolved %s subtype '%s'"), *Base, *RequestedSubtype);
				return false;
			}
			OutType.PinCategory = Base == TEXT("class")
				? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
		}
		else if (Base == TEXT("struct"))
		{
			UScriptStruct* Struct = ResolveStructSubtype(RequestedSubtype);
			if (!Struct)
			{
				OutError = FString::Printf(
					TEXT("unresolved struct subtype '%s'"), *RequestedSubtype);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = Struct;
		}
		else if (Base == TEXT("enum"))
		{
			UEnum* Enum = ResolveEnumSubtype(RequestedSubtype);
			if (!Enum)
			{
				OutError = FString::Printf(
					TEXT("unresolved enum subtype '%s'"), *RequestedSubtype);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutType.PinSubCategoryObject = Enum;
		}
		else if (UClass* Class = ResolveClassSubtype(RequestedBase))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
		}
		else if (UScriptStruct* Struct = ResolveStructSubtype(RequestedBase))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = Struct;
		}
		else if (UEnum* Enum = ResolveEnumSubtype(RequestedBase))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutType.PinSubCategoryObject = Enum;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("unknown or unresolved Blueprint type '%s'"), *RequestedBase);
			return false;
		}
		return true;
	}

	static bool ParseParamType(
		const TSharedPtr<FJsonObject>& Param,
		FEdGraphPinType& OutType,
		FString& OutError)
	{
		if (Param->HasField(TEXT("subtype")) || Param->HasField(TEXT("container")))
		{
			OutError = TEXT("subtype/container belong inside structured type {base, subtype?, container?}");
			return false;
		}
		const TSharedPtr<FJsonValue>* TypeValue = Param->Values.Find(TEXT("type"));
		if (!TypeValue || !TypeValue->IsValid())
		{
			OutError = TEXT("missing required 'type'");
			return false;
		}

		FString Base;
		FString Subtype;
		FString Container;
		bool bRawNamedTypeAllowed = false;
		if ((*TypeValue)->Type == EJson::String)
		{
			bRawNamedTypeAllowed = true;
			Base = (*TypeValue)->AsString();
		}
		else if ((*TypeValue)->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> TypeObject = (*TypeValue)->AsObject();
			for (const auto& Pair : TypeObject->Values)
			{
				if (Pair.Key != TEXT("base") && Pair.Key != TEXT("subtype")
					&& Pair.Key != TEXT("container"))
				{
					OutError = FString::Printf(
						TEXT("unsupported type field '%s'"), *Pair.Key);
					return false;
				}
			}
			if (!TypeObject->TryGetStringField(TEXT("base"), Base))
			{
				OutError = TEXT("structured type requires string field 'base'");
				return false;
			}
			if (TypeObject->HasField(TEXT("subtype"))
				&& !TypeObject->TryGetStringField(TEXT("subtype"), Subtype))
			{
				OutError = TEXT("structured type 'subtype' must be a string");
				return false;
			}
			if (TypeObject->HasField(TEXT("container"))
				&& !TypeObject->TryGetStringField(TEXT("container"), Container))
			{
				OutError = TEXT("structured type 'container' must be a string");
				return false;
			}
		}
		else
		{
			OutError = TEXT("'type' must be a string or {base, subtype?, container?}");
			return false;
		}

		if (Base != Base.TrimStartAndEnd()
			|| Subtype != Subtype.TrimStartAndEnd()
			|| Container != Container.TrimStartAndEnd())
		{
			OutError = TEXT("type base, subtype, and container must not contain surrounding whitespace");
			return false;
		}

		if (Base.IsEmpty())
		{
			OutError = TEXT("type base cannot be empty");
			return false;
		}
		const FString LowerBase = Base.ToLower();
		static const TSet<FString> StructuredBases = {
			TEXT("bool"), TEXT("boolean"), TEXT("byte"),
			TEXT("int"), TEXT("integer"), TEXT("int32"), TEXT("int64"),
			TEXT("float"), TEXT("double"), TEXT("real"),
			TEXT("string"), TEXT("fstring"), TEXT("name"), TEXT("fname"),
			TEXT("text"), TEXT("ftext"), TEXT("vector"), TEXT("fvector"),
			TEXT("rotator"), TEXT("frotator"), TEXT("transform"), TEXT("ftransform"),
			TEXT("linearcolor"), TEXT("color"), TEXT("object"), TEXT("class"),
			TEXT("struct"), TEXT("enum")
		};
		if (!bRawNamedTypeAllowed && !StructuredBases.Contains(LowerBase))
		{
			OutError = FString::Printf(
				TEXT("structured type base '%s' is unsupported; raw named types must use a type string"),
				*Base);
			return false;
		}
		if ((LowerBase == TEXT("object") || LowerBase == TEXT("class")
			|| LowerBase == TEXT("struct") || LowerBase == TEXT("enum"))
			&& Subtype.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("type '%s' requires a resolvable subtype"), *Base);
			return false;
		}
		if (!Subtype.IsEmpty()
			&& LowerBase != TEXT("object") && LowerBase != TEXT("class")
			&& LowerBase != TEXT("struct") && LowerBase != TEXT("enum"))
		{
			OutError = FString::Printf(
				TEXT("type '%s' does not accept a subtype"), *Base);
			return false;
		}
		if (!ResolveBaseType(Base, Subtype, OutType, OutError)) return false;

		const FString LowerContainer = Container.ToLower();
		if (LowerContainer.IsEmpty() || LowerContainer == TEXT("none"))
		{
			OutType.ContainerType = EPinContainerType::None;
		}
		else if (LowerContainer == TEXT("array"))
		{
			OutType.ContainerType = EPinContainerType::Array;
		}
		else if (LowerContainer == TEXT("set"))
		{
			OutType.ContainerType = EPinContainerType::Set;
		}
		else if (LowerContainer == TEXT("map"))
		{
			OutError = TEXT("map dispatcher parameters are not supported");
			return false;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("unsupported container '%s' (use none, array, or set)"), *Container);
			return false;
		}

		if (!FBlueprintEditorUtils::IsPinTypeValid(OutType))
		{
			OutError = TEXT("resolved pin type is not valid for this Blueprint");
			return false;
		}
		return true;
	}

	static bool ValidateRequestedName(
		const FString& Requested,
		FName& OutName,
		FString& OutError)
	{
		if (Requested.IsEmpty() || Requested.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("name cannot be empty");
			return false;
		}
		if (Requested.TrimStartAndEnd() != Requested)
		{
			OutError = TEXT("name cannot have leading or trailing whitespace");
			return false;
		}
		const int32 MaximumLength = FMath::Min(
			NAME_SIZE - 1, FKismetNameValidator::GetMaximumNameLength());
		if (Requested.Len() > MaximumLength)
		{
			OutError = FString::Printf(
				TEXT("name is too long (%d characters; maximum is %d)"),
				Requested.Len(), MaximumLength);
			return false;
		}
		const FString InvalidCharacters =
			FString(INVALID_NAME_CHARACTERS) + UE_BLUEPRINT_INVALID_NAME_CHARACTERS;
		FText InvalidReason;
		if (!FName::IsValidXName(Requested, *InvalidCharacters, &InvalidReason))
		{
			OutError = InvalidReason.IsEmpty()
				? TEXT("name is not a valid Unreal identifier")
				: InvalidReason.ToString();
			return false;
		}
		OutName = FName(*Requested);
		if (OutName.IsNone())
		{
			OutError = TEXT("'None' is not a usable Blueprint name");
			return false;
		}
		return true;
	}

	static bool IsReservedEntryPin(const FName Name)
	{
		static const TSet<FName> Reserved = {
			UEdGraphSchema_K2::PN_Execute,
			UEdGraphSchema_K2::PN_Then,
			UEdGraphSchema_K2::PN_Self,
			UEdGraphSchema_K2::PN_ReturnValue,
			FName(TEXT("Target")),
			FName(TEXT("WorldContextObject"))
		};
		return Reserved.Contains(Name);
	}

	static void SetMutationError(
		FNwiroDispatcherMutation& Mutation,
		const TCHAR* Code,
		const FString& Error)
	{
		Mutation.ErrorCode = Code;
		Mutation.Error = Error;
	}

	static void MarkRollbackFailure(
		TArray<FNwiroDispatcherMutation>& Mutations,
		const FString& Error)
	{
		for (FNwiroDispatcherMutation& Mutation : Mutations)
		{
			if (!Mutation.bRolledBack) continue;
			Mutation.bRollbackVerified = false;
			Mutation.ErrorCode = TEXT("ROLLBACK_FAILED");
			if (!Error.IsEmpty())
			{
				Mutation.Error += Mutation.Error.IsEmpty()
					? Error : TEXT("; ") + Error;
			}
		}
	}

	static bool ValidateDispatcherInvariant(
		UBlueprint* BP,
		FNwiroDispatcherMutation& Mutation,
		UClass* ValidationClass,
		FString& OutError);

	static bool PrevalidateBatch(
		UBlueprint* BP,
		const TArray<TSharedPtr<FJsonValue>>& Items,
		TArray<FNwiroDispatcherMutation>& OutMutations,
		FString& OutMessage)
	{
		OutMutations.Reset();
		TArray<FString> Errors;
		TSet<FName> BatchNames;
		if (!BP)
		{
			OutMessage = TEXT("INVALID_DISPATCHER: Blueprint is null");
			return false;
		}
		if (Items.IsEmpty())
		{
			OutMessage = TEXT("INVALID_DISPATCHER: at least one dispatcher is required");
			return false;
		}

		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			FNwiroDispatcherMutation& Mutation = OutMutations.AddDefaulted_GetRef();
			const TSharedPtr<FJsonValue>& Item = Items[Index];
			if (!Item.IsValid() || Item->Type != EJson::Object
				|| !Item->AsObject().IsValid())
			{
				Mutation.RequestedName = FString::Printf(TEXT("item_%d"), Index);
				SetMutationError(
					Mutation, TEXT("INVALID_DISPATCHER"),
					TEXT("dispatcher entries must be objects with fields {name, params?}"));
				Errors.Add(Mutation.Error);
				continue;
			}

			const TSharedPtr<FJsonObject> Object = Item->AsObject();
			for (const auto& Pair : Object->Values)
			{
				if (Pair.Key != TEXT("name") && Pair.Key != TEXT("params"))
				{
					SetMutationError(
						Mutation, TEXT("INVALID_DISPATCHER"),
						FString::Printf(TEXT("unsupported dispatcher field '%s'"), *Pair.Key));
					break;
				}
			}
			if (!Mutation.ErrorCode.IsEmpty())
			{
				Errors.Add(Mutation.Error);
				continue;
			}
			if (!Object->TryGetStringField(TEXT("name"), Mutation.RequestedName))
			{
				SetMutationError(
					Mutation, TEXT("INVALID_DISPATCHER"),
					TEXT("dispatcher requires string field 'name'"));
				Errors.Add(Mutation.Error);
				continue;
			}
			FString NameError;
			if (!ValidateRequestedName(Mutation.RequestedName, Mutation.Name, NameError))
			{
				SetMutationError(Mutation, TEXT("INVALID_DISPATCHER"), NameError);
				Errors.Add(FString::Printf(TEXT("%s: %s"), *Mutation.RequestedName, *NameError));
				continue;
			}
			if (BatchNames.Contains(Mutation.Name))
			{
				SetMutationError(
					Mutation, TEXT("DUPLICATE_DISPATCHER"),
					FString::Printf(TEXT("duplicate dispatcher name '%s' in request"), *Mutation.RequestedName));
				Errors.Add(Mutation.Error);
				continue;
			}
			BatchNames.Add(Mutation.Name);

			const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
			if (Object->HasField(TEXT("params")))
			{
				if (!Object->TryGetArrayField(TEXT("params"), Params))
				{
					SetMutationError(
						Mutation, TEXT("INVALID_PARAMETER"),
						FString::Printf(TEXT("dispatcher '%s': params must be an array"), *Mutation.RequestedName));
					Errors.Add(Mutation.Error);
					continue;
				}
			}

			TSet<FName> ParamNames;
			if (Params)
			{
				for (int32 ParamIndex = 0; ParamIndex < Params->Num(); ++ParamIndex)
				{
					const TSharedPtr<FJsonValue>& ParamValue = (*Params)[ParamIndex];
					if (!ParamValue.IsValid() || ParamValue->Type != EJson::Object
						|| !ParamValue->AsObject().IsValid())
					{
						SetMutationError(
							Mutation, TEXT("INVALID_PARAMETER"),
							FString::Printf(TEXT("parameter %d must be an object"), ParamIndex));
						break;
					}
					const TSharedPtr<FJsonObject> ParamObject = ParamValue->AsObject();
					for (const auto& Pair : ParamObject->Values)
					{
						if (Pair.Key != TEXT("name") && Pair.Key != TEXT("type"))
						{
							SetMutationError(
								Mutation, TEXT("INVALID_PARAMETER"),
								FString::Printf(TEXT("parameter %d has unsupported field '%s'"), ParamIndex, *Pair.Key));
							break;
						}
					}
					if (!Mutation.ErrorCode.IsEmpty()) break;

					FString RequestedParamName;
					if (!ParamObject->TryGetStringField(TEXT("name"), RequestedParamName))
					{
						SetMutationError(
							Mutation, TEXT("INVALID_PARAMETER"),
							FString::Printf(TEXT("parameter %d requires string field 'name'"), ParamIndex));
						break;
					}
					FNwiroDispatcherParamSpec Param;
					FString ParamNameError;
					if (!ValidateRequestedName(RequestedParamName, Param.Name, ParamNameError)
						|| IsReservedEntryPin(Param.Name))
					{
						SetMutationError(
							Mutation, TEXT("INVALID_PARAMETER"),
							IsReservedEntryPin(Param.Name)
								? FString::Printf(TEXT("parameter name '%s' is reserved"), *RequestedParamName)
								: ParamNameError);
						break;
					}
					if (ParamNames.Contains(Param.Name))
					{
						SetMutationError(
							Mutation, TEXT("DUPLICATE_PARAMETER"),
							FString::Printf(TEXT("duplicate parameter name '%s'"), *RequestedParamName));
						break;
					}
					FString TypeError;
					if (!ParseParamType(ParamObject, Param.Type, TypeError))
					{
						SetMutationError(
							Mutation, TEXT("INVALID_TYPE"),
							FString::Printf(TEXT("parameter '%s': %s"), *RequestedParamName, *TypeError));
						break;
					}
					ParamNames.Add(Param.Name);
					Mutation.Params.Add(MoveTemp(Param));
				}
			}
			if (!Mutation.ErrorCode.IsEmpty())
			{
				Errors.Add(FString::Printf(TEXT("%s: %s"), *Mutation.RequestedName, *Mutation.Error));
				continue;
			}

			FBPVariableDescription* ExistingMember = FindDirectVariable(BP, Mutation.Name);
			const bool bExistingDelegate = ExistingMember
				&& ExistingMember->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
			int32 DelegateGraphCount = 0;
			FindSignatureGraph(BP, Mutation.Name, &DelegateGraphCount);
			if ((bExistingDelegate && DelegateGraphCount != 1)
				|| (!ExistingMember && DelegateGraphCount > 0)
				|| (ExistingMember && !bExistingDelegate && DelegateGraphCount > 0))
			{
				SetMutationError(
					Mutation, TEXT("MALFORMED_EXISTING_DISPATCHER"),
					FString::Printf(
						TEXT("'%s' has mismatched delegate member/signature graph residue; remove it before recreating"),
						*Mutation.RequestedName));
				Errors.Add(Mutation.Error);
				continue;
			}
			if (bExistingDelegate && DelegateGraphCount == 1)
			{
				FNwiroDispatcherMutation Existing;
				Existing.Name = Mutation.Name;
				Existing.RequestedName = Mutation.RequestedName;
				if (UK2Node_FunctionEntry* Entry =
					FindOnlyEntry(FindSignatureGraph(BP, Mutation.Name)))
				{
					for (const TSharedPtr<FUserPinInfo>& Pin : Entry->UserDefinedPins)
					{
						if (Pin.IsValid())
						{
							FNwiroDispatcherParamSpec& Param =
								Existing.Params.AddDefaulted_GetRef();
							Param.Name = Pin->PinName;
							Param.Type = Pin->PinType;
						}
					}
				}
				FString ExistingError;
				if (!ValidateDispatcherInvariant(
					BP, Existing, BP->SkeletonGeneratedClass, ExistingError))
				{
					SetMutationError(
						Mutation, TEXT("MALFORMED_EXISTING_DISPATCHER"),
						FString::Printf(
							TEXT("'%s' is an existing malformed dispatcher: %s; remove it before recreating"),
							*Mutation.RequestedName, *ExistingError));
					Errors.Add(Mutation.Error);
					continue;
				}
			}
			if (ExistingMember || DelegateGraphCount > 0)
			{
				SetMutationError(
					Mutation, TEXT("NAME_COLLISION"),
					FString::Printf(TEXT("name '%s' already exists"), *Mutation.RequestedName));
				Errors.Add(Mutation.Error);
				continue;
			}

			bool bOtherGraphCollision = false;
			TArray<UEdGraph*> AllGraphs;
			BP->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (Graph && Graph->GetFName() == Mutation.Name)
				{
					bOtherGraphCollision = true;
					break;
				}
			}
			UObject* DirectObject = FindObjectFast<UObject>(BP, Mutation.Name);
			FKismetNameValidator Validator(BP, NAME_None);
			if (bOtherGraphCollision || DirectObject
				|| Validator.IsValid(Mutation.Name.ToString()) != EValidatorResult::Ok)
			{
				SetMutationError(
					Mutation, TEXT("NAME_COLLISION"),
					FString::Printf(
						TEXT("name '%s' collides with an existing Blueprint member, graph, or object"),
						*Mutation.RequestedName));
				Errors.Add(Mutation.Error);
			}
		}

		if (!Errors.IsEmpty())
		{
			OutMessage = FString::Printf(
				TEXT("Dispatcher batch rejected before mutation: %s"),
				*FString::Join(Errors, TEXT("; ")));
			return false;
		}
		return true;
	}

	static bool ValidateReflectedSignature(
		UClass* Class,
		const FNwiroDispatcherMutation& Mutation,
		FString& OutError)
	{
		FMulticastDelegateProperty* Property =
			FindOwnedDelegateProperty(Class, Mutation.Name);
		if (!Property || Property->GetOwner<UClass>() != Class)
		{
			OutError = TEXT("owned multicast delegate property is missing");
			return false;
		}
		UFunction* Signature = Property->SignatureFunction.Get();
		if (!Signature)
		{
			OutError = TEXT("delegate SignatureFunction is null");
			return false;
		}
		const FName ExpectedSignatureName(
			*(Mutation.Name.ToString() + HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX));
		if (Signature->GetFName() != ExpectedSignatureName
			|| Signature->GetOuter() != Class
			|| !Signature->HasAnyFunctionFlags(FUNC_Delegate))
		{
			OutError = FString::Printf(
				TEXT("signature function owner/name/flags mismatch (expected %s)"),
				*ExpectedSignatureName.ToString());
			return false;
		}

		TArray<FProperty*> Parameters;
		for (TFieldIterator<FProperty> It(Signature, EFieldIterationFlags::None); It; ++It)
		{
			FProperty* Parameter = *It;
			if (Parameter->GetOwner<UFunction>() == Signature
				&& Parameter->HasAnyPropertyFlags(CPF_Parm)
				&& !Parameter->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Parameters.Add(Parameter);
			}
		}
		if (Parameters.Num() != Mutation.Params.Num())
		{
			OutError = FString::Printf(
				TEXT("reflected parameter count is %d, expected %d"),
				Parameters.Num(), Mutation.Params.Num());
			return false;
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			FEdGraphPinType ReflectedType;
			if (Parameters[Index]->GetFName() != Mutation.Params[Index].Name
				|| !Schema->ConvertPropertyToPinType(Parameters[Index], ReflectedType)
				|| !ArePayloadTypesEquivalent(
					ReflectedType, Mutation.Params[Index].Type))
			{
				OutError = FString::Printf(
					TEXT("reflected parameter %d does not match requested name/type/order"),
					Index);
				return false;
			}
		}
		return true;
	}

	static bool ValidateDispatcherInvariant(
		UBlueprint* BP,
		FNwiroDispatcherMutation& Mutation,
		UClass* ValidationClass,
		FString& OutError)
	{
		FBPVariableDescription* Member = FindDirectVariable(BP, Mutation.Name);
		if (!Member || Member->VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate)
		{
			OutError = TEXT("exact multicast delegate member is missing");
			return false;
		}
		int32 GraphCount = 0;
		UEdGraph* Graph = FindSignatureGraph(BP, Mutation.Name, &GraphCount);
		Mutation.bSignatureGraphPresent = GraphCount == 1 && Graph != nullptr;
		if (!Mutation.bSignatureGraphPresent || Graph->GetOuter() != BP
			|| !Graph->GetSchema() || !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
		{
			OutError = TEXT("exact Blueprint-owned K2 signature graph is missing or duplicated");
			return false;
		}
		Mutation.Graph = Graph;

		UK2Node_FunctionEntry* Entry = FindOnlyEntry(Graph);
		if (!Entry || !Entry->IsEditable())
		{
			OutError = TEXT("signature graph must contain exactly one editable FunctionEntry");
			return false;
		}
		if (Entry->UserDefinedPins.Num() != Mutation.Params.Num())
		{
			OutError = FString::Printf(
				TEXT("signature entry has %d user pins, expected %d"),
				Entry->UserDefinedPins.Num(), Mutation.Params.Num());
			return false;
		}
		for (int32 Index = 0; Index < Mutation.Params.Num(); ++Index)
		{
			const TSharedPtr<FUserPinInfo>& Pin = Entry->UserDefinedPins[Index];
			if (!Pin.IsValid()
				|| Pin->PinName != Mutation.Params[Index].Name
				|| Pin->DesiredPinDirection != EGPD_Output
				|| Pin->PinType != Mutation.Params[Index].Type)
			{
				OutError = FString::Printf(
					TEXT("signature entry pin %d does not match requested name/type/order"),
					Index);
				return false;
			}
		}
		return ValidateReflectedSignature(ValidationClass, Mutation, OutError);
	}

	static bool RollbackMutations(
		UBlueprint* BP,
		TArray<FNwiroDispatcherMutation>& Mutations,
		FString& OutError)
	{
		TArray<FName> Members;
		bool bRemovedGraph = false;
		for (int32 Index = Mutations.Num() - 1; Index >= 0; --Index)
		{
			FNwiroDispatcherMutation& Mutation = Mutations[Index];
			UEdGraph* Graph = Mutation.Graph.Get();
			if (Graph && Graph->GetOuter() == BP)
			{
				FBlueprintEditorUtils::RemoveGraph(
					BP, Graph, EGraphRemoveFlags::MarkTransient);
				bRemovedGraph = true;
			}
			if (Mutation.bMemberCreated)
			{
				Members.AddUnique(Mutation.Name);
			}
			if (Mutation.bMemberCreated || Graph)
			{
				Mutation.bRolledBack = true;
			}
			Mutation.bCreated = false;
			Mutation.bSignatureGraphPresent = false;
			Mutation.bSkeletonSignatureValid = false;
			Mutation.GeneratedSignatureValid.Reset();
		}

		if (!Members.IsEmpty())
		{
			FBlueprintEditorUtils::BulkRemoveMemberVariables(BP, Members);
		}
		else if (bRemovedGraph)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}

		for (const FNwiroDispatcherMutation& Mutation : Mutations)
		{
			int32 GraphCount = 0;
			FindSignatureGraph(BP, Mutation.Name, &GraphCount);
			if (FindDirectVariable(BP, Mutation.Name) || GraphCount != 0
				|| FindOwnedDelegateProperty(BP->SkeletonGeneratedClass, Mutation.Name)
				|| FindOwnedSignatureFunction(BP->SkeletonGeneratedClass, Mutation.Name))
			{
				OutError = FString::Printf(
					TEXT("rollback postcondition failed for '%s'"),
					*Mutation.Name.ToString());
				MarkRollbackFailure(Mutations, OutError);
				return false;
			}
		}
		for (FNwiroDispatcherMutation& Mutation : Mutations)
		{
			if (Mutation.bRolledBack) Mutation.bRollbackVerified = true;
		}
		return true;
	}

	static bool VerifyRollbackAbsence(
		UBlueprint* BP,
		const TArray<FNwiroDispatcherMutation>& Mutations,
		const bool bCheckGeneratedClass,
		FString& OutError)
	{
		for (const FNwiroDispatcherMutation& Mutation : Mutations)
		{
			int32 GraphCount = 0;
			FindSignatureGraph(BP, Mutation.Name, &GraphCount);
			if (FindDirectVariable(BP, Mutation.Name) || GraphCount != 0
				|| FindOwnedDelegateProperty(BP->SkeletonGeneratedClass, Mutation.Name)
				|| FindOwnedSignatureFunction(BP->SkeletonGeneratedClass, Mutation.Name)
				|| (bCheckGeneratedClass
					&& (FindOwnedDelegateProperty(BP->GeneratedClass, Mutation.Name)
						|| FindOwnedSignatureFunction(BP->GeneratedClass, Mutation.Name))))
			{
				OutError = FString::Printf(
					TEXT("stale dispatcher state remains after rollback for '%s'"),
					*Mutation.Name.ToString());
				return false;
			}
		}
		return true;
	}
}

#include "NwiroVariableSupport.inl"
#include "NwiroFunctionSupport.inl"

static bool IsKnownEditBlueprintKey(const FString& Key)
{
	static const TSet<FString> KnownKeys = {
		TEXT("blueprint"), TEXT("name"), TEXT("path"), TEXT("action"),
		TEXT("graph"),
		TEXT("compile"),
		TEXT("parentClass"),
		TEXT("parent_class"),
		TEXT("actions"),
		TEXT("reparent"),
		TEXT("add_variables"),
		TEXT("remove_variables"),
		TEXT("rename_variables"),
		TEXT("add_components"),
		TEXT("remove_components"),
		TEXT("set_component_properties"),
		TEXT("add_functions"),
		TEXT("remove_functions"),
		TEXT("add_custom_events"),
		TEXT("add_event_dispatchers"),
		TEXT("add_interfaces"),
		TEXT("remove_interfaces"),
		TEXT("add_nodes"),
		TEXT("remove_nodes"),
		TEXT("connect_pins"),
		TEXT("break_connections"),
		TEXT("set_pin_defaults"),
	};
	return KnownKeys.Contains(Key);
}

static bool IsEditBlueprintOperationKey(const FString& Key)
{
	static const TSet<FString> OperationKeys = {
		TEXT("reparent"),
		TEXT("add_variables"),
		TEXT("remove_variables"),
		TEXT("rename_variables"),
		TEXT("add_components"),
		TEXT("remove_components"),
		TEXT("set_component_properties"),
		TEXT("add_functions"),
		TEXT("remove_functions"),
		TEXT("add_custom_events"),
		TEXT("add_event_dispatchers"),
		TEXT("add_interfaces"),
		TEXT("remove_interfaces"),
		TEXT("add_nodes"),
		TEXT("remove_nodes"),
		TEXT("connect_pins"),
		TEXT("break_connections"),
		TEXT("set_pin_defaults"),
	};
	return OperationKeys.Contains(Key);
}

static int32 GetArrayCount(const TSharedPtr<FJsonObject>& Obj, const FString& Key)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	return Obj.IsValid() && Obj->TryGetArrayField(Key, Arr) && Arr ? Arr->Num() : 0;
}

// Resolve an edit_blueprint op key to its item array. Accepts a real JSON
// array, or a JSON STRING that parses to an array (models frequently stringify
// nested arrays — same tolerance as edit_material's GetArrayFieldFlexible in
// NwiroMaterialTools.cpp). Returns false with OutError set when the key is
// PRESENT but unusable; the caller reports a per-op failure instead of
// silently skipping the op (the old silent skip let compile+save run and
// return success:true with zero mutations).
static bool ResolveOpArray(const TSharedPtr<FJsonObject>& Cmd, const FString& Key,
	TArray<TSharedPtr<FJsonValue>>& OutItems, FString& OutNormalizeWarning, FString& OutError)
{
	OutNormalizeWarning.Empty();

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Cmd->TryGetArrayField(Key, Arr) && Arr)
	{
		OutItems = *Arr;
		return true;
	}

	const TSharedPtr<FJsonValue> Val = Cmd->TryGetField(Key);
	if (Val.IsValid() && Val->Type == EJson::Null)
	{
		OutError = FString::Printf(TEXT("ERROR: null is not a valid value for '%s'; omit the key to skip this operation"), *Key);
		return false;
	}
	if (Val.IsValid() && Val->Type == EJson::Object)
	{
		// Models frequently send a SINGLE item object where an array of items
		// is expected. The container shape is unambiguous — wrap it; item-level
		// validation still applies to the wrapped object.
		OutItems.Add(MakeShareable(new FJsonValueObject(Val->AsObject())));
		OutNormalizeWarning = FString::Printf(TEXT("normalized single object to array; use %s:[{...}]"), *Key);
		return true;
	}
	if (Val.IsValid() && Val->Type == EJson::String)
	{
		TArray<TSharedPtr<FJsonValue>> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Val->AsString());
		if (FJsonSerializer::Deserialize(Reader, Parsed))
		{
			OutItems = MoveTemp(Parsed);
			OutNormalizeWarning = TEXT("normalized stringified array — send a real JSON array next time");
			return true;
		}
		TSharedPtr<FJsonObject> ParsedObj;
		TSharedRef<TJsonReader<>> ObjReader = TJsonReaderFactory<>::Create(Val->AsString());
		if (FJsonSerializer::Deserialize(ObjReader, ParsedObj) && ParsedObj.IsValid())
		{
			OutItems.Add(MakeShareable(new FJsonValueObject(ParsedObj)));
			OutNormalizeWarning = FString::Printf(TEXT("normalized stringified object to a one-element array; use %s:[{...}]"), *Key);
			return true;
		}
		OutError = FString::Printf(TEXT("ERROR: value for '%s' is a string that does not parse as a JSON array — send a real JSON array of objects"), *Key);
		return false;
	}

	OutError = FString::Printf(TEXT("ERROR: value for '%s' must be a JSON array of objects, e.g. %s:[{...}] — got a non-array value"), *Key, *Key);
	return false;
}

// ============================================================
// APPLY COMPONENT PROPERTY (shared helper)
// ============================================================

bool FNwiroBlueprintTools::ApplyComponentProperty(
	UActorComponent* CompTemplate,
	const FString& Key,
	const FString& Value,
	const FString& CompName,
	TArray<FString>* OutErrors,
	TArray<FString>* OutWarnings)
{
	if (!CompTemplate) return false;

	// Property name aliases: maps LLM-normalized keys to UE-normalized property names.
	// Needed when word order differs (NormalizeKey can't fix that).
	// e.g. "mass_override_in_kg" → "massoverrideinkg" but UE has "MassInKgOverride" → "massinkgoverride"
	static const TMap<FString, FString> PropAliases = {
		{ TEXT("massoverrideinkg"), TEXT("massinkgoverride") },
	};

	FString NKey = NormalizeKey(Key);
	if (const FString* Aliased = PropAliases.Find(NKey)) NKey = *Aliased;

	// Mesh aliases for shorthand values (e.g. "cube" → full engine path)
	static const TMap<FString, FString> MeshAliases = {
		{ TEXT("sphere"),   TEXT("/Engine/BasicShapes/Sphere.Sphere") },
		{ TEXT("cube"),     TEXT("/Engine/BasicShapes/Cube.Cube") },
		{ TEXT("cylinder"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder") },
		{ TEXT("cone"),     TEXT("/Engine/BasicShapes/Cone.Cone") },
		{ TEXT("plane"),    TEXT("/Engine/BasicShapes/Plane.Plane") },
	};

	// === Special case: StaticMesh — needs LoadObject ===
	if (NKey == TEXT("staticmesh") || NKey == TEXT("mesh"))
	{
		FString MeshPath = Value;
		if (const FString* Alias = MeshAliases.Find(MeshPath.ToLower())) MeshPath = *Alias;
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(CompTemplate))
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
			if (!Mesh && !MeshPath.StartsWith(TEXT("/")))
				Mesh = LoadObject<UStaticMesh>(nullptr, *(TEXT("/Engine/BasicShapes/") + MeshPath));
			if (Mesh) { SMC->SetStaticMesh(Mesh); return true; }
			if (OutErrors) OutErrors->Add(FString::Printf(TEXT("StaticMesh not found: %s"), *MeshPath));
		}
		return false;
	}

	// === Special case: SkeletalMesh — needs LoadObject ===
	if (NKey == TEXT("skeletalmesh") || NKey == TEXT("skeletalmeshasset"))
	{
		if (USkeletalMeshComponent* SkMC = Cast<USkeletalMeshComponent>(CompTemplate))
		{
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Value);
			if (Mesh) { SkMC->SetSkeletalMeshAsset(Mesh); return true; }
			if (OutErrors) OutErrors->Add(FString::Printf(TEXT("SkeletalMesh not found: %s"), *Value));
		}
		return false;
	}

	// === Level 1: Direct reflection on the component class ===
	for (TFieldIterator<FProperty> It(CompTemplate->GetClass()); It; ++It)
	{
		FString PropName = It->GetName();
		bool bMatch = NormalizeKey(PropName) == NKey;
		// Also match without leading 'b' (UE bool convention: bEnableGravity → enablegravity)
		if (!bMatch && PropName.StartsWith(TEXT("b")) && PropName.Len() > 1)
			bMatch = NormalizeKey(PropName.Mid(1)) == NKey;

		if (bMatch)
		{
			void* ValuePtr = It->ContainerPtrToValuePtr<void>(CompTemplate);
			if (It->ImportText_Direct(*Value, ValuePtr, CompTemplate, PPF_None))
				return true;

			// Object property fallback: try LoadObject
			if (FObjectProperty* ObjProp = CastField<FObjectProperty>(*It))
			{
				if (UObject* Obj = LoadObject<UObject>(nullptr, *Value))
				{
					ObjProp->SetObjectPropertyValue(ValuePtr, Obj);
					return true;
				}
			}
			if (OutErrors) OutErrors->Add(FString::Printf(TEXT("Failed to set %s.%s = %s"), *CompName, *Key, *Value));
			return false;
		}
	}

	// === Level 2: Deep struct reflection — search inside struct properties ===
	// Handles nested structs like FBodyInstance (enable_gravity, mass_override_in_kg, etc.)
	for (TFieldIterator<FStructProperty> StructIt(CompTemplate->GetClass()); StructIt; ++StructIt)
	{
		void* StructPtr = StructIt->ContainerPtrToValuePtr<void>(CompTemplate);

		for (TFieldIterator<FProperty> SubIt(StructIt->Struct); SubIt; ++SubIt)
		{
			FString SubName = SubIt->GetName();
			bool bMatch = NormalizeKey(SubName) == NKey;
			if (!bMatch && SubName.StartsWith(TEXT("b")) && SubName.Len() > 1)
				bMatch = NormalizeKey(SubName.Mid(1)) == NKey;

			if (bMatch)
			{
				void* SubValuePtr = SubIt->ContainerPtrToValuePtr<void>(StructPtr);
				if (SubIt->ImportText_Direct(*Value, SubValuePtr, nullptr, PPF_None))
					return true;
				if (OutErrors) OutErrors->Add(FString::Printf(TEXT("Failed to set %s.%s = %s (in struct %s)"), *CompName, *Key, *Value, *StructIt->GetName()));
				return false;
			}
		}
	}

	// Nothing matched
	if (OutWarnings) OutWarnings->Add(FString::Printf(TEXT("Unrecognized property '%s' on component '%s' — ignored"), *Key, *CompName));
	return false;
}

// ============================================================
// FIND BLUEPRINTS
// ============================================================

FString FNwiroBlueprintTools::FindBlueprints(const FString& SearchTerm)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.GetObjectPathString();

		if (!SearchTerm.IsEmpty() && !Name.Contains(SearchTerm, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), Path);
		Obj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

		// Try to get parent class info from tag
		FString ParentClass;
		if (Asset.GetTagValue(FName("ParentClass"), ParentClass))
		{
			Obj->SetStringField(TEXT("parentClass"), ParentClass);
		}

		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetArrayField(TEXT("blueprints"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// READ BLUEPRINT
// ============================================================

FString FNwiroBlueprintTools::ReadBlueprint(const FString& ArgsJson)
{
	// Accept either a plain asset path string or a JSON object with assetPath (+ optional graph).
	FString AssetPath;
	FString GraphFilter;
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Parsed) && Parsed.IsValid())
		{
			AssetPath = Parsed->GetStringField(TEXT("assetPath"));
			if (Parsed->HasField(TEXT("graph")))
				GraphFilter = Parsed->GetStringField(TEXT("graph"));
		}
		else
		{
			AssetPath = ArgsJson;
		}
	}

	FString LoadError;
	UBlueprint* BP = LoadBP(AssetPath, &LoadError);
	if (!BP) return NwiroAssetLoadFailure(AssetPath, LoadError, TEXT("Blueprint"));

	// If caller asked for a specific function graph, return its full node/pin data.
	if (!GraphFilter.IsEmpty())
	{
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> GObj = SerializeGraph(Graph, true);
				if (!GObj.IsValid())
					return TEXT("{\"error\": \"Failed to serialize graph\"}");
				FString Out;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
				FJsonSerializer::Serialize(GObj.ToSharedRef(), W);
				return Out;
			}
		}
		// Also check event graphs
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph && Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> GObj = SerializeGraph(Graph, true);
				if (!GObj.IsValid())
					return TEXT("{\"error\": \"Failed to serialize graph\"}");
				FString Out;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
				FJsonSerializer::Serialize(GObj.ToSharedRef(), W);
				return Out;
			}
		}
		return FString::Printf(TEXT("{\"error\": \"Graph not found: %s\"}"), *GraphFilter);
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetStringField(TEXT("name"), BP->GetName());
	Root->SetStringField(TEXT("path"), BP->GetPathName());

	if (BP->ParentClass)
	{
		Root->SetStringField(TEXT("parentClass"), BP->ParentClass->GetName());
	}

	// openable: replicate the editor's "Blueprint could not be loaded because
	// it derives from an invalid class" gate so acceptance can verify
	// openability through an MCP tool — no execute_python needed.
	{
		const bool bParentOk = (BP->ParentClass != nullptr) && BP->ParentClass->IsValidLowLevel();
		UClass* GenCls = BP->GeneratedClass;
		const bool bGenOk = (GenCls != nullptr) && GenCls->IsValidLowLevel()
			&& (GenCls->GetSuperClass() != nullptr);
		const bool bOpenable = bParentOk && bGenOk;
		Root->SetBoolField(TEXT("openable"), bOpenable);
		if (!bOpenable)
		{
			Root->SetStringField(TEXT("openableError"),
				!bParentOk
					? TEXT("ParentClass is null/invalid — editor will show 'derives from an invalid class'")
					: TEXT("GeneratedClass or its super struct is null/invalid — BP will not open"));
		}
	}

	// Variables and event dispatchers. Readback intentionally uses the union
	// of delegate members and signature graphs so legacy graph-only/member-only
	// corruption is observable instead of disappearing from structural output.
	TArray<TSharedPtr<FJsonValue>> VarArr;
	TArray<TSharedPtr<FJsonValue>> DispatcherArr;
	TSet<FName> DispatcherNames;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		const bool bIsDispatcher = Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
		if (bIsDispatcher)
		{
			DispatcherNames.Add(Var.VarName);
			continue;
		}
		TSharedPtr<FJsonObject> VObj = SerializeVariable(BP, Var.VarName);
		if (VObj.IsValid())
		{
			VarArr.Add(MakeShareable(new FJsonValueObject(VObj.ToSharedRef())));
		}
	}
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (Graph) DispatcherNames.Add(Graph->GetFName());
	}
	TArray<FName> SortedDispatcherNames;
	for (const FName Name : DispatcherNames) SortedDispatcherNames.Add(Name);
	SortedDispatcherNames.Sort(
		[](const FName A, const FName B) { return A.LexicalLess(B); });
	for (const FName Name : SortedDispatcherNames)
	{
		using namespace NwiroDispatcherPrivate;
		FNwiroDispatcherMutation Observed;
		Observed.Name = Name;
		Observed.RequestedName = Name.ToString();
		int32 GraphCount = 0;
		UEdGraph* Graph = FindSignatureGraph(BP, Name, &GraphCount);
		Observed.bSignatureGraphPresent = GraphCount == 1 && Graph != nullptr;
		if (UK2Node_FunctionEntry* Entry = FindOnlyEntry(Graph))
		{
			for (const TSharedPtr<FUserPinInfo>& Pin : Entry->UserDefinedPins)
			{
				if (!Pin.IsValid()) continue;
				FNwiroDispatcherParamSpec& Param =
					Observed.Params.AddDefaulted_GetRef();
				Param.Name = Pin->PinName;
				Param.Type = Pin->PinType;
			}
		}

		FString SkeletonError;
		Observed.bSkeletonSignatureValid = ValidateDispatcherInvariant(
			BP, Observed, BP->SkeletonGeneratedClass, SkeletonError);
		const bool bGeneratedStateIsCurrent =
			BP->Status == EBlueprintStatus::BS_UpToDate
			|| BP->Status == EBlueprintStatus::BS_UpToDateWithWarnings;
		if (bGeneratedStateIsCurrent && BP->GeneratedClass)
		{
			FString GeneratedError;
			Observed.GeneratedSignatureValid = ValidateDispatcherInvariant(
				BP, Observed, BP->GeneratedClass, GeneratedError);
			if (!Observed.GeneratedSignatureValid.GetValue() && SkeletonError.IsEmpty())
			{
				SkeletonError = TEXT("generated signature: ") + GeneratedError;
			}
		}

		TSharedRef<FJsonObject> Dispatcher = MakeShared<FJsonObject>();
		Dispatcher->SetStringField(TEXT("name"), Name.ToString());
		Dispatcher->SetBoolField(
			TEXT("signatureGraphPresent"), Observed.bSignatureGraphPresent);
		Dispatcher->SetBoolField(TEXT("skeletonSignatureValid"),
			Observed.bSkeletonSignatureValid);
		if (Observed.GeneratedSignatureValid.IsSet())
		{
			Dispatcher->SetBoolField(TEXT("generatedSignatureValid"),
				Observed.GeneratedSignatureValid.GetValue());
		}
		else
		{
			Dispatcher->SetField(
				TEXT("generatedSignatureValid"), MakeShared<FJsonValueNull>());
		}
		Dispatcher->SetBoolField(
			TEXT("signatureValid"),
			Observed.GeneratedSignatureValid.IsSet()
				? Observed.GeneratedSignatureValid.GetValue()
				: Observed.bSkeletonSignatureValid);
		TArray<TSharedPtr<FJsonValue>> Params;
		for (const FNwiroDispatcherParamSpec& Param : Observed.Params)
		{
			Params.Add(MakeShared<FJsonValueObject>(ParamToJson(Param)));
		}
		Dispatcher->SetArrayField(TEXT("params"), Params);
		if (!Observed.bSkeletonSignatureValid
			|| (Observed.GeneratedSignatureValid.IsSet()
				&& !Observed.GeneratedSignatureValid.GetValue()))
		{
			if (!FindDirectVariable(BP, Name))
				SkeletonError = TEXT("signature graph exists without a delegate member");
			else if (GraphCount == 0)
				SkeletonError = TEXT("delegate member has no registered signature graph");
			else if (GraphCount > 1)
				SkeletonError = TEXT("delegate has duplicate registered signature graphs");
			Dispatcher->SetStringField(TEXT("malformedReason"), SkeletonError);
		}
		DispatcherArr.Add(MakeShared<FJsonValueObject>(Dispatcher));
	}
	Root->SetArrayField(TEXT("variables"), VarArr);
	Root->SetArrayField(TEXT("eventDispatchers"), DispatcherArr);

	// Components (from SCS)
	TArray<TSharedPtr<FJsonValue>> CompArr;
	if (BP->SimpleConstructionScript)
	{
		for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (SCSNode && SCSNode->ComponentTemplate)
			{
				TSharedPtr<FJsonObject> CObj = SerializeComponent(
					SCSNode->ComponentTemplate, SCSNode->GetVariableName());
				if (CObj.IsValid())
				{
					CompArr.Add(MakeShareable(new FJsonValueObject(CObj.ToSharedRef())));
				}
			}
		}
	}
	Root->SetArrayField(TEXT("components"), CompArr);

	// Functions
	TArray<TSharedPtr<FJsonValue>> FuncArr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			TSharedRef<FJsonObject> FObj = MakeShareable(new FJsonObject());
			FObj->SetStringField(TEXT("name"), Graph->GetName());
			FuncArr.Add(MakeShareable(new FJsonValueObject(FObj)));
		}
	}
	Root->SetArrayField(TEXT("functions"), FuncArr);

	// Event Graphs
	TArray<TSharedPtr<FJsonValue>> GraphArr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		TSharedPtr<FJsonObject> GObj = SerializeGraph(Graph, true);
		if (GObj.IsValid())
		{
			GraphArr.Add(MakeShareable(new FJsonValueObject(GObj.ToSharedRef())));
		}
	}
	Root->SetArrayField(TEXT("eventGraphs"), GraphArr);

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> IntArr;
	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		if (Iface.Interface)
		{
			TSharedRef<FJsonObject> IObj = MakeShareable(new FJsonObject());
			IObj->SetStringField(TEXT("name"), Iface.Interface->GetName());
			IntArr.Add(MakeShareable(new FJsonValueObject(IObj)));
		}
	}
	Root->SetArrayField(TEXT("interfaces"), IntArr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// EDIT BLUEPRINT (Main Entry)
// ============================================================

// Resolve and validate all requested direct memberships before an undo transaction,
// compilation, or another edit operation can touch the target.
static UClass* ResolveInterfaceClass(const FString& In);
static bool PrepareInterfaceRemoval(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items,
	TArray<UClass*>& Classes, FString& Error)
{
	Classes.Reset();
	if (!BP) { Error = TEXT("remove_interfaces requires an existing Blueprint"); return false; }
	if (Items.IsEmpty()) { Error = TEXT("At least one interface is required"); return false; }
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const auto& Item = Items[Index];
		FString Name;
		if (Item.IsValid() && Item->Type == EJson::String) Name = Item->AsString();
		else if (Item.IsValid() && Item->Type == EJson::Object && Item->AsObject().IsValid())
			Item->AsObject()->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty())
		{
			Error = FString::Printf(TEXT("Interface %d must be a nonempty name or {name:string}"), Index);
			return false;
		}
		UClass* Class = ResolveInterfaceClass(Name);
		if (!Class || !BP->ImplementedInterfaces.ContainsByPredicate(
			[Class](const FBPInterfaceDescription& Desc) { return Desc.Interface == Class; }))
		{
			Error = FString::Printf(TEXT("Interface %d is not directly implemented by this Blueprint: %s"), Index, *Name);
			return false;
		}
		if (Classes.Contains(Class))
		{
			Error = FString::Printf(TEXT("Interface %d repeats an earlier resolved interface: %s"), Index, *Name);
			return false;
		}
		Classes.Add(Class);
	}
	return true;
}

static FString InterfaceRemovalResponse(UBlueprint* BP, bool bSuccess, const FString& Message, int32 Removed)
{
	const auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(bSuccess ? TEXT("message") : TEXT("error"), Message);
	Result->SetNumberField(TEXT("removed"), Removed);
	Result->SetBoolField(TEXT("savedToDisk"), false);
	TArray<TSharedPtr<FJsonValue>> Dirty;
	if (BP && BP->GetOutermost()->IsDirty()) Dirty.Add(MakeShared<FJsonValueString>(BP->GetOutermost()->GetName()));
	Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
	FString Out;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out));
	return Out;
}

static FString BlueprintPreflightError(const FString& Error)
{
	const auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Error);
	Result->SetBoolField(TEXT("savedToDisk"), false);
	FString Out;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out));
	return Out;
}

static bool PrepareVariableNodeTargets(const TArray<TSharedPtr<FJsonValue>>& Items, TArray<FName>& Names, FString& Error)
{
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const auto& Item = Items[Index];
		if (!Item.IsValid() || Item->Type != EJson::Object || !Item->AsObject().IsValid()) continue;
		const auto& Obj = Item->AsObject();
		FString Type;
		for (const TCHAR* Key : { TEXT("type"), TEXT("node"), TEXT("nodeType"), TEXT("node_type"), TEXT("nodetype"), TEXT("kind"), TEXT("nodeClass"), TEXT("class") })
		{
			if (Obj->HasField(Key)) { Obj->TryGetStringField(Key, Type); break; }
		}
		if (!Type.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase) && !Type.Equals(TEXT("GetVariable"), ESearchCase::IgnoreCase)
			&& !Type.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase) && !Type.Equals(TEXT("SetVariable"), ESearchCase::IgnoreCase)) continue;
		FString Name;
		if (!Obj->TryGetStringField(TEXT("variable"), Name) || Name.IsEmpty() || Name.Len() >= NAME_SIZE || FName(*Name).IsNone())
		{
			Error = FString::Printf(TEXT("add_nodes[%d] %s requires a nonempty 'variable' identifying a self member"), Index, *Type);
			return false;
		}
		Names.Add(FName(*Name));
	}
	return true;
}

static bool ValidateVariableNodeTargets(UBlueprint* BP, UClass* Parent, const TSharedPtr<FJsonObject>& Cmd,
	const TArray<FNwiroPreparedVariable>& Variables, const TArray<FName>& Names, FString& Error)
{
	if (Names.IsEmpty()) return true;
	TSet<FName> Available;
	if (Parent) for (TFieldIterator<FProperty> It(Parent); It; ++It) Available.Add(It->GetFName());
	if (BP)
	{
		for (const FBPVariableDescription& Var : BP->NewVariables) Available.Add(Var.VarName);
		if (BP->SimpleConstructionScript) for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			if (Node) Available.Add(Node->GetVariableName());
		if (const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP))
		{
			if (WidgetBP->WidgetTree) WidgetBP->WidgetTree->ForEachWidget([&](UWidget* Widget)
			{
				if (Widget && Widget->bIsVariable) Available.Add(Widget->GetFName());
			});
		}
	}
	for (const auto& Var : Variables) Available.Add(Var.Name);
	// Model the existing operation order for names changed before add_nodes.
	for (const TCHAR* Key : { TEXT("remove_variables"), TEXT("rename_variables"), TEXT("add_components"), TEXT("remove_components") })
	{
		if (!Cmd->HasField(Key)) continue;
		TArray<TSharedPtr<FJsonValue>> Items;
		FString Warning;
		if (!ResolveOpArray(Cmd, Key, Items, Warning, Error)) return false;
		for (const auto& Item : Items)
		{
			if (!Item.IsValid()) continue;
			FString Name;
			const auto Obj = Item->Type == EJson::Object ? Item->AsObject() : nullptr;
			if (Item->Type == EJson::String) Name = Item->AsString();
			else if (Obj.IsValid()) Obj->TryGetStringField(TEXT("name"), Name);
			if (FString(Key) == TEXT("rename_variables"))
			{
				FString OldName, NewName;
				if (Obj.IsValid() && Obj->TryGetStringField(TEXT("old"), OldName) && Obj->TryGetStringField(TEXT("new"), NewName)
					&& !OldName.IsEmpty() && !NewName.IsEmpty() && Available.Remove(FName(*OldName))) Available.Add(FName(*NewName));
			}
			else if (FString(Key) == TEXT("add_components"))
			{
				if (Name.IsEmpty() && Obj.IsValid()) Name = GetFieldNormalized(Obj, TEXT("componentname"));
				if (!Name.IsEmpty()) Available.Add(FName(*Name));
			}
			else if (!Name.IsEmpty()) Available.Remove(FName(*Name));
		}
	}
	for (const FName& Name : Names)
	{
		if (!Available.Contains(Name))
		{
			Error = FString::Printf(TEXT("Variable node target is not a self member after the requested declarations/removals/renames: %s"), *Name.ToString());
			return false;
		}
	}
	return true;
}

bool FNwiroBlueprintTools::ValidateBlueprintRequest(UBlueprint* BP,
    const TSharedPtr<FJsonObject>& Cmd, const FString& GraphName,
    const NwiroEnhancedInputContract::FPreparedActions& PreparedActions)
{
    check(NwiroBlueprintBatch::Active && NwiroBlueprintBatch::Active->bStaging);
    TArray<FNwiroPreparedVariable> Variables;
    TArray<FNwiroPreparedFunction> Functions;
    FNwiroUnitLedger VariableLedger, FunctionLedger;
    const TArray<TSharedPtr<FJsonValue>>* VariableItems = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* FunctionItems = nullptr;
    if (Cmd->TryGetArrayField(TEXT("add_variables"), VariableItems)
        && (!NwiroVariables::Prepare(*VariableItems, Variables, VariableLedger)
            || !NwiroVariables::ValidateTarget(BP, Variables, VariableLedger)))
        NwiroBlueprintBatch::Reject(TEXT("Prepared variable batch rejected on staging target"));
    if (Cmd->TryGetArrayField(TEXT("add_functions"), FunctionItems)
        && (!NwiroFunctions::Prepare(*FunctionItems, Functions, FunctionLedger)
            || !NwiroFunctions::ValidateTarget(BP, Functions, FunctionLedger)))
        NwiroBlueprintBatch::Reject(TEXT("Prepared function batch rejected on staging target"));
    auto ValidateType = [&](const TSharedPtr<FJsonObject>& Item, bool bVariable = false) -> bool
    {
        const TSharedPtr<FJsonValue>* Value = Item->Values.Find(TEXT("type"));
        if (!Value || !Value->IsValid()) { NwiroBlueprintBatch::Reject(TEXT("Missing required Blueprint type")); return false; }
        TSharedPtr<FJsonObject> Type;
        if ((*Value)->Type == EJson::Object) Type = (*Value)->AsObject();
        else if ((*Value)->Type == EJson::String)
        { Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("base"), (*Value)->AsString()); }
        else { NwiroBlueprintBatch::Reject(TEXT("Blueprint type must be a string or object")); return false; }
        FString OuterSubtype;
        TArray<FString> SubtypeAliases;
        for (const auto& Pair : Item->Values)
        {
            const FString FieldName(Pair.Key);
            const FString Key = FieldName.Replace(TEXT("_"), TEXT("")).Replace(TEXT("-"), TEXT("")).ToLower();
            if (Key == TEXT("container"))
            { NwiroBlueprintBatch::Reject(TEXT("container must be inside structured type")); return false; }
            if (Key != TEXT("subtype") && Key != TEXT("objectclass") && Key != TEXT("objecttype")) continue;
            FString Candidate;
            if (!bVariable || Key == TEXT("objecttype") || !Pair.Value->TryGetString(Candidate) || Candidate.IsEmpty()
                || (!OuterSubtype.IsEmpty() && !Candidate.Equals(OuterSubtype, ESearchCase::CaseSensitive)))
            { NwiroBlueprintBatch::Reject(TEXT("Malformed/conflicting subtype; use structured type {base, subtype}")); return false; }
            OuterSubtype = Candidate;
            SubtypeAliases.Add(FieldName);
        }
        if (!OuterSubtype.IsEmpty())
        {
            FString Inner;
            if (Type->HasField(TEXT("subtype")) && (!Type->TryGetStringField(TEXT("subtype"), Inner)
                || !Inner.Equals(OuterSubtype, ESearchCase::CaseSensitive)))
            { NwiroBlueprintBatch::Reject(TEXT("Conflicting nested and outer subtype")); return false; }
            Type->SetStringField(TEXT("subtype"), OuterSubtype);
        }
        const TSharedRef<FJsonObject> Param = MakeShared<FJsonObject>();
        if (SubtypeAliases.Num()) Param->SetObjectField(TEXT("type"), Type);
        else Param->SetField(TEXT("type"), *Value);
        FEdGraphPinType Exact;
        FString Error;
        if (!NwiroDispatcherPrivate::ParseParamType(Param, Exact, Error))
        { NwiroBlueprintBatch::Reject(Error); return false; }
        if (!(ParsePinType(Type) == Exact))
        {
            NwiroBlueprintBatch::Reject(TEXT("Resolved Blueprint type differs from requested subtype/container; no fallback is admitted"));
            return false;
        }
        if (SubtypeAliases.Num())
        {
            Item->SetObjectField(TEXT("type"), Type);
            for (const FString& Alias : SubtypeAliases) Item->RemoveField(Alias);
        }
        return true;
    };
    // Validate type declarations in the entire normalized request before any staged
    // operation. A fallback in an earlier handler cannot make an invalid peer valid.
    for (const FString& Key : {FString(TEXT("add_custom_events")), FString(TEXT("add_event_dispatchers"))})
    {
        const TArray<TSharedPtr<FJsonValue>>* Units = nullptr;
        if (!Cmd->TryGetArrayField(Key, Units)) continue;
        for (const auto& Unit : *Units)
        {
            const auto Item = Unit->AsObject();
            FString Name;
            if (!Item->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
                NwiroBlueprintBatch::Reject(Key + TEXT(": missing name"));
            for (const TCHAR* ParamKey : {TEXT("params"), TEXT("inputs"), TEXT("outputs")})
            {
                if (!Item->HasField(ParamKey)) continue;
                const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
                if (!Item->TryGetArrayField(ParamKey, Params))
                { NwiroBlueprintBatch::Reject(TEXT("Function/event parameters must be arrays")); continue; }
                for (const auto& Param : *Params)
                {
                    if (!Param.IsValid() || Param->Type != EJson::Object)
                    { NwiroBlueprintBatch::Reject(TEXT("Malformed function/event parameter")); continue; }
                    ValidateType(Param->AsObject());
                }
            }
        }
    }
    if (NwiroBlueprintBatch::Active->Errors.Num()) return false;
    auto AcceptResult = [&](const FNwiroBPResult& Result)
    {
        if (!Result.bSuccess) NwiroBlueprintBatch::Reject(Result.Message);
        if (Result.Level2.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
            if (Result.Level2->TryGetArrayField(TEXT("errors"), Errors) && Errors->Num())
                NwiroBlueprintBatch::Reject(Result.Message);
        }
    };
    // Match commit order: prepared functions, prepared variables, then remaining operations.
    if (FunctionItems) AcceptResult(DoAddFunctions(BP, Functions));
    if (NwiroBlueprintBatch::Active->Errors.Num()) return false;
    if (VariableItems) AcceptResult(DoAddVariables(BP, Variables));
    if (NwiroBlueprintBatch::Active->Errors.Num()) return false;
    if (Cmd->HasField(TEXT("reparent")))
    {
        FString Parent;
        if (!Cmd->TryGetStringField(TEXT("reparent"), Parent) || Parent.IsEmpty())
            NwiroBlueprintBatch::Reject(TEXT("reparent requires an explicit class name"));
        else AcceptResult(DoReparent(BP, Parent));
    }
    for (const FString& Key : NwiroBlueprintBatch::OperationKeys())
    {
        const TArray<TSharedPtr<FJsonValue>>* Units = nullptr;
        if (!Cmd->TryGetArrayField(Key, Units)) continue;
        // Never allow an unsafe legacy silent target lookup (notably RemoveGraph(nullptr)).
        for (const auto& Unit : *Units)
        {
            const auto Item = Unit->Type == EJson::Object ? Unit->AsObject() : nullptr;
            FString Name = Unit->Type == EJson::String ? Unit->AsString() : FString();
            if (Item.IsValid()) Item->TryGetStringField(TEXT("name"), Name);
            if (Key == TEXT("remove_functions") && (Name.IsEmpty() || !FindGraph(BP, Name)))
                NwiroBlueprintBatch::Reject(TEXT("remove_functions names a missing graph"));
            if (Key == TEXT("remove_components"))
            {
                bool Found = false;
                if (BP->SimpleConstructionScript) for (const USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
                    Found |= Node && Node->GetVariableName() == FName(*Name);
                if (!Found) NwiroBlueprintBatch::Reject(TEXT("remove_components names a missing component"));
            }
            if (Key == TEXT("rename_variables"))
            {
                FString Old, New;
                if (!Item->TryGetStringField(TEXT("old"), Old) || !Item->TryGetStringField(TEXT("new"), New)
                    || New.IsEmpty() || FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*Old)) == INDEX_NONE
                    || FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*New)) != INDEX_NONE)
                    NwiroBlueprintBatch::Reject(TEXT("rename_variables has unresolved/conflicting old/new identities"));
            }
            if ((Key == TEXT("add_interfaces") || Key == TEXT("remove_interfaces"))
                && (Name.IsEmpty() || !ResolveInterfaceClass(Name)))
                NwiroBlueprintBatch::Reject(TEXT("Unresolved interface identity"));
        }
        if (NwiroBlueprintBatch::Active->Errors.Num()) return false;
        if (Key == TEXT("add_variables") || Key == TEXT("add_functions")) continue;
        if (Key == TEXT("remove_variables")) AcceptResult(DoRemoveVariables(BP, *Units));
        else if (Key == TEXT("rename_variables")) AcceptResult(DoRenameVariables(BP, *Units));
        else if (Key == TEXT("add_components")) AcceptResult(DoAddComponents(BP, *Units));
        else if (Key == TEXT("remove_components")) AcceptResult(DoRemoveComponents(BP, *Units));
        else if (Key == TEXT("set_component_properties")) AcceptResult(DoSetComponentProperties(BP, *Units));
        else if (Key == TEXT("remove_functions")) AcceptResult(DoRemoveFunctions(BP, *Units));
        else if (Key == TEXT("add_custom_events")) AcceptResult(DoAddCustomEvents(BP, *Units));
        else if (Key == TEXT("add_event_dispatchers")) AcceptResult(DoAddEventDispatchers(BP, *Units));
        else if (Key == TEXT("add_interfaces")) AcceptResult(DoAddInterfaces(BP, *Units));
        else if (Key == TEXT("remove_interfaces")) AcceptResult(DoRemoveInterfaces(BP, *Units));
        else if (Key == TEXT("add_nodes")) AcceptResult(DoAddNodes(BP, GraphName, *Units, PreparedActions));
        else if (Key == TEXT("remove_nodes")) AcceptResult(DoRemoveNodes(BP, GraphName, *Units));
        else if (Key == TEXT("connect_pins")) AcceptResult(DoConnectPins(BP, GraphName, *Units));
        else if (Key == TEXT("break_connections")) AcceptResult(DoBreakConnections(BP, GraphName, *Units));
        else if (Key == TEXT("set_pin_defaults")) AcceptResult(DoSetPinDefaults(BP, GraphName, *Units));
        if (NwiroBlueprintBatch::Active->Errors.Num()) return false;
    }
    return true;
}


FString FNwiroBlueprintTools::EditBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON command\"}");
	}

	// Pure variable-option validation must precede target creation/loading and all peer work.
	const FString VariableEditabilityFailure = NwiroVariableEditabilityAdmission::ValidateRequest(Cmd,
		[](const TSharedPtr<FJsonObject>& Object, const FString& Key, TArray<TSharedPtr<FJsonValue>>& Items)
		{
			FString Warning, Error;
			return ResolveOpArray(Object, Key, Items, Warning, Error);
		});
	if (!VariableEditabilityFailure.IsEmpty()) return VariableEditabilityFailure;

	// Complete pure component-shape admission before any target create/load or peer work.
	const FString ComponentAdmissionFailure = NwiroComponentRequestAdmission::ValidateRequest(Cmd,
		[](const TSharedPtr<FJsonObject>& Object, const FString& Key, TArray<TSharedPtr<FJsonValue>>& Items)
		{
			FString Warning, Error;
			return ResolveOpArray(Object, Key, Items, Warning, Error);
		});
	if (!ComponentAdmissionFailure.IsEmpty()) return ComponentAdmissionFailure;

	// Validate this advertised node recipe before any target load/create, peer operation or compile/save.
	TArray<NwiroEnhancedInputContract::FRecipeItem> EnhancedInputRecipes;
	const FString EnhancedInputError = NwiroEnhancedInputContract::ValidateRequest(Cmd,
		[](const TSharedPtr<FJsonObject>& Object, const FString& Key, TArray<TSharedPtr<FJsonValue>>& Items)
		{
			FString Warning, Error;
			return ResolveOpArray(Object, Key, Items, Warning, Error);
		}, &EnhancedInputRecipes);
	if (!EnhancedInputError.IsEmpty()) return EnhancedInputError;
	NwiroEnhancedInputContract::FPreparedActions EnhancedInputActions;
	const FString EnhancedInputAssetError = NwiroEnhancedInputContract::PrepareActions(EnhancedInputRecipes, EnhancedInputActions);
	if (!EnhancedInputAssetError.IsEmpty()) return EnhancedInputAssetError;

	if (!IsInGameThread() || !GEditor)
		return TEXT("{\"success\":false,\"error\":\"edit_blueprint requires the editor game thread\"}");
	TArray<FString> Messages;
	// Support "actions" array format: flatten into top-level keys
	// e.g. {"actions":[{"type":"add_nodes","nodes":[...]},{"type":"connect_pins","pins":[...]}]}
	TSet<FString> ActionsFlattenedAliases;
	if (Cmd->HasField(TEXT("actions")))
	{
		TArray<TSharedPtr<FJsonValue>> Actions;
		FString Warning, Error;
		if (!ResolveOpArray(Cmd, TEXT("actions"), Actions, Warning, Error))
			return NwiroBlueprintBatch::AdmissionFailure({Error}, 0);
		for (const auto& Value : Actions)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
				return NwiroBlueprintBatch::AdmissionFailure({TEXT("Every action must be an object")}, 0);
			const auto Action = Value->AsObject();
			FString Key;
			if (!Action->TryGetStringField(TEXT("type"), Key) || !NwiroBlueprintBatch::OperationKeys().Contains(Key))
				return NwiroBlueprintBatch::AdmissionFailure({TEXT("Action type must name a supported operation")}, 0);
			TArray<TSharedPtr<FJsonValue>> Payload;
			int32 Payloads = 0;
			for (const auto& Pair : Action->Values)
			{
				if (Pair.Key == TEXT("type")) continue;
				++Payloads;
				if (!ResolveOpArray(Action, FString(Pair.Key), Payload, Warning, Error))
					return NwiroBlueprintBatch::AdmissionFailure({Error}, 0);
			}
			if (Payloads != 1)
				return NwiroBlueprintBatch::AdmissionFailure({TEXT("Action requires exactly one operation payload")}, 0);
			TArray<TSharedPtr<FJsonValue>> Existing;
			if (Cmd->HasField(Key) && !ResolveOpArray(Cmd, Key, Existing, Warning, Error))
				return NwiroBlueprintBatch::AdmissionFailure({Error}, 0);
			Existing.Append(Payload);
			Cmd->SetArrayField(Key, Existing);
		}
		Cmd->RemoveField(TEXT("actions"));
	}

	// ── Alias normalization: remap known LLM shorthand keys to canonical names ─
	TArray<FString> AliasWarnings;
	TArray<FString> HandlerWarnings;
	{
		struct FAliasEntry { const TCHAR* Alias; const TCHAR* Canonical; };
		static const FAliasEntry Aliases[] = {
			{ TEXT("components"),    TEXT("add_components")        },
			{ TEXT("component"),     TEXT("add_components")        },
			{ TEXT("variables"),     TEXT("add_variables")         },
			{ TEXT("variable"),      TEXT("add_variables")         },
			{ TEXT("nodes"),         TEXT("add_nodes")             },
			{ TEXT("node"),          TEXT("add_nodes")             },
			{ TEXT("connections"),   TEXT("connect_pins")          },
			{ TEXT("connection"),    TEXT("connect_pins")          },
			{ TEXT("pin_defaults"),  TEXT("set_pin_defaults")      },
			{ TEXT("defaults"),      TEXT("set_pin_defaults")      },
			{ TEXT("set_defaults"),  TEXT("set_pin_defaults")      },
			{ TEXT("functions"),     TEXT("add_functions")         },
			{ TEXT("function"),      TEXT("add_functions")         },
			{ TEXT("custom_events"), TEXT("add_custom_events")     },
			{ TEXT("events"),        TEXT("add_custom_events")     },
			{ TEXT("dispatchers"),   TEXT("add_event_dispatchers") },
			{ TEXT("interfaces"),    TEXT("add_interfaces")        },
			{ TEXT("interface"),     TEXT("add_interfaces")        },
		};
		for (const FAliasEntry& E : Aliases)
		{
			const TSharedPtr<FJsonValue>* AliasVal = Cmd->Values.Find(E.Alias);
			if (!AliasVal || !AliasVal->IsValid()) continue;

			if (Cmd->HasField(E.Canonical))
			{
				if (ActionsFlattenedAliases.Contains(E.Alias))
				{
					// This exact key was copied by actions flattening — alias is redundant, drop silently
					Cmd->RemoveField(E.Alias);
					continue;
				}
				return FString::Printf(
					TEXT("{\"success\":false,\"error\":\"Conflicting keys: both '%s' and '%s' provided. Use only '%s'.\"}"),
					E.Alias, E.Canonical, E.Canonical);
			}
			if ((*AliasVal)->Type != EJson::Array)
			{
				// Same single-object tolerance as the canonical op keys
				// (ResolveOpArray): wrap `variables: {...}` into a one-element array.
				if ((*AliasVal)->Type == EJson::Object)
				{
					TArray<TSharedPtr<FJsonValue>> Wrapped;
					Wrapped.Add(MakeShareable(new FJsonValueObject((*AliasVal)->AsObject())));
					Cmd->SetArrayField(E.Canonical, Wrapped);
					Cmd->RemoveField(E.Alias);
					AliasWarnings.Add(FString::Printf(TEXT("Normalized '%s' (single object) to '%s' array. Use %s:[{...}] next time."), E.Alias, E.Canonical, E.Canonical));
					UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_ALIAS: '%s' -> '%s' (wrapped single object)"), E.Alias, E.Canonical);
					continue;
				}
				// Match the canonical decoder for JSON-encoded arrays and objects.
				// Malformed strings and scalar values retain the rejection below.
				if ((*AliasVal)->Type == EJson::String)
				{
					TArray<TSharedPtr<FJsonValue>> Parsed;
					FString NormalizeWarning, NormalizeError;
					if (ResolveOpArray(Cmd, FString(E.Alias), Parsed, NormalizeWarning, NormalizeError))
					{
						Cmd->SetArrayField(E.Canonical, Parsed);
						Cmd->RemoveField(E.Alias);
						AliasWarnings.Add(FString::Printf(TEXT("Normalized '%s' to '%s': %s. Send '%s' as a real JSON array next time."), E.Alias, E.Canonical, *NormalizeWarning, E.Canonical));
						UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_ALIAS: '%s' -> '%s' (parsed from JSON string)"), E.Alias, E.Canonical);
						continue;
					}
				}
				return FString::Printf(
					TEXT("{\"success\":false,\"error\":\"Key '%s' must be an array (e.g. %s:[{...}]). Got a non-array value.\"}"),
					E.Alias, E.Canonical);
			}
			Cmd->SetField(E.Canonical, *AliasVal);
			Cmd->RemoveField(E.Alias);
			AliasWarnings.Add(FString::Printf(TEXT("Normalized '%s' to '%s'. Use '%s' directly next time."), E.Alias, E.Canonical, E.Canonical));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_ALIAS: '%s' -> '%s'"), E.Alias, E.Canonical);
		}
	}
	// ─────────────────────────────────────────────────────────────────────────

	// ── Clean up action-flattened helper keys that are not canonical ─────────
	// e.g. {type:"connect_pins", pins:[...]} copies "pins" to Cmd but sets canonical
	// "connect_pins". Remove non-canonical leftover keys so rejection does not fire.
	for (const FString& FlatKey : ActionsFlattenedAliases)
	{
		if (!IsKnownEditBlueprintKey(FlatKey))
			Cmd->RemoveField(FlatKey);
	}
	// ─────────────────────────────────────────────────────────────────────────


	const bool bPlannedBatch = NwiroBlueprintBatch::NeedsAdmission(Cmd);
	NwiroBlueprintBatch::FContext BatchContext;
	if (NwiroBlueprintBatch::Active)
		return TEXT("{\"success\":false,\"error\":\"Reentrant Blueprint edit during batch admission/commit is refused\"}");
	TGuardValue<NwiroBlueprintBatch::FContext*> BatchScope(
		NwiroBlueprintBatch::Active, bPlannedBatch ? &BatchContext : nullptr);
	if (bPlannedBatch)
	{
		if (!IsInGameThread())
			return NwiroBlueprintBatch::AdmissionFailure({TEXT("Blueprint admission requires the game thread")}, BatchContext.RequestedUnits);
		BatchContext.RequestedUnits = Cmd->HasField(TEXT("reparent")) ? 1 : 0;
		for (const FString& Key : NwiroBlueprintBatch::OperationKeys())
		{
			if (!Cmd->HasField(Key)) continue;
			TArray<TSharedPtr<FJsonValue>> Units;
			FString Warning, Error;
			if (!ResolveOpArray(Cmd, Key, Units, Warning, Error))
				return NwiroBlueprintBatch::AdmissionFailure({Key + TEXT(": ") + Error}, BatchContext.RequestedUnits);
			BatchContext.RequestedUnits += Units.Num();
			const bool bNamedRemoval = Key == TEXT("remove_variables") || Key == TEXT("remove_components")
				|| Key == TEXT("remove_functions") || Key == TEXT("remove_nodes") || Key == TEXT("add_interfaces") || Key == TEXT("remove_interfaces");
			for (const auto& Unit : Units)
			{
				if (!Unit.IsValid() || (Unit->Type != EJson::Object && !(bNamedRemoval && Unit->Type == EJson::String)))
					return NwiroBlueprintBatch::AdmissionFailure({Key + TEXT(": malformed request unit")}, BatchContext.RequestedUnits);
			}
			Cmd->SetArrayField(Key, Units);
		}
		BatchContext.bRequestCountComplete = true;
		for (const TCHAR* Field : {TEXT("blueprint"), TEXT("name"), TEXT("path"), TEXT("graph"), TEXT("parentClass"), TEXT("parent_class"), TEXT("reparent")})
		{
			FString Text;
			if (Cmd->HasField(Field) && !Cmd->TryGetStringField(Field, Text))
				return NwiroBlueprintBatch::AdmissionFailure({FString(Field) + TEXT(" must be a string")}, BatchContext.RequestedUnits);
		}
		if (Cmd->HasField(TEXT("compile")) && Cmd->Values[TEXT("compile")]->Type != EJson::Boolean)
			return NwiroBlueprintBatch::AdmissionFailure({TEXT("compile must be a boolean")}, BatchContext.RequestedUnits);
	}


	TArray<FNwiroPreparedVariable> PreparedVariables;
	FNwiroUnitLedger VariablePreflightLedger;
	if (Cmd->HasField(TEXT("add_variables")))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		FString Warning, Error;
		if (!ResolveOpArray(Cmd, TEXT("add_variables"), Items, Warning, Error))
			return NwiroVariables::RejectedResponse(nullptr, TEXT(""), PreparedVariables, VariablePreflightLedger, Error);
		Cmd->SetArrayField(TEXT("add_variables"), Items);
		if (!Warning.IsEmpty()) Messages.Add(Warning);
		if (!NwiroVariables::Prepare(Items, PreparedVariables, VariablePreflightLedger))
		{
			FString Target;
			Cmd->TryGetStringField(TEXT("blueprint"), Target);
			return NwiroVariables::RejectedResponse(nullptr, Target, PreparedVariables, VariablePreflightLedger,
				TEXT("add_variables rejected before target creation or mutation; inspect operations.errors"));
		}
	}


	TArray<FNwiroPreparedFunction> PreparedFunctions;
	FNwiroUnitLedger FunctionPreflightLedger;
	if (Cmd->HasField(TEXT("add_functions")))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		FString Warning, Error;
		if (!ResolveOpArray(Cmd, TEXT("add_functions"), Items, Warning, Error))
			return NwiroFunctions::RejectedResponse(nullptr, TEXT(""), PreparedFunctions, FunctionPreflightLedger, Error);
		Cmd->SetArrayField(TEXT("add_functions"), Items);
		if (!Warning.IsEmpty()) Messages.Add(Warning);
		if (!NwiroFunctions::Prepare(Items, PreparedFunctions, FunctionPreflightLedger))
		{
			FString Target;
			Cmd->TryGetStringField(TEXT("blueprint"), Target);
			return NwiroFunctions::RejectedResponse(nullptr, Target, PreparedFunctions, FunctionPreflightLedger,
				TEXT("add_functions rejected before target creation or mutation; inspect operations.errors"));
		}
	}
	if (!PreparedFunctions.IsEmpty())
	{
		for (const TCHAR* Key : { TEXT("reparent"), TEXT("add_interfaces"), TEXT("remove_interfaces"), TEXT("rename_variables") })
		{
			if (!Cmd->HasField(Key)) continue;
			if (FString(Key) != TEXT("reparent"))
			{
				TArray<TSharedPtr<FJsonValue>> OtherItems;
				FString Warning, Error;
				if (ResolveOpArray(Cmd, Key, OtherItems, Warning, Error) && OtherItems.IsEmpty()) continue;
			}
			return NwiroFunctions::RejectedResponse(nullptr, TEXT(""), PreparedFunctions, FunctionPreflightLedger,
				FString::Printf(TEXT("add_functions and %s require separate calls so the final function namespace can be validated"), Key));
		}
		TMap<FName, FString> OtherNames;
		for (const FNwiroPreparedVariable& Variable : PreparedVariables) OtherNames.Add(Variable.Name, TEXT("add_variables"));
		for (const TCHAR* Key : { TEXT("add_components"), TEXT("add_custom_events"), TEXT("add_event_dispatchers") })
		{
			if (!Cmd->HasField(Key)) continue;
			TArray<TSharedPtr<FJsonValue>> OtherItems;
			FString Warning, Error;
			if (!ResolveOpArray(Cmd, Key, OtherItems, Warning, Error))
				return NwiroFunctions::RejectedResponse(nullptr, TEXT(""), PreparedFunctions, FunctionPreflightLedger,
					FString::Printf(TEXT("cannot validate function name conflicts with %s: %s"), Key, *Error));
			for (const TSharedPtr<FJsonValue>& Value : OtherItems)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid()) continue;
				const auto Other = Value->AsObject();
				FString Name;
				Other->TryGetStringField(TEXT("name"), Name);
				if (Name.IsEmpty() && FString(Key) == TEXT("add_components")) Name = GetFieldNormalized(Other, TEXT("componentname"));
				if (!Name.IsEmpty() && Name.Len() < NAME_SIZE)
				{
					bool bHasControl = false;
					for (TCHAR Character : Name) bHasControl |= FChar::IsControl(Character);
					if (!bHasControl) OtherNames.Add(FName(*Name), Key);
				}
			}
		}
		for (int32 Unit = 0; Unit < PreparedFunctions.Num(); ++Unit)
			if (const FString* Other = OtherNames.Find(PreparedFunctions[Unit].Name))
				FunctionPreflightLedger.AddError(Unit, TEXT("add_functions"), TEXT("DUPLICATE_NAME"), TEXT("name"), PreparedFunctions[Unit].Name.ToString(),
					TEXT("function name conflicts with another requested operation: ") + *Other);
		if (!FunctionPreflightLedger.Errors.IsEmpty())
			return NwiroFunctions::RejectedResponse(nullptr, TEXT(""), PreparedFunctions, FunctionPreflightLedger,
				TEXT("add_functions rejected before any requested operation; inspect operations.errors"));
	}

	if (!PreparedVariables.IsEmpty() && Cmd->HasField(TEXT("reparent")))
		return NwiroVariables::RejectedResponse(nullptr, TEXT(""), PreparedVariables, VariablePreflightLedger,
			TEXT("add_variables and reparent require separate calls so variable names can be validated against the final parent"));

	// ── Unknown key rejection: surface to LLM so it can self-correct ────────
	{
		TArray<FString> UnknownKeys;
		for (const auto& Pair : Cmd->Values)
		{
			const FString Key(*Pair.Key);
			if (!IsKnownEditBlueprintKey(Key))
				UnknownKeys.Add(Key);
		}
		if (UnknownKeys.Num() > 0)
		{
			UnknownKeys.Sort();
			FString KeyList = FString::Join(UnknownKeys, TEXT(", "));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_REJECTED unknown keys: [%s]"), *KeyList);
			return FString::Printf(
				TEXT("{\"success\":false,\"error\":\"Unknown edit_blueprint key(s): %s. Valid op keys: add_components, add_variables, add_nodes, connect_pins, set_pin_defaults, add_functions, add_custom_events, add_event_dispatchers, add_interfaces, remove_variables, rename_variables, remove_components, set_component_properties, remove_functions, remove_interfaces, remove_nodes, break_connections\"}"),
				*KeyList);
		}
	}
	// ─────────────────────────────────────────────────────────────────────────


	NwiroBlueprintBatch::FObserveTarget ObserveTarget(BatchContext);
	// Reuse the same preflight target for variable/interface admission and commit.
	// Cache null too: LoadObject can create an empty package for an absent explicit
	// target, which must stay untouched until Pro's staged batch is admitted.
	UBlueprint* PreflightTarget = nullptr;
	bool bPreflightTargetResolved = false;
	auto ResolvePreflightTarget = [&]() -> UBlueprint*
	{
		if (bPreflightTargetResolved) return PreflightTarget;
		bPreflightTargetResolved = true;
		FString Target;
		Cmd->TryGetStringField(TEXT("blueprint"), Target);
		if (Target.IsEmpty() && bPlannedBatch) Cmd->TryGetStringField(TEXT("name"), Target);
		if (Target.IsEmpty()) return nullptr;
		if (bPlannedBatch && Target.StartsWith(TEXT("/")))
		{
			const FString Package = FPackageName::ObjectPathToPackageName(Target);
			if (Target.Len() > 1024 || (!FindPackage(nullptr, *Package)
				&& !FPackageName::DoesPackageExist(Package))) return nullptr;
		}
		PreflightTarget = LoadBP(Target);
		BatchContext.BindTarget(PreflightTarget);
		return PreflightTarget;
	};

	TArray<FName> VariableNodeTargets;
	if (Cmd->HasField(TEXT("add_nodes")))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		FString Warning, Error;
		if (!ResolveOpArray(Cmd, TEXT("add_nodes"), Items, Warning, Error)
			|| !PrepareVariableNodeTargets(Items, VariableNodeTargets, Error)) return BlueprintPreflightError(Error);
		if (!VariableNodeTargets.IsEmpty())
		{
			FString ParentName;
			UBlueprint* TargetBP = ResolvePreflightTarget();
			UClass* Parent = TargetBP ? TargetBP->ParentClass.Get() : nullptr;
			if (Cmd->TryGetStringField(TEXT("reparent"), ParentName) && !ParentName.IsEmpty()) Parent = FindClassByName(ParentName);
			else if (!TargetBP)
			{
				if (!Cmd->TryGetStringField(TEXT("parentClass"), ParentName)) Cmd->TryGetStringField(TEXT("parent_class"), ParentName);
				Parent = ParentName.IsEmpty() ? AActor::StaticClass() : FindClassByName(ParentName);
			}
			if (!ValidateVariableNodeTargets(TargetBP, Parent, Cmd, PreparedVariables, VariableNodeTargets, Error))
				return BlueprintPreflightError(Error);
		}
	}

	// An invalid removal must not auto-create the target or apply earlier operations.
	UBlueprint* InterfaceRemovalTarget = nullptr;
	if (Cmd->HasField(TEXT("remove_interfaces")))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		TArray<UClass*> Classes;
		FString Warning, Error;
		if (!ResolveOpArray(Cmd, TEXT("remove_interfaces"), Items, Warning, Error))
			return InterfaceRemovalResponse(nullptr, false, Error, 0);
		InterfaceRemovalTarget = ResolvePreflightTarget();
		if (!PrepareInterfaceRemoval(InterfaceRemovalTarget, Items, Classes, Error))
			return InterfaceRemovalResponse(InterfaceRemovalTarget, false, Error, 0);
		Cmd->SetArrayField(TEXT("remove_interfaces"), Items);
		if (!Warning.IsEmpty()) Messages.Add(Warning);
	}

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));

	// Handle create action: if no "blueprint" but has "name" + "parent_class" or "parentClass", create it
	FString ActionStr;
	Cmd->TryGetStringField(TEXT("action"), ActionStr);
	if (!bPlannedBatch && BPName.IsEmpty() && (ActionStr.Equals(TEXT("CreateBlueprint"), ESearchCase::IgnoreCase) || Cmd->HasField(TEXT("parent_class")) || Cmd->HasField(TEXT("parentClass"))))
	{
		FString CreateName = Cmd->GetStringField(TEXT("name"));
		if (CreateName.IsEmpty()) CreateName = TEXT("BP_NewBlueprint");

		FString CreateParent = Cmd->GetStringField(TEXT("parent_class"));
		if (CreateParent.IsEmpty()) CreateParent = Cmd->GetStringField(TEXT("parentClass"));
		if (CreateParent.IsEmpty()) CreateParent = TEXT("Actor");

		FString CreatePath = Cmd->GetStringField(TEXT("path"));
		if (CreatePath.IsEmpty()) CreatePath = TEXT("/Game");

		FString CreateJson = FString::Printf(TEXT("{\"name\":\"%s\",\"parentClass\":\"%s\",\"path\":\"%s\"}"), *CreateName, *CreateParent, *CreatePath);
		FString CreateResult = CreateBlueprint(CreateJson);
		BPName = CreateName;

		UBlueprint* CreatedBP = LoadBP(BPName);
		if (CreatedBP)
		{
			TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint"), CreatedBP->GetName());

			TArray<TSharedPtr<FJsonValue>> MsgArr;
			MsgArr.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("[create] Blueprint '%s' created (parent: %s)"), *CreateName, *CreateParent))));
			Result->SetArrayField(TEXT("messages"), MsgArr);

			FString Out;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Result, W);
			return Out;
		}
		else
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Failed to create blueprint: %s\"}"), *CreateName);
		}
	}

	if (bPlannedBatch && BPName.IsEmpty())
	{
		Cmd->TryGetStringField(TEXT("name"), BPName);
		if (!BPName.IsEmpty()) Cmd->SetStringField(TEXT("blueprint"), BPName);
	}

	if (BPName.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'blueprint' field\"}");
	}

	FString PlannedCreateName, PlannedCreateDirectory, PlannedCreatePackage;
	bool bExactCreateIdentity = true;
	if (bPlannedBatch)
	{
		if (BPName.StartsWith(TEXT("/")))
		{
			PlannedCreatePackage = FPackageName::ObjectPathToPackageName(BPName);
			PlannedCreateName = FPackageName::GetShortName(PlannedCreatePackage);
			PlannedCreateDirectory = FPackageName::GetLongPackagePath(PlannedCreatePackage);
			int32 ObjectSeparator = INDEX_NONE;
			if (BPName.FindChar(TEXT('.'), ObjectSeparator))
				bExactCreateIdentity = BPName.RightChop(ObjectSeparator + 1).Equals(PlannedCreateName, ESearchCase::CaseSensitive);
			bExactCreateIdentity &= !BPName.Contains(TEXT(":"));
		}
		else
		{
			PlannedCreateName = BPName;
			Cmd->TryGetStringField(TEXT("path"), PlannedCreateDirectory);
			if (PlannedCreateDirectory.IsEmpty()) PlannedCreateDirectory = TEXT("/Game");
			PlannedCreatePackage = PlannedCreateDirectory / PlannedCreateName;
		}
	}
	// LoadObject may create an empty UPackage even when the requested asset is
	// absent. Keep absent explicit targets untouched until the staged batch is
	// admitted; loaded unsaved packages and packages on disk still use LoadBP.
	const bool bAbsentExplicitPackage = bPlannedBatch && BPName.StartsWith(TEXT("/"))
		&& (BPName.Len() > 1024 || (!FindPackage(nullptr, *PlannedCreatePackage)
			&& !FPackageName::DoesPackageExist(PlannedCreatePackage)));
	UBlueprint* BP = bPreflightTargetResolved ? PreflightTarget
		: (bAbsentExplicitPackage ? nullptr : LoadBP(BPName));
	BatchContext.BindTarget(BP);
	TStrongObjectPtr<UBlueprint> KeepTarget(BP);
	TStrongObjectPtr<UClass> PlannedParent;
	if (bPlannedBatch)
	{
		if (!BP && !Cmd->HasField(TEXT("add_nodes")))
			return NwiroBlueprintBatch::AdmissionFailure({TEXT("Pin-only edits require an existing Blueprint; no auto-create")}, BatchContext.RequestedUnits);
		if (BP)
		{
			TArray<UEdGraph*> Graphs;
			BP->GetAllGraphs(Graphs);
			for (const UEdGraph* Graph : Graphs) if (Graph)
				for (const UEdGraphNode* Node : Graph->Nodes) if (Node) BatchContext.ExistingNodeGuids.Add(Node->NodeGuid);
		}
		UBlueprint* Stage = BP ? NwiroBlueprintBatch::DuplicateForValidation(BP) : nullptr;
		if (!BP)
		{
			if (!bExactCreateIdentity)
				return NwiroBlueprintBatch::AdmissionFailure({TEXT("New Blueprint object leaf must exactly match its package leaf; subobjects are not create targets")}, BatchContext.RequestedUnits);
			FString ParentName;
			Cmd->TryGetStringField(TEXT("parentClass"), ParentName);
			if (ParentName.IsEmpty()) Cmd->TryGetStringField(TEXT("parent_class"), ParentName);
			if (ParentName.IsEmpty()) ParentName = TEXT("Actor");
			UClass* Parent = FindClassByName(ParentName);
			if (!Parent || !FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
				return NwiroBlueprintBatch::AdmissionFailure({TEXT("Unresolved or unsupported parent class")}, BatchContext.RequestedUnits);
			if (PlannedCreateName.IsEmpty() || PlannedCreateName.Len() > 256
				|| PlannedCreatePackage.Len() > 1024 || !FPackageName::IsValidLongPackageName(PlannedCreatePackage)
				|| !FName::IsValidXName(PlannedCreateName, INVALID_OBJECTNAME_CHARACTERS))
				return NwiroBlueprintBatch::AdmissionFailure({TEXT("Invalid new Blueprint asset name/path")}, BatchContext.RequestedUnits);
			const FString Conflict = NwiroCheckCreateConflict(PlannedCreatePackage, PlannedCreateName, UBlueprint::StaticClass());
			if (!Conflict.IsEmpty()) return NwiroBlueprintBatch::AdmissionFailure({Conflict}, BatchContext.RequestedUnits);
			PlannedParent.Reset(Parent);
			const FName StageName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("NwiroBatchStage"));
			BatchContext.ExpectedStagePath = GetTransientPackage()->GetPathName() + TEXT(".") + StageName.ToString();
			Stage = FKismetEditorUtilities::CreateBlueprint(Parent, GetTransientPackage(), StageName, BPTYPE_Normal);
		}
		TStrongObjectPtr<UBlueprint> KeepStage(Stage);
		if (!Stage)
			return NwiroBlueprintBatch::AdmissionFailure({TEXT("Could not create an isolated Blueprint staging owner")}, BatchContext.RequestedUnits);
		const auto SavedNodeRefs = NodeRefs;
		{
			ON_SCOPE_EXIT { NodeRefs = SavedNodeRefs; };
			NodeRefs.Empty();
			if (BP) for (const auto& Pair : SavedNodeRefs)
			{
				if (Pair.Value.BlueprintPath != BP->GetPathName()) continue;
				auto Binding = Pair.Value;
				Binding.BlueprintPath = Stage->GetPathName();
				NodeRefs.Add(Binding.BlueprintPath + TEXT("|") + Binding.Ref, Binding);
			}
			FString StageGraph;
			Cmd->TryGetStringField(TEXT("graph"), StageGraph);
			if (StageGraph.IsEmpty()) StageGraph = TEXT("EventGraph");
			if (!ValidateBlueprintRequest(Stage, Cmd, StageGraph, EnhancedInputActions))
				return NwiroBlueprintBatch::AdmissionFailure(BatchContext.Errors, BatchContext.RequestedUnits);
		}
		if (BatchContext.TargetCompileAttempts || BatchContext.TargetSaveAttempts)
			return NwiroBlueprintBatch::AdmissionFailure({TEXT("Staging unexpectedly reached target compile/save; inspect target state")}, BatchContext.RequestedUnits);
		BatchContext.bStaging = false;
	}

	if (!BP && bPlannedBatch)
	{
		BatchContext.ExpectedTargetPath = PlannedCreatePackage + TEXT(".") + PlannedCreateName;
		BatchContext.bCreatingTarget = true;
		const auto CreateCommand = MakeShared<FJsonObject>();
		CreateCommand->SetStringField(TEXT("name"), PlannedCreateName);
		CreateCommand->SetStringField(TEXT("path"), PlannedCreateDirectory);
		CreateCommand->SetStringField(TEXT("parentClass"), PlannedParent->GetPathName());
		const FString CreationReceipt = CreateBlueprint(NwiroBlueprintBatch::Json(CreateCommand));
		BP = FindObject<UBlueprint>(nullptr, *BatchContext.ExpectedTargetPath);
		BatchContext.bCreatingTarget = false;
		KeepTarget.Reset(BP);
		BatchContext.BindTarget(BP);
		if (!BP)
		{
			const auto Failure = MakeShared<FJsonObject>();
			Failure->SetBoolField(TEXT("success"), false);
			Failure->SetStringField(TEXT("error"), TEXT("Admitted Blueprint creation failed: ") + CreationReceipt);
			Failure->SetBoolField(TEXT("compiled"), BatchContext.TargetCompileAttempts > 0);
			Failure->SetBoolField(TEXT("saveAttempted"), BatchContext.TargetSaveAttempts > 0);
			if (BatchContext.TargetSaveAttempts) Failure->SetField(TEXT("savedToDisk"), MakeShared<FJsonValueNull>());
			else Failure->SetBoolField(TEXT("savedToDisk"), false);
			const auto Batch = MakeShared<FJsonObject>();
			Batch->SetStringField(TEXT("phase"), TEXT("commit_failed"));
			Batch->SetStringField(TEXT("currentRequestMutation"), TEXT("partial_or_unknown"));
			Batch->SetField(TEXT("appliedUnits"), MakeShared<FJsonValueNull>());
			Batch->SetBoolField(TEXT("countsComplete"), false);
			Batch->SetNumberField(TEXT("stagingCompileAttempts"), BatchContext.StagingCompileAttempts);
			Batch->SetNumberField(TEXT("creationCompileAttempts"), BatchContext.CreationCompileAttempts);
			Failure->SetObjectField(TEXT("batchAdmission"), Batch);
			return NwiroBlueprintBatch::Json(Failure);
		}
	}
	if (!BP)
	{
		// Auto-create: use parentClass if specified, otherwise default to Actor
		FString ParentClassName = Cmd->GetStringField(TEXT("parentClass"));
		if (ParentClassName.IsEmpty()) ParentClassName = Cmd->GetStringField(TEXT("parent_class"));
		if (ParentClassName.IsEmpty()) ParentClassName = TEXT("Actor");

		UE_LOG(LogNwiroBP, Log, TEXT("Blueprint '%s' not found, auto-creating with parent '%s'"), *BPName, *ParentClassName);
		FString CreateJson = FString::Printf(TEXT("{\"name\":\"%s\",\"parentClass\":\"%s\"}"), *BPName, *ParentClassName);
		CreateBlueprint(CreateJson);
		BP = LoadBP(BPName);

		if (!BP)
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Failed to create blueprint: %s\"}"), *BPName);
		}
	}

	if (!NwiroFunctions::ValidateTarget(BP, PreparedFunctions, FunctionPreflightLedger))
		return NwiroFunctions::RejectedResponse(BP, BPName, PreparedFunctions, FunctionPreflightLedger,
			TEXT("add_functions rejected before any edit operation; inspect operations.errors"));

	if (!NwiroVariables::ValidateTarget(BP, PreparedVariables, VariablePreflightLedger))
		return NwiroVariables::RejectedResponse(BP, BPName, PreparedVariables, VariablePreflightLedger,
			TEXT("add_variables rejected before any edit operation; inspect operations.errors"));

	FString GraphName;
	Cmd->TryGetStringField(TEXT("graph"), GraphName);
	if (GraphName.IsEmpty())
	{
		GraphName = TEXT("EventGraph");
	}

	TUniquePtr<FGCScopeGuard> FunctionGCGuard;
	if (!PreparedFunctions.IsEmpty()) FunctionGCGuard = MakeUnique<FGCScopeGuard>();

	// One level-2 block per MIGRATED sub-operation (see FNwiroUnitLedger). Absent for
	// handlers that do not report yet, which is why this is a list of blocks that name
	// their own op rather than a fixed set of fields.
	TArray<TSharedPtr<FJsonValue>> Operations;
	bool bAllOk = true;
	TArray<FNwiroDispatcherMutation> DispatcherMutations;
	TArray<FString> CompileDiagnostics;
	bool bCompiled = false;
	FString CompileStatus = TEXT("not_requested");
	FString ValidationLevel = TEXT("skeleton");
	UPackage* const BlueprintPackage = BP->GetOutermost();
	const bool bPackageWasDirtyAtEntry = BlueprintPackage->IsDirty();
	const EBlueprintStatus BlueprintStatusAtEntry = BP->Status;
	const bool bCachedDependenciesWereUpToDateAtEntry =
		BP->bCachedDependenciesUpToDate;
	// Whether any op was asked for at all, and whether any of them actually changed the
	// asset. The pair matters: a call carrying only `compile` is an explicit compile
	// request and must still run, while a call whose ops all failed must not rewrite
	// the .uasset.
	bool bAnyOpRequested = false;
	bool bAnyNonFunctionOpRequested = false;
	bool bAnyNonDispatcherMutation = false;
	bool bDispatcherMutationActive = false;
	bool bDispatcherRollbackRestoredState = false;
	bool bBlockCompileAndPersistence = false;
	auto RestoreCleanStateAfterVerifiedDispatcherRollback = [&]()
	{
		if (bPackageWasDirtyAtEntry || bAnyNonDispatcherMutation
			|| bDispatcherMutationActive)
		{
			return;
		}
		BlueprintPackage->SetDirtyFlag(false);
		BP->Status = BlueprintStatusAtEntry;
		BP->bCachedDependenciesUpToDate =
			bCachedDependenciesWereUpToDateAtEntry;
	};

	// Process each operation type. An op key may arrive as a real JSON array
	// or — from many models — as a JSON-ENCODED STRING (e.g.
	// add_variables: "[{\"name\":\"IsActive\",\"type\":\"bool\"}]"). The old
	// TryGetArrayField-only path type-failed on the string and SILENTLY
	// skipped the op while compile+save still ran → success:true with zero
	// mutations. Accept the stringified form with a visible compatibility
	// warning; surface any present-but-unusable value as a per-op FAILURE.
	auto RunOp = [&](const FString& Key, TFunction<FNwiroBPResult(const TArray<TSharedPtr<FJsonValue>>&)> Func)
	{
		if (!Cmd->HasField(Key)) return; // op not requested
		if (bPlannedBatch && BatchContext.Errors.Num() > 0) { bAllOk = false; bBlockCompileAndPersistence = true; return; }
		bAnyOpRequested = true;
		bAnyNonFunctionOpRequested |= Key != TEXT("add_functions");

		TArray<TSharedPtr<FJsonValue>> Items;
		FString NormalizeWarning;
		FString ResolveError;
		if (!ResolveOpArray(Cmd, Key, Items, NormalizeWarning, ResolveError))
		{
			Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *ResolveError));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_BAD_OP_VALUE: '%s' — %s"), *Key, *ResolveError);
			bAllOk = false;
			return;
		}
		if (!NormalizeWarning.IsEmpty())
		{
			Cmd->SetArrayField(Key, Items); // downstream readers (e.g. GetArrayCount) see the real array
			Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *NormalizeWarning));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_NORMALIZED_OP_VALUE: '%s' — %s"), *Key, *NormalizeWarning);
		}
#if WITH_DEV_AUTOMATION_TESTS
		if (bPlannedBatch && NwiroBlueprintBatch::BeforeCommitOperation)
			NwiroBlueprintBatch::BeforeCommitOperation(BP, Key);
#endif
		if (bPlannedBatch) BatchContext.AttemptedCommitUnits += Items.Num();
		FNwiroBPResult R = Func(Items);
		const TArray<TSharedPtr<FJsonValue>>* UnitErrors = nullptr;
		const bool bReportedUnitFailure = R.Level2.IsValid()
			&& R.Level2->TryGetArrayField(TEXT("errors"), UnitErrors) && UnitErrors->Num() > 0;
		if (bPlannedBatch && (!R.bSuccess || bReportedUnitFailure || BatchContext.Errors.Num() > 0))
		{
			NwiroBlueprintBatch::Reject(R.Message);
			bAllOk = false;
			bBlockCompileAndPersistence = true;
		}
		// A stale prepared identity is an internal/reentrant failure, never a no-touch receipt.
		bool bEnhancedInputPlanInvalidated = false;
		if (Key == TEXT("add_nodes") && R.Data.IsValid()
			&& R.Data->TryGetBoolField(TEXT("enhancedInputPlanInvalidated"), bEnhancedInputPlanInvalidated)
			&& bEnhancedInputPlanInvalidated)
		{
			bBlockCompileAndPersistence = true;
		}
		Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *R.Message));
		if (!R.bSuccess) bAllOk = false;
		// Per-unit warnings join the alias-normalization ones in the top-level array.
		HandlerWarnings.Append(R.Warnings);
		// Only migrated handlers set Level2. Collected per operation rather than merged
		// into flat top-level buckets: edit_blueprint is a batch OF batches, and a
		// flattened set would lose which operation rejected which unit — the one thing
		// a caller needs in order to retry correctly.
		if (R.Level2.IsValid()) Operations.Add(MakeShareable(new FJsonValueObject(R.Level2.ToSharedRef())));
		if (Key == TEXT("add_event_dispatchers"))
		{
			// Dispatcher creation predates the level-2 ledger, but unlike the other legacy
			// handlers it publishes an exact mutation/rollback record. A preflight rejection
			// is therefore a real no-op, while an unverified rollback must never reach disk.
			bool bHadAttemptedMutation = false;
			bool bEveryAttemptedRollbackVerified = true;
			for (const FNwiroDispatcherMutation& Mutation : DispatcherMutations)
			{
				if (!Mutation.bMemberCreated && Mutation.Graph == nullptr) continue;
				bHadAttemptedMutation = true;
				bEveryAttemptedRollbackVerified &=
					Mutation.bRolledBack && Mutation.bRollbackVerified;
			}

			if (R.bSuccess)
			{
				bDispatcherMutationActive = true;
			}
			else if (bHadAttemptedMutation)
			{
				if (bEveryAttemptedRollbackVerified)
				{
					bDispatcherRollbackRestoredState = true;
				}
				else
				{
					bDispatcherMutationActive = true;
					bBlockCompileAndPersistence = true;
				}
			}
		}
		else
		{
			// A migrated handler answers precisely from its ledger; one without a ledger
			// cannot say, so it is conservatively assumed to have mutated.
			bAnyNonDispatcherMutation |=
				R.Level2.IsValid() ? LedgerAppliedAnything(R.Level2) : true;
		}
	};

	bool bFunctionBatchNoOp = false;
	if (bPlannedBatch)
	{
		RunOp(TEXT("add_functions"), [&](const auto&)
		{
			const auto Applied = DoAddFunctions(BP, PreparedFunctions);
			bFunctionBatchNoOp = Applied.bSuccess && !LedgerAppliedAnything(Applied.Level2);
			return Applied;
		});
		RunOp(TEXT("add_variables"), [&](const auto&) { return DoAddVariables(BP, PreparedVariables); });
	}
	if (!bPlannedBatch && Cmd->HasField(TEXT("add_functions")))
	{
		const FNwiroBPResult FunctionResult = DoAddFunctions(BP, PreparedFunctions);
		if (!FunctionResult.bSuccess) return NwiroFunctions::RejectedApplyResponse(BP, PreparedFunctions, FunctionResult);
		bAnyOpRequested = true;
		const bool bFunctionMutation = LedgerAppliedAnything(FunctionResult.Level2);
		bAnyNonDispatcherMutation |= bFunctionMutation;
		bFunctionBatchNoOp = !bFunctionMutation;
		Messages.Add(FString::Printf(TEXT("[add_functions] %s"), *FunctionResult.Message));
		Operations.Add(MakeShared<FJsonValueObject>(FunctionResult.Level2));
	}

	if (!bPlannedBatch && Cmd->HasField(TEXT("add_variables")))
	{
		const FNwiroBPResult VariableResult = DoAddVariables(BP, PreparedVariables);
		if (!VariableResult.bSuccess)
		{
			// AddMemberVariable compiles structurally. Cancel cannot undo it; report
			// the exact applied units and stop here instead of saving partial work.
			FNwiroUnitLedger FailureLedger;
			FailureLedger.Created = VariableResult.Level2->GetArrayField(TEXT("created"));
			FailureLedger.Errors = VariableResult.Level2->GetArrayField(TEXT("errors"));
			const FString Rejection = NwiroVariables::RejectedResponse(BP, BPName, PreparedVariables, FailureLedger,
				VariableResult.Message, true);
			if (Operations.IsEmpty()) return Rejection;
			// Functions applied earlier remain in memory; preserve their ledger alongside
			// the variable failure without changing the accepted variable response contract.
			TSharedPtr<FJsonObject> Failure;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Rejection), Failure);
			Operations.Append(Failure->GetArrayField(TEXT("operations")));
			Failure->SetArrayField(TEXT("operations"), Operations);
			FString FailureJson;
			FJsonSerializer::Serialize(Failure.ToSharedRef(), TJsonWriterFactory<>::Create(&FailureJson));
			return FailureJson;
		}
		bAnyOpRequested = true;
		bAnyNonFunctionOpRequested = true;
		bAnyNonDispatcherMutation |= LedgerAppliedAnything(VariableResult.Level2);
		Messages.Add(FString::Printf(TEXT("[add_variables] %s"), *VariableResult.Message));
		Operations.Add(MakeShared<FJsonValueObject>(VariableResult.Level2));
	}

	// Reparent (single string, not array)
	if (Cmd->HasField(TEXT("reparent")) && (!bPlannedBatch || BatchContext.Errors.IsEmpty()))
	{
		FString NewParent = Cmd->GetStringField(TEXT("reparent"));
		FNwiroBPResult R = DoReparent(BP, NewParent);
		if (bPlannedBatch && !R.bSuccess) NwiroBlueprintBatch::Reject(R.Message);
		Messages.Add(FString::Printf(TEXT("[reparent] %s"), *R.Message));
		if (!R.bSuccess) bAllOk = false;
		// reparent runs OUTSIDE RunOp, so it has to report itself to the save gate below.
		// It sets BP->ParentClass in memory and nothing else marks the package, so leaving
		// these unset left a SUCCESSFUL reparent uncompiled and unwritten whenever every
		// other op in the same call applied nothing. A failed reparent returns before it
		// mutates, so bSuccess is the right predicate.
		bAnyOpRequested = true;
		bAnyNonFunctionOpRequested = true;
		bAnyNonDispatcherMutation |= R.bSuccess;
	}

	RunOp(TEXT("remove_variables"), [&](const auto& A) { return DoRemoveVariables(BP, A); });
	RunOp(TEXT("rename_variables"), [&](const auto& A) { return DoRenameVariables(BP, A); });
	RunOp(TEXT("add_components"), [&](const auto& A) { return DoAddComponents(BP, A); });
	RunOp(TEXT("remove_components"), [&](const auto& A) { return DoRemoveComponents(BP, A); });
	RunOp(TEXT("set_component_properties"), [&](const auto& A) { return DoSetComponentProperties(BP, A); });
	RunOp(TEXT("remove_functions"), [&](const auto& A) { return DoRemoveFunctions(BP, A); });
	RunOp(TEXT("add_custom_events"), [&](const auto& A) { return DoAddCustomEvents(BP, A); });
	RunOp(TEXT("add_event_dispatchers"), [&](const auto& A)
	{
		return DoAddEventDispatchers(BP, A, &DispatcherMutations);
	});
	RunOp(TEXT("add_interfaces"), [&](const auto& A) { return DoAddInterfaces(BP, A); });
	RunOp(TEXT("remove_interfaces"), [&](const auto& A) { return DoRemoveInterfaces(BP, A); });
	RunOp(TEXT("add_nodes"), [&](const auto& A) { return DoAddNodes(BP, GraphName, A, EnhancedInputActions); });
	RunOp(TEXT("remove_nodes"), [&](const auto& A) { return DoRemoveNodes(BP, GraphName, A); });
	RunOp(TEXT("connect_pins"), [&](const auto& A) { return DoConnectPins(BP, GraphName, A); });
	RunOp(TEXT("break_connections"), [&](const auto& A) { return DoBreakConnections(BP, GraphName, A); });
	RunOp(TEXT("set_pin_defaults"), [&](const auto& A) { return DoSetPinDefaults(BP, GraphName, A); });

	if (bPlannedBatch && BatchContext.Errors.Num() > 0)
	{
		bAllOk = false;
		bBlockCompileAndPersistence = true;
	}

	// Compile if requested (default: true)
	bool bCompile = true;
	if (Cmd->HasField(TEXT("compile")))
	{
		bCompile = Cmd->GetBoolField(TEXT("compile"));
	}
	const bool bAnyMutation =
		bAnyNonDispatcherMutation || bDispatcherMutationActive;
	if (bDispatcherRollbackRestoredState
		&& !bPackageWasDirtyAtEntry && !bAnyMutation)
	{
		// Structural rollback dirties both package and Blueprint metadata even when it
		// exactly restores the asset. Never erase pre-existing work observed on entry.
		RestoreCleanStateAfterVerifiedDispatcherRollback();
	}

	// Compiling marks the package dirty and the persist step below then WRITES the
	// .uasset. Doing that for a call that changed nothing leaves the user with a modified
	// binary asset for an operation that did nothing — spurious source-control churn, and
	// it makes "did this call change anything?" unanswerable from the working tree, which
	// is the question this whole contract exists to answer.
	// Anything that landed marks the package, so pending work survives a LATER call whose own
	// ops all fail. Our mutations bypass UE's dirty flag — measured: add_nodes with
	// compile:false leaves the node in memory while IsDirty() still reports clean — so this
	// is explicit rather than relying on the engine to notice.
	if (bAnyMutation)
	{
		BP->MarkPackageDirty();
	}

	// Three ways to earn a save, and the third is the one that matters: at the baseline this
	// block was unconditional, so a call whose ops all failed still wrote the asset and thereby
	// flushed whatever an earlier `compile:false` batch had left in memory. Gating on THIS
	// call's mutations alone stranded that work, which is a worse failure than the no-op churn
	// the gate exists to stop — an unsaved Blueprint can fail to load after an editor restart.
	const bool bExplicitCompile = Cmd->HasTypedField<EJson::Boolean>(TEXT("compile")) && Cmd->GetBoolField(TEXT("compile"));
	const bool bFunctionOnlyNoOp = bFunctionBatchNoOp && !bAnyNonFunctionOpRequested && !bExplicitCompile;
	const bool bShouldCompile = bCompile && !bBlockCompileAndPersistence && !bFunctionOnlyNoOp
		&& (bAnyMutation || !bAnyOpRequested || bPackageWasDirtyAtEntry);

	if (bShouldCompile)
	{
		bCompiled = true;
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(
			BP, EBlueprintCompileOptions::SkipSave);

		// Reconstruct Timeline nodes after compile so track pins appear
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (UK2Node_Timeline* TLN = Cast<UK2Node_Timeline>(N))
				{
					TLN->ReconstructNode();
				}
			}
		}

		// Recompile again after reconstruction to pick up new pins
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(
			BP, EBlueprintCompileOptions::SkipSave, &Results);

		// Collect compiler errors so the LLM can self-correct
		TArray<FString>& CompileErrors = CompileDiagnostics;

		// Blueprint-level messages from the compiler log (type mismatches, missing implementations, etc.)
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			if (Msg->GetSeverity() == EMessageSeverity::Error)
				CompileErrors.Add(Msg->ToText().ToString());
		}

		// Node-level errors — include actual pin names so the LLM can wire the correct pin
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node->ErrorType <= EMessageSeverity::Error && !Node->ErrorMsg.IsEmpty())
				{
					TArray<FString> InPins, OutPins;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->PinName.IsNone()) continue;
						(Pin->Direction == EGPD_Input ? InPins : OutPins).Add(Pin->PinName.ToString());
					}
					FString PinInfo;
					if (InPins.Num() > 0 || OutPins.Num() > 0)
						PinInfo = FString::Printf(TEXT(" [pins: inputs=[%s] outputs=[%s]]"),
							*FString::Join(InPins, TEXT(",")), *FString::Join(OutPins, TEXT(",")));
					CompileErrors.AddUnique(FString::Printf(TEXT("Node '%s':%s UE: %s"),
						*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *PinInfo, *Node->ErrorMsg));
				}
			}
		}
		// Enrich compile errors with actionable hints grounded in UE's own messages
		for (FString& CE : CompileErrors)
		{
			if (CE.Contains(TEXT("Array inputs")) && CE.Contains(TEXT("must have an input wired")))
				CE += TEXT(" Hint: add a MakeArray node of the required element type and connect its output to this array pin.");
			else if (CE.Contains(TEXT("is not a")) && CE.Contains(TEXT("Target")) && CE.Contains(TEXT("must have a connection")))
				CE += TEXT(" Hint: connect this node's Target/self pin to an object of the correct type — do not leave it as blueprint self unless the blueprint inherits from that type.");
		}

		const bool bGlobalCompileClean =
			Results.NumErrors == 0 && BP->Status != EBlueprintStatus::BS_Error;
		bool bGeneratedInvariantFailure = false;
		if (bGlobalCompileClean)
		{
			CompileStatus = TEXT("success");
			ValidationLevel = TEXT("generated");
			for (FNwiroDispatcherMutation& Mutation : DispatcherMutations)
			{
				if (!Mutation.bCreated || Mutation.bRolledBack) continue;
				FString GeneratedError;
				const bool bGeneratedValid =
					NwiroDispatcherPrivate::ValidateDispatcherInvariant(
						BP, Mutation, BP->GeneratedClass, GeneratedError);
				Mutation.GeneratedSignatureValid = bGeneratedValid;
				if (!bGeneratedValid)
				{
					Mutation.ErrorCode = TEXT("GENERATED_SIGNATURE_INVALID");
					Mutation.Error = GeneratedError;
					bGeneratedInvariantFailure = true;
					break;
				}
			}
			if (bGeneratedInvariantFailure)
			{
				FString RollbackError;
				const bool bRollbackStructurallyVerified =
					NwiroDispatcherPrivate::RollbackMutations(
						BP, DispatcherMutations, RollbackError);
				FCompilerResultsLog CleanupResults;
				FKismetEditorUtilities::CompileBlueprint(
					BP, EBlueprintCompileOptions::SkipSave, &CleanupResults);
				const bool bCleanupCompileClean =
					CleanupResults.NumErrors == 0
					&& BP->Status != EBlueprintStatus::BS_Error;
				FString AbsenceError;
				const bool bRollbackReflectionVerified =
					NwiroDispatcherPrivate::VerifyRollbackAbsence(
						BP, DispatcherMutations, true, AbsenceError);
				if (!bRollbackReflectionVerified)
				{
					RollbackError += RollbackError.IsEmpty()
						? AbsenceError : TEXT("; ") + AbsenceError;
				}
				if (!bCleanupCompileClean)
				{
					RollbackError += RollbackError.IsEmpty()
						? TEXT("cleanup compile failed")
						: TEXT("; cleanup compile failed");
				}
				const bool bRollbackFullyVerified =
					bRollbackStructurallyVerified
						&& bRollbackReflectionVerified && bCleanupCompileClean;
				if (bRollbackFullyVerified)
				{
					bDispatcherMutationActive = false;
					bDispatcherRollbackRestoredState = true;
					RestoreCleanStateAfterVerifiedDispatcherRollback();
				}
				else
				{
					bBlockCompileAndPersistence = true;
				}
				if (!bRollbackFullyVerified)
				{
					RollbackError += RollbackError.IsEmpty()
						? TEXT("dispatcher rollback was not fully verified")
						: TEXT("; dispatcher rollback was not fully verified");
					NwiroDispatcherPrivate::MarkRollbackFailure(
						DispatcherMutations, RollbackError);
				}
				if (!RollbackError.IsEmpty())
				{
					CompileDiagnostics.AddUnique(RollbackError);
				}
				CompileStatus = TEXT("error");
				ValidationLevel = TEXT("skeleton");
				bAllOk = false;
				Messages.Add(bRollbackFullyVerified
					? TEXT("[compile] Generated dispatcher invariant failed; only the new dispatcher batch was rolled back")
					: TEXT("[compile] Generated dispatcher invariant failed; dispatcher rollback could not be fully verified"));
			}
			else if (BP->Status == EBlueprintStatus::BS_UpToDateWithWarnings)
			{
				const FString WarnList = CompileErrors.Num() > 0
					? FString::Join(CompileErrors, TEXT("; "))
					: TEXT("check graph for warnings");
				Messages.Add(FString::Printf(
					TEXT("[compile] Blueprint compiled with warning(s): %s"), *WarnList));
			}
			else
			{
				Messages.Add(TEXT("[compile] Blueprint compiled"));
			}
		}
		else
		{
			CompileStatus = TEXT("error");
			ValidationLevel = TEXT("skeleton");
			if (CompileErrors.Num() == 0)
				CompileErrors.Add(TEXT("Blueprint has compile errors — check node connections and pin types"));
			FString ErrorList = FString::Join(CompileErrors, TEXT("; "));
			Messages.Add(FString::Printf(TEXT("[compile] Blueprint compiled with error(s): %s"), *ErrorList));
			// Don't flip bAllOk — see IK rationale: a downstream compile error
			// is not a tool failure when the requested edits succeeded.
		}
	}

	// Build result JSON
	// Persist edited Blueprint to disk so the generated class survives an
	// editor restart (else "derives from an invalid class"). Save only when
	// the class is constructible; seed the CDO first. Skipped when ?compile=false,
	// and when the call requested ops but none applied anything. Recompute after
	// generated-signature validation because that phase may roll back the dispatcher.
	bool bPersisted = false;
	bool bSaveAttempted = false;
	// An explicit compile may refresh a function-only no-op, but never saves it.
	const bool bShouldPersist = bCompile && !bBlockCompileAndPersistence
		&& (!bFunctionBatchNoOp || bAnyNonFunctionOpRequested)
		&& (bAnyNonDispatcherMutation || bDispatcherMutationActive
			|| !bAnyOpRequested || bPackageWasDirtyAtEntry);
	if (bShouldPersist)
	{
		UClass* GenCls = BP->GeneratedClass;
		if (GenCls && GenCls->IsValidLowLevel()
			&& GenCls->ClassConstructor != nullptr
			&& GenCls->ClassWithin != nullptr)
		{
			GenCls->GetDefaultObject(true);
			BP->MarkPackageDirty();
			bSaveAttempted = true;
			bPersisted = UEditorAssetLibrary::SaveLoadedAsset(BP, false);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), bAllOk);
	if (!bPersisted && BatchContext.TargetSaveAttempts > 0)
		Result->SetField(TEXT("savedToDisk"), MakeShared<FJsonValueNull>()); // pre-save events prove an attempt, never its success
	else Result->SetBoolField(TEXT("savedToDisk"), bPersisted);
	Result->SetBoolField(TEXT("saveAttempted"), bSaveAttempted || BatchContext.TargetSaveAttempts > 0);
	if (bPlannedBatch)
	{
		const TSharedRef<FJsonObject> Batch = MakeShared<FJsonObject>();
		Batch->SetStringField(TEXT("phase"), bBlockCompileAndPersistence ? TEXT("commit_failed") : TEXT("committed"));
		Batch->SetStringField(TEXT("currentRequestMutation"), bBlockCompileAndPersistence ? TEXT("partial_or_unknown") : TEXT("admitted_operations_executed"));
		Batch->SetNumberField(TEXT("requestedUnits"), BatchContext.RequestedUnits);
		Batch->SetNumberField(TEXT("attemptedCommitUnits"), BatchContext.AttemptedCommitUnits);
		Batch->SetNumberField(TEXT("internalCompileAttempts"), BatchContext.InternalCompileAttempts);
		Batch->SetField(TEXT("appliedUnits"), MakeShared<FJsonValueNull>());
		Batch->SetBoolField(TEXT("countsComplete"), false);
		Batch->SetNumberField(TEXT("stagingCompileAttempts"), BatchContext.StagingCompileAttempts);
		Batch->SetNumberField(TEXT("creationCompileAttempts"), BatchContext.CreationCompileAttempts);
		Batch->SetNumberField(TEXT("targetCompileAttempts"), BatchContext.TargetCompileAttempts);
		Batch->SetNumberField(TEXT("targetSaveAttempts"), BatchContext.TargetSaveAttempts);
		Result->SetObjectField(TEXT("batchAdmission"), Batch);
	}

	Result->SetBoolField(TEXT("compiled"), bCompiled || BatchContext.TargetCompileAttempts > 0);
	Result->SetStringField(TEXT("compileStatus"), CompileStatus);
	Result->SetStringField(TEXT("validationLevel"), ValidationLevel);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	NwiroDispatcherPrivate::SetDispatcherArray(Result, DispatcherMutations);

	TArray<TSharedPtr<FJsonValue>> CompileDiagnosticValues;
	for (const FString& Diagnostic : CompileDiagnostics)
	{
		CompileDiagnosticValues.Add(MakeShared<FJsonValueString>(Diagnostic));
	}
	Result->SetArrayField(TEXT("compileDiagnostics"), CompileDiagnosticValues);

	TArray<TSharedPtr<FJsonValue>> MsgArr;
	for (const FString& M : Messages)
		MsgArr.Add(MakeShareable(new FJsonValueString(M)));
	Result->SetArrayField(TEXT("messages"), MsgArr);

	// Level-2 acceptance, per migrated sub-operation. Emitted only when at least one
	// operation reported, so a caller can tell "nothing reported" from "reported that
	// nothing happened".
	//
	// Deliberately NO policy_version: this shape matches neither policy 1.1 (flat,
	// single-op) nor any ratified successor, and edit_blueprint is a batch OF batches, so
	// the (op, unit) pair is a dimension 1.1 does not have. The `operations` key is the
	// feature detector instead — a key cannot drift from the shape it describes.
	// When the policies are collapsed and versioned once, the label goes here.
	if (Operations.Num() > 0)
	{
		Result->SetArrayField(TEXT("operations"), Operations);
	}

	if (AliasWarnings.Num() > 0 || HandlerWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& WarnStr : AliasWarnings)
			WarnArr.Add(MakeShareable(new FJsonValueString(WarnStr)));
		for (const FString& WarnStr : HandlerWarnings)
			WarnArr.Add(MakeShareable(new FJsonValueString(WarnStr)));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	// Post-edit state summary so the agent can see what's actually in the BP
	// now and judge whether the user's request is satisfied. Without this the
	// agent has to issue a separate read_blueprint, which it often skips.
	int32 VarCount = 0, CompCount = 0, FuncCount = 0, EventNodeCount = 0;
	if (BP->NewVariables.Num() > 0) VarCount = BP->NewVariables.Num();
	if (USimpleConstructionScript* SCS = BP->SimpleConstructionScript)
	{
		CompCount = SCS->GetAllNodes().Num();
	}
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetFName() != UEdGraphSchema_K2::FN_UserConstructionScript)
			FuncCount++;
	}
	// Two counts, because they answer different questions. EventNodeCount is "how many nodes
	// are in the event graph" and stays literal. AuthoredNodeCount is "how much of this did
	// somebody actually build", and it is what the emptiness check below needs: UE creates
	// BeginPlay / Tick / ActorBeginOverlap in every fresh Actor Blueprint, so the literal
	// count is 3 on a Blueprint nobody has touched — which made the `== 0` guard below
	// unreachable for the most common Blueprint type, exactly when it was most needed.
	//
	// An unwired auto-created event is not authored work; the same event with something
	// connected to it is.
	int32 AuthoredNodeCount = 0;
	for (UEdGraph* G : BP->UbergraphPages)
	{
		if (!G) continue;
		EventNodeCount += G->Nodes.Num();

		for (UEdGraphNode* N : G->Nodes)
		{
			if (!N) continue;
			bool bHasLink = false;
			for (UEdGraphPin* P : N->Pins)
			{
				if (P && P->LinkedTo.Num() > 0) { bHasLink = true; break; }
			}
			if (N->IsA<UK2Node_Event>() && !bHasLink) continue;
			++AuthoredNodeCount;
		}
	}
	TSharedRef<FJsonObject> State = MakeShareable(new FJsonObject());
	State->SetNumberField(TEXT("variables"), VarCount);
	State->SetNumberField(TEXT("components"), CompCount);
	State->SetNumberField(TEXT("functions"), FuncCount);
	State->SetNumberField(TEXT("eventGraphNodes"), EventNodeCount);
	// Exposed, not just used internally: the guard below decides on this number, and a
	// caller cannot evaluate a verdict it is not allowed to see.
	State->SetNumberField(TEXT("authoredEventNodes"), AuthoredNodeCount);
	State->SetStringField(TEXT("parentClass"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("(none)"));
	Result->SetObjectField(TEXT("currentState"), State);

	// nextSteps: nudge the agent toward completion if the BP still looks
	// empty for its parent class. Generic, not scenario-specific.
	TArray<TSharedPtr<FJsonValue>> Steps;
	if (VarCount == 0 && CompCount <= 1 && AuthoredNodeCount == 0)
	{
		Steps.Add(MakeShareable(new FJsonValueString(TEXT(
			"This BP is essentially empty after the edit. If the user asked for behavior (variables, components, event logic), keep editing — your work is NOT done."))));
	}
	if (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()) &&
		!BP->ParentClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		Steps.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT(
			"To make this Actor appear in-game, call spawn_actor with blueprint='%s'. Just editing the BP does not place it in the level."), *BP->GetPathName()))));
	}
	if (Steps.Num() > 0)
		Result->SetArrayField(TEXT("nextSteps"), Steps);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND BLUEPRINT NODES
// ============================================================

FString FNwiroBlueprintTools::FindBlueprintNodes(const FString& Query, const FString& BlueprintPath)
{
	UBlueprint* BP = nullptr;
	if (!BlueprintPath.IsEmpty())
	{
		BP = LoadBP(BlueprintPath);
	}

	TArray<TSharedPtr<FJsonValue>> Results;

	const int32 MaxResults = 50;
	TSet<FString> Seen;

	// Tokenize the query so multi-word searches like "Add float" or "Sin math"
	// can hit "Add_DoubleDouble" / "Sin_FloatFloat". ANY token match counts;
	// requiring ALL was too strict because the LLM mixes display/internal names.
	// Also expand float<->double so "float" matches "Double" (UE 5.5+ promoted
	// math operators to double precision).
	TArray<FString> QueryTokens;
	if (!Query.IsEmpty())
	{
		TArray<FString> Raw;
		Query.ParseIntoArray(Raw, TEXT(" "), /*CullEmpty*/ true);
		for (const FString& T : Raw)
		{
			QueryTokens.Add(T);
			if (T.Equals(TEXT("float"), ESearchCase::IgnoreCase))  QueryTokens.Add(TEXT("double"));
			if (T.Equals(TEXT("double"), ESearchCase::IgnoreCase)) QueryTokens.Add(TEXT("float"));
		}
	}

	// Walk every loaded UClass and inspect its declared UFunctions. This is
	// far more reliable than going through FBlueprintActionDatabase, which
	// stores function spawners under several different subclasses and often
	// returns nothing for KismetMathLibrary on a fresh load.
	//
	// We accept any function flagged BlueprintCallable / BlueprintPure /
	// BlueprintEvent. The class name is reported alongside so the LLM knows
	// where to find it (e.g. "KismetMathLibrary").
	const uint64 BPMask = FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent;

	// Score a hit so we can sort later. Higher = more relevant.
	//   +100  function name starts with the query
	//   +50   function name contains the query as a whole token
	//   +10   any token from the (expanded) query is a substring of the function
	//   +20   class is one of the well-known math/system libraries
	auto ScoreHit = [&Query, &QueryTokens](const FString& FuncName, const FString& ClassName) -> int32
	{
		int32 Score = 0;

		if (!Query.IsEmpty())
		{
			if (FuncName.StartsWith(Query, ESearchCase::IgnoreCase)) Score += 100;
			else if (FuncName.Contains(Query, ESearchCase::IgnoreCase)) Score += 50;
		}

		for (const FString& Tok : QueryTokens)
		{
			if (FuncName.Contains(Tok, ESearchCase::IgnoreCase)) { Score += 10; break; }
		}

		if (ClassName == TEXT("KismetMathLibrary")    ||
			ClassName == TEXT("KismetSystemLibrary")  ||
			ClassName == TEXT("KismetStringLibrary")  ||
			ClassName == TEXT("KismetTextLibrary")    ||
			ClassName == TEXT("GameplayStatics"))
		{
			Score += 20;
		}

		return Score;
	};

	struct FHit { FString FuncName; FString ClassName; int32 Score; };
	TArray<FHit> Hits;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (!Class) continue;
		// Skip skeleton/REINST classes — stale duplicates
		if (Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_"))) continue;

		const FString ClassName = Class->GetName();

		for (TFieldIterator<UFunction> FnIt(Class, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			UFunction* Fn = *FnIt;
			if (!Fn) continue;
			if (!(Fn->FunctionFlags & BPMask)) continue;

			const FString FuncName = Fn->GetName();

			// Function-name only match — class names like "BlueprintFns"
			// were producing false positives for "Print", "Sin", etc.
			if (QueryTokens.Num() > 0)
			{
				bool bAnyTokenHit = false;
				for (const FString& Tok : QueryTokens)
				{
					if (FuncName.Contains(Tok, ESearchCase::IgnoreCase)) { bAnyTokenHit = true; break; }
				}
				if (!bAnyTokenHit) continue;
			}

			const FString DedupeKey = ClassName + TEXT("::") + FuncName;
			if (Seen.Contains(DedupeKey)) continue;
			Seen.Add(DedupeKey);

			Hits.Add({ FuncName, ClassName, ScoreHit(FuncName, ClassName) });
		}
	}

	// Sort by score descending so the most relevant hits come first
	Hits.Sort([](const FHit& A, const FHit& B) { return A.Score > B.Score; });

	const int32 Take = FMath::Min(Hits.Num(), MaxResults);
	for (int32 i = 0; i < Take; ++i)
	{
		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("function"), Hits[i].FuncName);
		Obj->SetStringField(TEXT("class"), Hits[i].ClassName);
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	int32 Count = Results.Num();

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetArrayField(TEXT("nodes"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());
	NwiroEnhancedInputContract::AddDiscovery(Query, Root);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// ASSET LOADING
// ============================================================

UBlueprint* FNwiroBlueprintTools::LoadBP(const FString& PathOrName, FString* OutError)
{
	FString LocalError;
	if (!OutError) OutError = &LocalError;
	OutError->Reset();
	// Mounted package/object paths and export-text paths retain exact identity.
	// A rejection is terminal: no alias, fuzzy name, or character fallback loads.
	if (PathOrName.Contains(TEXT("/")))
	{
		const FString Path = PathOrName.StartsWith(TEXT("/")) || PathOrName.Contains(TEXT("'"))
			? PathOrName : TEXT("/Game/") + PathOrName;
		return Cast<UBlueprint>(NwiroSafeExactAssetLoad(Path, OutError));
	}
	FString ValidatedPath;
	if (!NwiroBlueprintPreload::NormalizeExactAssetPath(TEXT("/Game/") + PathOrName, ValidatedPath))
	{
		if (OutError) *OutError = TEXT("invalid_asset_path");
		return nullptr;
	}

	// Search by name in asset registry
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Exact name match
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Equals(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UBlueprint>(NwiroSafeRegistryLoad(Asset, OutError));
		}
	}

	// Partial name match (e.g., "ThirdPersonCharacter" matches "BP_ThirdPersonCharacter")
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UBlueprint>(NwiroSafeRegistryLoad(Asset, OutError));
		}
	}

	// Smart fallback: if the name contains "Character" or "Pawn", find any character blueprint
	if (PathOrName.Contains(TEXT("Character"), ESearchCase::IgnoreCase) || PathOrName.Contains(TEXT("Pawn"), ESearchCase::IgnoreCase))
	{
		UBlueprint* BestMatch = nullptr;
		for (const FAssetData& Asset : Assets)
		{
			FString Name = Asset.AssetName.ToString();
			// Skip GameMode, Controller, etc.
			if (Name.Contains(TEXT("GameMode")) || Name.Contains(TEXT("Controller")) || Name.Contains(TEXT("HUD")))
			{
				continue;
			}

			// Only consider plain Blueprints — WidgetBlueprint/AnimBlueprint
			// etc. don't derive from Character/Pawn anyway, and forcing a
			// load on a compile-failed Blueprint can cascade into UE-killing
			// CDO failures when its skeleton class is broken.
			const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
			if (ClassName != TEXT("Blueprint")) continue;

			// Use the GeneratedClass tag from the asset registry instead of
			// loading the asset. Forcing GetAsset() on a compile-failed BP
			// triggers `Ensure condition failed: ClassDefaultObject != nullptr`
			// and in some sessions escalates to a process exit.
			FString NativeParent;
			if (Asset.GetTagValue(TEXT("NativeParentClass"), NativeParent) && !NativeParent.IsEmpty())
			{
				const bool bIsCharOrPawn =
					NativeParent.Contains(TEXT("Character")) ||
					NativeParent.Contains(TEXT("Pawn"));
				if (!bIsCharOrPawn) continue;
			}
			if (!Asset.IsAssetLoaded()) continue;
			UBlueprint* CandidateBP = Cast<UBlueprint>(NwiroSafeRegistryLoad(Asset, OutError));
			if (!CandidateBP && OutError && !OutError->IsEmpty()) return nullptr;
			if (CandidateBP && CandidateBP->ParentClass)
			{
				if (CandidateBP->ParentClass->IsChildOf(ACharacter::StaticClass()) ||
					CandidateBP->ParentClass->IsChildOf(APawn::StaticClass()))
				{
					UE_LOG(LogNwiroBP, Log, TEXT("Smart fallback: '%s' not found, using '%s' instead"), *PathOrName, *Name);
					BestMatch = CandidateBP;
					break;
				}
			}
		}
		if (BestMatch) return BestMatch;
	}

	return Cast<UBlueprint>(NwiroSafeExactAssetLoad(ValidatedPath, OutError));
}

// ============================================================
// GRAPH HELPERS
// ============================================================

UEdGraph* FNwiroBlueprintTools::FindGraph(UBlueprint* BP, const FString& GraphName)
{
	if (!BP) return nullptr;

	// Check event graphs
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Check function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Check interface implementation graphs — created via create_function_graph
	// with override=true on a function from an implemented interface.
	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : Iface.Graphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
	}

	return nullptr;
}

// Strip everything that isn't a letter or digit and lowercase the rest, so
// "Event BeginPlay", "event_begin_play", "EventBeginPlay" and "eventbeginplay"
// all collapse to the same canonical key.
static FString NormalizeRefKey(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());
	for (TCHAR C : In)
	{
		if (FChar::IsAlnum(C)) Out.AppendChar(FChar::ToLower(C));
	}
	return Out;
}

UEdGraphNode* FNwiroBlueprintTools::FindNodeByRef(UEdGraph* Graph, const FString& Ref,
	FString* OutAmbiguity)
{
	if (!Graph) return nullptr;

	// 1. Exact session reference (set by add_nodes via the LLM-supplied ref).
	if (const FNwiroNodeRef* Found = NodeRefs.Find(MakeNodeRefKey(Graph, Ref)))
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Found->NodeGuid) return Node;
		}
	}

	// 2. Normalized session reference — handles `printString` vs `PrintString` vs `print_string`.
	const FString NormRef = NormalizeRefKey(Ref);
	if (!NormRef.IsEmpty())
	{
		const FString OwnerKey = NodeRefOwnerKey(Graph);
		for (const auto& Pair : NodeRefs)
		{
			// Compare the stored bare ref, not the composite key, and only within this
			// Blueprint's own bindings.
			if (Pair.Value.BlueprintPath == OwnerKey && NormalizeRefKey(Pair.Value.Ref) == NormRef)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && Node->NodeGuid == Pair.Value.NodeGuid) return Node;
				}
			}
		}
	}

	// 3. GUID — accept any of the FGuid string formats UE supports.
	{
		FGuid TestGuid;
		if (FGuid::Parse(Ref, TestGuid))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == TestGuid) return Node;
			}
		}
	}

	// 4. Match by normalized node title (covers "Event BeginPlay" / "Print String" / "Delay" etc.).
	//
	// The only step that can match more than one node: a title is display text, not
	// identity. Returning the first match wrote to the WRONG node and reported success, so
	// multiple matches are refused and named with their GUIDs, which steps 1-3 accept.
	if (!NormRef.IsEmpty())
	{
		auto ReportAmbiguity = [&](const TArray<UEdGraphNode*>& Hits)
		{
			if (!OutAmbiguity) return;
			TArray<FString> Candidates;
			for (UEdGraphNode* N : Hits)
			{
				Candidates.Add(FString::Printf(TEXT("'%s' (%s)"),
					*N->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
			}
			*OutAmbiguity = FString::Printf(
				TEXT("Ref '%s' matches %d nodes: %s. Refusing to guess — retry with one of these GUIDs."),
				*Ref, Hits.Num(), *FString::Join(Candidates, TEXT(" | ")));
		};

		// 4a. exact normalized title equality
		{
			TArray<UEdGraphNode*> Hits;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				const FString Title = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
				if (NormalizeRefKey(Title) == NormRef) Hits.Add(Node);
			}
			if (Hits.Num() == 1) return Hits[0];
			// An ambiguous 4a must NOT fall through to 4b. 4b is strictly looser, so it could
			// resolve to a single node that 4a already showed was one of several equally valid
			// readings of the same ref — turning a detected ambiguity back into a silent guess.
			if (Hits.Num() > 1) { ReportAmbiguity(Hits); return nullptr; }
		}
		// 4b. partial substring match against full title
		{
			TArray<UEdGraphNode*> Hits;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				const FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				if (NormalizeRefKey(FullTitle).Contains(NormRef)) Hits.Add(Node);
			}
			if (Hits.Num() == 1) return Hits[0];
			if (Hits.Num() > 1) { ReportAmbiguity(Hits); return nullptr; }
		}
	}

	return nullptr;
}

// Translation-unit private: native specs can assert identity without a public API.
namespace NwiroPinResolution
{
static UEdGraphPin* Resolve(UEdGraphNode* Node, const FString& PinName,
	EEdGraphPinDirection Dir, FString* OutAmbiguity)
{
	if (OutAmbiguity) OutAmbiguity->Reset();
	if (!Node || PinName.IsEmpty()) return nullptr;

	// Common synonyms the LLM mixes up. Map every input alias to a list of
	// candidate canonical names so we try all of them.
	auto Aliases = [](const FString& In) -> TArray<FString>
	{
		const FString N = NormalizeRefKey(In); // strip non-alnum, lowercase
		TArray<FString> Out = { In };
		if (N == TEXT("execute") || N == TEXT("exec") || N == TEXT("in") || N == TEXT("input"))
		{
			Out.Append({ TEXT("execute"), TEXT("exec"), TEXT("then") });
		}
		else if (N == TEXT("then") || N == TEXT("next") || N == TEXT("out") || N == TEXT("output") || N == TEXT("completed"))
		{
			Out.Append({ TEXT("then"), TEXT("Completed") });
		}
		else if (N == TEXT("self") || N == TEXT("target"))
		{
			Out.Append({ TEXT("self"), TEXT("Target") });
		}
		else if (N == TEXT("returnvalue") || N == TEXT("return") || N == TEXT("result"))
		{
			Out.Append({ TEXT("ReturnValue") });
		}
		// Branch node pin aliases — docstring lists True/False but actual UE pins are then/else.
		else if (N == TEXT("true"))
		{
			Out.Append({ TEXT("then") });
		}
		else if (N == TEXT("false"))
		{
			Out.Append({ TEXT("else") });
		}
		return Out;
	};
	const TArray<FString> Candidates = Aliases(PinName);
	const FString Normalized = NormalizeRefKey(PinName);
	if (Normalized.IsEmpty()) return nullptr;
	TArray<UEdGraphPin*> Eligible;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && (Dir == EGPD_MAX || Pin->Direction == Dir)) Eligible.AddUnique(Pin);
	}
	bool bResolvedTier = false;
	auto UniqueMatch = [&](const TCHAR* Tier, const auto& Predicate) -> UEdGraphPin*
	{
		TArray<UEdGraphPin*> Matches;
		for (UEdGraphPin* Pin : Eligible) if (Predicate(Pin)) Matches.Add(Pin);
		bResolvedTier = Matches.Num() > 0;
		if (Matches.Num() == 1) return Matches[0];
		if (Matches.Num() > 1 && OutAmbiguity)
		{
			TArray<FString> Names;
			for (const UEdGraphPin* Pin : Matches)
			{
				Names.Add(FString::Printf(TEXT("%s [%s, pinId=%s]"),
					*Pin->PinName.ToString(), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
					*Pin->PinId.ToString()));
			}
			Names.Sort();
			*OutAmbiguity = FString::Printf(TEXT("AMBIGUOUS_PIN: '%s' on node %s matches %s at %s tier. Use the exact canonical pin name; duplicate canonical pins require repairing the node."),
				*PinName, *Node->NodeGuid.ToString(), *FString::Join(Names, TEXT(", ")), Tier);
		}
		return nullptr;
	};
	auto EqualsAtRank = [](const FString& Name, const FString& Wanted, int32 Rank)
	{
		if (Name.IsEmpty()) return false;
		return Rank == 0 ? Name.Equals(Wanted, ESearchCase::CaseSensitive)
			: Rank == 1 ? Name.Equals(Wanted, ESearchCase::IgnoreCase)
			: NormalizeRefKey(Name) == NormalizeRefKey(Wanted);
	};
	// WHOLE direction-filtered list: actual identity, then friendly text.
	// Literal spelling wins before case folding and punctuation removal.
	for (int32 Source = 0; Source < 2; ++Source)
	{
		for (int32 Rank = 0; Rank < 3; ++Rank)
		{
			UEdGraphPin* Match = UniqueMatch(Source == 0 ? TEXT("canonical") : TEXT("friendly"),
				[&](const UEdGraphPin* Pin)
			{
				return EqualsAtRank(Source == 0 ? Pin->PinName.ToString()
					: Pin->PinFriendlyName.ToString(), PinName, Rank);
			});
			if (bResolvedTier) return Match; // ambiguity also stops: no weaker fallback
		}
	}
	// Alias candidates are a SET, not ordered preferences. A unique legacy alias
	// remains valid; two eligible alternatives refuse regardless of pin order.
	for (int32 Source = 0; Source < 2; ++Source)
	{
		for (int32 Rank = 0; Rank < 3; ++Rank)
		{
			UEdGraphPin* Match = UniqueMatch(TEXT("legacy alias"), [&](const UEdGraphPin* Pin)
			{
				const FString Name = Source == 0 ? Pin->PinName.ToString() : Pin->PinFriendlyName.ToString();
				for (const FString& Candidate : Candidates)
				{
					if (EqualsAtRank(Name, Candidate, Rank)) return true;
				}
				return false;
			});
			if (bResolvedTier) return Match;
		}
	}
	// Preserve partial-name convenience only if ONE eligible pin matches.
	return UniqueMatch(TEXT("substring"), [&](const UEdGraphPin* Pin)
	{
		return Pin->PinName.ToString().Contains(PinName, ESearchCase::IgnoreCase);
	});
}
}

UEdGraphPin* FNwiroBlueprintTools::FindPin(UEdGraphNode* Node, const FString& PinName,
	EEdGraphPinDirection Dir, FString* OutAmbiguity)
{
	return NwiroPinResolution::Resolve(Node, PinName, Dir, OutAmbiguity);
}

void FNwiroBlueprintTools::StoreNodeRef(const FString& Ref, UEdGraphNode* Node, const FString& GraphName)
{
	if (!Ref.IsEmpty() && Node)
	{
		// The owning Blueprint comes from the node, so every existing call site keeps
		// working unchanged while the binding stops being global.
		FNwiroNodeRef NR;
		NR.NodeGuid = Node->NodeGuid;
		NR.GraphName = GraphName;
		NR.BlueprintPath = NodeRefOwnerKey(Node->GetGraph());
		NR.Ref = Ref;
		NodeRefs.Add(MakeNodeRefKey(Node->GetGraph(), Ref), NR);
	}
}

void FNwiroBlueprintTools::ClearNodeRefs()
{
	NodeRefs.Empty();
}

// ============================================================
// TYPE PARSING
// ============================================================

FEdGraphPinType FNwiroBlueprintTools::ParsePinType(const TSharedPtr<FJsonObject>& TypeObj)
{
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; // default

	if (!TypeObj.IsValid()) return PinType;

	FString Base = TypeObj->GetStringField(TEXT("base")).ToLower();

	if (Base == TEXT("bool") || Base == TEXT("boolean"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (Base == TEXT("byte"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
	}
	else if (Base == TEXT("int") || Base == TEXT("integer") || Base == TEXT("int32"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (Base == TEXT("int64"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (Base == TEXT("float"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("float");
	}
	else if (Base == TEXT("double"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("double");
	}
	else if (Base == TEXT("string") || Base == TEXT("fstring"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (Base == TEXT("name") || Base == TEXT("fname"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (Base == TEXT("text") || Base == TEXT("ftext"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (Base == TEXT("vector") || Base == TEXT("fvector"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (Base == TEXT("rotator") || Base == TEXT("frotator"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (Base == TEXT("transform") || Base == TEXT("ftransform"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (Base == TEXT("linearcolor") || Base == TEXT("color"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (Base == TEXT("object"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UClass* ObjClass = FindFirstObject<UClass>(*SubType);
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *SubType));
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *SubType);
			if (ObjClass)
			{
				PinType.PinSubCategoryObject = ObjClass;
			}
		}
	}
	else if (Base == TEXT("class"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UClass* ObjClass = FindFirstObject<UClass>(*SubType);
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *SubType));
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *SubType);
			if (ObjClass)
				PinType.PinSubCategoryObject = ObjClass;
		}
	}
	else if (Base == TEXT("struct"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UScriptStruct* Struct = FindFirstObject<UScriptStruct>( *SubType);
			if (Struct)
			{
				PinType.PinSubCategoryObject = Struct;
			}
		}
	}
	else if (Base == TEXT("enum"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UEnum* Enum = FindFirstObject<UEnum>( *SubType);
			if (Enum)
			{
				PinType.PinSubCategoryObject = Enum;
			}
		}
	}

	// Unknown 'base' fallback — treat as Object/Class/Struct/Enum name
	// instead of silently defaulting to bool. LLM often passes raw type names
	// like "SoundBase", "StaticMesh", "Pawn" without wrapping in object/subtype.
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
		&& !Base.IsEmpty() && Base != TEXT("bool") && Base != TEXT("boolean"))
	{
		const FString OrigBase = TypeObj->GetStringField(TEXT("base"));
		const FString TryNames[] = { OrigBase, FString(TEXT("U")) + OrigBase, FString(TEXT("F")) + OrigBase, FString(TEXT("E")) + OrigBase };
		for (const FString& Try : TryNames)
		{
			if (UClass* Cls = FindFirstObject<UClass>(*Try))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Cls;
				break;
			}
			if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Try))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = Struct;
				break;
			}
			if (UEnum* Enum = FindFirstObject<UEnum>(*Try))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				PinType.PinSubCategoryObject = Enum;
				break;
			}
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			if (UClass* Cls = StaticLoadClass(UObject::StaticClass(), nullptr,
				*FString::Printf(TEXT("/Script/Engine.%s"), *OrigBase)))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Cls;
			}
		}
	}

	// Container type
	FString Container = TypeObj->GetStringField(TEXT("container")).ToLower();
	if (Container == TEXT("array"))
	{
		PinType.ContainerType = EPinContainerType::Array;
	}
	else if (Container == TEXT("set"))
	{
		PinType.ContainerType = EPinContainerType::Set;
	}
	else if (Container == TEXT("map"))
	{
		PinType.ContainerType = EPinContainerType::Map;
	}

	return PinType;
}

// ============================================================
// ADD VARIABLES
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoAddVariables(UBlueprint* BP, const TArray<FNwiroPreparedVariable>& Items)
{
	FNwiroUnitLedger Ledger;
	TArray<FName> AddedNames;
	auto RemoveBatchAdditions = [&]()
	{
		if (AddedNames.IsEmpty()) return;
		FBlueprintEditorUtils::BulkRemoveMemberVariables(BP, AddedNames);
		// Record only actual residual members as created. A removed batch still
		// needs inspection because compiler side effects are not a state snapshot.
		Ledger.Created.RemoveAll([&](const TSharedPtr<FJsonValue>& Entry)
		{
			const FName Name(*Entry->AsObject()->GetStringField(TEXT("name")));
			return FBlueprintEditorUtils::FindNewVariableIndex(BP, Name) == INDEX_NONE
				&& (!BP->SkeletonGeneratedClass || !FindFProperty<FProperty>(BP->SkeletonGeneratedClass, Name))
				&& (!BP->GeneratedClass || !FindFProperty<FProperty>(BP->GeneratedClass, Name));
		});
	};
	for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
	{
		if (NwiroBlueprintBatch::StopCommit({}))
		{
			RemoveBatchAdditions();
			for (int32 Remaining = Unit; Remaining < Items.Num(); ++Remaining)
				Ledger.Skipped.Add(FNwiroUnitLedger::MakeUnit(Remaining, TEXT("add_variables"), Items[Remaining].Name.ToString(), false));
			FNwiroBPResult Result = FNwiroBPResult::Fail(TEXT("Variable commit interrupted; no further operations or compile/save were run"));
			Result.Level2 = Ledger.Build(TEXT("add_variables"), Items.Num());
			return Result;
		}
		const FNwiroPreparedVariable& Item = Items[Unit];
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, Item.Name, Item.PinType, Item.DefaultValue))
		{
			Ledger.AddError(Unit, TEXT("add_variables"), TEXT("ADD_REFUSED"), TEXT("name"), Item.Name.ToString(),
				TEXT("Unreal refused the prepared variable; inspect applied units before retrying"));
			RemoveBatchAdditions();
			FNwiroBPResult Result = FNwiroBPResult::Fail(TEXT("Variable application stopped. Cleanup attempted only this batch; created lists verified residual members. Structural compiler side effects require inspection. No further operations or full compile/save were run."));
			Result.Level2 = Ledger.Build(TEXT("add_variables"), Items.Num());
			return Result;
		}
		AddedNames.Add(Item.Name);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Item.Name);
		if (!BP->NewVariables.IsValidIndex(Index))
		{
			Ledger.Created.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("add_variables"), Item.Name.ToString(), false));
			RemoveBatchAdditions();
			FNwiroBPResult Result = FNwiroBPResult::Fail(TEXT("Unreal added a variable but its description could not be recovered; state requires inspection. No further operations or full compile were run."));
			Result.Level2 = Ledger.Build(TEXT("add_variables"), Items.Num());
			return Result;
		}
		FBPVariableDescription& Variable = BP->NewVariables[Index];
		if (Item.bReplicated) Variable.PropertyFlags |= CPF_Net;
		if (Item.bSaveGame) Variable.PropertyFlags |= CPF_SaveGame;
		if (Item.InstanceEditable.IsSet())
		{
			Variable.PropertyFlags |= CPF_Edit;
			if (Item.InstanceEditable.GetValue()) Variable.PropertyFlags &= ~CPF_DisableEditOnInstance;
			else Variable.PropertyFlags |= CPF_DisableEditOnInstance;
		}
		if (Item.bExposeOnSpawn)
		{
			Variable.PropertyFlags |= CPF_Edit;
			Variable.PropertyFlags &= ~CPF_DisableEditOnInstance;
			Variable.SetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
		}
		if (Item.Category.IsSet()) Variable.Category = FText::FromString(Item.Category.GetValue());
		if (Item.Tooltip.IsSet()) Variable.SetMetaData(FBlueprintMetadata::MD_Tooltip, Item.Tooltip.GetValue());
		Ledger.Created.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("add_variables"), Item.Name.ToString(), true,
			Item.RequestedType, Item.PinType.PinSubCategoryObject.IsValid() ? Item.PinType.PinSubCategoryObject->GetPathName() : Item.PinType.PinCategory.ToString()));
	}
	FNwiroBPResult Result = FNwiroBPResult::Ok(FString::Printf(TEXT("Added %d of %d variable(s)"), Items.Num(), Items.Num()));
	Result.Level2 = Ledger.Build(TEXT("add_variables"), Items.Num());
	return Result;
}

// ============================================================
// REMOVE VARIABLES
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoRemoveVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	using namespace NwiroDispatcherPrivate;
	struct FRemoval
	{
		FName Name;
		bool bHasMember = false;
		TArray<TObjectPtr<UEdGraph>> DelegateGraphs;
	};

	if (!BP || Items.IsEmpty())
	{
		return FNwiroBPResult::Fail(
			TEXT("REMOVAL_FAILED: remove_variables requires at least one existing name"));
	}

	TArray<FRemoval> Removals;
	TSet<FName> Seen;
	TArray<FString> Errors;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Item = Items[Index];
		FString RequestedName;
		if (Item.IsValid() && Item->Type == EJson::String)
		{
			RequestedName = Item->AsString();
		}
		else if (Item.IsValid() && Item->Type == EJson::Object
			&& Item->AsObject().IsValid())
		{
			if (!Item->AsObject()->TryGetStringField(TEXT("name"), RequestedName))
			{
				Errors.Add(FString::Printf(
					TEXT("item %d requires string field 'name'"), Index));
				continue;
			}
		}
		else
		{
			Errors.Add(FString::Printf(
				TEXT("item %d must be a name string or {name} object"), Index));
			continue;
		}

		FName Name;
		FString NameError;
		if (!ValidateRequestedName(RequestedName, Name, NameError))
		{
			Errors.Add(FString::Printf(TEXT("'%s': %s"), *RequestedName, *NameError));
			continue;
		}
		if (Seen.Contains(Name))
		{
			Errors.Add(FString::Printf(TEXT("duplicate removal name '%s'"), *RequestedName));
			continue;
		}
		Seen.Add(Name);

		FRemoval& Removal = Removals.AddDefaulted_GetRef();
		Removal.Name = Name;
		FBPVariableDescription* Member = FindDirectVariable(BP, Name);
		Removal.bHasMember = Member != nullptr;
		const bool bDelegateMember = Member
			&& Member->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
		for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
		{
			if (Graph && Graph->GetFName() == Name)
			{
				Removal.DelegateGraphs.Add(Graph);
			}
		}

		if (!Member && Removal.DelegateGraphs.IsEmpty())
		{
			Errors.Add(FString::Printf(
				TEXT("'%s' does not exist (removed count remains zero)"), *RequestedName));
		}
		else if (Member && !bDelegateMember && !Removal.DelegateGraphs.IsEmpty())
		{
			Errors.Add(FString::Printf(
				TEXT("'%s' has a non-delegate member plus delegate graph residue; refusing partial removal"),
				*RequestedName));
		}
	}

	if (!Errors.IsEmpty())
	{
		return FNwiroBPResult::Fail(FString::Printf(
			TEXT("REMOVAL_FAILED: removed 0 variable(s). %s"),
			*FString::Join(Errors, TEXT("; "))));
	}

	TArray<FName> MemberNames;
	bool bRemovedGraphOnly = false;
	for (FRemoval& Removal : Removals)
	{
		for (int32 GraphIndex = Removal.DelegateGraphs.Num() - 1;
			GraphIndex >= 0; --GraphIndex)
		{
			UEdGraph* Graph = Removal.DelegateGraphs[GraphIndex].Get();
			if (Graph && Graph->GetOuter() == BP)
			{
				FBlueprintEditorUtils::RemoveGraph(
					BP, Graph, EGraphRemoveFlags::MarkTransient);
				if (!Removal.bHasMember) bRemovedGraphOnly = true;
			}
		}
		if (Removal.bHasMember) MemberNames.Add(Removal.Name);
	}
	if (!MemberNames.IsEmpty())
	{
		FBlueprintEditorUtils::BulkRemoveMemberVariables(BP, MemberNames);
	}
	if (bRemovedGraphOnly)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	for (const FRemoval& Removal : Removals)
	{
		int32 GraphCount = 0;
		FindSignatureGraph(BP, Removal.Name, &GraphCount);
		if (FindDirectVariable(BP, Removal.Name) || GraphCount != 0)
		{
			return FNwiroBPResult::Fail(FString::Printf(
				TEXT("REMOVAL_FAILED: postcondition failed for '%s'"),
				*Removal.Name.ToString()));
		}
	}

	return FNwiroBPResult::Ok(FString::Printf(
		TEXT("Removed %d variable/dispatcher name(s) with matching signature graphs"),
		Removals.Num()));
}

// ============================================================
// ADD COMPONENTS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoAddComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	if (!BP->SimpleConstructionScript)
	{
		return FNwiroBPResult::Fail(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	int32 Added = 0;
	// Items refused outright, before anything was attached. Distinct from Errors, which also
	// collects soft problems that deliberately keep the op successful.
	int32 Rejected = 0;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		if (NwiroBlueprintBatch::StopCommit(Errors)) break;
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString CompName = Obj->GetStringField(TEXT("name"));
		// Bounded alias set: models frequently guess componentName/componentType/
		// componentClass (observed live with gemma, which also ignored the
		// corrective error text twice — docs alone don't save weak models).
		if (CompName.IsEmpty()) CompName = GetFieldNormalized(Obj, TEXT("componentname"));
		FString ClassName = Obj->GetStringField(TEXT("class"));
		if (ClassName.IsEmpty()) ClassName = Obj->GetStringField(TEXT("type"));
		if (ClassName.IsEmpty()) ClassName = GetFieldNormalized(Obj, TEXT("componenttype"));
		if (ClassName.IsEmpty()) ClassName = GetFieldNormalized(Obj, TEXT("componentclass"));
		FString ParentName = Obj->GetStringField(TEXT("parent"));

		if (CompName.IsEmpty() || ClassName.IsEmpty())
		{
			Errors.Add(TEXT("Component needs 'name' and either 'class' or 'type'. Example: {\"name\":\"CubeMesh\",\"class\":\"StaticMeshComponent\",\"properties\":{\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\"}}"));
			continue;
		}

		// Find the component class — try multiple name variations and KEEP
		// trying if the first hit isn't an ActorComponent. Without this, asking
		// for "StaticMesh" returned the asset class UStaticMesh (which exists
		// but isn't a component) and silently dropped the request.
		UClass* CompClass = nullptr;
		const TArray<FString> ClassTries = {
			ClassName,
			TEXT("U") + ClassName,
			ClassName + TEXT("Component"),
			TEXT("U") + ClassName + TEXT("Component"),
		};
		for (const FString& Try : ClassTries)
		{
			UClass* Candidate = FindFirstObject<UClass>(*Try);
			if (Candidate && Candidate->IsChildOf(UActorComponent::StaticClass()))
			{
				CompClass = Candidate;
				break;
			}
		}

		// >>> NWIRO_PARITY BEGIN: unresolved-class-rejected
		// Rejected++, like the abstract guard below. This branch predates the counter and used to
		// fall through to the older `Added > 0 || Errors.Num() == 0` return, so a batch that named
		// one good component and one class that does not exist came back as success: Added > 0 hid
		// the error entirely. Nothing marked that as deliberate, and leaving it would mean two
		// adjacent refusals in the same loop disagreeing about what "refused" does to the envelope.
		if (!CompClass)
		{
			Errors.Add(FString::Printf(TEXT("Component class not found: %s (tried: %s)"), *ClassName, *FString::Join(ClassTries, TEXT(", "))));
			Rejected++;
			continue;
		}
		// <<< NWIRO_PARITY END: unresolved-class-rejected

		// >>> NWIRO_PARITY BEGIN: abstract-class-guard
		// The resolution loop above only asks IsChildOf(UActorComponent), and UActorComponent
		// itself is UCLASS(..., abstract, ...) — as are UMovementComponent and several other
		// plausible things to name. Such a class passes that filter and reaches CreateNode below,
		// where the engine builds a template of a class it refuses to instantiate and warns that it
		// "will be nulled out on save": the edit appears to succeed, then the component is gone the
		// next time the Blueprint is loaded.
		//
		// Refused BEFORE the node is built, for the same reason as the makeRoot guard further down:
		// the remedy this message suggests is to re-send the item naming a concrete subclass, and
		// that only works if the requested name was never taken by a half-built component.
		if (CompClass->HasAnyClassFlags(CLASS_Abstract))
		{
			Errors.Add(FString::Printf(
				TEXT("Component class '%s' is abstract and cannot be instantiated. ")
				TEXT("Nothing was created for '%s'. Re-send it naming a concrete subclass."),
				*CompClass->GetName(), *CompName));
			Rejected++;
			continue;
		}
		// <<< NWIRO_PARITY END: abstract-class-guard

		USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(CompClass, FName(*CompName));
		if (!NewNode)
		{
			Errors.Add(FString::Printf(TEXT("Failed to create SCS node for: %s"), *CompName));
			continue;
		}

		// Check if makeRoot requested — if so, skip normal attachment
		bool bWantRoot = false;
		if (Obj->HasField(TEXT("makeRoot")))
		{
			TSharedPtr<FJsonValue> MRVal = Obj->TryGetField(TEXT("makeRoot"));
			if (MRVal.IsValid())
			{
				if (MRVal->Type == EJson::Boolean) bWantRoot = MRVal->AsBool();
				else if (MRVal->Type == EJson::String) bWantRoot = MRVal->AsString().Equals(TEXT("true"), ESearchCase::IgnoreCase);
			}
		}

		if (bWantRoot)
		{
			// CRITICAL: a parent class can already provide a NATIVE root component
			// (Character -> CapsuleComponent, etc.). You cannot replace a native
			// root from the SCS — adding this component as a second SCS root makes
			// FinishSpawning assert SceneRootComponent->GetOwner()==this
			// (Actor.cpp:3854) and crash UE at spawn time. So when the parent has
			// a native root, ignore makeRoot and nest this component UNDER it.
			bool bParentHasNativeRoot = false;
			if (BP->ParentClass)
			{
				if (AActor* CDO = BP->ParentClass->GetDefaultObject<AActor>())
				{
					if (USceneComponent* NR = CDO->GetRootComponent())
					{
						bParentHasNativeRoot = NR->CreationMethod == EComponentCreationMethod::Native;
					}
				}
			}

			if (bParentHasNativeRoot && CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				BP->SimpleConstructionScript->AddNode(NewNode);
				if (AActor* CDO = BP->ParentClass->GetDefaultObject<AActor>())
				{
					if (USceneComponent* NR = CDO->GetRootComponent())
					{
						NewNode->SetParent(NR);
					}
				}
				goto NodeAttached;
			}

			// >>> NWIRO_PARITY BEGIN: make-root-rehome (keep in sync with the other product plugin)
			// Only a scene component can BE a root. Checked before anything is detached: the
			// re-homing below removes the existing roots first, so letting a non-scene component
			// through would dismantle a working hierarchy and then fail to replace it —
			// ValidateSceneRootNodes would re-create a DefaultSceneRoot while the user's former
			// root hangs off a component that cannot parent anything. Refuse instead: the request
			// is not satisfiable, and reporting success on it is the failure mode this whole area
			// was fixed for. (The native-root branch above already guards the same way.)
			if (!CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				// The whole ITEM is refused, not just the makeRoot part, and nothing is attached.
				// Attaching it anyway would break the one remedy this very message suggests:
				// re-sending the item with a SceneComponent class would then collide with the
				// component already sitting under that name at the wrong class, so the caller
				// would first have to discover a mutation it was never told about and delete it.
				// This tool's contract elsewhere is that re-sending an identical batch is safe
				// (see the NodeRefs/Skipped path in DoAddNodes); half-applying the item that
				// failed is exactly where that contract must not break.
				Errors.Add(FString::Printf(
					TEXT("Component '%s' (%s) cannot be made the root: only scene components can be a root. ")
					TEXT("Nothing was created for this item. Re-send it without \"makeRoot\", or with a ")
					TEXT("SceneComponent-derived class."),
					*CompName, *CompClass->GetName()));
				Rejected++;
				continue;
			}

			// No native root (plain Actor): this component becomes the SCS root.
			//
			// The previous root is classified by NODE IDENTITY, not by name. The engine names an
			// SCS component template "<VariableName>_GEN_VARIABLE"
			// (USimpleConstructionScript::ComponentTemplateNameSuffix), so comparing
			// ComponentTemplate->GetFName() against "DefaultSceneRoot" can never be true: the
			// generated placeholder was misclassified as user content and ended up demoted under
			// the new root instead of being removed. GetDefaultSceneRootNode() is the engine's
			// own answer to "is this the placeholder".
			//
			// The two kinds are then handled differently, on purpose:
			//
			//  * The generated placeholder is LEFT IN PLACE. AddNode() always runs
			//    ValidateSceneRootNodes(), which is the supported path that drops the placeholder
			//    and re-parents its children once a real scene root exists — and it keeps the
			//    SCS's own DefaultSceneRootNode member consistent, which RemoveNode() does not.
			//    (Both of that function's arms are keyed on that member, so removing the node
			//    ourselves leaves it stranded.)
			//
			//  * A user-authored root is real content: detached with scene-root validation
			//    suppressed, because the SCS is momentarily rootless between the removals and
			//    AddNode and must not be normalised mid-flight, then re-attached beneath the new
			//    root afterwards.
			USCS_Node* const PlaceholderRoot = BP->SimpleConstructionScript->GetDefaultSceneRootNode();
			TArray<USCS_Node*> OldRoots = BP->SimpleConstructionScript->GetRootNodes();
			TArray<USCS_Node*> DemotedRoots;
			for (USCS_Node* OldRoot : OldRoots)
			{
				if (!OldRoot || OldRoot == NewNode || OldRoot == PlaceholderRoot) continue;

				BP->SimpleConstructionScript->RemoveNode(OldRoot, /*bValidateSceneRootNodes*/ false);
				DemotedRoots.Add(OldRoot);
			}

			BP->SimpleConstructionScript->AddNode(NewNode);

			for (USCS_Node* Demoted : DemotedRoots)
			{
				NewNode->AddChildNode(Demoted);
			}
			goto NodeAttached;
			// <<< NWIRO_PARITY END: make-root-rehome
		}

		// Attach to parent or root
		if (!ParentName.IsEmpty())
		{
			for (USCS_Node* Existing : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Existing && Existing->GetVariableName().ToString().Equals(ParentName, ESearchCase::IgnoreCase))
				{
					Existing->AddChildNode(NewNode);
					goto NodeAttached;
				}
			}
			// Parent not found, attach to root
		}

		// Default: attach to existing SCS root, or native root component
		{
			const TArray<USCS_Node*>& RootNodes = BP->SimpleConstructionScript->GetRootNodes();
			bool bAttached = false;

			// Try SCS root nodes first
			if (RootNodes.Num() > 0 && CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				RootNodes[0]->AddChildNode(NewNode);
				bAttached = true;
			}

			// If no SCS root, attach to the native/inherited default scene root
			if (!bAttached && CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				USCS_Node* DefaultRoot = BP->SimpleConstructionScript->GetDefaultSceneRootNode();
				if (DefaultRoot)
				{
					DefaultRoot->AddChildNode(NewNode);
					bAttached = true;
				}
			}

			// Fallback: set as root node in SCS (attaches to native root automatically)
			if (!bAttached)
			{
				BP->SimpleConstructionScript->AddNode(NewNode);
			}
		}

	NodeAttached:
		// Apply inline properties on the component template
		if (UActorComponent* CompTemplate = NewNode->ComponentTemplate)
		{
			static const TSet<FString> ReservedKeys = { TEXT("name"), TEXT("class"), TEXT("type"), TEXT("parent"), TEXT("makeroot"), TEXT("isroot") };

			// Helper: iterate one JSON object and call ApplyComponentProperty for each key/value pair.
			auto ProcessProps = [&](const TSharedPtr<FJsonObject>& PropsObj)
			{
				for (const auto& Pair : PropsObj->Values)
				{
					const FString Key(*Pair.Key);
					if (ReservedKeys.Contains(NormalizeKey(Key))) continue;

					FString Value;
					if (Pair.Value->Type == EJson::String)       Value = Pair.Value->AsString();
					else if (Pair.Value->Type == EJson::Boolean)  Value = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
					else if (Pair.Value->Type == EJson::Number)   Value = FString::SanitizeFloat(Pair.Value->AsNumber());
					else continue; // skip nested objects/arrays at this level

					ApplyComponentProperty(CompTemplate, Key, Value, CompName, nullptr, &Warnings);
				}
			};

			// Flat top-level properties (e.g. static_mesh="..." at same level as name/type)
			ProcessProps(Obj);

			// Nested "properties" sub-object
			// e.g. {"name":"CubeMesh","type":"StaticMeshComponent","properties":{"static_mesh":"..."}}
			const TSharedPtr<FJsonObject>* NestedProps;
			if (Obj->TryGetObjectField(TEXT("properties"), NestedProps))
				ProcessProps(*NestedProps);
		}

		Added++;
		UE_LOG(LogNwiroBP, Log, TEXT("Added component: %s (%s)"), *CompName, *ClassName);
	}

	NwiroBlueprintBatch::RecordErrors(Errors);
	FString Msg = FString::Printf(TEXT("Added %d component(s)"), Added);
	if (Warnings.Num() > 0)
	{
		Msg += TEXT(". Warnings: ") + FString::Join(Warnings, TEXT("; "));
	}
	if (Errors.Num() > 0)
	{
		Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	}

	// A refused item must fail the envelope even when another item was added.
	// Preserve machine-readable property warnings on either verdict.
	FNwiroBPResult CompRes = Rejected == 0 && (Added > 0 || Errors.Num() == 0)
		? FNwiroBPResult::Ok(Msg) : FNwiroBPResult::Fail(Msg);
	CompRes.Warnings = Warnings;
	return CompRes;
}

// ============================================================
// REMOVE COMPONENTS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoRemoveComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	if (!BP->SimpleConstructionScript) return FNwiroBPResult::Fail(TEXT("No SCS"));

	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString CompName;
		if (Item->Type == EJson::String)
		{
			CompName = Item->AsString();
		}
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid())
		{
			CompName = Item->AsObject()->GetStringField(TEXT("name"));
		}

		if (CompName.IsEmpty()) continue;

		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
			{
				BP->SimpleConstructionScript->RemoveNode(Node);
				Removed++;
				break;
			}
		}
	}

	return FNwiroBPResult::Ok(FString::Printf(TEXT("Removed %d component(s)"), Removed));
}

// ============================================================
// ADD FUNCTIONS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoAddFunctions(UBlueprint* BP, const TArray<FNwiroPreparedFunction>& Items)
{
	FNwiroUnitLedger Ledger;
	struct FOwnedGraph
	{
		int32 Unit;
		UEdGraph* Graph;
		UFunction* PreviousSkeleton;
		UFunction* PreviousGenerated;
	};
	TArray<FOwnedGraph> Owned;
	TArray<int32> Redefined;
	auto OwnFunction = [](UClass* Class, FName Name) -> UFunction*
	{
		return Class ? Class->FindFunctionByName(Name, EIncludeSuperFlag::ExcludeSuper) : nullptr;
	};
	auto Failed = [&](int32 Unit, const FString& Error)
	{
		// Engine graph registration can compile structurally. Cancel is not rollback.
		for (const FOwnedGraph& Mutation : Owned)
			FBlueprintEditorUtils::RemoveGraph(BP, Mutation.Graph, EGraphRemoveFlags::MarkTransient);
		Ledger.Created.Empty();
		TArray<TSharedPtr<FJsonValue>> Residuals;
		for (int32 Index : Redefined) Residuals.Add(MakeShared<FJsonValueString>(Items[Index].Name.ToString()));
		for (const FOwnedGraph& Mutation : Owned)
		{
			const FName Name = Items[Mutation.Unit].Name;
			UFunction* Skeleton = OwnFunction(BP->SkeletonGeneratedClass, Name);
			UFunction* Generated = OwnFunction(BP->GeneratedClass, Name);
			if (NwiroFunctions::FindExactGraph(BP, Name)
				|| (Skeleton && Skeleton != Mutation.PreviousSkeleton)
				|| (Generated && Generated != Mutation.PreviousGenerated))
			{
				auto Entry = FNwiroUnitLedger::MakeUnit(Mutation.Unit, TEXT("add_functions"), Name.ToString(), false);
				Entry->AsObject()->SetStringField(TEXT("reason"), TEXT("graph or reflected signature remains after cleanup"));
				// The failing unit is accounted in errors; residual identity remains explicit below.
				if (Mutation.Unit != Unit) Ledger.Created.Add(Entry);
				Residuals.Add(MakeShared<FJsonValueString>(Name.ToString()));
			}
		}
		Ledger.AddError(Unit, TEXT("add_functions"), TEXT("APPLY_FAILED"), TEXT("function"), Items[Unit].Name.ToString(), Error);
		Ledger.Errors.Last()->AsObject()->SetBoolField(TEXT("mutationAttempted"), true);
		for (const TSharedPtr<FJsonValue>& Residual : Residuals)
			if (Residual->AsString() == Items[Unit].Name.ToString())
				Ledger.Errors.Last()->AsObject()->SetStringField(TEXT("residualFunction"), Residual->AsString());
		const TSet<int32> Accounted = Ledger.AccountedUnits();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
			if (!Accounted.Contains(Index)) NwiroFunctions::AddSkipped(Ledger, Index, Items[Index].Name, TEXT("function batch stopped; owned graph additions removed"));
		FNwiroBPResult Result = FNwiroBPResult::Fail(Error);
		Result.Level2 = Ledger.Build(TEXT("add_functions"), Items.Num());
		Result.Data = MakeShared<FJsonObject>();
		Result.Data->SetBoolField(TEXT("mutationAttempted"), true);
		Result.Data->SetArrayField(TEXT("residualFunctions"), Residuals);
		return Result;
	};
	auto AddPins = [](UK2Node_EditablePinBase* Node, const TArray<FNwiroPreparedFunctionPin>& Pins,
		EEdGraphPinDirection Direction, FString& Error)
	{
		for (const FNwiroPreparedFunctionPin& Param : Pins)
		{
			FText PinError;
			if (Node->FindPin(Param.Name) || !Node->CanCreateUserDefinedPin(Param.PinType, Direction, PinError))
			{ Error = FString::Printf(TEXT("cannot create parameter '%s': %s"), *Param.Name.ToString(), *PinError.ToString()); return false; }
			UEdGraphPin* Pin = Node->CreateUserDefinedPin(Param.Name, Param.PinType, Direction, false);
			if (!Pin || Pin->PinName != Param.Name || Pin->Direction != Direction || Pin->PinType != Param.PinType
				|| Node->UserDefinedPins.IsEmpty() || !Node->UserDefinedPins.Last().IsValid()
				|| Node->UserDefinedPins.Last()->PinName != Param.Name
				|| Node->UserDefinedPins.Last()->DesiredPinDirection != Direction
				|| Node->UserDefinedPins.Last()->PinType != Param.PinType)
			{ Error = FString::Printf(TEXT("Unreal did not retain exact parameter '%s'"), *Param.Name.ToString()); return false; }
			if (Param.bDefaultSpecified)
			{
				Node->UserDefinedPins.Last()->PinDefaultValue = Param.DefaultValue;
				const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
				Schema->SetPinAutogeneratedDefaultValue(Pin, Param.DefaultValue);
				if (!Schema->DoesDefaultValueMatch(*Pin, Param.DefaultValue))
				{ Error = FString::Printf(TEXT("Unreal did not retain parameter default '%s'"), *Param.Name.ToString()); return false; }
			}
		}
		return true;
	};

	bool bModified = false;
	for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
	{
		if (NwiroBlueprintBatch::StopCommit({})) return Failed(Unit, TEXT("Function commit interrupted; no further operations or compile/save were run"));
		const FNwiroPreparedFunction& Item = Items[Unit];
		if (Item.bExistingOverride || Item.bUnchanged)
		{
			NwiroFunctions::AddSkipped(Ledger, Unit, Item.Name, Item.bExistingOverride ? TEXT("existing override graph retained") : TEXT("existing ordinary signature unchanged"));
			continue;
		}
		if (!bModified) { BP->Modify(); bModified = true; }
		if (Item.bRedefine)
		{
			Redefined.Add(Unit);
			FString Error;
			if (!NwiroFunctions::ApplyRedefinition(BP, Item, Error)) return Failed(Unit, Error);
			auto Modified = FNwiroUnitLedger::MakeUnit(Unit, TEXT("add_functions"), Item.Name.ToString(), true);
			TSet<FString> Affected = { BP->GetPathName() };
			for (const auto& Call : Item.CallSites)
				if (UBlueprint* Caller = FBlueprintEditorUtils::FindBlueprintForNode(Call.Get())) Affected.Add(Caller->GetPathName());
			TArray<TSharedPtr<FJsonValue>> AffectedValues;
			for (const FString& Path : Affected) AffectedValues.Add(MakeShared<FJsonValueString>(Path));
			Modified->AsObject()->SetArrayField(TEXT("affectedBlueprints"), AffectedValues);
			Ledger.Modified.Add(Modified);
			continue;
		}
		UFunction* PreviousSkeleton = OwnFunction(BP->SkeletonGeneratedClass, Item.Name);
		UFunction* PreviousGenerated = OwnFunction(BP->GeneratedClass, Item.Name);
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, Item.Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph) return Failed(Unit, FString::Printf(TEXT("failed to create function '%s'"), *Item.Name.ToString()));
		Owned.Add({ Unit, NewGraph, PreviousSkeleton, PreviousGenerated });
		if (Item.bOverride)
			FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, NewGraph, true, Item.OverrideSignature.Get());
		else
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, true, nullptr);

		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node)) Entry = Candidate;
			if (UK2Node_FunctionResult* Candidate = Cast<UK2Node_FunctionResult>(Node)) ResultNode = Candidate;
		}
		if (!Entry) return Failed(Unit, TEXT("Unreal did not create a function entry node"));
		if (Item.bOverride)
		{
			// Inherit parameters through FunctionReference, not duplicate user-defined inputs.
			Entry->UserDefinedPins.Empty();
			Entry->FunctionReference.SetFromField<UFunction>(Item.OverrideSignature.Get(), false);
			Entry->ReconstructNode();
			if (Item.InterfaceIndex != INDEX_NONE)
			{
				if (!BP->ImplementedInterfaces.IsValidIndex(Item.InterfaceIndex))
					return Failed(Unit, TEXT("implemented interface changed after preflight"));
				BP->FunctionGraphs.Remove(NewGraph);
				BP->ImplementedInterfaces[Item.InterfaceIndex].Graphs.Add(NewGraph);
			}
		}
		else
		{
			if (Item.bPure)
			{
				Entry->SetExtraFlags(Entry->GetExtraFlags() | FUNC_BlueprintPure);
				Entry->ReconstructNode();
			}
			FString Error;
			if (!AddPins(Entry, Item.Inputs, EGPD_Output, Error)) return Failed(Unit, Error);
			NwiroFunctions::ApplyReferenceDefaultMetadata(Entry, Item.Inputs);
			if (!Item.Outputs.IsEmpty())
			{
				if (!ResultNode)
				{
					FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
					ResultNode = Creator.CreateNode();
					ResultNode->NodePosX = 400;
					ResultNode->bIsEditable = Entry->bIsEditable;
					ResultNode->FunctionReference = Entry->FunctionReference;
					Creator.Finalize();
				}
				if (!ResultNode || !AddPins(ResultNode, Item.Outputs, EGPD_Input, Error))
					return Failed(Unit, Error.IsEmpty() ? TEXT("Unreal did not create a function result node") : Error);
			}
		}
		Ledger.Created.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("add_functions"), Item.Name.ToString(), true));
		UE_LOG(LogNwiroBP, Log, TEXT("Added function: %s"), *Item.Name.ToString());
	}
	FNwiroBPResult Result = FNwiroBPResult::Ok(FString::Printf(TEXT("Added %d function(s); redefined %d; retained %d unchanged"), Ledger.Created.Num(), Ledger.Modified.Num(), Ledger.Skipped.Num()));
	Result.Level2 = Ledger.Build(TEXT("add_functions"), Items.Num());
	return Result;
}

// ============================================================
// ADD CUSTOM EVENTS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoAddCustomEvents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* G : BP->UbergraphPages)
	{
		if (G && G->GetName() == TEXT("EventGraph"))
		{
			EventGraph = G;
			break;
		}
	}

	if (!EventGraph && BP->UbergraphPages.Num() > 0)
	{
		EventGraph = BP->UbergraphPages[0];
	}

	if (!EventGraph)
	{
		return FNwiroBPResult::Fail(TEXT("No event graph found"));
	}

	int32 Added = 0;
	int32 YOffset = 0;
	// This helper has no local error ledger; retain the active commit-failure gate.
	const TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		if (NwiroBlueprintBatch::StopCommit(Errors)) break;
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString EventName = Obj->GetStringField(TEXT("name"));
		FString Ref = Obj->GetStringField(TEXT("ref"));
		if (EventName.IsEmpty()) continue;

		FGraphNodeCreator<UK2Node_CustomEvent> Creator(*EventGraph);
		UK2Node_CustomEvent* EventNode = Creator.CreateNode();
		EventNode->CustomFunctionName = FName(*EventName);
		EventNode->NodePosX = 0;
		EventNode->NodePosY = YOffset;
		Creator.Finalize();

		// Add parameters
		const TArray<TSharedPtr<FJsonValue>>* Params;
		if (Obj->TryGetArrayField(TEXT("params"), Params))
		{
			for (const TSharedPtr<FJsonValue>& PVal : *Params)
			{
				const TSharedPtr<FJsonObject>& PObj = PVal->AsObject();
				if (!PObj.IsValid()) continue;

				FString ParamName = PObj->GetStringField(TEXT("name"));
				FEdGraphPinType ParamType;

				const TSharedPtr<FJsonObject>* TypeObj;
				if (PObj->TryGetObjectField(TEXT("type"), TypeObj))
				{
					ParamType = ParsePinType(*TypeObj);
				}
				else
				{
					TSharedPtr<FJsonObject> SimpleType = MakeShareable(new FJsonObject());
					SimpleType->SetStringField(TEXT("base"), PObj->GetStringField(TEXT("type")));
					ParamType = ParsePinType(SimpleType);
				}

				TSharedPtr<FUserPinInfo> PinInfo = MakeShareable(new FUserPinInfo());
				PinInfo->PinName = FName(*ParamName);
				PinInfo->PinType = ParamType;
				EventNode->UserDefinedPins.Add(PinInfo);
			}
			EventNode->ReconstructNode();
		}

		// Set replication flags
		FString Replication = Obj->GetStringField(TEXT("replication")).ToLower();
		if (Replication == TEXT("multicast"))
		{
			EventNode->FunctionFlags |= FUNC_NetMulticast;
		}
		else if (Replication == TEXT("server"))
		{
			EventNode->FunctionFlags |= FUNC_NetServer;
		}
		else if (Replication == TEXT("client"))
		{
			EventNode->FunctionFlags |= FUNC_NetClient;
		}

		if (Obj->HasField(TEXT("reliable")) && Obj->GetBoolField(TEXT("reliable")))
		{
			EventNode->FunctionFlags |= FUNC_NetReliable;
		}

		StoreNodeRef(Ref.IsEmpty() ? EventName : Ref, EventNode, EventGraph->GetName());
		YOffset += 200;
		Added++;
		UE_LOG(LogNwiroBP, Log, TEXT("Added custom event: %s"), *EventName);
	}

	return FNwiroBPResult::Ok(FString::Printf(TEXT("Added %d custom event(s)"), Added));
}

// ============================================================
// ADD NODES
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoAddNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items, const NwiroEnhancedInputContract::FPreparedActions& PreparedActions)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph)
	{
		return FNwiroBPResult::Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	// Refresh skeleton if any node is component-bound — see IK comment.
	for (const TSharedPtr<FJsonValue>& PreItem : Items)
	{
		const TSharedPtr<FJsonObject>& PreObj = PreItem.IsValid() ? PreItem->AsObject() : nullptr;
		if (PreObj.IsValid() && PreObj->HasField(TEXT("component")))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			NwiroBlueprintBatch::RecordInternalCompile();
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::SkipSave);
			break;
		}
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	int32 Added = 0;
	int32 NodeX = 300;
	int32 NodeY = 0;
	TArray<FString> Errors;
	TArray<FString> NodeDetails;
	bool bPinResolutionRefused = false;
	auto ResolveAddedPin = [&](UEdGraphNode* Node, const FString& Name,
		EEdGraphPinDirection Dir = EGPD_MAX) -> UEdGraphPin*
	{
		// Refusal stops later automatic lookups, including alternate names.
		if (bPinResolutionRefused) return nullptr;
#if WITH_DEV_AUTOMATION_TESTS
		if (Node && NwiroBlueprintBatch::BeforeGeneratedPinLookup) NwiroBlueprintBatch::BeforeGeneratedPinLookup(Node, Name);
#endif
		FString Ambiguity;
		UEdGraphPin* Pin = FindPin(Node, Name, Dir, &Ambiguity);
		if (!Ambiguity.IsEmpty())
		{
			Errors.AddUnique(Ambiguity);
			bPinResolutionRefused = true;
		}
		if (!Pin && Ambiguity.IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("Required generated pin missing: %s"), *Name));
			bPinResolutionRefused = true;
		}
		return Pin;
	};


	auto SetAddedValue = [&](UEdGraphPin& Pin, const FString& Value)
	{
		FString Canonical, Error;
		if (!NwiroBlueprintBatch::SetDefault(&Pin, Value,
			[](const FString& Name) { return FindClassByName(Name); }, Canonical, Error))
		{ Errors.Add(Error); return false; }
		return true;
	};
	auto SetAddedObject = [&](UEdGraphPin& Pin, UObject* Object)
	{
		return SetAddedValue(Pin, Object ? Object->GetPathName() : FString());
	};
	auto ConnectAddedPins = [&](UEdGraphPin* A, UEdGraphPin* B)
	{
		FString Error;
		const bool bConnected = NwiroBlueprintBatch::Connect(A, B, false, Error);
		if (!bConnected) Errors.Add(Error);
		return bConnected;
	};


	auto UniqueAddedObjectOutput = [&](UEdGraphNode* Node) -> UEdGraphPin*
	{
		TArray<UEdGraphPin*> Matches;
		if (Node) for (UEdGraphPin* Pin : Node->Pins)
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object) Matches.Add(Pin);
		if (Matches.Num() == 1) return Matches[0];
		Errors.Add(TEXT("Generated cast result has missing or ambiguous object output pins"));
		return nullptr;
	};

	int32 Skipped = 0;
	// Items refused for malformed input, before any graph mutation. Distinct from Errors,
	// which also collects soft problems that deliberately keep the op successful.
	int32 Rejected = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		if (NwiroBlueprintBatch::StopCommit(Errors)) break;
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		// Accept all the field names the LLM tends to invent for the node-type
		// field. We've now seen `type`, `node`, `nodeType`, `node_type`, `kind`.
		FString NodeType;
		const TArray<FString> NodeTypeKeys = {
			TEXT("type"), TEXT("node"), TEXT("nodeType"), TEXT("node_type"),
			TEXT("nodetype"), TEXT("kind"), TEXT("nodeClass"), TEXT("class"),
		};
		for (const FString& Key : NodeTypeKeys)
		{
			if (Obj->HasField(Key)) { NodeType = Obj->GetStringField(Key); break; }
		}

		// Support "ref", "id" AND "name" — the LLM frequently uses "name" as
		// the user-facing identifier instead of "ref".
		FString Ref;
		if (Obj->HasField(TEXT("ref")))       Ref = Obj->GetStringField(TEXT("ref"));
		else if (Obj->HasField(TEXT("id")))   Ref = Obj->GetStringField(TEXT("id"));
		else if (Obj->HasField(TEXT("name")) && !NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase) && !NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
		{
			// "name" doubles as the event name for Event/CustomEvent — only
			// reuse it as a ref for non-event nodes.
			Ref = Obj->GetStringField(TEXT("name"));
		}
		int32 PosX = Obj->HasField(TEXT("x")) ? (int32)Obj->GetNumberField(TEXT("x")) : NodeX;
		int32 PosY = Obj->HasField(TEXT("y")) ? (int32)Obj->GetNumberField(TEXT("y")) : NodeY;

		// Dedup: only check session NodeRefs (exact + normalized w/ min 3 chars).
		// Avoids substring title false-positives like ref "PR" matching "Print String".
		// See IK mirror for full rationale (scenario/05-pause-menu repro).
		auto IsRefAlreadyAssigned = [Graph](const FString& Lookup) -> bool
		{
			if (Lookup.IsEmpty()) return false;
			if (const FNwiroNodeRef* Found = NodeRefs.Find(MakeNodeRefKey(Graph, Lookup)))
			{
				for (UEdGraphNode* N : Graph->Nodes) if (N && N->NodeGuid == Found->NodeGuid) return true;
			}
			const FString Norm = NormalizeRefKey(Lookup);
			if (Norm.Len() >= 3)
			{
				const FString OwnerKey = NodeRefOwnerKey(Graph);
				for (const auto& Pair : NodeRefs)
				{
					if (Pair.Value.BlueprintPath == OwnerKey && NormalizeRefKey(Pair.Value.Ref) == Norm)
					{
						for (UEdGraphNode* N : Graph->Nodes) if (N && N->NodeGuid == Pair.Value.NodeGuid) return true;
					}
				}
			}
			return false;
		};
		if (!Ref.IsEmpty() && IsRefAlreadyAssigned(Ref))
		{
			Skipped++;
			continue;
		}
		if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
		{
			FString EventName = Obj->HasField(TEXT("event")) ? Obj->GetStringField(TEXT("event")) : Obj->GetStringField(TEXT("name"));
			if (!EventName.IsEmpty())
			{
				// Strip "event"/"receive" prefixes for the dedup check.
				FString Norm = NormalizeRefKey(EventName);
				if (Norm.StartsWith(TEXT("receive"))) Norm = Norm.RightChop(7);
				if (Norm.StartsWith(TEXT("event"))) Norm = Norm.RightChop(5);

				bool bExists = false;
				for (UEdGraph* SearchGraph : BP->UbergraphPages)
				{
					if (!SearchGraph) continue;
					for (UEdGraphNode* Existing : SearchGraph->Nodes)
					{
						UK2Node_Event* Evt = Cast<UK2Node_Event>(Existing);
						if (!Evt) continue;
						FString FuncNorm = NormalizeRefKey(Evt->GetFunctionName().ToString());
						if (FuncNorm.StartsWith(TEXT("receive"))) FuncNorm = FuncNorm.RightChop(7);
						if (FuncNorm == Norm)
						{
							bExists = true;
							// Still register a session ref pointing to the existing node
							// so subsequent connect_pins can find it via the user's ref.
							if (!Ref.IsEmpty()) StoreNodeRef(Ref, Evt, SearchGraph->GetName());
							break;
						}
					}
					if (bExists) break;
				}
				if (bExists)
				{
					Skipped++;
					continue;
				}
			}
		}

		UEdGraphNode* CreatedNode = nullptr;
		FString NodeDetail;

		// ---- CallFunction ----
		// Auto-detect AddMappingContext even when requested as CallFunction
		if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
		{
			FString TempFunc = Obj->GetStringField(TEXT("function"));
			if (TempFunc.Contains(TEXT("AddMappingContext"), ESearchCase::IgnoreCase) ||
				TempFunc.Contains(TEXT("Add Mapping Context"), ESearchCase::IgnoreCase))
			{
				NodeType = TEXT("AddMappingContext");
			}
			else if (TempFunc.Contains(TEXT("SpawnActor"), ESearchCase::IgnoreCase) ||
				TempFunc.Contains(TEXT("Spawn Actor"), ESearchCase::IgnoreCase))
			{
				NodeType = TEXT("SpawnActor");
			}
		}

		if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
		{
			FString FunctionName = Obj->GetStringField(TEXT("function"));
			FString TargetClass = Obj->GetStringField(TEXT("target"));

			// Normalize: remove spaces so "Print String" -> "PrintString"
			FString FunctionNameNoSpaces = FunctionName.Replace(TEXT(" "), TEXT(""));

			UFunction* Func = nullptr;
			bool bBruteForce = false;
			bool bAmbiguousAllowed = false;

			// Helper lambda: search function in class + entire parent chain
			// Tries multiple naming conventions:
			//   - exact name, no-spaces, K2_ prefix
			//   - Float <-> Double synonyms (UE 5.5+ math is double)
			//   - bare math operator names ("Multiply" -> "Multiply_DoubleDouble", etc.)
			auto FindFuncInHierarchy = [&FunctionName, &FunctionNameNoSpaces](UClass* StartClass) -> UFunction*
			{
				TArray<FString> Tries;
				Tries.Add(FunctionName);
				Tries.Add(FunctionNameNoSpaces);
				Tries.Add(TEXT("K2_") + FunctionName);
				Tries.Add(TEXT("K2_") + FunctionNameNoSpaces);

				// UE 5.6 renamed LineTraceSingleByChannel → LineTraceSingle
				if (FunctionNameNoSpaces.Equals(TEXT("LineTraceSingleByChannel"), ESearchCase::IgnoreCase) ||
					FunctionNameNoSpaces.Equals(TEXT("LineTraceByChannel"), ESearchCase::IgnoreCase))
					Tries.Add(TEXT("LineTraceSingle"));

				// GetWorldLocation/GetWorldRotation: display name differs by target class
				if (FunctionNameNoSpaces.Equals(TEXT("GetWorldLocation"), ESearchCase::IgnoreCase))
				{
					if (StartClass->IsChildOf(USceneComponent::StaticClass()))
						Tries.Add(TEXT("K2_GetComponentLocation"));
					else if (StartClass->IsChildOf(AActor::StaticClass()))
						Tries.Add(TEXT("K2_GetActorLocation"));
				}
				if (FunctionNameNoSpaces.Equals(TEXT("GetWorldRotation"), ESearchCase::IgnoreCase))
				{
					if (StartClass->IsChildOf(USceneComponent::StaticClass()))
						Tries.Add(TEXT("K2_GetComponentRotation"));
					else if (StartClass->IsChildOf(AActor::StaticClass()))
						Tries.Add(TEXT("K2_GetActorRotation"));
				}

				// Float <-> Double synonyms
				if (FunctionNameNoSpaces.Contains(TEXT("Float")))
					Tries.Add(FunctionNameNoSpaces.Replace(TEXT("Float"), TEXT("Double")));
				if (FunctionNameNoSpaces.Contains(TEXT("Double")))
					Tries.Add(FunctionNameNoSpaces.Replace(TEXT("Double"), TEXT("Float")));

				// Bare math operator names — the LLM often writes just "Multiply"
				// or "Add" without specifying types. Try the full KismetMathLibrary
				// names for the most common overloads.
				static const TArray<FString> MathOps = {
					TEXT("Add"), TEXT("Subtract"), TEXT("Multiply"), TEXT("Divide"),
					TEXT("Min"), TEXT("Max"), TEXT("Equal"), TEXT("NotEqual"),
					TEXT("Less"), TEXT("LessEqual"), TEXT("Greater"), TEXT("GreaterEqual"),
				};
				for (const FString& Op : MathOps)
				{
					if (FunctionNameNoSpaces.Equals(Op, ESearchCase::IgnoreCase))
					{
						Tries.Add(Op + TEXT("_DoubleDouble"));
						Tries.Add(Op + TEXT("_FloatFloat"));
						Tries.Add(Op + TEXT("_IntInt"));
						Tries.Add(Op + TEXT("_VectorVector"));
						break;
					}
				}

				// Display-name conversion: the LLM frequently feeds us strings
				// like "To Vector (Float)", "To Float", "To Int" which are the
				// editor display names. The real C++ functions live under
				// `Conv_<Source>To<Dest>` in KismetMathLibrary. Strip the
				// parenthesized type hint and try every reasonable Conv_ form.
				{
					// "To Vector (Float)" → "ToVector"
					FString StrippedParens = FunctionName;
					int32 LParen;
					if (StrippedParens.FindChar('(', LParen)) StrippedParens = StrippedParens.Left(LParen);
					StrippedParens = StrippedParens.Replace(TEXT(" "), TEXT("")).TrimStartAndEnd();

					if (StrippedParens.StartsWith(TEXT("To"), ESearchCase::IgnoreCase) && StrippedParens.Len() > 2)
					{
						const FString DestType = StrippedParens.RightChop(2); // e.g. "Vector"
						// Try every common source type — first match wins.
						const TArray<FString> SrcTypes = { TEXT("Double"), TEXT("Float"), TEXT("Int"), TEXT("Int64"), TEXT("Bool"), TEXT("String"), TEXT("Name"), TEXT("Vector"), TEXT("Rotator") };
						for (const FString& Src : SrcTypes)
						{
							Tries.Add(FString::Printf(TEXT("Conv_%sTo%s"), *Src, *DestType));
						}
						// Also Make<Type>: MakeVector / MakeRotator etc.
						Tries.Add(TEXT("Make") + DestType);
					}
				}

				for (UClass* C = StartClass; C; C = C->GetSuperClass())
				{
					for (const FString& Try : Tries)
					{
						UFunction* F = C->FindFunctionByName(FName(*Try));
						if (F) return F;
					}
				}
				return nullptr;
			};

			// Search for the function in the target class hierarchy
			if (!TargetClass.IsEmpty())
			{
				UClass* TargetUClass = nullptr;

				// Check SCS component instance names first (e.g. "FirstPersonCamera" → UCameraComponent)
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (SCSNode && SCSNode->ComponentTemplate &&
							SCSNode->GetVariableName().ToString().Equals(TargetClass, ESearchCase::IgnoreCase))
						{
							TargetUClass = SCSNode->ComponentTemplate->GetClass();
							break;
						}
					}
				}

				if (!TargetUClass)
					TargetUClass = FindClassByName(TargetClass);

				if (TargetUClass)
					Func = FindFuncInHierarchy(TargetUClass);
			}

			// If not found, search in common libraries + BP parent chain
			if (!Func)
			{
				TArray<UClass*> SearchClasses = {
					UKismetSystemLibrary::StaticClass(),
					UKismetMathLibrary::StaticClass(),
					UKismetStringLibrary::StaticClass(),
					UGameplayStatics::StaticClass(),
					UEnhancedInputLocalPlayerSubsystem::StaticClass(),
					// Common UMG components — agents call SetPercent/SetText/IsChecked/
					// SetValue/etc without knowing or passing the owning widget class.
					// Sweeping these here makes the "natural minimal" call shape work.
					UProgressBar::StaticClass(),
					UTextBlock::StaticClass(),
					UButton::StaticClass(),
					USlider::StaticClass(),
					UCheckBox::StaticClass(),
					USpinBox::StaticClass(),
					UEditableText::StaticClass(),
					UEditableTextBox::StaticClass(),
					UMultiLineEditableText::StaticClass(),
					UMultiLineEditableTextBox::StaticClass(),
					UImage::StaticClass(),
					UBorder::StaticClass(),
					UWidgetSwitcher::StaticClass(),
					UComboBoxString::StaticClass(),
					UCanvasPanel::StaticClass(),
					UVerticalBox::StaticClass(),
					UHorizontalBox::StaticClass(),
					UUserWidget::StaticClass(),
				};

				// Also search blueprint's generated class and parent hierarchy
				if (BP->GeneratedClass)
				{
					for (UClass* C = BP->GeneratedClass; C; C = C->GetSuperClass())
					{
						SearchClasses.Add(C);
					}
				}

				for (UClass* Lib : SearchClasses)
				{
					Func = FindFuncInHierarchy(Lib);
					if (Func) break;
				}
			}

			// Last resort: brute-force search all loaded UClasses
			if (!Func)
			{
				UFunction* BFFunc = nullptr;
				UClass* BFClass = nullptr;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					UFunction* F = It->FindFunctionByName(FName(*FunctionName), EIncludeSuperFlag::ExcludeSuper);
					if (F && F->HasAnyFunctionFlags(FUNC_BlueprintCallable))
					{
						BFFunc = F;
						BFClass = *It;
						UE_LOG(LogNwiroBP, Log, TEXT("Found function '%s' via brute-force in class '%s'"), *FunctionName, *It->GetName());
						break;
					}
				}

				if (BFFunc && BFClass)
				{
					// Trust the result only if the owning class is a blueprint function library
					// or is in the BP's own parent chain — anything else is ambiguous.
					bool bTrusted = BFClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass());
					if (!bTrusted && BP->GeneratedClass)
					{
						for (UClass* C = BP->GeneratedClass; C; C = C->GetSuperClass())
						{
							if (C == BFClass) { bTrusted = true; break; }
						}
					}

					if (bTrusted)
					{
						Func = BFFunc;
						bBruteForce = true;
					}
					else
					{
						bool bAllowAmbiguous = false;
						Obj->TryGetBoolField(TEXT("allow_ambiguous_bruteforce"), bAllowAmbiguous);

						if (bAllowAmbiguous)
						{
							Func = BFFunc;
							bBruteForce = true;
							bAmbiguousAllowed = true;
						}
						else
						{
							Errors.Add(FString::Printf(
								TEXT("Function '%s' was found via brute-force in class '%s', but that class is not a trusted library or this blueprint's parent chain. ")
								TEXT("If this function belongs to a specific component or object, re-send the node with target:'ComponentInstanceName' or target:'ClassName' so the correct class hierarchy is searched. ")
								TEXT("Only use \"allow_ambiguous_bruteforce\": true if this untrusted owner class is intentional."),
								*FunctionName, *BFClass->GetName()));
							continue;
						}
					}
				}
			}

			if (Func)
			{
				// Array library functions (Array_Length, Array_Add, Array_Get, ...) carry an
				// "ArrayParm" meta and MUST use UK2Node_CallArrayFunction, not the base CallFunction
				// node: only the array variant propagates the connected array's element type into its
				// wildcard pins. With the base node the wildcard stayed unresolved and the BP failed
				// to compile.
				if (Func->HasMetaData(TEXT("ArrayParm")))
				{
					FGraphNodeCreator<UK2Node_CallArrayFunction> Creator(*Graph);
					UK2Node_CallArrayFunction* ArrNode = Creator.CreateNode();
					ArrNode->SetFromFunction(Func);
					ArrNode->NodePosX = PosX;
					ArrNode->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = ArrNode;
				}
				else
				{
					FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
					UK2Node_CallFunction* FuncNode = Creator.CreateNode();
					FuncNode->SetFromFunction(Func);
					FuncNode->NodePosX = PosX;
					FuncNode->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = FuncNode;
				}
				const FString ResolvedName = Func->GetName();
				const FString OwnerName = Func->GetOwnerClass() ? Func->GetOwnerClass()->GetName() : TEXT("unknown");
				if (bAmbiguousAllowed)
					NodeDetail = FString::Printf(TEXT("%s [ambiguous-allowed] found in %s via brute-force, requested:'%s'"), *ResolvedName, *OwnerName, *FunctionName);
				else if (bBruteForce)
					NodeDetail = FString::Printf(TEXT("%s [fallback] found in %s via brute-force, requested:'%s'"), *ResolvedName, *OwnerName, *FunctionName);
				else if (!ResolvedName.Equals(FunctionName, ESearchCase::IgnoreCase))
					NodeDetail = FString::Printf(TEXT("%s [fallback] resolved from requested:'%s' in %s"), *ResolvedName, *FunctionName, *OwnerName);
				else
					NodeDetail = FString::Printf(TEXT("%s [resolved] in %s"), *ResolvedName, *OwnerName);
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
			}
		}
		// ---- VariableGet ----
		else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("GetVariable"), ESearchCase::IgnoreCase))
		{
			FString VarName = Obj->GetStringField(TEXT("variable"));
			// >>> NWIRO_PARITY BEGIN: variable-get-required (keep in sync with the other product plugin)
			// Reject before allocating: an empty name binds the node to NAME_None, which
			// looks like a created node but resolves to nothing.
			if (VarName.IsEmpty())
			{
				Errors.Add(FString::Printf(
					TEXT("Node '%s' (VariableGet) requires a \"variable\" field naming the Blueprint variable to read"),
					*Ref));
				Rejected++;
				continue;
			}
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			// <<< NWIRO_PARITY END: variable-get-required
			UK2Node_VariableGet* GetNode = Creator.CreateNode();
			GetNode->VariableReference.SetSelfMember(FName(*VarName));
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = GetNode;
		}
		// ---- VariableSet ----
		else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetVariable"), ESearchCase::IgnoreCase))
		{
			FString VarName = Obj->GetStringField(TEXT("variable"));
			// >>> NWIRO_PARITY BEGIN: variable-set-required (keep in sync with the other product plugin)
			// See the VariableGet branch: reject before allocating.
			if (VarName.IsEmpty())
			{
				Errors.Add(FString::Printf(
					TEXT("Node '%s' (VariableSet) requires a \"variable\" field naming the Blueprint variable to write"),
					*Ref));
				Rejected++;
				continue;
			}
			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
			// <<< NWIRO_PARITY END: variable-set-required
			UK2Node_VariableSet* SetNode = Creator.CreateNode();
			SetNode->VariableReference.SetSelfMember(FName(*VarName));
			SetNode->NodePosX = PosX;
			SetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SetNode;
		}
		// ---- Branch (If) ----
		else if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("If"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
			UK2Node_IfThenElse* BranchNode = Creator.CreateNode();
			BranchNode->NodePosX = PosX;
			BranchNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = BranchNode;
		}
		// ---- InputAction ----
		else if (NodeType.Equals(TEXT("InputAction"), ESearchCase::IgnoreCase))
		{
			FString ActionName = Obj->GetStringField(TEXT("action"));
			FGraphNodeCreator<UK2Node_InputAction> Creator(*Graph);
			UK2Node_InputAction* ActionNode = Creator.CreateNode();
			ActionNode->InputActionName = FName(*ActionName);
			ActionNode->NodePosX = PosX;
			ActionNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = ActionNode;
		}
		// ---- InputKey ----
		else if (NodeType.Equals(TEXT("InputKey"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("KeyEvent"), ESearchCase::IgnoreCase))
		{
			FString KeyName = Obj->GetStringField(TEXT("key"));
			FKey Key(*KeyName);

			FGraphNodeCreator<UK2Node_InputKey> Creator(*Graph);
			UK2Node_InputKey* KeyNode = Creator.CreateNode();
			KeyNode->InputKey = Key;
			KeyNode->NodePosX = PosX;
			KeyNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = KeyNode;
		}
		// ---- CustomEvent ----
		else if (NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
		{
			FString EventName = Obj->GetStringField(TEXT("name"));
			FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
			UK2Node_CustomEvent* EventNode = Creator.CreateNode();
			EventNode->CustomFunctionName = FName(*EventName);
			EventNode->NodePosX = PosX;
			EventNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = EventNode;
		}
		// ---- Event (BeginPlay, Tick, ActorBeginOverlap, etc.) ----
		else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
		{
			FString EventName = Obj->GetStringField(TEXT("event"));
			if (EventName.IsEmpty()) EventName = Obj->GetStringField(TEXT("name"));

			// Component-bound event path — UK2Node_ComponentBoundEvent for events
			// on a named component. Falls through to the regular handler if no
			// component arg is supplied.
			FString CompName;
			Obj->TryGetStringField(TEXT("component"), CompName);

			// Widget BPs: components live in WidgetTree, not SCS. Try widget
			// lookup first; fall back to SCS for regular BPs.
			UClass* WidgetCompClass = nullptr;
			FObjectProperty* WidgetCompProp = nullptr;
			if (!CompName.IsEmpty())
			{
				if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP))
				{
					if (WBP->WidgetTree)
					{
						WBP->WidgetTree->ForEachWidget([&](UWidget* W)
						{
							if (W && W->GetName().Equals(CompName, ESearchCase::IgnoreCase) && !WidgetCompClass)
								WidgetCompClass = W->GetClass();
						});
						if (WidgetCompClass)
						{
							WidgetCompProp = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, FName(*CompName));
							// Force-compile to populate SkeletonGeneratedClass
							// with the just-added widget's FObjectProperty so the
							// OnClicked event can bind. See IK rationale.
							if (!WidgetCompProp)
							{
								NwiroBlueprintBatch::RecordInternalCompile();
								FKismetEditorUtilities::CompileBlueprint(BP);
								WidgetCompProp = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, FName(*CompName));
							}
						}
					}
				}
			}

			if (!CompName.IsEmpty() && WidgetCompClass && WidgetCompProp)
			{
				FMulticastDelegateProperty* DelegateProp = nullptr;
				for (TFieldIterator<FMulticastDelegateProperty> It(WidgetCompClass); It; ++It)
				{
					const FString PN = It->GetName();
					if (PN.Equals(EventName, ESearchCase::IgnoreCase)
						|| PN.Equals(FString(TEXT("On")) + EventName, ESearchCase::IgnoreCase))
					{ DelegateProp = *It; break; }
				}
				if (DelegateProp)
				{
					FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
					UK2Node_ComponentBoundEvent* CBE = Creator.CreateNode();
					CBE->InitializeComponentBoundEventParams(WidgetCompProp, DelegateProp);
					CBE->NodePosX = PosX;
					CBE->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = CBE;
					NodeDetail = FString::Printf(TEXT("WidgetBoundEvent %s on %s"),
						*DelegateProp->GetName(), *CompName);
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Widget '%s' (class %s) has no multicast delegate matching '%s'"),
						*CompName, *WidgetCompClass->GetName(), *EventName));
				}
			}
			else if (!CompName.IsEmpty() && BP->SimpleConstructionScript)
			{
				USCS_Node* TargetNode = nullptr;
				for (USCS_Node* SN : BP->SimpleConstructionScript->GetAllNodes())
				{
					if (SN && SN->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
					{
						TargetNode = SN;
						break;
					}
				}
				if (TargetNode && TargetNode->ComponentTemplate)
				{
					UClass* CompClass = TargetNode->ComponentTemplate->GetClass();
					FMulticastDelegateProperty* DelegateProp = nullptr;
					for (TFieldIterator<FMulticastDelegateProperty> It(CompClass); It; ++It)
					{
						const FString PN = It->GetName();
						if (PN.Equals(EventName, ESearchCase::IgnoreCase)
							|| PN.Equals(FString(TEXT("On")) + EventName, ESearchCase::IgnoreCase))
						{ DelegateProp = *It; break; }
					}
					if (DelegateProp)
					{
						FObjectProperty* CompProp = FindFProperty<FObjectProperty>(
							BP->SkeletonGeneratedClass, TargetNode->GetVariableName());
						if (CompProp)
						{
							FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
							UK2Node_ComponentBoundEvent* CBE = Creator.CreateNode();
							CBE->InitializeComponentBoundEventParams(CompProp, DelegateProp);
							CBE->NodePosX = PosX;
							CBE->NodePosY = PosY;
							Creator.Finalize();
							CreatedNode = CBE;
							NodeDetail = FString::Printf(TEXT("ComponentBoundEvent %s on %s"),
								*DelegateProp->GetName(), *CompName);
						}
						else
						{
							Errors.Add(FString::Printf(TEXT("Component property '%s' not yet on skeleton class — compile the BP first"), *CompName));
						}
					}
					else
					{
						Errors.Add(FString::Printf(TEXT("Component '%s' (class %s) has no multicast delegate matching '%s'"),
							*CompName, *CompClass->GetName(), *EventName));
					}
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Component '%s' not found on this blueprint"), *CompName));
				}
			}
			else
			{
			// Map user-friendly names to UE internal function names
			static TMap<FString, FString> EventNameMap = {
				{ TEXT("beginplay"), TEXT("ReceiveBeginPlay") },
				{ TEXT("tick"), TEXT("ReceiveTick") },
				{ TEXT("endplay"), TEXT("ReceiveEndPlay") },
				{ TEXT("actorbeginoverlap"), TEXT("ReceiveActorBeginOverlap") },
				{ TEXT("actorendoverlap"), TEXT("ReceiveActorEndOverlap") },
				{ TEXT("hit"), TEXT("ReceiveHit") },
				{ TEXT("anyDamage"), TEXT("ReceiveAnyDamage") },
				{ TEXT("pointdamage"), TEXT("ReceivePointDamage") },
				{ TEXT("radialdamage"), TEXT("ReceiveRadialDamage") },
				{ TEXT("destroyed"), TEXT("ReceiveDestroyed") },
				{ TEXT("begincursorover"), TEXT("ReceiveBeginCursorOver") },
				{ TEXT("endcursorover"), TEXT("ReceiveEndCursorOver") },
				{ TEXT("clicked"), TEXT("ReceiveActorOnClicked") },
				{ TEXT("released"), TEXT("ReceiveActorOnReleased") },
			};

			// Aggressive normalization: strip "event" / "receive" prefixes and
			// non-alphanumeric chars so all of "Tick", "EventTick", "Event Tick",
			// "event_tick", "ReceiveTick" map to the same key.
			FString NormKey = EventName.ToLower();
			NormKey.ReplaceInline(TEXT(" "), TEXT(""));
			NormKey.ReplaceInline(TEXT("_"), TEXT(""));
			if (NormKey.StartsWith(TEXT("receive"))) NormKey = NormKey.RightChop(7);
			if (NormKey.StartsWith(TEXT("event"))) NormKey = NormKey.RightChop(5);

			FString InternalName = EventName;
			if (const FString* Mapped = EventNameMap.Find(NormKey))
			{
				InternalName = *Mapped;
			}
			else if (!EventName.StartsWith(TEXT("Receive"), ESearchCase::IgnoreCase))
			{
				// Also try with Receive prefix on the cleaned name
				InternalName = TEXT("Receive") + EventName;
			}

			// Search existing event nodes in ALL graph pages
			bool bFoundExisting = false;
			for (UEdGraph* SearchGraph : BP->UbergraphPages)
			{
				if (!SearchGraph) continue;
				for (UEdGraphNode* ExistingNode : SearchGraph->Nodes)
				{
					UK2Node_Event* EvtNode = Cast<UK2Node_Event>(ExistingNode);
					if (EvtNode)
					{
						FString EvtFuncName = EvtNode->GetFunctionName().ToString();
						if (EvtFuncName.Equals(InternalName, ESearchCase::IgnoreCase) ||
							EvtFuncName.Contains(EventName, ESearchCase::IgnoreCase))
						{
							CreatedNode = EvtNode;
							bFoundExisting = true;
							break;
						}
					}
				}
				if (bFoundExisting) break;
			}

			if (!bFoundExisting)
			{
				// Find the event function in the parent class chain
				UFunction* EventFunc = nullptr;
				UClass* SearchClass = BP->ParentClass.Get() ? BP->ParentClass.Get() : (BP->GeneratedClass ? BP->GeneratedClass->GetSuperClass() : nullptr);

				if (SearchClass)
				{
					for (UClass* C = SearchClass; C; C = C->GetSuperClass())
					{
						EventFunc = C->FindFunctionByName(FName(*InternalName));
						if (EventFunc) break;

						// Also try original name
						EventFunc = C->FindFunctionByName(FName(*EventName));
						if (EventFunc) break;
					}
				}

				if (EventFunc)
				{
					FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
					UK2Node_Event* EvtNode = Creator.CreateNode();
					EvtNode->EventReference.SetFromField<UFunction>(EventFunc, false);
					EvtNode->bOverrideFunction = true;
					EvtNode->NodePosX = PosX;
					EvtNode->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = EvtNode;
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Built-in event not found: %s (tried %s). Use 'CustomEvent' type for custom events."), *EventName, *InternalName));
				}
			}
			}  // close: else (no component arg)
		}
		// ---- Self ----
		else if (NodeType.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
		{
			UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
			if (SelfNode)
			{
				Graph->AddNode(SelfNode, false, false);
				SelfNode->CreateNewGuid();
				SelfNode->NodePosX = PosX;
				SelfNode->NodePosY = PosY;
				SelfNode->AllocateDefaultPins();
				CreatedNode = SelfNode;
			}
		}
		// ---- SpawnActor ----
		else if (NodeType.Equals(TEXT("SpawnActor"), ESearchCase::IgnoreCase))
		{
			UK2Node_SpawnActorFromClass* SpawnNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
			if (SpawnNode)
			{
				Graph->AddNode(SpawnNode, false, false);
				SpawnNode->CreateNewGuid();
				SpawnNode->NodePosX = PosX;
				SpawnNode->NodePosY = PosY;
				SpawnNode->AllocateDefaultPins();

				FString ActorClassName = GetFieldNormalized(Obj, TEXT("actorclass")); // covers actor_class / actorClass / ActorClass
				if (ActorClassName.IsEmpty()) ActorClassName = GetFieldNormalized(Obj, TEXT("class"));
				if (!ActorClassName.IsEmpty())
				{
					UClass* ActorClass = FindClassByName(ActorClassName);
					if (ActorClass)
					{
						UEdGraphPin* ClassPin = ResolveAddedPin(SpawnNode, TEXT("Class"), EGPD_Input);
						if (ClassPin)
						{
							if (!SetAddedObject(*ClassPin, ActorClass)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
							SpawnNode->ReconstructNode();
						}
						NodeDetail = FString::Printf(TEXT("SpawnActor [resolved] class:'%s'"), *ActorClass->GetName());
					}
					else
					{
						Errors.Add(FString::Printf(TEXT("SpawnActor [not_resolved] class:'%s' not found — node created without class; fix actor_class field or use set_pin_defaults to set Class pin"), *ActorClassName));
					}
				}
				else
				{
					NodeDetail = TEXT("SpawnActor [resolved] no class requested");
				}

				CreatedNode = SpawnNode;
			}
		}
		// ---- Timeline (with float tracks + keyframes) ----
		else if (NodeType.Equals(TEXT("Timeline"), ESearchCase::IgnoreCase))
		{
			FString TimelineName = Obj->GetStringField(TEXT("name"));
			if (TimelineName.IsEmpty()) TimelineName = TEXT("MyTimeline");

			bool bLoop = Obj->HasField(TEXT("loop")) ? Obj->GetBoolField(TEXT("loop")) : false;
			bool bAutoPlay = Obj->HasField(TEXT("autoPlay")) ? Obj->GetBoolField(TEXT("autoPlay")) : false;
			float Length = Obj->HasField(TEXT("length")) ? (float)Obj->GetNumberField(TEXT("length")) : 1.0f;

			// Create timeline template FIRST via BlueprintEditorUtils
			UTimelineTemplate* TLTemplate = FBlueprintEditorUtils::AddNewTimeline(BP, FName(*TimelineName));
			UE_LOG(LogNwiroBP, Log, TEXT("Timeline '%s': Template %s"), *TimelineName, TLTemplate ? TEXT("CREATED") : TEXT("FAILED"));

			// Now create the node
			FGraphNodeCreator<UK2Node_Timeline> Creator(*Graph);
			UK2Node_Timeline* TLNode = Creator.CreateNode();
			TLNode->TimelineName = FName(*TimelineName);
			TLNode->bAutoPlay = bAutoPlay;
			TLNode->bLoop = bLoop;
			TLNode->NodePosX = PosX;
			TLNode->NodePosY = PosY;
			Creator.Finalize();

			if (TLTemplate)
			{
				TLTemplate->TimelineLength = Length;
				TLTemplate->bLoop = bLoop;
				TLTemplate->bAutoPlay = bAutoPlay;

				// Add float tracks
				const TArray<TSharedPtr<FJsonValue>>* Tracks;
				if (Obj->TryGetArrayField(TEXT("floatTracks"), Tracks))
				{
					for (const TSharedPtr<FJsonValue>& TrackVal : *Tracks)
					{
						const TSharedPtr<FJsonObject>& TrackObj = TrackVal->AsObject();
						if (!TrackObj.IsValid()) continue;

						FString TrackName = TrackObj->GetStringField(TEXT("name"));
						if (TrackName.IsEmpty()) continue;

						// Create inline curve
						UCurveFloat* Curve = NewObject<UCurveFloat>(TLTemplate, FName(*(TimelineName + TEXT("_") + TrackName)));

						// Add keyframes
						const TArray<TSharedPtr<FJsonValue>>* Keys;
						if (TrackObj->TryGetArrayField(TEXT("keys"), Keys))
						{
							for (const TSharedPtr<FJsonValue>& KeyVal : *Keys)
							{
								const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
								if (!KeyObj.IsValid()) continue;

								float Time = (float)KeyObj->GetNumberField(TEXT("time"));
								float Value = (float)KeyObj->GetNumberField(TEXT("value"));

								FString InterpStr = KeyObj->GetStringField(TEXT("interp")).ToLower();
								ERichCurveInterpMode InterpMode = RCIM_Linear;
								if (InterpStr == TEXT("cubic") || InterpStr == TEXT("auto"))
									InterpMode = RCIM_Cubic;
								else if (InterpStr == TEXT("constant") || InterpStr == TEXT("step"))
									InterpMode = RCIM_Constant;

								FKeyHandle KeyHandle = Curve->FloatCurve.AddKey(Time, Value);
								Curve->FloatCurve.SetKeyInterpMode(KeyHandle, InterpMode);
							}
						}

						// Add the track to the template
						FTTFloatTrack NewTrack;
						NewTrack.SetTrackName(FName(*TrackName), TLTemplate);
						NewTrack.CurveFloat = Curve;
						TLTemplate->FloatTracks.Add(NewTrack);

						UE_LOG(LogNwiroBP, Log, TEXT("Timeline '%s': Added float track '%s' with %d keys"),
							*TimelineName, *TrackName, Keys ? Keys->Num() : 0);
					}
				}

				// Reconstruct node to show new track pins
				TLNode->ReconstructNode();
			}

			CreatedNode = TLNode;
		}
		// ---- Macro (ForEachLoop, Sequence, etc.) ----
		else if (NodeType.Equals(TEXT("Macro"), ESearchCase::IgnoreCase))
		{
			FString MacroName = Obj->GetStringField(TEXT("macro"));
			// Search for macro graph in engine blueprints
			UEdGraph* MacroGraph = nullptr;

			// Check standard macro library
			static UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr,
				TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));

			if (MacroLib)
			{
				for (UEdGraph* MG : MacroLib->MacroGraphs)
				{
					if (MG && MG->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
					{
						MacroGraph = MG;
						break;
					}
				}
			}

			if (MacroGraph)
			{
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->NodePosX = PosX;
				MacroNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = MacroNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Macro not found: %s"), *MacroName));
			}
		}
		// ---- Cast ----
		else if (NodeType.Equals(TEXT("Cast"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
		{
			// "DynamicCast" is the spelling LLMs reach for first; alias it to the same handler.
			FString TargetClassName = Obj->GetStringField(TEXT("class"));
			UClass* CastClass = FindClassByName(TargetClassName);
			if (!CastClass) CastClass = FindClassByName(TargetClassName + TEXT("_C")); // user-BP generated class
			if (CastClass)
			{
				FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
				UK2Node_DynamicCast* CastNode = Creator.CreateNode();
				CastNode->TargetType = CastClass;
				CastNode->NodePosX = PosX;
				CastNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = CastNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Cast target class not found: %s"), *TargetClassName));
			}
		}
		// ---- CastTo<ClassName> alias for a DynamicCast node (e.g. CastToBP_Door). LLMs reach
		// for this spelling; without a handler it fell through to the unknown-node scan, got
		// silently dropped, and every connect_pins referencing it cascaded into Node-not-found.
		else if (NodeType.StartsWith(TEXT("CastTo"), ESearchCase::IgnoreCase) && NodeType.Len() > 6)
		{
			const FString CastToClass = NodeType.Mid(6);
			UClass* CastClass = FindClassByName(CastToClass);
			if (!CastClass) CastClass = FindClassByName(CastToClass + TEXT("_C")); // user-BP generated class
			if (CastClass)
			{
				FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
				UK2Node_DynamicCast* CastNode = Creator.CreateNode();
				CastNode->TargetType = CastClass;
				CastNode->NodePosX = PosX;
				CastNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = CastNode;
			}
			else
			{
				// Class not found/loaded: fall through to unknown-node error. Status stays Ok.
				Errors.Add(FString::Printf(TEXT("CastTo target class not found: %s"), *CastToClass));
			}
		}
		// ---- AddMappingContext (convenience: creates GetController→Cast→GetSubsystem→AddMappingContext chain) ----
		else if (NodeType.Equals(TEXT("AddMappingContext"), ESearchCase::IgnoreCase))
		{
			FString IMCPath = Obj->GetStringField(TEXT("context"));
			if (IMCPath.IsEmpty()) IMCPath = Obj->GetStringField(TEXT("mappingContext"));
			int32 Priority = Obj->HasField(TEXT("priority")) ? (int32)Obj->GetNumberField(TEXT("priority")) : 0;

			// Create 4 nodes: GetController → CastToPlayerController → GetSubsystem → AddMappingContext

			// 1. GetController
			UFunction* GetControllerFunc = ACharacter::StaticClass()->FindFunctionByName(FName(TEXT("GetController")));
			if (!GetControllerFunc) GetControllerFunc = APawn::StaticClass()->FindFunctionByName(FName(TEXT("GetController")));

			UK2Node_CallFunction* GetCtrlNode = nullptr;
			if (GetControllerFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> C1(*Graph);
				GetCtrlNode = C1.CreateNode();
				GetCtrlNode->SetFromFunction(GetControllerFunc);
				GetCtrlNode->NodePosX = PosX;
				GetCtrlNode->NodePosY = PosY;
				C1.Finalize();
			}

			if (!GetCtrlNode) Errors.Add(TEXT("AddMappingContext: required controller function not found"));

			// 2. CastToPlayerController
			UK2Node_DynamicCast* CastNode = nullptr;
			{
				UClass* PCClass = APlayerController::StaticClass();
				FGraphNodeCreator<UK2Node_DynamicCast> C2(*Graph);
				CastNode = C2.CreateNode();
				CastNode->TargetType = PCClass;
				CastNode->NodePosX = PosX + 250;
				CastNode->NodePosY = PosY;
				C2.Finalize();
			}

			// 3. GetLocalPlayerSubSystemFromPlayerController (from SubsystemBlueprintLibrary)
			UFunction* GetSubsysFunc = nullptr;
			{
				UClass* SubsysBPLib = FindFirstObject<UClass>(TEXT("SubsystemBlueprintLibrary"));
				if (!SubsysBPLib) SubsysBPLib = FindFirstObject<UClass>(TEXT("USubsystemBlueprintLibrary"));
				if (SubsysBPLib)
				{
					GetSubsysFunc = SubsysBPLib->FindFunctionByName(FName(TEXT("GetLocalPlayerSubSystemFromPlayerController")));
					if (!GetSubsysFunc) GetSubsysFunc = SubsysBPLib->FindFunctionByName(FName(TEXT("GetLocalPlayerSubsystem")));
				}
				// Brute force if not found
				if (!GetSubsysFunc)
				{
					for (TObjectIterator<UClass> It; It; ++It)
					{
						GetSubsysFunc = It->FindFunctionByName(FName(TEXT("GetLocalPlayerSubSystemFromPlayerController")), EIncludeSuperFlag::ExcludeSuper);
						if (GetSubsysFunc && GetSubsysFunc->HasAnyFunctionFlags(FUNC_BlueprintCallable))
						{
							UE_LOG(LogNwiroBP, Log, TEXT("Found GetLocalPlayerSubSystemFromPlayerController in %s"), *It->GetName());
							break;
						}
						GetSubsysFunc = nullptr;
					}
				}
			}

			UK2Node_CallFunction* GetSubsysNode = nullptr;
			if (GetSubsysFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> C3(*Graph);
				GetSubsysNode = C3.CreateNode();
				GetSubsysNode->SetFromFunction(GetSubsysFunc);
				GetSubsysNode->NodePosX = PosX + 500;
				GetSubsysNode->NodePosY = PosY;
				C3.Finalize();

				// Set the Class pin to UEnhancedInputLocalPlayerSubsystem
				UEdGraphPin* ClassPin = ResolveAddedPin(GetSubsysNode, TEXT("Class"));
				if (ClassPin)
				{
					FString SubsysClassPath = UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName();
					if (!SetAddedObject(*ClassPin, UEnhancedInputLocalPlayerSubsystem::StaticClass())) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
				}
			}
			else
			{
				Errors.Add(TEXT("AddMappingContext: required subsystem function not found"));
			}

			// 4. AddMappingContext
			UFunction* AddMCFunc = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(FName(TEXT("AddMappingContext")));
			if (!AddMCFunc)
			{
				// Search in interface hierarchy
				for (UClass* C = UEnhancedInputLocalPlayerSubsystem::StaticClass(); C; C = C->GetSuperClass())
				{
					AddMCFunc = C->FindFunctionByName(FName(TEXT("AddMappingContext")));
					if (AddMCFunc) break;
				}
			}
			// Also search interfaces
			if (!AddMCFunc)
			{
				for (const FImplementedInterface& Iface : UEnhancedInputLocalPlayerSubsystem::StaticClass()->Interfaces)
				{
					if (Iface.Class)
					{
						AddMCFunc = Iface.Class->FindFunctionByName(FName(TEXT("AddMappingContext")));
						if (AddMCFunc) break;
					}
				}
			}

			UK2Node_CallFunction* AddMCNode = nullptr;
			if (AddMCFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> C4(*Graph);
				AddMCNode = C4.CreateNode();
				AddMCNode->SetFromFunction(AddMCFunc);
				AddMCNode->NodePosX = PosX + 750;
				AddMCNode->NodePosY = PosY;
				C4.Finalize();

				// Set IMC default value if path provided
				if (!IMCPath.IsEmpty())
				{
					UEdGraphPin* MCPin = ResolveAddedPin(AddMCNode, TEXT("MappingContext"));
					if (MCPin)
					{
						if (!SetAddedValue(*MCPin, IMCPath)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
					}
				}

				// Set priority
				UEdGraphPin* PriorityPin = ResolveAddedPin(AddMCNode, TEXT("Priority"));
				if (PriorityPin)
				{
					if (!SetAddedValue(*PriorityPin, FString::FromInt(Priority))) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
				}
			}

			// Wire them together
			if (GetCtrlNode && CastNode)
			{
				// GetController ReturnValue → Cast Object pin
				UEdGraphPin* CtrlOut = ResolveAddedPin(GetCtrlNode, TEXT("ReturnValue"), EGPD_Output);
				UEdGraphPin* CastIn = ResolveAddedPin(CastNode, TEXT("Object"), EGPD_Input);
				if (CtrlOut && CastIn) if (!ConnectAddedPins(CtrlOut, CastIn)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }

			}

			if (CastNode && GetSubsysNode)
			{
				// Cast "AsPlayer Controller" → GetSubsystem "PlayerController" pin
				// Note: Cast output pin name has space: "AsPlayer Controller"
				UEdGraphPin* CastResult = UniqueAddedObjectOutput(CastNode);
				UEdGraphPin* SubsysPC = ResolveAddedPin(GetSubsysNode, TEXT("PlayerController"), EGPD_Input);
				if (CastResult && SubsysPC)
				{
					if (!ConnectAddedPins(CastResult, SubsysPC)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
					UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Connected Cast→GetSubsystem (%s→%s)"), *CastResult->PinName.ToString(), *SubsysPC->PinName.ToString());
				}
				else
				{
					UE_LOG(LogNwiroBP, Warning, TEXT("AddMappingContext: Failed to connect Cast→GetSubsystem (CastResult=%d, SubsysPC=%d)"), CastResult!=nullptr, SubsysPC!=nullptr);
				}
			}

			// Cast GetSubsystem result to EnhancedInputLocalPlayerSubsystem
			// This resolves the Object→Interface type mismatch
			UK2Node_DynamicCast* SubsysCastNode = nullptr;
			if (GetSubsysNode && AddMCNode)
			{
				FGraphNodeCreator<UK2Node_DynamicCast> C5(*Graph);
				SubsysCastNode = C5.CreateNode();
				SubsysCastNode->TargetType = UEnhancedInputLocalPlayerSubsystem::StaticClass();
				SubsysCastNode->NodePosX = PosX + 625;
				SubsysCastNode->NodePosY = PosY;
				C5.Finalize();

				// GetSubsystem ReturnValue → Cast Object
				UEdGraphPin* SubsysOut = ResolveAddedPin(GetSubsysNode, TEXT("ReturnValue"), EGPD_Output);
				UEdGraphPin* CastObjIn = ResolveAddedPin(SubsysCastNode, TEXT("Object"), EGPD_Input);
				if (SubsysOut && CastObjIn) if (!ConnectAddedPins(SubsysOut, CastObjIn)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }

				// Cast result → AddMappingContext self
				UEdGraphPin* CastResultOut = UniqueAddedObjectOutput(SubsysCastNode);
				UEdGraphPin* MCTarget = ResolveAddedPin(AddMCNode, TEXT("self"), EGPD_Input);
				if (CastResultOut && MCTarget)
				{
					if (!ConnectAddedPins(CastResultOut, MCTarget)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
					UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Connected SubsysCast→AddMC (%s→%s)"), *CastResultOut->PinName.ToString(), *MCTarget->PinName.ToString());
				}
			}

			// Exec chain: PlayerController Cast → SubsysCast → AddMappingContext
			if (CastNode && SubsysCastNode && AddMCNode)
			{
				// PC Cast then → SubsysCast execute
				UEdGraphPin* PCCastExec = ResolveAddedPin(CastNode, TEXT("then"), EGPD_Output);
				UEdGraphPin* SubCastExec = ResolveAddedPin(SubsysCastNode, TEXT("execute"), EGPD_Input);
				if (PCCastExec && SubCastExec) if (!ConnectAddedPins(PCCastExec, SubCastExec)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }

				// SubsysCast then → AddMC execute
				UEdGraphPin* SubCastThen = ResolveAddedPin(SubsysCastNode, TEXT("then"), EGPD_Output);
				UEdGraphPin* MCExecIn = ResolveAddedPin(AddMCNode, TEXT("execute"), EGPD_Input);
				if (SubCastThen && MCExecIn) if (!ConnectAddedPins(SubCastThen, MCExecIn)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
			}
			else if (CastNode && AddMCNode)
			{
				// Fallback: direct exec
				UEdGraphPin* CastExecOut = ResolveAddedPin(CastNode, TEXT("then"), EGPD_Output);
				UEdGraphPin* MCExecIn = ResolveAddedPin(AddMCNode, TEXT("execute"), EGPD_Input);
				if (CastExecOut && MCExecIn) if (!ConnectAddedPins(CastExecOut, MCExecIn)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
			}

			// Auto-connect to BeginPlay if available and Cast exec input is free
			if (CastNode)
			{
				UEdGraphPin* CastExecIn = ResolveAddedPin(CastNode, TEXT("execute"), EGPD_Input);
				if (CastExecIn && CastExecIn->LinkedTo.Num() == 0)
				{
					// Find BeginPlay event in this graph
					for (UEdGraphNode* N : Graph->Nodes)
					{
						UK2Node_Event* EvtNode = Cast<UK2Node_Event>(N);
						if (EvtNode && EvtNode->GetFunctionName().ToString().Contains(TEXT("BeginPlay")))
						{
							UEdGraphPin* BPExecOut = ResolveAddedPin(EvtNode, TEXT("then"), EGPD_Output);
							if (BPExecOut && BPExecOut->LinkedTo.Num() == 0)
							{
								if (!ConnectAddedPins(BPExecOut, CastExecIn)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
								UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Auto-connected BeginPlay→Cast"));
							}
							break;
						}
					}
				}
			}

			// Store refs
			if (GetCtrlNode) StoreNodeRef(Ref.IsEmpty() ? TEXT("get_controller") : Ref + TEXT("_getctrl"), GetCtrlNode, GraphName);
			if (CastNode) StoreNodeRef(Ref.IsEmpty() ? TEXT("cast_pc") : Ref + TEXT("_cast"), CastNode, GraphName);
			if (AddMCNode)
			{
				StoreNodeRef(Ref.IsEmpty() ? TEXT("add_mc") : Ref, AddMCNode, GraphName);
				// Return AddMCNode as created - its "then" pin is the exit for further chaining
				CreatedNode = AddMCNode;
			}
			else
			{
				Errors.Add(TEXT("AddMappingContext function not found"));
				if (CastNode) CreatedNode = CastNode;
			}

			Added += 3; // We created multiple nodes
		}
		// ---- Delay (uses CallFunction on KismetSystemLibrary::Delay) ----
		else if (NodeType.Equals(TEXT("Delay"), ESearchCase::IgnoreCase))
		{
			UFunction* DelayFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("Delay")));
			if (DelayFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* DelayNode = Creator.CreateNode();
				DelayNode->SetFromFunction(DelayFunc);
				DelayNode->NodePosX = PosX;
				DelayNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = DelayNode;
			}
		}
		// ---- PrintString ----
		else if (NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("Print"), ESearchCase::IgnoreCase))
		{
			UFunction* PrintFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("PrintString")));
			if (PrintFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* PrintNode = Creator.CreateNode();
				PrintNode->SetFromFunction(PrintFunc);
				PrintNode->NodePosX = PosX;
				PrintNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = PrintNode;
			}
		}
		// ---- SetTimer ----
		else if (NodeType.Equals(TEXT("SetTimer"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetTimerByFunctionName"), ESearchCase::IgnoreCase))
		{
			UFunction* TimerFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("K2_SetTimerDelegate")));
			if (!TimerFunc)
			{
				TimerFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("SetTimerByFunctionName")));
			}
			if (TimerFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* TimerNode = Creator.CreateNode();
				TimerNode->SetFromFunction(TimerFunc);
				TimerNode->NodePosX = PosX;
				TimerNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = TimerNode;
			}
		}
		// ---- DestroyActor ----
		else if (NodeType.Equals(TEXT("DestroyActor"), ESearchCase::IgnoreCase))
		{
			UFunction* DestroyFunc = AActor::StaticClass()->FindFunctionByName(FName(TEXT("K2_DestroyActor")));
			if (DestroyFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* DestroyNode = Creator.CreateNode();
				DestroyNode->SetFromFunction(DestroyFunc);
				DestroyNode->NodePosX = PosX;
				DestroyNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = DestroyNode;
			}
		}
		// ---- SetActorLocation ----
		else if (NodeType.Equals(TEXT("SetActorLocation"), ESearchCase::IgnoreCase))
		{
			UFunction* Func = AActor::StaticClass()->FindFunctionByName(FName(TEXT("K2_SetActorLocation")));
			if (Func)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
			}
		}
		// ---- GetActorLocation ----
		else if (NodeType.Equals(TEXT("GetActorLocation"), ESearchCase::IgnoreCase))
		{
			UFunction* Func = AActor::StaticClass()->FindFunctionByName(FName(TEXT("K2_GetActorLocation")));
			if (Func)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
			}
		}
		// ---- EnhancedInputAction (UE5 Enhanced Input) ----
		else if (NodeType.Equals(TEXT("EnhancedInputAction"), ESearchCase::IgnoreCase))
		{
			// Reuse the exact, strongly rooted object admitted before target work.
			// Never re-search by name after earlier peer operations have mutated the BP.
			const FString ActionPath = Obj->GetStringField(TEXT("action"));
			UInputAction* FoundAction = PreparedActions.FindExact(ActionPath);
			if (!FoundAction || !PreparedActions.ActionProperty)
			{
				FNwiroBPResult Failure = FNwiroBPResult::Fail(TEXT("Prepared EnhancedInputAction identity was invalidated; earlier in-memory peer changes may remain. Compile/save are blocked."));
				Failure.Data = MakeShared<FJsonObject>();
				Failure.Data->SetBoolField(TEXT("enhancedInputPlanInvalidated"), true);
				return Failure;
			}
			FGraphNodeCreator<UK2Node_EnhancedInputAction> Creator(*Graph);
			UK2Node_EnhancedInputAction* EIANode = Creator.CreateNode();
			PreparedActions.ActionProperty->SetObjectPropertyValue(
				PreparedActions.ActionProperty->ContainerPtrToValuePtr<void>(EIANode), FoundAction);
			EIANode->NodePosX = PosX;
			EIANode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = EIANode;
		}
		// ---- SwitchOnInt ----
		else if (NodeType.Equals(TEXT("SwitchOnInt"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SwitchInteger"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_SwitchInteger> Creator(*Graph);
			UK2Node_SwitchInteger* SwitchNode = Creator.CreateNode();
			SwitchNode->NodePosX = PosX;
			SwitchNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SwitchNode;
		}
		// ---- SwitchOnString ----
		else if (NodeType.Equals(TEXT("SwitchOnString"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SwitchString"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_SwitchString> Creator(*Graph);
			UK2Node_SwitchString* SwitchNode = Creator.CreateNode();
			SwitchNode->NodePosX = PosX;
			SwitchNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SwitchNode;
		}
		// ---- MakeArray ----
		else if (NodeType.Equals(TEXT("MakeArray"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_MakeArray> Creator(*Graph);
			UK2Node_MakeArray* ArrayNode = Creator.CreateNode();
			ArrayNode->NodePosX = PosX;
			ArrayNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = ArrayNode;
		}
		// ---- Select ----
		else if (NodeType.Equals(TEXT("Select"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_Select> Creator(*Graph);
			UK2Node_Select* SelectNode = Creator.CreateNode();
			SelectNode->NodePosX = PosX;
			SelectNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SelectNode;
		}
		// ---- GetComponent: Gets a component variable from self (e.g., CharacterMovement) ----
		else if (NodeType.Equals(TEXT("GetComponent"), ESearchCase::IgnoreCase))
		{
			FString CompName = Obj->GetStringField(TEXT("component"));
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* GetNode = Creator.CreateNode();
			GetNode->VariableReference.SetSelfMember(FName(*CompName));
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = GetNode;
		}
		// ---- SetProperty: Sets a property on a target object (e.g., MaxWalkSpeed on CharacterMovement) ----
		else if (NodeType.Equals(TEXT("SetProperty"), ESearchCase::IgnoreCase))
		{
			FString PropName = GetFieldNormalized(Obj, TEXT("property"));
			FString OwnerClassName = GetFieldNormalized(Obj, TEXT("ownerclass")); // covers ownerClass / owner_class / OwnerClass

			UClass* OwnerClass = !OwnerClassName.IsEmpty() ? FindClassByName(OwnerClassName) : nullptr;

			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
			UK2Node_VariableSet* SetNode = Creator.CreateNode();
			if (OwnerClass)
			{
				SetNode->VariableReference.SetExternalMember(FName(*PropName), OwnerClass);
				NodeDetail = FString::Printf(TEXT("SetProperty:'%s' [resolved] owner:'%s'"), *PropName, *OwnerClass->GetName());
			}
			else if (!OwnerClassName.IsEmpty())
			{
				SetNode->VariableReference.SetSelfMember(FName(*PropName));
				Errors.Add(FString::Printf(TEXT("SetProperty:'%s' [not_resolved] owner:'%s' not found — node created as self-member; fix ownerClass field"), *PropName, *OwnerClassName));
			}
			else
			{
				SetNode->VariableReference.SetSelfMember(FName(*PropName));
				NodeDetail = FString::Printf(TEXT("SetProperty:'%s' [resolved] self"), *PropName);
			}
			SetNode->NodePosX = PosX;
			SetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SetNode;
		}
		// ---- GetProperty: Gets a property from a target object ----
		else if (NodeType.Equals(TEXT("GetProperty"), ESearchCase::IgnoreCase))
		{
			FString PropName = GetFieldNormalized(Obj, TEXT("property"));
			FString OwnerClassName = GetFieldNormalized(Obj, TEXT("ownerclass")); // covers ownerClass / owner_class / OwnerClass

			UClass* OwnerClass = !OwnerClassName.IsEmpty() ? FindClassByName(OwnerClassName) : nullptr;

			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* GetNode = Creator.CreateNode();
			if (OwnerClass)
			{
				GetNode->VariableReference.SetExternalMember(FName(*PropName), OwnerClass);
				NodeDetail = FString::Printf(TEXT("GetProperty:'%s' [resolved] owner:'%s'"), *PropName, *OwnerClass->GetName());
			}
			else if (!OwnerClassName.IsEmpty())
			{
				GetNode->VariableReference.SetSelfMember(FName(*PropName));
				Errors.Add(FString::Printf(TEXT("GetProperty:'%s' [not_resolved] owner:'%s' not found — node created as self-member; fix ownerClass field"), *PropName, *OwnerClassName));
			}
			else
			{
				GetNode->VariableReference.SetSelfMember(FName(*PropName));
				NodeDetail = FString::Printf(TEXT("GetProperty:'%s' [resolved] self"), *PropName);
			}
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = GetNode;
		}
		// ---- Sequence ----
		else if (NodeType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("ExecutionSequence"), ESearchCase::IgnoreCase))
		{
			UK2Node_CallFunction* SeqNode = NewObject<UK2Node_CallFunction>(Graph);
			UFunction* Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralInt"));
			// Use ExecutionSequence macro instead
			FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
			UK2Node_ExecutionSequence* Node = Creator.CreateNode();
			Node->NodePosX = PosX;
			Node->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = Node;
		}
		// ---- ForEachLoop / WhileLoop (Macros) ----
		else if (NodeType.Equals(TEXT("ForEachLoop"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("ForLoop"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("WhileLoop"), ESearchCase::IgnoreCase))
		{
			FString MacroName;
			if (NodeType.Contains(TEXT("ForEach"))) MacroName = TEXT("ForEachLoop");
			else if (NodeType.Contains(TEXT("ForLoop")) || NodeType.Contains(TEXT("For"))) MacroName = TEXT("ForLoop");
			else MacroName = TEXT("WhileLoop");

			UEdGraph* MacroGraph = nullptr;
			TArray<UBlueprint*> MacroLibs;
			MacroLibs.Add(LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")));
			for (UBlueprint* Lib : MacroLibs)
			{
				if (!Lib) continue;
				for (UEdGraph* G : Lib->MacroGraphs)
				{
					if (G && G->GetFName().ToString().Equals(MacroName, ESearchCase::IgnoreCase))
					{
						MacroGraph = G;
						break;
					}
				}
			}
			if (MacroGraph)
			{
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->NodePosX = PosX;
				MacroNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = MacroNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Macro not found: %s"), *MacroName));
			}
		}
		// ---- Gate / DoOnce / FlipFlop / DoN / IsValid (Macros) ----
		else if (NodeType.Equals(TEXT("Gate"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("DoOnce"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("FlipFlop"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("DoN"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("IsValid"), ESearchCase::IgnoreCase))
		{
			UEdGraph* MacroGraph = nullptr;
			UBlueprint* StdMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
			if (StdMacros)
			{
				for (UEdGraph* G : StdMacros->MacroGraphs)
				{
					if (G && G->GetFName().ToString().Equals(NodeType, ESearchCase::IgnoreCase))
					{
						MacroGraph = G;
						break;
					}
				}
			}
			if (MacroGraph)
			{
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->NodePosX = PosX;
				MacroNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = MacroNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Standard macro not found: %s"), *NodeType));
			}
		}
		// ---- Math shorthand nodes (Add, Subtract, Multiply, Divide, Clamp, Lerp, Abs, etc.) ----
		else if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Abs"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Min"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Max"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Power"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Sqrt"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("RandomFloat"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("RandomInteger"), ESearchCase::IgnoreCase))
		{
			// Map shorthand to actual function names
			FString FuncName = NodeType;
			if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase)) FuncName = TEXT("Add_FloatFloat");
			else if (NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase)) FuncName = TEXT("Subtract_FloatFloat");
			else if (NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase)) FuncName = TEXT("Multiply_FloatFloat");
			else if (NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase)) FuncName = TEXT("Divide_FloatFloat");
			else if (NodeType.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase)) FuncName = TEXT("Lerp");
			else if (NodeType.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase)) FuncName = TEXT("FClamp");
			else if (NodeType.Equals(TEXT("RandomFloat"), ESearchCase::IgnoreCase)) FuncName = TEXT("RandomFloatInRange");
			else if (NodeType.Equals(TEXT("RandomInteger"), ESearchCase::IgnoreCase)) FuncName = TEXT("RandomIntegerInRange");

			UFunction* Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*FuncName));
			if (!Func) Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*(TEXT("K") + FuncName)));
			if (Func)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Math function not found: %s -> %s"), *NodeType, *FuncName));
			}
		}
		// ---- Common shorthand functions ----
		else if (NodeType.Equals(TEXT("GetPlayerController"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetPlayerPawn"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetPlayerCharacter"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetPlayerCameraManager"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetGameMode"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetGameInstance"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetGameState"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("PlaySound2D"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("PlaySoundAtLocation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SpawnEmitterAtLocation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SpawnEmitterAttached"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("LineTraceByChannel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SphereTraceByChannel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("BoxTraceByChannel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AddToViewport"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("RemoveFromParent"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetInputMode"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("FormatText"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AppendString"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetWorldDeltaSeconds"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorRotation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetActorRotation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorScale3D"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetActorScale3D"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorTransform"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetActorTransform"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AddActorWorldOffset"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AddActorWorldRotation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorHiddenInGame"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("IsOverlappingActor"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetVisibility"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetCollisionEnabled"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("PlayAnimMontage"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("StopAnimMontage"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetTimerByEvent"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("ClearTimer"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("OpenLevel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("QuitGame"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetPercent"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetText"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("ApplyDamage"), ESearchCase::IgnoreCase))
		{
			// Remove spaces from NodeType for function lookup
			FString FuncName = NodeType.Replace(TEXT(" "), TEXT(""));
			if (NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase))
			{
				FuncName = TEXT("Create");
			}
			UFunction* Func = nullptr;

			TArray<UClass*> SearchLibs = {
				UGameplayStatics::StaticClass(),
				UKismetSystemLibrary::StaticClass(),
				UKismetMathLibrary::StaticClass(),
				UKismetStringLibrary::StaticClass(),
				UWidgetBlueprintLibrary::StaticClass(),
				UUserWidget::StaticClass(),
				UWidget::StaticClass(),
				UProgressBar::StaticClass(),
				UTextBlock::StaticClass(),
			};
			if (BP->GeneratedClass)
			{
				for (UClass* C = BP->GeneratedClass; C; C = C->GetSuperClass())
					SearchLibs.Add(C);
			}

			for (UClass* Lib : SearchLibs)
			{
				for (TFieldIterator<UFunction> It(Lib); It; ++It)
				{
					FString Name = It->GetName();
					const FString DisplayName = It->GetMetaData(TEXT("DisplayName")).Replace(TEXT(" "), TEXT(""));
					if (Name.Equals(FuncName, ESearchCase::IgnoreCase) || Name.Replace(TEXT("_"), TEXT("")).Equals(FuncName, ESearchCase::IgnoreCase) || DisplayName.Equals(NodeType.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase))
					{
						Func = *It;
						break;
					}
				}
				if (Func) break;
			}

			if (Func)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				if (NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase))
				{
					FString WidgetClassName = GetFieldNormalized(Obj, TEXT("widgetclass"));
					if (WidgetClassName.IsEmpty()) WidgetClassName = GetFieldNormalized(Obj, TEXT("widgetblueprint"));
					if (WidgetClassName.IsEmpty()) WidgetClassName = GetFieldNormalized(Obj, TEXT("class"));
					if (!WidgetClassName.IsEmpty())
					{
						if (UClass* WidgetClass = FindClassByName(WidgetClassName))
						{
							if (UEdGraphPin* WidgetTypePin = ResolveAddedPin(FuncNode, TEXT("WidgetType"), EGPD_Input))
							{
								if (!SetAddedObject(*WidgetTypePin, WidgetClass)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); }
								NodeDetail = FString::Printf(TEXT("CreateWidget [resolved] class:'%s'"), *WidgetClass->GetName());
							}
						}
						else
						{
							Errors.Add(FString::Printf(TEXT("CreateWidget [not_resolved] class:'%s' not found — node created without WidgetType; use class/widgetClass/widgetBlueprint or set_pin_defaults"), *WidgetClassName));
						}
					}
				}
				CreatedNode = FuncNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Function not found: %s"), *NodeType));
			}
		}
		else
		{
			UFunction* FallbackFn = nullptr;
			const FString CleanName = NodeType.Replace(TEXT(" "), TEXT(""));
			const FString K2Name = TEXT("K2_") + CleanName;
			for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
			{
				for (TFieldIterator<UFunction> FnIt(*ClassIt, EFieldIterationFlags::None); FnIt; ++FnIt)
				{
					const FString FN = FnIt->GetName();
					if (FN.Equals(CleanName, ESearchCase::IgnoreCase)
						|| FN.Equals(K2Name, ESearchCase::IgnoreCase))
					{
						FallbackFn = *FnIt; break;
					}
					const FString DisplayName = FnIt->GetMetaData(TEXT("DisplayName"));
					if (!DisplayName.IsEmpty()
						&& DisplayName.Replace(TEXT(" "), TEXT("")).Equals(CleanName, ESearchCase::IgnoreCase))
					{
						FallbackFn = *FnIt; break;
					}
				}
				if (FallbackFn) break;
			}
			// Math-op auto-promote: if NodeType is a bare math operator like
			// "Subtract" / "Divide" / "LessEqual" / etc., the actual UFunction
			// is suffixed (e.g. Subtract_DoubleDouble) so the bare-name search
			// above misses. Try the same _DoubleDouble/_FloatFloat/_IntInt
			// suffixes the explicit CallFunction path uses.
			if (!FallbackFn)
			{
				static const TArray<FString> MathOps = {
					TEXT("Add"), TEXT("Subtract"), TEXT("Multiply"), TEXT("Divide"),
					TEXT("Min"), TEXT("Max"), TEXT("Equal"), TEXT("NotEqual"),
					TEXT("Less"), TEXT("LessEqual"), TEXT("Greater"), TEXT("GreaterEqual"),
				};
				for (const FString& Op : MathOps)
				{
					if (!CleanName.Equals(Op, ESearchCase::IgnoreCase)) continue;
					const TArray<FString> Suffixes = {
						TEXT("_DoubleDouble"), TEXT("_FloatFloat"),
						TEXT("_IntInt"), TEXT("_VectorVector"),
					};
					for (const FString& Suf : Suffixes)
					{
						UFunction* F = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*(Op + Suf)));
						if (F) { FallbackFn = F; break; }
					}
					break;
				}
			}
			if (FallbackFn)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(FallbackFn);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
				NodeDetail = FString::Printf(TEXT("%s [fallback] resolved as function %s in %s"),
					*FallbackFn->GetName(), *FallbackFn->GetName(),
					FallbackFn->GetOwnerClass() ? *FallbackFn->GetOwnerClass()->GetName() : TEXT("unknown"));
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Unknown node type: %s (also tried as a function name across all UClasses with K2_ prefix and DisplayName)"), *NodeType));
			}
		}

		if (CreatedNode)
		{
			// Auto-set pin defaults from extra JSON fields (key, axisName, value, etc.)
			static const TSet<FString> NodeReservedKeys = { TEXT("action"), TEXT("allow_ambiguous_bruteforce"), TEXT("autoPlay"), TEXT("class"), TEXT("component"), TEXT("context"), TEXT("event"), TEXT("floatTracks"), TEXT("function"), TEXT("id"), TEXT("interp"), TEXT("key"), TEXT("keys"), TEXT("kind"), TEXT("length"), TEXT("loop"), TEXT("macro"), TEXT("mappingContext"), TEXT("name"), TEXT("node"), TEXT("nodeClass"), TEXT("nodeType"), TEXT("node_type"), TEXT("nodetype"), TEXT("params"), TEXT("posX"), TEXT("posY"), TEXT("priority"), TEXT("ref"), TEXT("target"), TEXT("time"), TEXT("type"), TEXT("value"), TEXT("variable"), TEXT("x"), TEXT("y") };

			for (const auto& Pair : Obj->Values)
			{
				const FString Key(*Pair.Key);
				if (NodeReservedKeys.Contains(Key)) continue;

				FString PinVal;
				if (Pair.Value->Type == EJson::String) PinVal = Pair.Value->AsString();
				else if (Pair.Value->Type == EJson::Number) PinVal = FString::SanitizeFloat(Pair.Value->AsNumber());
				else if (Pair.Value->Type == EJson::Boolean) PinVal = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
				else continue;

				FString PinError;
				UEdGraphPin* Pin = FindPin(CreatedNode, Key, EGPD_Input, &PinError);
				if (!PinError.IsEmpty()) Errors.Add(PinError);
				else if (Pin) { if (!SetAddedValue(*Pin, PinVal)) { NwiroBlueprintBatch::RecordErrors(Errors); return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; "))); } }
				else Errors.Add(TEXT("Unresolved generated-node default pin: ") + Key);

			}

			StoreNodeRef(Ref.IsEmpty() ? FString::Printf(TEXT("node_%d"), Added) : Ref, CreatedNode, GraphName);
			// Also register a secondary ref keyed on the node's editable title
			// so the LLM can connect later via the friendly name it sees in
			// read_blueprint output. If a node with that title already exists
			// in the ref map, append an index suffix so each node stays
			// uniquely addressable (e.g. "Add Actor Local Rotation",
			// "Add Actor Local Rotation 2").
			if (CreatedNode)
			{
				const FString Title = CreatedNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
				if (!Title.IsEmpty())
				{
					FString Key = Title;
					int32 Suffix = 2;
					// Scoped: a title already used in another Blueprint must not push this
					// node to "Title 2".
					while (NodeRefs.Contains(MakeNodeRefKey(CreatedNode->GetGraph(), Key)))
					{
						Key = FString::Printf(TEXT("%s %d"), *Title, Suffix++);
					}
					StoreNodeRef(Key, CreatedNode, GraphName);
				}
			}
			Added++;
			if (NodeDetail.IsEmpty()) NodeDetail = CreatedNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
			// Node GUIDs were appended here, but the agent references nodes by title (see
			// StoreNodeRef above), never by guid, so the 36-char guid per node was pure
			// response bloat across multi-node edits. Dropped.
			NodeDetails.Add(NodeDetail);
		}

		NodeX += 300;
		if (Added % 5 == 0)
		{
			NodeX = 300;
			NodeY += 300;
		}
	}

	NwiroBlueprintBatch::RecordErrors(Errors);
	FString Msg = FString::Printf(TEXT("Added %d node(s)"), Added);
	if (NodeDetails.Num() > 0) Msg += TEXT(". Details: ") + FString::Join(NodeDetails, TEXT("; "));
	if (Skipped > 0) Msg += FString::Printf(TEXT(", skipped %d duplicate(s)"), Skipped);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	// >>> NWIRO_PARITY BEGIN: rejected-fails-op (keep in sync with the other product plugin)
	// The outer owner executes this once on an isolated staging Blueprint before
	// target admission. A rejected sibling therefore leaves the target untouched.
	// Unexpected failure during an admitted commit is partial/unknown and blocks
	// the outer compile/save; it does not promise undo of engine callbacks.
	if (Rejected > 0) return FNwiroBPResult::Fail(Msg);
	// <<< NWIRO_PARITY END: rejected-fails-op
	if (bPinResolutionRefused || (NwiroBlueprintBatch::Active && Errors.Num() > 0)) return FNwiroBPResult::Fail(Msg);
	return FNwiroBPResult::Ok(Msg);
}

// ============================================================
// CONNECT PINS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoConnectPins(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return FNwiroBPResult::Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));

	int32 Connected = 0;
	TArray<FString> Errors;
	bool bPinResolutionRefused = false;

	// Level-2 ledger: one entry per requested pin pair, so a caller can tell an
	// applied connection from a rejected one without parsing the message string.
	// Emitted regardless of the strict-verdict CVar — it changes no signal.
	FNwiroUnitLedger Ledger;
	int32 Unit = -1;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		if (NwiroBlueprintBatch::StopCommit(Errors)) break;
		++Unit;
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		// Format: "from"/"source": "nodeRef.PinName", "to"/"target": "nodeRef.PinName"
		FString FromStr = Obj->HasField(TEXT("from")) ? Obj->GetStringField(TEXT("from"))
			: Obj->HasField(TEXT("source")) ? Obj->GetStringField(TEXT("source")) : TEXT("");
		FString ToStr = Obj->HasField(TEXT("to")) ? Obj->GetStringField(TEXT("to"))
			: Obj->HasField(TEXT("target")) ? Obj->GetStringField(TEXT("target")) : TEXT("");

		if (FromStr.IsEmpty() || ToStr.IsEmpty())
		{
			Errors.Add(TEXT("Connection needs both 'from' and 'to'"));
			Ledger.AddError(Unit, TEXT("connect_pins"), TEXT("MISSING_FIELD"),
				FromStr.IsEmpty() ? TEXT("from") : TEXT("to"), TEXT(""),
				TEXT("Connection needs both 'from' and 'to'"));
			continue;
		}

		// Build a snapshot of every available "ref.pin" combo in this graph,
		// so we can hand it back in error messages and let the LLM self-correct
		// instead of nuking the graph and starting over.
		// The ref list is built ONCE and rendered twice: joined for the human sentence,
		// raw for the diagnostic's allowed[]. Previously only the sentence existed, so a
		// caller had to parse English to learn which refs it could have used.
		// Built at most once per call and returned by reference. It used to be rebuilt for
		// every failing unit — twice each, since both the message and allowed[] wanted it —
		// with a nested duplicate scan inside, so a batch of M bad endpoints on an N-node
		// graph cost O(M*N^2) on the editor thread. Counting titles in a map makes the build
		// O(N), and caching makes it once instead of 2M times.
		TArray<FString> AvailableRefsCache;
		bool bAvailableRefsBuilt = false;
		auto BuildAvailableRefList = [&]() -> const TArray<FString>&
		{
			if (bAvailableRefsBuilt) return AvailableRefsCache;
			bAvailableRefsBuilt = true;

			// Node-not-found only needs the valid node REF to pick from, not its pins. The
			// all-pins dump per node was the dominant edit_blueprint response bloat (multi-KB
			// error strings on big graphs, accumulated across self-correction turns until the
			// agent's token budget ran out). Pins are still surfaced for the targeted
			// PinNotFound case below, where they are actually actionable.
			TMap<FString, int32> TitleCounts;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				TitleCounts.FindOrAdd(Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString())++;
			}

			// Every entry must RESOLVE. A title shared by two nodes returns AMBIGUOUS_REF, so
			// listing it would repeat the failure this list exists to escape — emit those
			// nodes' GUIDs instead, which step 3 accepts and which resolve uniquely.
			TSet<FString> Emitted;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				const FString Title = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
				const int32* Count = TitleCounts.Find(Title);
				const FString Entry = (Count && *Count == 1)
					? Title
					: Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
				bool bAlready = false;
				Emitted.Add(Entry, &bAlready);
				if (!bAlready) AvailableRefsCache.Add(Entry);
			}
			return AvailableRefsCache;
		};

		// Parse "ref.pinName" or "ref:pinName" — accept either separator. The
		// LLM keeps mixing them up. Splits at the LAST dot/colon so node refs
		// containing periods (e.g. asset paths) still work.
		enum class EParseResult : uint8 { Ok, NoDot, NodeNotFound, PinNotFound, Ambiguous, AmbiguousPin };
		// Diagnostic code for the level-2 ledger. Exhaustive on purpose: folding NoDot
		// into PIN_NOT_FOUND would send a caller looking for a pin when the reference
		// never named one.
		auto ParseCode = [](EParseResult R) -> FString
		{
			switch (R)
			{
			case EParseResult::NoDot:        return TEXT("MALFORMED_PIN_REF");
			case EParseResult::NodeNotFound: return TEXT("NODE_NOT_FOUND");
			case EParseResult::PinNotFound:  return TEXT("PIN_NOT_FOUND");
			// Distinct from NODE_NOT_FOUND on purpose: the node is not missing, several
			// match. A caller told "not found" will rebuild a node that already exists.
			case EParseResult::Ambiguous:    return TEXT("AMBIGUOUS_REF");
			case EParseResult::AmbiguousPin: return TEXT("AMBIGUOUS_PIN");
			default:                         return TEXT("UNKNOWN");
			}
		};
		// Filled by ParsePinRef and read by the ExplainFailure call that immediately
		// follows it. Reset on entry so a `to` parse cannot inherit the `from` message.
		FString AmbiguityMsg;
		auto ParsePinRef = [&](const FString& Str, UEdGraphNode*& OutNode, UEdGraphPin*& OutPin) -> EParseResult
		{
			int32 SepIdx = INDEX_NONE;
			Str.FindLastChar('.', SepIdx);
			int32 ColonIdx = INDEX_NONE;
			Str.FindLastChar(':', ColonIdx);
			if (ColonIdx > SepIdx) SepIdx = ColonIdx;
			if (SepIdx == INDEX_NONE) return EParseResult::NoDot;

			const FString NodeRef = Str.Left(SepIdx);
			const FString PinName = Str.Mid(SepIdx + 1);

			AmbiguityMsg.Reset();
			OutNode = FindNodeByRef(Graph, NodeRef, &AmbiguityMsg);
			if (!OutNode)
			{
				return AmbiguityMsg.IsEmpty() ? EParseResult::NodeNotFound
				                              : EParseResult::Ambiguous;
			}

			OutPin = FindPin(OutNode, PinName, EGPD_MAX, &AmbiguityMsg);
			if (!AmbiguityMsg.IsEmpty())
			{
				bPinResolutionRefused = true;
				return EParseResult::AmbiguousPin;
			}
			return OutPin ? EParseResult::Ok : EParseResult::PinNotFound;
		};

		UEdGraphNode* FromNode = nullptr;
		UEdGraphPin* FromPin = nullptr;
		UEdGraphNode* ToNode = nullptr;
		UEdGraphPin* ToPin = nullptr;

		auto ExplainFailure = [&](const FString& Str, EParseResult R, UEdGraphNode* Node) -> FString
		{
			switch (R)
			{
				case EParseResult::NoDot:
					return FString::Printf(TEXT("'%s' missing dot — expected format 'NodeRef.PinName'"), *Str);
				case EParseResult::NodeNotFound:
					// The list is not repeated here: it ships in allowed[], and duplicating
					// it would put the same multi-KB payload on the wire twice.
					return FString::Printf(
						TEXT("Node not found in '%s'. %d node ref(s) available — see errors[].allowed"),
						*Str, BuildAvailableRefList().Num());
				case EParseResult::AmbiguousPin:
				case EParseResult::Ambiguous:
					// FindNodeByRef already named the candidates and their GUIDs; repeating
					// the available-refs dump here would bury the one list that resolves it.
					return AmbiguityMsg;
				case EParseResult::PinNotFound:
				{
					TArray<FString> InPins, OutPins;
					if (Node) for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin) (Pin->Direction == EGPD_Input ? InPins : OutPins).Add(Pin->PinName.ToString());
					}
					return FString::Printf(TEXT("Pin not found in '%s'. The node exists but its pins are inputs=[%s] outputs=[%s]"),
						*Str,
						*FString::Join(InPins, TEXT(",")),
						*FString::Join(OutPins, TEXT(",")));
				}
				default:
					return FString::Printf(TEXT("Unknown parse error for '%s'"), *Str);
			}
		};

		// NoDot and Ambiguous return empty on purpose: a malformed reference has no
		// enumerable replacement, and the ambiguity message already carries the GUIDs.
		// Which COMPONENT of the compound endpoint the diagnostic is about. Paired with
		// AllowedFor so that field and allowed[] always describe the same thing.
		auto FieldFor = [](EParseResult R, const TCHAR* Base) -> FString
		{
			if (R == EParseResult::NodeNotFound || R == EParseResult::Ambiguous)
			{
				return FString(Base) + TEXT(".node");
			}
			if (R == EParseResult::PinNotFound || R == EParseResult::AmbiguousPin)
			{
				return FString(Base) + TEXT(".pin");
			}
			return FString(Base);
		};

		auto AllowedFor = [&](EParseResult R, UEdGraphNode* Node) -> TArray<FString>
		{
			if (R == EParseResult::NodeNotFound) return BuildAvailableRefList();
			if (R == EParseResult::PinNotFound)
			{
				TArray<FString> Pins;
				if (Node) for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin) Pins.Add(Pin->PinName.ToString());
				}
				return Pins;
			}
			return TArray<FString>();
		};

		const EParseResult FromR = ParsePinRef(FromStr, FromNode, FromPin);
		if (FromR != EParseResult::Ok)
		{
			const FString Why = ExplainFailure(FromStr, FromR, FromNode);
			Errors.Add(Why);
			Ledger.AddError(Unit, TEXT("connect_pins"), ParseCode(FromR),
				FieldFor(FromR, TEXT("from")), FromStr, Why, AllowedFor(FromR, FromNode));
			continue;
		}

		const EParseResult ToR = ParsePinRef(ToStr, ToNode, ToPin);
		if (ToR != EParseResult::Ok)
		{
			const FString Why = ExplainFailure(ToStr, ToR, ToNode);
			Errors.Add(Why);
			Ledger.AddError(Unit, TEXT("connect_pins"), ParseCode(ToR),
				FieldFor(ToR, TEXT("to")), ToStr, Why, AllowedFor(ToR, ToNode));
			continue;
		}

		if (NwiroBlueprintBatch::Active && NwiroBlueprintBatch::Active->bStaging)
		{
			auto CanonicalEndpoint = [&](UEdGraphNode* Node, UEdGraphPin* Pin, const FString& Requested)
			{
				int32 Dot = INDEX_NONE, Colon = INDEX_NONE;
				Requested.FindLastChar('.', Dot); Requested.FindLastChar(':', Colon);
				return NwiroBlueprintBatch::PlannedNodeRef(Node, Requested.Left(FMath::Max(Dot, Colon)))
					+ TEXT(".") + Pin->PinName.ToString();
			};
			Obj->SetStringField(TEXT("from"), CanonicalEndpoint(FromNode, FromPin, FromStr));
			Obj->SetStringField(TEXT("to"), CanonicalEndpoint(ToNode, ToPin, ToStr));
		}
		TStrongObjectPtr<UEdGraphNode> KeepFrom(FromNode), KeepTo(ToNode);
		const FGuid FromId = FromPin->PinId, ToId = ToPin->PinId;
		const FEdGraphPinType FromType = FromPin->PinType, ToType = ToPin->PinType;
		auto ReacquireEndpoints = [&]()
		{
			FromPin = IsValid(KeepFrom.Get()) ? KeepFrom->FindPinById(FromId) : nullptr;
			ToPin = IsValid(KeepTo.Get()) ? KeepTo->FindPinById(ToId) : nullptr;
			return FromPin && ToPin && Graph->Nodes.Contains(KeepFrom.Get()) && Graph->Nodes.Contains(KeepTo.Get())
				&& FromPin->PinType == FromType && ToPin->PinType == ToType;
		};
		// Pre-check: reject unrelated PC_Object connections that TryCreateConnection
		// silently accepts but the compiler then rejects (e.g. Actor → StaticMeshComponent).
		{
			UClass* FromClass = Cast<UClass>(FromType.PinSubCategoryObject.Get());
			UClass* ToClass   = Cast<UClass>(ToType.PinSubCategoryObject.Get());
			const bool bFromIsObj = FromType.PinCategory == UEdGraphSchema_K2::PC_Object;
			const bool bToIsObj   = ToType.PinCategory   == UEdGraphSchema_K2::PC_Object;
			if (bFromIsObj && bToIsObj && FromClass && ToClass &&
				!FromClass->IsChildOf(ToClass) && !ToClass->IsChildOf(FromClass))
			{
				const FString Why = FString::Printf(
					TEXT("Type mismatch: cannot connect %s (%s) to %s (%s) — unrelated types. Did you mean to call Get<ComponentName> first?"),
					*FromStr, *FromClass->GetName(), *ToStr, *ToClass->GetName());
				Errors.Add(Why);
				Ledger.AddError(Unit, TEXT("connect_pins"), TEXT("TYPE_MISMATCH"),
					TEXT("from"), FromStr, Why);
				continue;
			}
		}

		// Try to connect
		if (!ReacquireEndpoints())
		{
			const FString Why = TEXT("Connection endpoint changed during type resolution");
			Errors.Add(Why);
			Ledger.AddError(Unit, TEXT("connect_pins"), TEXT("PIN_IDENTITY_CHANGED"), TEXT("from"), FromStr, Why);
			continue;
		}
		FString ConnectionError;
		const bool bConnected = NwiroBlueprintBatch::Connect(FromPin, ToPin, true, ConnectionError);

		if (bConnected)
		{
			Connected++;
			Ledger.Modified.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("connect_pins"),
				FString::Printf(TEXT("%s -> %s"), *FromStr, *ToStr), /*bAsRequested*/ true));
		}
		else
		{
			const FString Why = FString::Printf(TEXT("Failed to connect: %s -> %s: %s"), *FromStr, *ToStr, *ConnectionError);
			Errors.Add(Why);
			Ledger.AddError(Unit, TEXT("connect_pins"), TEXT("CONNECT_REFUSED"),
				TEXT("from"), FromStr, Why);
		}
	}

	// Units the loop neither applied nor rejected fell through an early guard (a
	// non-object item). Account for them so the invariant holds exactly rather than
	// approximately — an invariant that only usually holds cannot be checked.
	const TSet<int32> AccountedUnits = Ledger.AccountedUnits();
	for (int32 UnitIndex = 0; UnitIndex < Items.Num(); ++UnitIndex)
	{
		if (AccountedUnits.Contains(UnitIndex)) continue;
		Ledger.Skipped.Add(FNwiroUnitLedger::MakeUnit(UnitIndex, TEXT("connect_pins"),
			TEXT("(malformed request item)"), /*bAsRequested*/ false));
	}

	NwiroBlueprintBatch::RecordErrors(Errors);
	FString Msg = FString::Printf(TEXT("Connected %d pin pair(s)"), Connected);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	// Strict by default — see CVarStrictConnectPins for what was measured before
	// flipping it, and for the escape hatch that restores the legacy always-Ok verdict.
	// The Integration Kit applies the same rule with no CVar.
	//
	// Note this fails on a PARTIAL batch too, not just zero-of-N. That is deliberate and
	// is why the ledger is attached either way: modified[] still names the units that DID
	// land, so success:false stays readable instead of implying nothing happened.
	const bool bStrict = CVarStrictConnectPins.GetValueOnGameThread() != 0;
	FNwiroBPResult R = (!bPinResolutionRefused && (!bStrict || (Errors.Num() == 0 && Connected == Items.Num())))
		? FNwiroBPResult::Ok(Msg) : FNwiroBPResult::Fail(Msg);
	R.Level2 = Ledger.Build(TEXT("connect_pins"), Items.Num());
	return R;
}

// ============================================================
// SET PIN DEFAULTS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoSetPinDefaults(UBlueprint* BP, const FString& GraphName,
    const TArray<TSharedPtr<FJsonValue>>& Items)
{
    UEdGraph* Graph = FindGraph(BP, GraphName);
    if (!Graph) return FNwiroBPResult::Fail(TEXT("Graph not found: ") + GraphName);
    FNwiroUnitLedger Ledger;
    TArray<FString> Errors;
    int32 Set = 0;
    for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
    {
        if (NwiroBlueprintBatch::StopCommit(Errors)) break;
        const auto& Value = Items[Unit];
        FString Error, Ref, Name, Requested;
        const auto Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        if (!Item.IsValid() || !(Item->TryGetStringField(TEXT("ref"), Ref) || Item->TryGetStringField(TEXT("node"), Ref))
            || Ref.IsEmpty() || !Item->TryGetStringField(TEXT("pin"), Name) || Name.IsEmpty()
            || !Item->TryGetStringField(TEXT("value"), Requested))
            Error = TEXT("set_pin_defaults requires string ref/node, pin, and value");
        UEdGraphNode* Node = Error.IsEmpty() ? FindNodeByRef(Graph, Ref, &Error) : nullptr;
        if (!Node && Error.IsEmpty()) Error = TEXT("Node not found: ") + Ref;
        UEdGraphPin* Pin = Node ? FindPin(Node, Name, EGPD_Input, &Error) : nullptr;
        if (!Pin && Error.IsEmpty()) Error = TEXT("Pin not found: ") + Ref + TEXT(".") + Name;
        FString Canonical;
        if (Pin && Error.IsEmpty())
        {
            // Save canonical operands before any schema callback can reconstruct pins.
            const FString CanonicalName = Pin->PinName.ToString();
            const FString CanonicalRef = NwiroBlueprintBatch::PlannedNodeRef(Node, Ref);
            if (NwiroBlueprintBatch::SetDefault(Pin, Requested,
                [](const FString& ClassName) { return FindClassByName(ClassName); }, Canonical, Error))
            {
                ++Set;
                if (NwiroBlueprintBatch::Active && NwiroBlueprintBatch::Active->bStaging)
                {
                    Item->SetStringField(TEXT("ref"), CanonicalRef);
                    Item->SetStringField(TEXT("pin"), CanonicalName);
                    Item->SetStringField(TEXT("value"), Canonical);
                }
                Ledger.Modified.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("set_pin_defaults"), Ref + TEXT(".") + Name, true));
            }
        }
        if (!Error.IsEmpty())
        {
            Errors.Add(Error);
            Ledger.AddError(Unit, TEXT("set_pin_defaults"), TEXT("PIN_DEFAULT_REJECTED"), TEXT("pin/value"), Ref + TEXT(".") + Name, Error);
        }
    }
    NwiroBlueprintBatch::RecordErrors(Errors);
    FNwiroBPResult Result = Errors.IsEmpty() ? FNwiroBPResult::Ok(FString::Printf(TEXT("Set %d validated pin default(s)"), Set))
        : FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; ")));
    const TSet<int32> Accounted = Ledger.AccountedUnits();
    for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
        if (!Accounted.Contains(Unit)) Ledger.Skipped.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("set_pin_defaults"), TEXT("not attempted after commit interruption"), false));
    Result.Level2 = Ledger.Build(TEXT("set_pin_defaults"), Items.Num());
    return Result;
}

// ============================================================
// CREATE BLUEPRINT
// ============================================================

FString FNwiroBlueprintTools::CreateBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString ParentClassName = Cmd->GetStringField(TEXT("parentClass"));

	if (Name.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'name' field\"}");
	}

	// FName NAME_SIZE assertion guard. See NwiroIKBlueprintTools::CreateBlueprint
	// (fuzz finding: 100KB name crashed UnrealNames.cpp).
	if (Name.Len() > 256 || Path.Len() > 1024 || ParentClassName.Len() > 1024)
	{
		return TEXT("{\"success\": false, \"error\": \"Name/path too long. Asset names must be <256 chars and paths <1024 chars (UE FName limit).\"}");
	}

	if (Path.IsEmpty())
	{
		Path = TEXT("/Game");
	}

	// Hallucination-tolerant detection: the LLM frequently passes one of many
	// variants when it wants a Blueprint Interface. There is no UClass called
	// "BlueprintInterface" — BPIs are normal blueprints with BPTYPE_Interface
	// and parent = UInterface. We detect intent here and create the right
	// asset, instead of silently falling back to Actor.
	auto IsInterfaceIntent = [](const FString& In) -> bool
	{
		if (In.IsEmpty()) return false;
		FString S = In.ToLower();
		S.RemoveFromStart(TEXT("u"));
		S.RemoveFromStart(TEXT("/"));
		int32 Dot; if (S.FindLastChar('.', Dot)) S = S.RightChop(Dot + 1);
		return S == TEXT("blueprintinterface")
			|| S == TEXT("blueprint_interface")
			|| S == TEXT("bpi")
			|| S == TEXT("interface");
	};
	const bool bWantsInterface = IsInterfaceIntent(ParentClassName);

	// Find parent class
	UClass* ParentClass = bWantsInterface ? UInterface::StaticClass() : AActor::StaticClass();
	if (!ParentClassName.IsEmpty() && !bWantsInterface)
	{
		UClass* Found = FindClassByName(ParentClassName);
		if (Found) ParentClass = Found;
	}

	FString FullPath = Path / Name;
	// The package name must be a mounted, /-rooted long package name before anything below touches
	// the object system: a Windows or relative path reaches the asset registry's path tree, whose
	// check() kills the editor (PathTree.cpp:39); '//' is a Fatal in CreatePackage; a joined name of
	// NAME_SIZE characters or more is a checkf in FName. Length first, so the validator never sees a
	// string the name table cannot hold.
	{
		FText PathError;
		if (FullPath.Len() >= NAME_SIZE || !FPackageName::IsValidLongPackageName(FullPath, false, &PathError))
		{
			const FString Reason = PathError.IsEmpty()
				? FString::Printf(TEXT("the package name is %d characters; the limit is %d"), FullPath.Len(), NAME_SIZE - 1)
				: PathError.ToString();
			TSharedRef<FJsonObject> Err = MakeShareable(new FJsonObject());
			Err->SetBoolField(TEXT("success"), false);
			Err->SetStringField(TEXT("error"), FString::Printf(
				TEXT("Invalid package path '%s': %s. 'path' must be a content folder such as /Game/Blueprints, not a disk path."),
				*FullPath, *Reason));
			FString Out;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Err, W);
			return Out;
		}
	}
	{ const FString _C = NwiroCheckCreateConflict(FullPath, Name, UBlueprint::StaticClass()); if (!_C.IsEmpty()) return _C; }

	// Check if blueprint already exists
	UBlueprint* ExistingBP = LoadObject<UBlueprint>(nullptr, *FullPath);
	if (ExistingBP)
	{
		return FString::Printf(TEXT("{\"success\": true, \"name\": \"%s\", \"path\": \"%s\", \"message\": \"Blueprint already exists\"}"), *Name, *ExistingBP->GetPathName());
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateBlueprint", "AI: Create Blueprint"));

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create package\"}");
	}
	Tx.AlsoModify(Package);

	UBlueprint* NewBP = nullptr;

	{
		// Prevent GC during entire create+compile cycle
		FGCScopeGuard GCScopeGuard;

		Package->AddToRoot();

		const EBlueprintType BPType = bWantsInterface ? BPTYPE_Interface : BPTYPE_Normal;
		NewBP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*Name), BPType, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

		if (!NewBP)
		{
			Package->RemoveFromRoot();
			return TEXT("{\"success\": false, \"error\": \"Failed to create blueprint\"}");
		}

		NewBP->AddToRoot();
		FAssetRegistryModule::AssetCreated(NewBP);
		NewBP->MarkPackageDirty();

		FKismetEditorUtilities::CompileBlueprint(NewBP,
			NwiroBlueprintBatch::Active && NwiroBlueprintBatch::Active->bCreatingTarget
				? EBlueprintCompileOptions::SkipSave : EBlueprintCompileOptions::None);

		// Post-compile repair: ONLY if compile failed to populate ClassConstructor
		// or ClassWithin. Do NOT force-rebuild CDO of a successfully compiled BP —
		// that corrupts SceneRootComponent ownership (parent CDO's components get
		// inherited with parent's owner, triggering
		// "SceneRootComponent->GetOwner() == this" assert during SpawnActor).
		if (UClass* GenClass = NewBP->GeneratedClass)
		{
			const bool bNeedsConstructor = (GenClass->ClassConstructor == nullptr);
			const bool bNeedsWithin = (GenClass->ClassWithin == nullptr);
			if (bNeedsConstructor || bNeedsWithin)
			{
				if (UClass* SuperClass = GenClass->GetSuperClass())
				{
					if (!SuperClass->ClassConstructor) SuperClass->Bind();
				}
				if (bNeedsWithin)
				{
					UClass* ParentWithin = GenClass->GetSuperClass() ? GenClass->GetSuperClass()->ClassWithin.Get() : nullptr;
					GenClass->ClassWithin = ParentWithin ? ParentWithin : UObject::StaticClass();
				}
				GenClass->Bind();
				GenClass->StaticLink(true);
				GenClass->GetDefaultObject(true);
			}
		}

		NewBP->RemoveFromRoot();
		Package->RemoveFromRoot();
	}

	// AGENT_POLICY §6: BP must be openable in editor. Save explicitly so
	// the .uasset on disk has a fully-baked CDO — without this the BP lives
	// only in memory and shows "invalid class" on the next UE start.
	//
	// CRITICAL: only save when the CDO is fully constructible. SaveLoadedAsset
	// triggers validation + thumbnail generation which instantiates the CDO; a
	// half-baked class (null ClassConstructor/ClassWithin) asserts at
	// UObjectGlobals.cpp:3396 and crashes the editor. Skip rather than crash.
	{
		UClass* GenCls = NewBP->GeneratedClass;
		const bool bConstructible = GenCls && GenCls->IsValidLowLevel()
			&& GenCls->ClassConstructor != nullptr
			&& GenCls->ClassWithin != nullptr
			&& GenCls->GetDefaultObject(false) != nullptr;
		if (bConstructible && !(NwiroBlueprintBatch::Active && NwiroBlueprintBatch::Active->bCreatingTarget))
		{
			UEditorAssetLibrary::SaveLoadedAsset(NewBP, false);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), NewBP->GetPathName());
	Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());

	// nextSteps: tell the agent what this empty BP still needs. Agents often
	// stop after create_blueprint thinking the asset alone fulfills the
	// user's intent — it doesn't. These hints are generic per parent class
	// type, not scenario-specific. Helps every prompt that creates a BP.
	TArray<TSharedPtr<FJsonValue>> NextSteps;
	auto AddStep = [&NextSteps](const FString& S)
	{
		NextSteps.Add(MakeShareable(new FJsonValueString(S)));
	};
	const FString PCN = ParentClass->GetName();
	const bool bIsActor = ParentClass->IsChildOf(AActor::StaticClass());
	const bool bIsPawn = ParentClass->IsChildOf(APawn::StaticClass());
	const bool bIsCharacter = ParentClass->IsChildOf(ACharacter::StaticClass());
	const bool bIsGameMode = ParentClass->IsChildOf(AGameModeBase::StaticClass());
	const bool bIsWidget = PCN == TEXT("UserWidget") || ParentClass->GetName().Contains(TEXT("UserWidget"));

	AddStep(TEXT("This blueprint was created EMPTY (no components, no variables, no graph nodes). It will not do anything yet."));
	if (bIsCharacter)
	{
		AddStep(TEXT("As a Character: typical setup needs a Camera component, gameplay variables (Health, Speed, etc.), and event-graph wiring (BeginPlay, input bindings). Use edit_blueprint with add_components/add_variables/add_nodes."));
	}
	else if (bIsPawn)
	{
		AddStep(TEXT("As a Pawn: typical setup needs a visual mesh component, a Camera or SpringArm, and input handling. Use edit_blueprint."));
	}
	else if (bIsActor)
	{
		AddStep(TEXT("As an Actor: add mesh/collision components and any required variables via edit_blueprint."));
	}
	else if (bIsGameMode)
	{
		AddStep(TEXT("As a GameMode: set DefaultPawnClass via set_cdo_property, then call set_game_mode to activate it in the world."));
	}
	else if (bIsWidget)
	{
		AddStep(TEXT("As a UserWidget: add UI elements via add_widget (TextBlock, Button, ComboBox, CheckBox, Slider, Image, etc.). For UI logic, use edit_blueprint with the EventGraph or function graphs."));
	}
	if (bIsActor && !bIsGameMode)
	{
		AddStep(FString::Printf(TEXT("WHEN COMPLETE: call spawn_actor with blueprint='%s' to place an instance in the level. The BP sitting in the content browser alone does not appear in-game."), *NewBP->GetPathName()));
	}
	AddStep(TEXT("VERIFY before stopping: call read_blueprint with this path and check that components/variables/event-nodes are present. An empty BP means the user's request is not satisfied."));
	Result->SetArrayField(TEXT("nextSteps"), NextSteps);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroBP, Log, TEXT("Created blueprint: %s (parent: %s)"), *Name, *ParentClass->GetName());
	return Out;
}

// ============================================================
// CLASS RESOLUTION (static method)
// ============================================================

UClass* FNwiroBlueprintTools::FindClassByName(const FString& Name)
{
	if (Name.IsEmpty()) return nullptr;

	// Asset path of a user BP — load it and return its generated class.
	if (Name.StartsWith(TEXT("/")) || Name.Contains(TEXT("/")))
	{
		FString TryPath = Name;
		if (TryPath.EndsWith(TEXT("_C"))) TryPath = TryPath.LeftChop(2);
		if (!TryPath.Contains(TEXT(".")))
		{
			int32 Slash; TryPath.FindLastChar('/', Slash);
			TryPath += TEXT(".") + TryPath.RightChop(Slash + 1);
		}
		if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *TryPath))
		{
			if (BP->GeneratedClass) return BP->GeneratedClass;
		}
		if (UClass* DirectC = LoadObject<UClass>(nullptr, *TryPath))
		{
			return DirectC;
		}
	}

	UClass* C = FindFirstObject<UClass>(*Name);
	if (C) return C;

	C = FindFirstObject<UClass>(*(TEXT("U") + Name));
	if (C) return C;
	C = FindFirstObject<UClass>(*(TEXT("A") + Name));
	if (C) return C;

	// Common aliases
	static TMap<FString, FString> Aliases = {
		{ TEXT("character"), TEXT("ACharacter") },
		{ TEXT("pawn"), TEXT("APawn") },
		{ TEXT("actor"), TEXT("AActor") },
		{ TEXT("playercontroller"), TEXT("APlayerController") },
		{ TEXT("gamemode"), TEXT("AGameModeBase") },
		{ TEXT("gamestate"), TEXT("AGameStateBase") },
		{ TEXT("playerstate"), TEXT("APlayerState") },
		{ TEXT("hud"), TEXT("AHUD") },
		{ TEXT("charactermovementcomponent"), TEXT("UCharacterMovementComponent") },
		{ TEXT("charactermovement"), TEXT("UCharacterMovementComponent") },
		{ TEXT("movementcomponent"), TEXT("UMovementComponent") },
		{ TEXT("scenecomponent"), TEXT("USceneComponent") },
		{ TEXT("actorcomponent"), TEXT("UActorComponent") },
		{ TEXT("staticmeshcomponent"), TEXT("UStaticMeshComponent") },
		{ TEXT("skeletalmeshcomponent"), TEXT("USkeletalMeshComponent") },
		{ TEXT("capsulecomponent"), TEXT("UCapsuleComponent") },
		{ TEXT("springarmcomponent"), TEXT("USpringArmComponent") },
		{ TEXT("cameracomponent"), TEXT("UCameraComponent") },
		{ TEXT("widgetcomponent"), TEXT("UWidgetComponent") },
		{ TEXT("userwidget"), TEXT("UUserWidget") },
		{ TEXT("widget"), TEXT("UWidget") },
		{ TEXT("progressbar"), TEXT("UProgressBar") },
		{ TEXT("textblock"), TEXT("UTextBlock") },
		{ TEXT("audiocomponent"), TEXT("UAudioComponent") },
		{ TEXT("pointlightcomponent"), TEXT("UPointLightComponent") },
		{ TEXT("spotlightcomponent"), TEXT("USpotLightComponent") },
		{ TEXT("particlesystemcomponent"), TEXT("UParticleSystemComponent") },
		{ TEXT("niagaracomponent"), TEXT("UNiagaraComponent") },
		{ TEXT("boxcollision"), TEXT("UBoxComponent") },
		{ TEXT("spherecollision"), TEXT("USphereComponent") },
		{ TEXT("arrowcomponent"), TEXT("UArrowComponent") },
		{ TEXT("enhancedinputlocalplayersubsystem"), TEXT("UEnhancedInputLocalPlayerSubsystem") },
		{ TEXT("enhancedinputsubsystem"), TEXT("UEnhancedInputLocalPlayerSubsystem") },
		{ TEXT("inputsubsystem"), TEXT("UEnhancedInputLocalPlayerSubsystem") },
	};

	if (const FString* Alias = Aliases.Find(Name.ToLower()))
	{
		C = FindFirstObject<UClass>(**Alias);
		if (C) return C;
	}

	// Try with Component suffix
	C = FindFirstObject<UClass>(*(Name + TEXT("Component")));
	if (C) return C;
	C = FindFirstObject<UClass>(*(TEXT("U") + Name + TEXT("Component")));
	if (C) return C;

	// Force-load engine class if not yet in memory
	C = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Name));
	if (C) return C;
	C = StaticLoadClass(UObject::StaticClass(), nullptr, *Name);
	if (C) return C;

	return nullptr;
}

// ============================================================
// RENAME VARIABLES
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoRenameVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Renamed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString OldName = Obj->GetStringField(TEXT("old"));
		FString NewName = Obj->GetStringField(TEXT("new"));
		if (OldName.IsEmpty() || NewName.IsEmpty()) continue;

		FBlueprintEditorUtils::RenameMemberVariable(BP, FName(*OldName), FName(*NewName));
		Renamed++;
	}
	return FNwiroBPResult::Ok(FString::Printf(TEXT("Renamed %d variable(s)"), Renamed));
}

// ============================================================
// SET COMPONENT PROPERTIES
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoSetComponentProperties(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	if (!BP->SimpleConstructionScript) return FNwiroBPResult::Fail(TEXT("No SCS"));

	int32 Set = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		if (NwiroBlueprintBatch::StopCommit(Errors)) break;
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString CompName = Obj->GetStringField(TEXT("component"));
		FString PropName = Obj->GetStringField(TEXT("property"));
		FString Value = Obj->GetStringField(TEXT("value"));

		if (CompName.IsEmpty() || PropName.IsEmpty()) continue;

		// Find the component template
		UActorComponent* CompTemplate = nullptr;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
			{
				CompTemplate = Node->ComponentTemplate;
				break;
			}
		}

		if (!CompTemplate)
		{
			Errors.Add(FString::Printf(TEXT("Component not found: %s"), *CompName));
			continue;
		}

		if (ApplyComponentProperty(CompTemplate, PropName, Value, CompName, &Errors, nullptr))
			Set++;
	}

	NwiroBlueprintBatch::RecordErrors(Errors);
	FString Msg = FString::Printf(TEXT("Set %d component property(ies)"), Set);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	return Set > 0 || Errors.Num() == 0 ? FNwiroBPResult::Ok(Msg) : FNwiroBPResult::Fail(Msg);
}

// ============================================================
// REMOVE FUNCTIONS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoRemoveFunctions(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString FuncName;
		if (Item->Type == EJson::String) FuncName = Item->AsString();
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid()) FuncName = Item->AsObject()->GetStringField(TEXT("name"));

		if (!FuncName.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveGraph(BP, FindGraph(BP, FuncName));
			Removed++;
		}
	}
	return FNwiroBPResult::Ok(FString::Printf(TEXT("Removed %d function(s)"), Removed));
}

// ============================================================
// ADD EVENT DISPATCHERS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoAddEventDispatchers(
	UBlueprint* BP,
	const TArray<TSharedPtr<FJsonValue>>& Items,
	TArray<FNwiroDispatcherMutation>* OutMutations)
{
	using namespace NwiroDispatcherPrivate;
	TArray<FNwiroDispatcherMutation> Mutations;
	auto Publish = [&]()
	{
		if (OutMutations) *OutMutations = Mutations;
	};

	FString PreflightMessage;
	if (!PrevalidateBatch(BP, Items, Mutations, PreflightMessage))
	{
		Publish();
		return FNwiroBPResult::Fail(PreflightMessage);
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	FNwiroDispatcherMutation* FailedMutation = nullptr;
	for (FNwiroDispatcherMutation& Mutation : Mutations)
	{
		// >>> NWIRO_PARITY BEGIN: dispatcher-member-type
		FEdGraphPinType DelegateType;
		DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		// <<< NWIRO_PARITY END: dispatcher-member-type
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, Mutation.Name, DelegateType))
		{
			SetMutationError(
				Mutation, TEXT("CREATION_FAILED"),
				TEXT("AddMemberVariable rejected the dispatcher"));
			FailedMutation = &Mutation;
			break;
		}
		Mutation.bMemberCreated = true;

		Mutation.Graph = FBlueprintEditorUtils::CreateNewGraph(
			BP, Mutation.Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!Mutation.Graph)
		{
			SetMutationError(
				Mutation, TEXT("CREATION_FAILED"),
				TEXT("CreateNewGraph failed for the dispatcher signature"));
			FailedMutation = &Mutation;
			break;
		}

		UEdGraph* Graph = Mutation.Graph.Get();
		// >>> NWIRO_PARITY BEGIN: dispatcher-signature
		Graph->bEditable = false;
		Schema->CreateDefaultNodesForGraph(*Graph);
		Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
		Schema->AddExtraFunctionFlags(
			Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
		Schema->MarkFunctionEntryAsEditable(Graph, true);
		// <<< NWIRO_PARITY END: dispatcher-signature

		UK2Node_FunctionEntry* Entry = FindOnlyEntry(Graph);
		if (!Entry)
		{
			SetMutationError(
				Mutation, TEXT("CREATION_FAILED"),
				TEXT("signature graph did not create exactly one FunctionEntry"));
			FailedMutation = &Mutation;
			break;
		}
		for (const FNwiroDispatcherParamSpec& Param : Mutation.Params)
		{
			FText PinError;
			if (!Entry->CanCreateUserDefinedPin(Param.Type, EGPD_Output, PinError)
				|| !Entry->CreateUserDefinedPin(
					Param.Name, Param.Type, EGPD_Output, false))
			{
				SetMutationError(
					Mutation, TEXT("CREATION_FAILED"),
					FString::Printf(
						TEXT("failed to create parameter '%s': %s"),
						*Param.Name.ToString(), *PinError.ToString()));
				FailedMutation = &Mutation;
				break;
			}
		}
		if (FailedMutation) break;

		// >>> NWIRO_PARITY BEGIN: dispatcher-register-signature
		BP->DelegateSignatureGraphs.Add(Graph);
		// <<< NWIRO_PARITY END: dispatcher-register-signature
		Mutation.bSignatureGraphPresent = true;
	}

	if (FailedMutation)
	{
		FString RollbackError;
		const bool bRollbackVerified =
			RollbackMutations(BP, Mutations, RollbackError);
		if (!bRollbackVerified)
		{
			FailedMutation->Error += TEXT("; ") + RollbackError;
		}
		Publish();
		return FNwiroBPResult::Fail(bRollbackVerified
			? FString::Printf(
				TEXT("Dispatcher batch creation failed and was rolled back: %s"),
				*FailedMutation->Error)
			: FString::Printf(
				TEXT("Dispatcher batch creation failed and rollback could not be verified: %s"),
				*FailedMutation->Error));
	}

	// >>> NWIRO_PARITY BEGIN: dispatcher-structural-refresh
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	// <<< NWIRO_PARITY END: dispatcher-structural-refresh
	for (FNwiroDispatcherMutation& Mutation : Mutations)
	{
		FString ValidationError;
		if (!ValidateDispatcherInvariant(
			BP, Mutation, BP->SkeletonGeneratedClass, ValidationError))
		{
			SetMutationError(
				Mutation, TEXT("SKELETON_SIGNATURE_INVALID"), ValidationError);
			FString RollbackError;
			const bool bRollbackVerified =
				RollbackMutations(BP, Mutations, RollbackError);
			if (!bRollbackVerified)
			{
				Mutation.Error += TEXT("; ") + RollbackError;
			}
			Publish();
			return FNwiroBPResult::Fail(bRollbackVerified
				? FString::Printf(
					TEXT("Dispatcher skeleton validation failed and the batch was rolled back: %s"),
					*Mutation.Error)
				: FString::Printf(
					TEXT("Dispatcher skeleton validation failed and rollback could not be verified: %s"),
					*Mutation.Error));
		}
		Mutation.bSkeletonSignatureValid = true;
		Mutation.bCreated = true;
		UE_LOG(LogNwiroBP, Log, TEXT("Added event dispatcher with signature: %s"),
			*Mutation.Name.ToString());
	}

	Publish();
	return FNwiroBPResult::Ok(FString::Printf(
		TEXT("Added %d event dispatcher(s) with validated skeleton signatures"),
		Mutations.Num()));
}

// ============================================================
// ADD/REMOVE INTERFACES
// ============================================================

// See NwiroIKBlueprintTools.cpp ResolveInterfaceClass for the shared docstring.
// Handles C++ UInterface class names AND Blueprint Interface asset paths.
static UClass* ResolveInterfaceClass(const FString& In)
{
	if (In.IsEmpty()) return nullptr;
	if (UClass* C = FNwiroBlueprintTools::FindClassByName(In))
	{
		if (C->IsChildOf(UInterface::StaticClass())) return C;
	}
	if (UClass* C = FindFirstObject<UClass>(*(TEXT("U") + In)))
	{
		if (C->IsChildOf(UInterface::StaticClass())) return C;
	}
	if (UClass* C = FindFirstObject<UClass>(*(TEXT("I") + In)))
	{
		if (C->IsChildOf(UInterface::StaticClass())) return C;
	}
	if (In.StartsWith(TEXT("/")))
	{
		FString PackagePath = In;
		int32 Dot; if (PackagePath.FindLastChar('.', Dot)) PackagePath = PackagePath.Left(Dot);
		UBlueprint* BPI = LoadObject<UBlueprint>(nullptr, *PackagePath);
		if (BPI && BPI->BlueprintType == BPTYPE_Interface && BPI->GeneratedClass)
		{
			return BPI->GeneratedClass;
		}
	}
	return nullptr;
}

FNwiroBPResult FNwiroBlueprintTools::DoAddInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Added = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		if (NwiroBlueprintBatch::StopCommit(Errors)) break;
		FString IfaceName;
		if (Item->Type == EJson::String) IfaceName = Item->AsString();
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid()) IfaceName = Item->AsObject()->GetStringField(TEXT("name"));
		if (IfaceName.IsEmpty()) continue;

		UClass* IfaceClass = ResolveInterfaceClass(IfaceName);
		if (!IfaceClass)
		{
			Errors.Add(FString::Printf(TEXT("Interface not found: %s (accepts C++ interface class name like 'DamageInterface', or Blueprint Interface asset path like '/Game/BPI/BPI_Foo')"), *IfaceName));
			continue;
		}

		FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(IfaceClass->GetPathName()));
		Added++;
	}

	NwiroBlueprintBatch::RecordErrors(Errors);
	FString Msg = FString::Printf(TEXT("Added %d interface(s)"), Added);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	return Added > 0 || Errors.Num() == 0 ? FNwiroBPResult::Ok(Msg) : FNwiroBPResult::Fail(Msg);
}

FNwiroBPResult FNwiroBlueprintTools::DoRemoveInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	TArray<UClass*> Classes;
	FString Error;
	if (!PrepareInterfaceRemoval(BP, Items, Classes, Error)) return FNwiroBPResult::Fail(Error);
	FNwiroUnitLedger Ledger;
	for (int32 Index = 0; Index < Classes.Num(); ++Index)
	{
		UClass* Class = Classes[Index];
		FBlueprintEditorUtils::RemoveInterface(BP, Class->GetClassPathName());
		if (BP->ImplementedInterfaces.ContainsByPredicate(
			[Class](const FBPInterfaceDescription& Desc) { return Desc.Interface == Class; }))
		{
			FNwiroBPResult Result = FNwiroBPResult::Fail(TEXT("Interface removal did not remove the requested membership"));
			Result.Level2 = Ledger.Build(TEXT("remove_interfaces"), Items.Num());
			return Result;
		}
		Ledger.Removed.Add(FNwiroUnitLedger::MakeUnit(Index, TEXT("remove_interfaces"), Class->GetPathName(), true));
	}
	FNwiroBPResult Result = FNwiroBPResult::Ok(FString::Printf(TEXT("Removed %d interface(s)"), Ledger.Removed.Num()));
	Result.Level2 = Ledger.Build(TEXT("remove_interfaces"), Items.Num());
	return Result;
}

// ============================================================
// REPARENT
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoReparent(UBlueprint* BP, const FString& NewParentClass)
{
	UClass* NewParent = FindClassByName(NewParentClass);
	if (!NewParent)
	{
		return FNwiroBPResult::Fail(FString::Printf(TEXT("Parent class not found: %s"), *NewParentClass));
	}

	BP->ParentClass = NewParent;
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	UE_LOG(LogNwiroBP, Log, TEXT("Reparented to: %s"), *NewParent->GetName());
	return FNwiroBPResult::Ok(FString::Printf(TEXT("Reparented to %s [resolved] path:%s"), *NewParent->GetName(), *NewParent->GetPathName()));
}

// ============================================================
// REMOVE NODES
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoRemoveNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return FNwiroBPResult::Fail(TEXT("Graph not found"));

	int32 Removed = 0;
	TArray<FString> Refusals;
	// A fully-refused batch used to return Ok with the refusal only in prose, so a caller had
	// no machine-readable trace that nothing was deleted. The verdict deliberately stays Ok
	// (see below); the ledger is what makes the refusal readable.
	FNwiroUnitLedger Ledger;
	int32 Unit = -1;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		++Unit;
		FString Ref;
		if (Item->Type == EJson::String) Ref = Item->AsString();
		else if (Item->Type == EJson::Object) Ref = Item->AsObject()->GetStringField(TEXT("ref"));
		if (Ref.IsEmpty())
		{
			Ledger.AddError(Unit, TEXT("remove_nodes"), TEXT("MISSING_FIELD"), TEXT("ref"), TEXT(""),
				TEXT("Entry has no node ref"));
			continue;
		}

		FString Ambiguity;
		UEdGraphNode* Node = FindNodeByRef(Graph, Ref, &Ambiguity);
		if (Node)
		{
			Ledger.Removed.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("remove_nodes"),
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), /*bAsRequested*/ true));
			FBlueprintEditorUtils::RemoveNode(BP, Node);
			Removed++;
		}
		else if (!Ambiguity.IsEmpty())
		{
			Refusals.Add(Ambiguity);
			Ledger.AddError(Unit, TEXT("remove_nodes"), TEXT("AMBIGUOUS_REF"), TEXT("ref"), Ref, Ambiguity);
		}
		else
		{
			Ledger.AddError(Unit, TEXT("remove_nodes"), TEXT("NODE_NOT_FOUND"), TEXT("ref"), Ref,
				FString::Printf(TEXT("Node not found: %s"), *Ref));
		}
	}

	const TSet<int32> AccountedUnits = Ledger.AccountedUnits();
	for (int32 UnitIndex = 0; UnitIndex < Items.Num(); ++UnitIndex)
	{
		if (AccountedUnits.Contains(UnitIndex)) continue;
		Ledger.Skipped.Add(FNwiroUnitLedger::MakeUnit(UnitIndex, TEXT("remove_nodes"),
			TEXT("(malformed request item)"), /*bAsRequested*/ false));
	}

	// Reported but deliberately NOT a verdict change: delete_node gates
	// MarkBlueprintAsModified + CompileBlueprint on bSuccess, so a Fail would leave a
	// really-removed node uncompiled. Verdicts move for all four handlers together.
	FString Msg = FString::Printf(TEXT("Removed %d node(s)"), Removed);
	if (Refusals.Num() > 0) Msg += TEXT(". Refused: ") + FString::Join(Refusals, TEXT("; "));
	FNwiroBPResult Result = FNwiroBPResult::Ok(Msg);
	Result.Level2 = Ledger.Build(TEXT("remove_nodes"), Items.Num());
	return Result;
}

// ============================================================
// BREAK CONNECTIONS
// ============================================================

FNwiroBPResult FNwiroBlueprintTools::DoBreakConnections(UBlueprint* BP, const FString& GraphName,
    const TArray<TSharedPtr<FJsonValue>>& Items)
{
    UEdGraph* Graph = FindGraph(BP, GraphName);
    if (!Graph) return FNwiroBPResult::Fail(TEXT("Graph not found: ") + GraphName);
    struct FBreak { TWeakObjectPtr<UEdGraphNode> Node; FGuid PinId; bool bWholeNode; };
    TStrongObjectPtr<UEdGraph> KeepGraph(Graph);
    TArray<FBreak> Plan;
    TArray<FString> Errors;
    FNwiroUnitLedger Ledger;
    // All identities resolve before the first break, including explicit empty-pin intent.
    for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
    {
        const auto& Value = Items[Unit];
        const auto Item = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        FString Ref, Name, Error;
        if (!Item.IsValid() || !(Item->TryGetStringField(TEXT("ref"), Ref) || Item->TryGetStringField(TEXT("node"), Ref))
            || Ref.IsEmpty() || !Item->TryGetStringField(TEXT("pin"), Name))
            Error = TEXT("break_connections requires a node and explicit string pin; empty pin means whole node");
        UEdGraphNode* Node = Error.IsEmpty() ? FindNodeByRef(Graph, Ref, &Error) : nullptr;
        if (!Node && Error.IsEmpty()) Error = TEXT("Node not found: ") + Ref;
        UEdGraphPin* Pin = Node && !Name.IsEmpty() ? FindPin(Node, Name, EGPD_MAX, &Error) : nullptr;
        if (Node && !Name.IsEmpty() && !Pin && Error.IsEmpty()) Error = TEXT("Pin not found: ") + Name;
        if (!Error.IsEmpty()) { Errors.Add(Error); continue; }
        Plan.Add({Node, Pin ? Pin->PinId : FGuid(), Name.IsEmpty()});
        if (NwiroBlueprintBatch::Active && NwiroBlueprintBatch::Active->bStaging)
        {
            Item->SetStringField(TEXT("ref"), NwiroBlueprintBatch::PlannedNodeRef(Node, Ref));
            Item->SetStringField(TEXT("pin"), Pin ? Pin->PinName.ToString() : FString());
        }
    }
    if (!Errors.IsEmpty())
    {
        NwiroBlueprintBatch::RecordErrors(Errors);
        return FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; ")));
    }
    for (int32 Unit = 0; Unit < Plan.Num(); ++Unit)
    {
        const FBreak& Break = Plan[Unit];
        UEdGraphNode* Node = Break.Node.Get();
        UEdGraphPin* Pin = Node && !Break.bWholeNode ? Node->FindPinById(Break.PinId) : nullptr;
        if (!Node || (!Break.bWholeNode && !Pin))
        { Errors.Add(TEXT("Break plan identity changed during commit; result may be partial")); break; }
        TStrongObjectPtr<UEdGraphNode> KeepNode(Node);
        const FString CommittedNodeGuid = Node->NodeGuid.ToString();
        if (Break.bWholeNode)
        {
            Node->BreakAllNodeLinks();
#if WITH_DEV_AUTOMATION_TESTS
            if (NwiroBlueprintBatch::AfterWholeNodeBreak) NwiroBlueprintBatch::AfterWholeNodeBreak(KeepNode.Get());
#endif
        }
        else Pin->BreakAllPinLinks(); // default bNotifyNodes=false
        Node = Break.Node.Get();
        bool Verified = Node && Graph->Nodes.Contains(Node);
        if (Verified && Break.bWholeNode)
        {
            for (const UEdGraphPin* Current : Node->Pins)
                if (Current && Current->LinkedTo.Num()) Verified = false;
        }
        else if (Verified)
        {
            Pin = Node->FindPinById(Break.PinId);
            Verified = Pin && Pin->LinkedTo.IsEmpty();
        }
        if (!Verified)
        {
            const FString Error = TEXT("Break callback did not preserve the requested unlinked owner/pin; commit may be partial");
            Errors.Add(Error);
            Ledger.AddError(Unit, TEXT("break_connections"), TEXT("BREAK_NOT_VERIFIED"), TEXT("pin"), CommittedNodeGuid, Error);
            break;
        }
        Ledger.Modified.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("break_connections"), CommittedNodeGuid, true));
    }
    NwiroBlueprintBatch::RecordErrors(Errors);
    FNwiroBPResult Result = Errors.IsEmpty() ? FNwiroBPResult::Ok(TEXT("Applied all admitted break requests"))
        : FNwiroBPResult::Fail(FString::Join(Errors, TEXT("; ")));
    const TSet<int32> Accounted = Ledger.AccountedUnits();
    for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
        if (!Accounted.Contains(Unit)) Ledger.Skipped.Add(FNwiroUnitLedger::MakeUnit(Unit, TEXT("break_connections"), TEXT("not attempted after commit interruption"), false));
    Result.Level2 = Ledger.Build(TEXT("break_connections"), Items.Num());
    return Result;
}

// ============================================================
// SERIALIZATION HELPERS
// ============================================================

TSharedPtr<FJsonObject> FNwiroBlueprintTools::SerializeVariable(UBlueprint* BP, const FName& VarName)
{
	int32 Idx = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName);
	if (Idx == INDEX_NONE) return nullptr;

	const FBPVariableDescription& Var = BP->NewVariables[Idx];

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("name"), VarName.ToString());
	Obj->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
	Obj->SetStringField(TEXT("declaredDefaultValue"), Var.DefaultValue);
	Obj->SetStringField(TEXT("defaultValue"), Var.DefaultValue);
	Obj->SetStringField(TEXT("defaultValueSource"), TEXT("variableDeclaration"));
	Obj->SetStringField(TEXT("defaultValueStatus"), TEXT("compiled_default_unavailable"));
	Obj->SetStringField(TEXT("blueprintStatus"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(BP->Status));
	// Defaults edited through the Details panel live on the generated CDO, while
	// NewVariables.DefaultValue may still be empty. Observe only the existing CDO;
	// a read must not create one or compile a dirty Blueprint to obtain it.
	UClass* Generated = BP->GeneratedClass;
	UObject* CDO = Generated ? Generated->GetDefaultObject(false) : nullptr;
	FProperty* Property = CDO ? FindFProperty<FProperty>(Generated, VarName) : nullptr;
	FEdGraphPinType ReflectedType;
	if (Property && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, ReflectedType)
		&& ReflectedType == Var.VarType)
	{
		FString DefaultValue;
		Property->ExportTextItem_Direct(DefaultValue, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
		Obj->SetStringField(TEXT("defaultValue"), DefaultValue);
		Obj->SetStringField(TEXT("defaultValueSource"), TEXT("generatedClassCDO"));
		const bool bCompiled = BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings;
		Obj->SetStringField(TEXT("defaultValueStatus"), bCompiled ? TEXT("compiled") : TEXT("possibly_stale"));
	}
	Obj->SetStringField(TEXT("category"), Var.Category.ToString());

	// Flags
	Obj->SetBoolField(TEXT("replicated"), (Var.PropertyFlags & CPF_Net) != 0);
	Obj->SetBoolField(TEXT("saveGame"), (Var.PropertyFlags & CPF_SaveGame) != 0);

	return Obj;
}

TSharedPtr<FJsonObject> FNwiroBlueprintTools::SerializeComponent(const UActorComponent* Comp, const FName& VarName)
{
	if (!Comp) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("name"), VarName.ToString());
	Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

	if (const USceneComponent* Scene = Cast<USceneComponent>(Comp))
	{
		TSharedPtr<FJsonObject> Loc = MakeShareable(new FJsonObject());
		Loc->SetNumberField(TEXT("x"), Scene->GetRelativeLocation().X);
		Loc->SetNumberField(TEXT("y"), Scene->GetRelativeLocation().Y);
		Loc->SetNumberField(TEXT("z"), Scene->GetRelativeLocation().Z);
		Obj->SetObjectField(TEXT("location"), Loc);

		FVector Scale = Scene->GetRelativeScale3D();
		if (Scale != FVector::OneVector)
		{
			Obj->SetStringField(TEXT("scale"), FString::Printf(TEXT("(%.2f,%.2f,%.2f)"), Scale.X, Scale.Y, Scale.Z));
		}

		Obj->SetStringField(TEXT("mobility"), Scene->Mobility == EComponentMobility::Static ? TEXT("Static") :
			Scene->Mobility == EComponentMobility::Stationary ? TEXT("Stationary") : TEXT("Movable"));
	}

	// StaticMeshComponent details
	if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp))
	{
		if (SMC->GetStaticMesh())
			Obj->SetStringField(TEXT("staticMesh"), SMC->GetStaticMesh()->GetPathName());
		else
			Obj->SetStringField(TEXT("staticMesh"), TEXT("None"));
	}

	// Physics info
	if (const UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
	{
		Obj->SetBoolField(TEXT("simulatePhysics"), Prim->BodyInstance.bSimulatePhysics);
		Obj->SetBoolField(TEXT("generateOverlapEvents"), Prim->GetGenerateOverlapEvents());
	}

	return Obj;
}

// Wiring-completeness scan: report exec-pin gaps so the agent can self-verify a graph is
// fully wired. BLOCKING = a node whose INPUT exec pin is unlinked (unreachable; this is how an
// unwired Branch arm surfaces, on the downstream node). ADVISORY = an unlinked OUTPUT exec pin
// (execution dead-ends; usually a missing downstream node, sometimes a legit terminal). Data
// pins are deliberately NOT classified in v1 (too many optional/auto-resolved inputs would be
// false positives). Council-reviewed: narrow + high-precision so the signal stays trustworthy.
static void NwiroCollectDangling(UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutBlocking, TArray<TSharedPtr<FJsonValue>>& OutAdvisory)
{
	if (!Graph) return;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const FString Title = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden || Pin->bAdvancedView) continue;
			if (Pin->LinkedTo.Num() > 0) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue; // v1: exec pins only
			TSharedPtr<FJsonObject> E = MakeShareable(new FJsonObject());
			E->SetStringField(TEXT("node"), Title);
			E->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			if (Pin->Direction == EGPD_Input)
			{
				E->SetStringField(TEXT("type"), TEXT("exec-in"));
				E->SetStringField(TEXT("hint"), TEXT("unreachable: nothing executes into this pin; wire an exec output into it"));
				OutBlocking.Add(MakeShareable(new FJsonValueObject(E.ToSharedRef())));
			}
			else
			{
				E->SetStringField(TEXT("type"), TEXT("exec-out"));
				E->SetStringField(TEXT("hint"), TEXT("execution dead-ends here; wire the next node or confirm this is intentionally terminal"));
				OutAdvisory.Add(MakeShareable(new FJsonValueObject(E.ToSharedRef())));
			}
		}
	}
}

TSharedPtr<FJsonObject> FNwiroBlueprintTools::SerializeGraph(UEdGraph* Graph, bool bIncludeNodes)
{
	if (!Graph) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("name"), Graph->GetName());
	Obj->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());

	if (bIncludeNodes)
	{
		TArray<TSharedPtr<FJsonValue>> NodeArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			TSharedPtr<FJsonObject> NObj = SerializeNode(Node);
			if (NObj.IsValid())
			{
				NodeArr.Add(MakeShareable(new FJsonValueObject(NObj.ToSharedRef())));
			}
		}
		Obj->SetArrayField(TEXT("nodes"), NodeArr);

		// Wiring-completeness feedback so the agent reads its own gaps back and self-corrects.
		TArray<TSharedPtr<FJsonValue>> Blocking, Advisory;
		NwiroCollectDangling(Graph, Blocking, Advisory);
		Obj->SetArrayField(TEXT("blocking"), Blocking);
		Obj->SetArrayField(TEXT("advisory"), Advisory);
		Obj->SetNumberField(TEXT("blockingCount"), Blocking.Num());
		Obj->SetNumberField(TEXT("advisoryCount"), Advisory.Num());
	}

	return Obj;
}

TSharedPtr<FJsonObject> FNwiroBlueprintTools::SerializeNode(UEdGraphNode* Node)
{
	if (!Node) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Obj->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
	// `ref` is what connect_pins expects in the "from"/"to" strings before the
	// dot. Look it up in NodeRefs (the auto-stored secondary key, possibly
	// suffixed for uniqueness) so each node in the JSON output gets a unique
	// handle even when multiple nodes share the same title.
	{
		FString RefValue;
		for (const auto& Pair : NodeRefs)
		{
			if (Pair.Value.NodeGuid == Node->NodeGuid)
			{
				// The stored bare ref, never Pair.Key — the key carries the owning
				// Blueprint's path and is not a value a caller can use.
				const FString& Candidate = Pair.Value.Ref;
				if (RefValue.IsEmpty() || (RefValue.StartsWith(TEXT("node_")) && !Candidate.StartsWith(TEXT("node_"))))
				{
					RefValue = Candidate;
				}
			}
		}
		if (RefValue.IsEmpty())
		{
			RefValue = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
		}
		Obj->SetStringField(TEXT("ref"), RefValue);
	}
	Obj->SetNumberField(TEXT("x"), Node->NodePosX);
	Obj->SetNumberField(TEXT("y"), Node->NodePosY);

	// Serialize pins
	TArray<TSharedPtr<FJsonValue>> PinArr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;

		TSharedPtr<FJsonObject> PObj = MakeShareable(new FJsonObject());
		PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PObj->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
		PObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		PObj->SetStringField(TEXT("defaultValue"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text
			? Pin->DefaultTextValue.ToString() : Pin->GetDefaultAsString());
		PObj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);

		// Show what this pin is connected to
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LinkedArr;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					FString ConnStr = FString::Printf(TEXT("%s.%s"),
						*Linked->GetOwningNode()->NodeGuid.ToString(),
						*Linked->PinName.ToString());
					LinkedArr.Add(MakeShareable(new FJsonValueString(ConnStr)));
				}
			}
			PObj->SetArrayField(TEXT("connectedTo"), LinkedArr);
		}

		// Show DefaultObject for object reference pins
		if (Pin->DefaultObject)
		{
			PObj->SetStringField(TEXT("defaultObject"), Pin->DefaultObject->GetPathName());
		}

		if (!Pin->PinFriendlyName.IsEmpty())
		{
			PObj->SetStringField(TEXT("friendlyName"), Pin->PinFriendlyName.ToString());
		}

		PinArr.Add(MakeShareable(new FJsonValueObject(PObj.ToSharedRef())));
	}
	Obj->SetArrayField(TEXT("pins"), PinArr);

	return Obj;
}

FString FNwiroBlueprintTools::PinTypeToString(const FEdGraphPinType& PinType)
{
	FString Result;

	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) Result = TEXT("Boolean");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte) Result = TEXT("Byte");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int) Result = TEXT("Int");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) Result = TEXT("Int64");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real) Result = PinType.PinSubCategory.ToString();
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String) Result = TEXT("String");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name) Result = TEXT("Name");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text) Result = TEXT("Text");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) Result = TEXT("Exec");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		Result = TEXT("Object");
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += TEXT(":") + PinType.PinSubCategoryObject->GetName();
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		Result = TEXT("Struct");
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result = PinType.PinSubCategoryObject->GetName();
		}
	}
	else
	{
		Result = PinType.PinCategory.ToString();
	}

	// Container
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		Result = TEXT("Array<") + Result + TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		Result = TEXT("Set<") + Result + TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		Result = TEXT("Map<") + Result + TEXT(">");
	}

	return Result;
}

// ============================================================
// DELETE BLUEPRINT
// ============================================================

// ============================================================
// CLEAR GRAPH — remove all nodes from EventGraph
// ============================================================

FString FNwiroBlueprintTools::ClearGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	// Hard guard: clear_graph is destructive. The LLM kept calling it as a
	// "reset" reflex whenever connect_pins failed, wiping out work in progress.
	// Refuse the call unless it carries an explicit confirm flag — the system
	// prompt instructs the model to set this only when the USER literally
	// asked to start the graph over.
	FString Confirm;
	Cmd->TryGetStringField(TEXT("confirm"), Confirm);
	if (!Confirm.Equals(TEXT("user_requested"), ESearchCase::IgnoreCase))
	{
		return TEXT("{\"success\":false,\"error\":\"clear_graph refused: this is destructive. Read the connect_pins / edit_blueprint error message — it lists the available node refs and pin names so you can self-correct without wiping the graph. Only call clear_graph again with arguments.confirm=\\\"user_requested\\\" if the user literally said to start over.\"}");
	}

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	FString GraphName = Cmd->HasField(TEXT("graph")) ? Cmd->GetStringField(TEXT("graph")) : TEXT("EventGraph");

	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return TEXT("{\"success\":false,\"error\":\"Graph not found\"}");

	int32 Removed = 0;
	TArray<UEdGraphNode*> NodesToRemove;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		// Keep default event nodes (BeginPlay, Tick, etc.) — they have bCanRenameNode=false
		// Remove everything else
		if (!Node->CanUserDeleteNode()) continue;
		if (Node->IsA<UK2Node_FunctionEntry>()) continue;
		if (Node->IsA<UK2Node_FunctionResult>()) continue;
		if (Node->IsA<UK2Node_Event>()) continue;
		if (Node->IsA<UK2Node_Tunnel>()) continue;
		NodesToRemove.Add(Node);
	}

	for (UEdGraphNode* Node : NodesToRemove)
	{
		FBlueprintEditorUtils::RemoveNode(BP, Node);
		Removed++;
	}

	// Only THIS Blueprint's refs. Emptying the whole map here killed every other
	// Blueprint's bindings as a side effect of clearing one graph, which is the second
	// way a ref could die without being reused.
	const FString OwnerKey = NodeRefOwnerKey(Graph);
	for (auto It = NodeRefs.CreateIterator(); It; ++It)
	{
		if (It->Value.BlueprintPath == OwnerKey) It.RemoveCurrent();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	return FString::Printf(TEXT("{\"success\":true,\"removed\":%d,\"message\":\"Graph cleared and compiled\"}"), Removed);
}

FString FNwiroBlueprintTools::DeleteBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString AssetPath = Cmd->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty()) AssetPath = Cmd->GetStringField(TEXT("path"));
	if (AssetPath.IsEmpty()) AssetPath = Cmd->GetStringField(TEXT("name"));

	FString LoadError;
	if (UBlueprint* BP = LoadBP(AssetPath, &LoadError))
	{
		const FString FullPath = BP->GetPathName();
		const FString PackagePath = BP->GetPackage()->GetName();

		// Do not enter editor deletion with a half-constructed class. Refusing to
		// mutate is a failure, not a successful deletion or an implicit repair.
		const UClass* GC = BP->GeneratedClass;
		const bool bClassOK = GC && GC->ClassWithin && GC->ClassConstructor && GC->GetDefaultObject(false);
		if (!bClassOK)
		{
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint has no usable generated class/CDO; deletion was not attempted\",\"code\":\"blueprint_not_ready_for_deletion\",\"path\":\"%s\",\"deleted\":false,\"savedToDisk\":false}"), *FullPath);
		}

		if (UEditorAssetLibrary::DeleteAsset(PackagePath))
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\",\"kind\":\"Blueprint\"}"), *FullPath);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to delete blueprint: %s\"}"), *FullPath);
	}

	// A rejected Blueprint preload is terminal, including the generic delete fallback.
	if (!LoadError.IsEmpty()) return NwiroAssetLoadFailure(AssetPath, LoadError, TEXT("Blueprint"));

	if (UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath))
	{
		const FString FullPath = Asset->GetPathName();
		const FString PackagePath = Asset->GetPackage()->GetName();
		const FString Kind = Asset->GetClass()->GetName();
		if (UEditorAssetLibrary::DeleteAsset(PackagePath))
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\",\"kind\":\"%s\"}"), *FullPath, *Kind);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to delete %s: %s\"}"), *Kind, *FullPath);
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Asset not found: %s\"}"), *AssetPath);
}

// ============================================================
// DUPLICATE BLUEPRINT
// ============================================================

FString FNwiroBlueprintTools::DuplicateBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SourcePath = Cmd->GetStringField(TEXT("source"));
	if (SourcePath.IsEmpty()) SourcePath = Cmd->GetStringField(TEXT("assetPath"));
	FString NewName = Cmd->GetStringField(TEXT("newName"));
	FString DestPath = Cmd->GetStringField(TEXT("destinationPath"));

	UBlueprint* SourceBP = LoadBP(SourcePath);
	if (!SourceBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Source blueprint not found: %s\"}"), *SourcePath);

	if (DestPath.IsEmpty()) DestPath = FPaths::GetPath(SourceBP->GetPackage()->GetName());
	if (NewName.IsEmpty()) NewName = SourceBP->GetName() + TEXT("_Copy");

	FString FullDestPath = DestPath / NewName;
	FString SourcePkg = SourceBP->GetPackage()->GetName();

	if (UEditorAssetLibrary::DuplicateAsset(SourcePkg, FullDestPath))
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"), *NewName, *FullDestPath);

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to duplicate to: %s\"}"), *FullDestPath);
}

// ============================================================
// RENAME BLUEPRINT
// ============================================================

FString FNwiroBlueprintTools::RenameBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString AssetPath = Cmd->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty()) AssetPath = Cmd->GetStringField(TEXT("source"));
	FString NewName = Cmd->GetStringField(TEXT("newName"));

	UBlueprint* BP = LoadBP(AssetPath);
	if (!BP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *AssetPath);

	if (NewName.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"newName is required\"}");

	FString SourcePkg = BP->GetPackage()->GetName();
	FString DestPkg = FPaths::GetPath(SourcePkg) / NewName;

	if (UEditorAssetLibrary::RenameAsset(SourcePkg, DestPkg))
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"), *NewName, *DestPkg);

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to rename to: %s\"}"), *NewName);
}

// ============================================================
// DELETE NODE (standalone)
// ============================================================

FString FNwiroBlueprintTools::DeleteNode(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	FString GraphName = Cmd->GetStringField(TEXT("graph"));
	if (GraphName.IsEmpty()) GraphName = TEXT("EventGraph");

	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	// Build array from "refs" array or single "ref"
	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* RefsArr;
	if (Cmd->TryGetArrayField(TEXT("refs"), RefsArr))
	{
		for (const auto& V : *RefsArr)
		{
			TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
			Obj->SetStringField(TEXT("ref"), V->AsString());
			Items.Add(MakeShareable(new FJsonValueObject(Obj)));
		}
	}
	else if (Cmd->HasField(TEXT("ref")))
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("ref"), Cmd->GetStringField(TEXT("ref")));
		Items.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "DeleteNode", "AI: Delete Node"), BP);
	FNwiroBPResult R = DoRemoveNodes(BP, GraphName, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// CREATE FUNCTION GRAPH (standalone)
// ============================================================

FString FNwiroBlueprintTools::CreateFunctionGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	if (!IsInGameThread() || !GEditor)
		return TEXT("{\"success\":false,\"error\":\"create_function_graph requires the editor game thread\"}");

	TArray<FNwiroPreparedFunction> Prepared;
	FNwiroUnitLedger Preflight;
	FString BPName;
	if (!Cmd->HasTypedField<EJson::String>(TEXT("blueprint")) || !Cmd->TryGetStringField(TEXT("blueprint"), BPName) || BPName.IsEmpty())
		return NwiroFunctions::RejectedResponse(nullptr, BPName, Prepared, Preflight, TEXT("blueprint must be a nonempty string"));
	TArray<TSharedPtr<FJsonValue>> Items;
	if (Cmd->HasField(TEXT("functions")))
	{
		// Preserve the legacy batch precedence over top-level single-function fields.
		const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
		if (!Cmd->TryGetArrayField(TEXT("functions"), Batch))
			return NwiroFunctions::RejectedResponse(nullptr, BPName, Prepared, Preflight, TEXT("functions must be an array"));
		Items = *Batch;
	}
	else
	{
		const auto Item = MakeShared<FJsonObject>();
		for (const TCHAR* Key : { TEXT("name"), TEXT("inputs"), TEXT("outputs"), TEXT("pure"), TEXT("override"), TEXT("redefine") })
			if (Cmd->HasField(Key)) Item->SetField(Key, Cmd->TryGetField(Key));
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	if (!NwiroFunctions::Prepare(Items, Prepared, Preflight))
		return NwiroFunctions::RejectedResponse(nullptr, BPName, Prepared, Preflight, TEXT("function batch rejected before target loading; inspect operations.errors"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return NwiroFunctions::RejectedResponse(nullptr, BPName, Prepared, Preflight, TEXT("Blueprint not found: ") + BPName);
	if (!NwiroFunctions::ValidateTarget(BP, Prepared, Preflight))
		return NwiroFunctions::RejectedResponse(BP, BPName, Prepared, Preflight, TEXT("function batch rejected before mutation; inspect operations.errors"));

	// Modify belongs to the validated apply phase. Preserve GC protection through compile.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateFunctionGraph", "AI: Create Function Graph"));
	FGCScopeGuard GCGuard;
	FNwiroBPResult Applied = DoAddFunctions(BP, Prepared);
	if (!Applied.bSuccess) return NwiroFunctions::RejectedApplyResponse(BP, Prepared, Applied);
	const bool bMutated = LedgerAppliedAnything(Applied.Level2);
	bool bCompiled = false;
	bool bCompileSucceeded = true;
	TArray<TSharedPtr<FJsonValue>> CompileErrors;
	if (bMutated)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Results);
		bCompiled = true;
		bCompileSucceeded = Results.NumErrors == 0 && (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
			if (Message->GetSeverity() == EMessageSeverity::Error) CompileErrors.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
		if (!bCompileSucceeded && CompileErrors.IsEmpty())
			CompileErrors.Add(MakeShared<FJsonValueString>(TEXT("Blueprint did not reach a compiled status")));
	}
	const auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Applied.bSuccess && bCompileSucceeded);
	Result->SetStringField(TEXT("message"), Applied.Message);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetBoolField(TEXT("savedToDisk"), false);
	Result->SetBoolField(TEXT("mutationAttempted"), bMutated);
	Result->SetStringField(TEXT("compileStatus"), !bCompiled ? TEXT("not_needed") : bCompileSucceeded ? TEXT("success") : TEXT("error"));
	Result->SetStringField(TEXT("validationLevel"), !bCompiled ? TEXT("target") : bCompileSucceeded ? TEXT("generated") : TEXT("uncertain"));
	Result->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Applied.Level2) });
	Result->SetArrayField(TEXT("compileErrors"), CompileErrors);
	if (!bCompileSucceeded) Result->SetStringField(TEXT("error"), TEXT("function graphs were created but Blueprint compilation failed; inspect compileErrors"));
	TArray<TSharedPtr<FJsonValue>> Dirty;
	if (BP->GetOutermost()->IsDirty()) Dirty.Add(MakeShared<FJsonValueString>(BP->GetOutermost()->GetName()));
	Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
	Result->SetBoolField(TEXT("targetStateObserved"), true);
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

// ============================================================
// ADD INTERFACE (standalone)
// ============================================================

FString FNwiroBlueprintTools::AddInterface(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("interfaces"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("interface")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("interface")))));
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddInterface", "AI: Add Interface"), BP);
	FNwiroBPResult R = DoAddInterfaces(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// REMOVE INTERFACE (standalone)
// ============================================================

FString FNwiroBlueprintTools::RemoveInterface(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("interfaces"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("interface")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("interface")))));
	}

	TArray<UClass*> Prepared;
	FString Error;
	if (!PrepareInterfaceRemoval(BP, Items, Prepared, Error))
		return InterfaceRemovalResponse(BP, false, Error, 0);

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "RemoveInterface", "AI: Remove Interface"), BP);
	FNwiroBPResult R = DoRemoveInterfaces(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
	}

	const int32 Removed = R.Level2.IsValid() ? R.Level2->GetArrayField(TEXT("removed")).Num() : 0;
	return InterfaceRemovalResponse(BP, R.bSuccess, R.Message, Removed);
}

// ============================================================
// CREATE EVENT DISPATCHER (standalone)
// ============================================================

FString FNwiroBlueprintTools::CreateEventDispatcher(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->HasField(TEXT("dispatchers")))
	{
		if (!Cmd->TryGetArrayField(TEXT("dispatchers"), Arr) || Arr->IsEmpty())
		{
			return TEXT("{\"success\":false,\"error\":\"'dispatchers' must be a nonempty array\"}");
		}
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("name")))
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Cmd->GetStringField(TEXT("name")));
		if (Cmd->HasField(TEXT("params"))) Obj->SetField(TEXT("params"), Cmd->TryGetField(TEXT("params")));
		Items.Add(MakeShareable(new FJsonValueObject(Obj)));
	}
	else
	{
		return TEXT("{\"success\":false,\"error\":\"Provide 'name' or a nonempty 'dispatchers' array\"}");
	}

	// The handler records Modify after prevalidation; rejected targets must stay unchanged.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateEventDispatcher", "AI: Create Event Dispatcher"));
	TArray<FNwiroDispatcherMutation> Mutations;
	FNwiroBPResult R = DoAddEventDispatchers(BP, Items, &Mutations);
	bool bCompiled = false;
	FString CompileStatus = TEXT("not_requested");
	FString ValidationLevel = TEXT("skeleton");
	TArray<FString> CompileDiagnostics;

	if (!R.bSuccess)
	{
		bool bHadMutation = false;
		bool bEveryRollbackVerified = true;
		for (const FNwiroDispatcherMutation& Mutation : Mutations)
		{
			if (Mutation.bMemberCreated || Mutation.Graph)
			{
				bHadMutation = true;
				bEveryRollbackVerified &=
					Mutation.bRolledBack && Mutation.bRollbackVerified;
			}
		}
		if (!bHadMutation || bEveryRollbackVerified)
		{
			Tx.Cancel();
		}
		else
		{
			CompileDiagnostics.Add(
				TEXT("Standalone transaction retained because dispatcher rollback could not be proven"));
		}
	}
	else
	{
		bCompiled = true;
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(
			BP, EBlueprintCompileOptions::SkipSave, &Results);
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				CompileDiagnostics.Add(Message->ToText().ToString());
			}
		}

		const bool bGlobalCompileClean =
			Results.NumErrors == 0 && BP->Status != EBlueprintStatus::BS_Error;
		if (!bGlobalCompileClean)
		{
			CompileStatus = TEXT("error");
			ValidationLevel = TEXT("skeleton");
			if (CompileDiagnostics.IsEmpty())
			{
				CompileDiagnostics.Add(TEXT("Blueprint compile failed"));
			}
			R.Message += TEXT(
				". Skeleton signatures are retained; generated signatures are unvalidated because the whole Blueprint compile failed");
		}
		else
		{
			CompileStatus = TEXT("success");
			ValidationLevel = TEXT("generated");
			FNwiroDispatcherMutation* InvalidMutation = nullptr;
			for (FNwiroDispatcherMutation& Mutation : Mutations)
			{
				FString GeneratedError;
				const bool bGeneratedValid =
					NwiroDispatcherPrivate::ValidateDispatcherInvariant(
						BP, Mutation, BP->GeneratedClass, GeneratedError);
				Mutation.GeneratedSignatureValid = bGeneratedValid;
				if (!bGeneratedValid)
				{
					Mutation.ErrorCode = TEXT("GENERATED_SIGNATURE_INVALID");
					Mutation.Error = GeneratedError;
					InvalidMutation = &Mutation;
					break;
				}
			}
			if (InvalidMutation)
			{
				FString RollbackError;
				const bool bRollbackStructurallyVerified =
					NwiroDispatcherPrivate::RollbackMutations(
						BP, Mutations, RollbackError);
				FCompilerResultsLog CleanupResults;
				FKismetEditorUtilities::CompileBlueprint(
					BP, EBlueprintCompileOptions::SkipSave, &CleanupResults);
				FString AbsenceError;
				const bool bRollbackReflectionVerified =
					NwiroDispatcherPrivate::VerifyRollbackAbsence(
						BP, Mutations, true, AbsenceError);
				if (!bRollbackReflectionVerified)
				{
					RollbackError += RollbackError.IsEmpty()
						? AbsenceError : TEXT("; ") + AbsenceError;
				}
				const bool bCleanupCompileClean =
					CleanupResults.NumErrors == 0
					&& BP->Status != EBlueprintStatus::BS_Error;
				if (!bCleanupCompileClean)
				{
					RollbackError += RollbackError.IsEmpty()
						? TEXT("cleanup compile failed")
						: TEXT("; cleanup compile failed");
				}
				const bool bRollbackFullyVerified =
					bRollbackStructurallyVerified
					&& bRollbackReflectionVerified && bCleanupCompileClean;
				if (!bRollbackFullyVerified)
				{
					RollbackError += RollbackError.IsEmpty()
						? TEXT("dispatcher rollback was not fully verified")
						: TEXT("; dispatcher rollback was not fully verified");
					NwiroDispatcherPrivate::MarkRollbackFailure(
						Mutations, RollbackError);
				}
				if (!RollbackError.IsEmpty())
				{
					CompileDiagnostics.AddUnique(RollbackError);
				}
				CompileStatus = TEXT("error");
				ValidationLevel = TEXT("skeleton");
				R = FNwiroBPResult::Fail(bRollbackFullyVerified
					? FString::Printf(
						TEXT("Generated dispatcher validation failed and the dispatcher batch was rolled back: %s"),
						*InvalidMutation->Error)
					: FString::Printf(
						TEXT("Generated dispatcher validation failed and rollback could not be verified: %s"),
						*InvalidMutation->Error));
				if (bRollbackFullyVerified)
				{
					Tx.Cancel();
				}
				else
				{
					CompileDiagnostics.AddUnique(
						TEXT("Standalone transaction retained because dispatcher cleanup was not fully verified"));
				}
			}
		}
	}

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), R.bSuccess);
	Response->SetStringField(TEXT("message"), R.Message);
	Response->SetBoolField(TEXT("compiled"), bCompiled);
	Response->SetStringField(TEXT("compileStatus"), CompileStatus);
	Response->SetStringField(TEXT("validationLevel"), ValidationLevel);
	Response->SetBoolField(TEXT("savedToDisk"), false);
	NwiroDispatcherPrivate::SetDispatcherArray(Response, Mutations);
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FString& Diagnostic : CompileDiagnostics)
	{
		Diagnostics.Add(MakeShared<FJsonValueString>(Diagnostic));
	}
	Response->SetArrayField(TEXT("compileDiagnostics"), Diagnostics);
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Response, Writer);
	return Output;
}

// ============================================================
// REPARENT BLUEPRINT (standalone)
// ============================================================

FString FNwiroBlueprintTools::ReparentBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	FString NewParent = Cmd->GetStringField(TEXT("parentClass"));
	if (NewParent.IsEmpty()) NewParent = Cmd->GetStringField(TEXT("parent"));

	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "ReparentBlueprint", "AI: Reparent Blueprint"), BP);
	FNwiroBPResult R = DoReparent(BP, NewParent);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// REMOVE COMPONENT (standalone)
// ============================================================

FString FNwiroBlueprintTools::RemoveComponent(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("components"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("name")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("name")))));
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "RemoveComponent", "AI: Remove Component"), BP);
	FNwiroBPResult R = DoRemoveComponents(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// EDIT COMPONENT (standalone)
// ============================================================

FString FNwiroBlueprintTools::EditComponent(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	// Convert {name, properties:{K:V}} entries into flat {component, property, value} triplets
	// that DoSetComponentProperties expects.
	auto ExpandToTriplets = [](const FString& CompName, const TSharedPtr<FJsonObject>& PropsObj, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		for (const auto& Pair : PropsObj->Values)
		{
			const FString Key(*Pair.Key);
			FString ValStr;
			if (Pair.Value->Type == EJson::String)
				ValStr = Pair.Value->AsString();
			else if (Pair.Value->Type == EJson::Boolean)
				ValStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
			else if (Pair.Value->Type == EJson::Number)
				ValStr = FString::SanitizeFloat(Pair.Value->AsNumber());
			else
				continue;

			TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject());
			Item->SetStringField(TEXT("component"), CompName);
			Item->SetStringField(TEXT("property"), Key);
			Item->SetStringField(TEXT("value"), ValStr);
			Out.Add(MakeShareable(new FJsonValueObject(Item)));
		}
	};

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("components"), Arr))
	{
		for (const auto& CV : *Arr)
		{
			const TSharedPtr<FJsonObject>& CO = CV->AsObject();
			if (!CO.IsValid()) continue;
			FString CompName = CO->GetStringField(TEXT("name"));
			const TSharedPtr<FJsonObject>* PropsObj;
			if (CO->TryGetObjectField(TEXT("properties"), PropsObj))
				ExpandToTriplets(CompName, *PropsObj, Items);
		}
	}
	else
	{
		// Single component from top-level: {name, properties}
		FString CompName = Cmd->GetStringField(TEXT("name"));
		const TSharedPtr<FJsonObject>* PropsObj;
		if (Cmd->TryGetObjectField(TEXT("properties"), PropsObj))
			ExpandToTriplets(CompName, *PropsObj, Items);
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "EditComponent", "AI: Edit Component"), BP);
	FNwiroBPResult R = DoSetComponentProperties(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// ORGANIZE GRAPH NODES (headless layered / Sugiyama layout)
// ============================================================
//
// Nodes added programmatically tend to pile up near the same coordinates, so
// graphs read as overlapping mush. This lays a UEdGraph out left-to-right along
// exec flow: layer 0 = nodes with no incoming exec link (events / pure roots),
// each exec child pushed one column right (longest-path), pure/data nodes placed
// one column LEFT of the consumer they feed, then nodes stacked top-to-bottom
// inside each column with spacing based on estimated node height. Headless-safe
// (no Slate). Comment nodes are left where they are.
static void OrganizeGraphNodesImpl(UEdGraph* Graph)
{
	if (!Graph) return;

	auto IsExec = [](UEdGraphPin* P){ return P && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; };

	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		if (N->IsA(UEdGraphNode_Comment::StaticClass())) continue;
		Nodes.Add(N);
	}
	if (Nodes.Num() == 0) return;

	auto HasAnyExec = [&](UEdGraphNode* N)
	{
		for (UEdGraphPin* P : N->Pins) if (IsExec(P)) return true;
		return false;
	};

	TMap<UEdGraphNode*, int32> Layer;
	for (UEdGraphNode* N : Nodes) Layer.Add(N, 0);
	bool bChanged = true; int32 Guard = 0;
	while (bChanged && Guard++ < 2000)
	{
		bChanged = false;
		for (UEdGraphNode* N : Nodes)
		{
			const int32 Base = Layer[N];
			for (UEdGraphPin* OutPin : N->Pins)
			{
				if (!IsExec(OutPin) || OutPin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* Linked : OutPin->LinkedTo)
				{
					UEdGraphNode* Child = Linked ? Linked->GetOwningNode() : nullptr;
					if (Child && Layer.Contains(Child) && Layer[Child] < Base + 1) { Layer[Child] = Base + 1; bChanged = true; }
				}
			}
		}
	}

	for (UEdGraphNode* N : Nodes)
	{
		if (HasAnyExec(N)) continue;
		int32 ConsumerLayer = -1;
		for (UEdGraphPin* P : N->Pins)
		{
			if (P->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Linked : P->LinkedTo)
			{
				UEdGraphNode* C = Linked ? Linked->GetOwningNode() : nullptr;
				if (C && Layer.Contains(C)) ConsumerLayer = FMath::Max(ConsumerLayer, Layer[C]);
			}
		}
		Layer[N] = ConsumerLayer > 0 ? ConsumerLayer - 1 : 0;
	}

	int32 MaxLayer = 0;
	for (const auto& Kvp : Layer) MaxLayer = FMath::Max(MaxLayer, Kvp.Value);
	TArray<TArray<UEdGraphNode*>> Layers; Layers.SetNum(MaxLayer + 1);
	for (UEdGraphNode* N : Nodes) Layers[Layer[N]].Add(N);

	const float HSpacing = 360.f, VSpacing = 80.f;
	float CurrentX = 0.f;
	for (int32 L = 0; L < Layers.Num(); ++L)
	{
		float CurrentY = 0.f, MaxW = 0.f;
		for (UEdGraphNode* N : Layers[L])
		{
			if (!N) continue;
			N->Modify();
			N->NodePosX = (int32)CurrentX;
			N->NodePosY = (int32)CurrentY;
			const int32 Pins = FMath::Max(N->Pins.Num(), 1);
			const float EstH = FMath::Max(96.f, 40.f + Pins * 26.f);
			CurrentY += EstH + VSpacing;
			MaxW = FMath::Max(MaxW, 240.f);
		}
		CurrentX += MaxW + HSpacing;
	}
}

FString FNwiroBlueprintTools::OrganizeBlueprintNodes(const FString& JsonCommand)
{
	// Game-thread marshal: node mutation + asset save here are game-thread-only, but tool
	// dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroBlueprintTools::OrganizeBlueprintNodes(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("blueprint"), TEXT("name"), TEXT("assetPath") })
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'path' (blueprint asset path or name)\"}");

	FString GraphName;
	Cmd->TryGetStringField(TEXT("graph"), GraphName);

	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *Path);

	TArray<UEdGraph*> All; BP->GetAllGraphs(All);
	int32 Organized = 0;
	for (UEdGraph* G : All)
	{
		if (!G) continue;
		if (!GraphName.IsEmpty() && !G->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) continue;
		OrganizeGraphNodesImpl(G);
		Organized++;
	}
	if (Organized == 0)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Graph not found: %s\"}"), *GraphName);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	UEditorAssetLibrary::SaveLoadedAsset(BP, false);
	return FString::Printf(TEXT("{\"success\":true,\"organized\":%d,\"blueprint\":\"%s\"}"), Organized, *BP->GetName());
}

// ============================================================
// RENDER BLUEPRINT GRAPH -> PNG
// ============================================================
//
// Render any UBlueprint's graph (EventGraph by default, or a named graph)
// to a PNG file. Builds an off-screen SGraphEditor over the live UEdGraph,
// zooms to fit, then runs FWidgetRenderer at the requested size. Works for
// any blueprint type that exposes graphs.
FString FNwiroBlueprintTools::RenderBlueprintGraph(const FString& JsonCommand)
{
	// Game-thread marshal: render-target draw + canvas here are game/render-thread-only, but
	// tool dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroBlueprintTools::RenderBlueprintGraph(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("blueprint"), TEXT("name"), TEXT("assetPath") })
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'path' (blueprint asset path or name)\"}");

	FString GraphName;
	Cmd->TryGetStringField(TEXT("graph"), GraphName);
	if (GraphName.IsEmpty())
		Cmd->TryGetStringField(TEXT("graphName"), GraphName);

	double RequestedWidth = 1600.0, RequestedHeight = 900.0;
	if ((Cmd->HasField(TEXT("width")) && !Cmd->TryGetNumberField(TEXT("width"), RequestedWidth))
		|| (Cmd->HasField(TEXT("height")) && !Cmd->TryGetNumberField(TEXT("height"), RequestedHeight))
		|| !FMath::IsFinite(RequestedWidth) || !FMath::IsFinite(RequestedHeight))
		return TEXT("{\"success\":false,\"error\":\"width and height must be finite numbers\"}");
	const int32 Width = static_cast<int32>(FMath::Clamp(RequestedWidth, 64.0, 4096.0));
	const int32 Height = static_cast<int32>(FMath::Clamp(RequestedHeight, 64.0, 4096.0));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *Path);

	UEdGraph* Graph = nullptr;
	TArray<UEdGraph*> Graphs;
	BP->GetAllGraphs(Graphs);
	if (!GraphName.IsEmpty())
	{
		for (UEdGraph* G : Graphs)
			if (G && G->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) { Graph = G; break; }
	}
	if (!Graph)
	{
		for (UEdGraph* G : BP->UbergraphPages)
			if (G && G->GetName().Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)) { Graph = G; break; }
	}
	if (!Graph && BP->UbergraphPages.Num() > 0) Graph = BP->UbergraphPages[0];
	if (!Graph && BP->FunctionGraphs.Num() > 0) Graph = BP->FunctionGraphs[0];

	if (!Graph)
	{
		FString Available;
		for (UEdGraph* G : Graphs) { if (G) { if (!Available.IsEmpty()) Available += TEXT(","); Available += G->GetName(); } }
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Graph not found. Requested:'%s' Available:[%s]\"}"),
			*GraphName, *Available);
	}

	// Auto-organize the graph first (default on) so the render isn't overlapping
	// mush. Pass "organize": false to render the raw positions as-is.
	bool bOrganize = true;
	Cmd->TryGetBoolField(TEXT("organize"), bOrganize);
	if (bOrganize)
	{
		OrganizeGraphNodesImpl(Graph);
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);
	}

	// HEADLESS-SAFE RENDER: draw the graph straight from node data (positions,
	// titles, pins, links) onto a render-target canvas. Screenshotting an
	// off-screen SGraphEditor renders BLANK without a live Slate tick loop
	// (grid + "READ-ONLY" watermark, no nodes). Canvas drawing needs no editor
	// tab and cannot come back empty.
	struct FNodeBox { UEdGraphNode* Node=nullptr; float X=0,Y=0,W=0,H=0; TArray<UEdGraphPin*> In, Out; FLinearColor Header=FLinearColor(0.20f,0.22f,0.28f,1.f); FString Title; };
	TArray<FNodeBox> Boxes;
	TMap<UEdGraphNode*, int32> NodeToBox;
	const float HeaderH = 26.f, PinRowH = 16.f, BoxPad = 10.f, MinW = 150.f;
	float MinX = FLT_MAX, MinY = FLT_MAX, MaxX = -FLT_MAX, MaxY = -FLT_MAX;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		FNodeBox B; B.Node = N; B.X = (float)N->NodePosX; B.Y = (float)N->NodePosY;
		for (UEdGraphPin* P : N->Pins) { if (!P || P->bHidden) continue; if (P->Direction == EGPD_Input) B.In.Add(P); else B.Out.Add(P); }
		B.Title = N->GetNodeTitle(ENodeTitleType::ListView).ToString().Left(42);
		{ FLinearColor C = N->GetNodeTitleColor(); if (C.R + C.G + C.B < 0.05f) C = FLinearColor(0.20f, 0.22f, 0.28f, 1.f); C.A = 1.f; B.Header = C; }
		const int32 Rows = FMath::Max(B.In.Num(), B.Out.Num());
		B.W = FMath::Max(MinW, 28.f + B.Title.Len() * 7.0f);
		B.H = HeaderH + Rows * PinRowH + BoxPad;
		NodeToBox.Add(N, Boxes.Num());
		Boxes.Add(B);
		MinX = FMath::Min(MinX, B.X); MinY = FMath::Min(MinY, B.Y);
		MaxX = FMath::Max(MaxX, B.X + B.W); MaxY = FMath::Max(MaxY, B.Y + B.H);
	}
	if (Boxes.Num() == 0) { MinX = MinY = 0; MaxX = (float)Width; MaxY = (float)Height; }

	const float Margin = 60.f;
	const float ContentW = (MaxX - MinX) + Margin * 2.f;
	const float ContentH = (MaxY - MinY) + Margin * 2.f;
	// The requested (bounded) dimensions define the image, not the graph bounds.
	// Fit and center the complete graph without changing its aspect ratio.
	const int32 ImgW = Width, ImgH = Height;
	const float RScale = FMath::Min(1.f, FMath::Min(ImgW / FMath::Max(ContentW, 1.f), ImgH / FMath::Max(ContentH, 1.f)));
	const float OffsetX = (ImgW - ContentW * RScale) * 0.5f;
	const float OffsetY = (ImgH - ContentH * RScale) * 0.5f;
	auto MapX = [&](float X){ return OffsetX + (X - MinX + Margin) * RScale; };
	auto MapY = [&](float Y){ return OffsetY + (Y - MinY + Margin) * RScale; };

	UWorld* RWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!RWorld) return TEXT("{\"success\":false,\"error\":\"No editor world for rendering\"}");

	UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(RWorld, ImgW, ImgH, RTF_RGBA8);
	if (!RT) return TEXT("{\"success\":false,\"error\":\"CreateRenderTarget2D failed\"}");

	UCanvas* RCanvas = nullptr; FVector2D RCanvasSize(0, 0); FDrawToRenderTargetContext RCtx;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(RWorld, RT, RCanvas, RCanvasSize, RCtx);
	if (RCanvas)
	{
		UFont* RFont = GEngine ? GEngine->GetSmallFont() : nullptr;
		{ FCanvasTileItem BG(FVector2D(0, 0), GWhiteTexture, FVector2D((float)ImgW, (float)ImgH), FLinearColor(0.06f, 0.06f, 0.07f, 1.f)); BG.BlendMode = SE_BLEND_Opaque; RCanvas->DrawItem(BG); }
		for (const FNodeBox& B : Boxes)
		{
			for (int32 oi = 0; oi < B.Out.Num(); ++oi)
			{
				UEdGraphPin* OutPin = B.Out[oi]; if (!OutPin) continue;
				const float ox = MapX(B.X + B.W);
				const float oy = MapY(B.Y) + (HeaderH + oi * PinRowH + PinRowH * 0.5f) * RScale;
				for (UEdGraphPin* LP : OutPin->LinkedTo)
				{
					if (!LP || !LP->GetOwningNode()) continue;
					int32* TIdx = NodeToBox.Find(LP->GetOwningNode()); if (!TIdx) continue;
					const FNodeBox& T = Boxes[*TIdx];
					int32 ii = T.In.IndexOfByKey(LP); if (ii == INDEX_NONE) ii = 0;
					const float ix = MapX(T.X);
					const float iy = MapY(T.Y) + (HeaderH + ii * PinRowH + PinRowH * 0.5f) * RScale;
					FCanvasLineItem Line(FVector2D(ox, oy), FVector2D(ix, iy));
					Line.SetColor(FLinearColor(0.55f, 0.55f, 0.6f, 1.f)); Line.LineThickness = 1.5f;
					RCanvas->DrawItem(Line);
				}
			}
		}
		for (const FNodeBox& B : Boxes)
		{
			const float x = MapX(B.X), y = MapY(B.Y), w = B.W * RScale, h = B.H * RScale;
			{ FCanvasTileItem Body(FVector2D(x, y), GWhiteTexture, FVector2D(w, h), FLinearColor(0.12f, 0.12f, 0.14f, 1.f)); Body.BlendMode = SE_BLEND_Opaque; RCanvas->DrawItem(Body); }
			{ FCanvasTileItem Hdr(FVector2D(x, y), GWhiteTexture, FVector2D(w, HeaderH * RScale), B.Header); Hdr.BlendMode = SE_BLEND_Opaque; RCanvas->DrawItem(Hdr); }
			if (RFont)
			{
				{ FCanvasTextItem T(FVector2D(x + 5.f * RScale, y + 4.f * RScale), FText::FromString(B.Title), RFont, FLinearColor::White); T.Scale = FVector2D(RScale, RScale); RCanvas->DrawItem(T); }
				for (int32 i = 0; i < B.In.Num(); ++i)
				{ FCanvasTextItem T(FVector2D(x + 5.f * RScale, y + (HeaderH + i * PinRowH) * RScale), FText::FromString(B.In[i]->PinName.ToString().Left(16)), RFont, FLinearColor(0.78f, 0.84f, 0.9f, 1.f)); T.Scale = FVector2D(0.8f * RScale, 0.8f * RScale); RCanvas->DrawItem(T); }
				for (int32 i = 0; i < B.Out.Num(); ++i)
				{ FString PN = B.Out[i]->PinName.ToString().Left(16); FCanvasTextItem T(FVector2D(x + w - (6.f + PN.Len() * 5.f) * RScale, y + (HeaderH + i * PinRowH) * RScale), FText::FromString(PN), RFont, FLinearColor(0.78f, 0.84f, 0.9f, 1.f)); T.Scale = FVector2D(0.8f * RScale, 0.8f * RScale); RCanvas->DrawItem(T); }
			}
		}
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(RWorld, RCtx);

	FString SavePath;
	Cmd->TryGetStringField(TEXT("saveTo"), SavePath);
	if (SavePath.IsEmpty())
	{
		const FString BaseName = FPaths::GetBaseFilename(BP->GetPathName());
		SavePath = FPaths::ProjectSavedDir() / TEXT("NwiroGraphRenders") / (BaseName + TEXT("_") + Graph->GetName() + TEXT(".png"));
	}
	SavePath = FPaths::ConvertRelativePathToFull(SavePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SavePath), true);
	UKismetRenderingLibrary::ExportRenderTarget(RWorld, RT, FPaths::GetPath(SavePath), FPaths::GetCleanFilename(SavePath));

	if (!IFileManager::Get().FileExists(*SavePath))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"ExportRenderTarget produced no file at %s\"}"), *SavePath);

	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"blueprint\":\"%s\",\"graph\":\"%s\",\"nodes\":%d}"),
		*SavePath.Replace(TEXT("\\"), TEXT("/")), ImgW, ImgH, *BP->GetName(), *Graph->GetName(), Boxes.Num());
}

#if WITH_DEV_AUTOMATION_TESTS
// The spec body is BYTE-IDENTICAL to the copy in plugin/NwiroIntegrationKit; the two
// product trees stay independent (AGENTS.md) and scripts/check-plugin-parity.ts fails
// if they diverge. Only these four macros differ between the products.
#include "NwiroToolDispatch.h"
#define NWIRO_SPEC_TOOLS  FNwiroBlueprintTools
#define NWIRO_SPEC_SCHEMA FNwiroToolDispatch
#define NWIRO_SPEC_PREFIX "Nwiro.BlueprintTools."
#define NWIRO_SPEC_PKG    "/Game/__NwiroToolSpec/"
#include "Tests/NwiroBlueprintToolsSpec.inl"
#include "Tests/NwiroPinBatchSpec.inl"
// Scoped to this translation unit on purpose: UE unity builds concatenate .cpp files,
// so a leftover define would leak into whatever is stitched in after this one.
#undef NWIRO_SPEC_TOOLS
#undef NWIRO_SPEC_SCHEMA
#undef NWIRO_SPEC_PREFIX
#undef NWIRO_SPEC_PKG
#include "Tests/NwiroEventDispatcherProofSpec.inl"
#include "Tests/NwiroVariableProofSpec.inl"
#include "Tests/NwiroRenamedWidgetDispatcherSpec.inl"
#include "Tests/NwiroFunctionProofSpec.inl"
#include "Tests/NwiroFunctionAcceptanceSpec.inl"
#include "Tests/NwiroFunctionPersistenceSpec.inl"
#include "Tests/NwiroVariableEditabilitySpec.inl"
#include "Tests/NwiroBlueprintPreloadGuardSpec.inl"
#include "Tests/NwiroPreloadColdSpec.inl"
#include "Tests/NwiroBlueprintRemediationSpec.inl"
#endif
