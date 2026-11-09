#pragma once

#include "NwiroEnhancedInputContract.h"
#include "InputAction.h"
#include "K2Node_EnhancedInputAction.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Runtime/Launch/Resources/Version.h"

namespace NwiroEnhancedInputContract
{
static bool ReadyActionObject(const UObject* Object)
{
    if (!Object) return false;
    EInternalObjectFlags NotReady = EInternalObjectFlags::Async | EInternalObjectFlags::AsyncLoadingPhase1
        | EInternalObjectFlags::AsyncLoadingPhase2 | EInternalObjectFlags::PendingConstruction;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
    NotReady |= EInternalObjectFlags::Remote;
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
    NotReady |= EInternalObjectFlags::LocalNotReady;
#endif
    return !Object->HasAnyInternalFlags(NotReady) && IsValid(Object)
        && !Object->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_WillBeLoaded
            | RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed);
}

struct FPreparedActions
{
    // Strong roots survive any target skeleton refresh/GC in this synchronous call.
    TArray<TStrongObjectPtr<UInputAction>> Roots;
    TMap<FString, UInputAction*> ByRequestedPath;
    TMap<FString, FString> CanonicalByRequestedPath;
    FObjectProperty* ActionProperty = nullptr;

    UInputAction* FindExact(const FString& Requested) const
    {
        UInputAction* const* Found = ByRequestedPath.Find(Requested);
        const FString* Canonical = CanonicalByRequestedPath.Find(Requested);
        return Found && ReadyActionObject(*Found) && Canonical && (*Found)->GetPathName().Equals(*Canonical, ESearchCase::CaseSensitive)
            ? *Found : nullptr;
    }
};

static bool CanonicalActionPath(const FString& Requested, FString& Canonical, FString& Error)
{
    Canonical.Reset();
    if (Requested != Requested.TrimStartAndEnd() || !Requested.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
        || Requested.Contains(TEXT(":")))
    {
        Error = TEXT("Use an exact /Game InputAction object or package path; bare names, fuzzy matches and subobjects are not accepted");
        return false;
    }
    const FString Package = FPackageName::ObjectPathToPackageName(Requested);
    if (!FPackageName::IsValidLongPackageName(Package))
    {
        Error = TEXT("The InputAction package path is invalid");
        return false;
    }
    Canonical = Requested.Contains(TEXT(".")) ? Requested
        : Requested + TEXT(".") + FPackageName::GetLongPackageAssetName(Requested);
    if (!FPackageName::IsValidObjectPath(Canonical))
    {
        Error = TEXT("The InputAction object path is invalid");
        Canonical.Reset();
        return false;
    }
    return true;
}

static FString AssetRefusal(const TArray<TSharedPtr<FJsonValue>>& Errors)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("code"), TEXT("ENHANCED_INPUT_ASSET_REJECTED"));
    Result->SetStringField(TEXT("error"), TEXT("EnhancedInputAction asset admission failed before target load/create or mutation"));
    Result->SetArrayField(TEXT("errors"), Errors);
    Result->SetBoolField(TEXT("currentRequestMutation"), false);
    Result->SetBoolField(TEXT("targetLoadRequested"), false);
    Result->SetBoolField(TEXT("targetMutationStarted"), false);
    Result->SetBoolField(TEXT("compiled"), false);
    Result->SetBoolField(TEXT("saveAttempted"), false);
    FString Out;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    if (!FJsonSerializer::Serialize(Result, Writer))
        return TEXT("{\"success\":false,\"code\":\"ENHANCED_INPUT_ASSET_REJECTED\",\"currentRequestMutation\":false,\"targetLoadRequested\":false,\"targetMutationStarted\":false,\"compiled\":false,\"saveAttempted\":false}");
    return Out;
}

static FString PrepareActions(const TArray<FRecipeItem>& Recipes, FPreparedActions& Out)
{
    if (Recipes.IsEmpty()) return FString();
    TArray<TSharedPtr<FJsonValue>> Errors;
    if (!IsInGameThread())
    {
        Issue(Errors, TEXT("add_nodes"), INDEX_NONE, TEXT("action"), TEXT("GAME_THREAD_REQUIRED"),
            TEXT("Enhanced Input admission must run on the editor game thread"));
        return AssetRefusal(Errors);
    }
    struct FCandidate
    {
        FRecipeItem Recipe;
        FString Requested;
        FString Canonical;
    };
    TArray<FCandidate> Candidates;
    // All shape/canonical path checks finish before any object lookup.
    for (const FRecipeItem& Recipe : Recipes)
    {
        FCandidate Candidate;
        Candidate.Recipe = Recipe;
        Candidate.Requested = Recipe.Object->GetStringField(TEXT("action"));
        FString Why;
        if (!CanonicalActionPath(Candidate.Requested, Candidate.Canonical, Why))
            Issue(Errors, Recipe.Container, Recipe.Unit, TEXT("action"), TEXT("CANONICAL_ACTION_PATH_REQUIRED"), Why);
        else Candidates.Add(MoveTemp(Candidate));
    }
    if (!Errors.IsEmpty()) return AssetRefusal(Errors);

    UClass* const ActionClass = UInputAction::StaticClass();
    Out.ActionProperty = CastField<FObjectProperty>(UK2Node_EnhancedInputAction::StaticClass()->FindPropertyByName(FName(TEXT("InputAction"))));
    if (!Out.ActionProperty || Out.ActionProperty->PropertyClass != ActionClass)
    {
        Issue(Errors, TEXT("add_nodes"), INDEX_NONE, TEXT("action"), TEXT("INPUT_ACTION_PROPERTY_UNAVAILABLE"),
            TEXT("The native event node's exact InputAction property is unavailable or has an unexpected type"));
        return AssetRefusal(Errors);
    }
    // This admission never loads assets, scans the registry or resolves redirects.
    // A cold/missing asset is explicitly unavailable, not proof of nonexistence.
    for (const FCandidate& Candidate : Candidates)
    {
        const FRecipeItem& Recipe = Candidate.Recipe;
        UObject* Existing = StaticFindObjectSafe(UObject::StaticClass(), FSoftObjectPath(Candidate.Canonical).GetAssetPath(), false);
        if (!Existing)
        {
            Issue(Errors, Recipe.Container, Recipe.Unit, TEXT("action"), TEXT("ACTION_NOT_LOADED"),
                TEXT("The exact InputAction is not already loaded. Load or create this exact asset separately, then retry; admission performs no loading or fallback search."));
            continue;
        }
        if (!ReadyActionObject(Existing))
        {
            Issue(Errors, Recipe.Container, Recipe.Unit, TEXT("action"), TEXT("INPUT_ACTION_NOT_READY"),
                TEXT("The exact object is not fully initialized on the game thread; no target work is allowed"));
            continue;
        }
        UInputAction* Action = Cast<UInputAction>(Existing);
        if (!Action)
        {
            Issue(Errors, Recipe.Container, Recipe.Unit, TEXT("action"), TEXT("NOT_AN_INPUT_ACTION"),
                TEXT("The exact loaded object is not an InputAction; no alternate object is selected"));
            continue;
        }
        if (!Action->GetPathName().Equals(Candidate.Canonical, ESearchCase::CaseSensitive))
        {
            Issue(Errors, Recipe.Container, Recipe.Unit, TEXT("action"), TEXT("CANONICAL_ACTION_IDENTITY_REQUIRED"),
                TEXT("The loaded object does not match the exact canonical path spelling; no alternate identity is admitted"));
            continue;
        }
        if (!Out.ByRequestedPath.Contains(Candidate.Requested))
        {
            Out.Roots.Emplace(Action);
            Out.ByRequestedPath.Add(Candidate.Requested, Action);
            Out.CanonicalByRequestedPath.Add(Candidate.Requested, Candidate.Canonical);
        }
    }
    return Errors.IsEmpty() ? FString() : AssetRefusal(Errors);
}
} // namespace NwiroEnhancedInputContract
