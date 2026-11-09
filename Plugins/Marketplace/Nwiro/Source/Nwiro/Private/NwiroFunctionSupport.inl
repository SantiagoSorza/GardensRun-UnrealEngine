// Copyright 2026 Nwiro. All Rights Reserved.
// Function-only preparation, included after the accepted variable type adapter.
// Neither preparation nor target validation creates graphs or compiles dependencies.

#include "Misc/DefaultValueHelper.h"
#include "Misc/EngineVersionComparison.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"

struct FNwiroPreparedFunctionPin
{
	FName Name;
	FEdGraphPinType PinType;
	TStrongObjectPtr<UObject> SubtypeHold;
	TStrongObjectPtr<UObject> DefaultObjectHold;
	FString DefaultValue;
	bool bDefaultSpecified = false;
};

struct FNwiroPreparedFunction
{
	FName Name;
	TArray<FNwiroPreparedFunctionPin> Inputs, Outputs;
	TStrongObjectPtr<UFunction> OverrideSignature;
	int32 InterfaceIndex = INDEX_NONE;
	bool bPure = false;
	bool bPureSpecified = false;
	bool bOverride = false;
	bool bInputsSpecified = false;
	bool bOutputsSpecified = false;
	bool bExistingOverride = false;
	bool bRedefine = false;
	bool bUnchanged = false;
	TStrongObjectPtr<UEdGraph> ExistingGraph;
	TStrongObjectPtr<UK2Node_FunctionEntry> ExistingEntry;
	TArray<TStrongObjectPtr<UK2Node_FunctionResult>> ExistingResults;
	TArray<TStrongObjectPtr<UK2Node_CallFunction>> CallSites;
};

namespace NwiroFunctions
{
	static bool ReadName(const TSharedPtr<FJsonObject>& Object, FName& Out, FString& Error)
	{
		FString Name;
		if (!Object->HasTypedField<EJson::String>(TEXT("name")) || !Object->TryGetStringField(TEXT("name"), Name))
		{ Error = TEXT("name must be a string"); return false; }
		if (Name.IsEmpty() || Name.TrimStartAndEnd() != Name || Name.Len() >= NAME_SIZE
			|| Name.Len() > FKismetNameValidator::GetMaximumNameLength())
		{ Error = TEXT("name must be nonblank, within Unreal's Blueprint name limit, and have no surrounding whitespace"); return false; }
		for (TCHAR Character : Name)
		{
			if (FChar::IsControl(Character)) { Error = TEXT("name cannot contain control characters"); return false; }
		}
		// Blueprint signatures permit spaces; dispatcher's identifier restriction does not apply.
		if (!FName::IsValidXName(Name, UE_BLUEPRINT_INVALID_NAME_CHARACTERS))
		{ Error = TEXT("name contains characters Unreal does not permit in Blueprint signatures"); return false; }
		Out = FName(*Name);
		if (Out.IsNone()) { Error = TEXT("name cannot be None"); return false; }
		return true;
	}

	// Schema numeric validation intentionally accepts some coercions. Check bounds first.
	static bool ValidateInteger(const FString& Value, const TCHAR* PositiveLimit, const TCHAR* NegativeLimit)
	{
		if (Value.IsEmpty()) return true;
		int32 Index = Value[0] == TEXT('-') || Value[0] == TEXT('+') ? 1 : 0;
		const bool bNegative = Value[0] == TEXT('-');
		if (Index == Value.Len()) return false;
		for (int32 Digit = Index; Digit < Value.Len(); ++Digit)
			if (Value[Digit] < TEXT('0') || Value[Digit] > TEXT('9')) return false;
		while (Index < Value.Len() - 1 && Value[Index] == TEXT('0')) ++Index;
		const FString Digits = Value.Mid(Index);
		const FString Limit = bNegative ? NegativeLimit : PositiveLimit;
		return Digits.Len() < Limit.Len() || (Digits.Len() == Limit.Len() && Digits.Compare(Limit) <= 0);
	}

	static bool PrepareDefault(const TSharedPtr<FJsonObject>& Object, FNwiroPreparedFunctionPin& Pin,
		bool bInput, FString& Error)
	{
		Pin.bDefaultSpecified = Object->HasField(TEXT("default"));
		if (!Pin.bDefaultSpecified) return true;
		if (!bInput) { Error = TEXT("default is supported only on function inputs"); return false; }
		if (!Object->HasTypedField<EJson::String>(TEXT("default"))
			|| !Object->TryGetStringField(TEXT("default"), Pin.DefaultValue))
		{ Error = TEXT("default must be a string"); return false; }
		if (Pin.DefaultValue.Len() > 65535)
		{ Error = TEXT("default must contain at most 65535 characters and no null characters"); return false; }
		for (TCHAR Character : Pin.DefaultValue)
			if (Character == 0) { Error = TEXT("default cannot contain null characters"); return false; }
		if (Pin.PinType.IsContainer() || Pin.PinType.bIsReference)
		{ Error = TEXT("container/reference inputs require wired values, not literal defaults"); return false; }

		const FName Category = Pin.PinType.PinCategory;
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		FString Value = Pin.DefaultValue;
		TObjectPtr<UObject> DefaultObject = nullptr;
		FText DefaultText;
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			if (Value.IsEmpty() || Value == TEXT("0") || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Value = TEXT("false");
			else if (Value == TEXT("1") || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Value = TEXT("true");
			else { Error = TEXT("boolean default must be true, false, 0, or 1"); return false; }
			Pin.DefaultValue = Value;
		}
		else if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64)
		{
			const bool bInt64 = Category == UEdGraphSchema_K2::PC_Int64;
			if (!ValidateInteger(Value, bInt64 ? TEXT("9223372036854775807") : TEXT("2147483647"),
				bInt64 ? TEXT("9223372036854775808") : TEXT("2147483648")))
			{ Error = TEXT("integer default must be a decimal integer within the declared type's range"); return false; }
		}
		else if (Category == UEdGraphSchema_K2::PC_Real && !Value.IsEmpty())
		{
			double Number = 0;
			if (!FDefaultValueHelper::ParseDouble(Value, Number) || !FMath::IsFinite(Number)
				|| (Pin.PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float && FMath::Abs(Number) > MAX_flt))
			{ Error = TEXT("real default must be a finite number within the declared type's range"); return false; }
		}
		else if (Category == UEdGraphSchema_K2::PC_Name && Value.Len() >= NAME_SIZE)
		{ Error = TEXT("name default exceeds Unreal's name limit"); return false; }
		else if (Category == UEdGraphSchema_K2::PC_Byte && Cast<UEnum>(Pin.PinType.PinSubCategoryObject.Get()))
		{
			UEnum* Enum = CastChecked<UEnum>(Pin.PinType.PinSubCategoryObject.Get());
			if (Value.IsEmpty()) Value = Enum->GetNameStringByIndex(0);
			if (Value.Len() >= NAME_SIZE || Enum->GetIndexByNameString(Value) == INDEX_NONE)
			{ Error = TEXT("enum default must name an exact enumerator of the declared enum"); return false; }
			Pin.DefaultValue = Value;
		}
		else if (Category == UEdGraphSchema_K2::PC_Byte && !ValidateInteger(Value, TEXT("255"), TEXT("0")))
		{ Error = TEXT("byte default must be a decimal integer from 0 to 255"); return false; }
		else if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class)
		{
			if (!Value.IsEmpty() && !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				if (Value.Len() >= NAME_SIZE || !Value.StartsWith(TEXT("/")) || Value.Contains(TEXT(":"))
					|| !FPackageName::IsValidObjectPath(Value))
				{ Error = TEXT("object/class default must be None or an exact loaded object path"); return false; }
				DefaultObject = FindObjectSafe<UObject>(nullptr, *Value);
				if (!IsValid(DefaultObject) || DefaultObject->GetOutermost() == GetTransientPackage()
					|| (DefaultObject->HasAnyFlags(RF_Transient) && !DefaultObject->IsNative())
					|| (Category == UEdGraphSchema_K2::PC_Class ? !DefaultObject->IsA<UClass>() : !DefaultObject->IsAsset()))
				{ Error = TEXT("default object must already be loaded and be an eligible class or asset"); return false; }
				Pin.DefaultObjectHold.Reset(DefaultObject.Get());
			}
			// UE represents null references as empty values, including autogenerated defaults.
			Pin.DefaultValue = DefaultObject ? DefaultObject->GetPathName() : FString();
			Value.Empty();
		}
		else if (Category == UEdGraphSchema_K2::PC_Text)
		{
			// Treat JSON strings as plain text, not LOCTABLE/other loading expressions.
			DefaultText = FText::AsCultureInvariant(Value);
			Pin.DefaultValue.Reset();
			FTextStringHelper::WriteToBuffer(Pin.DefaultValue, DefaultText);
			Value.Empty();
		}
		else if (Category == UEdGraphSchema_K2::PC_Struct && !Value.IsEmpty())
		{
			const UObject* Struct = Pin.PinType.PinSubCategoryObject.Get();
			if (Struct != TBaseStructure<FVector>::Get() && Struct != TBaseStructure<FRotator>::Get()
				&& Struct != TBaseStructure<FTransform>::Get() && Struct != TBaseStructure<FLinearColor>::Get())
			{ Error = TEXT("literal struct defaults support Vector, Rotator, Transform and LinearColor; wire other structs explicitly"); return false; }
		}
		return Schema->DefaultValueSimpleValidation(Pin.PinType, Pin.Name, Value, DefaultObject, DefaultText, &Error);
	}

	static bool IsReferenceDefaultType(const FEdGraphPinType& Type)
	{
		return Type.PinCategory == UEdGraphSchema_K2::PC_Class || Type.PinCategory == UEdGraphSchema_K2::PC_Object;
	}

	static FName ReferenceDefaultKey(FName Name)
	{
		const FString Key = TEXT("CPP_Default_") + Name.ToString();
		return Key.Len() < NAME_SIZE ? FName(*Key) : NAME_None;
	}

	static bool ValidateReferenceDefaultNames(const TArray<FNwiroPreparedFunctionPin>& Inputs,
		const TSet<FName>& Names, FString& Error)
	{
		for (const auto& Pin : Inputs)
		{
			if (!IsReferenceDefaultType(Pin.PinType) || Pin.DefaultValue.IsEmpty()) continue;
			const FName Key = ReferenceDefaultKey(Pin.Name);
			if (Key.IsNone() || Names.Contains(Key))
			{ Error = TEXT("reference default metadata key is too long or collides with a parameter name"); return false; }
		}
		return true;
	}

	static bool ReferenceDefaultsMatch(UK2Node_FunctionEntry* Entry, const TArray<FNwiroPreparedFunctionPin>& Inputs)
	{
		for (const auto& Pin : Inputs)
		{
			if (!IsReferenceDefaultType(Pin.PinType)) continue;
			const FName Key = ReferenceDefaultKey(Pin.Name);
			const FString* Stored = Entry->MetaData.GetMetaDataMap().Find(Key);
			if (Pin.DefaultValue.IsEmpty() ? Stored != nullptr : !Stored || *Stored != Pin.DefaultValue) return false;
		}
		return true;
	}

	static void ApplyReferenceDefaultMetadata(UK2Node_FunctionEntry* Entry, const TArray<FNwiroPreparedFunctionPin>& Inputs)
	{
		// UE skips class/object user-pin defaults. Its serialized metadata map is
		// copied into both skeleton and generated functions, including CPP_Default_.
		for (const auto& Old : Entry->UserDefinedPins)
			if (Old.IsValid() && IsReferenceDefaultType(Old->PinType))
			{
				const FName Key = ReferenceDefaultKey(Old->PinName);
				if (!Key.IsNone()) Entry->MetaData.RemoveMetaData(Key);
			}
		for (const auto& Pin : Inputs)
			if (IsReferenceDefaultType(Pin.PinType))
			{
				const FName Key = ReferenceDefaultKey(Pin.Name);
				if (Key.IsNone()) continue;
				Entry->MetaData.RemoveMetaData(Key);
				if (!Pin.DefaultValue.IsEmpty()) Entry->MetaData.SetMetaData(Key, Pin.DefaultValue);
			}
	}

	static bool PreparePins(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key,
		TArray<FNwiroPreparedFunctionPin>& Out, bool& bSpecified,
		TSet<FName>& Names, FString& Field, FString& Error)
	{
		bSpecified = Object->HasField(Key);
		if (!bSpecified) return true;
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		Field = Key;
		if (!Object->TryGetArrayField(Key, Pins)) { Error = Field + TEXT(" must be an array"); return false; }
		Out.SetNum(Pins->Num());
		for (int32 Index = 0; Index < Pins->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Pins)[Index];
			const FString Prefix = FString::Printf(TEXT("%s[%d]"), Key, Index);
			Field = Prefix;
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{ Error = TEXT("each parameter must be an object"); return false; }
			const auto PinObject = Value->AsObject();
			for (const auto& Pair : PinObject->Values)
			{
				if (Pair.Key != TEXT("name") && Pair.Key != TEXT("type") && Pair.Key != TEXT("subtype")
					&& Pair.Key != TEXT("container") && Pair.Key != TEXT("objectClass") && Pair.Key != TEXT("object_class") && Pair.Key != TEXT("default"))
				{ Field = Prefix + TEXT(".") + FString(*Pair.Key); Error = TEXT("unknown function parameter option"); return false; }
			}
			FNwiroPreparedFunctionPin& Pin = Out[Index];
			Field = Prefix + TEXT(".name");
			if (!ReadName(PinObject, Pin.Name, Error)) return false;
			if (Names.Contains(Pin.Name)) { Error = TEXT("duplicate or reserved parameter name"); return false; }
			Names.Add(Pin.Name);
			Field = Prefix + TEXT(".type");
			// Reuse the accepted type contract without changing variable names/options or ParsePinType.
			FNwiroPreparedVariable Type;
			if (!NwiroVariables::PrepareType(PinObject, Type, Error)) return false;
			Pin.PinType = Type.PinType;
			Pin.SubtypeHold = MoveTemp(Type.SubtypeHold);
			Field = Prefix + TEXT(".default");
			if (!PrepareDefault(PinObject, Pin, FString(Key) == TEXT("inputs"), Error)) return false;
		}
		return true;
	}

	static bool PrepareOne(const TSharedPtr<FJsonValue>& Value,
		FNwiroPreparedFunction& Out, FString& Field, FString& Error)
	{
		Field = TEXT("function");
		if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
		{ Error = TEXT("each function must be an object"); return false; }
		const auto Object = Value->AsObject();
		for (const auto& Pair : Object->Values)
		{
			if (Pair.Key != TEXT("name") && Pair.Key != TEXT("pure") && Pair.Key != TEXT("override")
				&& Pair.Key != TEXT("inputs") && Pair.Key != TEXT("outputs") && Pair.Key != TEXT("redefine"))
			{ Field = FString(*Pair.Key); Error = TEXT("unknown function option"); return false; }
		}
		Field = TEXT("name");
		if (!ReadName(Object, Out.Name, Error)) return false;
		for (const TCHAR* Key : { TEXT("pure"), TEXT("override"), TEXT("redefine") })
		{
			if (!Object->HasField(Key)) continue;
			Field = Key;
			if (!Object->HasTypedField<EJson::Boolean>(Key)) { Error = Field + TEXT(" must be a boolean"); return false; }
			if (Field == TEXT("pure")) { Out.bPureSpecified = true; Out.bPure = Object->GetBoolField(Key); }
			else if (Field == TEXT("override")) Out.bOverride = Object->GetBoolField(Key);
			else Out.bRedefine = Object->GetBoolField(Key);
		}
		if (Out.bOverride && Out.bRedefine)
		{ Field = TEXT("redefine"); Error = TEXT("redefine and override cannot both be true"); return false; }
		TSet<FName> Names = { UEdGraphSchema_K2::PN_Then, UEdGraphSchema_K2::PN_Execute, UEdGraphSchema_K2::PN_Self };
		return PreparePins(Object, TEXT("inputs"), Out.Inputs, Out.bInputsSpecified, Names, Field, Error)
			&& PreparePins(Object, TEXT("outputs"), Out.Outputs, Out.bOutputsSpecified, Names, Field, Error)
			&& ValidateReferenceDefaultNames(Out.Inputs, Names, Error);
	}

	static bool Prepare(const TArray<TSharedPtr<FJsonValue>>& Items,
		TArray<FNwiroPreparedFunction>& Out, FNwiroUnitLedger& Ledger)
	{
		Out.SetNum(Items.Num());
		TSet<FName> Names;
		for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
		{
			FString Field, Error;
			if (!PrepareOne(Items[Unit], Out[Unit], Field, Error))
				Ledger.AddError(Unit, TEXT("add_functions"), TEXT("INVALID_VALUE"), Field, Out[Unit].Name.ToString(), Error);
			else if (Names.Contains(Out[Unit].Name))
				Ledger.AddError(Unit, TEXT("add_functions"), TEXT("DUPLICATE_NAME"), TEXT("name"), Out[Unit].Name.ToString(), TEXT("duplicate function name in this batch (case-insensitive)"));
			Names.Add(Out[Unit].Name);
		}
		return Ledger.Errors.IsEmpty();
	}

	static UEdGraph* FindExactGraph(UBlueprint* BP, FName Name)
	{
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs) if (Graph && Graph->GetFName() == Name) return Graph;
		return nullptr;
	}

	static bool ReadyDependency(UClass* Class, FString& Error)
	{
		const UBlueprint* Dependency = Class ? Cast<UBlueprint>(Class->ClassGeneratedBy) : nullptr;
		if (Dependency && Dependency->Status != BS_UpToDate && Dependency->Status != BS_UpToDateWithWarnings)
		{
			Error = FString::Printf(TEXT("compile dependency '%s' first, then retry the override; validation does not compile dependencies"), *Dependency->GetPathName());
			return false;
		}
		return true;
	}

	static bool MatchesSignature(const TArray<FNwiroPreparedFunctionPin>& Pins,
		UFunction* Signature, bool bOutputs, FString& Error)
	{
		int32 Index = 0;
		for (TFieldIterator<FProperty> It(Signature); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
			const bool bOutput = Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm);
			if (bOutput != bOutputs) continue;
			FEdGraphPinType Type;
			if (Index >= Pins.Num() || Property->GetFName() != Pins[Index].Name
				|| !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, Type))
			{ Error = TEXT("supplied parameters must match the inherited signature names and order"); return false; }
			// Reference/const qualifiers belong to the inherited function, not this JSON contract.
			Type.bIsReference = false;
			Type.bIsConst = false;
			if (Type != Pins[Index].PinType)
			{ Error = TEXT("supplied parameter type does not match the inherited signature"); return false; }
			if (Pins[Index].bDefaultSpecified)
			{
				FString Default;
				UEdGraphSchema_K2::FindFunctionParameterDefaultValue(Signature, Property, Default);
				if (Default != Pins[Index].DefaultValue)
				{ Error = TEXT("override cannot change inherited parameter defaults"); return false; }
			}
			++Index;
		}
		if (Index != Pins.Num()) { Error = TEXT("supplied parameter count does not match the inherited signature"); return false; }
		return true;
	}

	static bool SamePins(const TArray<FNwiroPreparedFunctionPin>& Pins, UK2Node_EditablePinBase* Node)
	{
		if (!Node) return Pins.IsEmpty();
		if (Node->UserDefinedPins.Num() != Pins.Num()) return false;
		for (int32 Index = 0; Index < Pins.Num(); ++Index)
		{
			const TSharedPtr<FUserPinInfo>& Existing = Node->UserDefinedPins[Index];
			if (!Existing.IsValid() || Existing->PinName != Pins[Index].Name
				|| Existing->PinType != Pins[Index].PinType || Existing->PinDefaultValue != Pins[Index].DefaultValue) return false;
		}
		return true;
	}

	static void RetainPins(UK2Node_EditablePinBase* Node, TArray<FNwiroPreparedFunctionPin>& Pins)
	{
		if (!Node) return;
		for (const TSharedPtr<FUserPinInfo>& Existing : Node->UserDefinedPins)
		{
			FNwiroPreparedFunctionPin& Pin = Pins.AddDefaulted_GetRef();
			Pin.Name = Existing->PinName;
			Pin.PinType = Existing->PinType;
			Pin.DefaultValue = Existing->PinDefaultValue;
			Pin.SubtypeHold.Reset(Pin.PinType.PinSubCategoryObject.Get());
		}
	}

	static bool KeepLinkedPins(UK2Node* Node, const TArray<FNwiroPreparedFunctionPin>& Pins,
		EEdGraphPinDirection Direction, bool bPure, FString& Error)
	{
		for (UEdGraphPin* Existing : Node->Pins)
		{
			if (!Existing || Existing->LinkedTo.IsEmpty()) continue;
			const UEdGraphPin* Root = Existing;
			while (Root->ParentPin) Root = Root->ParentPin;
			if (Root->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (bPure) { Error = TEXT("disconnect execution links before making a function pure"); return false; }
				continue;
			}
			if (Root->Direction != Direction || Root->PinName == UEdGraphSchema_K2::PN_Self) continue;
			const auto* Requested = Pins.FindByPredicate([&](const FNwiroPreparedFunctionPin& Pin) { return Pin.Name == Root->PinName; });
			FEdGraphPinType ExistingType = Root->PinType;
			ExistingType.bIsConst = false;
			ExistingType.bIsReference = false;
			if (!Requested || Requested->PinType != ExistingType)
			{
				Error = FString::Printf(TEXT("disconnect linked parameter '%s' on '%s' before removing or changing its type"),
					*Root->PinName.ToString(), *Node->GetPathName());
				return false;
			}
		}
		return true;
	}

	struct FCallSiteCollector : FBasePinChangeHelper
	{
		TArray<UK2Node_CallFunction*> Calls;
		virtual void EditCallSite(UK2Node_CallFunction* Call, UBlueprint*) override { Calls.AddUnique(Call); }
	};

	static bool CheckLoadedReferencerClosure(UBlueprint* BP, FString& Error)
	{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		// IsLoadingAssets only observes the initial search; later mount scans are invisible in UE 5.5.
		// An unchanged redefinition returns before this helper. Never weaken mutation admission.
		(void)BP;
		Error = TEXT("changed function redefinition requires Unreal Engine 5.6 or newer: this engine cannot safely verify complete AssetRegistry referencer discovery; create a new function or keep the existing signature unchanged");
		return false;
#else
		FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (!Module)
		{
			Error = TEXT("function redefinition requires an initialized AssetRegistry module; initialize the editor asset registry and retry");
			return false;
		}
		IAssetRegistry& Registry = Module->Get();
		auto DiscoveryComplete = [&]() { return Registry.IsSearchAllAssets() && !Registry.IsGathering(); };
		if (!DiscoveryComplete())
		{
			Error = TEXT("function redefinition requires completed all-assets discovery; finish the editor asset registry scan and retry");
			return false;
		}
		if (!BP || !BP->GetOutermost())
		{
			Error = TEXT("function redefinition target has no package");
			return false;
		}
		constexpr int32 MaxPackages = 4096;
		constexpr int32 MaxEdges = 65536;
		const FName TargetPackage = BP->GetOutermost()->GetFName();
		TArray<FName> Pending{ TargetPackage };
		TSet<FName> Visited{ TargetPackage };
		int32 Edges = 0;
		for (int32 Index = 0; Index < Pending.Num(); ++Index)
		{
			if (!DiscoveryComplete())
			{
				Error = TEXT("asset registry discovery changed during function preflight; finish discovery and retry");
				return false;
			}
			TArray<FName> Referencers;
			// Empty query flags include hard and soft Package dependencies. The UE5.8
			// false return means no cached dependency node; new unsaved packages are valid.
			Registry.GetReferencers(Pending[Index], Referencers, UE::AssetRegistry::EDependencyCategory::Package);
			if (Referencers.Num() > MaxEdges - Edges)
			{
				Error = TEXT("function redefinition dependency scope exceeds the 65536-edge preflight limit; review and reduce the affected package scope");
				return false;
			}
			Edges += Referencers.Num();
			Referencers.Sort(FNameLexicalLess());
			for (FName Referencer : Referencers)
			{
				if (Referencer.IsNone())
				{
					Error = TEXT("asset registry returned an invalid package referencer; refresh discovery before redefining the function");
					return false;
				}
				if (Visited.Contains(Referencer)) continue;
				if (Visited.Num() >= MaxPackages)
				{
					Error = TEXT("function redefinition dependency scope exceeds the 4096-package preflight limit; review and reduce the affected package scope");
					return false;
				}
				// FindPackage is lookup only. IsFullyLoaded does no loads, but UE may
				// cache a fully-loaded bit for native/new packages; graph/dirty state stays untouched.
				UPackage* Package = FindPackage(nullptr, *Referencer.ToString());
				if (!Package || !Package->IsFullyLoaded())
				{
					Error = FString::Printf(TEXT("function redefinition cannot inspect saved dependent package '%s' (referencing '%s') because it is not fully loaded; explicitly load and review the dependent assets, then retry"),
						*Referencer.ToString(), *Pending[Index].ToString());
					return false;
				}
				Visited.Add(Referencer);
				Pending.Add(Referencer);
			}
		}
		if (!DiscoveryComplete())
		{
			Error = TEXT("asset registry discovery changed during function preflight; finish discovery and retry");
			return false;
		}
		return true;
#endif
	}

	static bool PrepareRedefinition(UBlueprint* BP, UEdGraph* Graph, FNwiroPreparedFunction& Item, FString& Error)
	{
		if (!Graph || !BP->FunctionGraphs.Contains(Graph) || !Graph->bEditable || BP->BlueprintType == BPTYPE_Interface
			|| Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript
			|| (BP->ParentClass && BP->ParentClass->FindFunctionByName(Item.Name))
			|| FBlueprintEditorUtils::FindFunctionInImplementedInterfaces(BP, Item.Name))
		{ Error = TEXT("redefine requires an existing editable ordinary function, not an inherited/interface or construction signature"); return false; }
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
			{
				if (Entry) { Error = TEXT("function graph must contain exactly one entry node"); return false; }
				Entry = Candidate;
			}
			if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node)) Item.ExistingResults.Emplace(Result);
		}
		if (!Entry || !Entry->bIsEditable || !Entry->CustomGeneratedFunctionName.IsNone())
		{ Error = TEXT("function entry is missing, locked, or uses a generated alias"); return false; }
		for (const auto& Pin : Entry->UserDefinedPins)
			if (!Pin.IsValid()) { Error = TEXT("existing function has an invalid parameter declaration"); return false; }
		for (const auto& Result : Item.ExistingResults)
		{
			if (!Result->bIsEditable) { Error = TEXT("function result is locked"); return false; }
			for (const auto& Pin : Result->UserDefinedPins)
				if (!Pin.IsValid()) { Error = TEXT("existing function has an invalid output declaration"); return false; }
		}
		if (!Item.bInputsSpecified)
		{
			RetainPins(Entry, Item.Inputs);
			if (!ReferenceDefaultsMatch(Entry, Item.Inputs))
			{ Error = TEXT("reference input defaults and stored metadata disagree; explicitly supply the complete inputs declaration to redefine their defaults"); return false; }
		}
		if (!Item.bOutputsSpecified && !Item.ExistingResults.IsEmpty()) RetainPins(Item.ExistingResults[0].Get(), Item.Outputs);
		if (!Item.bPureSpecified) Item.bPure = Entry->HasAnyExtraFlags(FUNC_BlueprintPure);
		TSet<FName> Names;
		for (const auto* Pins : { &Item.Inputs, &Item.Outputs })
			for (const auto& Pin : *Pins)
			{
				if (Names.Contains(Pin.Name)) { Error = TEXT("retained and supplied parameters have duplicate names"); return false; }
				Names.Add(Pin.Name);
				for (const FBPVariableDescription& Local : Entry->LocalVariables)
					if (Local.VarName == Pin.Name) { Error = TEXT("parameter name collides with a function local variable"); return false; }
			}
		if (!ValidateReferenceDefaultNames(Item.Inputs, Names, Error)) return false;
		Item.bUnchanged = SamePins(Item.Inputs, Entry) && ReferenceDefaultsMatch(Entry, Item.Inputs)
			&& Item.bPure == Entry->HasAnyExtraFlags(FUNC_BlueprintPure);
		if (Item.ExistingResults.IsEmpty()) Item.bUnchanged &= Item.Outputs.IsEmpty();
		for (const auto& Result : Item.ExistingResults) Item.bUnchanged &= SamePins(Item.Outputs, Result.Get());
		Item.ExistingGraph.Reset(Graph);
		Item.ExistingEntry.Reset(Entry);
		if (Item.bUnchanged) return true;
		if (!CheckLoadedReferencerClosure(BP, Error)) return false;
		if (!KeepLinkedPins(Entry, Item.Inputs, EGPD_Output, Item.bPure, Error)) return false;
		for (const auto& Result : Item.ExistingResults)
			if (!KeepLinkedPins(Result.Get(), Item.Outputs, EGPD_Input, Item.bPure, Error)) return false;

		FCallSiteCollector Collector;
		Collector.Broadcast(BP, Entry, Graph);
		for (UK2Node_CallFunction* Call : Collector.Calls)
		{
			if (!KeepLinkedPins(Call, Item.Inputs, EGPD_Input, Item.bPure, Error)
				|| !KeepLinkedPins(Call, Item.Outputs, EGPD_Output, Item.bPure, Error)) return false;
			Item.CallSites.Emplace(Call);
		}
		// A changed declaration must not invalidate loaded inherited implementations or bindings.
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (*It != BP && It->ParentClass && BP->GeneratedClass && It->ParentClass->IsChildOf(BP->GeneratedClass)
				&& FindExactGraph(*It, Item.Name))
			{ Error = TEXT("update/remove loaded child overrides before redefining their inherited signature"); return false; }
		}
		for (TObjectIterator<UK2Node_CreateDelegate> It(RF_Transient); It; ++It)
		{
			if (!FBasePinChangeHelper::NodeIsNotTransient(*It) || It->GetFunctionName() != Item.Name) continue;
			UClass* Scope = It->GetScopeClass();
			UFunction* Function = Scope ? Scope->FindFunctionByName(Item.Name) : nullptr;
			if (Function && Function->GetOwnerClass()->ClassGeneratedBy == BP)
			{ Error = TEXT("remove loaded delegate bindings before redefining their function signature"); return false; }
		}
		return true;
	}

	static void ReplacePins(UK2Node_EditablePinBase* Node, const TArray<FNwiroPreparedFunctionPin>& Pins, EEdGraphPinDirection Direction)
	{
		Node->Modify();
		Node->UserDefinedPins.Empty();
		for (const auto& Pin : Pins)
		{
			const auto Definition = MakeShared<FUserPinInfo>();
			Definition->PinName = Pin.Name;
			Definition->PinType = Pin.PinType;
			Definition->DesiredPinDirection = Direction;
			Definition->PinDefaultValue = Pin.DefaultValue;
			Node->UserDefinedPins.Add(Definition);
		}
		const bool bPrevious = Node->bDisableOrphanPinSaving;
		Node->bDisableOrphanPinSaving = true;
		Node->ReconstructNode();
		Node->bDisableOrphanPinSaving = bPrevious;
	}

	static bool ApplyRedefinition(UBlueprint* BP, const FNwiroPreparedFunction& Item, FString& Error)
	{
		UEdGraph* Graph = Item.ExistingGraph.Get();
		UK2Node_FunctionEntry* Entry = Item.ExistingEntry.Get();
		if (!Graph || !Entry || Entry->GetGraph() != Graph)
		{ Error = TEXT("function target changed after preflight"); return false; }
		Graph->Modify();
		Entry->Modify();
		Entry->SetExtraFlags(Item.bPure ? Entry->GetExtraFlags() | FUNC_BlueprintPure : Entry->GetExtraFlags() & ~FUNC_BlueprintPure);
		TArray<UK2Node_FunctionResult*> Results;
		for (const auto& Result : Item.ExistingResults) Results.Add(Result.Get());
		if (Results.IsEmpty() && !Item.Outputs.IsEmpty())
		{
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
			UK2Node_FunctionResult* Result = Creator.CreateNode();
			if (!Result) { Error = TEXT("failed to create function result node"); return false; }
			Result->NodePosX = 400;
			Result->bIsEditable = Entry->bIsEditable;
			Result->FunctionReference = Entry->FunctionReference;
			Creator.Finalize();
			Results.Add(Result);
		}
		ApplyReferenceDefaultMetadata(Entry, Item.Inputs);
		ReplacePins(Entry, Item.Inputs, EGPD_Output);
		for (UK2Node_FunctionResult* Result : Results) ReplacePins(Result, Item.Outputs, EGPD_Input);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		for (const auto& Call : Item.CallSites)
		{
			Call->Modify();
			const bool bPrevious = Call->bDisableOrphanPinSaving;
			Call->bDisableOrphanPinSaving = true;
			Call->ReconstructNode();
			Call->bDisableOrphanPinSaving = bPrevious;
			if (UBlueprint* Caller = FBlueprintEditorUtils::FindBlueprintForNode(Call.Get()))
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Caller);
				Caller->BroadcastChanged();
			}
		}
		Graph->NotifyGraphChanged();
		BP->BroadcastChanged();
		return true;
	}

	static bool ValidateTarget(UBlueprint* BP, TArray<FNwiroPreparedFunction>& Items, FNwiroUnitLedger& Ledger)
	{
		FKismetNameValidator Validator(BP);
		for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
		{
			FNwiroPreparedFunction& Item = Items[Unit];
			FString Error;
			UEdGraph* Existing = FindExactGraph(BP, Item.Name);
			if (Item.bRedefine)
			{
				PrepareRedefinition(BP, Existing, Item, Error);
			}
			else if (Existing)
			{
				if (Item.bOverride) { Item.bExistingOverride = true; continue; }
				Error = TEXT("function or graph already exists; use redefine:true to change an ordinary signature");
			}
			else if (!Item.bOverride)
			{
				if (Validator.IsValid(Item.Name) != EValidatorResult::Ok)
					Error = TEXT("name collides with an existing/inherited member or graph");
			}
			else
			{
				UFunction* Signature = nullptr;
				if (BP->ParentClass)
				{
					Signature = BP->ParentClass->FindFunctionByName(Item.Name);
					if (Signature && !ReadyDependency(BP->ParentClass, Error)) Signature = nullptr;
				}
				for (int32 Index = 0; !Signature && Error.IsEmpty() && Index < BP->ImplementedInterfaces.Num(); ++Index)
				{
					UClass* Interface = BP->ImplementedInterfaces[Index].Interface;
					if (!Interface) continue;
					Signature = Interface->FindFunctionByName(Item.Name);
					if (Signature && !ReadyDependency(Interface, Error)) Signature = nullptr;
					if (Signature) Item.InterfaceIndex = Index;
				}
				if (Signature)
				{
					Item.OverrideSignature.Reset(Signature);
					// Match editor eligibility; final/static signatures cannot pass compiler override flags.
					if (!UEdGraphSchema_K2::CanKismetOverrideFunction(Signature)
						|| !BP->AllowFunctionOverride(Signature) || Signature->HasAnyFunctionFlags(FUNC_Final | FUNC_Static))
						Error = TEXT("inherited function cannot be overridden by this Blueprint");
					else if (Item.bPureSpecified && Item.bPure != Signature->HasAnyFunctionFlags(FUNC_BlueprintPure))
						Error = TEXT("override cannot change inherited function purity");
					if (Error.IsEmpty() && Item.bInputsSpecified) MatchesSignature(Item.Inputs, Signature, false, Error);
					if (Error.IsEmpty() && Item.bOutputsSpecified) MatchesSignature(Item.Outputs, Signature, true, Error);
				}
				else if (Error.IsEmpty()) Error = TEXT("override function not found on the compiled parent or implemented interfaces; compile the dependency first if its signature is pending");
			}
			if (!Error.IsEmpty())
				Ledger.AddError(Unit, TEXT("add_functions"), TEXT("INVALID_TARGET"), TEXT("name"), Item.Name.ToString(), Error);
		}
		return Ledger.Errors.IsEmpty();
	}

	static void AddSkipped(FNwiroUnitLedger& Ledger, int32 Unit, FName Name, const FString& Reason)
	{
		auto Entry = FNwiroUnitLedger::MakeUnit(Unit, TEXT("add_functions"), Name.ToString(), false);
		Entry->AsObject()->SetStringField(TEXT("reason"), Reason);
		Ledger.Skipped.Add(Entry);
	}

	static FString RejectedResponse(UBlueprint* BP, const FString& BPName,
		const TArray<FNwiroPreparedFunction>& Items, FNwiroUnitLedger Ledger,
		const FString& Message, bool bMutationAttempted = false,
		const TArray<TSharedPtr<FJsonValue>>* ResidualFunctions = nullptr)
	{
		const TSet<int32> Accounted = Ledger.AccountedUnits();
		for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
			if (!Accounted.Contains(Unit)) AddSkipped(Ledger, Unit, Items[Unit].Name, TEXT("function batch rejected"));
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetBoolField(TEXT("compiled"), false);
		Result->SetBoolField(TEXT("savedToDisk"), false);
		Result->SetStringField(TEXT("compileStatus"), TEXT("blocked"));
		Result->SetStringField(TEXT("validationLevel"), bMutationAttempted ? TEXT("uncertain") : TEXT("preflight"));
		Result->SetBoolField(TEXT("mutationAttempted"), bMutationAttempted);
		if (ResidualFunctions) Result->SetArrayField(TEXT("residualFunctions"), *ResidualFunctions);
		Result->SetStringField(TEXT("blueprint"), BP ? BP->GetName() : BPName);
		Result->SetStringField(TEXT("error"), Message);
		Result->SetArrayField(TEXT("messages"), { MakeShared<FJsonValueString>(Message) });
		Result->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Ledger.Build(TEXT("add_functions"), Items.Num())) });
		TArray<TSharedPtr<FJsonValue>> Dirty;
		if (BP && BP->GetOutermost()->IsDirty()) Dirty.Add(MakeShared<FJsonValueString>(BP->GetOutermost()->GetName()));
		if (BP) Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
		else Result->SetField(TEXT("dirtyPackages"), MakeShared<FJsonValueNull>());
		Result->SetBoolField(TEXT("targetStateObserved"), BP != nullptr);
		FString Json;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}
	static FString RejectedApplyResponse(UBlueprint* BP,
		const TArray<FNwiroPreparedFunction>& Items, const FNwiroBPResult& Applied)
	{
		FNwiroUnitLedger Ledger;
		Ledger.Created = Applied.Level2->GetArrayField(TEXT("created"));
		Ledger.Modified = Applied.Level2->GetArrayField(TEXT("modified"));
		Ledger.Removed = Applied.Level2->GetArrayField(TEXT("removed"));
		Ledger.Skipped = Applied.Level2->GetArrayField(TEXT("skipped"));
		Ledger.Errors = Applied.Level2->GetArrayField(TEXT("errors"));
		const TArray<TSharedPtr<FJsonValue>>* Residuals = nullptr;
		if (Applied.Data.IsValid()) Applied.Data->TryGetArrayField(TEXT("residualFunctions"), Residuals);
		return RejectedResponse(BP, BP->GetName(), Items, Ledger, Applied.Message, true, Residuals);
	}
}
