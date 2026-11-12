// Copyright 2026 Nwiro. All Rights Reserved.
// Public tool regression proofs. Fixtures are unique, unsaved /Game packages.
namespace NwiroBlueprintRemediationProof
{
	using NwiroVariableProof::Serialize;
	using NwiroFunctionProof::FFixture;
	using NwiroFunctionProof::FObservedCall;
	using NwiroFunctionProof::FNeverSaveOnCompile;

	static TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	static TSharedPtr<FJsonObject> ReadVariable(UBlueprint* BP, const FString& Name)
	{
		const auto Result = Parse(FNwiroBlueprintTools::ReadBlueprint(BP->GetPathName()));
		if (!Result.IsValid()) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		if (!Result->TryGetArrayField(TEXT("variables"), Variables)) return nullptr;
		for (const auto& Variable : *Variables)
			if (Variable->AsObject()->GetStringField(TEXT("name")) == Name) return Variable->AsObject();
		return nullptr;
	}

	static TSharedPtr<FJsonObject> Remove(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items, bool bStandalone)
	{
		const auto Cmd = MakeShared<FJsonObject>();
		Cmd->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Cmd->SetArrayField(bStandalone ? TEXT("interfaces") : TEXT("remove_interfaces"), Items);
		if (!bStandalone)
		{
			Cmd->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("MustNotAppear"), TEXT("int")) });
			Cmd->SetBoolField(TEXT("compile"), false);
		}
		return Parse(bStandalone ? FNwiroBlueprintTools::RemoveInterface(Serialize(Cmd)) : FNwiroBlueprintTools::EditBlueprint(Serialize(Cmd)));
	}

	static bool Interfaces(FAutomationTestBase& Test)
	{
		FNeverSaveOnCompile SaveGuard;
		FFixture Interface(2);
		if (!Test.TestNotNull(TEXT("owned interface"), Interface.BP.Get())) return false;
		FBlueprintEditorUtils::RenameGraph(Interface.BP->FunctionGraphs[0], TEXT("InterfaceProbe"));
		FKismetEditorUtilities::CompileBlueprint(Interface.BP.Get(), EBlueprintCompileOptions::SkipSave);
		for (bool bStandalone : { false, true })
		{
			FFixture Fixture(0);
			UBlueprint* BP = Fixture.BP.Get();
			FBlueprintEditorUtils::ImplementNewInterface(BP, Interface.BP->GeneratedClass->GetClassPathName());
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
			const FString Direct = Interface.BP->GetPathName();
			const auto Good = MakeShared<FJsonValueString>(Direct);
			const auto Missing = MakeShared<FJsonValueString>(TEXT("/Script/Engine.Interface_AssetUserData"));
			TArray<TArray<TSharedPtr<FJsonValue>>> Rejections = {
				{ Good, Missing }, { Missing, Good }, { Good, Good }, { MakeShared<FJsonValueNull>() }, {}
			};
			FObservedCall Observation(BP);
			for (bool bDirty : { false, true }) for (const auto& Items : Rejections)
			{
				BP->Status = bDirty ? BS_Dirty : BS_UpToDate;
				Fixture.Package->SetDirtyFlag(bDirty);
				const NwiroVariableProof::FSnapshot Before(BP);
				const TArray<FBPInterfaceDescription> BeforeInterfaces = BP->ImplementedInterfaces;
				const int32 Compiles = Observation.Compiles;
				const auto Result = Remove(BP, Items, bStandalone);
				Test.TestTrue(TEXT("invalid whole interface batch fails"), Result.IsValid() && !Result->GetBoolField(TEXT("success")));
				Test.TestTrue(TEXT("invalid interface batch leaves target structure/status/dirty intact"), Before.Matches(BP));
				Test.TestEqual(TEXT("direct membership count unchanged"), BP->ImplementedInterfaces.Num(), BeforeInterfaces.Num());
				for (int32 Index = 0; Index < BeforeInterfaces.Num() && Index < BP->ImplementedInterfaces.Num(); ++Index)
				{
					Test.TestTrue(TEXT("exact interface identity unchanged"), BP->ImplementedInterfaces[Index].Interface == BeforeInterfaces[Index].Interface);
					Test.TestTrue(TEXT("exact interface graphs unchanged"), BP->ImplementedInterfaces[Index].Graphs == BeforeInterfaces[Index].Graphs);
				}
				Test.TestEqual(TEXT("invalid interface batch never compiles"), Observation.Compiles, Compiles);
			}
			// An inherited implementation is visible at runtime but cannot be removed
			// from the child Blueprint's direct interface declaration array.
			{
				// The general fixture seeds ExistingFunction, which is already inherited.
				// A normal child needs no duplicate function or variable declarations.
				const FName ChildName(*(TEXT("BP_InterfaceChild_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
				TStrongObjectPtr<UBlueprint> Child(FKismetEditorUtilities::CreateBlueprint(BP->GeneratedClass,
					Fixture.Package, ChildName, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("NwiroInterfaceRemovalProof")));
				if (!Test.TestNotNull(TEXT("normal inherited child fixture"), Child.Get())) return false;
				FKismetEditorUtilities::CompileBlueprint(Child.Get(), EBlueprintCompileOptions::SkipSave);
				if (!Test.TestEqual(TEXT("inherited child compiles without duplicate declarations"), Child->Status.GetValue(), BS_UpToDate)) return false;
				Test.TestEqual(TEXT("child has no direct interface declaration"), Child->ImplementedInterfaces.Num(), 0);
				Test.TestTrue(TEXT("child actually inherits interface implementation"), Child->GeneratedClass && Child->GeneratedClass->ImplementsInterface(Interface.BP->GeneratedClass));
				Fixture.Package->SetDirtyFlag(false);
				const NwiroVariableProof::FSnapshot Before(Child.Get());
				const auto Result = Remove(Child.Get(), { Good }, bStandalone);
				Test.TestTrue(TEXT("inherited-only interface removal fails"), Result.IsValid() && !Result->GetBoolField(TEXT("success")));
				Test.TestTrue(TEXT("inherited-only removal is no-touch"), Before.Matches(Child.Get()));
			}
			FFixture OtherInterface(2);
			FBlueprintEditorUtils::RenameGraph(OtherInterface.BP->FunctionGraphs[0], TEXT("OtherInterfaceProbe"));
			FKismetEditorUtilities::CompileBlueprint(OtherInterface.BP.Get(), EBlueprintCompileOptions::SkipSave);
			FBlueprintEditorUtils::ImplementNewInterface(BP, OtherInterface.BP->GeneratedClass->GetClassPathName());
			const auto Other = MakeShared<FJsonValueString>(OtherInterface.BP->GetPathName());
			const auto Result = Remove(BP, { Good, Other }, bStandalone);
			Test.TestTrue(TEXT("actual direct interface removal succeeds"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
			Test.TestEqual(TEXT("requested direct membership is gone"), BP->ImplementedInterfaces.Num(), 0);
			if (bStandalone) Test.TestEqual(TEXT("standalone reports actual removed count"), Result->GetIntegerField(TEXT("removed")), 2);
			Test.TestTrue(TEXT("successful interface removal is dirty"), Fixture.Package->IsDirty());
			Test.TestEqual(TEXT("interface tool never saves"), Observation.Saves, 0);
			Test.TestFalse(TEXT("interface fixture remains unsaved"), IFileManager::Get().FileExists(*Fixture.FileName));
		}
		return true;
	}

	static bool DefaultsAndDeletion(FAutomationTestBase& Test)
	{
		FNeverSaveOnCompile SaveGuard;
		for (int32 Kind : { 0, 1 })
		{
			FFixture Fixture(Kind);
			UBlueprint* BP = Fixture.BP.Get();
			UObject* CDO = BP->GeneratedClass->GetDefaultObject(false);
			FIntProperty* Property = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("TrackInput"));
			if (!Test.TestNotNull(TEXT("existing default object"), CDO) || !Test.TestNotNull(TEXT("reflected default property"), Property)) return false;
			Property->SetPropertyValue_InContainer(CDO, 876);
			BP->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("TrackInput"))].DefaultValue.Reset();
			FObservedCall Observation(BP);
			for (bool bDirty : { false, true })
			{
				BP->Status = bDirty ? BS_Dirty : BS_UpToDate;
				Fixture.Package->SetDirtyFlag(bDirty);
				const NwiroVariableProof::FSnapshot Before(BP);
				const auto Var = ReadVariable(BP, TEXT("TrackInput"));
				if (!Test.TestTrue(TEXT("public reader returns member"), Var.IsValid())) return false;
				Test.TestEqual(TEXT("effective CDO default observed"), Var->GetStringField(TEXT("defaultValue")), FString(TEXT("876")));
				Test.TestEqual(TEXT("declaration remains independently visible"), Var->GetStringField(TEXT("declaredDefaultValue")), FString());
				Test.TestEqual(TEXT("CDO source explicit"), Var->GetStringField(TEXT("defaultValueSource")), FString(TEXT("generatedClassCDO")));
				Test.TestEqual(TEXT("stale compiled state explicit"), Var->GetStringField(TEXT("defaultValueStatus")), FString(bDirty ? TEXT("possibly_stale") : TEXT("compiled")));
				Test.TestTrue(TEXT("default read leaves source structure/status/dirty intact"), Before.Matches(BP));
				Test.TestTrue(TEXT("default read preserves CDO identity"), BP->GeneratedClass->GetDefaultObject(false) == CDO);
				Test.TestEqual(TEXT("default read preserves CDO value"), Property->GetPropertyValue_InContainer(CDO), 876);
			}
			Test.TestEqual(TEXT("compiled default reads never compile"), Observation.Compiles, 0);
			// A pending member not represented in the generated class falls back
			// to its declaration without forcing a full compile or creating a CDO.
			FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Int;
			FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("PendingDefault"), Type, TEXT("924"));
			const int32 CompilesBefore = Observation.Compiles;
			const NwiroVariableProof::FSnapshot BeforePending(BP);
			const auto Pending = ReadVariable(BP, TEXT("PendingDefault"));
			Test.TestTrue(TEXT("pending default has declaration source"), Pending.IsValid() && Pending->GetStringField(TEXT("defaultValueSource")) == TEXT("variableDeclaration"));
			Test.TestTrue(TEXT("pending default value retained"), Pending.IsValid() && Pending->GetStringField(TEXT("defaultValue")) == TEXT("924"));
			Test.TestTrue(TEXT("pending default read no-touch"), BeforePending.Matches(BP));
			Test.TestEqual(TEXT("pending read never compiles"), Observation.Compiles, CompilesBefore);
			Test.TestEqual(TEXT("default reader never saves"), Observation.Saves, 0);
		}
		// Exercise both terminal protections. Actor Blueprints lacking a class are
		// rejected by the existing preload guard. SaveGame Blueprints do not require
		// a construction script and reach the delete-specific class/CDO guard.
		for (UClass* Parent : { AActor::StaticClass(), USaveGame::StaticClass() })
		{
			const FString Name = TEXT("BP_Incomplete_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			UPackage* Package = CreatePackage(*(TEXT("/Game/NwiroDeleteAcceptance/") + Name));
			TStrongObjectPtr<UBlueprint> Incomplete(NewObject<UBlueprint>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional));
			Incomplete->ParentClass = Parent;
			const FString Path = Incomplete->GetPathName();
			const EObjectFlags Flags = Incomplete->GetFlags();
			Package->SetDirtyFlag(false);
			const auto Cmd = MakeShared<FJsonObject>(); Cmd->SetStringField(TEXT("assetPath"), Path);
			const auto Result = Parse(FNwiroBlueprintTools::DeleteBlueprint(Serialize(Cmd)));
			Test.TestTrue(TEXT("incomplete deletion reports failure"), Result.IsValid() && !Result->GetBoolField(TEXT("success")));
			FString Code;
			if (Parent == AActor::StaticClass())
			{
				Test.TestTrue(TEXT("actor rejection retains exact terminal preload code"), Result.IsValid() && Result->TryGetStringField(TEXT("errorCode"), Code)
					&& Code == TEXT("loaded_blueprint_missing_generated_class"));
			}
			else
			{
				Test.TestTrue(TEXT("incomplete SaveGame reaches delete-specific guard"), Result.IsValid() && Result->TryGetStringField(TEXT("code"), Code)
					&& Code == TEXT("blueprint_not_ready_for_deletion"));
				bool Deleted = true, Saved = true;
				Test.TestTrue(TEXT("delete guard explicitly reports no deletion"), Result.IsValid() && Result->TryGetBoolField(TEXT("deleted"), Deleted) && !Deleted);
				Test.TestTrue(TEXT("delete guard explicitly reports no save"), Result.IsValid() && Result->TryGetBoolField(TEXT("savedToDisk"), Saved) && !Saved);
			}
			Test.TestTrue(TEXT("incomplete deletion preserves exact object"), StaticFindObject(UBlueprint::StaticClass(), nullptr, *Path) == Incomplete.Get());
			Test.TestEqual(TEXT("incomplete deletion preserves flags"), Incomplete->GetFlags(), Flags);
			Test.TestFalse(TEXT("incomplete deletion preserves clean package"), Package->IsDirty());
			Test.TestNull(TEXT("incomplete deletion does not seed a generated class"), Incomplete->GeneratedClass.Get());
			Incomplete->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
		return true;
	}

	static bool VariableNodes(FAutomationTestBase& Test)
	{
		FNeverSaveOnCompile SaveGuard;
		FFixture Fixture(0);
		UBlueprint* BP = Fixture.BP.Get();
		FObservedCall Observation(BP);
		for (const TCHAR* Type : { TEXT("VariableGet"), TEXT("VariableSet"), TEXT("GetVariable"), TEXT("SetVariable") })
		{
			for (const FString& BadName : { FString(), FString(TEXT("MissingMember_Proof")) }) for (bool bDirty : { false, true })
			{
				const auto Cmd = MakeShared<FJsonObject>(); Cmd->SetStringField(TEXT("blueprint"), BP->GetPathName());
				Cmd->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("MustNotAppear"), TEXT("int")) });
				const auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("type"), Type);
				if (!BadName.IsEmpty()) Node->SetStringField(TEXT("variable"), BadName);
				const auto ValidNode = MakeShared<FJsonObject>(); ValidNode->SetStringField(TEXT("type"), TEXT("VariableGet")); ValidNode->SetStringField(TEXT("variable"), TEXT("TrackInput"));
				Cmd->SetArrayField(TEXT("add_nodes"), { MakeShared<FJsonValueObject>(ValidNode), MakeShared<FJsonValueObject>(Node) });
				Cmd->SetBoolField(TEXT("compile"), false);
				BP->Status = bDirty ? BS_Dirty : BS_UpToDate; Fixture.Package->SetDirtyFlag(bDirty);
				const NwiroVariableProof::FSnapshot Before(BP); const int32 Compiles = Observation.Compiles;
				const auto Result = Parse(FNwiroBlueprintTools::EditBlueprint(Serialize(Cmd)));
				Test.TestTrue(TEXT("invalid variable node rejects complete edit"), Result.IsValid() && !Result->GetBoolField(TEXT("success")));
				Test.TestTrue(TEXT("invalid variable node does not create earlier variable/node"), Before.Matches(BP));
				Test.TestEqual(TEXT("invalid variable node does not compile"), Observation.Compiles, Compiles);
			}
		}
		const auto Cmd = MakeShared<FJsonObject>(); Cmd->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Cmd->SetArrayField(TEXT("add_variables"), { NwiroVariableProof::Variable(TEXT("NewMember"), TEXT("int")) });
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const TCHAR* Type : { TEXT("VariableGet"), TEXT("VariableSet") })
		{
			const auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("type"), Type); Node->SetStringField(TEXT("variable"), TEXT("NewMember"));
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
		}
		Cmd->SetArrayField(TEXT("add_nodes"), Nodes); Cmd->SetBoolField(TEXT("compile"), false);
		const auto Result = Parse(FNwiroBlueprintTools::EditBlueprint(Serialize(Cmd)));
		Test.TestTrue(TEXT("same-call declaration plus both accessor nodes succeeds"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
		TArray<UK2Node_Variable*> Variables; FBlueprintEditorUtils::GetAllNodesOfClass(BP, Variables);
		int32 Resolved = 0;
		for (UK2Node_Variable* Node : Variables)
		{
			if (Node->VariableReference.GetMemberName() != TEXT("NewMember")) continue;
			UEdGraphPin* Pin = Node->FindPin(TEXT("NewMember"));
			Test.TestTrue(TEXT("new member pin resolves exact integer type"), Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
			++Resolved;
		}
		Test.TestEqual(TEXT("exactly two resolved new member nodes"), Resolved, 2);
		Test.TestEqual(TEXT("variable node requests never save"), Observation.Saves, 0);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroInterfaceRemovalRemediationProof,
	"Nwiro.Pro.Blueprint.Remediation.InterfaceRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroInterfaceRemovalRemediationProof::RunTest(const FString& Parameters)
{
	return NwiroBlueprintRemediationProof::Interfaces(*this);
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroDefaultDeletionRemediationProof,
	"Nwiro.Pro.Blueprint.Remediation.DefaultsAndDeletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroDefaultDeletionRemediationProof::RunTest(const FString& Parameters)
{
	return NwiroBlueprintRemediationProof::DefaultsAndDeletion(*this);
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroVariableNodesRemediationProof,
	"Nwiro.Pro.Blueprint.Remediation.VariableNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroVariableNodesRemediationProof::RunTest(const FString& Parameters)
{
	return NwiroBlueprintRemediationProof::VariableNodes(*this);
}
