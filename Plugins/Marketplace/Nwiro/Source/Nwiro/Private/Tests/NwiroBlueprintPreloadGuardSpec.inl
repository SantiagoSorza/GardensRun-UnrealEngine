// Copyright 2026 Nwiro. All Rights Reserved.
// Included by the owning product's BlueprintTools.cpp at the root's serial boundary.
#if WITH_DEV_AUTOMATION_TESTS
#include "NwiroBlueprintPreloadGuard.h"
#include "NwiroAssetGuard.h"
#include "NwiroAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintCore.h"
#include "BlueprintEditorSettings.h"
#include "GameFramework/Pawn.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace NwiroPreloadTests
{
	using namespace NwiroBlueprintPreload;
	struct FNeverSaveOnCompile
	{
		UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
		TEnumAsByte<ESaveOnCompile> Previous = Settings->SaveOnCompile;
		FNeverSaveOnCompile() { Settings->SaveOnCompile = SoC_Never; }
		~FNeverSaveOnCompile() { Settings->SaveOnCompile = Previous; }
	};

	static FAssetData MakeMetadata(FAssetDataTagMap Tags, const FString& Name = TEXT("Candidate"),
		FTopLevelAssetPath AssetClass = UBlueprint::StaticClass()->GetClassPathName())
	{
		const FString Folder = TEXT("/Game/NwiroPreloadMetadata_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FAssetData(FName(*(Folder + TEXT("/") + Name)), FName(*Folder), FName(*Name), AssetClass, MoveTemp(Tags));
	}

	static FAssetDataTagMap Tags(const TCHAR* Type = TEXT("BPType_Normal"),
		const TCHAR* Generated = TEXT("None"), const UClass* Parent = AActor::StaticClass())
	{
		FAssetDataTagMap Result;
		Result.Add(TEXT("BlueprintType"), Type);
		Result.Add(TEXT("GeneratedClass"), Generated);
		Result.Add(TEXT("NativeParentClass"), FString::Printf(TEXT("Class'%s'"), *Parent->GetPathName()));
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPreloadMetadataSpec,
	"Nwiro.Pro.Blueprint.Preload.Metadata", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPreloadMetadataSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroPreloadTests;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	for (const TCHAR* Name : {TEXT("OrdinaryAsset"), TEXT("OtherFixture"), TEXT("ThirdControl")})
	{
		const FAssetData Asset = MakeMetadata(Tags(), Name);
		FString Path; Asset.AppendObjectPath(Path);
		TestNull(TEXT("metadata fixture is not loaded before inspection"), FindObject<UObject>(nullptr, *Path));
		TestTrue(TEXT("normal Actor explicit None rejected"), InspectRegistryCandidate(Asset, Registry).Disposition == EDisposition::Rejected);
		FString Rejection;
		TestNull(TEXT("real registry loader rejects before GetAsset"), NwiroSafeRegistryLoad(Asset, &Rejection));
		TestEqual(TEXT("loader reports the metadata rejection"), Rejection, FString(TEXT("blueprint_generated_class_explicit_none")));
		TestNull(TEXT("inspection did not produce an object"), FindObject<UObject>(nullptr, *Path));
		TestNull(TEXT("inspection did not produce a package"), FindPackage(nullptr, *Asset.PackageName.ToString()));
	}
	TestTrue(TEXT("native Actor ancestry includes Pawn"), InspectDiskMetadata(MakeMetadata(Tags(TEXT("BPTYPE_Normal"), TEXT("None"), APawn::StaticClass())), Registry).Disposition == EDisposition::Rejected);
	for (const TCHAR* Type : {TEXT("BPTYPE_Const"), TEXT("BPTYPE_Interface"), TEXT("BPTYPE_LevelScript"), TEXT("BPTYPE_MacroLibrary"), TEXT("BPTYPE_FunctionLibrary")})
	{
		TestTrue(FString::Printf(TEXT("construction-script exemption: %s"), Type), InspectDiskMetadata(MakeMetadata(Tags(Type)), Registry).Disposition == EDisposition::NotFlagged);
	}
	TestTrue(TEXT("normal non-Actor is outside this assertion predicate"), InspectDiskMetadata(MakeMetadata(Tags(TEXT("BPTYPE_Normal"), TEXT("None"), UObject::StaticClass())), Registry).Disposition == EDisposition::NotFlagged);
	TestTrue(TEXT("class-name substring does not establish Blueprint inheritance"), InspectDiskMetadata(MakeMetadata(Tags(), TEXT("NonBlueprintControl"), UBlueprintCore::StaticClass()->GetClassPathName()), Registry).Disposition == EDisposition::NotFlagged);
	for (const TCHAR* Tag : {TEXT("GeneratedClass"), TEXT("BlueprintType"), TEXT("NativeParentClass")})
	{
		FAssetDataTagMap Missing = Tags(); Missing.Remove(Tag);
		const FAssetData MissingAsset = MakeMetadata(MoveTemp(Missing));
		FString MissingValue;
		TestFalse(FString::Printf(TEXT("fixture really omits %s"), Tag), MissingAsset.GetTagValue(Tag, MissingValue));
		TestTrue(FString::Printf(TEXT("missing %s remains unknown"), Tag), InspectDiskMetadata(MissingAsset, Registry).Disposition == EDisposition::MetadataUnknown);
	}
	// FAssetData rejects/removes empty tag values. Its representable unknown
	// state is an absent tag, covered above; do not construct an invalid fixture.
	for (const TCHAR* Generated : {TEXT("none"), TEXT("None "), TEXT("not-an-object-path")})
	{
		TestTrue(FString::Printf(TEXT("malformed tag '%s' is not literal None"), Generated), InspectDiskMetadata(MakeMetadata(Tags(TEXT("BPTYPE_Normal"), Generated)), Registry).Disposition == EDisposition::MetadataUnknown);
	}
	TestTrue(TEXT("recorded generated-class path does not force a class load"), InspectDiskMetadata(MakeMetadata(Tags(TEXT("BPTYPE_Normal"), TEXT("BlueprintGeneratedClass'/Game/NotLoaded/Probe.Probe_C'"))), Registry).Disposition == EDisposition::NotFlagged);
	FAssetDataTagMap UnknownType = Tags(); UnknownType.Add(TEXT("BlueprintType"), TEXT("not-an-enum-value"));
	TestTrue(TEXT("unknown type remains unknown"), InspectDiskMetadata(MakeMetadata(MoveTemp(UnknownType)), Registry).Disposition == EDisposition::MetadataUnknown);
	FAssetDataTagMap OversizedType = Tags(); OversizedType.Add(TEXT("BlueprintType"), FString::ChrN(NAME_SIZE * 2, TEXT('X')));
	TestTrue(TEXT("oversized enum tag rejected before FName lookup"), InspectDiskMetadata(MakeMetadata(MoveTemp(OversizedType)), Registry).Disposition == EDisposition::MetadataUnknown);
	FAssetDataTagMap UnresolvedParent = Tags(); UnresolvedParent.Add(TEXT("NativeParentClass"), TEXT("Class'/Script/NwiroAbsentModule.NoClass'"));
	TestTrue(TEXT("unresolved parent remains unknown without LoadClass"), InspectDiskMetadata(MakeMetadata(MoveTemp(UnresolvedParent)), Registry).Disposition == EDisposition::MetadataUnknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPreloadPathsSpec,
	"Nwiro.Pro.Blueprint.Preload.Paths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPreloadPathsSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroBlueprintPreload;
	FString Normalized;
	for (const TCHAR* Path : {TEXT("/Game/Folder/Probe"), TEXT("/Game/Folder/Probe.Probe"), TEXT("Blueprint'/Game/Folder/Probe.Probe'")})
	{
		TestTrue(TEXT("supported exact spelling normalizes"), NormalizeExactAssetPath(Path, Normalized));
		TestEqual(TEXT("exact spellings share identity"), Normalized, FString(TEXT("/Game/Folder/Probe.Probe")));
	}
	for (const TCHAR* Path : {TEXT(""), TEXT("ShortName"), TEXT("/Game/Bad Name"), TEXT("/Game/Folder/Probe.Probe:Component"), TEXT("/Game/Folder/Probe\n"), TEXT("/Game\\Folder\\Probe")})
	{
		TestFalse(TEXT("invalid or non-exact input does not reach name construction"), NormalizeExactAssetPath(Path, Normalized));
	}
	TestFalse(TEXT("oversized input rejected before name construction"), NormalizeExactAssetPath(TEXT("/Game/") + FString::ChrN(NAME_SIZE * 2, TEXT('X')), Normalized));
	EBlueprintType Type = BPTYPE_MAX;
	TestTrue(TEXT("actual engine tag spelling parses"), TryBlueprintType(TEXT("BPType_Normal"), Type));
	TestEqual(TEXT("normal enum value"), static_cast<int32>(Type), static_cast<int32>(BPTYPE_Normal));
	TestTrue(TEXT("namespaced native enum spelling parses"), TryBlueprintType(TEXT("EBlueprintType::BPTYPE_Interface"), Type));
	TestEqual(TEXT("interface enum value"), static_cast<int32>(Type), static_cast<int32>(BPTYPE_Interface));
	TestFalse(TEXT("MAX sentinel is not a Blueprint category"), TryBlueprintType(TEXT("BPTYPE_MAX"), Type));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPreloadLoadedSpec,
	"Nwiro.Pro.Blueprint.Preload.LoadedPrecedence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPreloadLoadedSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroPreloadTests;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FString PackageName = TEXT("/Game/NwiroPreloadLoaded_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/LoadedFixture");
	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("owned in-memory fixture package"), Package)) return false;
	FNeverSaveOnCompile NoAutomaticSaves;
	UBlueprint* Blueprint = nullptr;
	UClass* OriginalGeneratedClass = nullptr;
	ON_SCOPE_EXIT
	{
		if (Blueprint) Blueprint->GeneratedClass = OriginalGeneratedClass;
		Package->SetDirtyFlag(false);
		ForEachObjectWithPackage(Package, [](UObject* Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			return true;
		}, true);
		Package->ClearFlags(RF_Public | RF_Standalone);
		Package->MarkAsGarbage();
	};
	Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
		TEXT("LoadedFixture"), BPTYPE_Normal, TEXT("NwiroPreloadGuardTest"));
	if (!TestNotNull(TEXT("factory-created loaded Blueprint"), Blueprint)) return false;
	OriginalGeneratedClass = Blueprint->GeneratedClass.Get();
	if (!TestNotNull(TEXT("factory produced a generated class"), OriginalGeneratedClass)) return false;
	FAssetData Stale(FName(*PackageName), FName(*FPackageName::GetLongPackagePath(PackageName)),
		Blueprint->GetFName(), UBlueprint::StaticClass()->GetClassPathName(), Tags());
	const FInspection Candidate = InspectRegistryCandidate(Stale, Registry);
	TestTrue(TEXT("healthy loaded state overrides stale disk None"), Candidate.Disposition == EDisposition::AlreadyLoaded);
	TestEqual(TEXT("exact existing object returned"), Candidate.ExistingObject, static_cast<UObject*>(Blueprint));
	FString Normalized;
	const FInspection Exact = InspectExactAssetPath(Blueprint->GetPathName(), Registry, Normalized);
	TestTrue(TEXT("exact path also uses loaded state"), Exact.Disposition == EDisposition::AlreadyLoaded);
	TestEqual(TEXT("exact normalized path preserved"), Normalized, Blueprint->GetPathName());
	FString LoadError;
	TestEqual(TEXT("real registry loader preserves healthy loaded precedence"), NwiroSafeRegistryLoad(Stale, &LoadError), static_cast<UObject*>(Blueprint));
	TestTrue(TEXT("healthy loaded precedence has no rejection"), LoadError.IsEmpty());
	Package->SetDirtyFlag(false);
	const auto ParseResponse = [this](const FString& Text)
	{
		TSharedPtr<FJsonObject> Result;
		TestTrue(TEXT("public response is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result) && Result.IsValid());
		return Result;
	};
	const TSharedPtr<FJsonObject> Healthy = ParseResponse(FNwiroBlueprintTools::ReadBlueprint(Blueprint->GetPathName()));
	if (!Healthy.IsValid()) return false;
	TestEqual(TEXT("public read accepts the exact healthy loaded Blueprint"), Healthy->GetStringField(TEXT("path")), Blueprint->GetPathName());
	TestTrue(TEXT("healthy loaded Blueprint remains openable"), Healthy->GetBoolField(TEXT("openable")));
	TestFalse(TEXT("healthy read leaves package clean"), Package->IsDirty());
	Blueprint->GeneratedClass = nullptr;
	TestTrue(TEXT("known invalid loaded state rejects before any load"), InspectRegistryCandidate(Stale, Registry).Disposition == EDisposition::Rejected);
	for (const FString& Path : {PackageName, Blueprint->GetPathName(), TEXT("Blueprint'") + Blueprint->GetPathName() + TEXT("'")})
	{
		TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("path"), Path);
		Command->SetNumberField(TEXT("depth"), 0);
		FString Json; FJsonSerializer::Serialize(Command, TJsonWriterFactory<>::Create(&Json));
		for (const FString& Response : {FNwiroBlueprintTools::ReadBlueprint(Path), FNwiroAssetTools::ReadAsset(Json), FNwiroAssetTools::GetAssetThumbnail(Json)})
		{
			const TSharedPtr<FJsonObject> Result = ParseResponse(Response);
			if (!Result.IsValid()) return false;
			TestFalse(TEXT("public path refuses invalid loaded Blueprint"), Result->GetBoolField(TEXT("success")));
			TestEqual(TEXT("public rejection survives each exact alias"), Result->GetStringField(TEXT("errorCode")), FString(TEXT("loaded_blueprint_missing_generated_class")));
			TestNull(TEXT("rejection does not create a generated class"), Blueprint->GeneratedClass.Get());
			TestFalse(TEXT("rejection does not dirty the package"), Package->IsDirty());
		}
	}
	Blueprint->GeneratedClass = OriginalGeneratedClass;
	TestTrue(TEXT("loaded fixture restored"), InspectRegistryCandidate(Stale, Registry).Disposition == EDisposition::AlreadyLoaded);
	return true;
}
#endif
