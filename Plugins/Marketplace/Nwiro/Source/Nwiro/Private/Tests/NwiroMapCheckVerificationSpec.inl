// Copyright 2026 Nwiro. All Rights Reserved.
// Real editor checks: log history and output formatting are not verification evidence.
#include "Engine/StaticMeshActor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace NwiroVerificationMapSpec
{
    static TSharedPtr<FJsonObject> Parse(const FString& Text)
    {
        TSharedPtr<FJsonObject> Result;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
        return Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMapCheckFreshEvidenceSpec,
    "Nwiro.Pro.Verification.MapCheck.CurrentInvocation",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroMapCheckFreshEvidenceSpec::RunTest(const FString& Parameters)
{
    using namespace NwiroVerificationMapSpec;
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false,
        FName(*FString::Printf(TEXT("NwiroMapCheck_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    TestNotNull(TEXT("Creates an owned isolated editor world"), World);
    if (!World) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    // First leave old errors in both the structured log and ordinary log history.
    // Expected automation suppression covers only this deliberately seeded message.
    const FString StaleMarker = TEXT("NwiroStaleMapCheckEvidence_58D1");
    AddExpectedError(StaleMarker, EAutomationExpectedErrorFlags::Contains, 1);
    FMessageLog(TEXT("MapCheck")).Error(FText::FromString(StaleMarker));
    const TSharedPtr<FJsonObject> Clean = Parse(NwiroVerificationMap::RunMapCheck(World));
    TestTrue(TEXT("A real clean check returns JSON"), Clean.IsValid());
    if (!Clean.IsValid()) return false;
    TestTrue(TEXT("The editor ran the check"), Clean->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Evidence collection completed"), Clean->GetStringField(TEXT("verificationStatus")), FString(TEXT("completed")));
    TestEqual(TEXT("The clean check excludes prior structured errors"), Clean->GetIntegerField(TEXT("errorCount")), 0);
    for (const auto& Item : Clean->GetArrayField(TEXT("messages")))
        TestFalse(TEXT("Prior output messages are excluded"), Item->AsObject()->GetStringField(TEXT("message")).Contains(StaleMarker));

    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
    TestNotNull(TEXT("Creates owned zero-scale error fixture"), Actor);
    if (!Actor) return false;
    Actor->SetActorScale3D(FVector::ZeroVector);
    World->GetOutermost()->SetDirtyFlag(true);

    TSharedPtr<FJsonObject> FailedMap;
    {
        // The old implementation parsed mirrored English text. Disable mirroring so
        // it would reuse the earlier clean summary even though the current check fails.
        FMessageLogScopedOverride Quiet(TEXT("MapCheck"));
        Quiet.SuppressLoggingToOutputLog();
        FailedMap = Parse(NwiroVerificationMap::RunMapCheck(World));
    }
    TestTrue(TEXT("Suppressed output still produces structured evidence"), FailedMap.IsValid());
    if (!FailedMap.IsValid()) return false;
    TestTrue(TEXT("success retains its tool-execution meaning"), FailedMap->GetBoolField(TEXT("success")));
    TestEqual(TEXT("An invalid map still has completed evidence"), FailedMap->GetStringField(TEXT("verificationStatus")), FString(TEXT("completed")));
    TestTrue(TEXT("Current zero-scale actor produces real engine errors"), FailedMap->GetIntegerField(TEXT("errorCount")) > 0);
    TestEqual(TEXT("Counts agree with the engine severity channel"), FailedMap->GetIntegerField(TEXT("errorCount")), FMessageLog(TEXT("MapCheck")).NumMessages(EMessageSeverity::Error));
    TestEqual(TEXT("No output messages were mirrored"), FailedMap->GetArrayField(TEXT("messages")).Num(), 0);
    TestTrue(TEXT("Omitted presentation messages are explicitly marked"), FailedMap->GetBoolField(TEXT("truncated")));
    TestEqual(TEXT("Canonical world target is returned"), FailedMap->GetStringField(TEXT("world")), World->GetPathName());
    TestFalse(TEXT("Map verification performs no save"), FailedMap->GetBoolField(TEXT("savePerformed")));
    TestTrue(TEXT("Dirty input state is observed"), FailedMap->GetBoolField(TEXT("packageDirtyBefore")));
    TestTrue(TEXT("World stays dirty"), FailedMap->GetBoolField(TEXT("packageDirty")));
    TestTrue(TEXT("Every invocation receives a distinct evidence ID"), Clean->GetStringField(TEXT("verificationId")) != FailedMap->GetStringField(TEXT("verificationId")));
    FDateTime CompletedAt;
    TestTrue(TEXT("Completion timestamp is ISO 8601"), FDateTime::ParseIso8601(*FailedMap->GetStringField(TEXT("completedAt")), CompletedAt));

    World->DestroyActor(Actor);
    const TSharedPtr<FJsonObject> Repaired = Parse(NwiroVerificationMap::RunMapCheck(World));
    TestTrue(TEXT("Repaired check returns JSON"), Repaired.IsValid());
    if (!Repaired.IsValid()) return false;
    TestEqual(TEXT("New clean run excludes the previous real failure"), Repaired->GetIntegerField(TEXT("errorCount")), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMapCheckUnknownSpec,
    "Nwiro.Pro.Verification.MapCheck.MissingWorldIsUnknown",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroMapCheckUnknownSpec::RunTest(const FString& Parameters)
{
    const auto Result = NwiroVerificationMapSpec::Parse(NwiroVerificationMap::RunMapCheck(nullptr));
    TestTrue(TEXT("Unavailable check returns structured JSON"), Result.IsValid());
    if (!Result.IsValid()) return false;
    TestFalse(TEXT("Unavailable check cannot report success"), Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Unavailable check is explicitly unknown"), Result->GetStringField(TEXT("verificationStatus")), FString(TEXT("unknown")));
    TestTrue(TEXT("Unknown errors are null, never a clean zero"), Result->TryGetField(TEXT("errorCount"))->Type == EJson::Null);
    TestTrue(TEXT("Unknown warnings are null"), Result->TryGetField(TEXT("warningCount"))->Type == EJson::Null);
    TestFalse(TEXT("No canonical target is invented"), Result->HasField(TEXT("world")));
    TestFalse(TEXT("Unknown result explains failure"), Result->GetStringField(TEXT("error")).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMapCheckBackendUnknownSpec,
    "Nwiro.Pro.Verification.MapCheck.MissingBackendIsUnknown",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FNwiroMapCheckBackendUnknownSpec::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TestNotNull(TEXT("Test host has an editor world"), World);
    if (!World) return false;
    TSharedPtr<FJsonObject> Result;
    {
        const auto PreviousBackend = FMessageLog::OnGetLog();
        ON_SCOPE_EXIT { FMessageLog::OnGetLog() = PreviousBackend; };
        FMessageLog::OnGetLog().Unbind();
        Result = NwiroVerificationMapSpec::Parse(NwiroVerificationMap::RunMapCheck(World));
    }
    TestTrue(TEXT("Unavailable backend returns structured JSON"), Result.IsValid());
    if (!Result.IsValid()) return false;
    TestFalse(TEXT("Unavailable backend cannot report a clean check"), Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Unavailable backend is unknown"), Result->GetStringField(TEXT("verificationStatus")), FString(TEXT("unknown")));
    TestTrue(TEXT("Unknown errors stay null"), Result->TryGetField(TEXT("errorCount"))->Type == EJson::Null);
    return true;
}
