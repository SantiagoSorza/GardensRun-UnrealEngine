// Copyright 2026 Nwiro. All Rights Reserved.

// Public-handler acceptance for exact reference identity and complete preflight.
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroStructReferenceTypes,
	"Nwiro.Pro.DataTools.StructReferenceTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroStructReferenceTypes::RunTest(const FString& Parameters)
{
	using namespace NwiroDataToolsAcceptance;
	FFixture Fixture(*this);
	const FString EnumName = Fixture.NewName(TEXT("ReferenceEnum"));
	const FString NestedName = Fixture.NewName(TEXT("NestedStruct"));
	if (!Fixture.Call(false, EnumName, TEXT("[\"Idle\",\"Active\"]"), true) ||
		!Fixture.Call(true, NestedName, TEXT("[{\"name\":\"Count\",\"type\":\"int\"}]"), true)) return false;
	UEnum* Enum = Cast<UEnum>(Fixture.Find(EnumName));
	UScriptStruct* Nested = Cast<UScriptStruct>(Fixture.Find(NestedName));
	if (!TestNotNull(TEXT("nested enum fixture"), Enum) || !TestNotNull(TEXT("nested struct fixture"), Nested)) return false;

	const bool bEnumDirtyBefore = Enum->GetOutermost()->IsDirty();
	const bool bNestedDirtyBefore = Nested->GetOutermost()->IsDirty();
	const int32 NestedSizeBefore = Nested->GetStructureSize();
	const FString BlueprintName = Fixture.NewName(TEXT("ReferenceActor"));
	UPackage* BlueprintPackage = CreatePackage(*(Fixture.Root / BlueprintName));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), BlueprintPackage,
		FName(*BlueprintName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("loaded generated-class fixture"), Blueprint) ||
		!TestNotNull(TEXT("generated class fixture"), Blueprint->GeneratedClass.Get())) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);

	struct FExpectedField { FString Name; FString Base; UObject* Subtype; bool bArray; };
	TArray<FExpectedField> Expected;
	TArray<TSharedPtr<FJsonValue>> Fields;
	auto AddField = [&](const TCHAR* Name, const TCHAR* Base, UObject* Subtype, bool bArray, bool bStructured, const FString& Reference)
	{
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("name"), Name);
		TSharedPtr<FJsonObject> Options = bStructured ? MakeShared<FJsonObject>() : Field;
		Options->SetStringField(bStructured ? TEXT("base") : TEXT("type"), Base);
		if (Subtype) Options->SetStringField(TEXT("subtype"), Reference);
		if (bArray) Options->SetStringField(TEXT("container"), TEXT("array"));
		if (bStructured) Field->SetObjectField(TEXT("type"), Options);
		Fields.Add(MakeShared<FJsonValueObject>(Field));
		Expected.Add({ Name, Base, Subtype, bArray });
	};
	AddField(TEXT("ActorClass"), TEXT("class"), AActor::StaticClass(), false, true, TEXT("Actor"));
	AddField(TEXT("Transform"), TEXT("transform"), nullptr, false, false, TEXT(""));
	AddField(TEXT("ActorInstance"), TEXT("object"), AActor::StaticClass(), false, false, TEXT("/Script/Engine.Actor"));
	AddField(TEXT("ActorClasses"), TEXT("class"), AActor::StaticClass(), true, false, TEXT("AActor"));
	AddField(TEXT("ActorInstances"), TEXT("object"), AActor::StaticClass(), true, true, TEXT("Actor"));
	AddField(TEXT("State"), TEXT("enum"), Enum, false, true, EnumName);
	AddField(TEXT("States"), TEXT("enum"), Enum, true, false, Enum->GetPathName());
	AddField(TEXT("Data"), TEXT("struct"), Nested, false, false, NestedName);
	AddField(TEXT("DataArray"), TEXT("struct"), Nested, true, true, Nested->GetPathName());
	AddField(TEXT("SpawnClass"), TEXT("class"), Blueprint->GeneratedClass, false, true, Blueprint->GeneratedClass->GetPathName());
	AddField(TEXT("SpawnObjects"), TEXT("object"), Blueprint->GeneratedClass, true, false, Blueprint->GeneratedClass->GetPathName());
	AddField(TEXT("Counts"), TEXT("int"), nullptr, true, true, TEXT(""));
	FString FieldsJson;
	FJsonSerializer::Serialize(Fields, TJsonWriterFactory<>::Create(&FieldsJson));
	const FString StructName = Fixture.NewName(TEXT("CollectableObjectData"));
	const FGuid PriorTransaction = GEditor->Trans->GetUndoContext().TransactionId;
	if (!Fixture.Call(true, StructName, FieldsJson, true)) return false;
	TStrongObjectPtr<UUserDefinedStruct> Struct(Cast<UUserDefinedStruct>(Fixture.Find(StructName)));
	if (!TestNotNull(TEXT("reference struct created"), Struct.Get())) return false;
	const int32 OriginalSize = Struct->GetStructureSize();
	TArray<FName> OriginalNames;
	TArray<int32> OriginalOffsets;
	for (TFieldIterator<FProperty> It(Struct.Get(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		OriginalNames.Add(It->GetFName());
		OriginalOffsets.Add(It->GetOffset_ForInternal());
	}
	auto AssertExactTypes = [&]()
	{
		const auto& Descriptions = FStructureEditorUtils::GetVarDesc(Struct.Get());
		TestEqual(TEXT("exact editor field count"), Descriptions.Num(), Expected.Num());
		TArray<FName> Names;
		TArray<int32> Offsets;
		for (TFieldIterator<FProperty> It(Struct.Get(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Names.Add(It->GetFName());
			Offsets.Add(It->GetOffset_ForInternal());
		}
		TestEqual(TEXT("exact reflected count"), Names.Num(), Expected.Num());
		TestTrue(TEXT("stable reflected names"), Names == OriginalNames);
		TestTrue(TEXT("stable reflected offsets"), Offsets == OriginalOffsets);
		TestEqual(TEXT("stable compiled size"), Struct->GetStructureSize(), OriginalSize);
		if (Descriptions.Num() != Expected.Num() || Names != OriginalNames || Offsets != OriginalOffsets ||
			Struct->GetStructureSize() != OriginalSize) return false;
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			const FExpectedField& Wanted = Expected[Index];
			TestEqual(TEXT("requested friendly name"), Descriptions[Index].FriendlyName, Wanted.Name);
			FProperty* Property = FindFProperty<FProperty>(Struct.Get(), Descriptions[Index].VarName);
			if (!TestNotNull(TEXT("reflected field"), Property)) continue;
			FArrayProperty* Array = CastField<FArrayProperty>(Property);
			TestEqual(TEXT("exact container"), Array != nullptr, Wanted.bArray);
			if (Wanted.bArray)
			{
				if (!Array) continue;
				Property = Array->Inner;
			}
			if (Wanted.Base == TEXT("class"))
			{
				FClassProperty* Class = CastField<FClassProperty>(Property);
				if (TestNotNull(TEXT("class property kind"), Class))
				{
					TestTrue(TEXT("exact class constraint (not UObject fallback)"), Class->MetaClass == Wanted.Subtype);
					TestTrue(TEXT("class property stores UClass"), Class->PropertyClass == UClass::StaticClass());
				}
			}
			else if (Wanted.Base == TEXT("object"))
			{
				FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property);
				TestNull(TEXT("object is not class property"), CastField<FClassProperty>(Property));
				if (TestNotNull(TEXT("object property kind"), Object)) TestTrue(TEXT("exact object class"), Object->PropertyClass == Wanted.Subtype);
			}
			else if (Wanted.Base == TEXT("enum"))
			{
				FByteProperty* Byte = CastField<FByteProperty>(Property);
				FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
				TestTrue(TEXT("exact enum identity"), (Byte && Byte->Enum == Wanted.Subtype) || (EnumProperty && EnumProperty->GetEnum() == Wanted.Subtype));
			}
			else if (Wanted.Base == TEXT("struct") || Wanted.Base == TEXT("transform"))
			{
				FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (TestNotNull(TEXT("struct property kind"), StructProperty)) TestTrue(TEXT("exact struct identity"),
					StructProperty->Struct == (Wanted.Subtype ? Wanted.Subtype : TBaseStructure<FTransform>::Get()));
			}
			else TestNotNull(TEXT("int array inner"), CastField<FIntProperty>(Property));
		}
		return true;
	};
	AssertExactTypes();
	TestEqual(TEXT("referenced enum dirtiness preserved"), Enum->GetOutermost()->IsDirty(), bEnumDirtyBefore);
	TestEqual(TEXT("referenced struct dirtiness preserved"), Nested->GetOutermost()->IsDirty(), bNestedDirtyBefore);
	TestEqual(TEXT("referenced struct layout preserved"), Nested->GetStructureSize(), NestedSizeBefore);
	TestTrue(TEXT("referenced enum identity preserved"), Fixture.Find(EnumName) == Enum);
	TestTrue(TEXT("referenced struct identity preserved"), Fixture.Find(NestedName) == Nested);
	Fixture.AssertNotSaved(StructName);
	const FTransactionContext Created = GEditor->Trans->GetUndoContext();
	if (TestTrue(TEXT("owns publication undo"), Created.TransactionId.IsValid() && Created.TransactionId != PriorTransaction &&
		Created.Title.EqualTo(NSLOCTEXT("Nwiro", "CreateStruct", "AI: Create Struct"))))
	{
		GEditor->UndoTransaction();
		TestNull(TEXT("undo removes published path"), Fixture.Find(StructName));
		if (AssertExactTypes())
		{
			GEditor->RedoTransaction();
			TestTrue(TEXT("redo restores same reference struct"), Fixture.Find(StructName) == Struct.Get());
			AssertExactTypes();
		}
	}

	// Reject the whole batch before publishing even when its first field is valid.
	for (const TCHAR* Invalid : {
		TEXT("\"class\""), TEXT("{\"base\":\"object\"}"),
		TEXT("{\"base\":\"class\",\"subtype\":7}"),
		TEXT("{\"base\":\"class\",\"subtype\":\"\"}"),
		TEXT("{\"base\":\"class\",\"subtype\":\" Actor\"}"),
		TEXT("{\"base\":\"class\",\"subtype\":\"/Game/Missing.BP_Missing_C\"}"),
		TEXT("{\"base\":\"class\",\"subtype\":\"/Script/CoreUObject.Vector\"}"),
		TEXT("{\"base\":\"object\",\"subtype\":\"/Script/CoreUObject.Interface\"}"),
		TEXT("{\"base\":\"struct\",\"subtype\":\"Actor\"}"),
		TEXT("{\"base\":\"enum\",\"subtype\":\"Actor\"}"),
		TEXT("{\"base\":\"int\",\"subtype\":\"Actor\"}"),
		TEXT("{\"base\":\"class\",\"subtype\":\"Actor\",\"container\":\"set\"}"),
		TEXT("{\"base\":\"int\",\"container\":\"map\"}"),
		TEXT("{\"base\":\"int\",\"container\":false}"),
		TEXT("{\"base\":\"int\",\"typo\":\"array\"}"),
		TEXT("{\"base\":false}"), TEXT("false"), TEXT("[]"), TEXT("null") })
	{
		const FString RejectedName = Fixture.NewName(TEXT("ReferenceRejected"));
		Fixture.Call(true, RejectedName, FString::Printf(TEXT("[{\"name\":\"First\",\"type\":\"int\"},{\"name\":\"Invalid\",\"type\":%s}]"), Invalid), false);
		Fixture.AssertAbsent(RejectedName);
	}
	Fixture.Reject(true, TEXT("[{\"name\":\"Mixed\",\"type\":{\"base\":\"class\",\"subtype\":\"Actor\"},\"subtype\":\"Object\"}]"));
	Fixture.Reject(true, TEXT("[{\"name\":\"Mixed\",\"type\":{\"base\":\"int\"},\"container\":\"array\"}]"));
	Fixture.Reject(true, TEXT("[{\"name\":\"Bad\",\"type\":\"int\",\"subtype\":null}]"));
	Fixture.Reject(true, TEXT("[{\"name\":\"Bad\",\"type\":\"int\\u0000ignored\"}]"));
	Fixture.Reject(true, TEXT("[{\"name\":\"Bad\\nName\",\"type\":\"int\"}]"));
	Fixture.Reject(true, TEXT("[{\"name\":\"Bad\",\"type\":\"class\",\"subtype\":\"Actor\\u0000ignored\"}]"));
	const FString LongName = Fixture.NewName(TEXT("OversizedReference"));
	Fixture.Call(true, LongName, FString::Printf(TEXT("[{\"name\":\"Bad\",\"type\":\"class\",\"subtype\":\"%s\"}]"),
		*FString::ChrN(NAME_SIZE, TEXT('A'))), false);
	Fixture.AssertAbsent(LongName);

	const FString AssetPathRejected = Fixture.NewName(TEXT("BlueprintAssetReference"));
	Fixture.Call(true, AssetPathRejected, FString::Printf(TEXT("[{\"name\":\"Class\",\"type\":\"class\",\"subtype\":\"%s\"}]"),
		*Blueprint->GetPathName()), false);
	Fixture.AssertAbsent(AssetPathRejected);

	// A compiled transient type is still unsuitable as a persistent asset dependency.
	{
		TStrongObjectPtr<UUserDefinedStruct> Transient(FStructureEditorUtils::CreateUserDefinedStruct(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass()),
			RF_Transient | RF_Transactional));
		if (TestNotNull(TEXT("transient nested fixture"), Transient.Get()))
		{
			TestTrue(TEXT("transient fixture is otherwise compiled"), Transient->Status == UDSS_UpToDate);
			const FString RejectedName = Fixture.NewName(TEXT("TransientReference"));
			Fixture.Call(true, RejectedName, FString::Printf(TEXT("[{\"name\":\"Data\",\"type\":\"struct\",\"subtype\":\"%s\"}]"),
				*Transient->GetPathName()), false);
			Fixture.AssertAbsent(RejectedName);
			Transient->ClearFlags(RF_Public | RF_Standalone);
			Transient->MarkAsGarbage();
		}
	}

	auto AssertSingleEnumReference = [&](const FString& Name, UObject* ExpectedEnum)
	{
		UUserDefinedStruct* Exact = Cast<UUserDefinedStruct>(Fixture.Find(Name));
		if (!TestNotNull(TEXT("exact path disambiguates"), Exact)) return;
		const auto& Descriptions = FStructureEditorUtils::GetVarDesc(Exact);
		if (!TestEqual(TEXT("exact reference has one field"), Descriptions.Num(), 1)) return;
		TestTrue(TEXT("exact editor enum selected"), Descriptions[0].ToPinType().PinSubCategoryObject.Get() == ExpectedEnum);
		FProperty* Property = FindFProperty<FProperty>(Exact, Descriptions[0].VarName);
		if (!TestNotNull(TEXT("exact enum reflection exists"), Property)) return;
		FByteProperty* Byte = CastField<FByteProperty>(Property);
		FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
		TestTrue(TEXT("exact reflected enum selected"),
			(Byte && Byte->Enum == ExpectedEnum) || (EnumProperty && EnumProperty->GetEnum() == ExpectedEnum));
	};

	// A non-native type in a real package is still unsaveable when explicitly transient.
	// This distinguishes the type's own Native identity from its native UClass implementation.
	TestFalse(TEXT("user struct is not native"), Nested->IsNative());
	TestTrue(TEXT("user struct belongs to a persistent package"), Nested->GetOutermost() != GetTransientPackage());
	{
		Nested->SetFlags(RF_Transient);
		ON_SCOPE_EXIT { Nested->ClearFlags(RF_Transient); };
		for (const TCHAR* Container : { TEXT("none"), TEXT("array") })
		{
			const FString RejectedName = Fixture.NewName(TEXT("TransientFlagReference"));
			Fixture.Call(true, RejectedName, FString::Printf(TEXT("[{\"name\":\"Data\",\"type\":\"struct\",\"subtype\":\"%s\",\"container\":\"%s\"}]"),
				*Nested->GetPathName(), Container), false);
			Fixture.AssertAbsent(RejectedName);
		}
	}

	// Two equal short names must never pick whichever object enumeration visits first.
	FFixture OtherFixture(*this);
	const FString AmbiguousName = TEXT("Ambiguous_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Fixture.Packages.Add(Fixture.Root / AmbiguousName);
	OtherFixture.Packages.Add(OtherFixture.Root / AmbiguousName);
	if (Fixture.Call(false, AmbiguousName, TEXT("[\"One\"]"), true) &&
		OtherFixture.Call(false, AmbiguousName, TEXT("[\"Two\"]"), true))
	{
		const FString RejectedName = Fixture.NewName(TEXT("AmbiguousReference"));
		Fixture.Call(true, RejectedName, FString::Printf(TEXT("[{\"name\":\"State\",\"type\":\"enum\",\"subtype\":\"%s\"}]"), *AmbiguousName), false);
		Fixture.AssertAbsent(RejectedName);
		const FString ExactName = Fixture.NewName(TEXT("ExactReference"));
		if (Fixture.Call(true, ExactName, FString::Printf(TEXT("[{\"name\":\"State\",\"type\":\"enum\",\"subtype\":\"%s\"}]"), *OtherFixture.ObjectPath(AmbiguousName)), true))
		{
			AssertSingleEnumReference(ExactName, OtherFixture.Find(AmbiguousName));
		}
	}
	// An exact reflected name and its C++ prefix alias may name different loaded types.
	const FString AliasName = TEXT("Collision_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PrefixedName = TEXT("E") + AliasName;
	Fixture.Packages.Add(Fixture.Root / AliasName);
	Fixture.Packages.Add(Fixture.Root / PrefixedName);
	if (Fixture.Call(false, AliasName, TEXT("[\"One\"]"), true) &&
		Fixture.Call(false, PrefixedName, TEXT("[\"Two\"]"), true))
	{
		const FString RejectedName = Fixture.NewName(TEXT("PrefixCollision"));
		Fixture.Call(true, RejectedName, FString::Printf(TEXT("[{\"name\":\"State\",\"type\":\"enum\",\"subtype\":\"%s\"}]"), *PrefixedName), false);
		Fixture.AssertAbsent(RejectedName);
		const FString ExactName = Fixture.NewName(TEXT("ExactPrefixedReference"));
		if (Fixture.Call(true, ExactName, FString::Printf(TEXT("[{\"name\":\"State\",\"type\":\"enum\",\"subtype\":\"%s\"}]"),
			*Fixture.ObjectPath(PrefixedName)), true))
		{
			AssertSingleEnumReference(ExactName, Fixture.Find(PrefixedName));
		}
	}
	Fixture.AssertNotSaved(EnumName);
	Fixture.AssertNotSaved(NestedName);
	Fixture.AssertNotSaved(BlueprintName);
	return true;
}
