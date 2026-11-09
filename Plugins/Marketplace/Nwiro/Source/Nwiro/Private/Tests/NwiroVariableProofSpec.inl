// Copyright 2026 Nwiro. All Rights Reserved.
// Exercises public EditBlueprint with Actor and actual Widget Blueprint targets.
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/StrongObjectPtr.h"

namespace NwiroVariableProof
{
	static FString Serialize(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		FJsonSerializer::Serialize(Obj, TJsonWriterFactory<>::Create(&Out));
		return Out;
	}

	static TSharedPtr<FJsonValue> Variable(const FString& Name, const FString& Type)
	{
		const auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Obj);
	}

	static TSharedPtr<FJsonValue> Typed(const FString& Name, const FString& Base,
		const FString& Subtype = FString(), const FString& Container = TEXT("none"))
	{
		const auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		const auto Type = MakeShared<FJsonObject>();
		Type->SetStringField(TEXT("base"), Base);
		if (!Subtype.IsEmpty()) Type->SetStringField(TEXT("subtype"), Subtype);
		Type->SetStringField(TEXT("container"), Container);
		Obj->SetObjectField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Obj);
	}

	static TSharedPtr<FJsonObject> Edit(UBlueprint* BP,
		const TArray<TSharedPtr<FJsonValue>>& Items, bool bOmitCompile = false,
		const FString& Op = TEXT("add_variables"))
	{
		const auto Cmd = MakeShared<FJsonObject>();
		Cmd->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Cmd->SetArrayField(Op, Items);
		if (!bOmitCompile) Cmd->SetBoolField(TEXT("compile"), false);
		TSharedPtr<FJsonObject> Out;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroBlueprintTools::EditBlueprint(Serialize(Cmd))), Out);
		return Out;
	}

	struct FFixture
	{
		TStrongObjectPtr<UBlueprint> BP;
		UPackage* Package = nullptr;
		FString FileName;

		explicit FFixture(bool bWidget)
		{
			const FString Name = TEXT("BP_VariableProof_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Path = TEXT("/Game/NwiroVariableAcceptance_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/") + Name;
			Package = CreatePackage(*Path);
			FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
			BP.Reset(FKismetEditorUtilities::CreateBlueprint(bWidget ? UUserWidget::StaticClass() : AActor::StaticClass(),
				Package, FName(*Name), BPTYPE_Normal,
				bWidget ? UWidgetBlueprint::StaticClass() : UBlueprint::StaticClass(),
				bWidget ? UWidgetBlueprintGeneratedClass::StaticClass() : UBlueprintGeneratedClass::StaticClass(),
				TEXT("NwiroVariableAcceptance")));
			if (!BP.IsValid()) return;
			if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP.Get()))
			{
				WidgetBP->WidgetTree->RootWidget = WidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ProofRoot"));
			}
			FEdGraphPinType Type;
			Type.PinCategory = UEdGraphSchema_K2::PC_Int;
			FBlueprintEditorUtils::AddMemberVariable(BP.Get(), TEXT("TrackInput"), Type, TEXT("73"));
			FKismetEditorUtilities::CompileBlueprint(BP.Get(), EBlueprintCompileOptions::SkipSave);
			Package->SetDirtyFlag(false);
		}

		~FFixture()
		{
			// Every object belongs to this GUID package. There is no SaveAll, disk
			// deletion, global transaction reset or GC of unrelated user assets.
			if (Package)
			{
				ForEachObjectWithPackage(Package, [](UObject* Obj)
				{
					Obj->ClearFlags(RF_Public | RF_Standalone);
					Obj->MarkAsGarbage();
					return true;
				}, true);
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}
	};

	struct FSnapshot
	{
		TArray<FBPVariableDescription> Variables;
		TArray<UEdGraph*> Graphs;
		TArray<UEdGraphNode*> Nodes;
		TArray<FProperty*> Properties;
		UClass* Generated;
		UClass* Skeleton;
		UWidgetTree* Tree;
		UWidget* Root;
		EBlueprintStatus Status;
		bool bDirty;
		bool bCached;

		explicit FSnapshot(UBlueprint* BP)
			: Variables(BP->NewVariables), Generated(BP->GeneratedClass), Skeleton(BP->SkeletonGeneratedClass),
			Tree(Cast<UWidgetBlueprint>(BP) ? CastChecked<UWidgetBlueprint>(BP)->WidgetTree : nullptr),
			Root(Tree ? Tree->RootWidget.Get() : nullptr), Status(BP->Status), bDirty(BP->GetOutermost()->IsDirty()),
			bCached(BP->bCachedDependenciesUpToDate)
		{
			BP->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes) Nodes.Add(Node);
			for (TFieldIterator<FProperty> It(Generated); It; ++It) Properties.Add(*It);
		}

		bool Matches(UBlueprint* BP) const
		{
			if (Variables.Num() != BP->NewVariables.Num() || Generated != BP->GeneratedClass
				|| Skeleton != BP->SkeletonGeneratedClass || Status != BP->Status
				|| bDirty != BP->GetOutermost()->IsDirty() || bCached != BP->bCachedDependenciesUpToDate) return false;
			for (int32 Index = 0; Index < Variables.Num(); ++Index)
				if (!FBPVariableDescription::StaticStruct()->CompareScriptStruct(&Variables[Index], &BP->NewVariables[Index], 0)) return false;
			TArray<UEdGraph*> CurrentGraphs;
			BP->GetAllGraphs(CurrentGraphs);
			if (CurrentGraphs != Graphs) return false;
			TArray<UEdGraphNode*> CurrentNodes;
			for (UEdGraph* Graph : CurrentGraphs) for (UEdGraphNode* Node : Graph->Nodes) CurrentNodes.Add(Node);
			if (CurrentNodes != Nodes) return false;
			TArray<FProperty*> CurrentProperties;
			for (TFieldIterator<FProperty> It(BP->GeneratedClass); It; ++It) CurrentProperties.Add(*It);
			const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP);
			return CurrentProperties == Properties && (!WidgetBP || (WidgetBP->WidgetTree == Tree && Tree->RootWidget == Root));
		}
	};

	static bool Reflected(UBlueprint* BP, const FString& Name, const FName Category,
		UObject* Subtype = nullptr, const EPinContainerType Container = EPinContainerType::None,
		const FName Subcategory = NAME_None)
	{
		FProperty* Property = FindFProperty<FProperty>(BP->GeneratedClass, FName(*Name));
		FEdGraphPinType Pin;
		return Property && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, Pin)
			&& Pin.PinCategory == Category && Pin.PinSubCategory == Subcategory
			&& Pin.PinSubCategoryObject.Get() == Subtype && Pin.ContainerType == Container;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroVariablesPreflightProof,
	"Nwiro.Pro.Blueprint.Variables.Production.FailedNoTouch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroVariablesPreflightProof::RunTest(const FString& Parameters)
{
	using namespace NwiroVariableProof;
	TArray<TSharedPtr<FJsonValue>> Invalid = {
		Typed(TEXT("AttemptedB"), TEXT("struct")),
		Typed(TEXT("AttemptedB"), TEXT("struct"), TEXT("Text")),
		Typed(TEXT("AttemptedB"), TEXT("struct"), TEXT("/Script/Engine.Actor")),
		Typed(TEXT("AttemptedB"), TEXT("enum")),
		Typed(TEXT("AttemptedB"), TEXT("enum"), TEXT("DoesNotExist_Proof")),
		Typed(TEXT("AttemptedB"), TEXT("object")),
		Typed(TEXT("AttemptedB"), TEXT("class"), TEXT("DoesNotExist_Proof")),
		Typed(TEXT("AttemptedB"), TEXT("class"), TEXT("/Script/CoreUObject.Vector")),
		Variable(TEXT("AttemptedB"), TEXT("DoesNotExist_Proof")),
		Variable(TEXT("AttemptedB"), TEXT("")),
		Typed(TEXT("AttemptedB"), TEXT("")),
		Typed(TEXT("AttemptedB"), TEXT("int"), TEXT("Actor")),
		Typed(TEXT("AttemptedB"), TEXT("int"), FString(), TEXT("map")),
		Typed(TEXT("AttemptedB"), TEXT("int"), FString(), TEXT("invalid")),
		Typed(TEXT("AttemptedB"), TEXT("bool"), FString(), TEXT("set")),
		Typed(TEXT("AttemptedB"), TEXT("text"), FString(), TEXT("set")),
		Variable(TEXT("attempteda"), TEXT("int")),
		Variable(TEXT("TrackInput"), TEXT("int")),
		Variable(TEXT("None"), TEXT("int")),
		Variable(TEXT("bad\"\\name"), TEXT("int")),
		Variable(TEXT("embedded\tcontrol"), TEXT("int")),
		Variable(FString::ChrN(NAME_SIZE, TCHAR('N')), TEXT("int")),
		MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("not an object")), MakeShared<FJsonValueNumber>(42)
	};
	Invalid.Add(Variable(TEXT("AttemptedB"), FString(6, TEXT("int\0xx"))));
	auto NumericName = Variable(TEXT("AttemptedB"), TEXT("int"));
	NumericName->AsObject()->SetNumberField(TEXT("name"), 17); Invalid.Add(NumericName);
	auto NumericBase = Typed(TEXT("AttemptedB"), TEXT("int"));
	NumericBase->AsObject()->GetObjectField(TEXT("type"))->SetNumberField(TEXT("base"), 17); Invalid.Add(NumericBase);
	auto BadFlag = Variable(TEXT("AttemptedB"), TEXT("int"));
	BadFlag->AsObject()->SetStringField(TEXT("replicated"), TEXT("true")); Invalid.Add(BadFlag);
	auto BadDefault = Variable(TEXT("AttemptedB"), TEXT("int"));
	BadDefault->AsObject()->SetNumberField(TEXT("default"), 7); Invalid.Add(BadDefault);
	auto BadContainer = Variable(TEXT("AttemptedB"), TEXT("int"));
	BadContainer->AsObject()->SetBoolField(TEXT("container"), true); Invalid.Add(BadContainer);
	auto Mixed = Typed(TEXT("AttemptedB"), TEXT("object"), TEXT("Actor"));
	Mixed->AsObject()->SetStringField(TEXT("subtype"), TEXT("Actor")); Invalid.Add(Mixed);
	auto BadTerminal = Typed(TEXT("AttemptedB"), TEXT("int"));
	BadTerminal->AsObject()->GetObjectField(TEXT("type"))->SetStringField(TEXT("value"), TEXT("float")); Invalid.Add(BadTerminal);
	auto DuplicateDefault = Variable(TEXT("AttemptedB"), TEXT("int"));
	DuplicateDefault->AsObject()->SetStringField(TEXT("default"), TEXT("1"));
	DuplicateDefault->AsObject()->SetStringField(TEXT("default_value"), TEXT("2")); Invalid.Add(DuplicateDefault);

	// Flag-only transient dependencies in real /Game packages must still reject.
	FFixture FlaggedTypes(false);
	if (!TestNotNull(TEXT("persistent-package dependency fixture"), FlaggedTypes.BP.Get())) return false;
	UClass* FlaggedClass = FlaggedTypes.BP->GeneratedClass;
	if (!TestNotNull(TEXT("generated dependency class"), FlaggedClass)) return false;
	TestTrue(TEXT("dependency class has native Actor parent"), FlaggedClass->GetSuperClass() == AActor::StaticClass());
	TestFalse(TEXT("native parent does not make generated class native"), FlaggedClass->IsNative());
	TestTrue(TEXT("class dependency is outside the transient package"), FlaggedClass->GetOutermost() != GetTransientPackage());
	FlaggedClass->SetFlags(RF_Transient);

	TStrongObjectPtr<UUserDefinedStruct> FlaggedStruct(FStructureEditorUtils::CreateUserDefinedStruct(
		FlaggedTypes.Package, MakeUniqueObjectName(FlaggedTypes.Package,
			UUserDefinedStruct::StaticClass(), TEXT("FlaggedVariableDependency")), RF_Transactional));
	if (!TestNotNull(TEXT("persistent-package user struct dependency"), FlaggedStruct.Get())) return false;
	TestEqual(TEXT("user struct otherwise compiled"), FlaggedStruct->Status.GetValue(), UDSS_UpToDate);
	TestFalse(TEXT("user struct is not a native type"), FlaggedStruct->IsNative());
	TestTrue(TEXT("struct dependency is outside the transient package"), FlaggedStruct->GetOutermost() != GetTransientPackage());
	FlaggedStruct->SetFlags(RF_Transient);

	Invalid.Add(Typed(TEXT("AttemptedB"), TEXT("class"), FlaggedClass->GetPathName()));
	Invalid.Add(Typed(TEXT("AttemptedB"), TEXT("object"), FlaggedClass->GetPathName(), TEXT("array")));
	Invalid.Add(Typed(TEXT("AttemptedB"), TEXT("struct"), FlaggedStruct->GetPathName()));
	Invalid.Add(Typed(TEXT("AttemptedB"), TEXT("struct"), FlaggedStruct->GetPathName(), TEXT("array")));

	for (bool bWidget : { false, true })
	{
		FFixture Fixture(bWidget);
		if (!TestNotNull(TEXT("real Blueprint fixture"), Fixture.BP.Get())) return false;
		UBlueprint* BP = Fixture.BP.Get();
		if (!TestTrue(TEXT("fixture owns initial TrackInput declaration"), BP->NewVariables.IsValidIndex(0)
			&& BP->NewVariables[0].VarName == TEXT("TrackInput"))) return false;
		TestEqual(TEXT("fixture compiled"), BP->Status.GetValue(), BS_UpToDate);
		Invalid.Add(Variable(bWidget ? TEXT("bIsEnabled") : TEXT("Tags"), TEXT("int")));
		for (bool bDirty : { false, true })
		{
			if (bDirty)
			{
				BP->NewVariables[0].DefaultValue = TEXT("918");
				BP->NewVariables[0].SetMetaData(FBlueprintMetadata::MD_Tooltip, TEXT("Keep this unsaved \"edit\""));
				BP->Status = BS_Dirty;
				Fixture.Package->SetDirtyFlag(true);
			}
			const FSnapshot Before(BP);
			for (bool bOmitCompile : { false, true }) for (bool bReverse : { false, true })
			{
				for (int32 Case = 0; Case < Invalid.Num(); ++Case)
				{
					TArray<TSharedPtr<FJsonValue>> Items = { Variable(TEXT("AttemptedA"), TEXT("int")), Invalid[Case] };
					if (bReverse) Swap(Items[0], Items[1]);
					int32 Compiles = 0, Saves = 0;
					const auto CompileHandle = BP->OnCompiled().AddLambda([&](UBlueprint*) { ++Compiles; });
					const auto SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
					{ if (Object && Object->GetOutermost() == Fixture.Package) ++Saves; });
					const auto Result = Edit(BP, Items, bOmitCompile);
					BP->OnCompiled().Remove(CompileHandle);
					FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
					const FString Context = FString::Printf(TEXT("widget=%d dirty=%d omitted=%d reverse=%d case=%d"), bWidget, bDirty, bOmitCompile, bReverse, Case);
					if (!TestTrue(Context + TEXT(" parses JSON"), Result.IsValid())) return false;
					TestFalse(Context + TEXT(" rejects batch"), Result->GetBoolField(TEXT("success")));
					bool bSaved = true;
					TestTrue(Context + TEXT(" savedToDisk is explicitly false"), Result->TryGetBoolField(TEXT("savedToDisk"), bSaved) && !bSaved);
					TestEqual(Context + TEXT(" no compile callbacks"), Compiles, 0);
					TestEqual(Context + TEXT(" no save callbacks"), Saves, 0);
					TestTrue(Context + TEXT(" exact source/reflection/tree/graph/status/dirty state"), Before.Matches(BP));
					TestFalse(Context + TEXT(" no asset file"), IFileManager::Get().FileExists(*Fixture.FileName));
					const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
					if (!TestTrue(Context + TEXT(" one operation ledger is present"), Result->TryGetArrayField(TEXT("operations"), Operations)
						&& Operations && Operations->Num() == 1 && (*Operations)[0].IsValid()
						&& (*Operations)[0]->Type == EJson::Object && (*Operations)[0]->AsObject().IsValid())) return false;
					const auto Ledger = (*Operations)[0]->AsObject();
					TestEqual(Context + TEXT(" requested unit count preserved"), Ledger->GetNumberField(TEXT("requestUnits")), 2.0);

					TestEqual(Context + TEXT(" all units accounted"), Ledger->GetArrayField(TEXT("errors")).Num() + Ledger->GetArrayField(TEXT("skipped")).Num(), 2);
					TestEqual(Context + TEXT(" applied zero"), Ledger->GetArrayField(TEXT("created")).Num(), 0);
				}
			}
		}
		Invalid.Pop();
	}
	const FString Missing = TEXT("/Game/BP_NwiroInvalidCreate_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const auto Command = MakeShared<FJsonObject>();
	Command->SetStringField(TEXT("blueprint"), Missing);
	Command->SetArrayField(TEXT("variables"), { Typed(TEXT("Bad"), TEXT("struct"), TEXT("Text")) });
	const FString Response = FNwiroBlueprintTools::EditBlueprint(Serialize(Command));
	TestTrue(TEXT("invalid alias request rejects before implicit target creation"), Response.Contains(TEXT("\"success\": false")) || Response.Contains(TEXT("\"success\":false")));
	TestNull(TEXT("invalid request did not allocate target package"), FindPackage(nullptr, *Missing));
	const auto CreateCommand = MakeShared<FJsonObject>();
	CreateCommand->SetStringField(TEXT("action"), TEXT("CreateBlueprint"));
	CreateCommand->SetStringField(TEXT("name"), FPackageName::GetShortName(Missing));
	CreateCommand->SetStringField(TEXT("parentClass"), TEXT("Actor"));
	CreateCommand->SetStringField(TEXT("path"), TEXT("/Game"));
	CreateCommand->SetArrayField(TEXT("add_variables"), { Typed(TEXT("Bad"), TEXT("struct"), TEXT("Text")) });
	const FString CreateResponse = FNwiroBlueprintTools::EditBlueprint(Serialize(CreateCommand));
	TestTrue(TEXT("explicit create validates variables before creation"), CreateResponse.Contains(TEXT("operations")));
	TestNull(TEXT("invalid explicit create did not allocate a package"), FindPackage(nullptr, *Missing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroVariablesReflectionProof,
	"Nwiro.Pro.Blueprint.Variables.Production.ReflectedTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroVariablesReflectionProof::RunTest(const FString& Parameters)
{
	using namespace NwiroVariableProof;
	UPackage* TypePackage = CreatePackage(*(TEXT("/Game/NwiroVariableTypeProof_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		ForEachObjectWithPackage(TypePackage, [](UObject* Obj)
		{
			Obj->ClearFlags(RF_Public | RF_Standalone);
			Obj->MarkAsGarbage();
			return true;
		}, true);
		TypePackage->SetDirtyFlag(false);
		TypePackage->MarkAsGarbage();
	};
	TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(TypePackage,
		MakeUniqueObjectName(TypePackage, UUserDefinedStruct::StaticClass(), TEXT("VariableProofStruct")), RF_Transactional));
	TStrongObjectPtr<UUserDefinedEnum> Enum(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(TypePackage,
		MakeUniqueObjectName(TypePackage, UUserDefinedEnum::StaticClass(), TEXT("VariableProofEnum")), RF_Public | RF_Transactional)));
	if (!TestNotNull(TEXT("compiled user struct"), Struct.Get()) || !TestNotNull(TEXT("user enum"), Enum.Get())) return false;
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum.Get());
	for (bool bWidget : { false, true })
	{
		FFixture Fixture(bWidget);
		UBlueprint* BP = Fixture.BP.Get();
		if (!TestNotNull(TEXT("positive fixture"), BP)) return false;
		const int32 InitialIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("TrackInput"));
		if (!TestTrue(TEXT("category omission baseline exists before public mutation"), BP->NewVariables.IsValidIndex(InitialIndex))) return false;
		const FString DefaultCategory = BP->NewVariables[InitialIndex].Category.ToString();
		TArray<TSharedPtr<FJsonValue>> Items;
		const TArray<FString> Aliases = { TEXT("bool"), TEXT("boolean"), TEXT("byte"), TEXT("int"), TEXT("integer"), TEXT("int32"), TEXT("int64"),
			TEXT("float"), TEXT("double"), TEXT("string"), TEXT("fstring"), TEXT("name"), TEXT("fname"), TEXT("text"), TEXT("ftext"),
			TEXT("vector"), TEXT("fvector"), TEXT("rotator"), TEXT("frotator"), TEXT("transform"), TEXT("ftransform"), TEXT("linearcolor"), TEXT("color") };
		for (int32 Index = 0; Index < Aliases.Num(); ++Index)
			Items.Add(Variable(FString::Printf(TEXT("Alias_%d"), Index), Aliases[Index].ToUpper()));
		Items.Append({ Typed(TEXT("ActorObject"), TEXT("object"), TEXT("AActor")),
			Typed(TEXT("ActorClass"), TEXT("class"), TEXT("/Script/Engine.Actor")),
			Typed(TEXT("UserStruct"), TEXT("struct"), Struct->GetPathName()),
			Typed(TEXT("UserEnum"), TEXT("enum"), Enum->GetPathName()),
			Typed(TEXT("ArrayOfActors"), TEXT("object"), TEXT("Actor"), TEXT("array")),
			Typed(TEXT("SetOfInts"), TEXT("int"), FString(), TEXT("set")),
			Variable(TEXT("Vector2D"), TEXT("Vector2D")) });
		auto TopLevelArray = Variable(TEXT("ArrayOfStructs"), TEXT("struct"));
		TopLevelArray->AsObject()->SetStringField(TEXT("subtype"), Struct->GetPathName());
		TopLevelArray->AsObject()->SetStringField(TEXT("container"), TEXT("array"));
		Items.Add(TopLevelArray);
		auto Options = Variable(TEXT("Options"), TEXT("int"));
		Options->AsObject()->SetStringField(TEXT("default"), TEXT("31"));
		Options->AsObject()->SetStringField(TEXT("category"), TEXT("Proof Category"));
		Options->AsObject()->SetStringField(TEXT("tooltip"), TEXT("quoted \"value\" \\ preserved"));
		Options->AsObject()->SetBoolField(TEXT("save_game"), true);
		Options->AsObject()->SetBoolField(TEXT("expose_on_spawn"), true);
		Items.Add(Options);
		const auto Result = Edit(BP, Items, false, TEXT("variables"));
		if (!TestTrue(TEXT("positive batch success"), Result.IsValid() && Result->GetBoolField(TEXT("success"))))
		{
			if (Result.IsValid()) AddError(Serialize(Result.ToSharedRef()));
			return false;
		}
		TestFalse(TEXT("positive compile:false does not save"), Result->GetBoolField(TEXT("savedToDisk")));
		TestEqual(TEXT("reported exact applied count"), Result->GetArrayField(TEXT("operations"))[0]->AsObject()->GetArrayField(TEXT("created")).Num(), Items.Num());
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
		TestEqual(TEXT("positive generated class compiled"), BP->Status.GetValue(), BS_UpToDate);
		if (!TestNotNull(TEXT("compiled generated class"), BP->GeneratedClass.Get())) return false;
		TestEqual(TEXT("exact member count"), BP->NewVariables.Num(), Items.Num() + 1);
		const TArray<FName> Categories = { UEdGraphSchema_K2::PC_Boolean, UEdGraphSchema_K2::PC_Boolean, UEdGraphSchema_K2::PC_Byte,
			UEdGraphSchema_K2::PC_Int, UEdGraphSchema_K2::PC_Int, UEdGraphSchema_K2::PC_Int, UEdGraphSchema_K2::PC_Int64,
			UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_String, UEdGraphSchema_K2::PC_String,
			UEdGraphSchema_K2::PC_Name, UEdGraphSchema_K2::PC_Name, UEdGraphSchema_K2::PC_Text, UEdGraphSchema_K2::PC_Text };
		for (int32 Index = 0; Index < Aliases.Num(); ++Index)
		{
			UObject* Subtype = Index == 15 || Index == 16 ? TBaseStructure<FVector>::Get()
				: Index == 17 || Index == 18 ? static_cast<UObject*>(TBaseStructure<FRotator>::Get())
				: Index == 19 || Index == 20 ? static_cast<UObject*>(TBaseStructure<FTransform>::Get())
				: Index >= 21 ? static_cast<UObject*>(TBaseStructure<FLinearColor>::Get()) : nullptr;
			TestTrue(TEXT("reflected alias ") + Aliases[Index], Reflected(BP, FString::Printf(TEXT("Alias_%d"), Index),
				Index < Categories.Num() ? Categories[Index] : UEdGraphSchema_K2::PC_Struct, Subtype, EPinContainerType::None,
				Index == 7 ? UEdGraphSchema_K2::PC_Float : Index == 8 ? UEdGraphSchema_K2::PC_Double : NAME_None));
		}
		TestTrue(TEXT("exact Actor object"), Reflected(BP, TEXT("ActorObject"), UEdGraphSchema_K2::PC_Object, AActor::StaticClass()));
		TestTrue(TEXT("exact Actor class"), Reflected(BP, TEXT("ActorClass"), UEdGraphSchema_K2::PC_Class, AActor::StaticClass()));
		TestTrue(TEXT("exact user struct"), Reflected(BP, TEXT("UserStruct"), UEdGraphSchema_K2::PC_Struct, Struct.Get()));
		TestTrue(TEXT("exact user enum"), Reflected(BP, TEXT("UserEnum"), UEdGraphSchema_K2::PC_Byte, Enum.Get()));
		TestTrue(TEXT("array element identity"), Reflected(BP, TEXT("ArrayOfActors"), UEdGraphSchema_K2::PC_Object, AActor::StaticClass(), EPinContainerType::Array));
		TestTrue(TEXT("set element identity"), Reflected(BP, TEXT("SetOfInts"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::Set));
		TestTrue(TEXT("top-level container preserved"), Reflected(BP, TEXT("ArrayOfStructs"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::Array));
		TestTrue(TEXT("raw Vector2D with existing TrackInput"), Reflected(BP, TEXT("Vector2D"), UEdGraphSchema_K2::PC_Struct, TBaseStructure<FVector2D>::Get()));
		FIntProperty* OptionsProperty = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("Options"));
		UObject* CDO = BP->GeneratedClass->GetDefaultObject(false);
		if (!TestNotNull(TEXT("compiled Options integer property"), OptionsProperty)
			|| !TestNotNull(TEXT("compiled class default object"), CDO)) return false;
		// Full compilation consumes NewVariables.DefaultValue and places the
		// actual default on the generated CDO (KismetCompiler.cpp).
		TestEqual(TEXT("compiled default preserved on CDO"), OptionsProperty->GetPropertyValue_InContainer(CDO), 31);
		TestTrue(TEXT("savegame reflected"), OptionsProperty->HasAnyPropertyFlags(CPF_SaveGame));
		TestFalse(TEXT("positive fixture remained unsaved"), IFileManager::Get().FileExists(*Fixture.FileName));
		// FIX038: use the actual add_variables spelling, then read categories
		// through the public serializer on both Actor and Widget fixtures.
		auto SecondCategory = Variable(TEXT("SecondCategory"), TEXT("int"));
		SecondCategory->AsObject()->SetStringField(TEXT("category"), TEXT("Secondary|Grouping"));
		const auto CategoryResult = Edit(BP, { SecondCategory, Variable(TEXT("OmittedCategory"), TEXT("int")) });
		if (!TestTrue(TEXT("public add_variables category batch succeeds"), CategoryResult.IsValid() && CategoryResult->GetBoolField(TEXT("success")))) return false;
		TestFalse(TEXT("category batch does not save"), CategoryResult->GetBoolField(TEXT("savedToDisk")));
		TSharedPtr<FJsonObject> Readback;
		const FString ReadText = FNwiroBlueprintTools::ReadBlueprint(BP->GetPathName());
		const TArray<TSharedPtr<FJsonValue>>* ReadVariables = nullptr;
		if (!TestTrue(TEXT("public read_blueprint returns exact fixture variables"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReadText), Readback)
			&& Readback.IsValid() && !Readback->HasField(TEXT("error")) && Readback->GetStringField(TEXT("path")) == BP->GetPathName()
			&& Readback->TryGetArrayField(TEXT("variables"), ReadVariables))) return false;
		const auto CheckCategory = [&](const TCHAR* Name, const FString& Expected)
		{
			int32 Matches = 0;
			FString Actual;
			for (const TSharedPtr<FJsonValue>& Value : *ReadVariables)
			{
				const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
				if (Row.IsValid() && Row->GetStringField(TEXT("name")) == Name)
				{
					++Matches;
					Actual = Row->GetStringField(TEXT("category"));
				}
			}
			const FString Label = FString(bWidget ? TEXT("Widget category ") : TEXT("Actor category ")) + Name;
			TestEqual(Label + TEXT(" exact readback identity"), Matches, 1);
			TestEqual(Label + TEXT(" exact public value"), Actual, Expected);
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Name);
			if (TestTrue(Label + TEXT(" native declaration exists"), BP->NewVariables.IsValidIndex(Index)))
				TestEqual(Label + TEXT(" exact native value"), BP->NewVariables[Index].Category.ToString(), Expected);
		};
		CheckCategory(TEXT("Options"), TEXT("Proof Category"));
		CheckCategory(TEXT("SecondCategory"), TEXT("Secondary|Grouping"));
		CheckCategory(TEXT("OmittedCategory"), DefaultCategory);
		CheckCategory(TEXT("Alias_0"), DefaultCategory);
		CheckCategory(TEXT("TrackInput"), DefaultCategory);
		// Incident-shaped retry: invalid struct/Text cannot poison the name; valid
		// text in the following public request compiles on Actor and Widget fixtures.
		const FSnapshot Before(BP);
		auto UnappliedCategory = Variable(TEXT("UnappliedCategory"), TEXT("int"));
		UnappliedCategory->AsObject()->SetStringField(TEXT("category"), TEXT("Must not apply"));
		const auto Rejected = Edit(BP, { UnappliedCategory, Typed(TEXT("RetryAsText"), TEXT("struct"), TEXT("Text")) }, true);
		TestTrue(TEXT("invalid Text subtype rejects without touch"), Rejected.IsValid() && !Rejected->GetBoolField(TEXT("success")) && Before.Matches(BP));
		const auto Retry = Edit(BP, { Variable(TEXT("RetryAsText"), TEXT("text")) });
		TestTrue(TEXT("same-name text retry succeeds"), Retry.IsValid() && Retry->GetBoolField(TEXT("success")));
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
		TestTrue(TEXT("retry is actually FText"), Reflected(BP, TEXT("RetryAsText"), UEdGraphSchema_K2::PC_Text));
	}
	return true;
}
