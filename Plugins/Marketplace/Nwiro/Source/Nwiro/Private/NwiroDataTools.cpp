// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroDataTools.h"
#include "NwiroTypeValidation.h"
#include "NwiroAssetGuard.h"
#include "NwiroTransactionHelper.h"
#include "Engine/DataTable.h"
#include "DataTableUtils.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/CoreRedirects.h"
#include "UObject/EnumProperty.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/DataTableFactory.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Json.h"

#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "CoreGlobals.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/Change.h"
#include "Misc/ITransaction.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroData, Log, All);

// Shared validation/publication for the two create-only handlers below.
namespace NwiroDataCreation
{
struct FRequestedField
{
	FString Name;
	FEdGraphPinType Type;
	TStrongObjectPtr<UObject> ReferencedType;
};

static FString ErrorResponse(const FString& Error)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Error);
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

static FString SuccessResponse(UObject* Asset, const TCHAR* CountKey, int32 Count)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Asset->GetName());
	Result->SetStringField(TEXT("path"), Asset->GetPathName());
	Result->SetNumberField(CountKey, Count);
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

static bool CheckTargetAvailable(const FString& PackagePath, const FString& Name, FString& Error)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FSoftObjectPath ObjectPath(PackagePath + TEXT(".") + Name);
	if (FindPackage(nullptr, *PackagePath) || FPackageName::DoesPackageExist(PackagePath) ||
		Registry.GetAssetByObjectPath(ObjectPath).IsValid())
	{
		Error = FString::Printf(TEXT("Asset package '%s' already exists. Choose a new name or path."), *PackagePath);
		return false;
	}
	return true;
}

static bool ReadCreateRequest(const FString& JsonCommand, TSharedPtr<FJsonObject>& Cmd,
	FString& Name, FString& PackagePath, FString& Error)
{
	if (!IsInGameThread())
	{
		Error = TEXT("Asset creation must run on the editor game thread.");
		return false;
	}
	// Canceling a nested Unreal transaction cancels the caller's entire transaction.
	if (!GEditor || GIsTransacting || GEditor->IsTransactionActive())
	{
		Error = TEXT("Asset creation requires an idle editor transaction; finish the active edit first.");
		return false;
	}
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
	{
		Error = TEXT("Invalid JSON object.");
		return false;
	}
	if (!Cmd->HasTypedField<EJson::String>(TEXT("name")) || !Cmd->TryGetStringField(TEXT("name"), Name) ||
		Name.TrimStartAndEnd().IsEmpty() || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Name.Len() >= NAME_SIZE)
	{
		Error = TEXT("'name' must be a nonempty Unreal asset name.");
		return false;
	}
	FText Reason;
	if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &Reason))
	{
		Error = Reason.ToString();
		return false;
	}
	FString Path = TEXT("/Game/Data");
	if (Cmd->HasField(TEXT("path")))
	{
		if (!Cmd->HasTypedField<EJson::String>(TEXT("path")) || !Cmd->TryGetStringField(TEXT("path"), Path))
		{
			Error = TEXT("'path' must be a content folder string.");
			return false;
		}
		if (Path.IsEmpty()) Path = TEXT("/Game/Data");
	}
	PackagePath = Path / Name;
	// Package validation checks syntax, but FName construction also has a hard length limit.
	if (PackagePath.Len() >= NAME_SIZE)
	{
		Error = TEXT("Asset package path is too long for an Unreal name.");
		return false;
	}
	if (!FPackageName::IsValidLongPackageName(PackagePath, false, &Reason))
	{
		Error = Reason.ToString();
		return false;
	}
	return CheckTargetAvailable(PackagePath, Name, Error);
}

// Publication only moves the completed asset. Serializing a populated enum can
// assert, and reloading a compiled struct can append duplicate reflected fields.
class FAssetPublicationChange final : public FCommandChange
{
public:
	FAssetPublicationChange(FName InStagingName, UPackage* InPackage, FName InAssetName)
		: StagingName(InStagingName), Package(InPackage), AssetName(InAssetName) {}

	virtual void Apply(UObject* Object) override
	{
		if (Move(Object, Package.Get(), AssetName))
		{
			Object->SetFlags(RF_Standalone);
			FAssetRegistryModule::AssetCreated(Object);
			Object->MarkPackageDirty();
		}
	}

	virtual void Revert(UObject* Object) override
	{
		if (!CanMove(Object, GetTransientPackage(), StagingName)) return;
		FAssetRegistryModule::AssetDeleted(Object);
		if (Move(Object, GetTransientPackage(), StagingName))
			Object->ClearFlags(RF_Standalone);
		else
			FAssetRegistryModule::AssetCreated(Object);
	}

	virtual bool HasExpired(UObject* Object) const override { return !IsValid(Object); }
	virtual FString ToString() const override { return TEXT("Publish completed data asset"); }

private:
	static bool CanMove(UObject* Object, UPackage* Outer, FName Name)
	{
		if (!ensure(Object && Outer)) return false;
		UObject* Existing = StaticFindObject(UObject::StaticClass(), Outer, *Name.ToString());
		return ensureMsgf(!Existing || Existing == Object, TEXT("Cannot restore asset publication over another object."));
	}

	static bool Move(UObject* Object, UPackage* Outer, FName Name)
	{
		if (!CanMove(Object, Outer, Name) || !ensureMsgf(Object->Rename(*Name.ToString(), Outer,
			REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional), TEXT("Failed to restore asset publication.")))
			return false;
		if (UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Object))
			CastChecked<UUserDefinedStructEditorData>(Struct->EditorData)->ReinitializeDefaultInstance();
		return true;
	}

	FName StagingName;
	TStrongObjectPtr<UPackage> Package;
	FName AssetName;
};

static bool PublishAsset(UObject* Asset, const FString& Name, const FString& PackagePath, FString& Error)
{
	// Recheck after editor callbacks: never rename over a loaded or unloaded asset.
	if (!CheckTargetAvailable(PackagePath, Name, Error)) return false;
	if (!GUndo)
	{
		Error = TEXT("Could not start an editor transaction for asset publication.");
		return false;
	}
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		Error = TEXT("Failed to create asset package.");
		return false;
	}
	const FName StagingName = Asset->GetFName();
	const ERenameFlags Flags = REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional;
	if (!Asset->Rename(*Name, Package, Flags))
	{
		Error = TEXT("Failed to move the completed asset to its requested package.");
		// Garbage objects remain findable until GC. Vacate only this newly allocated,
		// empty package so a corrected request can reuse the target immediately.
		TArray<UObject*> Contents;
		GetObjectsWithOuter(Package, Contents);
		const FString DiscardedPath = TEXT("/Temp/NwiroDiscarded_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (Contents.IsEmpty() && Package->Rename(*DiscardedPath, nullptr,
			REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional))
			Package->MarkAsGarbage();
		else
			Error += TEXT(" The newly allocated package could not be released.");
		return false;
	}
	GUndo->StoreUndo(Asset, MakeUnique<FAssetPublicationChange>(StagingName, Package, FName(*Name)));
	return true;
}

static bool ResolveStructFieldType(const FString& FieldType, FEdGraphPinType& OutPinType,
	FString& OutError)
{
	OutPinType = FEdGraphPinType();
	OutError.Reset();

	const FString TypeLower = FieldType.ToLower();
	if (TypeLower == TEXT("bool") || TypeLower == TEXT("boolean"))
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	else if (TypeLower == TEXT("int") || TypeLower == TEXT("integer"))
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	else if (TypeLower == TEXT("float"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = TEXT("float");
	}
	else if (TypeLower == TEXT("double") || TypeLower == TEXT("real"))
	{
		// "real" is an alias defaulting to highest precision (double).
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = TEXT("double");
	}
	else if (TypeLower == TEXT("string"))
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
	else if (TypeLower == TEXT("name"))
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	else if (TypeLower == TEXT("text"))
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	else if (TypeLower == TEXT("vector"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (TypeLower == TEXT("rotator"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (TypeLower == TEXT("transform"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (TypeLower == TEXT("color") || TypeLower == TEXT("linearcolor"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else
	{
		OutError = FString::Printf(
			TEXT("Unsupported field type '%s'. Supported: bool, int, float, double, real, string, "
			     "name, text, vector, rotator, transform, color."), *FieldType);
		return false;
	}

	return true;
}


// Normalize the two documented forms before staging any asset. No type lookup loads content.
static bool ResolveRequestedStructFieldType(const TSharedPtr<FJsonObject>& Field,
	FEdGraphPinType& OutPinType, FString& Error)
{
	OutPinType = FEdGraphPinType();
	Error.Reset();
	TSharedPtr<FJsonObject> TypeOptions = Field;
	FString Base;
	if (Field->HasTypedField<EJson::String>(TEXT("type")))
	{
		Field->TryGetStringField(TEXT("type"), Base);
	}
	else if (Field->HasTypedField<EJson::Object>(TEXT("type")))
	{
		TypeOptions = Field->GetObjectField(TEXT("type"));
		if (Field->HasField(TEXT("subtype")) || Field->HasField(TEXT("container")))
		{
			Error = TEXT("Put subtype and container inside the type object, or use a string type with sibling options; do not mix forms.");
			return false;
		}
		for (const auto& Option : TypeOptions->Values)
		{
			if (Option.Key != TEXT("base") && Option.Key != TEXT("subtype") && Option.Key != TEXT("container"))
			{
				Error = FString::Printf(TEXT("Unknown type option '%s'."), *Option.Key);
				return false;
			}
		}
		if (!TypeOptions->HasTypedField<EJson::String>(TEXT("base")) || !TypeOptions->TryGetStringField(TEXT("base"), Base))
		{
			Error = TEXT("A type object requires a string base.");
			return false;
		}
	}
	else
	{
		Error = TEXT("type must be a primitive string or an object with base, optional subtype, and optional container.");
		return false;
	}
	if (Base.IsEmpty() || Base.Len() >= NAME_SIZE || Base != Base.TrimStartAndEnd())
	{
		Error = TEXT("Type base must be a nonempty name without surrounding whitespace and shorter than NAME_SIZE.");
		return false;
	}
	for (TCHAR Character : Base)
	{
		if (FChar::IsControl(Character))
		{
			Error = TEXT("Type base must not contain control characters.");
			return false;
		}
	}
	Base = Base.ToLower();
	FString Container = TEXT("none");
	if (TypeOptions->HasField(TEXT("container")) &&
		(!TypeOptions->HasTypedField<EJson::String>(TEXT("container")) || !TypeOptions->TryGetStringField(TEXT("container"), Container)))
	{
		Error = TEXT("container must be a string: none or array.");
		return false;
	}
	if (!Container.Equals(TEXT("none"), ESearchCase::IgnoreCase) && !Container.Equals(TEXT("array"), ESearchCase::IgnoreCase))
	{
		Error = TEXT("Struct fields support only none and array containers; set and map are unsupported.");
		return false;
	}
	const bool bReference = Base == TEXT("class") || Base == TEXT("object") || Base == TEXT("struct") || Base == TEXT("enum");
	if (bReference)
	{
		FString Subtype;
		if (!TypeOptions->HasTypedField<EJson::String>(TEXT("subtype")) || !TypeOptions->TryGetStringField(TEXT("subtype"), Subtype))
		{
			Error = TEXT("class, object, struct, and enum require a string subtype naming a loaded type.");
			return false;
		}
		UClass* Kind = Base == TEXT("struct") ? UScriptStruct::StaticClass() :
			Base == TEXT("enum") ? UEnum::StaticClass() : UClass::StaticClass();
		UObject* Resolved = NwiroTypeValidation::ResolveLoadedType(Subtype, Kind, Error);
		if (!Resolved) return false;
		// Native reflected types are valid package imports even though native UClass carries RF_Transient.
		if (Resolved->GetOutermost() == GetTransientPackage() ||
			(Resolved->HasAnyFlags(RF_Transient) && !Resolved->IsNative()))
		{
			Error = TEXT("Subtype must belong to a persistent package; transient types cannot be referenced by a saved struct.");
			return false;
		}
		if (UClass* Class = Cast<UClass>(Resolved))
		{
			if (Class->HasAnyClassFlags(CLASS_Interface | CLASS_Deprecated | CLASS_NewerVersionExists) ||
				!UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Class))
			{
				Error = TEXT("Class subtype must be a current Blueprint variable class; interfaces require a distinct interface type.");
				return false;
			}
			OutPinType.PinCategory = Base == TEXT("class") ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
		}
		else if (UScriptStruct* Struct = Cast<UScriptStruct>(Resolved))
		{
			if (!UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Struct) ||
				FStructureEditorUtils::IsStructureValid(Struct, nullptr, &Error) != FStructureEditorUtils::EStructureError::Ok)
			{
				if (Error.IsEmpty()) Error = TEXT("Struct subtype is not a valid Blueprint variable struct.");
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		}
		else if (UEnum* Enum = Cast<UEnum>(Resolved))
		{
			if (!UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Enum))
			{
				Error = TEXT("Enum subtype is not a Blueprint variable enum.");
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		OutPinType.PinSubCategoryObject = Resolved;
	}
	else
	{
		if (TypeOptions->HasField(TEXT("subtype")))
		{
			Error = TEXT("Primitive types do not accept subtype; use class, object, struct, or enum for a reference.");
			return false;
		}
		if (!ResolveStructFieldType(Base, OutPinType, Error)) return false;
	}
	OutPinType.ContainerType = Container.Equals(TEXT("array"), ESearchCase::IgnoreCase) ? EPinContainerType::Array : EPinContainerType::None;
	return true;
}

}

// ============================================================
// CREATE DATA TABLE
// ============================================================

FString FNwiroDataTools::CreateDataTable(const FString& JsonCommand)
{
	using namespace NwiroDataCreation;
	TSharedPtr<FJsonObject> Cmd;
	FString Name, PackagePath, Error;
	if (!ReadCreateRequest(JsonCommand, Cmd, Name, PackagePath, Error)) return ErrorResponse(Error);
	if (GEditor->PlayWorld) return ErrorResponse(TEXT("DataTable creation requires an idle editor outside PIE."));
	FString RowStructName;
	if (!Cmd->HasTypedField<EJson::String>(TEXT("rowStruct")) || !Cmd->TryGetStringField(TEXT("rowStruct"), RowStructName) || RowStructName.IsEmpty())
		return ErrorResponse(TEXT("A nonempty string rowStruct is required."));

	// Find the row struct — accepts bare name, /Script/Engine shorthand, OR
	// full content path. LoadObject fallback for path-shaped input handles
	// the case where the struct isn't already loaded in memory.
	UScriptStruct* RowStruct = nullptr;
	if (!RowStructName.IsEmpty())
	{
		if (RowStructName.StartsWith(TEXT("/")))
		{
			FString RowPackagePath = RowStructName;
			int32 Dot; if (RowPackagePath.FindLastChar('.', Dot)) RowPackagePath = RowPackagePath.Left(Dot);
			if (UObject* Loaded = LoadObject<UObject>(nullptr, *RowPackagePath))
			{
				RowStruct = Cast<UScriptStruct>(Loaded);
			}
		}
		if (!RowStruct) RowStruct = FindObject<UScriptStruct>(nullptr, *RowStructName);
		if (!RowStruct) RowStruct = FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *RowStructName));
		if (!RowStruct)
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter Filter;
			Filter.ClassPaths.Add(UUserDefinedStruct::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			Filter.PackagePaths.Add(TEXT("/Game"));
			TArray<FAssetData> Assets;
			ARM.Get().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				if (A.AssetName.ToString().Equals(RowStructName, ESearchCase::IgnoreCase))
				{
					RowStruct = Cast<UScriptStruct>(A.GetAsset());
					break;
				}
			}
		}
	}

	if (!RowStruct)
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Row struct '%s' not found (or 'rowStruct' arg missing). Pass either a bare struct name like 'MyRowStruct' or a full asset path like '/Game/Data/S_MyRow'.\"}"),
			*RowStructName.Replace(TEXT("\""), TEXT("\\\"")));
	}

	TStrongObjectPtr<UScriptStruct> KeepRowStruct(RowStruct);
	TStrongObjectPtr<UDataTableFactory> Factory(NewObject<UDataTableFactory>());
	Factory->Struct = RowStruct;
	// NewObject records RF_Transactional creation while GUndo is active, before
	// UDataTableFactory can assign RowStruct. Build first, then publish the fully
	// configured asset with the existing move-only creation undo/redo change.
	TStrongObjectPtr<UDataTable> Table(Cast<UDataTable>(Factory->FactoryCreateNew(
		UDataTable::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UDataTable::StaticClass()),
		RF_Public | RF_Transactional, nullptr, GWarn)));
	if (!Table.IsValid()) return ErrorResponse(TEXT("Failed to create DataTable."));
	bool bPublished = false;
	ON_SCOPE_EXIT
	{
		if (!bPublished)
		{
			Table->ClearFlags(RF_Public | RF_Standalone);
			Table->MarkAsGarbage();
		}
	};
	if (Table->GetRowStruct() != RowStruct)
		return ErrorResponse(TEXT("DataTable factory did not bind the requested row struct."));

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateDataTable", "AI: Create DataTable"));
	if (!PublishAsset(Table.Get(), Name, PackagePath, Error))
	{
		Tx.Cancel();
		return ErrorResponse(Error);
	}
	Table->SetFlags(RF_Standalone);
	FAssetRegistryModule::AssetCreated(Table.Get());
	Table->MarkPackageDirty();
	bPublished = true;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("created"), true);
	Result->SetStringField(TEXT("name"), Table->GetName());
	Result->SetStringField(TEXT("path"), Table->GetPathName());
	Result->SetStringField(TEXT("rowStruct"), RowStruct->GetName());
	Result->SetStringField(TEXT("rowStructPath"), RowStruct->GetPathName());
	Result->SetBoolField(TEXT("savedToDisk"), false);
	Result->SetBoolField(TEXT("dirtyAfter"), Table->GetOutermost()->IsDirty());
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

// ============================================================
// ADD DATA TABLE ROW
// ============================================================

FString FNwiroDataTools::AddDataTableRow(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString TablePath;
	for (const TCHAR* K : { TEXT("table"), TEXT("path"), TEXT("assetPath"), TEXT("dataTable"), TEXT("tablePath") }) {
		if (Cmd->TryGetStringField(K, TablePath) && !TablePath.IsEmpty()) break;
	}
	FString RowName = Cmd->GetStringField(TEXT("rowName"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(TablePath);
	UDataTable* DT = Cast<UDataTable>(Asset);
	if (!DT)
	{
		// Search by name
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FARFilter Filter;
		Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		TArray<FAssetData> Assets;
		ARM.Get().GetAssets(Filter, Assets);
		for (const FAssetData& A : Assets)
		{
			if (A.AssetName.ToString().Contains(TablePath, ESearchCase::IgnoreCase))
			{
				DT = Cast<UDataTable>(A.GetAsset());
				break;
			}
		}
	}

	if (!DT) return FString::Printf(TEXT("{\"success\":false,\"error\":\"DataTable not found: %s\"}"), *TablePath);
	if (RowName.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'rowName'\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddDataTableRow", "AI: Add DataTable Row"), DT);

	// Add row via JSON. CreateTableFromJSONString REPLACES all rows, so we
	// gather existing rows first and rewrite the full set with the new one
	// appended (or overwriting on rowName collision when bUpdate is set).
	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	if (Cmd->TryGetObjectField(TEXT("values"), ValuesObj) && ValuesObj && ValuesObj->IsValid())
	{
		bool bUpdate = false;
		Cmd->TryGetBoolField(TEXT("update"), bUpdate);

		FString NewRowInner;
		TSharedRef<TJsonWriter<>> JW = TJsonWriterFactory<>::Create(&NewRowInner);
		FJsonSerializer::Serialize((*ValuesObj).ToSharedRef(), JW);
		const FString NewRowFields = NewRowInner.Mid(1, NewRowInner.Len() - 2);

		const FString ExistingJson = DT->GetTableAsJSON();
		TArray<TSharedPtr<FJsonValue>> ExistingArr;
		{
			TSharedRef<TJsonReader<>> RR = TJsonReaderFactory<>::Create(ExistingJson);
			FJsonSerializer::Deserialize(RR, ExistingArr);
		}

		TArray<FString> RowJsons;
		RowJsons.Reserve(ExistingArr.Num() + 1);
		for (const TSharedPtr<FJsonValue>& V : ExistingArr)
		{
			const TSharedPtr<FJsonObject>& Obj = V->AsObject();
			if (!Obj.IsValid()) continue;
			FString ExistingName;
			Obj->TryGetStringField(TEXT("Name"), ExistingName);
			if (bUpdate && ExistingName.Equals(RowName)) continue;
			FString Buf;
			TSharedRef<TJsonWriter<>> RW = TJsonWriterFactory<>::Create(&Buf);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), RW);
			RowJsons.Add(Buf);
		}
		RowJsons.Add(FString::Printf(TEXT("{\"Name\":\"%s\",%s}"), *RowName, *NewRowFields));

		const FString JsonString = FString::Printf(TEXT("[%s]"), *FString::Join(RowJsons, TEXT(",")));
		DT->CreateTableFromJSONString(JsonString);
	}

	DT->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"table\":\"%s\",\"row\":\"%s\",\"rowCount\":%d}"),
		*DT->GetName(), *RowName, DT->GetRowMap().Num());
}

// ============================================================
// READ DATA TABLE
// ============================================================

FString FNwiroDataTools::ReadDataTable(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString TablePath;
	for (const TCHAR* K : { TEXT("table"), TEXT("path"), TEXT("assetPath"), TEXT("dataTable"), TEXT("tablePath") }) {
		if (Cmd->TryGetStringField(K, TablePath) && !TablePath.IsEmpty()) break;
	}

	UObject* Asset = UEditorAssetLibrary::LoadAsset(TablePath);
	UDataTable* DT = Cast<UDataTable>(Asset);
	if (!DT)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"DataTable not found: %s\"}"), *TablePath);

	FString JsonOut = DT->GetTableAsJSON();

	return FString::Printf(TEXT("{\"success\":true,\"table\":\"%s\",\"rowStruct\":\"%s\",\"rowCount\":%d,\"rows\":%s}"),
		*DT->GetName(),
		DT->RowStruct ? *DT->RowStruct->GetName() : TEXT("None"),
		DT->GetRowMap().Num(),
		*JsonOut);
}

// ============================================================
// IMPORT DATA TABLE JSON
// ============================================================

// JSON import is deliberately stricter than FJsonValue's coercive TryGet helpers.
// Keep Unreal's import names and string literals; do not duplicate its value parser.
namespace NwiroDataImport
{
static FString ValidateStructTypes(const TSharedPtr<FJsonObject>& Object, const UScriptStruct* Struct, const FString& Path, int32 Depth);
static FString ValidateValueType(const TSharedPtr<FJsonValue>& Value, const FProperty* Property, const FString& Path, int32 Depth)
{
	if (Depth > 64 || !Value.IsValid()) return Path + TEXT(": invalid value or excessive nesting.");
	const FNumericProperty* NumberProperty = CastField<FNumericProperty>(Property);
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		if (Value->Type == EJson::String) return FString(); // Native enum-name validation follows.
		NumberProperty = EnumProperty->GetUnderlyingProperty();
	}
	if (NumberProperty)
	{
		if (NumberProperty->IsEnum() && Value->Type == EJson::String) return FString();
		double Number = 0;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
			return Path + TEXT(": expected a finite JSON number, not a coerced string or boolean.");
		if (NumberProperty->IsInteger())
		{
			int64 Integer = 0;
			const bool Unsigned = NumberProperty->IsA<FByteProperty>() || NumberProperty->IsA<FUInt16Property>()
				|| NumberProperty->IsA<FUInt32Property>() || NumberProperty->IsA<FUInt64Property>();
			// The native DataTable importer reads every integer through int64.
			if (FMath::FloorToDouble(Number) != Number || !Value->TryGetNumber(Integer)
				|| (Unsigned && Integer < 0) || !NumberProperty->CanHoldValue(Integer))
				return Path + TEXT(": expected an exact integer within the property's native range.");
		}
		else if (NumberProperty->IsA<FFloatProperty>() && FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()))
			return Path + TEXT(": number exceeds the property's finite float range.");
		return FString();
	}
	if (Property->IsA<FBoolProperty>())
		return Value->Type == EJson::Boolean ? FString() : Path + TEXT(": expected a JSON boolean.");
	const FArrayProperty* Array = CastField<FArrayProperty>(Property);
	const FSetProperty* Set = CastField<FSetProperty>(Property);
	if (Array || Set)
	{
		if (Value->Type != EJson::Array) return Path + TEXT(": expected a JSON array.");
		const FProperty* Inner = Array ? Array->Inner : Set->ElementProp;
		const auto& Items = Value->AsArray();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const FString Error = ValidateValueType(Items[Index], Inner, Path + FString::Printf(TEXT("[%d]"), Index), Depth + 1);
			if (!Error.IsEmpty()) return Error;
		}
		return FString();
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		if (Value->Type != EJson::Object || !Value->AsObject().IsValid()) return Path + TEXT(": expected a JSON object for a map.");
		for (const auto& Entry : Value->AsObject()->Values)
		{
			FString Key; Key = Entry.Key;
			const FString Error = ValidateValueType(Entry.Value, Map->ValueProp, Path + TEXT("[") + Key + TEXT("]"), Depth + 1);
			if (!Error.IsEmpty()) return Error;
		}
		return FString(); // Native import validates the map's serialized string keys.
	}
	if (const FStructProperty* Nested = CastField<FStructProperty>(Property))
	{
		if (Value->Type == EJson::Object && Value->AsObject().IsValid())
			return ValidateStructTypes(Value->AsObject(), Nested->Struct, Path, Depth + 1);
		// Vector/rotator/textual struct literals remain engine-supported import forms.
		return Value->Type == EJson::String ? FString() : Path + TEXT(": expected a struct object or native string literal.");
	}
	// Unreal imports text/name/string, object/class/soft references and remaining
	// supported scalar types through their native string import functions.
	return Value->Type == EJson::String ? FString() : Path + TEXT(": expected a native string representation.");
}
static FString ValidateStructTypes(const TSharedPtr<FJsonObject>& Object, const UScriptStruct* Struct, const FString& Path, int32 Depth)
{
	if (Depth > 64 || !Object.IsValid() || !Struct) return Path + TEXT(": invalid struct or excessive nesting.");
	if (Struct == TBaseStructure<FInstancedStruct>::Get())
	{
		// Match the native InstancedStruct export marker and redirect resolution.
		FString TypePath;
		if (!Object->HasField(TEXT("_structType"))) return FString();
		if (!Object->HasTypedField<EJson::String>(TEXT("_structType")) || !Object->TryGetStringField(TEXT("_structType"), TypePath))
			return Path + TEXT(": _structType must be a string.");
		if (TypePath.IsEmpty()) return FString();
		const FCoreRedirectObjectName OldName(TypePath);
		const FCoreRedirectObjectName NewName = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Struct, OldName, ECoreRedirectMatchFlags::AllowPartialMatch);
		if (OldName != NewName) TypePath = NewName.ToString();
		const UScriptStruct* Inner = LoadObject<UScriptStruct>(nullptr, *TypePath);
		return Inner ? ValidateStructTypes(Object, Inner, Path, Depth + 1) : Path + TEXT(": InstancedStruct type could not be resolved.");
	}
	TArray<FString> ImportNames;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FProperty* Property = *It;
		DataTableUtils::GetPropertyImportNames(Property, ImportNames);
		for (const FString& Name : ImportNames)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
			if (!Value.IsValid()) continue; // Native import options govern absent/extra fields.
			const FString PropertyPath = Path + TEXT(".") + Name;
			if (Property->ArrayDim == 1)
			{
				const FString Error = ValidateValueType(Value, Property, PropertyPath, Depth + 1);
				if (!Error.IsEmpty()) return Error;
			}
			else
			{
				if (Value->Type != EJson::Array || Value->AsArray().Num() != Property->ArrayDim)
					return PropertyPath + TEXT(": expected an array matching the property's fixed dimensions.");
				for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
				{
					const FString Error = ValidateValueType(Value->AsArray()[Index], Property, PropertyPath + FString::Printf(TEXT("[%d]"), Index), Depth + 1);
					if (!Error.IsEmpty()) return Error;
				}
			}
		}
	}
	return FString();
}
}

FString FNwiroDataTools::ImportDataTableJson(const FString& JsonCommand)
{
	using NwiroDataCreation::ErrorResponse;
	if (!IsInGameThread() || !GEditor || GEditor->PlayWorld || GIsTransacting || GEditor->IsTransactionActive())
		return ErrorResponse(TEXT("DataTable import requires an idle editor game thread outside PIE and active transactions."));

	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		return ErrorResponse(TEXT("Invalid JSON object."));
	FString TablePath;
	for (const TCHAR* Key : { TEXT("table"), TEXT("path"), TEXT("assetPath"), TEXT("dataTable"), TEXT("tablePath") })
	{
		if (Cmd->TryGetStringField(Key, TablePath) && !TablePath.IsEmpty()) break;
	}
	FString JsonData;
	if (TablePath.IsEmpty() || !Cmd->HasTypedField<EJson::String>(TEXT("json")) || !Cmd->TryGetStringField(TEXT("json"), JsonData))
		return ErrorResponse(TEXT("A DataTable path and a string json containing a nonempty array of rows are required."));

	TStrongObjectPtr<UDataTable> Table(Cast<UDataTable>(UEditorAssetLibrary::LoadAsset(TablePath)));
	if (!Table.IsValid()) return ErrorResponse(TEXT("DataTable not found: ") + TablePath);
	const bool bDirtyBefore = Table->GetOutermost()->IsDirty();
	const auto Response = [&](bool bSuccess, const TArray<FString>& Problems)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetStringField(TEXT("path"), Table->GetPathName());
		Result->SetStringField(TEXT("mode"), TEXT("replace"));
		Result->SetNumberField(TEXT("rowCount"), Table->GetRowMap().Num());
		Result->SetBoolField(TEXT("savedToDisk"), false);
		Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
		Result->SetBoolField(TEXT("dirtyAfter"), Table->GetOutermost()->IsDirty());
		TArray<TSharedPtr<FJsonValue>> Dirty;
		if (Table->GetOutermost()->IsDirty()) Dirty.Add(MakeShared<FJsonValueString>(Table->GetOutermost()->GetName()));
		Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
		if (!Problems.IsEmpty())
		{
			Result->SetStringField(TEXT("error"), FString::Join(Problems, TEXT("; ")));
			TArray<TSharedPtr<FJsonValue>> Details;
			for (const FString& Problem : Problems) Details.Add(MakeShared<FJsonValueString>(Problem));
			Result->SetArrayField(TEXT("problems"), Details);
		}
		FString Json;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
		return Json;
	};
	if (!IsValid(Table->GetRowStruct()))
		return Response(false, { TEXT("DataTable has no valid row struct.") });

	// Reject invalid row shapes before the engine's AsObject/GetStringField calls.
	// The engine importer empties its table before validating individual rows.
	TArray<TSharedPtr<FJsonValue>> Rows;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonData), Rows) || Rows.IsEmpty())
		return Response(false, { TEXT("json must be a valid, nonempty array of row objects.") });
	const FString KeyField = Table->ImportKeyField.IsEmpty() ? TEXT("Name") : Table->ImportKeyField;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (!Rows[Index].IsValid() || Rows[Index]->Type != EJson::Object || !Rows[Index]->AsObject().IsValid())
			return Response(false, { FString::Printf(TEXT("Row %d must be a JSON object."), Index) });
		const TSharedPtr<FJsonObject>& Row = Rows[Index]->AsObject();
		FString RowName;
		if (!Row->HasTypedField<EJson::String>(KeyField) || !Row->TryGetStringField(KeyField, RowName)
			|| RowName.IsEmpty() || RowName.Len() >= NAME_SIZE)
			return Response(false, { FString::Printf(TEXT("Row %d requires a nonempty string '%s' shorter than NAME_SIZE."), Index, *KeyField) });
		for (const auto& Field : Row->Values)
		{
			if (Field.Key.Len() >= NAME_SIZE)
				return Response(false, { FString::Printf(TEXT("Row %d contains a field name exceeding the Unreal name limit."), Index) });
		}
	}

	// Parse once into unregistered, nontransactional storage. Import options and
	// the exact row struct are preserved; any engine row/type diagnostic rejects
	// the whole replacement before touching the target or its undo history.
	TStrongObjectPtr<UDataTable> Staged(NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient));
	if (!Staged.IsValid() || !Staged->CopyImportOptions(Table.Get()))
		return Response(false, { TEXT("Could not prepare a temporary DataTable for validation.") });
	// Strict types are checked before staging can coerce strings, booleans,
	// fractional integers or narrow integer overflow without a diagnostic.
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FString Error = NwiroDataImport::ValidateStructTypes(Rows[Index]->AsObject(), Table->GetRowStruct(), FString::Printf(TEXT("Row[%d]"), Index), 0);
		if (!Error.IsEmpty()) return Response(false, { Error });
	}
	const TArray<FString> Problems = Staged->CreateTableFromJSONString(JsonData);
	if (!Problems.IsEmpty()) return Response(false, Problems);

	// Copy validated typed rows instead of destructively parsing the live table
	// again. The engine copy uses one change-notification scope; its only error
	// is a null source, ruled out above. Import hooks have run on Staged; ordinary
	// target row/change notifications run once after the complete copy.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "ImportDataTableJson", "AI: Import DataTable JSON"), Table.Get());
	const TArray<FString> CopyProblems = Table->CreateTableFromOtherTable(Staged.Get());
	if (!CopyProblems.IsEmpty()) return Response(false, CopyProblems);
	Table->MarkPackageDirty();
	return Response(true, {});
}

// ============================================================
// CREATE STRUCT
// ============================================================

FString FNwiroDataTools::CreateStruct(const FString& JsonCommand)
{
	using namespace NwiroDataCreation;
	TSharedPtr<FJsonObject> Cmd;
	FString Name, PackagePath, Error;
	if (!ReadCreateRequest(JsonCommand, Cmd, Name, PackagePath, Error)) return ErrorResponse(Error);

	const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
	if (!Cmd->TryGetArrayField(TEXT("fields"), Fields) || !Fields || Fields->IsEmpty())
		return ErrorResponse(TEXT("'fields' must be a nonempty array of objects with name and type."));

	TArray<FRequestedField> Requested;
	TSet<FString> Names;
	for (int32 Index = 0; Index < Fields->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Fields)[Index];
		if (!Value.IsValid() || Value->Type != EJson::Object)
			return ErrorResponse(FString::Printf(TEXT("Field %d must be an object."), Index));
		const TSharedPtr<FJsonObject>& Field = Value->AsObject();
		FRequestedField Request;
		if (!Field.IsValid() || !Field->HasTypedField<EJson::String>(TEXT("name")) ||
			!Field->TryGetStringField(TEXT("name"), Request.Name) || Request.Name.TrimStartAndEnd().IsEmpty())
			return ErrorResponse(FString::Printf(TEXT("Field %d requires a nonempty string name."), Index));
		// Unreal appends a uint32 counter and GUID to the compiled member FName.
		if (Request.Name.Len() > NAME_SIZE - 45)
			return ErrorResponse(FString::Printf(TEXT("Field %d name is too long for an Unreal struct member."), Index));
		for (TCHAR Character : Request.Name)
		{
			if (FChar::IsControl(Character))
				return ErrorResponse(FString::Printf(TEXT("Field %d name must not contain control characters."), Index));
		}
		const FString NameKey = Request.Name.ToLower();
		if (Names.Contains(NameKey))
			return ErrorResponse(FString::Printf(TEXT("Duplicate field name '%s'."), *Request.Name));
		Names.Add(NameKey);
		if (!ResolveRequestedStructFieldType(Field, Request.Type, Error))
			return ErrorResponse(FString::Printf(TEXT("Field '%s': %s"), *Request.Name, *Error));
		Request.ReferencedType.Reset(Request.Type.PinSubCategoryObject.Get());
		Requested.Add(MoveTemp(Request));
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateStruct", "AI: Create Struct"));
	TStrongObjectPtr<UUserDefinedStruct> NewStruct(FStructureEditorUtils::CreateUserDefinedStruct(
		GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass()),
		RF_Public | RF_Transactional));
	if (!NewStruct.IsValid())
	{
		Tx.Cancel();
		return ErrorResponse(TEXT("Failed to create struct."));
	}
	bool bPublished = false;
	ON_SCOPE_EXIT
	{
		if (!bPublished)
		{
			// Cancel only discards undo recording. The unpublished asset is explicitly discarded.
			Tx.Cancel();
			NewStruct->ClearFlags(RF_Public | RF_Standalone);
			NewStruct->MarkAsGarbage();
		}
	};

	TArray<FGuid> SeededGuids;
	for (const FStructVariableDescription& Seed : FStructureEditorUtils::GetVarDesc(NewStruct.Get()))
		SeededGuids.Add(Seed.VarGuid);
	TArray<FGuid> RequestedGuids;
	for (const FRequestedField& Field : Requested)
	{
		const int32 Before = FStructureEditorUtils::GetVarDesc(NewStruct.Get()).Num();
		if (!FStructureEditorUtils::AddVariable(NewStruct.Get(), Field.Type) ||
			FStructureEditorUtils::GetVarDesc(NewStruct.Get()).Num() != Before + 1)
			return ErrorResponse(FString::Printf(TEXT("Failed to add field '%s'."), *Field.Name));
		RequestedGuids.Add(FStructureEditorUtils::GetVarDesc(NewStruct.Get()).Last().VarGuid);
	}
	// Unreal requires at least one member, so remove its seeds only after adding the requested fields.
	for (const FGuid& Guid : SeededGuids)
	{
		if (!FStructureEditorUtils::RemoveVariable(NewStruct.Get(), Guid))
			return ErrorResponse(TEXT("Failed to remove an automatically seeded struct member."));
	}
	// Vacate generated friendly names before assigning user names (including MemberVar_0).
	for (int32 Index = 0; Index < Requested.Num(); ++Index)
	{
		FString TemporaryName;
		do { TemporaryName = TEXT("Nwiro_") + FGuid::NewGuid().ToString(EGuidFormats::Digits); }
		while (Names.Contains(TemporaryName.ToLower()) ||
			!FStructureEditorUtils::IsUniqueVariableFriendlyName(NewStruct.Get(), TemporaryName));
		if (!FStructureEditorUtils::RenameVariable(NewStruct.Get(), RequestedGuids[Index], TemporaryName))
			return ErrorResponse(TEXT("Failed to prepare struct member names."));
	}
	for (int32 Index = 0; Index < Requested.Num(); ++Index)
	{
		if (!FStructureEditorUtils::RenameVariable(NewStruct.Get(), RequestedGuids[Index], Requested[Index].Name))
			return ErrorResponse(FString::Printf(TEXT("Failed to name field '%s'."), *Requested[Index].Name));
	}
	const TArray<FStructVariableDescription>& Members = FStructureEditorUtils::GetVarDesc(NewStruct.Get());
	if (Members.Num() != Requested.Num() || NewStruct->Status != UDSS_UpToDate)
		return ErrorResponse(TEXT("Created struct did not compile with the requested member count."));
	for (int32 Index = 0; Index < Requested.Num(); ++Index)
	{
		if (Members[Index].FriendlyName != Requested[Index].Name || Members[Index].ToPinType() != Requested[Index].Type)
			return ErrorResponse(TEXT("Created struct does not match the requested names and types."));
	}
	// Keep compilation-time struct/editor-data snapshots out of the publication undo.
	// Struct deserialization preloads EditorData, which cannot be restored mid-flip.
	Tx.Cancel();
	FNwiroTransactionHelper PublishTx(NSLOCTEXT("Nwiro", "CreateStruct", "AI: Create Struct"));
	if (!PublishAsset(NewStruct.Get(), Name, PackagePath, Error))
	{
		PublishTx.Cancel();
		return ErrorResponse(Error);
	}
	// The struct's default instance caches its package and must follow the move out of transient storage.
	CastChecked<UUserDefinedStructEditorData>(NewStruct->EditorData)->ReinitializeDefaultInstance();
	NewStruct->SetFlags(RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewStruct.Get());
	NewStruct->MarkPackageDirty();
	bPublished = true;
	return SuccessResponse(NewStruct.Get(), TEXT("fieldCount"), Members.Num());
}

// ============================================================
// CREATE ENUM
// ============================================================

FString FNwiroDataTools::CreateEnum(const FString& JsonCommand)
{
	using namespace NwiroDataCreation;
	TSharedPtr<FJsonObject> Cmd;
	FString Name, PackagePath, Error;
	if (!ReadCreateRequest(JsonCommand, Cmd, Name, PackagePath, Error)) return ErrorResponse(Error);

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Cmd->TryGetArrayField(TEXT("values"), Values) || !Values || Values->IsEmpty())
		return ErrorResponse(TEXT("'values' must be a nonempty array of nonempty strings."));
	TArray<FString> Requested;
	TSet<FString> Names;
	for (int32 Index = 0; Index < Values->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
		FString Label;
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Label) || Label.TrimStartAndEnd().IsEmpty())
			return ErrorResponse(FString::Printf(TEXT("Enum value %d must be a nonempty string."), Index));
		const FString NameKey = Label.ToLower();
		if (Names.Contains(NameKey))
			return ErrorResponse(FString::Printf(TEXT("Duplicate enum value '%s'."), *Label));
		Names.Add(NameKey);
		Requested.Add(MoveTemp(Label));
	}

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateEnum", "AI: Create Enum"));
	TStrongObjectPtr<UUserDefinedEnum> NewEnum(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(
		GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UUserDefinedEnum::StaticClass()),
		RF_Public | RF_Transactional)));
	if (!NewEnum.IsValid())
	{
		Tx.Cancel();
		return ErrorResponse(TEXT("Failed to create enum."));
	}
	bool bPublished = false;
	ON_SCOPE_EXIT
	{
		if (!bPublished)
		{
			Tx.Cancel();
			NewEnum->ClearFlags(RF_Public | RF_Standalone);
			NewEnum->MarkAsGarbage();
		}
	};
	for (int32 Index = 0; Index < Requested.Num(); ++Index)
	{
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(NewEnum.Get());
		// The last entry is Unreal's MAX sentinel. The newly added user entry is at Index.
		if (NewEnum->NumEnums() != Index + 2 || NewEnum->GetValueByIndex(Index) != Index)
			return ErrorResponse(TEXT("Failed to append an enum value."));
		FString TemporaryName;
		do { TemporaryName = TEXT("Nwiro_") + FGuid::NewGuid().ToString(EGuidFormats::Digits); }
		while (Names.Contains(TemporaryName.ToLower()) ||
			!FEnumEditorUtils::IsEnumeratorDisplayNameValid(NewEnum.Get(), Index, FText::FromString(TemporaryName)));
		if (!FEnumEditorUtils::SetEnumeratorDisplayName(NewEnum.Get(), Index, FText::FromString(TemporaryName)))
			return ErrorResponse(TEXT("Failed to prepare enum display names."));
	}
	for (int32 Index = 0; Index < Requested.Num(); ++Index)
	{
		if (!FEnumEditorUtils::SetEnumeratorDisplayName(NewEnum.Get(), Index, FText::FromString(Requested[Index])))
			return ErrorResponse(FString::Printf(TEXT("Invalid enum display name '%s'."), *Requested[Index]));
		if (NewEnum->GetDisplayNameTextByIndex(Index).ToString() != Requested[Index])
			return ErrorResponse(TEXT("Created enum does not match the requested display names."));
	}
	// Undo records only publication of the completed asset, not its staging edits.
	Tx.Cancel();
	FNwiroTransactionHelper PublishTx(NSLOCTEXT("Nwiro", "CreateEnum", "AI: Create Enum"));
	if (!PublishAsset(NewEnum.Get(), Name, PackagePath, Error))
	{
		PublishTx.Cancel();
		return ErrorResponse(Error);
	}
	NewEnum->SetFlags(RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewEnum.Get());
	NewEnum->MarkPackageDirty();
	bPublished = true;
	return SuccessResponse(NewEnum.Get(), TEXT("valueCount"), NewEnum->NumEnums() - 1);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroDataToolsDefectSpec.inl"
#include "Tests/NwiroDataToolsReferenceSpec.inl"
#include "Tests/NwiroDataTableImportSpec.inl"
#endif
