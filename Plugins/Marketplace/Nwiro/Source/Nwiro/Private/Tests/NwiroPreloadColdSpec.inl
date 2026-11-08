// Copyright 2026 Nwiro. All Rights Reserved.
// Included by BlueprintTools.cpp. Explicit owned fresh-process acceptance only.
// Three different fresh processes; root's outer Windows Job owns their lifetime.
#if WITH_DEV_AUTOMATION_TESTS
#include "NwiroBlueprintTools.h"
#include "NwiroAssetTools.h"
#include "NwiroBlueprintPreloadGuard.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "BlueprintEditorSettings.h"
#include "Misc/OutputDeviceRedirector.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectSaveContext.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace NwiroPreloadCold
{
	static constexpr const TCHAR* Product = TEXT("Pro");
	struct FNeverSaveOnCompile
	{
		UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
		TEnumAsByte<ESaveOnCompile> Previous = Settings->SaveOnCompile;
		FNeverSaveOnCompile() { Settings->SaveOnCompile = SoC_Never; }
		~FNeverSaveOnCompile() { Settings->SaveOnCompile = Previous; }
	};
	struct FContext
	{
		FString Run, Root, Evidence;
		bool Initialize(FAutomationTestBase& Test, const TCHAR* RequiredMode)
		{
			FString Mode;
			FParse::Value(FCommandLine::Get(), TEXT("NwiroPreloadColdRun="), Run);
			FParse::Value(FCommandLine::Get(), TEXT("NwiroPreloadColdMode="), Mode);
			bool bRunValid = Run.Len() == 32;
			for (TCHAR Ch : Run) bRunValid &= FChar::IsHexDigit(Ch);
			if (!Test.TestTrue(TEXT("explicit unique 32-hex run and exact phase required"), bRunValid && Mode == RequiredMode)) return false;
#if PLATFORM_WINDOWS
			BOOL bInJob = 0;
			if (!Test.TestTrue(TEXT("native process must actually belong to a Windows Job"), ::IsProcessInJob(::GetCurrentProcess(), nullptr, &bInJob) != 0 && bInJob)) return false;
#else
			Test.AddError(TEXT("This deliberately invalid fixture requires the root-owned Windows containment harness"));
			return false;
#endif
			Root = TEXT("/Game/NwiroPreloadCold/") + Run + TEXT("/") + Product + TEXT("/");
			Evidence = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NwiroPreloadCold") / Run / Product);
			TArray<FString> PhaseReceipts;
			if (Mode == TEXT("writer")) PhaseReceipts = {TEXT("writer.json"), TEXT("guarded-reader.json"), TEXT("bypass-armed.json"), TEXT("bypass-returned.json")};
			else if (Mode == TEXT("guarded")) PhaseReceipts = {TEXT("guarded-reader.json")};
			else PhaseReceipts = {TEXT("bypass-armed.json"), TEXT("bypass-returned.json")};
			for (const FString& File : PhaseReceipts)
			{
				if (!Test.TestFalse(TEXT("each phase requires fresh evidence paths"), IFileManager::Get().FileExists(*(Evidence / File)))) return false;
			}
			return true;
		}
		FString Package(const TCHAR* Name) const { return Root + Name; }
		FString Object(const TCHAR* Name) const { return Package(Name) + TEXT(".") + Name; }
		FString File(const TCHAR* Name) const { return FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Package(Name), FPackageName::GetAssetPackageExtension())); }
	};
	struct FSaveObserver
	{
		int32 Attempts = 0;
		FDelegateHandle Handle;
		FSaveObserver() { Handle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([this](UObject*, FObjectPreSaveContext) { ++Attempts; }); }
		~FSaveObserver() { FCoreUObjectDelegates::OnObjectPreSave.Remove(Handle); }
	};
	static FString Hash(const FString& File)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.IsEmpty()) return FString();
		uint8 Digest[FSHA1::DigestSize]; FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
		return BytesToHex(Digest, FSHA1::DigestSize);
	}
	static bool Receipt(const FContext& Context, const TCHAR* File, const TSharedRef<FJsonObject>& Object)
	{
		if (IFileManager::Get().FileExists(*(Context.Evidence / File))) return false;
		Object->SetStringField(TEXT("run"), Context.Run);
		Object->SetStringField(TEXT("product"), Product);
		Object->SetStringField(TEXT("source_event_at"), FDateTime::UtcNow().ToIso8601());
		Object->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
		FString Text; FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
		IFileManager::Get().MakeDirectory(*Context.Evidence, true);
		return FFileHelper::SaveStringToFile(Text, *(Context.Evidence / File), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_NoReplaceExisting);
	}
	static bool SaveOwned(FAutomationTestBase& Test, const FContext& Context, UBlueprint* Blueprint, const TCHAR* Name)
	{
		if (!Test.TestTrue(TEXT("save is exactly the new owned package"), Blueprint && Blueprint->GetPathName() == Context.Object(Name)
			&& !IFileManager::Get().FileExists(*Context.File(Name)))) return false;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Context.File(Name)), true);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		return Test.TestTrue(TEXT("explicit owned package save completed"), UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Context.File(Name), Args)
			&& !Hash(Context.File(Name)).IsEmpty());
	}
	static bool ColdMetadata(FAutomationTestBase& Test, const FContext& Context, FString& BadHash)
	{
		for (const TCHAR* Name : {TEXT("Healthy"), TEXT("ExplicitNone")})
		{
			if (!Test.TestNull(TEXT("fresh reader has not loaded fixture package"), FindPackage(nullptr, *Context.Package(Name)))) return false;
		}
		FString Text; TSharedPtr<FJsonObject> Writer;
		if (IFileManager::Get().FileSize(*(Context.Evidence / TEXT("writer.json"))) <= 0
			|| IFileManager::Get().FileSize(*(Context.Evidence / TEXT("writer.json"))) > 64 * 1024
			|| !FFileHelper::LoadFileToString(Text, *(Context.Evidence / TEXT("writer.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Writer) || !Writer.IsValid()
			|| Writer->GetStringField(TEXT("run")) != Context.Run || Writer->GetStringField(TEXT("product")) != Product
			|| Writer->GetStringField(TEXT("status")) != TEXT("SAVED_FIXTURES_READER_PROOF_PENDING")
			|| Writer->GetStringField(TEXT("healthy_path")) != Context.Object(TEXT("Healthy"))
			|| Writer->GetStringField(TEXT("invalid_path")) != Context.Object(TEXT("ExplicitNone"))
			|| Writer->GetNumberField(TEXT("pid")) == FPlatformProcess::GetCurrentProcessId())
		{
			Test.AddError(TEXT("exact successful writer receipt required")); return false;
		}
		BadHash = Hash(Context.File(TEXT("ExplicitNone")));
		if (!Test.TestTrue(TEXT("both cold package hashes match explicit writer bytes"), !BadHash.IsEmpty()
			&& BadHash == Writer->GetStringField(TEXT("invalid_sha1")) && Hash(Context.File(TEXT("Healthy"))) == Writer->GetStringField(TEXT("healthy_sha1")))) return false;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.ScanFilesSynchronous({Context.File(TEXT("Healthy")), Context.File(TEXT("ExplicitNone"))}, true);
		const FAssetData Bad = Registry.GetAssetByObjectPath(FSoftObjectPath(Context.Object(TEXT("ExplicitNone"))), true);
		FString Generated, Type, Parent;
		const bool bTags = Bad.GetTagValue(TEXT("GeneratedClass"), Generated) && Bad.GetTagValue(TEXT("BlueprintType"), Type)
			&& Bad.GetTagValue(TEXT("NativeParentClass"), Parent);
		EBlueprintType ParsedType = BPTYPE_MAX;
		if (!Test.TestTrue(TEXT("actual saved metadata is UBlueprint normal Actor with literal None"), Bad.IsValid() && bTags
			&& Bad.AssetClassPath == UBlueprint::StaticClass()->GetClassPathName() && Generated == TEXT("None")
			&& NwiroBlueprintPreload::TryBlueprintType(Type, ParsedType) && ParsedType == BPTYPE_Normal
			&& FPackageName::ExportTextPathToObjectPath(Parent) == AActor::StaticClass()->GetPathName())) return false;
		for (const TCHAR* Name : {TEXT("Healthy"), TEXT("ExplicitNone")})
		{
			if (!Test.TestNull(TEXT("disk registry scan did not load fixture package"), FindPackage(nullptr, *Context.Package(Name)))) return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPreloadColdWriter,
	"Nwiro.Pro.Acceptance.PreloadCold.WriteFixture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPreloadColdWriter::RunTest(const FString& Parameters)
{
	using namespace NwiroPreloadCold;
	FContext Context; if (!Context.Initialize(*this, TEXT("writer"))) return false;
	FNeverSaveOnCompile NoAutomaticSaves;
	for (const TCHAR* Name : {TEXT("Healthy"), TEXT("ExplicitNone")})
	{
		if (!TestTrue(TEXT("GUID fixture path is wholly new"), !FindPackage(nullptr, *Context.Package(Name))
			&& !IFileManager::Get().FileExists(*Context.File(Name)))) return false;
	}
	UPackage* HealthyPackage = CreatePackage(*Context.Package(TEXT("Healthy")));
	UBlueprint* Healthy = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), HealthyPackage, TEXT("Healthy"), BPTYPE_Normal, TEXT("NwiroPreloadCold"));
	if (!TestTrue(TEXT("factory healthy baseline"), Healthy && Healthy->GeneratedClass && Healthy->SimpleConstructionScript)) return false;
	if (!SaveOwned(*this, Context, Healthy, TEXT("Healthy"))) return false;
	// Deliberate invalid Blueprint state, valid engine package serialization.
	// No generated-class export exists that could incidentally regenerate this object.
	UPackage* BadPackage = CreatePackage(*Context.Package(TEXT("ExplicitNone")));
	UBlueprint* Bad = NewObject<UBlueprint>(BadPackage, TEXT("ExplicitNone"), RF_Public | RF_Standalone | RF_Transactional);
	Bad->ParentClass = AActor::StaticClass(); Bad->BlueprintType = BPTYPE_Normal;
	Bad->BlueprintSystemVersion = UBlueprint::GetCurrentBlueprintSystemVersion();
	if (!TestTrue(TEXT("exact pre-save assertion prerequisites"), !Bad->GeneratedClass && !Bad->SkeletonGeneratedClass && !Bad->SimpleConstructionScript)) return false;
	if (!SaveOwned(*this, Context, Bad, TEXT("ExplicitNone"))) return false;
	if (!TestTrue(TEXT("save did not repair or compile fixture"), !Bad->GeneratedClass && !Bad->SkeletonGeneratedClass && !Bad->SimpleConstructionScript)) return false;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("SAVED_FIXTURES_READER_PROOF_PENDING"));
	Result->SetStringField(TEXT("healthy_sha1"), Hash(Context.File(TEXT("Healthy"))));
	Result->SetStringField(TEXT("invalid_sha1"), Hash(Context.File(TEXT("ExplicitNone"))));
	Result->SetStringField(TEXT("healthy_path"), Context.Object(TEXT("Healthy")));
	Result->SetStringField(TEXT("invalid_path"), Context.Object(TEXT("ExplicitNone")));
	return TestTrue(TEXT("writer receipt saved"), Receipt(Context, TEXT("writer.json"), Result));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPreloadColdGuarded,
	"Nwiro.Pro.Acceptance.PreloadCold.GuardedReader", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPreloadColdGuarded::RunTest(const FString& Parameters)
{
	using namespace NwiroPreloadCold;
	FContext Context; if (!Context.Initialize(*this, TEXT("guarded"))) return false;
	FSaveObserver SaveObserver;
	FString BadHash; if (!ColdMetadata(*this, Context, BadHash)) return false;
	TSharedPtr<FJsonObject> Healthy;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroBlueprintTools::ReadBlueprint(Context.Object(TEXT("Healthy")))), Healthy)
		|| !Healthy.IsValid() || !TestTrue(TEXT("healthy public read uses success schema"), !Healthy->HasField(TEXT("error")) && !Healthy->HasField(TEXT("errorCode"))
			&& Healthy->GetStringField(TEXT("name")) == TEXT("Healthy") && Healthy->GetStringField(TEXT("path")) == Context.Object(TEXT("Healthy")))) return false;
	UBlueprint* LoadedHealthy = FindObject<UBlueprint>(nullptr, *Context.Object(TEXT("Healthy")));
	if (!TestTrue(TEXT("healthy public read actually loaded the expected usable Blueprint"), LoadedHealthy
		&& LoadedHealthy->GeneratedClass && LoadedHealthy->SimpleConstructionScript && LoadedHealthy->ParentClass == AActor::StaticClass())) return false;
	int32 Calls = 0;
	for (const FString& Path : {Context.Package(TEXT("ExplicitNone")), Context.Object(TEXT("ExplicitNone")), TEXT("Blueprint'") + Context.Object(TEXT("ExplicitNone")) + TEXT("'")})
	{
		TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>(); Command->SetStringField(TEXT("path"), Path);
		FString Json; FJsonSerializer::Serialize(Command, TJsonWriterFactory<>::Create(&Json));
		for (const FString& Response : {FNwiroBlueprintTools::ReadBlueprint(Path), FNwiroAssetTools::ReadAsset(Json), FNwiroAssetTools::GetAssetThumbnail(Json)})
		{
			TSharedPtr<FJsonObject> Result;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Result) || !Result.IsValid()) return false;
			if (!TestTrue(TEXT("public reader refuses actual disk metadata before load"), !Result->GetBoolField(TEXT("success"))
				&& Result->GetStringField(TEXT("errorCode")) == TEXT("blueprint_generated_class_explicit_none")
				&& Result->GetStringField(TEXT("path")) == Context.Object(TEXT("ExplicitNone")))) return false;
			if (!TestNull(TEXT("rejected package remains entirely unloaded"), FindPackage(nullptr, *Context.Package(TEXT("ExplicitNone"))))) return false;
			if (!TestEqual(TEXT("rejected package bytes unchanged"), Hash(Context.File(TEXT("ExplicitNone"))), BadHash)) return false;
			++Calls;
		}
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("GUARDED_PUBLIC_READS_PASS")); Result->SetNumberField(TEXT("rejections"), Calls);
	if (!TestEqual(TEXT("reader observed no save attempts"), SaveObserver.Attempts, 0)) return false;
	Result->SetNumberField(TEXT("observed_object_presave_events"), SaveObserver.Attempts); Result->SetStringField(TEXT("invalid_sha1"), BadHash);
	return TestTrue(TEXT("guarded reader stayed responsive through final receipt"), Receipt(Context, TEXT("guarded-reader.json"), Result));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPreloadColdBypass,
	"Nwiro.Pro.Acceptance.PreloadCold.UnguardedControl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPreloadColdBypass::RunTest(const FString& Parameters)
{
	using namespace NwiroPreloadCold;
	FContext Context; if (!Context.Initialize(*this, TEXT("bypass"))) return false;
	FString BadHash; if (!ColdMetadata(*this, Context, BadHash)) return false;
	if (!TestNotNull(TEXT("healthy cold loading works in this same control worker"), LoadObject<UBlueprint>(nullptr, *Context.Object(TEXT("Healthy"))))) return false;
	TSharedRef<FJsonObject> Armed = MakeShared<FJsonObject>();
	Armed->SetStringField(TEXT("status"), TEXT("ARMED_DIRECT_LOAD_CONTROL")); Armed->SetStringField(TEXT("invalid_sha1"), BadHash);
	if (!Receipt(Context, TEXT("bypass-armed.json"), Armed)) return false;
	UE_LOG(LogTemp, Display, TEXT("NWIRO_PRELOAD_CONTROL_ARMED run=%s product=%s pid=%u"), *Context.Run, Product, FPlatformProcess::GetCurrentProcessId());
	if (GLog) GLog->Flush();
	// The production guard is bypassed only by this explicit contained test call.
	// No runtime override or canonical source mutation is introduced.
	UBlueprint* Unexpected = LoadObject<UBlueprint>(nullptr, *Context.Object(TEXT("ExplicitNone")));
	TSharedRef<FJsonObject> Returned = MakeShared<FJsonObject>(); Returned->SetStringField(TEXT("status"), TEXT("CONTROL_DID_NOT_REPRODUCE"));
	Returned->SetBoolField(TEXT("returned_object"), Unexpected != nullptr); Receipt(Context, TEXT("bypass-returned.json"), Returned);
	AddError(TEXT("Direct loader returned: this fixture is not an accepted assertion control on this engine/build"));
	return false;
}
#endif
