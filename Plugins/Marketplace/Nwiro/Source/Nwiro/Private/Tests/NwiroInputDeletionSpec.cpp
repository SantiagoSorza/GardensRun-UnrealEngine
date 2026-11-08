// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroInputTools.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Json.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroInputDeletionExact,
	"Nwiro.Pro.Input.Delete.ExactFolderNameAndFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroInputDeletionExact::RunTest(const FString& Parameters)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);
	const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = TEXT("/Game/NwiroInputDeletionProof/") + Unique;
	TArray<TWeakObjectPtr<UObject>> Owned;
	auto Add = [&](const FString& Folder, const FString& Name, UClass* Class) -> FString
	{
		UObject* Asset = NewObject<UObject>(CreatePackage(*(Folder + TEXT("/") + Name)), Class, *Name, RF_Public | RF_Standalone);
		Owned.Add(Asset); FAssetRegistryModule::AssetCreated(Asset); Asset->GetPackage()->SetDirtyFlag(false);
		return Asset->GetPathName();
	};
	auto Call = [&](const FString& Name, const FString& Path, bool Expected) -> TSharedPtr<FJsonObject>
	{
		auto Request = MakeShared<FJsonObject>();
		if (!Name.IsEmpty()) Request->SetStringField(TEXT("name"), Name);
		if (!Path.IsEmpty()) Request->SetStringField(TEXT("path"), Path);
		FString Json; FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Json));
		TSharedPtr<FJsonObject> Reply;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroInputTools::DeleteInputAction(Json)), Reply);
		bool Success = false;
		TestTrue(TEXT("structured deletion response"), Reply.IsValid() && Reply->TryGetBoolField(TEXT("success"), Success));
		TestEqual(TEXT("expected deletion outcome"), Success, Expected);
		return Reply;
	};
	const FString Name = TEXT("IA_Proof_") + Unique;
	const FString A = Add(Root + TEXT("/A"), Name, UInputAction::StaticClass());
	const FString B = Add(Root + TEXT("/B"), Name, UInputAction::StaticClass());
	const FString Longer = Add(Root, Name + TEXT("_Longer"), UInputAction::StaticClass());
	const FString WrongType = Add(Root, TEXT("M_Protected"), UMaterial::StaticClass());
	Call(Name, TEXT(""), false); // exact duplicate names must not choose whichever registry result arrives first
	Call(Name.LeftChop(4), TEXT(""), false); // destructive lookup never accepts substring matches
	Call(TEXT("OtherName"), A, false);
	Call(TEXT(""), WrongType, false);
	Call(TEXT(""), Root + TEXT("/A"), false);
	Call(Name, Root + TEXT("/Missing"), false);
	for (const auto& Object : Owned)
	{
		TestTrue(TEXT("rejected requests retain every owned asset"), Object.IsValid() && Registry.GetAssetByObjectPath(FSoftObjectPath(Object->GetPathName())).IsValid());
		if (Object.IsValid()) TestFalse(TEXT("rejected requests preserve clean packages"), Object->GetPackage()->IsDirty());
	}
	auto Reply = Call(Name, Root + TEXT("/A"), true);
	TestTrue(TEXT("folder plus name reports canonical deleted object path"), Reply.IsValid() && Reply->GetStringField(TEXT("deleted")) == A);
	TestFalse(TEXT("selected asset removed from registry"), Registry.GetAssetByObjectPath(FSoftObjectPath(A)).IsValid());
	TestTrue(TEXT("same name in other folder retained"), Registry.GetAssetByObjectPath(FSoftObjectPath(B)).IsValid());
	TestTrue(TEXT("substring neighbor retained"), Registry.GetAssetByObjectPath(FSoftObjectPath(Longer)).IsValid());
	Call(TEXT(""), B.Left(B.Find(TEXT("."))), true); // full package path
	const FString Context = Add(Root, TEXT("IMC_Proof"), UInputMappingContext::StaticClass());
	Call(TEXT(""), Context, true); // preserve existing context deletion compatibility
	Call(TEXT(""), Context, false);
	// Only fixture-owned objects are cleaned; no source files are written by fixture setup.
	for (const auto& Object : Owned)
	{
		if (!Object.IsValid()) continue;
		FAssetRegistryModule::AssetDeleted(Object.Get());
		Object->GetPackage()->SetDirtyFlag(false);
		Object->ClearFlags(RF_Public | RF_Standalone);
		Object->MarkAsGarbage();
	}
	return true;
}
#endif
