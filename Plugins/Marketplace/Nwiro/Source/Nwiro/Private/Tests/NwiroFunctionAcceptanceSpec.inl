// Copyright 2026 Nwiro. All Rights Reserved.
// Full public acceptance after the safe baseline RED. Initial RED proofs stay unchanged.
// Included after FunctionProofSpec.inl. All fixtures remain in memory and unsaved.
#include "K2Node_CallFunction.h"
#include "Engine/SimpleConstructionScript.h"
#include "Curves/CurveFloat.h"

namespace NwiroFunctionAcceptance
{
	using namespace NwiroFunctionProof;

	static TSharedPtr<FJsonObject> Invoke(const TSharedRef<FJsonObject>& Command, bool bStandalone)
	{
		TSharedPtr<FJsonObject> Result;
		const FString Response = bStandalone ? FNwiroBlueprintTools::CreateFunctionGraph(Serialize(Command))
			: FNwiroBlueprintTools::EditBlueprint(Serialize(Command));
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Result);
		return Result;
	}

	static TSharedRef<FJsonObject> EditCommand(UBlueprint* BP, const FString& GraphName = FString())
	{
		const auto Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Command->SetBoolField(TEXT("compile"), false);
		if (!GraphName.IsEmpty()) Command->SetStringField(TEXT("graph"), GraphName);
		return Command;
	}

	static bool Compile(FAutomationTestBase& Test, UBlueprint* BP, const FString& Context)
	{
		FCompilerResultsLog Compiler;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Compiler);
		const bool bClean = Test.TestEqual(Context + TEXT(" compiler errors"), Compiler.NumErrors, 0);
		return Test.TestEqual(Context + TEXT(" compiled status"), BP->Status.GetValue(), BS_UpToDate) && bClean;
	}

	static bool NoTouch(FAutomationTestBase& Test, UBlueprint* BP,
		const TSharedRef<FJsonObject>& Command, bool bStandalone, const FString& Context)
	{
		const NwiroVariableProof::FSnapshot Before(BP);
		USimpleConstructionScript* Construction = BP->SimpleConstructionScript;
		const TArray<USCS_Node*> ComponentNodes = Construction ? Construction->GetAllNodes() : TArray<USCS_Node*>();
		FObservedCall Observation(BP);
		const auto Result = Invoke(Command, bStandalone);
		bool Success = true;
		const bool bRejected = Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Success) && !Success;
		Test.TestTrue(Context + TEXT(" explicit rejection"), bRejected);
		const bool bComponentsUnchanged = Construction == BP->SimpleConstructionScript
			&& (!Construction || Construction->GetAllNodes() == ComponentNodes);
		Test.TestTrue(Context + TEXT(" component construction state unchanged"), bComponentsUnchanged);
		const bool bUnchanged = Before.Matches(BP) && bComponentsUnchanged;
		Test.TestTrue(Context + TEXT(" complete state unchanged"), bUnchanged);
		Test.TestEqual(Context + TEXT(" no compile callback"), Observation.Compiles, 0);
		Test.TestEqual(Context + TEXT(" no package save callback"), Observation.Saves, 0);
		const FString FileName = FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		Test.TestFalse(Context + TEXT(" no package file"), IFileManager::Get().FileExists(*FileName));
		if (!bRejected && Result.IsValid()) Test.AddInfo(Context + TEXT(" response ") + Serialize(Result.ToSharedRef()));
		return bRejected && bUnchanged && Observation.Compiles == 0 && Observation.Saves == 0;
	}

	static bool RunExtendedPreflight(FAutomationTestBase& Test)
	{
		TArray<TSharedPtr<FJsonValue>> InvalidPins = {
			Pin(TEXT("Data"), TEXT("struct")),
			Pin(TEXT("Data"), TEXT("struct"), TEXT("/Script/Engine.Actor")),
			Pin(TEXT("Data"), TEXT("class"), TEXT("/Script/CoreUObject.Vector")),
			Pin(TEXT("Data"), TEXT("object")),
			Pin(TEXT("Data"), TEXT("enum")),
			Pin(TEXT("Data"), TEXT("int"), FString(), TEXT("map")),
			Pin(TEXT("Data"), TEXT("bool"), FString(), TEXT("set")),
			Pin(TEXT("Data"), TEXT("int"), FString(), TEXT("wrong")),
			Pin(TEXT("Data"), TEXT("int"), TEXT("Actor")),
			Pin(TEXT("then"), TEXT("int")),
			Pin(TEXT("Data\nBreak"), TEXT("int")),
			MakeShared<FJsonValueBoolean>(true)
		};
		const auto Conflicting = Pin(TEXT("Data"), TEXT("int"), FString(), TEXT("array"), true);
		Conflicting->AsObject()->SetStringField(TEXT("container"), TEXT("array")); InvalidPins.Add(Conflicting);
		const auto WrongType = Pin(TEXT("Data"), TEXT("int"));
		WrongType->AsObject()->SetBoolField(TEXT("type"), true); InvalidPins.Add(WrongType);
		const auto UnknownOption = Pin(TEXT("Data"), TEXT("int"));
		UnknownOption->AsObject()->SetBoolField(TEXT("optionalTypo"), true); InvalidPins.Add(UnknownOption);

		for (bool bStandalone : { false, true }) for (bool bDirty : { false, true })
		{
			FFixture Fixture(0);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("extended preflight Actor"), BP)) return false;
			BP->Status = bDirty ? BS_Dirty : BS_UpToDate;
			Fixture.Package->SetDirtyFlag(bDirty);
			for (int32 Index = 0; Index < InvalidPins.Num(); ++Index) for (bool bReverse : { false, true })
			{
				const auto InvalidFunction = Function(TEXT("RejectedFunction"));
				InvalidFunction->AsObject()->SetArrayField(TEXT("inputs"), { InvalidPins[Index] });
				TArray<TSharedPtr<FJsonValue>> Items = { Function(TEXT("WouldBeAdded")), InvalidFunction };
				if (bReverse) Swap(Items[0], Items[1]);
				const auto Command = EditCommand(BP);
				Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), Items);
				// Omitted compile is the normal edit route; invalid functions must stop
				// earlier variable/component operations as well as the outer compiler.
				Command->RemoveField(TEXT("compile"));
				if (!bStandalone)
				{
					Command->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("OtherVariable"), TEXT("int")) });
					const auto Component = MakeShared<FJsonObject>();
					Component->SetStringField(TEXT("name"), TEXT("OtherComponent"));
					Component->SetStringField(TEXT("class"), TEXT("SceneComponent"));
					Command->SetArrayField(TEXT("add_components"), { MakeShared<FJsonValueObject>(Component) });
				}
				const FString Context = FString::Printf(TEXT("extended standalone=%d dirty=%d pin=%d reverse=%d"), bStandalone, bDirty, Index, bReverse);
				if (!NoTouch(Test, BP, Command, bStandalone, Context)) return false;
			}
			for (int32 Shape = 0; Shape < 5; ++Shape)
			{
				const auto Item = Function(TEXT("RejectedFunction"));
				if (Shape == 0) Item->AsObject()->SetStringField(TEXT("pure"), TEXT("true"));
				if (Shape == 1) Item->AsObject()->SetStringField(TEXT("override"), TEXT("true"));
				if (Shape == 2) Item->AsObject()->SetStringField(TEXT("inputs"), TEXT("[]"));
				if (Shape == 3) Item->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Duplicate"), TEXT("int")), Pin(TEXT("duplicate"), TEXT("float")) });
				if (Shape == 4)
				{
					Item->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Same"), TEXT("int")) });
					Item->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Same"), TEXT("int")) });
				}
				const auto Command = EditCommand(BP);
				Command->RemoveField(TEXT("compile"));
				Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Function(TEXT("WouldBeAdded")), Item });
				if (!NoTouch(Test, BP, Command, bStandalone, FString::Printf(TEXT("strict function option shape=%d"), Shape))) return false;
			}
		}

		// Invalid shape must reject before legacy implicit target creation/load.
		for (bool bStandalone : { false, true })
		{
			const FString Name = TEXT("BP_MissingFunction_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString PackagePath = TEXT("/Game/NwiroFunctionAcceptance_") + Name + TEXT("/") + Name;
			const auto Command = MakeShared<FJsonObject>();
			Command->SetStringField(TEXT("blueprint"), PackagePath + TEXT(".") + Name);
			const auto Item = Function(TEXT("WouldBeAdded"));
			Item->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Data"), TEXT("struct")) });
			Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Item });
			Test.TestNull(TEXT("missing target package starts absent"), FindPackage(nullptr, *PackagePath));
			const auto Result = Invoke(Command, bStandalone);
			Test.TestTrue(TEXT("pre-target malformed subtype returns JSON"), Result.IsValid());
			Test.TestFalse(TEXT("pre-target malformed subtype rejects"), Succeeded(Result));
			Test.TestNull(TEXT("rejection did not create target package"), FindPackage(nullptr, *PackagePath));
			const FString FileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
			Test.TestFalse(TEXT("pre-target failure did not save"), IFileManager::Get().FileExists(*FileName));
		}

		FFixture Mixed(0);
		UBlueprint* BP = Mixed.BP.Get();
		if (!Test.TestNotNull(TEXT("mixed operation target"), BP)) return false;
		const auto Collision = EditCommand(BP);
		Collision->SetArrayField(TEXT("add_functions"), { Function(TEXT("SharedName")) });
		Collision->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("SharedName"), TEXT("int")) });
		if (!NoTouch(Test, BP, Collision, false, TEXT("cross-operation same-name collision"))) return false;
		const auto Independent = EditCommand(BP);
		Independent->SetArrayField(TEXT("add_functions"), { Function(TEXT("IndependentFunction")) });
		Independent->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("IndependentVariable"), TEXT("int")) });
		FObservedCall Observation(BP);
		Test.TestTrue(TEXT("independent mixed variable/function succeeds"), Succeeded(Invoke(Independent, false)));
		Test.TestNotNull(TEXT("mixed function graph exists"), Graph(BP, TEXT("IndependentFunction")));
		Test.TestTrue(TEXT("mixed variable declaration exists"), FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("IndependentVariable")) != INDEX_NONE);
		Compile(Test, BP, TEXT("mixed independent result"));
		Test.TestEqual(TEXT("mixed positive never saves"), Observation.Saves, 0);
		Test.TestFalse(TEXT("mixed positive stays off disk"), IFileManager::Get().FileExists(*Mixed.FileName));
		return true;
	}

	static FString PublicNodeGuid(UBlueprint* BP, const FString& GraphName, const FString& ClassName)
	{
		const auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Request->SetStringField(TEXT("graph"), GraphName);
		TSharedPtr<FJsonObject> Response;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroBlueprintTools::ReadBlueprint(Serialize(Request))), Response);
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("nodes"), Nodes)) return FString();
		for (const auto& Node : *Nodes)
		{
			const auto Object = Node->AsObject();
			if (Object.IsValid() && Object->GetStringField(TEXT("class")) == ClassName) return Object->GetStringField(TEXT("guid"));
		}
		return FString();
	}

	static bool BuildCaller(FAutomationTestBase& Test, UBlueprint* Provider,
		const TArray<TSharedPtr<FJsonValue>>& Inputs, const TArray<TSharedPtr<FJsonValue>>& Outputs,
		UScriptStruct* Struct)
	{
		FFixture Caller(0);
		UBlueprint* BP = Caller.BP.Get();
		if (!Test.TestNotNull(TEXT("owned caller target"), BP)) return false;
		FObservedCall Observation(BP);
		const auto Signature = Function(TEXT("Consume Records"));
		TArray<TSharedPtr<FJsonValue>> CallerInputs = Inputs;
		CallerInputs.Insert(Pin(TEXT("Provider"), TEXT("object"), Provider->GeneratedClass->GetPathName(), TEXT("none"), true), 0);
		Signature->AsObject()->SetArrayField(TEXT("inputs"), CallerInputs);
		Signature->AsObject()->SetArrayField(TEXT("outputs"), Outputs);
		if (!Test.TestTrue(TEXT("public caller signature created"), Succeeded(Call(BP, { Signature }, false)))) return false;
		const FString Entry = PublicNodeGuid(BP, TEXT("Consume Records"), TEXT("K2Node_FunctionEntry"));
		const FString Result = PublicNodeGuid(BP, TEXT("Consume Records"), TEXT("K2Node_FunctionResult"));
		if (!Test.TestFalse(TEXT("public caller entry GUID available"), Entry.IsEmpty())
			|| !Test.TestFalse(TEXT("public caller result GUID available"), Result.IsEmpty())) return false;
		const auto CallNode = MakeShared<FJsonObject>();
		CallNode->SetStringField(TEXT("type"), TEXT("CallFunction"));
		CallNode->SetStringField(TEXT("function"), TEXT("Get Coins"));
		CallNode->SetStringField(TEXT("target"), Provider->GeneratedClass->GetPathName());
		CallNode->SetStringField(TEXT("ref"), TEXT("TypedCall"));
		const auto Command = EditCommand(BP, TEXT("Consume Records"));
		Command->SetArrayField(TEXT("add_nodes"), { MakeShared<FJsonValueObject>(CallNode) });
		TArray<TSharedPtr<FJsonValue>> Links;
		auto Link = [&](const FString& From, const FString& To)
		{
			const auto Connection = MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("from"), From); Connection->SetStringField(TEXT("to"), To);
			Links.Add(MakeShared<FJsonValueObject>(Connection));
		};
		Link(Entry + TEXT(".Provider"), TEXT("TypedCall.self"));
		Link(Entry + TEXT(".then"), TEXT("TypedCall.execute"));
		Link(TEXT("TypedCall.then"), Result + TEXT(".execute"));
		for (const auto& Input : Inputs)
		{
			const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
			Link(Entry + TEXT(".") + Name, TEXT("TypedCall.") + Name);
		}
		for (const auto& Output : Outputs)
		{
			const FString Name = Output->AsObject()->GetStringField(TEXT("name"));
			Link(TEXT("TypedCall.") + Name, Result + TEXT(".") + Name);
		}
		Command->SetArrayField(TEXT("connect_pins"), Links);
		const auto Response = Invoke(Command, false);
		if (Response.IsValid()) Test.AddInfo(TEXT("public caller wiring response ") + Serialize(Response.ToSharedRef()));
		if (!Test.TestTrue(TEXT("public caller nodes and links succeed"), Succeeded(Response))) return false;
		if (!Compile(Test, BP, TEXT("public typed caller"))) return false;
		UFunction* Consumer = BP->GeneratedClass->FindFunctionByName(TEXT("Consume Records"));
		Test.TestTrue(TEXT("compiled caller exact custom struct array output"), ReflectedPin(Consumer, TEXT("Records"),
			UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::Array, true));
		UEdGraph* CallerGraph = Graph(BP, TEXT("Consume Records"));
		UK2Node_CallFunction* ActualCall = nullptr;
		if (CallerGraph) for (UEdGraphNode* Node : CallerGraph->Nodes)
			if (UK2Node_CallFunction* Candidate = Cast<UK2Node_CallFunction>(Node)) ActualCall = Candidate;
		if (!Test.TestNotNull(TEXT("compiled graph has real call node"), ActualCall)) return false;
		// Editor node lookup uses the caller skeleton; generated-scope lookup resolves the compiled function.
		UFunction* EditorFunction = ActualCall->GetTargetFunction();
		UFunction* GeneratedFunction = ActualCall->FunctionReference.ResolveMember<UFunction>(BP->GeneratedClass.Get());
		UFunction* ExpectedGenerated = Provider->GeneratedClass->FindFunctionByName(TEXT("Get Coins"));
		UFunction* ExpectedSkeleton = Provider->SkeletonGeneratedClass
			? Provider->SkeletonGeneratedClass->FindFunctionByName(TEXT("Get Coins")) : nullptr;
		Test.AddInfo(FString::Printf(TEXT("caller function binding: editor=%s editorOwner=%s generated=%s generatedOwner=%s expectedSkeleton=%s expectedGenerated=%s referenceParent=%s"),
			*GetPathNameSafe(EditorFunction), *GetPathNameSafe(EditorFunction ? EditorFunction->GetOuter() : nullptr),
			*GetPathNameSafe(GeneratedFunction), *GetPathNameSafe(GeneratedFunction ? GeneratedFunction->GetOuter() : nullptr),
			*GetPathNameSafe(ExpectedSkeleton), *GetPathNameSafe(ExpectedGenerated), *GetPathNameSafe(ActualCall->FunctionReference.GetMemberParentClass())));
		Test.TestTrue(TEXT("editor caller resolves exact provider skeleton function and owner"),
			EditorFunction && EditorFunction == ExpectedSkeleton && EditorFunction->GetOuter() == Provider->SkeletonGeneratedClass.Get());
		Test.TestTrue(TEXT("generated caller resolves exact provider generated function and owner"),
			GeneratedFunction && GeneratedFunction == ExpectedGenerated && GeneratedFunction->GetOuter() == Provider->GeneratedClass.Get());
		Test.TestTrue(TEXT("caller external reference retains exact provider generated class"),
			!ActualCall->FunctionReference.IsSelfContext() && ActualCall->FunctionReference.GetMemberParentClass() == Provider->GeneratedClass.Get());
		for (const auto& Input : CallerInputs)
		{
			const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
			UEdGraphPin* Pin = ActualCall->FindPin(Name == TEXT("Provider") ? FName(TEXT("self")) : FName(*Name));
			Test.TestTrue(TEXT("caller input connected: ") + Name, Pin && Pin->LinkedTo.Num() == 1);
		}
		UEdGraphPin* Returned = ActualCall->FindPin(TEXT("Records"));
		Test.TestTrue(TEXT("caller output connected to result"), Returned && Returned->LinkedTo.Num() == 1);
		Test.TestEqual(TEXT("public caller never saves"), Observation.Saves, 0);
		Test.TestFalse(TEXT("public caller absent on disk"), IFileManager::Get().FileExists(*Caller.FileName));
		return true;
	}

	static bool RunReflectedSignatures(FAutomationTestBase& Test)
	{
		for (int32 Kind : { 0, 1, 2, 3 }) for (bool bNested : { false, true })
		{
			FFixture Fixture(Kind == 3 ? 0 : Kind);
			TUniquePtr<NwiroVariableProof::FFixture> Widget;
			if (Kind == 3) Widget = MakeUnique<NwiroVariableProof::FFixture>(true);
			UBlueprint* BP = Widget ? Widget->BP.Get() : Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("real Actor/SaveGame/BPI/Widget signature target"), BP)) return false;
			UPackage* Package = BP->GetOutermost();
			TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(Package,
				FName(*(TEXT("SignatureData_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), RF_Public | RF_Transactional));
			TStrongObjectPtr<UUserDefinedEnum> Enum(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package,
				FName(*(TEXT("SignatureEnum_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), RF_Public | RF_Transactional)));
			if (!Test.TestNotNull(TEXT("owned signature struct"), Struct.Get()) || !Test.TestNotNull(TEXT("owned signature enum"), Enum.Get())) return false;
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum.Get());
			const TArray<TSharedPtr<FJsonValue>> Inputs = {
				Pin(TEXT("Target Level"), TEXT("struct"), Struct->GetPathName(), TEXT("none"), bNested),
				Pin(TEXT("Record Input"), TEXT("struct"), Struct->GetPathName(), TEXT("array"), bNested),
				Pin(TEXT("Spawn Classes"), TEXT("class"), TEXT("/Script/Engine.Actor"), TEXT("array"), bNested),
				Pin(TEXT("Owners"), TEXT("object"), TEXT("/Script/Engine.Actor"), TEXT("array"), bNested),
				Pin(TEXT("Choice"), TEXT("enum"), Enum->GetPathName(), TEXT("none"), bNested),
				Pin(TEXT("Tokens"), TEXT("int"), FString(), TEXT("set"), bNested)
			};
			const TArray<TSharedPtr<FJsonValue>> Outputs = { Pin(TEXT("Records"), TEXT("struct"), Struct->GetPathName(), TEXT("array"), bNested) };
			const auto Item = Function(TEXT("Get Coins"));
			Item->AsObject()->SetArrayField(TEXT("inputs"), Inputs);
			Item->AsObject()->SetArrayField(TEXT("outputs"), Outputs);
			FObservedCall Observation(BP);
			// Both type spellings cross public routes; Widget exercises standalone's GC scope.
			const auto Result = Call(BP, { Item }, Kind == 3 || bNested, true);
			if (Result.IsValid()) Test.AddInfo(TEXT("explicit signature response ") + Serialize(Result.ToSharedRef()));
			if (!Test.TestTrue(TEXT("explicit typed signature succeeds"), Succeeded(Result))) return false;
			if (!Compile(Test, BP, TEXT("explicit provider"))) return false;
			UFunction* Function = BP->GeneratedClass->FindFunctionByName(TEXT("Get Coins"));
			if (!Test.TestNotNull(TEXT("exact spaced function identity"), Function)) return false;
			Test.TestTrue(TEXT("spaced custom struct input identity"), ReflectedPin(Function, TEXT("Target Level"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::None, false));
			Test.TestTrue(TEXT("custom struct array input identity"), ReflectedPin(Function, TEXT("Record Input"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::Array, false));
			Test.TestTrue(TEXT("class array exact Actor constraint"), ReflectedPin(Function, TEXT("Spawn Classes"), UEdGraphSchema_K2::PC_Class, AActor::StaticClass(), EPinContainerType::Array, false));
			Test.TestTrue(TEXT("object array exact Actor class"), ReflectedPin(Function, TEXT("Owners"), UEdGraphSchema_K2::PC_Object, AActor::StaticClass(), EPinContainerType::Array, false));
			Test.TestTrue(TEXT("exact user enum identity"), ReflectedPin(Function, TEXT("Choice"), UEdGraphSchema_K2::PC_Byte, Enum.Get(), EPinContainerType::None, false));
			Test.TestTrue(TEXT("hashable integer set input identity"), ReflectedPin(Function, TEXT("Tokens"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::Set, false));
			Test.TestTrue(TEXT("custom struct array output identity"), ReflectedPin(Function, TEXT("Records"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::Array, true));
			TArray<FName> ExpectedOrder;
			for (const auto& Parameter : Inputs) ExpectedOrder.Add(FName(*Parameter->AsObject()->GetStringField(TEXT("name"))));
			for (const auto& Parameter : Outputs) ExpectedOrder.Add(FName(*Parameter->AsObject()->GetStringField(TEXT("name"))));
			for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
			{
				UFunction* Declared = Class ? Class->FindFunctionByName(TEXT("Get Coins")) : nullptr;
				if (!Test.TestTrue(TEXT("signature belongs to exact generated/skeleton class"), Declared && Declared->GetOuter() == Class)) return false;
				TArray<FName> ActualOrder;
				for (TFieldIterator<FProperty> It(Declared); It; ++It)
					if (It->HasAnyPropertyFlags(CPF_Parm)) ActualOrder.Add(It->GetFName());
				Test.TestTrue(TEXT("ordered signature parameter names preserved"), ActualOrder == ExpectedOrder);
			}
			FArrayProperty* Classes = FindFProperty<FArrayProperty>(Function, TEXT("Spawn Classes"));
			FClassProperty* ClassElement = Classes ? CastField<FClassProperty>(Classes->Inner) : nullptr;
			Test.TestTrue(TEXT("reflected class MetaClass stays Actor"), ClassElement && ClassElement->MetaClass == AActor::StaticClass());
			FArrayProperty* Objects = FindFProperty<FArrayProperty>(Function, TEXT("Owners"));
			FObjectPropertyBase* ObjectElement = Objects ? CastField<FObjectPropertyBase>(Objects->Inner) : nullptr;
			Test.TestTrue(TEXT("reflected object PropertyClass stays Actor"), ObjectElement && ObjectElement->PropertyClass == AActor::StaticClass());
			UEdGraph* SignatureGraph = Graph(BP, TEXT("Get Coins"));
			if (!Test.TestNotNull(TEXT("signature editor graph"), SignatureGraph)) return false;
			for (UEdGraphNode* Node : SignatureGraph->Nodes)
			{
				UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node);
				if (!Editable) continue;
				const EEdGraphPinDirection Direction = Cast<UK2Node_FunctionEntry>(Node) ? EGPD_Output : EGPD_Input;
				for (const TSharedPtr<FUserPinInfo>& Definition : Editable->UserDefinedPins)
				{
					if (!Test.TestTrue(TEXT("signature user pin metadata exists"), Definition.IsValid())) return false;
					Test.TestEqual(TEXT("stored signature pin direction"), Definition->DesiredPinDirection.GetValue(), Direction);
					UEdGraphPin* Actual = Editable->FindPin(Definition->PinName, Direction);
					Test.TestTrue(TEXT("actual signature pin matches stored type/direction"), Actual && Actual->PinType == Definition->PinType);
				}
			}
			TUniquePtr<FFixture> Implementation;
			TUniquePtr<FObservedCall> ImplementationObservation;
			UBlueprint* Provider = BP;
			if (Kind == 2)
			{
				Implementation = MakeUnique<FFixture>(0);
				Provider = Implementation->BP.Get();
				if (!Test.TestNotNull(TEXT("interface implementer"), Provider)) return false;
				ImplementationObservation = MakeUnique<FObservedCall>(Provider);
				UEdGraph* Seed = Graph(Provider, TEXT("ExistingFunction"));
				if (!Test.TestNotNull(TEXT("owned implementer seed graph"), Seed)) return false;
				FBlueprintEditorUtils::RemoveGraph(Provider, Seed, EGraphRemoveFlags::MarkTransient);
				if (!Compile(Test, Provider, TEXT("remove owned implementer seed"))) return false;
				Test.TestNull(TEXT("implementer seed removed before interface attachment"), Graph(Provider, TEXT("ExistingFunction")));
				const auto Attach = MakeShared<FJsonObject>();
				Attach->SetStringField(TEXT("blueprint"), Provider->GetPathName());
				Attach->SetStringField(TEXT("interface"), BP->GeneratedClass->GetPathName());
				TSharedPtr<FJsonObject> Attached;
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroBlueprintTools::AddInterface(Serialize(Attach))), Attached);
				if (!Test.TestTrue(TEXT("public BPI attachment succeeds"), Succeeded(Attached))) return false;
				if (!Compile(Test, Provider, TEXT("interface implementation"))) return false;
				Test.TestTrue(TEXT("generated implementation owns BPI"), Provider->GeneratedClass->ImplementsInterface(BP->GeneratedClass));
			}
			if (!BuildCaller(Test, Provider, Inputs, Outputs, Struct.Get())) return false;
			if (Implementation)
			{
				Test.TestEqual(TEXT("interface implementation never saves"), ImplementationObservation->Saves, 0);
				Test.TestFalse(TEXT("interface implementation absent on disk"), IFileManager::Get().FileExists(*Implementation->FileName));
			}
			Test.TestEqual(TEXT("provider creation never saves"), Observation.Saves, 0);
			const FString FileName = Widget ? Widget->FileName : Fixture.FileName;
			Test.TestFalse(TEXT("provider absent on disk"), IFileManager::Get().FileExists(*FileName));
		}
		return true;
	}

	static bool RunInterfaceOverride(FAutomationTestBase& Test)
	{
		FFixture Unrelated(2), Interface(2), Implementation(0);
		if (!Test.TestNotNull(TEXT("unrelated interface fixture"), Unrelated.BP.Get())
			|| !Test.TestNotNull(TEXT("selected interface fixture"), Interface.BP.Get())
			|| !Test.TestNotNull(TEXT("interface override target"), Implementation.BP.Get())) return false;
		// Remove only our fixture seed declarations, avoiding a shared unrelated
		// function name in two interfaces. Engine removal releases the graph name.
		for (UBlueprint* BP : { Unrelated.BP.Get(), Interface.BP.Get() })
		{
			UEdGraph* Seed = Graph(BP, TEXT("ExistingFunction"));
			if (!Test.TestNotNull(TEXT("owned interface seed graph"), Seed)) return false;
			FBlueprintEditorUtils::RemoveGraph(BP, Seed, EGraphRemoveFlags::MarkTransient);
			if (!Compile(Test, BP, TEXT("interface fixture seed removal"))) return false;
		}
		TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(Interface.Package,
			FName(*(TEXT("OverrideData_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), RF_Public | RF_Transactional));
		if (!Test.TestNotNull(TEXT("interface override custom struct"), Struct.Get())) return false;
		const auto Input = Pin(TEXT("Target Level"), TEXT("struct"), Struct->GetPathName(), TEXT("none"), true);
		const auto Output = Pin(TEXT("Records"), TEXT("struct"), Struct->GetPathName(), TEXT("array"), true);
		const auto Declaration = Function(TEXT("Get Coins"));
		Declaration->AsObject()->SetArrayField(TEXT("inputs"), { Input });
		Declaration->AsObject()->SetArrayField(TEXT("outputs"), { Output });
		if (!Test.TestTrue(TEXT("public interface signature declaration"), Succeeded(Call(Interface.BP.Get(), { Declaration }, true)))) return false;
		UBlueprint* Target = Implementation.BP.Get();
		FObservedCall TargetObservation(Target), UnrelatedObservation(Unrelated.BP.Get()), InterfaceObservation(Interface.BP.Get());
		for (UBlueprint* Dependency : { Unrelated.BP.Get(), Interface.BP.Get() })
		{
			const auto Attach = MakeShared<FJsonObject>();
			Attach->SetStringField(TEXT("blueprint"), Target->GetPathName());
			Attach->SetStringField(TEXT("interface"), Dependency->GeneratedClass->GetPathName());
			TSharedPtr<FJsonObject> Result;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroBlueprintTools::AddInterface(Serialize(Attach))), Result);
			if (!Test.TestTrue(TEXT("public ordered interface attachment"), Succeeded(Result))) return false;
		}
		if (!Test.TestTrue(TEXT("unrelated interface is searched before selected interface"), Target->ImplementedInterfaces.Num() == 2
			&& Target->ImplementedInterfaces[0].Interface == Unrelated.BP->GeneratedClass
			&& Target->ImplementedInterfaces[1].Interface == Interface.BP->GeneratedClass)) return false;
		UEdGraph* AutoGraph = Graph(Target, TEXT("Get Coins"));
		if (!Test.TestNotNull(TEXT("engine-created interface graph"), AutoGraph)) return false;
		FBlueprintEditorUtils::RemoveGraph(Target, AutoGraph, EGraphRemoveFlags::MarkTransient);
		Test.TestNull(TEXT("public override must create a new graph"), Graph(Target, TEXT("Get Coins")));
		Unrelated.BP->Status = BS_Dirty;
		Unrelated.Package->SetDirtyFlag(true);
		const NwiroVariableProof::FSnapshot UnrelatedBefore(Unrelated.BP.Get());
		const int32 UnrelatedCompilesBefore = UnrelatedObservation.Compiles;
		const int32 SelectedCompilesBefore = InterfaceObservation.Compiles;
		const auto Mismatch = Function(TEXT("Get Coins"));
		Mismatch->AsObject()->SetBoolField(TEXT("override"), true);
		Mismatch->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Target Level"), TEXT("int")) });
		const auto BadCommand = EditCommand(Target);
		BadCommand->SetArrayField(TEXT("add_functions"), { Mismatch });
		if (!NoTouch(Test, Target, BadCommand, false, TEXT("inherited input type mismatch"))) return false;
		Declaration->AsObject()->SetBoolField(TEXT("override"), true);
		if (!Test.TestTrue(TEXT("matching compiled BPI override passes unrelated dirty dependency"),
			Succeeded(Call(Target, { Declaration }, false)))) return false;
		UEdGraph* Created = Graph(Target, TEXT("Get Coins"));
		Test.TestTrue(TEXT("public BPI override creates a different graph"), Created && Created != AutoGraph);
		Test.TestTrue(TEXT("BPI override registered in exact interface slot"), Created && Target->ImplementedInterfaces[1].Graphs.Contains(Created)
			&& !Target->FunctionGraphs.Contains(Created));
		Test.TestTrue(TEXT("unrelated dirty interface untouched by public validation/apply"), UnrelatedBefore.Matches(Unrelated.BP.Get()));
		Test.TestEqual(TEXT("unrelated interface not compiled by override"), UnrelatedObservation.Compiles, UnrelatedCompilesBefore);
		Test.TestEqual(TEXT("selected interface not compiled by override"), InterfaceObservation.Compiles, SelectedCompilesBefore);
		const auto Ignored = Function(TEXT("Get Coins"));
		Ignored->AsObject()->SetBoolField(TEXT("override"), true);
		Ignored->AsObject()->SetBoolField(TEXT("pure"), true);
		Ignored->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Ignored Value"), TEXT("bool")) });
		for (bool bStandalone : { false, true })
		{
			const NwiroVariableProof::FSnapshot Before(Target);
			const int32 CompilesBefore = TargetObservation.Compiles;
			Test.TestTrue(TEXT("existing BPI override ignores syntactically valid metadata"), Succeeded(Call(Target, { Ignored }, bStandalone, true)));
			Test.TestTrue(TEXT("ignored metadata preserves existing BPI state"), Before.Matches(Target));
			Test.TestEqual(TEXT("ignored metadata no-op does not compile"), TargetObservation.Compiles, CompilesBefore);
		}
		// Final compilation has explicit prerequisites; it is outside the no-touch
		// observations above and does not conceal a compile during public preflight.
		if (!Compile(Test, Unrelated.BP.Get(), TEXT("explicit unrelated dependency"))
			|| !Compile(Test, Target, TEXT("public recreated BPI override"))) return false;
		for (UClass* Class : { Target->GeneratedClass.Get(), Target->SkeletonGeneratedClass.Get() })
		{
			UFunction* Reflected = Class ? Class->FindFunctionByName(TEXT("Get Coins")) : nullptr;
			Test.TestTrue(TEXT("recreated override declared directly on exact class"), Reflected && Reflected->GetOuter() == Class);
			Test.TestTrue(TEXT("recreated BPI exact struct input"), ReflectedPin(Reflected, TEXT("Target Level"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::None, false));
			Test.TestTrue(TEXT("recreated BPI exact array output"), ReflectedPin(Reflected, TEXT("Records"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::Array, true));
		}
		Test.TestEqual(TEXT("BPI target never saves"), TargetObservation.Saves, 0);
		Test.TestEqual(TEXT("unrelated BPI never saves"), UnrelatedObservation.Saves, 0);
		Test.TestEqual(TEXT("selected BPI never saves"), InterfaceObservation.Saves, 0);
		for (const FString& FileName : { Unrelated.FileName, Interface.FileName, Implementation.FileName })
			Test.TestFalse(TEXT("BPI override fixtures absent on disk"), IFileManager::Get().FileExists(*FileName));
		return true;
	}

	static bool RunOverrides(FAutomationTestBase& Test)
	{
		for (bool bStandalone : { false, true })
		{
			FFixture Parent(0);
			UBlueprint* ParentBP = Parent.BP.Get();
			if (!Test.TestNotNull(TEXT("owned override parent"), ParentBP)) return false;
			UFunction* NativeCallable = AActor::StaticClass()->FindFunctionByName(TEXT("K2_GetActorLocation"));
			if (!Test.TestNotNull(TEXT("real native callable exists"), NativeCallable)) return false;
			Test.TestTrue(TEXT("native control is Blueprint callable"), NativeCallable->HasAnyFunctionFlags(FUNC_BlueprintCallable));
			Test.TestFalse(TEXT("native control cannot be overridden"), UEdGraphSchema_K2::CanKismetOverrideFunction(NativeCallable));
			const auto NativeOverride = Function(TEXT("K2_GetActorLocation"));
			NativeOverride->AsObject()->SetBoolField(TEXT("override"), true);
			const auto NativeCommand = EditCommand(ParentBP);
			NativeCommand->RemoveField(TEXT("compile"));
			NativeCommand->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Function(TEXT("WouldBeAdded")), NativeOverride });
			if (!NoTouch(Test, ParentBP, NativeCommand, bStandalone, TEXT("native non-overridable callable"))) return false;
			if (!Test.TestTrue(TEXT("second inherited function created"), Succeeded(Call(ParentBP, { Function(TEXT("OtherInheritedFunction")) }, true)))) return false;
			const FString Name = TEXT("BP_OverrideChild_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			TStrongObjectPtr<UBlueprint> Child(FKismetEditorUtilities::CreateBlueprint(ParentBP->GeneratedClass,
				Parent.Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("NwiroFunctionAcceptance")));
			if (!Test.TestNotNull(TEXT("owned inherited child"), Child.Get())) return false;
			if (!Compile(Test, Child.Get(), TEXT("initial override child"))) return false;
			const auto Override = Function(TEXT("ExistingFunction"));
			Override->AsObject()->SetBoolField(TEXT("override"), true);
			FObservedCall ParentObservation(ParentBP);
			FObservedCall ChildObservation(Child.Get());
			if (!Test.TestTrue(TEXT("valid compiled-parent override succeeds"), Succeeded(Call(Child.Get(), { Override }, bStandalone, true)))) return false;
			if (!Compile(Test, Child.Get(), TEXT("inherited override"))) return false;
			UEdGraph* Existing = Graph(Child.Get(), TEXT("ExistingFunction"));
			if (!Test.TestNotNull(TEXT("override graph exists"), Existing)) return false;
			const NwiroVariableProof::FSnapshot BeforeNoOp(Child.Get());
			const int32 CompilesBeforeNoOp = ChildObservation.Compiles;
			Test.TestTrue(TEXT("existing override remains successful no-op"), Succeeded(Call(Child.Get(), { Override }, bStandalone, true)));
			Test.TestTrue(TEXT("existing override preserves complete target state"), BeforeNoOp.Matches(Child.Get()));
			Test.TestTrue(TEXT("existing override preserves graph identity"), Graph(Child.Get(), TEXT("ExistingFunction")) == Existing);
			Test.TestEqual(TEXT("existing override does not compile"), ChildObservation.Compiles, CompilesBeforeNoOp);
			if (!bStandalone)
			{
				for (int32 Mode = 0; Mode < 3; ++Mode)
				{
					Child->Status = BS_Dirty;
					Parent.Package->SetDirtyFlag(true);
					const NwiroVariableProof::FSnapshot Before(Child.Get());
					const int32 CompilesBefore = ChildObservation.Compiles;
					const auto NoOp = EditCommand(Child.Get());
					NoOp->SetArrayField(TEXT("add_functions"), { Override });
					if (Mode == 0) NoOp->RemoveField(TEXT("compile"));
					if (Mode == 2) NoOp->SetBoolField(TEXT("compile"), true);
					Test.TestTrue(TEXT("edit override no-op command succeeds"), Succeeded(Invoke(NoOp, false)));
					if (Mode < 2)
					{
						Test.TestTrue(TEXT("omitted/false compile preserves dirty no-op state"), Before.Matches(Child.Get()));
						Test.TestEqual(TEXT("omitted/false compile does not compile no-op"), ChildObservation.Compiles, CompilesBefore);
					}
					else
					{
						Test.TestTrue(TEXT("explicit compile true still compiles no-op"), ChildObservation.Compiles > CompilesBefore);
						Test.TestEqual(TEXT("explicitly compiled no-op status"), Child->Status.GetValue(), BS_UpToDate);
					}
					Test.TestTrue(TEXT("all no-op modes preserve graph identity"), Graph(Child.Get(), TEXT("ExistingFunction")) == Existing);
				}
			}
			Child->Status = BS_Dirty;
			Parent.Package->SetDirtyFlag(true);
			const NwiroVariableProof::FSnapshot BeforeEmpty(Child.Get());
			const int32 CompilesBeforeEmpty = ChildObservation.Compiles;
			const auto Empty = EditCommand(Child.Get());
			Empty->RemoveField(TEXT("compile"));
			Empty->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), TArray<TSharedPtr<FJsonValue>>{});
			Test.TestTrue(TEXT("empty function batch succeeds as no-op"), Succeeded(Invoke(Empty, bStandalone)));
			Test.TestTrue(TEXT("empty batch preserves dirty target state"), BeforeEmpty.Matches(Child.Get()));
			Test.TestEqual(TEXT("empty batch does not compile"), ChildObservation.Compiles, CompilesBeforeEmpty);
			ParentBP->Status = BS_Dirty;
			Parent.Package->SetDirtyFlag(true);
			const NwiroVariableProof::FSnapshot ParentBefore(ParentBP);
			const int32 ParentCompilesBefore = ParentObservation.Compiles;
			const auto Pending = Function(TEXT("OtherInheritedFunction"));
			Pending->AsObject()->SetBoolField(TEXT("override"), true);
			const auto Command = EditCommand(Child.Get());
			Command->RemoveField(TEXT("compile"));
			Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Function(TEXT("WouldBeAdded")), Pending });
			if (!NoTouch(Test, Child.Get(), Command, bStandalone, TEXT("dirty dependency override"))) return false;
			Test.TestTrue(TEXT("dirty dependency state preserved"), ParentBefore.Matches(ParentBP));
			Test.TestEqual(TEXT("dirty dependency never compiled"), ParentObservation.Compiles, ParentCompilesBefore);
			Test.TestEqual(TEXT("override parent never saved"), ParentObservation.Saves, 0);
			Test.TestEqual(TEXT("override child never saved"), ChildObservation.Saves, 0);
			Test.TestFalse(TEXT("override package stays off disk"), IFileManager::Get().FileExists(*Parent.FileName));
		}
		return RunInterfaceOverride(Test);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionExtendedPreflightProof,
	"Nwiro.Pro.Blueprint.Functions.Production.ExtendedPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionExtendedPreflightProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroFunctionAcceptance::RunExtendedPreflight(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionReflectedSignaturesProof,
	"Nwiro.Pro.Blueprint.Functions.Production.ReflectedSignaturesAndCallers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionReflectedSignaturesProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroFunctionAcceptance::RunReflectedSignatures(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionOverrideProof,
	"Nwiro.Pro.Blueprint.Functions.Production.Overrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionOverrideProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroFunctionAcceptance::RunOverrides(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}


// FIX-008 / FIX-025: explicit defaults and ordinary signature replacement.
// These cases extend acceptance; the original RED proof bodies remain unchanged.
namespace NwiroFunctionEvolutionProof
{
	using namespace NwiroFunctionAcceptance;

	static TSharedPtr<FJsonValue> DefaultPin(const FString& Name, const FString& Type, const FString& Value,
		const FString& Subtype = FString())
	{
		auto Result = Pin(Name, Type, Subtype);
		Result->AsObject()->SetStringField(TEXT("default"), Value);
		return Result;
	}

	static UK2Node_FunctionEntry* Entry(UBlueprint* BP, FName Name)
	{
		UEdGraph* Found = Graph(BP, Name);
		if (Found) for (UEdGraphNode* Node : Found->Nodes)
			if (auto* Result = Cast<UK2Node_FunctionEntry>(Node)) return Result;
		return nullptr;
	}

	static UK2Node_FunctionResult* Exit(UBlueprint* BP, FName Name)
	{
		UEdGraph* Found = Graph(BP, Name);
		if (Found) for (UEdGraphNode* Node : Found->Nodes)
			if (auto* Result = Cast<UK2Node_FunctionResult>(Node)) return Result;
		return nullptr;
	}

	static TSharedPtr<FJsonValue> Link(const FString& From, const FString& To)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("from"), From);
		Result->SetStringField(TEXT("to"), To);
		return MakeShared<FJsonValueObject>(Result);
	}

	static FString ReadGraph(UBlueprint* BP, FName Name)
	{
		auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Request->SetStringField(TEXT("graph"), Name.ToString());
		return FNwiroBlueprintTools::ReadBlueprint(Serialize(Request));
	}

	static FString PinValue(const UEdGraphPin* Pin)
	{
		return !Pin ? FString() : Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text
			? Pin->DefaultTextValue.ToString() : Pin->GetDefaultAsString();
	}

	static bool RejectUnchanged(FAutomationTestBase& Test, UBlueprint* BP, FName Name,
		const TSharedRef<FJsonObject>& Command, bool bStandalone, const FString& Context)
	{
		const FString Before = ReadGraph(BP, Name);
		TArray<UEdGraphPin*> Pins;
		UEdGraph* FunctionGraph = Graph(BP, Name);
		if (FunctionGraph) for (UEdGraphNode* Node : FunctionGraph->Nodes) Pins.Append(Node->Pins);
		const bool Rejected = NoTouch(Test, BP, Command, bStandalone, Context);
		TArray<UEdGraphPin*> After;
		if (FunctionGraph) for (UEdGraphNode* Node : FunctionGraph->Nodes) After.Append(Node->Pins);
		return Test.TestTrue(Context + TEXT(" pin identities unchanged"), Pins == After)
			&& Test.TestEqual(Context + TEXT(" complete graph/pin/default/link readback unchanged"), ReadGraph(BP, Name), Before)
			&& Rejected;
	}

	static bool MakeBody(FAutomationTestBase& Test, UBlueprint* BP, FName Name, const FString& Input)
	{
		auto Command = EditCommand(BP, Name.ToString());
		const FString In = PublicNodeGuid(BP, Name.ToString(), TEXT("K2Node_FunctionEntry"));
		const FString Out = PublicNodeGuid(BP, Name.ToString(), TEXT("K2Node_FunctionResult"));
		if (!Test.TestFalse(TEXT("body entry ref"), In.IsEmpty()) || !Test.TestFalse(TEXT("body result ref"), Out.IsEmpty())) return false;
		Command->SetArrayField(TEXT("connect_pins"), {
			Link(In + TEXT(".then"), Out + TEXT(".execute")), Link(In + TEXT(".") + Input, Out + TEXT(".Result"))
		});
		return Test.TestTrue(TEXT("public provider body wiring succeeds"), Succeeded(Invoke(Command, false)))
			&& Compile(Test, BP, TEXT("provider with actual body"));
	}

	static UK2Node_CallFunction* MakeCaller(FAutomationTestBase& Test, UBlueprint* Caller, UBlueprint* Provider,
		FName FunctionName, bool bWireLinked)
	{
		auto FunctionItem = Function(TEXT("Call Provider"));
		TArray<TSharedPtr<FJsonValue>> Inputs = { Pin(TEXT("Provider"), TEXT("object"), Provider->GeneratedClass->GetPathName()) };
		if (bWireLinked) Inputs.Add(Pin(TEXT("Linked"), TEXT("int")));
		FunctionItem->AsObject()->SetArrayField(TEXT("inputs"), Inputs);
		FunctionItem->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")) });
		if (!Test.TestTrue(TEXT("public consumer signature"), Succeeded(Call(Caller, { FunctionItem }, false)))) return nullptr;
		const FString In = PublicNodeGuid(Caller, TEXT("Call Provider"), TEXT("K2Node_FunctionEntry"));
		const FString Out = PublicNodeGuid(Caller, TEXT("Call Provider"), TEXT("K2Node_FunctionResult"));
		auto Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("type"), TEXT("CallFunction"));
		Node->SetStringField(TEXT("function"), FunctionName.ToString());
		Node->SetStringField(TEXT("target"), Provider->GeneratedClass->GetPathName());
		Node->SetStringField(TEXT("ref"), TEXT("ProviderCall"));
		auto Command = EditCommand(Caller, TEXT("Call Provider"));
		Command->SetArrayField(TEXT("add_nodes"), { MakeShared<FJsonValueObject>(Node) });
		TArray<TSharedPtr<FJsonValue>> Links = {
			Link(In + TEXT(".Provider"), TEXT("ProviderCall.self")),
			Link(In + TEXT(".then"), TEXT("ProviderCall.execute")),
			Link(TEXT("ProviderCall.then"), Out + TEXT(".execute")),
			Link(TEXT("ProviderCall.Result"), Out + TEXT(".Result"))
		};
		if (bWireLinked) Links.Add(Link(In + TEXT(".Linked"), TEXT("ProviderCall.Linked")));
		Command->SetArrayField(TEXT("connect_pins"), Links);
		if (!Test.TestTrue(TEXT("public connected consumer"), Succeeded(Invoke(Command, false)))
			|| !Compile(Test, Caller, TEXT("connected consumer"))) return nullptr;
		UEdGraph* G = Graph(Caller, TEXT("Call Provider"));
		if (G) for (UEdGraphNode* Candidate : G->Nodes)
			if (auto* CallNode = Cast<UK2Node_CallFunction>(Candidate)) return CallNode;
		return nullptr;
	}

	static bool RunInputDefaults(FAutomationTestBase& Test)
	{
		for (int32 Kind : { 0, 1, 2 }) for (bool bStandalone : { false, true })
		{
			FFixture Fixture(Kind);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned default provider"), BP)) return false;
			TStrongObjectPtr<UUserDefinedEnum> Enum(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Fixture.Package,
				FName(*(TEXT("DefaultEnum_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), RF_Public | RF_Transactional)));
			if (!Test.TestNotNull(TEXT("owned enum for exact default"), Enum.Get())) return false;
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum.Get());
			const FString EnumValue = Enum->GetNameStringByIndex(0);
			FFixture ClassProvider(0);
			if (!Test.TestNotNull(TEXT("owned generated class default provider"), ClassProvider.BP.Get())) return false;
			TStrongObjectPtr<UCurveFloat> Curve(NewObject<UCurveFloat>(Fixture.Package,
				FName(*(TEXT("DefaultCurve_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), RF_Public | RF_Standalone));
			if (!Test.TestNotNull(TEXT("owned object default asset"), Curve.Get())) return false;
			const TArray<TSharedPtr<FJsonValue>> Inputs = {
				DefaultPin(TEXT("Amount"), TEXT("int"), TEXT("42")),
				DefaultPin(TEXT("Enabled"), TEXT("bool"), TEXT("true")),
				DefaultPin(TEXT("Ratio"), TEXT("float"), TEXT("1.25")),
				DefaultPin(TEXT("Precise"), TEXT("double"), TEXT("2.5")),
				DefaultPin(TEXT("Large"), TEXT("int64"), TEXT("9223372036854775807")),
				DefaultPin(TEXT("Small"), TEXT("byte"), TEXT("255")),
				DefaultPin(TEXT("Label"), TEXT("name"), TEXT("Seed")),
				DefaultPin(TEXT("Message"), TEXT("string"), TEXT("hello\nworld")),
				DefaultPin(TEXT("Title"), TEXT("text"), TEXT("plain title")),
				DefaultPin(TEXT("Offset"), TEXT("vector"), TEXT("1,2,3")),
				DefaultPin(TEXT("ActorType"), TEXT("class"), TEXT("/Script/Engine.Actor"), TEXT("Actor")),
				DefaultPin(TEXT("State"), TEXT("enum"), EnumValue, Enum->GetPathName()),
				DefaultPin(TEXT("SaveType"), TEXT("class"), USaveGame::StaticClass()->GetPathName(), TEXT("Object")),
				DefaultPin(TEXT("GeneratedType"), TEXT("class"), ClassProvider.BP->GeneratedClass->GetPathName(), TEXT("Actor")),
				DefaultPin(TEXT("Curve"), TEXT("object"), Curve->GetPathName(), TEXT("/Script/Engine.CurveFloat")),
				DefaultPin(TEXT("NullType"), TEXT("class"), TEXT("None"), TEXT("Actor")),
				DefaultPin(TEXT("NullObject"), TEXT("object"), TEXT("None"), TEXT("Object"))
			};
			auto Item = Function(TEXT("Default Function"));
			Item->AsObject()->SetArrayField(TEXT("inputs"), Inputs);
			Item->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")) });
			FObservedCall Observation(BP);
			auto Response = Call(BP, { Item }, bStandalone, true);
			const FString ResponseJson = Response.IsValid() ? Serialize(Response.ToSharedRef()) : TEXT("<invalid JSON response>");
			if (!Test.TestTrue(TEXT("public default signature succeeds; response: ") + ResponseJson, Succeeded(Response))
				|| !Compile(Test, BP, TEXT("default signature"))) return false;
			auto* FunctionEntry = Entry(BP, TEXT("Default Function"));
			if (!Test.TestNotNull(TEXT("default entry"), FunctionEntry)) return false;
			TSharedPtr<FJsonObject> Readback;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReadGraph(BP, TEXT("Default Function"))), Readback);
			TMap<FString, FString> PublicValues;
			if (Readback.IsValid()) for (const auto& Node : Readback->GetArrayField(TEXT("nodes")))
				if (Node->AsObject()->GetStringField(TEXT("class")) == TEXT("K2Node_FunctionEntry"))
					for (const auto& Value : Node->AsObject()->GetArrayField(TEXT("pins")))
						PublicValues.Add(Value->AsObject()->GetStringField(TEXT("name")), Value->AsObject()->GetStringField(TEXT("defaultValue")));
			for (const auto& Input : Inputs)
			{
				const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
				const FString DeclaredDefault = Input->AsObject()->GetStringField(TEXT("default"));
				const FString Base = Input->AsObject()->GetStringField(TEXT("type"));
				const bool bReference = Base == TEXT("class") || Base == TEXT("object");
				const bool bNullReference = bReference && (DeclaredDefault.IsEmpty() || DeclaredDefault == TEXT("None"));
				const FString Expected = bNullReference ? FString() : DeclaredDefault;
				Test.TestEqual(TEXT("entry default: ") + Name, PinValue(FunctionEntry->FindPin(FName(*Name))), Expected);
				Test.TestTrue(TEXT("public default present: ") + Name, PublicValues.Contains(Name));
				if (PublicValues.Contains(Name)) Test.TestEqual(TEXT("public default readback: ") + Name, PublicValues[Name], Expected);
				for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
				{
					UFunction* Reflected = Class->FindFunctionByName(TEXT("Default Function"), EIncludeSuperFlag::ExcludeSuper);
					if (!Test.TestNotNull(TEXT("exact owner reflected function"), Reflected)) return false;
					Test.TestTrue(TEXT("default function exact owner"), Reflected->GetOuter() == Class);
					if (!bReference) Test.TestTrue(TEXT("compiled parameter metadata exists: ") + Name, Reflected->HasMetaData(FName(*Name)));
					else
					{
						FProperty* Property = FindFProperty<FProperty>(Reflected, FName(*Name));
						if (!Test.TestNotNull(TEXT("reference default reflected parameter: ") + Name, Property)) return false;
						FString Actual;
						Test.TestEqual(TEXT("compiled reference parameter metadata presence: ") + Name,
							UEdGraphSchema_K2::FindFunctionParameterDefaultValue(Reflected, Property, Actual), !bNullReference);
						Test.TestEqual(TEXT("compiled reference default is exact: ") + Name, Actual, Expected);
						if (bNullReference) Test.TestFalse(TEXT("null reference has no stale metadata: ") + Name,
							Reflected->HasMetaData(FName(*(TEXT("CPP_Default_") + Name))));
					}
				}
			}
			if (Kind != 2)
			{
				if (!MakeBody(Test, BP, TEXT("Default Function"), TEXT("Amount"))) return false;
				FFixture Consumer(0);
				FObservedCall ConsumerObservation(Consumer.BP.Get());
				UK2Node_CallFunction* CallNode = MakeCaller(Test, Consumer.BP.Get(), BP, TEXT("Default Function"), false);
				if (!Test.TestNotNull(TEXT("real default caller"), CallNode)) return false;
				for (const auto& Input : Inputs)
				{
					const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
					const FString Declared = Input->AsObject()->GetStringField(TEXT("default"));
					Test.TestEqual(TEXT("caller receives declared default: ") + Name, PinValue(CallNode->FindPin(FName(*Name))),
						Declared == TEXT("None") ? FString() : Declared);
				}
				Test.TestTrue(TEXT("caller exact generated provider"), CallNode->FunctionReference.GetMemberParentClass() == BP->GeneratedClass.Get());
				Test.TestEqual(TEXT("consumer never saves"), ConsumerObservation.Saves, 0);
				Test.TestFalse(TEXT("consumer package stays absent"), IFileManager::Get().FileExists(*Consumer.FileName));
			}
			Test.TestFalse(TEXT("class default provider remains off disk"), IFileManager::Get().FileExists(*ClassProvider.FileName));
			Test.TestEqual(TEXT("defaults never save"), Observation.Saves, 0);
			Test.TestFalse(TEXT("defaults remain off disk"), IFileManager::Get().FileExists(*Fixture.FileName));
		}
		return true;
	}


#if UE_VERSION_OLDER_THAN(5, 6, 0)
	static bool ExpectUnsupportedRedefinition(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result, const FString& Context)
	{
		bool Success = true, MutationAttempted = true;
		FString Level;
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		bool Rejected = Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Success) && !Success
			&& Result->TryGetBoolField(TEXT("mutationAttempted"), MutationAttempted) && !MutationAttempted
			&& Result->TryGetStringField(TEXT("validationLevel"), Level) && Level == TEXT("preflight")
			&& Result->TryGetArrayField(TEXT("operations"), Operations);
		bool FoundCompatibilityError = false;
		if (Rejected) for (const auto& Operation : *Operations)
		{
			const TSharedPtr<FJsonObject> Object = Operation.IsValid() ? Operation->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("errors"), Errors)) continue;
			for (const auto& Error : *Errors)
			{
				const TSharedPtr<FJsonObject> Detail = Error.IsValid() ? Error->AsObject() : nullptr;
				FString Code, Message, Op, Received;
				FoundCompatibilityError |= Detail.IsValid()
					&& Detail->TryGetStringField(TEXT("code"), Code) && Code == TEXT("INVALID_TARGET")
					&& Detail->TryGetStringField(TEXT("op"), Op) && Op == TEXT("add_functions")
					&& Detail->TryGetStringField(TEXT("received"), Received) && Received == TEXT("Mutable Function")
					&& Detail->TryGetStringField(TEXT("message"), Message)
					&& Message.Contains(TEXT("requires Unreal Engine 5.6 or newer"));
			}
		}
		return Test.TestTrue(Context + TEXT(" explicit compatibility rejection before mutation"), Rejected && FoundCompatibilityError);
	}

	static bool RunLegacyRedefinitionRejection(FAutomationTestBase& Test)
	{
		for (int32 Kind : { 0, 1 }) for (bool bStandalone : { false, true }) for (bool bDirty : { false, true })
		{
			FFixture Fixture(Kind), Consumer(0);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned legacy redefinition target"), BP)) return false;
			auto Initial = Function(TEXT("Mutable Function"));
			Initial->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Linked"), TEXT("int")), DefaultPin(TEXT("Keep"), TEXT("int"), TEXT("5")) });
			Initial->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")) });
			if (!Test.TestTrue(TEXT("UE 5.5 public function creation remains supported"), Succeeded(Call(BP, { Initial }, bStandalone)))
				|| !MakeBody(Test, BP, TEXT("Mutable Function"), TEXT("Linked"))
				|| !MakeCaller(Test, Consumer.BP.Get(), BP, TEXT("Mutable Function"), true)) return false;
			BP->Status = bDirty ? BS_Dirty : BS_UpToDate; Fixture.Package->SetDirtyFlag(bDirty);
			Consumer.BP->Status = bDirty ? BS_Dirty : BS_UpToDate; Consumer.Package->SetDirtyFlag(bDirty);
			const NwiroVariableProof::FSnapshot Before(BP), CallerBefore(Consumer.BP.Get());
			const FString BeforeGraph = ReadGraph(BP, TEXT("Mutable Function"));
			const FString BeforeCallerGraph = ReadGraph(Consumer.BP.Get(), TEXT("Call Provider"));
			FObservedCall Observation(BP), CallerObservation(Consumer.BP.Get());
			const auto Untouched = [&]()
			{
				Test.TestTrue(TEXT("legacy target graph/declarations/status/dirty unchanged"), Before.Matches(BP));
				Test.TestTrue(TEXT("legacy caller graph/declarations/status/dirty unchanged"), CallerBefore.Matches(Consumer.BP.Get()));
				Test.TestEqual(TEXT("legacy target pins/defaults/links unchanged"), ReadGraph(BP, TEXT("Mutable Function")), BeforeGraph);
				Test.TestEqual(TEXT("legacy caller pins/defaults/links unchanged"), ReadGraph(Consumer.BP.Get(), TEXT("Call Provider")), BeforeCallerGraph);
				Test.TestEqual(TEXT("legacy target never compiles"), Observation.Compiles, 0);
				Test.TestEqual(TEXT("legacy caller never compiles"), CallerObservation.Compiles, 0);
				Test.TestEqual(TEXT("legacy target never saves"), Observation.Saves, 0);
				Test.TestEqual(TEXT("legacy caller never saves"), CallerObservation.Saves, 0);
				Test.TestFalse(TEXT("legacy target stays off disk"), IFileManager::Get().FileExists(*Fixture.FileName));
				Test.TestFalse(TEXT("legacy caller stays off disk"), IFileManager::Get().FileExists(*Consumer.FileName));
				return !Test.HasAnyErrors();
			};
			auto Update = Function(TEXT("Mutable Function"));
			Update->AsObject()->SetBoolField(TEXT("redefine"), true);
			Update->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Linked"), TEXT("int")), DefaultPin(TEXT("Keep"), TEXT("int"), TEXT("17")) });
			if (!ExpectUnsupportedRedefinition(Test, Call(BP, { Update }, bStandalone, true), TEXT("changed existing signature")) || !Untouched()) return false;
			for (bool bReverse : { false, true })
			{
				TArray<TSharedPtr<FJsonValue>> Items = { Function(TEXT("Would Be Added")), Update };
				if (bReverse) Swap(Items[0], Items[1]);
				auto Command = EditCommand(BP); Command->RemoveField(TEXT("compile"));
				Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), Items);
				if (!bStandalone) Command->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("WouldBeVariable"), TEXT("int")) });
				if (!ExpectUnsupportedRedefinition(Test, Invoke(Command, bStandalone), TEXT("mixed create/redefine batch")) || !Untouched()) return false;
			}
			auto NoOp = Function(TEXT("Mutable Function")); NoOp->AsObject()->SetBoolField(TEXT("redefine"), true);
			if (!Test.TestTrue(TEXT("UE 5.5 identical redefine remains a no-op"), Succeeded(Call(BP, { NoOp }, bStandalone, true))) || !Untouched()) return false;
		}
		return !Test.HasAnyErrors();
	}
#endif

	static bool RunRedefinition(FAutomationTestBase& Test)
	{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		return RunLegacyRedefinitionRejection(Test);
#else
		// Fixture setup finishes discovery; production preflight never starts a scan.
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SearchAllAssets(true);
		Registry.WaitForCompletion();
		if (!Test.TestTrue(TEXT("signature fixture registry discovery complete"), Registry.IsSearchAllAssets() && !Registry.IsGathering())) return false;
		for (int32 Kind : { 0, 1 }) for (bool bStandalone : { false, true })
		{
			FFixture Fixture(Kind), Consumer(0);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned redefinition target"), BP)) return false;
			auto Item = Function(TEXT("Mutable Function"));
			Item->AsObject()->SetArrayField(TEXT("inputs"), {
				Pin(TEXT("Linked"), TEXT("int")), DefaultPin(TEXT("Keep"), TEXT("int"), TEXT("5")),
				Pin(TEXT("Change"), TEXT("int")), Pin(TEXT("Remove"), TEXT("bool")),
				DefaultPin(TEXT("ClassDefault"), TEXT("class"), AActor::StaticClass()->GetPathName(), TEXT("Object")),
				DefaultPin(TEXT("RemovedClassDefault"), TEXT("class"), AActor::StaticClass()->GetPathName(), TEXT("Object")),
				DefaultPin(TEXT("ResetClassDefault"), TEXT("class"), AActor::StaticClass()->GetPathName(), TEXT("Object")),
				DefaultPin(TEXT("ClearedClassDefault"), TEXT("class"), AActor::StaticClass()->GetPathName(), TEXT("Object"))
			});
			Item->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")), Pin(TEXT("Discard"), TEXT("float")) });
			if (!Test.TestTrue(TEXT("initial mutable signature"), Succeeded(Call(BP, { Item }, false)))
				|| !MakeBody(Test, BP, TEXT("Mutable Function"), TEXT("Linked"))) return false;
			UK2Node_CallFunction* CallNode = MakeCaller(Test, Consumer.BP.Get(), BP, TEXT("Mutable Function"), true);
			if (!Test.TestNotNull(TEXT("loaded connected caller"), CallNode)) return false;
			const FGuid CallGuid = CallNode->NodeGuid;
			UEdGraph* Original = Graph(BP, TEXT("Mutable Function"));
			const FGuid GraphGuid = Original->GraphGuid;
			UK2Node_FunctionEntry* OriginalEntry = Entry(BP, TEXT("Mutable Function"));
			UK2Node_FunctionResult* OriginalExit = Exit(BP, TEXT("Mutable Function"));
			const FGuid EntryGuid = OriginalEntry->NodeGuid, ExitGuid = OriginalExit->NodeGuid;
			OriginalEntry->MetaData.SetMetaData(TEXT("NwiroUnrelatedProof"), FString(TEXT("preserve")));
			TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(Fixture.Package,
				FName(*(TEXT("RedefinedData_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), RF_Public | RF_Transactional));
			if (!Test.TestNotNull(TEXT("redefinition custom struct"), Struct.Get())) return false;
			auto Update = Function(TEXT("Mutable Function"));
			Update->AsObject()->SetBoolField(TEXT("redefine"), true);
			Update->AsObject()->SetArrayField(TEXT("inputs"), {
				DefaultPin(TEXT("Keep"), TEXT("int"), TEXT("17")), Pin(TEXT("Linked"), TEXT("int")),
				Pin(TEXT("Change"), TEXT("double")), Pin(TEXT("Record"), TEXT("struct"), Struct->GetPathName()),
				DefaultPin(TEXT("ClassDefault"), TEXT("class"), USaveGame::StaticClass()->GetPathName(), TEXT("Object")),
				Pin(TEXT("ResetClassDefault"), TEXT("class"), TEXT("Object")),
				DefaultPin(TEXT("ClearedClassDefault"), TEXT("class"), TEXT("None"), TEXT("Object"))
			});
			Update->AsObject()->SetArrayField(TEXT("outputs"), {
				Pin(TEXT("Result"), TEXT("int")), Pin(TEXT("Records"), TEXT("struct"), Struct->GetPathName(), TEXT("array")),
				Pin(TEXT("ActorType"), TEXT("class"), TEXT("Actor"))
			});
			FObservedCall Observation(BP), ConsumerObservation(Consumer.BP.Get());
			auto Response = Call(BP, { Update }, bStandalone, true);
			if (Response.IsValid()) Test.AddInfo(TEXT("redefinition response ") + Serialize(Response.ToSharedRef()));
			if (!Test.TestTrue(TEXT("public ordinary redefinition succeeds"), Succeeded(Response))
				|| !Compile(Test, BP, TEXT("redefined provider"))
				|| !Compile(Test, Consumer.BP.Get(), TEXT("caller after redefinition"))) return false;
			Test.TestTrue(TEXT("graph identity retained"), Graph(BP, TEXT("Mutable Function")) == Original && Original->GraphGuid == GraphGuid);
			Test.TestTrue(TEXT("entry identity retained"), Entry(BP, TEXT("Mutable Function")) == OriginalEntry && OriginalEntry->NodeGuid == EntryGuid);
			Test.TestTrue(TEXT("result/body identity retained"), Exit(BP, TEXT("Mutable Function")) == OriginalExit && OriginalExit->NodeGuid == ExitGuid);
			Test.TestTrue(TEXT("caller node identity retained"), CallNode->NodeGuid == CallGuid);
			Test.TestTrue(TEXT("body data link retained"), OriginalEntry->FindPin(TEXT("Linked"))->LinkedTo.Contains(OriginalExit->FindPin(TEXT("Result"))));
			Test.TestTrue(TEXT("body execution link retained"), OriginalEntry->FindPin(UEdGraphSchema_K2::PN_Then)->LinkedTo.Contains(OriginalExit->FindPin(UEdGraphSchema_K2::PN_Execute)));
			Test.TestTrue(TEXT("caller data link retained"), CallNode->FindPin(TEXT("Linked"))->LinkedTo.Num() == 1 && CallNode->FindPin(TEXT("Result"))->LinkedTo.Num() == 1);
			Test.TestEqual(TEXT("loaded caller updated autogenerated default"), PinValue(CallNode->FindPin(TEXT("Keep"))), FString(TEXT("17")));
			Test.TestEqual(TEXT("caller reconstructed exact changed class default"), PinValue(CallNode->FindPin(TEXT("ClassDefault"))), USaveGame::StaticClass()->GetPathName());
			Test.TestEqual(TEXT("caller clears explicit None class default"), PinValue(CallNode->FindPin(TEXT("ClearedClassDefault"))), FString());
			Test.TestEqual(TEXT("caller resets omitted class default"), PinValue(CallNode->FindPin(TEXT("ResetClassDefault"))), FString());
			Test.TestFalse(TEXT("removed reference default metadata absent"), OriginalEntry->MetaData.HasMetaData(TEXT("CPP_Default_RemovedClassDefault")));
			Test.TestFalse(TEXT("reset reference default metadata absent"), OriginalEntry->MetaData.HasMetaData(TEXT("CPP_Default_ResetClassDefault")));
			Test.TestEqual(TEXT("unrelated entry metadata survives"), OriginalEntry->MetaData.GetMetaData(TEXT("NwiroUnrelatedProof")), FString(TEXT("preserve")));
			for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
			{
				UFunction* Reflected = Class->FindFunctionByName(TEXT("Mutable Function"), EIncludeSuperFlag::ExcludeSuper);
				if (!Test.TestNotNull(TEXT("redefined exact reflected function"), Reflected)) return false;
				Test.TestTrue(TEXT("redefined exact owner"), Reflected->GetOuter() == Class);
				Test.TestTrue(TEXT("new exact struct input"), ReflectedPin(Reflected, TEXT("Record"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::None, false));
				Test.TestTrue(TEXT("new exact array output"), ReflectedPin(Reflected, TEXT("Records"), UEdGraphSchema_K2::PC_Struct, Struct.Get(), EPinContainerType::Array, true));
				Test.TestTrue(TEXT("new exact class output"), ReflectedPin(Reflected, TEXT("ActorType"), UEdGraphSchema_K2::PC_Class, AActor::StaticClass(), EPinContainerType::None, true));
				Test.TestNull(TEXT("removed input absent"), FindFProperty<FProperty>(Reflected, TEXT("Remove")));
				Test.TestNull(TEXT("removed output absent"), FindFProperty<FProperty>(Reflected, TEXT("Discard")));
				TArray<FName> Names;
				for (TFieldIterator<FProperty> It(Reflected); It; ++It) if (It->HasAnyPropertyFlags(CPF_Parm)) Names.Add(It->GetFName());
				Test.TestTrue(TEXT("complete reflected parameter order"), Names == TArray<FName>{ TEXT("Keep"), TEXT("Linked"), TEXT("Change"), TEXT("Record"), TEXT("ClassDefault"), TEXT("ResetClassDefault"), TEXT("ClearedClassDefault"), TEXT("Result"), TEXT("Records"), TEXT("ActorType") });
			}
			// An omitted side must retain its existing declaration, defaults and links.
			auto Partial = Function(TEXT("Mutable Function"));
			Partial->AsObject()->SetBoolField(TEXT("redefine"), true);
			Partial->AsObject()->SetArrayField(TEXT("outputs"), {
				Pin(TEXT("Result"), TEXT("int")), Pin(TEXT("Records"), TEXT("struct"), Struct->GetPathName(), TEXT("array"))
			});
			if (!Test.TestTrue(TEXT("partial signature update succeeds"), Succeeded(Call(BP, { Partial }, bStandalone)))) return false;
			Test.TestEqual(TEXT("omitted input default retained"), PinValue(Entry(BP, TEXT("Mutable Function"))->FindPin(TEXT("Keep"))), FString(TEXT("17")));
			if (!Compile(Test, BP, TEXT("reference defaults after omitted inputs")) || !Compile(Test, Consumer.BP.Get(), TEXT("caller after omitted reference defaults"))) return false;
			Test.TestEqual(TEXT("omitted reference input default retained"), PinValue(CallNode->FindPin(TEXT("ClassDefault"))), USaveGame::StaticClass()->GetPathName());
			for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
			{
				UFunction* Reflected = Class->FindFunctionByName(TEXT("Mutable Function"), EIncludeSuperFlag::ExcludeSuper);
				if (!Test.TestNotNull(TEXT("reference redefinition compiled owner"), Reflected)) return false;
				Test.TestEqual(TEXT("compiled reference default survives omitted input side"), Reflected->GetMetaData(TEXT("CPP_Default_ClassDefault")), USaveGame::StaticClass()->GetPathName());
				Test.TestFalse(TEXT("compiled cleared reference uses engine null without metadata"), Reflected->HasMetaData(TEXT("CPP_Default_ClearedClassDefault")));
				Test.TestFalse(TEXT("compiled removed reference metadata absent"), Reflected->HasMetaData(TEXT("CPP_Default_RemovedClassDefault")));
				Test.TestFalse(TEXT("compiled reset reference metadata absent"), Reflected->HasMetaData(TEXT("CPP_Default_ResetClassDefault")));
				Test.TestEqual(TEXT("compiled unrelated metadata preserved"), Reflected->GetMetaData(TEXT("NwiroUnrelatedProof")), FString(TEXT("preserve")));
			}
			auto NoOp = Function(TEXT("Mutable Function"));
			NoOp->AsObject()->SetBoolField(TEXT("redefine"), true);
			BP->Status = BS_Dirty; Fixture.Package->SetDirtyFlag(true);
			const NwiroVariableProof::FSnapshot Before(BP);
			const FString BeforeRead = ReadGraph(BP, TEXT("Mutable Function"));
			const int32 BeforeCompiles = Observation.Compiles;
			if (!Test.TestTrue(TEXT("identical redefine succeeds as no-op"), Succeeded(Call(BP, { NoOp }, bStandalone)))) return false;
			Test.TestTrue(TEXT("identical redefine preserves dirty state"), Before.Matches(BP));
			Test.TestEqual(TEXT("identical redefine preserves pins"), ReadGraph(BP, TEXT("Mutable Function")), BeforeRead);
			Test.TestEqual(TEXT("identical redefine does not compile"), Observation.Compiles, BeforeCompiles);
			// Distinguish an inherited null default from a caller explicitly cleared by the user.
			UEdGraphPin* UserCleared = CallNode->FindPin(TEXT("ClassDefault"));
			if (!Test.TestNotNull(TEXT("owned caller reference to clear"), UserCleared)) return false;
			GetDefault<UEdGraphSchema_K2>()->TrySetDefaultObject(*UserCleared, nullptr);
			Test.TestEqual(TEXT("user-cleared reference keeps non-null autogenerated default"), UserCleared->AutogeneratedDefaultValue, USaveGame::StaticClass()->GetPathName());
			Test.TestFalse(TEXT("user-cleared null is a caller override"), UserCleared->DoesDefaultValueMatchAutogenerated());
			UEdGraphPin* AutoNull = CallNode->FindPin(TEXT("ClearedClassDefault"));
			if (!Test.TestNotNull(TEXT("owned caller autogenerated null"), AutoNull)) return false;
			Test.TestTrue(TEXT("engine recognizes autogenerated reference null"), AutoNull->DoesDefaultValueMatchAutogenerated());
			auto Transition = Function(TEXT("Mutable Function"));
			Transition->AsObject()->SetBoolField(TEXT("redefine"), true);
			TArray<TSharedPtr<FJsonValue>> TransitionInputs = Update->AsObject()->GetArrayField(TEXT("inputs"));
			for (auto& Input : TransitionInputs)
			{
				const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
				if (Name == TEXT("ClassDefault")) Input = DefaultPin(Name, TEXT("class"), AActor::StaticClass()->GetPathName(), TEXT("Object"));
				if (Name == TEXT("ClearedClassDefault")) Input = DefaultPin(Name, TEXT("class"), USaveGame::StaticClass()->GetPathName(), TEXT("Object"));
			}
			Transition->AsObject()->SetArrayField(TEXT("inputs"), TransitionInputs);
			if (!Test.TestTrue(TEXT("null-to-non-null signature evolution succeeds"), Succeeded(Call(BP, { Transition }, bStandalone)))
				|| !Compile(Test, BP, TEXT("reference null transition provider"))
				|| !Compile(Test, Consumer.BP.Get(), TEXT("reference null transition caller"))) return false;
			Test.TestNull(TEXT("user-cleared caller null remains an override"), CallNode->FindPin(TEXT("ClassDefault"))->DefaultObject.Get());
			Test.TestTrue(TEXT("autogenerated caller null follows new exact class default"), CallNode->FindPin(TEXT("ClearedClassDefault"))->DefaultObject == USaveGame::StaticClass());
			// External metadata edits must not be silently overwritten by an omitted input side.
			OriginalEntry->MetaData.SetMetaData(TEXT("CPP_Default_ClassDefault"), USaveGame::StaticClass()->GetPathName());
			auto Omitted = EditCommand(BP);
			Omitted->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { NoOp });
			if (!RejectUnchanged(Test, BP, TEXT("Mutable Function"), Omitted, bStandalone, TEXT("omitted inputs reject conflicting reference metadata"))) return false;
			Test.TestEqual(TEXT("external reference metadata remains untouched"), OriginalEntry->MetaData.GetMetaData(TEXT("CPP_Default_ClassDefault")), USaveGame::StaticClass()->GetPathName());
			if (!Test.TestTrue(TEXT("explicit inputs repair conflicting reference metadata"), Succeeded(Call(BP, { Update }, bStandalone)))
				|| !Compile(Test, BP, TEXT("explicit reference metadata repair"))) return false;
			Test.TestEqual(TEXT("explicit reference metadata repair is exact"), OriginalEntry->MetaData.GetMetaData(TEXT("CPP_Default_ClassDefault")), USaveGame::StaticClass()->GetPathName());
			Test.TestEqual(TEXT("redefinition never saves target"), Observation.Saves, 0);
			Test.TestEqual(TEXT("redefinition never saves caller"), ConsumerObservation.Saves, 0);
			Test.TestFalse(TEXT("redefined target stays off disk"), IFileManager::Get().FileExists(*Fixture.FileName));
			Test.TestFalse(TEXT("redefined caller stays off disk"), IFileManager::Get().FileExists(*Consumer.FileName));
		}
		return true;
#endif
	}

	static bool RunRejectedEvolution(FAutomationTestBase& Test)
	{
		for (bool bStandalone : { false, true }) for (bool bDirty : { false, true })
		{
			FFixture Fixture(0);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned evolution rejection target"), BP)) return false;
			auto Initial = Function(TEXT("Mutable Function"));
			Initial->AsObject()->SetArrayField(TEXT("inputs"), { DefaultPin(TEXT("Linked"), TEXT("int"), TEXT("7")) });
			Initial->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")) });
			if (!Test.TestTrue(TEXT("rejection seed"), Succeeded(Call(BP, { Initial }, false)))
				|| !MakeBody(Test, BP, TEXT("Mutable Function"), TEXT("Linked"))) return false;
			BP->Status = bDirty ? BS_Dirty : BS_UpToDate; Fixture.Package->SetDirtyFlag(bDirty);
			TArray<TSharedPtr<FJsonValue>> Invalid = {
				DefaultPin(TEXT("Value"), TEXT("int"), TEXT("oops")),
				DefaultPin(TEXT("Value"), TEXT("int"), TEXT("2147483648")),
				DefaultPin(TEXT("Value"), TEXT("int64"), TEXT("-9223372036854775809")),
				DefaultPin(TEXT("Value"), TEXT("byte"), TEXT("999999999999999999999")),
				DefaultPin(TEXT("Value"), TEXT("bool"), TEXT("yesplease")),
				DefaultPin(TEXT("Value"), TEXT("double"), TEXT("1e9999")),
				DefaultPin(TEXT("Value"), TEXT("vector"), TEXT("invalid")),
				DefaultPin(TEXT("Value"), TEXT("class"), TEXT("/Script/CoreUObject.Vector"), TEXT("Actor")),
				DefaultPin(TEXT("Value"), TEXT("object"), TEXT("/Game/NwiroNeverLoadedDefault.Asset"), TEXT("Object")),
				DefaultPin(TEXT("Value"), TEXT("name"), FString::ChrN(NAME_SIZE, TEXT('x')))
			};
			auto WrongJson = Pin(TEXT("Value"), TEXT("int")); WrongJson->AsObject()->SetNumberField(TEXT("default"), 42); Invalid.Add(WrongJson);
			auto Container = Pin(TEXT("Value"), TEXT("int"), FString(), TEXT("array")); Container->AsObject()->SetStringField(TEXT("default"), TEXT("()")); Invalid.Add(Container);
			for (int32 Index = 0; Index < Invalid.Num(); ++Index)
			{
				auto Bad = Function(TEXT("Invalid Default"));
				Bad->AsObject()->SetArrayField(TEXT("inputs"), { Invalid[Index] });
				auto Command = EditCommand(BP); Command->RemoveField(TEXT("compile"));
				Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Function(TEXT("Would Be Added")), Bad });
				if (!bStandalone) Command->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("WouldBeVariable"), TEXT("int")) });
				if (!RejectUnchanged(Test, BP, TEXT("Mutable Function"), Command, bStandalone, FString::Printf(TEXT("invalid default %d"), Index))) return false;
			}
			for (int32 Shape = 0; Shape < 8; ++Shape)
			{
				auto Bad = Function(Shape == 0 ? TEXT("Missing Function") : TEXT("Mutable Function"));
				Bad->AsObject()->SetBoolField(TEXT("redefine"), true);
				if (Shape == 1) Bad->AsObject()->SetArrayField(TEXT("inputs"), {});
				if (Shape == 2) Bad->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Linked"), TEXT("float")) });
				if (Shape == 3) Bad->AsObject()->SetBoolField(TEXT("pure"), true);
				if (Shape == 4) Bad->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Result"), TEXT("int")) });
				if (Shape == 5) Bad->AsObject()->SetBoolField(TEXT("override"), true);
				if (Shape == 6) Bad->AsObject()->SetStringField(TEXT("redefine"), TEXT("true"));
				if (Shape == 7) Bad->AsObject()->SetArrayField(TEXT("outputs"), { DefaultPin(TEXT("Result"), TEXT("int"), TEXT("5")) });
				auto Command = EditCommand(BP); Command->RemoveField(TEXT("compile"));
				Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Function(TEXT("Would Be Added")), Bad });
				if (!RejectUnchanged(Test, BP, TEXT("Mutable Function"), Command, bStandalone, FString::Printf(TEXT("invalid redefine %d"), Shape))) return false;
			}
		}
		// FIX-011 exact historical malformed shape (BP-008.calls.jsonl:189), using owned targets.
		for (bool bDirty : { false, true })
		{
			FFixture Interface(2);
			UBlueprint* BP = Interface.BP.Get();
			BP->Status = bDirty ? BS_Dirty : BS_UpToDate; Interface.Package->SetDirtyFlag(bDirty);
			auto Command = EditCommand(BP); Command->RemoveField(TEXT("compile"));
			Command->SetStringField(TEXT("name"), TEXT("SetPaused"));
			auto Parameter = MakeShared<FJsonObject>();
			Parameter->SetStringField(TEXT("name"), TEXT("Paused"));
			Parameter->SetStringField(TEXT("type"), TEXT("bool"));
			Parameter->SetStringField(TEXT("direction"), TEXT("input"));
			Command->SetArrayField(TEXT("functions"), { MakeShared<FJsonValueObject>(Parameter) });
			if (!RejectUnchanged(Test, BP, TEXT("ExistingFunction"), Command, true, TEXT("FIX011 nested parameter-shaped function rejects"))) return false;
			Test.TestNull(TEXT("FIX011 parameter-named graph absent"), Graph(BP, TEXT("Paused")));
			Test.TestNull(TEXT("FIX011 rejected requested graph absent"), Graph(BP, TEXT("SetPaused")));
			Command->RemoveField(TEXT("functions"));
			Command->SetArrayField(TEXT("inputs"), { Pin(TEXT("Paused"), TEXT("bool")), Pin(TEXT("Index"), TEXT("int")) });
			FObservedCall Observation(BP);
			if (!Test.TestTrue(TEXT("FIX011 valid top-level multi-parameter request succeeds"), Succeeded(Invoke(Command, true)))) return false;
			for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
			{
				UFunction* Function = Class->FindFunctionByName(TEXT("SetPaused"), EIncludeSuperFlag::ExcludeSuper);
				if (!Test.TestNotNull(TEXT("FIX011 exact top-level function exists"), Function)) return false;
				Test.TestTrue(TEXT("FIX011 exact top-level function owner"), Function->GetOuter() == Class);
				Test.TestTrue(TEXT("FIX011 first parameter exact bool"), ReflectedPin(Function, TEXT("Paused"), UEdGraphSchema_K2::PC_Boolean, nullptr, EPinContainerType::None, false));
				Test.TestTrue(TEXT("FIX011 second parameter exact int"), ReflectedPin(Function, TEXT("Index"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::None, false));
			}
			Test.TestNull(TEXT("FIX011 valid request has no parameter-named graph"), Graph(BP, TEXT("Paused")));
			Test.TestEqual(TEXT("FIX011 valid request never saves"), Observation.Saves, 0);
			Test.TestFalse(TEXT("FIX011 fixture remains off disk"), IFileManager::Get().FileExists(*Interface.FileName));
		}
		for (bool bStandalone : { false, true })
		{
			FFixture Interface(2);
			auto Bad = Function(TEXT("ExistingFunction")); Bad->AsObject()->SetBoolField(TEXT("redefine"), true);
			auto Command = EditCommand(Interface.BP.Get());
			Command->SetArrayField(bStandalone ? TEXT("functions") : TEXT("add_functions"), { Bad });
			if (!RejectUnchanged(Test, Interface.BP.Get(), TEXT("ExistingFunction"), Command, bStandalone, TEXT("interface signature cannot be redefined as ordinary"))) return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionInputDefaultsProof,
	"Nwiro.Pro.Blueprint.Functions.Production.InputDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionInputDefaultsProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroFunctionEvolutionProof::RunInputDefaults(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionSignatureRedefinitionProof,
	"Nwiro.Pro.Blueprint.Functions.Production.SignatureRedefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionSignatureRedefinitionProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroFunctionEvolutionProof::RunRedefinition(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionEvolutionNoTouchProof,
	"Nwiro.Pro.Blueprint.Functions.Production.DefaultsAndRedefinitionNoTouch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionEvolutionNoTouchProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroFunctionEvolutionProof::RunRejectedEvolution(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}
