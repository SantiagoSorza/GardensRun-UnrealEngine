// Copyright 2026 Nwiro. All Rights Reserved.

// Included by the owning DataTools.cpp. These tests call public handlers and inspect
// the resulting assets; they do not substitute resolver or engine-only probes.
#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace NwiroDataToolsAcceptance
{
struct FFixture
{
	FAutomationTestBase& Test;
	const FString Root = TEXT("/Game/NwiroDataToolsAcceptance_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<FString> Packages;
	int32 Sequence = 0;

	explicit FFixture(FAutomationTestBase& InTest) : Test(InTest) {}

	~FFixture()
	{
		// Only this fixture's unique, unsaved packages are eligible for cleanup.
		for (const FString& PackageName : Packages)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (!Package) continue;
			TArray<UObject*> Objects;
			// Both the UE5.6 boolean and UE5.8 flags overload default to nested objects.
			GetObjectsWithOuter(Package, Objects);
			for (UObject* Object : Objects)
			{
				if (Object->IsAsset()) FAssetRegistryModule::AssetDeleted(Object);
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}
			Package->SetDirtyFlag(false);
			Package->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
	}

	FString NewName(const TCHAR* Prefix)
	{
		const FString Name = FString::Printf(TEXT("%s_%d"), Prefix, ++Sequence);
		Packages.Add(Root / Name);
		return Name;
	}

	FString ObjectPath(const FString& Name) const { return (Root / Name) + TEXT(".") + Name; }

	UObject* Find(const FString& Name) const
	{
		return StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath(Name));
	}

	bool Call(bool bStruct, const FString& Name, const FString& Items, bool bExpectedSuccess)
	{
		const FString List = Items.IsEmpty() ? FString() :
			FString::Printf(TEXT(",\"%s\":%s"), bStruct ? TEXT("fields") : TEXT("values"), *Items);
		const FString Command = FString::Printf(TEXT("{\"name\":\"%s\",\"path\":\"%s\"%s}"),
			*Name, *Root, *List);
		return CallCommand(bStruct, Name, Command, bExpectedSuccess);
	}

	bool CallCommand(bool bStruct, const FString& Name, const FString& Command, bool bExpectedSuccess)
	{
		const FString Response = bStruct ? FNwiroDataTools::CreateStruct(Command) :
			FNwiroDataTools::CreateEnum(Command);
		TSharedPtr<FJsonObject> Json;
		const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Json) && Json.IsValid();
		if (!Test.TestTrue(*FString::Printf(TEXT("%s response is valid JSON"), *Name), bParsed)) return false;
		bool bSuccess = false;
		if (!Test.TestTrue(TEXT("response has boolean success"), Json->TryGetBoolField(TEXT("success"), bSuccess))) return false;
		Test.TestEqual(*FString::Printf(TEXT("%s success"), *Name), bSuccess, bExpectedSuccess);
		if (bSuccess != bExpectedSuccess) Test.AddError(TEXT("Public handler response: ") + Response);
		if (bExpectedSuccess && bSuccess)
		{
			TSharedPtr<FJsonObject> Request;
			const TArray<TSharedPtr<FJsonValue>>* RequestedItems = nullptr;
			const bool bRequestParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Command), Request) &&
				Request.IsValid() && Request->TryGetArrayField(bStruct ? TEXT("fields") : TEXT("values"), RequestedItems);
			if (Test.TestTrue(TEXT("successful request has an item array"), bRequestParsed))
			{
				double ReportedCount = -1;
				Test.TestTrue(TEXT("success reports a numeric item count"),
					Json->TryGetNumberField(bStruct ? TEXT("fieldCount") : TEXT("valueCount"), ReportedCount));
				Test.TestEqual(TEXT("reported count equals the requested population"), ReportedCount, static_cast<double>(RequestedItems->Num()));
			}
			UPackage* Package = FindPackage(nullptr, *(Root / Name));
			if (Test.TestNotNull(TEXT("successful creation has a package"), Package))
			{
				Test.TestTrue(TEXT("successful creation marks its package dirty"), Package->IsDirty());
			}
		}
		if (!bSuccess)
		{
			FString Error;
			Test.TestTrue(TEXT("failure explains itself"),
				Json->TryGetStringField(TEXT("error"), Error) && !Error.TrimStartAndEnd().IsEmpty());
		}
		return bSuccess;
	}

	void AssertAbsent(const FString& Name)
	{
		AssertAbsentAt(Root / Name, Name);
	}

	void AssertAbsentAt(const FString& PackageName, const FString& Name)
	{
		const FString FullObjectPath = PackageName + TEXT(".") + Name;
		Test.TestNull(TEXT("failed request leaves no object"),
			StaticFindObject(UObject::StaticClass(), nullptr, *FullObjectPath));
		Test.TestNull(TEXT("failed request leaves no package or dirty state"), FindPackage(nullptr, *PackageName));
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Test.TestFalse(TEXT("failed request leaves no registry entry"),
			Registry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath)).IsValid());
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		Test.TestFalse(TEXT("failed request leaves no saved asset"), IFileManager::Get().FileExists(*Filename));
	}

	void AssertNotSaved(const FString& Name)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Root / Name, FPackageName::GetAssetPackageExtension());
		Test.TestFalse(TEXT("handler does not save an asset"), IFileManager::Get().FileExists(*Filename));
	}

	void Reject(bool bStruct, const TCHAR* Items)
	{
		const FString Name = NewName(bStruct ? TEXT("BadStruct") : TEXT("BadEnum"));
		Call(bStruct, Name, Items, false);
		AssertAbsent(Name);
	}
};

struct FExpectedField
{
	const TCHAR* Name;
	const TCHAR* Type;
	FName Category;
	FName SubCategory;
	UScriptStruct* Struct;
	FFieldClass* PropertyClass;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroDataToolsDefectSpec,
	"Nwiro.Pro.DataTools.StructAndEnumDefects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNwiroDataToolsDefectSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroDataToolsAcceptance;
	FFixture Fixture(*this);

	// All supported aliases pass through the real handler. The last two names
	// exercise display names with spaces and collision with Unreal's seed name.
	const FExpectedField Expected[] = {
		{ TEXT("Bool"), TEXT("bool"), UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, FBoolProperty::StaticClass() },
		{ TEXT("Boolean"), TEXT("boolean"), UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, FBoolProperty::StaticClass() },
		{ TEXT("Int"), TEXT("int"), UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, FIntProperty::StaticClass() },
		{ TEXT("Integer"), TEXT("integer"), UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, FIntProperty::StaticClass() },
		{ TEXT("Float"), TEXT("float"), UEdGraphSchema_K2::PC_Real, TEXT("float"), nullptr, FFloatProperty::StaticClass() },
		{ TEXT("Double"), TEXT("double"), UEdGraphSchema_K2::PC_Real, TEXT("double"), nullptr, FDoubleProperty::StaticClass() },
		{ TEXT("Real"), TEXT("real"), UEdGraphSchema_K2::PC_Real, TEXT("double"), nullptr, FDoubleProperty::StaticClass() },
		{ TEXT("String"), TEXT("string"), UEdGraphSchema_K2::PC_String, NAME_None, nullptr, FStrProperty::StaticClass() },
		{ TEXT("Name"), TEXT("name"), UEdGraphSchema_K2::PC_Name, NAME_None, nullptr, FNameProperty::StaticClass() },
		{ TEXT("Text"), TEXT("text"), UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, FTextProperty::StaticClass() },
		{ TEXT("Vector"), TEXT("Vector"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get(), FStructProperty::StaticClass() },
		{ TEXT("Rotator"), TEXT("rotator"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FRotator>::Get(), FStructProperty::StaticClass() },
		{ TEXT("Transform"), TEXT("transform"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FTransform>::Get(), FStructProperty::StaticClass() },
		{ TEXT("Color"), TEXT("color"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get(), FStructProperty::StaticClass() },
		{ TEXT("LinearColor"), TEXT("linearcolor"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get(), FStructProperty::StaticClass() },
		{ TEXT("Camera Z Goal"), TEXT("float"), UEdGraphSchema_K2::PC_Real, TEXT("float"), nullptr, FFloatProperty::StaticClass() },
		{ TEXT("MemberVar_0"), TEXT("int"), UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, FIntProperty::StaticClass() },
	};
	FString Fields = TEXT("[");
	for (const FExpectedField& Field : Expected)
	{
		if (Fields.Len() > 1) Fields += TEXT(",");
		Fields += FString::Printf(TEXT("{\"name\":\"%s\",\"type\":\"%s\"}"), Field.Name, Field.Type);
	}
	Fields += TEXT("]");
	const FString StructName = Fixture.NewName(TEXT("ExactStruct"));
	if (Fixture.Call(true, StructName, Fields, true))
	{
		UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Fixture.Find(StructName));
		if (TestNotNull(TEXT("created struct exists"), Struct))
		{
			const TArray<FStructVariableDescription>& Members = FStructureEditorUtils::GetVarDesc(Struct);
			TestEqual(TEXT("FIX-003 exact requested member count"), Members.Num(), static_cast<int32>(UE_ARRAY_COUNT(Expected)));
			for (int32 Index = 0; Index < FMath::Min(Members.Num(), static_cast<int32>(UE_ARRAY_COUNT(Expected))); ++Index)
			{
				const FExpectedField& Field = Expected[Index];
				TestEqual(TEXT("member names and order are exact"), Members[Index].FriendlyName, FString(Field.Name));
				const FEdGraphPinType Pin = Members[Index].ToPinType();
				TestEqual(TEXT("member pin category"), Pin.PinCategory, Field.Category);
				TestEqual(TEXT("member pin subcategory"), Pin.PinSubCategory, Field.SubCategory);
				TestTrue(TEXT("member pin subtype identity"), Pin.PinSubCategoryObject.Get() == Field.Struct);
				TestTrue(TEXT("member remains scalar"), Pin.ContainerType == EPinContainerType::None);
				FProperty* Property = FindFProperty<FProperty>(Struct, Members[Index].VarName);
				if (TestNotNull(TEXT("member has a compiled property"), Property))
				{
					TestTrue(TEXT("compiled property class"), Property->GetClass() == Field.PropertyClass);
					if (Field.Struct)
					{
						const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
						TestTrue(TEXT("compiled struct identity"), StructProperty && StructProperty->Struct == Field.Struct);
					}
				}
			}
			Fixture.AssertNotSaved(StructName);
			const TArray<FStructVariableDescription> BeforeMembers = Members;
			for (bool bWasDirty : { false, true })
			{
				Struct->GetOutermost()->SetDirtyFlag(bWasDirty);
				Fixture.Call(true, StructName, TEXT("[{\"name\":\"Replacement\",\"type\":\"bool\"}]"), false);
				TestTrue(TEXT("existing struct object unchanged"), Fixture.Find(StructName) == Struct);
				const auto& CurrentMembers = FStructureEditorUtils::GetVarDesc(Struct);
				TestEqual(TEXT("existing struct member count unchanged"), CurrentMembers.Num(), BeforeMembers.Num());
				for (int32 Index = 0; Index < FMath::Min(CurrentMembers.Num(), BeforeMembers.Num()); ++Index)
				{
					TestEqual(TEXT("existing member name preserved"), CurrentMembers[Index].FriendlyName, BeforeMembers[Index].FriendlyName);
					TestTrue(TEXT("existing member type preserved"), CurrentMembers[Index].ToPinType() == BeforeMembers[Index].ToPinType());
				}
				TestEqual(TEXT("existing struct dirty state preserved"), Struct->GetOutermost()->IsDirty(), bWasDirty);
			}
		}
	}

	// Requested names can collide with later engine-generated friendly names.
	const FString GeneratedNames = Fixture.NewName(TEXT("GeneratedStructNames"));
	if (Fixture.Call(true, GeneratedNames,
		TEXT("[{\"name\":\"MemberVar_2\",\"type\":\"int\"},{\"name\":\"MemberVar_1\",\"type\":\"float\"}]"), true))
	{
		UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Fixture.Find(GeneratedNames));
		if (TestNotNull(TEXT("generated-name collision struct exists"), Struct))
		{
			const auto& Members = FStructureEditorUtils::GetVarDesc(Struct);
			TestEqual(TEXT("generated-name collision preserves count"), Members.Num(), 2);
			if (Members.Num() == 2)
			{
				TestEqual(TEXT("first generated-name collision is preserved"), Members[0].FriendlyName, FString(TEXT("MemberVar_2")));
				TestEqual(TEXT("second generated-name collision is preserved"), Members[1].FriendlyName, FString(TEXT("MemberVar_1")));
				TestEqual(TEXT("first collision retains int"), Members[0].ToPinType().PinCategory, UEdGraphSchema_K2::PC_Int);
				TestEqual(TEXT("second collision retains float"), Members[1].ToPinType().PinSubCategory, FName(TEXT("float")));
			}
		}
	}

	// Validation rejects the whole batch before any package allocation, including
	// a bad late field after a valid field and malformed/non-object elements.
	const TCHAR* BadFields[] = {
		TEXT(""), TEXT("null"), TEXT("7"), TEXT("[]"),
		TEXT("[null]"), TEXT("[7]"), TEXT("[{}]"),
		TEXT("[{\"name\":\"A\"}]"), TEXT("[{\"type\":\"int\"}]"),
		TEXT("[{\"name\":\"   \",\"type\":\"int\"}]"),
		TEXT("[{\"name\":\"A\",\"type\":7}]"),
		TEXT("[{\"name\":7,\"type\":\"int\"}]"),
		TEXT("[{\"name\":\"\",\"type\":\"int\"}]"),
		TEXT("[{\"name\":\"A\",\"type\":\"   \"}]"),
		TEXT("[{\"name\":\"A\",\"type\":\"int\"},null]"),
		TEXT("[{\"name\":\"A\",\"type\":\"int\"},{\"name\":\"a\",\"type\":\"float\"}]"),
		TEXT("[{\"name\":\"A\",\"type\":\"int\"},{\"name\":\"A\",\"type\":\"float\"}]"),
		TEXT("[{\"name\":\"A\",\"type\":\"int\"},{\"name\":\"Bad\\\"\\\\Name\",\"type\":\"bad\\\"\\\\type\"}]"),
	};
	for (const TCHAR* Bad : BadFields) Fixture.Reject(true, Bad);
	const TCHAR* Unsupported[] = { TEXT("class"), TEXT("object"), TEXT("byte"), TEXT("int64"), TEXT("int32"), TEXT("EMyEnum"), TEXT("FMyStruct"), TEXT("array<int>"), TEXT("wildcard"), TEXT("") };
	for (const TCHAR* Type : Unsupported)
	{
		Fixture.Reject(true, *FString::Printf(TEXT("[{\"name\":\"Good\",\"type\":\"int\"},{\"name\":\"Bad\",\"type\":\"%s\"}]"), Type));
	}
	const FString RetryStruct = Fixture.NewName(TEXT("RetryStruct"));
	Fixture.Call(true, RetryStruct, TEXT("[{\"name\":\"Good\",\"type\":\"int\"},{\"name\":\"Bad\",\"type\":\"object\"}]"), false);
	Fixture.AssertAbsent(RetryStruct);
	if (Fixture.Call(true, RetryStruct, TEXT("[{\"name\":\"MemberVar_0\",\"type\":\"int\"}]"), true))
	{
		UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Fixture.Find(RetryStruct));
		if (TestNotNull(TEXT("same-path retry creates struct"), Struct))
		{
			const auto& Members = FStructureEditorUtils::GetVarDesc(Struct);
			TestEqual(TEXT("one requested field yields exactly one member"), Members.Num(), 1);
			if (Members.Num()) TestEqual(TEXT("seed-name collision preserves requested name"), Members[0].FriendlyName, FString(TEXT("MemberVar_0")));
		}
	}

	// Labels include collisions with Unreal's generated display names and JSON escapes.
	const TArray<TArray<FString>> EnumCases = {
		{ TEXT("Only") },
		{ TEXT("First"), TEXT("Middle"), TEXT("Last") },
		{ TEXT("NewEnumerator1"), TEXT("NewEnumerator0") },
		{ TEXT("Camera Z Goal"), TEXT("Quoted \"label\""), TEXT("Back\\slash") },
	};
	for (const TArray<FString>& Labels : EnumCases)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Label : Labels) JsonValues.Add(MakeShared<FJsonValueString>(Label));
		FString Values;
		FJsonSerializer::Serialize(JsonValues, TJsonWriterFactory<>::Create(&Values));
		const FString Name = Fixture.NewName(TEXT("ExactEnum"));
		if (!Fixture.Call(false, Name, Values, true)) continue;
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Fixture.Find(Name));
		if (!TestNotNull(TEXT("created enum exists"), Enum)) continue;
		TestEqual(TEXT("FIX-001 requested values plus sentinel"), Enum->NumEnums(), Labels.Num() + 1);
		for (int32 Index = 0; Index < Labels.Num() && Index < Enum->NumEnums(); ++Index)
		{
			TestEqual(TEXT("FIX-001 exact enum label and order"), Enum->GetDisplayNameTextByIndex(Index).ToString(), Labels[Index]);
			TestEqual(TEXT("enum values remain contiguous"), Enum->GetValueByIndex(Index), static_cast<int64>(Index));
		}
		Fixture.AssertNotSaved(Name);
		for (bool bWasDirty : { false, true })
		{
			Enum->GetOutermost()->SetDirtyFlag(bWasDirty);
			Fixture.Call(false, Name, TEXT("[\"Replacement\"]"), false);
			TestTrue(TEXT("existing enum object unchanged"), Fixture.Find(Name) == Enum);
			TestEqual(TEXT("existing enum values unchanged"), Enum->NumEnums(), Labels.Num() + 1);
			for (int32 Index = 0; Index < Labels.Num() && Index < Enum->NumEnums(); ++Index)
			{
				TestEqual(TEXT("existing enum label preserved"), Enum->GetDisplayNameTextByIndex(Index).ToString(), Labels[Index]);
			}
			TestEqual(TEXT("existing enum dirty state preserved"), Enum->GetOutermost()->IsDirty(), bWasDirty);
		}
	}
	const TCHAR* BadValues[] = {
		TEXT(""), TEXT("null"), TEXT("7"), TEXT("[]"), TEXT("[null]"), TEXT("[7]"),
		TEXT("[\"First\",null]"), TEXT("[\"First\",7]"), TEXT("[\"\"]"), TEXT("[\"   \"]"),
		TEXT("[\"First\",\"First\"]"), TEXT("[\"First\",\"first\"]"),
	};
	for (const TCHAR* Bad : BadValues) Fixture.Reject(false, Bad);
	const FString RetryEnum = Fixture.NewName(TEXT("RetryEnum"));
	Fixture.Call(false, RetryEnum, TEXT("[\"First\",null]"), false);
	Fixture.AssertAbsent(RetryEnum);
	if (Fixture.Call(false, RetryEnum, TEXT("[\"Recovered\"]"), true))
	{
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Fixture.Find(RetryEnum));
		if (TestNotNull(TEXT("same-path retry creates enum"), Enum))
		{
			TestEqual(TEXT("retry enum has one value plus sentinel"), Enum->NumEnums(), 2);
			TestEqual(TEXT("retry enum preserves label"), Enum->GetDisplayNameTextByIndex(0).ToString(), FString(TEXT("Recovered")));
		}
	}
	// This reserved display name passes basic string validation but Unreal rejects it.
	// Failure must discard staging and leave the final path immediately reusable.
	const FString StageRetry = Fixture.NewName(TEXT("StageFailureRetry"));
	Fixture.Call(false, StageRetry, TEXT("[\"First\",\"(INVALID)\"]"), false);
	Fixture.AssertAbsent(StageRetry);
	if (Fixture.Call(false, StageRetry, TEXT("[\"RecoveredAfterStageFailure\"]"), true))
	{
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Fixture.Find(StageRetry));
		if (TestNotNull(TEXT("staging failure path is reusable"), Enum))
		{
			TestEqual(TEXT("stage retry has exactly one value"), Enum->NumEnums(), 2);
			TestEqual(TEXT("stage retry label"), Enum->GetDisplayNameTextByIndex(0).ToString(),
				FString(TEXT("RecoveredAfterStageFailure")));
		}
	}

	// Both handlers reject malformed roots and non-string required fields without mutation.
	for (bool bStruct : { true, false })
	{
		for (const TCHAR* Malformed : { TEXT("{"), TEXT("null"), TEXT("[]") })
		{
			const FString Name = Fixture.NewName(TEXT("MalformedRoot"));
			Fixture.CallCommand(bStruct, Name, Malformed, false);
			Fixture.AssertAbsent(Name);
		}
		const FString BadName = Fixture.NewName(TEXT("NonStringName"));
		const FString ItemKey = bStruct ? TEXT("fields") : TEXT("values");
		const FString Items = bStruct ? TEXT("[{\"name\":\"A\",\"type\":\"int\"}]") : TEXT("[\"A\"]");
		const FString BadNameCommand = FString::Printf(TEXT("{\"name\":7,\"path\":\"%s\",\"%s\":%s}"),
			*Fixture.Root, *ItemKey, *Items);
		Fixture.Packages.AddUnique(Fixture.Root / TEXT("7"));
		Fixture.CallCommand(bStruct, BadName, BadNameCommand, false);
		Fixture.AssertAbsent(BadName);
		Fixture.AssertAbsent(TEXT("7"));
		// A broken handler might substitute its default path. Use a globally unique
		// name and track that fallback too, so a RED run cannot leave user-state debris.
		const FString BadPath = Fixture.NewName(*(TEXT("NonStringPath_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		const FString FallbackPackage = FString(TEXT("/Game/Data")) / BadPath;
		Fixture.Packages.Add(FallbackPackage);
		const FString BadPathCommand = FString::Printf(TEXT("{\"name\":\"%s\",\"path\":7,\"%s\":%s}"),
			*BadPath, *ItemKey, *Items);
		Fixture.CallCommand(bStruct, BadPath, BadPathCommand, false);
		Fixture.AssertAbsent(BadPath);
		Fixture.AssertAbsentAt(FallbackPackage, BadPath);
	}

	// Overlong mounted paths must be rejected before FSoftObjectPath interns a FName.
	// Do not pass the invalid long path to the assertion helpers themselves.
	for (bool bStruct : { true, false })
	{
		const FString Name = Fixture.NewName(TEXT("OversizedPath"));
		const FString LongPath = Fixture.Root + TEXT("/") + FString::ChrN(NAME_SIZE, TEXT('A'));
		const FString Command = FString::Printf(TEXT("{\"name\":\"%s\",\"path\":\"%s\",\"%s\":%s}"),
			*Name, *LongPath, bStruct ? TEXT("fields") : TEXT("values"),
			bStruct ? TEXT("[{\"name\":\"A\",\"type\":\"int\"}]") : TEXT("[\"A\"]"));
		Fixture.CallCommand(bStruct, Name, Command, false);
		Fixture.AssertAbsent(Name);
	}

	// The handler must not absorb or cancel a caller-owned transaction.
	if (GEditor)
	{
		FScopedTransaction Outer(NSLOCTEXT("NwiroTests", "DataToolsOuter", "Nwiro data tools caller transaction"));
		for (bool bStruct : { true, false })
		{
			const FString Name = Fixture.NewName(TEXT("NestedCreation"));
			Fixture.Call(bStruct, Name, bStruct ? TEXT("[{\"name\":\"A\",\"type\":\"int\"}]") : TEXT("[\"A\"]"), false);
			TestTrue(TEXT("rejected creation preserves the caller's active transaction"), GEditor->IsTransactionActive());
			Fixture.AssertAbsent(Name);
		}
	}

	// Only undo the transaction just created by this fixture. Never reset history.
	if (TestNotNull(TEXT("editor available for transaction acceptance"), GEditor) &&
		TestNotNull(TEXT("transaction buffer available"), GEditor->Trans.Get()))
	{
		for (bool bStruct : { true, false })
		{
			const FString Name = Fixture.NewName(bStruct ? TEXT("UndoStruct") : TEXT("UndoEnum"));
			const FGuid PriorTransaction = GEditor->Trans->GetUndoContext().TransactionId;
			const FString Items = bStruct ?
				TEXT("[{\"name\":\"Count\",\"type\":\"int\"},{\"name\":\"Location\",\"type\":\"vector\"}]") :
				TEXT("[\"First\",\"Second\"]");
			if (!Fixture.Call(bStruct, Name, Items, true)) continue;
			const FTransactionContext Created = GEditor->Trans->GetUndoContext();
			const FText ExpectedTitle = bStruct ?
				NSLOCTEXT("Nwiro", "CreateStruct", "AI: Create Struct") :
				NSLOCTEXT("Nwiro", "CreateEnum", "AI: Create Enum");
			const bool bOwnTransaction = Created.TransactionId.IsValid() &&
				Created.TransactionId != PriorTransaction && Created.Title.EqualTo(ExpectedTitle);
			if (!TestTrue(TEXT("creation produced its own undo transaction"), bOwnTransaction)) continue;
			TStrongObjectPtr<UObject> CreatedAsset(Fixture.Find(Name));
			if (!TestNotNull(TEXT("created asset exists before undo"), CreatedAsset.Get())) continue;
			UUserDefinedStruct* CreatedStruct = Cast<UUserDefinedStruct>(CreatedAsset.Get());
			const int32 OriginalSize = CreatedStruct ? CreatedStruct->GetStructureSize() : 0;
			TArray<FName> OriginalNames;
			TArray<int32> OriginalOffsets;
			TArray<FName> OriginalTypes;
			if (CreatedStruct)
			{
				for (TFieldIterator<FProperty> It(CreatedStruct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					OriginalNames.Add(It->GetFName());
					OriginalOffsets.Add(It->GetOffset_ForInternal());
					OriginalTypes.Add(It->GetClass()->GetFName());
				}
				TestEqual(TEXT("creation has exactly two reflected properties"), OriginalNames.Num(), 2);
			}
			auto AssertRestoredLayout = [&]()
			{
				if (!CreatedStruct) return true;
				TArray<FName> Names;
				TArray<int32> Offsets;
				TArray<FName> Types;
				for (TFieldIterator<FProperty> It(CreatedStruct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					Names.Add(It->GetFName());
					Offsets.Add(It->GetOffset_ForInternal());
					Types.Add(It->GetClass()->GetFName());
				}
				TestEqual(TEXT("restoration preserves exact reflected count"), Names.Num(), 2);
				TestTrue(TEXT("restoration preserves reflected names and order"), Names == OriginalNames);
				TestTrue(TEXT("restoration preserves property offsets"), Offsets == OriginalOffsets);
				TestTrue(TEXT("restoration preserves reflected property types"), Types == OriginalTypes);
				TestEqual(TEXT("restoration preserves compiled struct size"), CreatedStruct->GetStructureSize(), OriginalSize);
				// Do not read defaults or redo after detecting a mismatched memory layout.
				return Names.Num() == 2 && Names == OriginalNames && Offsets == OriginalOffsets &&
					Types == OriginalTypes && CreatedStruct->GetStructureSize() == OriginalSize;
			};
			if (!TestTrue(TEXT("own creation can undo"), GEditor->UndoTransaction())) continue;
			if (!AssertRestoredLayout()) continue;
			TestFalse(TEXT("undo removes standalone publication flag"), CreatedAsset->HasAnyFlags(RF_Standalone));
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TestFalse(TEXT("undo removes asset registry publication"),
				Registry.GetAssetByObjectPath(FSoftObjectPath(Fixture.ObjectPath(Name))).IsValid());
			TestNull(TEXT("undo removes the created object's final path"), Fixture.Find(Name));
			const FTransactionContext Redo = GEditor->Trans->GetRedoContext();
			if (!TestTrue(TEXT("redo still targets our creation"), Redo.TransactionId == Created.TransactionId)) continue;
			if (!TestTrue(TEXT("own creation can redo"), GEditor->RedoTransaction())) continue;
			if (!AssertRestoredLayout()) continue;
			TestTrue(TEXT("redo restores the same asset"), Fixture.Find(Name) == CreatedAsset.Get());
			TestTrue(TEXT("redo restores standalone publication flag"), CreatedAsset->HasAnyFlags(RF_Standalone));
			TestTrue(TEXT("redo restores asset registry publication"),
				Registry.GetAssetByObjectPath(FSoftObjectPath(Fixture.ObjectPath(Name))).IsValid());
			if (bStruct)
			{
				UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Fixture.Find(Name));
				if (!TestNotNull(TEXT("redo restores struct at final path"), Struct)) continue;
				const auto& Members = FStructureEditorUtils::GetVarDesc(Struct);
				TestEqual(TEXT("redo restores exact struct count"), Members.Num(), 2);
				TestTrue(TEXT("redo struct compiles"), Struct->Status == UDSS_UpToDate);
				const uint8* DefaultData = Struct->GetDefaultInstance();
				TestNotNull(TEXT("redo default instance is available through the public API"), DefaultData);
				if (Members.Num() == 2)
				{
					TestEqual(TEXT("redo first field name"), Members[0].FriendlyName, FString(TEXT("Count")));
					TestEqual(TEXT("redo second field name"), Members[1].FriendlyName, FString(TEXT("Location")));
					TestEqual(TEXT("redo first pin type"), Members[0].ToPinType().PinCategory, UEdGraphSchema_K2::PC_Int);
					TestTrue(TEXT("redo vector subtype"), Members[1].ToPinType().PinSubCategoryObject.Get() == TBaseStructure<FVector>::Get());
					FIntProperty* Count = FindFProperty<FIntProperty>(Struct, Members[0].VarName);
					FStructProperty* Location = FindFProperty<FStructProperty>(Struct, Members[1].VarName);
					TestNotNull(TEXT("redo has compiled int property"), Count);
					TestTrue(TEXT("redo has compiled vector property"), Location && Location->Struct == TBaseStructure<FVector>::Get());
					if (Count && DefaultData)
						TestEqual(TEXT("redo int default is readable"), Count->GetPropertyValue_InContainer(DefaultData), 0);
					if (Location && Location->Struct == TBaseStructure<FVector>::Get() && DefaultData)
						TestTrue(TEXT("redo vector default is readable"),
							Location->ContainerPtrToValuePtr<FVector>(DefaultData)->Equals(FVector::ZeroVector));
				}
			}
			else
			{
				UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Fixture.Find(Name));
				if (!TestNotNull(TEXT("redo restores enum at final path"), Enum)) continue;
				TestEqual(TEXT("redo enum count includes sentinel"), Enum->NumEnums(), 3);
				if (Enum->NumEnums() >= 2)
				{
					TestEqual(TEXT("redo first enum label"), Enum->GetDisplayNameTextByIndex(0).ToString(), FString(TEXT("First")));
					TestEqual(TEXT("redo second enum label"), Enum->GetDisplayNameTextByIndex(1).ToString(), FString(TEXT("Second")));
					TestEqual(TEXT("redo first enum value"), Enum->GetValueByIndex(0), static_cast<int64>(0));
					TestEqual(TEXT("redo second enum value"), Enum->GetValueByIndex(1), static_cast<int64>(1));
				}
			}
			Fixture.AssertNotSaved(Name);
		}
	}

	return true;
}
