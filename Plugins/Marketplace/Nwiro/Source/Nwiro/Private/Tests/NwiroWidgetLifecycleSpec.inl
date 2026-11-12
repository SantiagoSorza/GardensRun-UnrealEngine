// Copyright 2026 Nwiro. All Rights Reserved.
// Included in WidgetTools.cpp to exercise the production creation helper without
// inheriting create_widget_blueprint's pre-existing disk save. All other edits
// call the public native functions; every fixture belongs to a fresh GUID package.
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

namespace NwiroWidgetLifecycleProof
{
	static FString Json(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Out));
		return Out;
	}

	static TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	struct FFixture
	{
		UPackage* Package = nullptr;
		TStrongObjectPtr<UWidgetBlueprint> BP;
		FString FileName;
		FCompilerResultsLog CompileLog;

		explicit FFixture(UClass* RootClass)
		{
			const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Name = TEXT("WBP_WidgetLifecycle_") + Guid;
			const FString Path = TEXT("/Game/NwiroWidgetLifecycleAcceptance_") + Guid + TEXT("/") + Name;
			Package = CreatePackage(*Path);
			FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
			BP.Reset(CreateWidgetBlueprintAsset(Package, FName(*Name), RootClass, CompileLog));
			if (BP.IsValid()) FAssetRegistryModule::AssetCreated(BP.Get());
		}

		~FFixture()
		{
			if (BP.IsValid()) FAssetRegistryModule::AssetDeleted(BP.Get());
			BP.Reset();
			if (Package)
			{
				// No disk deletion, global GC or transaction reset. A failing file
				// absence assertion leaves any unexpected file as diagnostic evidence.
				ForEachObjectWithPackage(Package, [](UObject* Object)
				{
					Object->ClearFlags(RF_Public | RF_Standalone);
					Object->MarkAsGarbage();
					return true;
				}, true);
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}
	};

	static TSharedPtr<FJsonObject> Add(UWidgetBlueprint* BP, const FString& Class,
		const FString& Name, const FString& Parent)
	{
		const auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Request->SetStringField(TEXT("widgetClass"), Class);
		Request->SetStringField(TEXT("name"), Name);
		Request->SetStringField(TEXT("parent"), Parent);
		return Parse(FNwiroWidgetTools::AddWidget(Json(Request)));
	}

	static TSharedPtr<FJsonObject> Set(UWidgetBlueprint* BP, const FString& Widget,
		const FString& Property, const FString& Value)
	{
		const auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Request->SetStringField(TEXT("widget"), Widget);
		Request->SetStringField(TEXT("property"), Property);
		Request->SetStringField(TEXT("value"), Value);
		return Parse(FNwiroWidgetTools::SetWidgetProperty(Json(Request)));
	}

	static bool Succeeded(const TSharedPtr<FJsonObject>& Result)
	{
		bool bSuccess = false;
		return Result.IsValid() && Result->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroWidgetLifecycleProof,
	"Nwiro.Pro.Widget.Production.CreateAddSetCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroWidgetLifecycleProof::RunTest(const FString& Parameters)
{
	using namespace NwiroWidgetLifecycleProof;
	for (UClass* RootClass : { UCanvasPanel::StaticClass(), UVerticalBox::StaticClass() })
	{
		FFixture Fixture(RootClass);
		UWidgetBlueprint* BP = Fixture.BP.Get();
		if (!TestNotNull(TEXT("production creation helper returns a widget Blueprint"), BP)) return false;
		TestEqual(TEXT("initial compile has no errors"), Fixture.CompileLog.NumErrors, 0);
		TestTrue(TEXT("new Blueprint is load-completed"), BP->HasAnyFlags(RF_LoadCompleted));
		TestTrue(TEXT("engine marks new Blueprint regenerated"), BP->bHasBeenRegenerated != 0);
		if (!TestNotNull(TEXT("generated class exists"), BP->GeneratedClass.Get())) return false;
		if (!TestNotNull(TEXT("skeleton class exists"), BP->SkeletonGeneratedClass.Get())) return false;
		TestTrue(TEXT("skeleton and generated class are distinct"), BP->GeneratedClass != BP->SkeletonGeneratedClass);
		TestTrue(TEXT("initial compiler status is successful"), BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
		TestFalse(TEXT("creation helper did not save"), IFileManager::Get().FileExists(*Fixture.FileName));

		if (!TestTrue(TEXT("public add panel"), Succeeded(Add(BP, TEXT("VerticalBox"), TEXT("Panel"), TEXT("RootPanel"))))) return false;
		if (!TestTrue(TEXT("public add label"), Succeeded(Add(BP, TEXT("TextBlock"), TEXT("Label"), TEXT("Panel"))))) return false;
		if (!TestTrue(TEXT("public add bar"), Succeeded(Add(BP, TEXT("ProgressBar"), TEXT("Bar"), TEXT("Panel"))))) return false;
		for (const TCHAR* Name : { TEXT("Panel"), TEXT("Label"), TEXT("Bar") })
		{
			TestNotNull(TEXT("structural edit immediately updates skeleton members"),
				FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, Name));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
			TestTrue(TEXT("each source widget has a valid stable GUID"), BP->WidgetVariableNameToGuidMap.FindRef(Name).IsValid());
#endif
		}
		if (!TestTrue(TEXT("public set label"), Succeeded(Set(BP, TEXT("Label"), TEXT("Text"), TEXT("Smoke"))))) return false;
		if (!TestTrue(TEXT("public set bar"), Succeeded(Set(BP, TEXT("Bar"), TEXT("Percent"), TEXT("0.37"))))) return false;
		UTextBlock* Label = Cast<UTextBlock>(BP->WidgetTree->FindWidget(TEXT("Label")));
		UProgressBar* Bar = Cast<UProgressBar>(BP->WidgetTree->FindWidget(TEXT("Bar")));
		if (!TestNotNull(TEXT("source label exists"), Label) || !TestNotNull(TEXT("source bar exists"), Bar)) return false;
		TestEqual(TEXT("source text reflects write"), Label->GetText().ToString(), FString(TEXT("Smoke")));
		TestTrue(TEXT("source percent reflects write"), FMath::IsNearlyEqual(Bar->GetPercent(), 0.37f));
		TestTrue(TEXT("edits leave package dirty"), Fixture.Package->IsDirty());
		TestEqual(TEXT("property edits leave compile pending"), BP->Status.GetValue(), BS_Dirty);
		TestFalse(TEXT("public add/set did not save"), IFileManager::Get().FileExists(*Fixture.FileName));

		// The reported crash was delayed until the first later compile. Recompile
		// twice in the same process and verify generated templates, not tool replies.
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			FCompilerResultsLog Log;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Log);
			TestEqual(TEXT("later compile has no errors"), Log.NumErrors, 0);
			TestTrue(TEXT("later compiler status is successful"), BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
			UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(BP->GeneratedClass);
			if (!TestNotNull(TEXT("later widget generated class exists"), Generated)) return false;
			TestNotNull(TEXT("later generated class has its actual CDO"), Generated->GetDefaultObject(false));
			UWidgetTree* TemplateTree = Generated->GetWidgetTreeArchetype();
			if (!TestNotNull(TEXT("compiled class contains widget templates"), TemplateTree)) return false;
			UTextBlock* CompiledLabel = Cast<UTextBlock>(TemplateTree->FindWidget(TEXT("Label")));
			UProgressBar* CompiledBar = Cast<UProgressBar>(TemplateTree->FindWidget(TEXT("Bar")));
			if (!TestNotNull(TEXT("compiled label exists"), CompiledLabel) || !TestNotNull(TEXT("compiled bar exists"), CompiledBar)) return false;
			TestEqual(TEXT("compiled text preserves source default"), CompiledLabel->GetText().ToString(), FString(TEXT("Smoke")));
			TestTrue(TEXT("compiled percent preserves source default"), FMath::IsNearlyEqual(CompiledBar->GetPercent(), 0.37f));
		}
		TestFalse(TEXT("explicit SkipSave compilation did not save"), IFileManager::Get().FileExists(*Fixture.FileName));

		TArray<UWidget*> Before;
		BP->WidgetTree->GetAllWidgets(Before);
		UClass* SkeletonBefore = BP->SkeletonGeneratedClass;
		const EBlueprintStatus StatusBefore = BP->Status;
		Fixture.Package->SetDirtyFlag(false);
		TestFalse(TEXT("duplicate widget name is rejected"), Succeeded(Add(BP, TEXT("Button"), TEXT("Label"), TEXT("Panel"))));
		TestFalse(TEXT("unknown parent is rejected"), Succeeded(Add(BP, TEXT("Button"), TEXT("MissingParentChild"), TEXT("Missing"))));
		TestFalse(TEXT("leaf parent is rejected"), Succeeded(Add(BP, TEXT("Button"), TEXT("LeafChild"), TEXT("Label"))));
		TArray<UWidget*> After;
		BP->WidgetTree->GetAllWidgets(After);
		TestTrue(TEXT("rejected adds preserve exact tree object identities"), Before == After);
		TestTrue(TEXT("rejected adds preserve skeleton identity"), SkeletonBefore == BP->SkeletonGeneratedClass);
		TestEqual(TEXT("rejected adds do not compile"), BP->Status.GetValue(), StatusBefore);
		TestFalse(TEXT("rejected adds preserve a clean package"), Fixture.Package->IsDirty());
	}
	return true;
}
