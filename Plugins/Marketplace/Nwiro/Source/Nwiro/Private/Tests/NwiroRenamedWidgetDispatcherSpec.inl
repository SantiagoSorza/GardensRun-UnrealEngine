// Copyright 2026 Nwiro. All Rights Reserved.
// Public dispatcher regressions for renamed Widget and Actor Blueprints.
// Included after the variable proof fixture/snapshot support in this module.
//
// Scope: engine virtual in-memory Blueprint rename, followed by public dispatcher
// operations. This does NOT reproduce the historical saved on-disk RenameBlueprint
// seed. Public RenameBlueprint calls AssetTools::RenameAssets, whose reference
// fixup can save packages; a no-save proof must not silently invoke that path.
// The historical display spelling "Button Pressed" is rejected by the current
// naming contract. A response, untouched state and working ButtonPressed retry
// are the selected behavior; this proof does not assert spaced-name support.

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Misc/EngineVersionComparison.h"

namespace NwiroRenamedDispatcherProof
{
	static TSharedPtr<FJsonValue> Dispatcher(const FString& Name, bool bTyped = false)
	{
		const auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		if (bTyped)
		{
			const auto Ready = MakeShared<FJsonObject>();
			Ready->SetStringField(TEXT("name"), TEXT("Ready"));
			Ready->SetStringField(TEXT("type"), TEXT("bool"));
			const auto Context = MakeShared<FJsonObject>();
			Context->SetStringField(TEXT("name"), TEXT("Context"));
			Context->SetStringField(TEXT("type"), TEXT("Actor"));
			Obj->SetArrayField(TEXT("params"), {
				MakeShared<FJsonValueObject>(Ready), MakeShared<FJsonValueObject>(Context) });
		}
		return MakeShared<FJsonValueObject>(Obj);
	}

	static TSharedPtr<FJsonObject> Call(UBlueprint* BP,
		const TArray<TSharedPtr<FJsonValue>>& Items, bool bStandalone, bool bSingle = false)
	{
		const auto Cmd = MakeShared<FJsonObject>();
		Cmd->SetStringField(TEXT("blueprint"), BP->GetPathName());
		if (bStandalone)
		{
			if (bSingle)
			{
				Cmd->SetStringField(TEXT("name"), Items[0]->AsObject()->GetStringField(TEXT("name")));
				if (Items[0]->AsObject()->HasField(TEXT("params")))
					Cmd->SetField(TEXT("params"), Items[0]->AsObject()->TryGetField(TEXT("params")));
			}
			else Cmd->SetArrayField(TEXT("dispatchers"), Items);
		}
		else
		{
			Cmd->SetArrayField(TEXT("add_event_dispatchers"), Items);
			Cmd->SetBoolField(TEXT("compile"), false);
		}
		const FString Request = NwiroVariableProof::Serialize(Cmd);
		const FString Response = bStandalone
			? FNwiroBlueprintTools::CreateEventDispatcher(Request)
			: FNwiroBlueprintTools::EditBlueprint(Request);
		TSharedPtr<FJsonObject> Out;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Out);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroRenamedWidgetDispatcherProof,
	"Nwiro.Pro.Blueprint.EventDispatcher.Production.RenamedWidgetAndActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroRenamedWidgetDispatcherProof::RunTest(const FString& Parameters)
{
	using namespace NwiroVariableProof;
	using namespace NwiroRenamedDispatcherProof;
	for (bool bWidget : { false, true }) for (bool bStandalone : { false, true })
	{
		FFixture Fixture(bWidget);
		UBlueprint* BP = Fixture.BP.Get();
		if (!TestNotNull(TEXT("owned Actor/Widget fixture"), BP)) return false;
		UButton* Button = nullptr;
		UTextBlock* Label = nullptr;
		TArray<UWidget*> WidgetsBefore;
		if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP))
		{
			UCanvasPanel* Canvas = CastChecked<UCanvasPanel>(WidgetBP->WidgetTree->RootWidget);
			Button = WidgetBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ProofButton"));
			Label = WidgetBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ProofLabel"));
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
			// UE 5.6+ tracks a stable variable GUID for each new source widget.
			WidgetBP->OnVariableAdded(Button->GetFName());
			WidgetBP->OnVariableAdded(Label->GetFName());
#endif
			Label->SetText(FText::FromString(TEXT("Preserve this button label")));
			Button->AddChild(Label);
			Canvas->AddChild(Button);
		}
		const auto VariableResult = Edit(BP, { Variable(TEXT("LabelValue"), TEXT("text")) });
		if (!TestTrue(TEXT("public Text member fixture"), VariableResult.IsValid() && VariableResult->GetBoolField(TEXT("success")))) return false;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
		if (!TestTrue(TEXT("engine rename succeeds in the same owned package"),
			BP->Rename(*(TEXT("Renamed_") + BP->GetName()), Fixture.Package,
				REN_DontCreateRedirectors | REN_NonTransactional))) return false;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
		TestEqual(TEXT("renamed Blueprint remains compiled"), BP->Status.GetValue(), BS_UpToDate);
		if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP))
			WidgetBP->WidgetTree->GetAllWidgets(WidgetsBefore);
		Fixture.Package->SetDirtyFlag(false);

		for (bool bDirty : { false, true })
		{
			if (bDirty)
			{
				const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("LabelValue"));
				if (!TestTrue(TEXT("Text fixture declaration remains available"), BP->NewVariables.IsValidIndex(Index))) return false;
				BP->NewVariables[Index].SetMetaData(FBlueprintMetadata::MD_Tooltip, TEXT("Preserve unsaved tooltip"));
				BP->Status = BS_Dirty;
				Fixture.Package->SetDirtyFlag(true);
			}
			const FSnapshot Before(BP);
			for (int32 Shape = 0; Shape < 3; ++Shape)
			{
				TArray<TSharedPtr<FJsonValue>> Items = { Dispatcher(TEXT("Button Pressed")) };
				if (Shape == 1) Items.Insert(Dispatcher(TEXT("WouldBeAdded"), true), 0);
				if (Shape == 2) Items.Add(Dispatcher(TEXT("WouldBeAdded"), true));
				int32 Compiles = 0, Saves = 0;
				const auto CompileHandle = BP->OnCompiled().AddLambda([&](UBlueprint*) { ++Compiles; });
				const auto SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
				{ if (Object && Object->GetOutermost() == Fixture.Package) ++Saves; });
				const auto Result = Call(BP, Items, bStandalone, Shape == 0);
				BP->OnCompiled().Remove(CompileHandle);
				FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
				const FString Context = FString::Printf(TEXT("widget=%d standalone=%d dirty=%d shape=%d"), bWidget, bStandalone, bDirty, Shape);
				TestTrue(Context + TEXT(" deliberate name rejection returns JSON"), Result.IsValid() && !Result->GetBoolField(TEXT("success")));
				TestEqual(Context + TEXT(" rejection never compiles"), Compiles, 0);
				TestEqual(Context + TEXT(" rejection never saves"), Saves, 0);
				TestTrue(Context + TEXT(" complete state is unchanged"), Before.Matches(BP));
				TestNull(Context + TEXT(" no valid-prefix multicast member"), FindFProperty<FMulticastDelegateProperty>(BP->GeneratedClass, TEXT("WouldBeAdded")));
				TestEqual(Context + TEXT(" no valid-prefix declaration"), FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("WouldBeAdded")), INDEX_NONE);
				TestFalse(Context + TEXT(" no disk file"), IFileManager::Get().FileExists(*Fixture.FileName));
				if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP))
				{
					TArray<UWidget*> CurrentWidgets;
					WidgetBP->WidgetTree->GetAllWidgets(CurrentWidgets);
					TestTrue(Context + TEXT(" exact WidgetTree membership"), CurrentWidgets == WidgetsBefore);
					TestTrue(Context + TEXT(" button content identity"), Button->GetContent() == Label);
					TestEqual(Context + TEXT(" label text preserved"), Label->GetText().ToString(), FString(TEXT("Preserve this button label")));
				}
			}
		}

		int32 Saves = 0;
		const auto SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
		{ if (Object && Object->GetOutermost() == Fixture.Package) ++Saves; });
		const auto Result = Call(BP, { Dispatcher(TEXT("ButtonPressed"), true) }, bStandalone, true);
		FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
		if (!TestTrue(TEXT("valid canonical name survives renamed target"), Result.IsValid() && Result->GetBoolField(TEXT("success")))) return false;
		TestEqual(TEXT("public success did not save"), Saves, 0);
		FCompilerResultsLog Compiler;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Compiler);
		TestEqual(TEXT("renamed Widget/Actor dispatcher compiles cleanly"), Compiler.NumErrors, 0);
		int32 SignatureGraphs = 0;
		for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
		{
			if (Graph && Graph->GetFName() == TEXT("ButtonPressed"))
			{
				++SignatureGraphs;
				TestTrue(TEXT("registered signature graph belongs to renamed Blueprint"), Graph->GetOuter() == BP);
			}
		}
		TestEqual(TEXT("one same-name signature graph"), SignatureGraphs, 1);
		for (UClass* Class : { BP->SkeletonGeneratedClass.Get(), BP->GeneratedClass.Get() })
		{
			FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(Class, TEXT("ButtonPressed"));
			if (!TestTrue(TEXT("non-null reflected signature"), Property && Property->SignatureFunction)) return false;
			UFunction* Signature = Property->SignatureFunction;
			TestTrue(TEXT("signature belongs to its reflected class"), Signature->GetOuter() == Class);
			TestNotNull(TEXT("boolean payload reflection"), FindFProperty<FBoolProperty>(Signature, TEXT("Ready")));
			FObjectPropertyBase* Context = FindFProperty<FObjectPropertyBase>(Signature, TEXT("Context"));
			TestTrue(TEXT("Actor payload identity"), Context && Context->PropertyClass == AActor::StaticClass());
		}
		const auto ReadCommand = MakeShared<FJsonObject>();
		ReadCommand->SetStringField(TEXT("assetPath"), BP->GetPathName());
		const FString ReadResponse = FNwiroBlueprintTools::ReadBlueprint(Serialize(ReadCommand));
		TestTrue(TEXT("public read remains responsive and exposes dispatcher"), ReadResponse.Contains(TEXT("ButtonPressed")));
		TestFalse(TEXT("successful proof remains unsaved"), IFileManager::Get().FileExists(*Fixture.FileName));
	}
	return true;
}
