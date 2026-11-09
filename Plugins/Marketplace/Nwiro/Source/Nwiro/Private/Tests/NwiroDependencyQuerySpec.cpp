// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroDebugTools.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "HAL/FileManager.h"
#include "Json.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace NwiroDependencyQueryProof
{
	using FTool = FString (*)(const FString&);

	static TSharedPtr<FJsonObject> Call(FAutomationTestBase& Test, FTool Tool, const TSharedRef<FJsonObject>& Request)
	{
		FString Input;
		FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Input));
		const FString Output = Tool(Input);
		TSharedPtr<FJsonObject> Result;
		Test.TestTrue(TEXT("tool returned a JSON object"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Output), Result) && Result.IsValid());
		return Result;
	}

	static TSharedRef<FJsonObject> Request(const FString& Path, const TCHAR* Key = TEXT("path"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(Key, Path);
		return Result;
	}

	static bool Success(const TSharedPtr<FJsonObject>& Result)
	{
		bool Value = false;
		return Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Value) && Value;
	}

	static FString Json(const TSharedPtr<FJsonObject>& Result)
	{
		FString Out;
		if (Result.IsValid()) FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
		return Out;
	}

	static bool Contains(const TSharedPtr<FJsonObject>& Result, const TCHAR* Key, FName Package)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(Key, Values)) return false;
		for (const auto& Value : *Values) if (Value->AsString() == Package.ToString()) return true;
		return false;
	}

	static TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Node, FName Package)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(TEXT("dependencies"), Values)) return nullptr;
		for (const auto& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			FString Asset;
			if (Object.IsValid() && Object->TryGetStringField(TEXT("asset"), Asset) && Asset == Package.ToString()) return Object;
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> Tree(const TSharedPtr<FJsonObject>& Result)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Result.IsValid() && Result->TryGetObjectField(TEXT("tree"), Value) ? *Value : nullptr;
	}

	static void Rejected(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result)
	{
		Test.TestFalse(TEXT("invalid/unavailable query fails"), Success(Result));
		if (!Result.IsValid()) return;
		FString Error;
		Test.TestTrue(TEXT("failure explains the problem"), Result->TryGetStringField(TEXT("error"), Error) && !Error.IsEmpty());
		Test.TestFalse(TEXT("failure does not claim zero results"), Result->HasField(TEXT("count")));
		Test.TestFalse(TEXT("failure does not return a successful-looking dependency tree"), Result->HasField(TEXT("tree")));
	}

	struct FFixture
	{
		FString Root = TEXT("/Game/NwiroDependencyQueryProof/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<TStrongObjectPtr<UPrimaryAssetLabel>> Assets;
		TArray<FString> Files;
		TArray<TArray<uint8>> Bytes;

		UPrimaryAssetLabel* Add(const TCHAR* Name)
		{
			UPackage* Package = CreatePackage(*(Root + TEXT("/") + Name));
			UPrimaryAssetLabel* Asset = NewObject<UPrimaryAssetLabel>(Package, Name, RF_Public | RF_Standalone);
			Asset->bIsRuntimeLabel = true;
			Asset->bLabelAssetsInMyDirectory = false;
			Assets.Emplace(Asset);
			FAssetRegistryModule::AssetCreated(Asset);
			return Asset;
		}

		bool SaveAll(FAutomationTestBase& Test)
		{
			Files.Reset();
			Bytes.Reset();
			for (const auto& Asset : Assets)
			{
				UPackage* Package = Asset->GetOutermost();
				const FString File = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
				Files.Add(File);
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags = SAVE_NoError;
				const bool bSaved = UPackage::SavePackage(Package, Asset.Get(), *File, Args);
				if (!Test.TestTrue(TEXT("owned dependency fixture saved"), bSaved)) return false;
				TArray<uint8> Content;
				if (!Test.TestTrue(TEXT("owned fixture bytes captured"), FFileHelper::LoadFileToArray(Content, *File))) return false;
				Bytes.Add(MoveTemp(Content));
			}
			// Fixture setup explicitly populates disk dependency data; production queries never initiate scans.
			AR.SearchAllAssets(true);
			AR.ScanFilesSynchronous(Files, true);
			return Test.TestFalse(TEXT("fixture registry discovery completed"), AR.IsLoadingAssets());
		}

		void Unchanged(FAutomationTestBase& Test, bool bDirty)
		{
			for (int32 I = 0; I < Files.Num(); ++I)
			{
				TArray<uint8> Now;
				Test.TestTrue(TEXT("fixture remains readable"), FFileHelper::LoadFileToArray(Now, *Files[I]));
				Test.TestTrue(TEXT("query preserves saved bytes"), Now == Bytes[I]);
				Test.TestEqual(TEXT("query preserves preexisting dirty state"), Assets[I]->GetOutermost()->IsDirty(), bDirty);
			}
		}

		~FFixture()
		{
			for (const auto& Asset : Assets)
			{
				FAssetRegistryModule::AssetDeleted(Asset.Get());
				UPackage* Package = Asset->GetOutermost();
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			// Each filename was derived from this GUID-owned root. No directory recursion or unrelated deletion.
			for (const FString& File : Files) IFileManager::Get().Delete(*File, false, true);
		}
	};

	struct FObserved
	{
		int32 Loads = 0, Saves = 0;
		FDelegateHandle Load, Save;
		FObserved()
		{
			Load = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([this](UObject*) { ++Loads; });
			Save = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([this](UObject*, FObjectPreSaveContext) { ++Saves; });
		}
		~FObserved()
		{
			FCoreUObjectDelegates::OnAssetLoaded.Remove(Load);
			FCoreUObjectDelegates::OnObjectPreSave.Remove(Save);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroDependencyQueryPaths,
	"Nwiro.Pro.Debug.Dependencies.ExactPathsAndFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroDependencyQueryPaths::RunTest(const FString& Parameters)
{
	using namespace NwiroDependencyQueryProof;
	FFixture Fixture;
	UPrimaryAssetLabel* A = Fixture.Add(TEXT("A"));
	UPrimaryAssetLabel* B = Fixture.Add(TEXT("B"));
	UPrimaryAssetLabel* C = Fixture.Add(TEXT("C"));
	UPrimaryAssetLabel* Unused = Fixture.Add(TEXT("Unused"));
	A->ExplicitAssets.Add(TSoftObjectPtr<UObject>(B));
	B->ExplicitAssets.Add(TSoftObjectPtr<UObject>(C));
	if (!Fixture.SaveAll(*this)) return false;
	const FTool Tools[] = { &FNwiroDebugTools::GetAssetReferences, &FNwiroDebugTools::GetAssetReferencers, &FNwiroDebugTools::GetDependencyTree };
	FObserved Observed;
	for (const bool bDirty : { false, true })
	{
		for (const auto& Asset : Fixture.Assets) Asset->GetOutermost()->SetDirtyFlag(bDirty);
		for (const FTool Tool : Tools)
		{
			const auto PackageResult = Call(*this, Tool, Request(B->GetOutermost()->GetName()));
			const auto ObjectResult = Call(*this, Tool, Request(B->GetPathName()));
			const auto AliasResult = Call(*this, Tool, Request(B->GetPathName(), TEXT("asset")));
			TestTrue(TEXT("registered package succeeds"), Success(PackageResult));
			TestEqual(TEXT("object/package paths yield identical data"), Json(PackageResult), Json(ObjectResult));
			TestEqual(TEXT("legacy asset alias yields identical data"), Json(ObjectResult), Json(AliasResult));
			for (const FString& Path : { FString(), FString(TEXT("B")), Fixture.Root, Fixture.Root + TEXT("/Missing"),
				B->GetOutermost()->GetName() + TEXT(".Missing"), B->GetPathName() + TEXT(":SubObject"),
				FString(TEXT(" /Game/B")), FString(TEXT("/Game/../B")), FString(TEXT("/Game/B.B\"broken")) })
				Rejected(*this, Call(*this, Tool, Request(Path)));
			auto WrongType = MakeShared<FJsonObject>(); WrongType->SetNumberField(TEXT("path"), 1);
			Rejected(*this, Call(*this, Tool, WrongType));
		}
		const auto Dependencies = Call(*this, Tools[0], Request(B->GetPathName()));
		TestTrue(TEXT("real soft dependency is preserved"), Contains(Dependencies, TEXT("dependencies"), C->GetOutermost()->GetFName()));
		const auto Referencers = Call(*this, Tools[1], Request(B->GetPathName()));
		TestTrue(TEXT("real reverse dependency is preserved"), Contains(Referencers, TEXT("referencers"), A->GetOutermost()->GetFName()));
		const auto Empty = Call(*this, Tools[1], Request(Unused->GetPathName()));
		TestTrue(TEXT("known package with no referencers succeeds"), Success(Empty));
		if (Empty.IsValid()) TestEqual(TEXT("valid empty result remains zero"), Empty->GetIntegerField(TEXT("count")), 0);
		const auto Orphans = Call(*this, &FNwiroDebugTools::FindOrphanAssets, Request(Fixture.Root));
		TestTrue(TEXT("orphan query retains its valid folder contract"), Success(Orphans));
		Fixture.Unchanged(*this, bDirty);
	}
	UPrimaryAssetLabel* Unsaved = Fixture.Add(TEXT("Unsaved"));
	TArray<FName> BaselineDependencies;
	const bool bHasDependencyRecord = Fixture.AR.GetDependencies(Unsaved->GetOutermost()->GetFName(), BaselineDependencies);
	TestFalse(TEXT("unsaved fixture has no disk dependency record"), bHasDependencyRecord);
	if (!bHasDependencyRecord)
	{
		for (const FTool Tool : Tools) Rejected(*this, Call(*this, Tool, Request(Unsaved->GetPathName())));
	}
	Rejected(*this, Call(*this, &FNwiroDebugTools::FindOrphanAssets, Request(B->GetPathName())));
	Rejected(*this, Call(*this, &FNwiroDebugTools::FindOrphanAssets, Request(B->GetOutermost()->GetName())));
	TestEqual(TEXT("queries load no assets"), Observed.Loads, 0);
	TestEqual(TEXT("queries save no assets"), Observed.Saves, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroDependencyQueryTree,
	"Nwiro.Pro.Debug.Dependencies.RecursiveTraversal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroDependencyQueryTree::RunTest(const FString& Parameters)
{
	using namespace NwiroDependencyQueryProof;
	FFixture Fixture;
	UPrimaryAssetLabel* A = Fixture.Add(TEXT("A"));
	UPrimaryAssetLabel* B = Fixture.Add(TEXT("B"));
	UPrimaryAssetLabel* C = Fixture.Add(TEXT("C"));
	A->ExplicitAssets.Add(TSoftObjectPtr<UObject>(B));
	B->ExplicitAssets.Add(TSoftObjectPtr<UObject>(C));
	C->ExplicitAssets.Add(TSoftObjectPtr<UObject>(A));
	if (!Fixture.SaveAll(*this)) return false;
	const FTool Tool = &FNwiroDebugTools::GetDependencyTree;
	FObserved Observed;
	auto Deep = Request(A->GetPathName()); Deep->SetNumberField(TEXT("depth"), 4);
	const auto Result = Call(*this, Tool, Deep);
	TestTrue(TEXT("cycle fixture query succeeds"), Success(Result));
	const auto Root = Tree(Result);
	const auto BNode = Child(Root, B->GetOutermost()->GetFName());
	const auto CNode = Child(BNode, C->GetOutermost()->GetFName());
	const auto Cycle = Child(CNode, A->GetOutermost()->GetFName());
	TestTrue(TEXT("real recursive A to B to C path retained"), CNode.IsValid());
	bool Circular = false;
	TestTrue(TEXT("cycle terminates at ancestor"), Cycle.IsValid() && Cycle->TryGetBoolField(TEXT("circular"), Circular) && Circular);
	auto Shallow = Request(A->GetPathName()); Shallow->SetNumberField(TEXT("depth"), 1);
	const auto Limit = Child(Tree(Call(*this, Tool, Shallow)), B->GetOutermost()->GetFName());
	bool Limited = false;
	TestTrue(TEXT("unexpanded depth boundary is explicit"), Limit.IsValid() && Limit->TryGetBoolField(TEXT("depthLimitReached"), Limited) && Limited);
	if (Limit.IsValid()) TestFalse(TEXT("unexpanded node does not claim empty dependencies"), Limit->HasField(TEXT("dependencies")));
	for (const double Depth : { -1.0, 1.5, 65.0 })
	{
		auto Invalid = Request(A->GetPathName()); Invalid->SetNumberField(TEXT("depth"), Depth);
		Rejected(*this, Call(*this, Tool, Invalid));
	}
	Fixture.Unchanged(*this, false);
	TestEqual(TEXT("tree loads no assets"), Observed.Loads, 0);
	TestEqual(TEXT("tree saves no assets"), Observed.Saves, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroDependencyQueryMissingRecord,
	"Nwiro.Pro.Debug.Dependencies.MissingRecursivePackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroDependencyQueryMissingRecord::RunTest(const FString& Parameters)
{
	using namespace NwiroDependencyQueryProof;
	FFixture Fixture;
	UPrimaryAssetLabel* A = Fixture.Add(TEXT("A"));
	const FString MissingPackage = Fixture.Root + TEXT("/Missing");
	A->ExplicitAssets.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(MissingPackage + TEXT(".Missing"))));
	if (!Fixture.SaveAll(*this)) return false;
	TestFalse(TEXT("missing child has no saved package record"), Fixture.AR.GetAssetPackageDataCopy(FName(*MissingPackage)).IsSet());
	FObserved Observed;
	const auto References = Call(*this, &FNwiroDebugTools::GetAssetReferences, Request(A->GetPathName()));
	TestTrue(TEXT("existing parent still reports its broken reference"), Success(References));
	TestTrue(TEXT("broken reference is not silently omitted"), Contains(References, TEXT("dependencies"), FName(*MissingPackage)));
	auto Deep = Request(A->GetPathName()); Deep->SetNumberField(TEXT("depth"), 2);
	Rejected(*this, Call(*this, &FNwiroDebugTools::GetDependencyTree, Deep));
	Fixture.Unchanged(*this, false);
	TestEqual(TEXT("missing dependency query loads no assets"), Observed.Loads, 0);
	TestEqual(TEXT("missing dependency query saves no assets"), Observed.Saves, 0);
	return !HasAnyErrors();
}
#endif
