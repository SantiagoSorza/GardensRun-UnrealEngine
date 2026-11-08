// Copyright 2026 Nwiro. All Rights Reserved.
// Exercise real BlueprintCompilationManager saving policy and package disk state.
#include "BlueprintEditorSettings.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectSaveContext.h"

namespace NwiroVerificationCompileSpec
{
    static TSharedPtr<FJsonObject> Parse(const FString& Text)
    {
        TSharedPtr<FJsonObject> Result;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
        return Result;
    }

    static bool CheckSavePolicy(FAutomationTestBase& Test, ESaveOnCompile Policy, bool bExisting)
    {
        // Plain -unattended cancels PromptForCheckoutAndSave before saving. The
        // engine's scoped script mode permits actual owned-fixture saves without
        // dialogs, so both the verification and positive control exercise policy.
        TGuardValue<bool> ScriptGuard(GIsRunningUnattendedScript, true);
        UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
        const auto OriginalPolicy = Settings->SaveOnCompile;
        Settings->SaveOnCompile = SoC_Never;
        ON_SCOPE_EXIT { Settings->SaveOnCompile = OriginalPolicy; };

        const FString Name = TEXT("BP_Verification_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString PackagePath = TEXT("/Game/NwiroAutomation/Verification/") + Name;
        const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
        // This GUID fixture is owned by this invocation; never recycle an existing asset.
        if (FindPackage(nullptr, *PackagePath) || IFileManager::Get().FileExists(*Filename))
        {
            Test.AddError(TEXT("Refusing to touch an existing verification fixture"));
            return false;
        }
        UPackage* Package = CreatePackage(*PackagePath);
        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
            FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
            TEXT("NwiroVerificationSavePolicy"));
        Test.TestNotNull(TEXT("Creates an owned package-backed Blueprint"), BP);
        if (!BP) return false;
        FAssetRegistryModule::AssetCreated(BP);
        ON_SCOPE_EXIT
        {
            Settings->SaveOnCompile = SoC_Never;
            // Delete only the package created above, after all disk assertions.
            UEditorAssetLibrary::DeleteAsset(PackagePath);
        };
        // Empty/data-only Blueprints can take a skeleton shortcut and never enter
        // the save queue. A real callable graph requires full function compilation.
        UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(BP,
            TEXT("VerificationCallable"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FunctionGraph, true, nullptr);
        Test.TestFalse(TEXT("Fixture is not a data-only Blueprint"), FBlueprintEditorUtils::IsDataOnlyBlueprint(BP));
        BP->BlueprintDescription = TEXT("Owned verification baseline");
        BP->MarkPackageDirty();
        FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
        TArray<uint8> BeforeBytes;
        const FDateTime SentinelTimestamp(2001, 1, 2, 3, 4, 5);
        if (bExisting)
        {
            const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(BP, false);
            Test.TestTrue(TEXT("Explicitly saves only the owned baseline fixture"), bSaved);
            if (!bSaved) return false;
            Test.TestFalse(TEXT("Saved baseline starts clean"), Package->IsDirty());
            Test.TestTrue(TEXT("Baseline bytes are readable"), FFileHelper::LoadFileToArray(BeforeBytes, *Filename));
            IFileManager::Get().SetTimeStamp(*Filename, SentinelTimestamp);
            Test.TestTrue(TEXT("Timestamp sentinel is installed"), IFileManager::Get().GetTimeStamp(*Filename) == SentinelTimestamp);
        }
        else Test.TestFalse(TEXT("New package has never existed on disk"), IFileManager::Get().FileExists(*Filename));

        BP->BlueprintDescription = TEXT("Unsaved user edit must survive verification");
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        BP->MarkPackageDirty();
        Test.TestTrue(TEXT("Fixture has unsaved changes before verification"), Package->IsDirty());
        Settings->SaveOnCompile = Policy;
        int32 CompileCount = 0;
        int32 SaveCount = 0;
        const FDelegateHandle CompileHandle = BP->OnCompiled().AddLambda([&CompileCount](UBlueprint*) { ++CompileCount; });
        const FDelegateHandle SaveHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
            [&SaveCount, Package](const FString&, UPackage* SavedPackage, FObjectPostSaveContext)
            { if (SavedPackage == Package) ++SaveCount; });
        const FString Response = FNwiroDebugTools::BPGetCompileErrors(
            FString::Printf(TEXT("{\"blueprint\":\"%s\"}"), *BP->GetPathName()));
        BP->OnCompiled().Remove(CompileHandle);
        UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
        Test.AddInfo(TEXT("Native verification response: ") + Response);
        const auto Result = Parse(Response);
        Test.TestTrue(TEXT("Verification returns JSON"), Result.IsValid());
        if (!Result.IsValid()) return false;
        Test.TestTrue(TEXT("Verification succeeds"), Result->GetBoolField(TEXT("success")));
        Test.TestEqual(TEXT("Verification evidence completed"), Result->GetStringField(TEXT("verificationStatus")), FString(TEXT("completed")));
        Test.TestTrue(TEXT("Verification invokes the actual compiler"), CompileCount > 0);
        Test.TestTrue(TEXT("Compile execution is reported"), Result->GetBoolField(TEXT("compilePerformed")));
        Test.TestNotNull(TEXT("Real function bytecode was compiled"), BP->GeneratedClass->FindFunctionByName(TEXT("VerificationCallable")));
        Test.TestEqual(TEXT("Successful fixture compiles without errors"), Result->GetIntegerField(TEXT("errorCount")), 0);
        Test.TestEqual(TEXT("Canonical object path is returned"), Result->GetStringField(TEXT("assetPath")), BP->GetPathName());
        Test.TestEqual(TEXT("No real package-save callback occurs"), SaveCount, 0);
        Test.TestFalse(TEXT("No save is reported"), Result->GetBoolField(TEXT("savePerformed")));
        Test.TestTrue(TEXT("Input package was dirty"), Result->GetBoolField(TEXT("packageDirtyBefore")));
        Test.TestTrue(TEXT("Observed package remains dirty"), Result->GetBoolField(TEXT("packageDirty")));
        Test.TestTrue(TEXT("Actual package remains dirty"), Package->IsDirty());
        Test.TestEqual(TEXT("Unsaved memory edit remains"), BP->BlueprintDescription, FString(TEXT("Unsaved user edit must survive verification")));
        Test.TestTrue(TEXT("User save policy remains selected"), Settings->SaveOnCompile == Policy);
        FGuid VerificationId;
        Test.TestTrue(TEXT("Result has a valid verification ID"), FGuid::Parse(Result->GetStringField(TEXT("verificationId")), VerificationId));
        FDateTime CompletedAt;
        Test.TestTrue(TEXT("Completion timestamp is ISO 8601"), FDateTime::ParseIso8601(*Result->GetStringField(TEXT("completedAt")), CompletedAt));
        if (bExisting)
        {
            TArray<uint8> AfterBytes;
            Test.TestTrue(TEXT("Existing .uasset remains readable"), FFileHelper::LoadFileToArray(AfterBytes, *Filename));
            Test.TestTrue(TEXT("Existing .uasset bytes are unchanged"), BeforeBytes == AfterBytes);
            Test.TestTrue(TEXT("Existing .uasset timestamp is unchanged"), IFileManager::Get().GetTimeStamp(*Filename) == SentinelTimestamp);
        }
        else Test.TestFalse(TEXT("Verification does not create a new .uasset"), IFileManager::Get().FileExists(*Filename));

        if (bExisting)
        {
            // Positive control: the same real editor policy MUST save this owned
            // dirty fixture without SkipSave. Otherwise the earlier no-save proof
            // could pass vacuously because this host never exercised SaveOnCompile.
            int32 ControlSaveCount = 0;
            const FDelegateHandle ControlHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
                [&ControlSaveCount, Package](const FString&, UPackage* SavedPackage, FObjectPostSaveContext)
                { if (SavedPackage == Package) ++ControlSaveCount; });
            FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None);
            UPackage::PackageSavedWithContextEvent.Remove(ControlHandle);
            Test.TestTrue(TEXT("Positive control proves SaveOnCompile really saves without SkipSave"), ControlSaveCount > 0);
            Test.TestFalse(TEXT("Positive-control save clears the owned package dirty flag"), Package->IsDirty());
            TArray<uint8> ControlBytes;
            Test.TestTrue(TEXT("Positive-control .uasset is readable"), FFileHelper::LoadFileToArray(ControlBytes, *Filename));
            Test.TestTrue(TEXT("Positive control persists the formerly unsaved edit"), ControlBytes != BeforeBytes);
            Test.AddInfo(FString::Printf(TEXT("Observed fixture saves: verification=%d, ordinary compile control=%d"), SaveCount, ControlSaveCount));
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroCompileAlwaysExistingSpec,
    "Nwiro.Pro.Verification.Compile.SkipSave.AlwaysExistingDirty",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroCompileAlwaysExistingSpec::RunTest(const FString& Parameters)
{ return NwiroVerificationCompileSpec::CheckSavePolicy(*this, SoC_Always, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroCompileSuccessExistingSpec,
    "Nwiro.Pro.Verification.Compile.SkipSave.SuccessOnlyExistingDirty",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroCompileSuccessExistingSpec::RunTest(const FString& Parameters)
{ return NwiroVerificationCompileSpec::CheckSavePolicy(*this, SoC_SuccessOnly, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroCompileAlwaysNewSpec,
    "Nwiro.Pro.Verification.Compile.SkipSave.AlwaysNewUnsaved",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroCompileAlwaysNewSpec::RunTest(const FString& Parameters)
{ return NwiroVerificationCompileSpec::CheckSavePolicy(*this, SoC_Always, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroCompileSuccessNewSpec,
    "Nwiro.Pro.Verification.Compile.SkipSave.SuccessOnlyNewUnsaved",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroCompileSuccessNewSpec::RunTest(const FString& Parameters)
{ return NwiroVerificationCompileSpec::CheckSavePolicy(*this, SoC_SuccessOnly, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroCompileUnknownSpec,
    "Nwiro.Pro.Verification.Compile.InvalidInputIsUnknown",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroCompileUnknownSpec::RunTest(const FString& Parameters)
{
    for (const FString Command : { FString(TEXT("not json")), FString(TEXT("{}")), FString(TEXT("{\"blueprint\":42}")) })
    {
        const auto Result = NwiroVerificationCompileSpec::Parse(FNwiroDebugTools::BPGetCompileErrors(Command));
        TestTrue(TEXT("Invalid input returns structured JSON"), Result.IsValid());
        if (!Result.IsValid()) return false;
        TestFalse(TEXT("Invalid input cannot report success"), Result->GetBoolField(TEXT("success")));
        TestFalse(TEXT("No compile was attempted"), Result->GetBoolField(TEXT("compilePerformed")));
        TestFalse(TEXT("No save was attempted"), Result->GetBoolField(TEXT("savePerformed")));
        TestEqual(TEXT("Invalid input is unknown"), Result->GetStringField(TEXT("verificationStatus")), FString(TEXT("unknown")));
        TestTrue(TEXT("Unknown errors are null"), Result->TryGetField(TEXT("errorCount"))->Type == EJson::Null);
    }
    return true;
}

#include "NwiroToolDispatch.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroCompileCapabilitySchemaSpec,
    "Nwiro.Pro.Verification.Compile.AdvertisedNoSaveCapability",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroCompileCapabilitySchemaSpec::RunTest(const FString& Parameters)
{
    // This is the actual serialization used by /tools and agent-facing tools/list.
    const FString DefinitionsJson = FNwiroToolDispatch::GetEnabledToolDefinitionsJson();
    TArray<TSharedPtr<FJsonValue>> Definitions;
    const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DefinitionsJson), Definitions);
    TestTrue(TEXT("Actual enabled tool definitions parse"), bParsed);
    if (!bParsed) return false;
    int32 Matched = 0;
    for (const auto& Value : Definitions)
    {
        const auto Tool = Value->AsObject();
        if (!Tool.IsValid() || Tool->GetStringField(TEXT("name")) != TEXT("bp_get_compile_errors")) continue;
        ++Matched;
        const auto Schema = Tool->GetObjectField(TEXT("inputSchema"));
        const TSharedPtr<FJsonObject>* Capability = nullptr;
        const bool bHasCapability = Schema->TryGetObjectField(TEXT("x-nwiro-verification"), Capability);
        TestTrue(TEXT("No-save capability is advertised before dispatch"), bHasCapability);
        if (!bHasCapability) continue;
        TestEqual(TEXT("Capability version is 1"), (*Capability)->GetIntegerField(TEXT("version")), 1);
        TestTrue(TEXT("Capability guarantees no save"), (*Capability)->GetBoolField(TEXT("noSave")));
        TestTrue(TEXT("Capability guarantees canonical result identity"), (*Capability)->GetBoolField(TEXT("canonicalTarget")));
        TestFalse(TEXT("Capability metadata is not a fake input parameter"), Schema->GetObjectField(TEXT("properties"))->HasField(TEXT("x-nwiro-verification")));
        TestEqual(TEXT("Existing blueprint input stays required"), Schema->GetArrayField(TEXT("required"))[0]->AsString(), FString(TEXT("blueprint")));
    }
    TestEqual(TEXT("Exactly one real compile-verification definition is advertised"), Matched, 1);
    return true;
}
