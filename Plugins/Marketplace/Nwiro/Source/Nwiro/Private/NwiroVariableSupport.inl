// Copyright 2026 Nwiro. All Rights Reserved.
// Variable-only preparation. Included after the operation ledger; the general
// ParsePinType contract for nodes, functions and custom events is unchanged.
#include "UObject/StrongObjectPtr.h"

struct FNwiroPreparedVariable
{
	FName Name;
	FEdGraphPinType PinType;
	TStrongObjectPtr<UObject> SubtypeHold;
	FString RequestedType;
	FString DefaultValue;
	TOptional<FString> Category;
	TOptional<FString> Tooltip;
	bool bReplicated = false;
	bool bExposeOnSpawn = false;
	bool bSaveGame = false;
	TOptional<bool> InstanceEditable;
};

#include "NwiroTypeValidation.h"
#include "Kismet2/StructureEditorUtils.h"

namespace NwiroVariables
{
	static bool OptionalString(const TSharedPtr<FJsonObject>& Obj,
		const TArray<FString>& Aliases, FString& Out, bool& bPresent, FString& Error)
	{
		bPresent = false;
		for (const auto& Pair : Obj->Values)
		{
			const FString Key = NormalizeKey(FString(*Pair.Key));
			if (!Aliases.Contains(Key)) continue;
			if (bPresent)
			{
				Error = FString::Printf(TEXT("conflicting spellings for '%s'"), *Aliases[0]);
				return false;
			}
			bPresent = true;
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
			{
				Error = FString::Printf(TEXT("'%s' must be a string"), *FString(*Pair.Key));
				return false;
			}
			Out = Pair.Value->AsString();
		}
		return true;
	}

	static bool PrepareType(const TSharedPtr<FJsonObject>& Obj,
		FNwiroPreparedVariable& Out, FString& Error)
	{
		FString Base, Subtype, Container = TEXT("none");
		bool bSubtypePresent = false, bContainerPresent = false;
		if (!OptionalString(Obj, { TEXT("subtype"), TEXT("objectclass") },
			Subtype, bSubtypePresent, Error)
			|| !OptionalString(Obj, { TEXT("container") }, Container, bContainerPresent, Error)) return false;
		const TSharedPtr<FJsonValue>* TypeValue = Obj->Values.Find(TEXT("type"));
		if (!TypeValue || !TypeValue->IsValid()) { Error = TEXT("missing required 'type'"); return false; }
		if ((*TypeValue)->Type == EJson::String) Base = (*TypeValue)->AsString();
		else if ((*TypeValue)->Type == EJson::Object)
		{
			if (bSubtypePresent || bContainerPresent)
			{
				Error = TEXT("object type keeps subtype/container inside type; do not mix top-level fields");
				return false;
			}
			const TSharedPtr<FJsonObject> Type = (*TypeValue)->AsObject();
			if (!Type.IsValid()) { Error = TEXT("type object is invalid"); return false; }
			for (const auto& Pair : Type->Values)
			{
				if (Pair.Key != TEXT("base") && Pair.Key != TEXT("subtype") && Pair.Key != TEXT("container"))
				{ Error = TEXT("type accepts only base, subtype, container; maps are unsupported"); return false; }
			}
			if (!Type->HasTypedField<EJson::String>(TEXT("base")) || !Type->TryGetStringField(TEXT("base"), Base)) { Error = TEXT("type.base must be a string"); return false; }
			if (!OptionalString(Type, { TEXT("subtype") }, Subtype, bSubtypePresent, Error)
				|| !OptionalString(Type, { TEXT("container") }, Container, bContainerPresent, Error)) return false;
		}
		else { Error = TEXT("type must be a string or {base, subtype, container} object"); return false; }
		if (Base.IsEmpty() || Base.TrimStartAndEnd() != Base || Subtype.TrimStartAndEnd() != Subtype
			|| Container.TrimStartAndEnd() != Container || Base.Len() >= NAME_SIZE || Subtype.Len() >= NAME_SIZE || Container.Len() >= NAME_SIZE)
		{ Error = TEXT("type strings must be nonblank, within Unreal's name limit, and have no surrounding whitespace"); return false; }
		for (const FString* TypeString : { &Base, &Subtype, &Container })
		{
			for (TCHAR Character : *TypeString)
			{
				if (FChar::IsControl(Character)) { Error = TEXT("type strings cannot contain control characters"); return false; }
			}
		}
		Out.RequestedType = Base;
		Base = Base.ToLower();
		Container = Container.ToLower();
		if (Container != TEXT("none") && Container != TEXT("array") && Container != TEXT("set"))
		{ Error = TEXT("container must be none, array, or set; map variables require a value-type contract and are unsupported"); return false; }
		const bool bReference = Base == TEXT("object") || Base == TEXT("class") || Base == TEXT("struct") || Base == TEXT("enum");
		if (bReference && (!bSubtypePresent || Subtype.IsEmpty()))
		{ Error = FString::Printf(TEXT("type '%s' requires an explicit resolvable subtype"), *Base); return false; }
		if (!bReference && bSubtypePresent)
		{ Error = FString::Printf(TEXT("type '%s' does not accept subtype"), *Base); return false; }
		FEdGraphPinType& Pin = Out.PinType;
		if (Base == TEXT("bool") || Base == TEXT("boolean")) Pin.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		else if (Base == TEXT("byte")) Pin.PinCategory = UEdGraphSchema_K2::PC_Byte;
		else if (Base == TEXT("int") || Base == TEXT("integer") || Base == TEXT("int32")) Pin.PinCategory = UEdGraphSchema_K2::PC_Int;
		else if (Base == TEXT("int64")) Pin.PinCategory = UEdGraphSchema_K2::PC_Int64;
		else if (Base == TEXT("float") || Base == TEXT("double"))
		{ Pin.PinCategory = UEdGraphSchema_K2::PC_Real; Pin.PinSubCategory = FName(*Base); }
		else if (Base == TEXT("string") || Base == TEXT("fstring")) Pin.PinCategory = UEdGraphSchema_K2::PC_String;
		else if (Base == TEXT("name") || Base == TEXT("fname")) Pin.PinCategory = UEdGraphSchema_K2::PC_Name;
		else if (Base == TEXT("text") || Base == TEXT("ftext")) Pin.PinCategory = UEdGraphSchema_K2::PC_Text;
		else
		{
			UObject* Resolved = nullptr;
			if (Base == TEXT("vector") || Base == TEXT("fvector")) Resolved = TBaseStructure<FVector>::Get();
			else if (Base == TEXT("rotator") || Base == TEXT("frotator")) Resolved = TBaseStructure<FRotator>::Get();
			else if (Base == TEXT("transform") || Base == TEXT("ftransform")) Resolved = TBaseStructure<FTransform>::Get();
			else if (Base == TEXT("linearcolor") || Base == TEXT("color")) Resolved = TBaseStructure<FLinearColor>::Get();
			else if (Base == TEXT("object") || Base == TEXT("class")) Resolved = NwiroTypeValidation::ResolveLoadedType(Subtype, UClass::StaticClass(), Error);
			else if (Base == TEXT("struct")) Resolved = NwiroTypeValidation::ResolveLoadedType(Subtype, UScriptStruct::StaticClass(), Error);
			else if (Base == TEXT("enum")) Resolved = NwiroTypeValidation::ResolveLoadedType(Subtype, UEnum::StaticClass(), Error);
			else
			{
				Resolved = NwiroTypeValidation::ResolveLoadedType(Out.RequestedType, nullptr, Error);
			}
			if (const UClass* Class = Cast<UClass>(Resolved))
			{
				if (Class->HasAnyClassFlags(CLASS_Interface | CLASS_Deprecated | CLASS_NewerVersionExists) || !UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Class))
				{ Error = TEXT("resolved class is not an eligible Blueprint variable type"); return false; }
				Pin.PinCategory = Base == TEXT("class") ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
			}
			else if (const UScriptStruct* Struct = Cast<UScriptStruct>(Resolved))
			{
				if (!UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Struct)
					|| FStructureEditorUtils::IsStructureValid(Struct, nullptr, &Error) != FStructureEditorUtils::EStructureError::Ok)
				{ Error = TEXT("resolved struct is not a valid compiled Blueprint variable type"); return false; }
				Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
			}
			else if (const UEnum* Enum = Cast<UEnum>(Resolved))
			{
				if (!UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Enum))
				{ Error = TEXT("resolved enum is not an eligible Blueprint variable type"); return false; }
				Pin.PinCategory = UEdGraphSchema_K2::PC_Byte;
			}
			else { if (!Error.IsEmpty()) return false; Error = FString::Printf(TEXT("unresolved or ambiguous type '%s' subtype '%s'; supply the exact class/struct/enum object path"), *Out.RequestedType, *Subtype); return false; }
			if (Resolved->GetOutermost() == GetTransientPackage()
				|| (Resolved->HasAnyFlags(RF_Transient) && !Resolved->IsNative()))
			{ Error = TEXT("transient type dependencies cannot be used by persistable Blueprint variables"); return false; }
			Pin.PinSubCategoryObject = Resolved;
			Out.SubtypeHold.Reset(Resolved);
		}
		Pin.ContainerType = Container == TEXT("array") ? EPinContainerType::Array
			: Container == TEXT("set") ? EPinContainerType::Set : EPinContainerType::None;
		if (!FBlueprintEditorUtils::IsPinTypeValid(Pin)
			|| (Pin.ContainerType == EPinContainerType::Set && !FBlueprintEditorUtils::HasGetTypeHash(Pin)))
		{ Error = TEXT("Unreal rejected the variable type; set elements must support GetTypeHash"); return false; }
		return true;
	}

	static bool PrepareOne(const TSharedPtr<FJsonValue>& Item, FNwiroPreparedVariable& Out,
		FString& Code, FString& Field, FString& Error)
	{
		Code = TEXT("NOT_AN_OBJECT");
		if (!Item.IsValid() || Item->Type != EJson::Object || !Item->AsObject().IsValid())
		{ Error = TEXT("each variable must be an object with name and type"); return false; }
		const TSharedPtr<FJsonObject> Obj = Item->AsObject();
		FString Name;
		Field = TEXT("name"); Code = TEXT("INVALID_NAME");
		if (!Obj->HasTypedField<EJson::String>(TEXT("name")) || !Obj->TryGetStringField(TEXT("name"), Name) || Name.TrimStartAndEnd().IsEmpty()
			|| Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			|| Name.Len() >= NAME_SIZE || Name.Len() > FKismetNameValidator::GetMaximumNameLength())
		{ Error = TEXT("name must be a nonblank string within Unreal's Blueprint name limit and cannot be None"); return false; }
		for (TCHAR Character : Name)
		{
			if (FChar::IsControl(Character)) { Error = TEXT("variable names cannot contain control characters"); return false; }
		}
		FText NameError;
		if (!FName::IsValidXName(Name, FString(INVALID_NAME_CHARACTERS) + FString(UE_BLUEPRINT_INVALID_NAME_CHARACTERS), &NameError))
		{ Error = NameError.ToString(); return false; }
		Out.Name = FName(*Name);
		Field = TEXT("type"); Code = TEXT("INVALID_TYPE");
		if (!PrepareType(Obj, Out, Error)) return false;
		// Only fields this operation implements are accepted. This catches misspelled
		// options and map terminal descriptors before they can be silently dropped.
		const TSet<FString> Allowed = { TEXT("name"), TEXT("type"), TEXT("subtype"), TEXT("objectclass"),
			TEXT("container"), TEXT("default"), TEXT("defaultvalue"), TEXT("initialvalue"),
			TEXT("replicated"), TEXT("exposeonspawn"), TEXT("savegame"), TEXT("instanceeditable"), TEXT("category"), TEXT("tooltip") };
		TSet<FString> Seen;
		for (const auto& Pair : Obj->Values)
		{
			Field = FString(*Pair.Key); Code = TEXT("INVALID_OPTION");
			const FString Key = NormalizeKey(Field);
			if (!Allowed.Contains(Key) || Seen.Contains(Key))
			{ Error = FString::Printf(TEXT("unsupported or duplicate variable option '%s'"), *Field); return false; }
			Seen.Add(Key);
		}
		bool bPresent = false;
		Field = TEXT("default");
		if (!OptionalString(Obj, { TEXT("default"), TEXT("defaultvalue"), TEXT("initialvalue") }, Out.DefaultValue, bPresent, Error)) return false;
		FString Value;
		Field = TEXT("category");
		if (!OptionalString(Obj, { TEXT("category") }, Value, bPresent, Error)) return false;
		if (bPresent) Out.Category = Value;
		Field = TEXT("tooltip");
		if (!OptionalString(Obj, { TEXT("tooltip") }, Value, bPresent, Error)) return false;
		if (bPresent) Out.Tooltip = Value;
		NwiroVariableEditabilityAdmission::FEditabilityOptions Options;
		TArray<TSharedPtr<FJsonValue>> OptionErrors;
		if (!NwiroVariableEditabilityAdmission::DecodeItemOptions(Obj, TEXT("add_variables"), INDEX_NONE, Options, OptionErrors))
		{
			const auto First = OptionErrors[0]->AsObject();
			Code = First->GetStringField(TEXT("code"));
			Field = First->GetStringField(TEXT("field"));
			Error = First->GetStringField(TEXT("message"));
			return false;
		}
		Out.InstanceEditable = Options.InstanceEditable;
		Out.bExposeOnSpawn = Options.ExposeOnSpawn.Get(false);
		for (const auto& Pair : Obj->Values)
		{
			const FString Key = NormalizeKey(FString(*Pair.Key));
			bool* Flag = Key == TEXT("replicated") ? &Out.bReplicated : Key == TEXT("savegame") ? &Out.bSaveGame : nullptr;
			if (Flag && (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Boolean))
			{ Field = FString(*Pair.Key); Error = Field + TEXT(" must be a boolean"); return false; }
			if (Flag) *Flag = Pair.Value->AsBool();
		}
		if (Out.bReplicated && Out.PinType.ContainerType == EPinContainerType::Set)
		{ Field = TEXT("replicated"); Error = TEXT("Unreal does not support replicated set properties"); return false; }
		return true;
	}

	static bool Prepare(const TArray<TSharedPtr<FJsonValue>>& Items,
		TArray<FNwiroPreparedVariable>& Out, FNwiroUnitLedger& Ledger)
	{
		Out.SetNum(Items.Num());
		TSet<FName> Names;
		for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
		{
			FString Code, Field, Error;
			if (!PrepareOne(Items[Unit], Out[Unit], Code, Field, Error))
				Ledger.AddError(Unit, TEXT("add_variables"), Code, Field, Out[Unit].Name.ToString(), Error);
			else if (Names.Contains(Out[Unit].Name))
				Ledger.AddError(Unit, TEXT("add_variables"), TEXT("DUPLICATE_NAME"), TEXT("name"), Out[Unit].Name.ToString(), TEXT("duplicate variable name in this batch (case-insensitive)"));
			Names.Add(Out[Unit].Name);
		}
		return Ledger.Errors.IsEmpty();
	}

	static bool ValidateTarget(UBlueprint* BP, const TArray<FNwiroPreparedVariable>& Items, FNwiroUnitLedger& Ledger)
	{
		TSet<FName> Existing;
		FBlueprintEditorUtils::GetClassVariableList(BP, Existing, true);
		FKismetNameValidator Validator(BP);
		for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
		{
			if (Existing.Contains(Items[Unit].Name) || Validator.IsValid(Items[Unit].Name) != EValidatorResult::Ok)
				Ledger.AddError(Unit, TEXT("add_variables"), TEXT("ALREADY_EXISTS"), TEXT("name"), Items[Unit].Name.ToString(), TEXT("name collides with an existing/inherited member or graph"));
		}
		return Ledger.Errors.IsEmpty();
	}

	static FString RejectedResponse(UBlueprint* BP, const FString& BPName,
		const TArray<FNwiroPreparedVariable>& Items, FNwiroUnitLedger Ledger,
		const FString& Message, bool bMutationAttempted = false)
	{
		const TSet<int32> Accounted = Ledger.AccountedUnits();
		for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
		{
			if (Accounted.Contains(Unit)) continue;
			auto Entry = FNwiroUnitLedger::MakeUnit(Unit, TEXT("add_variables"), Items[Unit].Name.ToString(), false);
			Entry->AsObject()->SetStringField(TEXT("reason"), TEXT("variable batch rejected"));
			Ledger.Skipped.Add(Entry);
		}
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetBoolField(TEXT("compiled"), false);
		Result->SetBoolField(TEXT("savedToDisk"), false);
		Result->SetStringField(TEXT("compileStatus"), TEXT("blocked"));
		Result->SetStringField(TEXT("validationLevel"), bMutationAttempted ? TEXT("uncertain") : TEXT("preflight"));
		Result->SetStringField(TEXT("blueprint"), BP ? BP->GetName() : BPName);
		Result->SetStringField(TEXT("error"), Message);
		Result->SetBoolField(TEXT("mutationAttempted"), bMutationAttempted);
		Result->SetArrayField(TEXT("messages"), { MakeShared<FJsonValueString>(Message) });
		Result->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Ledger.Build(TEXT("add_variables"), Items.Num())) });
		TArray<TSharedPtr<FJsonValue>> Dirty;
		if (BP && BP->GetOutermost()->IsDirty()) Dirty.Add(MakeShared<FJsonValueString>(BP->GetOutermost()->GetName()));
		if (BP) Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
		else Result->SetField(TEXT("dirtyPackages"), MakeShared<FJsonValueNull>());
		Result->SetBoolField(TEXT("targetStateObserved"), BP != nullptr);
		FString Json;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}
}
