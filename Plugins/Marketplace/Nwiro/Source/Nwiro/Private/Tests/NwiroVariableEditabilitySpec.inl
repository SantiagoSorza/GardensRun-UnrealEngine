// Copyright 2026 Nwiro. All Rights Reserved.
// Included after FunctionAcceptanceSpec.inl; only GUID-owned, unsaved fixtures.
namespace NwiroVariableEditabilityProof
{
	struct FFixture
	{
		TStrongObjectPtr<UBlueprint> BP;
		UPackage* Package = nullptr;
		FString FileName;

		explicit FFixture(int32 Kind)
		{
			const FString Name = TEXT("BP_Editability_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Path = TEXT("/Game/NwiroEditabilityAcceptance_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/") + Name;
			Package = CreatePackage(*Path);
			FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
			const bool bWidget = Kind == 2;
			BP.Reset(FKismetEditorUtilities::CreateBlueprint(bWidget ? UUserWidget::StaticClass()
				: Kind == 1 ? USaveGame::StaticClass() : AActor::StaticClass(), Package, FName(*Name), BPTYPE_Normal,
				bWidget ? UWidgetBlueprint::StaticClass() : UBlueprint::StaticClass(),
				bWidget ? UWidgetBlueprintGeneratedClass::StaticClass() : UBlueprintGeneratedClass::StaticClass(),
				TEXT("NwiroEditabilityAcceptance")));
			if (!BP.IsValid()) return;
			FKismetEditorUtilities::CompileBlueprint(BP.Get(), EBlueprintCompileOptions::SkipSave);
			Package->SetDirtyFlag(false);
		}

		~FFixture()
		{
			if (!Package) return;
			// Release the owned graph normally; do not individually invalidate live
			// generated functions while compiler/GC references still exist.
			ForEachObjectWithPackage(Package, [](UObject* Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				return true;
			}, true);
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	};

	static bool Run(FAutomationTestBase& Test)
	{
		for (int32 Kind : { 0, 1, 2 })
		{
			FFixture Fixture(Kind);
			UBlueprint* BP = Fixture.BP.Get();
			if (!Test.TestNotNull(TEXT("owned editability Blueprint"), BP)) return false;
			TArray<TSharedPtr<FJsonValue>> Variables;
			auto Add = [&](const TCHAR* Name, const TCHAR* Key, bool Value, bool bSpawn)
			{
				auto Item = NwiroVariableProof::Variable(Name, TEXT("int"));
				if (Key) Item->AsObject()->SetBoolField(Key, Value);
				if (bSpawn) Item->AsObject()->SetBoolField(TEXT("exposeOnSpawn"), true);
				Variables.Add(Item);
			};
			Add(TEXT("DefaultOnly"), nullptr, false, false);
			Add(TEXT("Editable"), TEXT("instanceEditable"), true, false);
			Add(TEXT("Locked"), TEXT("instanceEditable"), false, false);
			Add(TEXT("SnakeAlias"), TEXT("instance_editable"), true, false);
			Add(TEXT("CaseAlias"), TEXT("INSTANCE_EDITABLE"), true, false);
			Add(TEXT("SpawnImplicit"), nullptr, false, true);
			Add(TEXT("SpawnExplicit"), TEXT("instanceEditable"), true, true);
			NwiroFunctionProof::FObservedCall Observation(BP);
			const auto Result = NwiroVariableProof::Edit(BP, Variables);
			if (!Test.TestTrue(TEXT("public editability batch succeeds"), Result.IsValid() && Result->GetBoolField(TEXT("success")))) return false;
			Test.TestEqual(TEXT("compile:false does not fully compile"), Observation.Compiles, 0);
			for (const auto& Item : Variables)
			{
				const FString Name = Item->AsObject()->GetStringField(TEXT("name"));
				const bool bEditable = Name != TEXT("DefaultOnly") && Name != TEXT("Locked");
				const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*Name));
				if (!Test.TestTrue(TEXT("variable description exists: ") + Name, BP->NewVariables.IsValidIndex(Index))) return false;
				const uint64 Flags = BP->NewVariables[Index].PropertyFlags;
				Test.TestTrue(TEXT("description retains CPF_Edit: ") + Name, (Flags & CPF_Edit) != 0);
				Test.TestEqual(TEXT("description instance flag: ") + Name, (Flags & CPF_DisableEditOnInstance) == 0, bEditable);
			}
			if (!NwiroFunctionAcceptance::Compile(Test, BP, TEXT("editability reflection"))) return false;
			for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
			{
				for (const auto& Item : Variables)
				{
					const FString Name = Item->AsObject()->GetStringField(TEXT("name"));
					const bool bEditable = Name != TEXT("DefaultOnly") && Name != TEXT("Locked");
					FProperty* Property = FindFProperty<FProperty>(Class, FName(*Name));
					if (!Test.TestNotNull(TEXT("exact reflected variable: ") + Name, Property)) return false;
					Test.TestTrue(TEXT("exact reflected owner: ") + Name, Property->GetOwnerStruct() == Class);
					Test.TestTrue(TEXT("reflected CPF_Edit: ") + Name, Property->HasAnyPropertyFlags(CPF_Edit));
					Test.TestEqual(TEXT("reflected instance editability: ") + Name, !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance), bEditable);
					const bool bSpawn = Name.StartsWith(TEXT("Spawn"));
					Test.TestEqual(TEXT("expose-on-spawn metadata: ") + Name, Property->HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn), bSpawn);
				}
			}
			for (bool bDirty : { false, true }) for (int32 Shape = 0; Shape < 4; ++Shape)
			{
				BP->Status = bDirty ? BS_Dirty : BS_UpToDate;
				Fixture.Package->SetDirtyFlag(bDirty);
				auto Bad = NwiroVariableProof::Variable(TEXT("BadVariable"), TEXT("int"));
				if (Shape == 0) Bad->AsObject()->SetStringField(TEXT("instanceEditable"), TEXT("true"));
				if (Shape == 1) Bad->AsObject()->SetField(TEXT("instanceEditable"), MakeShared<FJsonValueNull>());
				if (Shape == 2)
				{
					Bad->AsObject()->SetBoolField(TEXT("instanceEditable"), true);
					Bad->AsObject()->SetBoolField(TEXT("instance_editable"), true);
				}
				if (Shape == 3)
				{
					Bad->AsObject()->SetBoolField(TEXT("instanceEditable"), false);
					Bad->AsObject()->SetBoolField(TEXT("expose_on_spawn"), true);
				}
				const NwiroVariableProof::FSnapshot Before(BP);
				const int32 Compiles = Observation.Compiles;
				const auto Rejected = NwiroVariableProof::Edit(BP,
					{ NwiroVariableProof::Variable(TEXT("WouldBeAdded"), TEXT("int")), Bad }, true);
				Test.TestTrue(TEXT("invalid editability rejects entire batch"), Rejected.IsValid() && !Rejected->GetBoolField(TEXT("success")));
				Test.TestTrue(TEXT("invalid editability complete state preserved"), Before.Matches(BP));
				Test.TestEqual(TEXT("invalid editability no compile"), Observation.Compiles, Compiles);
			}
			Test.TestEqual(TEXT("editability never saves"), Observation.Saves, 0);
			Test.TestFalse(TEXT("editability fixture never written"), IFileManager::Get().FileExists(*Fixture.FileName));
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroVariableInstanceEditableProof,
	"Nwiro.Pro.Blueprint.Variables.Production.InstanceEditable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroVariableInstanceEditableProof::RunTest(const FString& Parameters)
{
	const auto Previous = GetDefault<UBlueprintEditorSettings>()->SaveOnCompile;
	bool Completed;
	{ NwiroFunctionProof::FNeverSaveOnCompile Guard; Completed = NwiroVariableEditabilityProof::Run(*this); }
	TestEqual(TEXT("save-on-compile preference restored"), GetDefault<UBlueprintEditorSettings>()->SaveOnCompile.GetValue(), Previous.GetValue());
	return Completed;
}
