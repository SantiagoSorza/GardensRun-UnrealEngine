// Copyright 2026 Nwiro. All Rights Reserved.
// Public function signature proofs. Included after the variable proof support.
// Fixtures are owned GUID packages. No SaveConfig, package save, disk deletion,
// global transaction reset or garbage collection is performed by this proof.
#include "BlueprintEditorSettings.h"
#include "GameFramework/SaveGame.h"
#include "UObject/Interface.h"

namespace NwiroFunctionProof
{
	using NwiroVariableProof::Serialize;

	struct FNeverSaveOnCompile
	{
		UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
		TEnumAsByte<ESaveOnCompile> Previous = Settings->SaveOnCompile;
		FNeverSaveOnCompile() { Settings->SaveOnCompile = SoC_Never; }
		~FNeverSaveOnCompile() { Settings->SaveOnCompile = Previous; }
	};

	struct FFixture
	{
		TStrongObjectPtr<UBlueprint> BP;
		UPackage* Package = nullptr;
		FString FileName;

		explicit FFixture(int32 Kind, UClass* Parent = nullptr)
		{
			const FString Name = TEXT("BP_FunctionProof_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Path = TEXT("/Game/NwiroFunctionAcceptance_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/") + Name;
			Package = CreatePackage(*Path);
			FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
			if (!Parent) Parent = Kind == 2 ? UInterface::StaticClass() : Kind == 1 ? USaveGame::StaticClass() : AActor::StaticClass();
			BP.Reset(FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(*Name),
				Kind == 2 ? BPTYPE_Interface : BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), TEXT("NwiroFunctionAcceptance")));
			if (!BP.IsValid()) return;
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(BP.Get(), TEXT("ExistingFunction"),
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP.Get(), Graph, true, nullptr);
			if (Kind != 2)
			{
				FEdGraphPinType Type;
				Type.PinCategory = UEdGraphSchema_K2::PC_Int;
				FBlueprintEditorUtils::AddMemberVariable(BP.Get(), TEXT("TrackInput"), Type, TEXT("73"));
			}
			FKismetEditorUtilities::CompileBlueprint(BP.Get(), EBlueprintCompileOptions::SkipSave);
			Package->SetDirtyFlag(false);
		}

		~FFixture()
		{
			if (!Package) return;
			ForEachObjectWithPackage(Package, [](UObject* Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				return true;
			}, true);
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	};

	struct FObservedCall
	{
		UBlueprint* BP;
		int32 Compiles = 0;
		int32 Saves = 0;
		FDelegateHandle CompileHandle;
		FDelegateHandle SaveHandle;
		explicit FObservedCall(UBlueprint* InBP) : BP(InBP)
		{
			CompileHandle = BP->OnCompiled().AddLambda([this](UBlueprint*) { ++Compiles; });
			SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([this](UObject* Object, FObjectPreSaveContext)
			{ if (Object && Object->GetOutermost() == BP->GetOutermost()) ++Saves; });
		}
		~FObservedCall()
		{
			BP->OnCompiled().Remove(CompileHandle);
			FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
		}
	};

	static TSharedPtr<FJsonValue> Function(const FString& Name)
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Name);
		return MakeShared<FJsonValueObject>(Object);
	}

	static TSharedPtr<FJsonValue> Pin(const FString& Name, const FString& Base,
		const FString& Subtype = FString(), const FString& Container = TEXT("none"), bool bNested = false)
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Name);
		const auto Type = bNested ? MakeShared<FJsonObject>() : Object;
		Type->SetStringField(bNested ? TEXT("base") : TEXT("type"), Base);
		if (!Subtype.IsEmpty()) Type->SetStringField(TEXT("subtype"), Subtype);
		Type->SetStringField(TEXT("container"), Container);
		if (bNested) Object->SetObjectField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Object);
	}

	static TSharedPtr<FJsonObject> Call(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items,
		bool bStandalone, bool bSingle = false)
	{
		const auto Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("blueprint"), BP->GetPathName());
		if (bStandalone && bSingle)
		{
			for (const auto& Field : Items[0]->AsObject()->Values) Command->SetField(Field.Key, Field.Value);
		}
		else Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), Items);
		if (!bStandalone) Command->SetBoolField(TEXT("compile"), false);
		const FString Request = Serialize(Command);
		const FString Response = bStandalone ? FNwiroBlueprintTools::CreateFunctionGraph(Request)
			: FNwiroBlueprintTools::EditBlueprint(Request);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Result);
		return Result;
	}

	static bool Succeeded(const TSharedPtr<FJsonObject>& Result)
	{
		bool Success = false;
		return Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Success) && Success;
	}

	static UEdGraph* Graph(UBlueprint* BP, FName Name)
	{
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		for (UEdGraph* Candidate : Graphs) if (Candidate && Candidate->GetFName() == Name) return Candidate;
		return nullptr;
	}

	static bool ReflectedPin(UFunction* Function, FName Name, FName Category,
		UObject* Subtype, EPinContainerType Container, bool bOutput)
	{
		FProperty* Property = Function ? FindFProperty<FProperty>(Function, Name) : nullptr;
		FEdGraphPinType Type;
		return Property && Property->HasAnyPropertyFlags(CPF_Parm)
			&& (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm)) == bOutput
			&& GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, Type)
			&& Type.PinCategory == Category && Type.PinSubCategoryObject.Get() == Subtype && Type.ContainerType == Container;
	}

	static bool RunFailedNoTouch(FAutomationTestBase& Test)
	{
		for (int32 Kind : { 0, 1, 2 }) for (bool bStandalone : { false, true })
		for (bool bDirty : { false, true }) for (int32 Shape = 0; Shape < 5; ++Shape)
		{
			// A baseline defect may mutate. Never reuse that target for another case.
			FFixture Fixture(Kind);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned function fixture"), BP)) return false;
			if (!Test.TestEqual(TEXT("initial fixture compiled"), BP->Status.GetValue(), BS_UpToDate)) return false;
			BP->Status = bDirty ? BS_Dirty : BS_UpToDate;
			Fixture.Package->SetDirtyFlag(bDirty);
			const NwiroVariableProof::FSnapshot Before(BP);
			TArray<TSharedPtr<FJsonValue>> Items = { Function(TEXT("WouldBeAdded")), Function(TEXT("ExistingFunction")) };
			if (Shape == 1) Swap(Items[0], Items[1]);
			if (Shape == 2) Items[1] = Function(TEXT("WouldBeAdded"));
			if (Shape == 3) Items[1] = Function(TEXT(""));
			if (Shape == 4)
			{
				Items[1] = Function(TEXT("InvalidTypeFunction"));
				Items[1]->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Value"), TEXT("NwiroUnknownFunctionType_Proof")) });
			}
			const FString Context = FString::Printf(TEXT("kind=%d standalone=%d dirty=%d shape=%d"), Kind, bStandalone, bDirty, Shape);
			FObservedCall Observation(BP);
			Test.TestEqual(Context + TEXT(" save-on-compile disabled during call"),
				GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), SoC_Never);
			const auto Result = Call(BP, Items, bStandalone);
			if (Result.IsValid()) Test.AddInfo(Context + TEXT(" response ") + Serialize(Result.ToSharedRef()));
			Test.TestTrue(Context + TEXT(" returns JSON"), Result.IsValid());
			Test.TestFalse(Context + TEXT(" rejects complete batch"), Succeeded(Result));
			Test.TestTrue(Context + TEXT(" complete target state unchanged"), Before.Matches(BP));
			Test.TestNull(Context + TEXT(" no valid-prefix graph"), Graph(BP, TEXT("WouldBeAdded")));
			Test.TestEqual(Context + TEXT(" rejection never compiles"), Observation.Compiles, 0);
			Test.TestEqual(Context + TEXT(" rejection never saves"), Observation.Saves, 0);
			Test.TestFalse(Context + TEXT(" no disk file"), IFileManager::Get().FileExists(*Fixture.FileName));
		}
		return true;
	}

	static bool RunSiblingContainers(FAutomationTestBase& Test)
	{
		for (int32 Kind : { 0, 1, 2 }) for (bool bStandalone : { false, true })
		{
			FFixture Fixture(Kind);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned signature target"), BP)) return false;
			TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(Fixture.Package,
				MakeUniqueObjectName(Fixture.Package, UUserDefinedStruct::StaticClass(),
					FName(*(TEXT("FunctionData_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)))), RF_Public | RF_Transactional));
			if (!Test.TestNotNull(TEXT("compiled custom struct"), Struct.Get())) return false;
			Test.TestEqual(TEXT("custom struct fixture compiled"), Struct->Status.GetValue(), UDSS_UpToDate);
			const auto Item = Function(TEXT("Get Records"));
			// Raw reflected names safely resolve on the baseline. Sibling container
			// loss then has an exact array oracle without creating an untyped struct pin.
			Item->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Record"), Struct->GetName()) });
			Item->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Records"), Struct->GetName(), FString(), TEXT("array")) });
			FObservedCall Observation(BP);
			const auto Result = Call(BP, { Item }, bStandalone, true);
			if (Result.IsValid()) Test.AddInfo(TEXT("typed signature response ") + Serialize(Result.ToSharedRef()));
			Test.TestTrue(TEXT("public typed function succeeds"), Succeeded(Result));
			FCompilerResultsLog Compiler;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Compiler);
			Test.TestEqual(TEXT("typed function compiles"), Compiler.NumErrors, 0);
			Test.TestEqual(TEXT("typed function compiled status"), BP->Status.GetValue(), BS_UpToDate);
			if (!Test.TestNotNull(TEXT("generated class"), BP->GeneratedClass.Get())) return false;
			UFunction* Reflected = BP->GeneratedClass->FindFunctionByName(TEXT("Get Records"));
			Test.TestNotNull(TEXT("exact spaced function reflected"), Reflected);
			Test.TestTrue(TEXT("exact custom struct input"), ReflectedPin(Reflected, TEXT("Record"),
				UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::None, false));
			Test.TestTrue(TEXT("exact custom struct array output"), ReflectedPin(Reflected, TEXT("Records"),
				UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::Array, true));
			Test.TestEqual(TEXT("signature creation never saves"), Observation.Saves, 0);
			Test.TestFalse(TEXT("signature package absent on disk"), IFileManager::Get().FileExists(*Fixture.FileName));
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionFailedNoTouchProof,
	"Nwiro.Pro.Blueprint.Functions.Production.FailedNoTouch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroFunctionFailedNoTouchProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{
		NwiroFunctionProof::FNeverSaveOnCompile Guard;
		Completed = NwiroFunctionProof::RunFailedNoTouch(*this);
	}
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionSiblingContainersProof,
	"Nwiro.Pro.Blueprint.Functions.Production.SiblingContainers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroFunctionSiblingContainersProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{
		NwiroFunctionProof::FNeverSaveOnCompile Guard;
		Completed = NwiroFunctionProof::RunSiblingContainers(*this);
	}
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}
