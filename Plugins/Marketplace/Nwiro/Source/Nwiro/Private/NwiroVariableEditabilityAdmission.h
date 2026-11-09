#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Pure variable-option shape admission, before any target access or peer mutation.
// It does not resolve variable types or change existing-variable semantics.
namespace NwiroVariableEditabilityAdmission
{
using FDecode = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, const FString&, TArray<TSharedPtr<FJsonValue>>&)>;
static FString Normalize(const FString& Key) { return Key.Replace(TEXT("_"), TEXT("")).ToLower(); }
static void Issue(TArray<TSharedPtr<FJsonValue>>& Errors, const FString& Container, int32 Unit,
    const FString& Field, const FString& Code, const FString& Canonical, const FString& Message)
{
    const auto E=MakeShared<FJsonObject>(); E->SetStringField(TEXT("op"),TEXT("add_variables"));
    E->SetStringField(TEXT("container"),Container);E->SetNumberField(TEXT("unit"),Unit);E->SetStringField(TEXT("field"),Field);
    E->SetStringField(TEXT("code"),Code);E->SetStringField(TEXT("canonicalField"),Canonical);E->SetStringField(TEXT("message"),Message);
    Errors.Add(MakeShared<FJsonValueObject>(E));
}
struct FEditabilityOptions
{
    TOptional<bool> InstanceEditable;
    TOptional<bool> ExposeOnSpawn;
};
// Decode original keys once. Never rewrite them: doing so would erase duplicate aliases.
static bool DecodeItemOptions(const TSharedPtr<FJsonObject>& Item, const FString& Container,
    int32 Unit, FEditabilityOptions& Out, TArray<TSharedPtr<FJsonValue>>& Errors)
{
    const int32 InitialErrors = Errors.Num();
    Out = FEditabilityOptions();
    FEditabilityOptions Decoded;
    TSet<FString> Seen;
    TArray<FString> Keys;
    for (const auto& Pair : Item->Values) Keys.Add(FString(Pair.Key));
    Keys.Sort();
    for (const FString& Key : Keys)
    {
        const FString Normal = Normalize(Key);
        if (Normal == TEXT("isexposed"))
        {
            Issue(Errors, Container, Unit, Key, TEXT("UNSUPPORTED_VARIABLE_EDITABILITY_ALIAS"),
                TEXT("instance_editable or expose_on_spawn"),
                TEXT("isExposed is ambiguous; use instanceEditable for placed-instance editing or exposeOnSpawn for a spawn pin"));
            continue;
        }
        TOptional<bool>* Flag = Normal == TEXT("instanceeditable") ? &Decoded.InstanceEditable
            : Normal == TEXT("exposeonspawn") ? &Decoded.ExposeOnSpawn : nullptr;
        if (!Flag) continue;
        const FString Canonical = Normal == TEXT("instanceeditable") ? TEXT("instance_editable") : TEXT("expose_on_spawn");
        if (Seen.Contains(Normal))
            Issue(Errors, Container, Unit, Key, TEXT("DUPLICATE_VARIABLE_EDITABILITY_ALIAS"), Canonical,
                TEXT("Provide exactly one spelling of each variable option, even when duplicate values agree"));
        Seen.Add(Normal);
        const TSharedPtr<FJsonValue> Value = Item->TryGetField(Key);
        if (!Value.IsValid() || Value->Type != EJson::Boolean)
            Issue(Errors, Container, Unit, Key, TEXT("INVALID_VARIABLE_EDITABILITY_BOOLEAN"), Canonical,
                TEXT("Use an actual JSON boolean true or false; omit the field to preserve creation defaults"));
        else *Flag = Value->AsBool();
    }
    if (Decoded.InstanceEditable.IsSet() && !Decoded.InstanceEditable.GetValue()
        && Decoded.ExposeOnSpawn.IsSet() && Decoded.ExposeOnSpawn.GetValue())
        Issue(Errors, Container, Unit, TEXT("instance_editable"), TEXT("CONFLICTING_VARIABLE_EDITABILITY"), TEXT("instance_editable"),
            TEXT("exposeOnSpawn:true requires instance editing; set instanceEditable:true, omit it, or set exposeOnSpawn:false"));
    for (const auto& Pair : Item->Values)
    {
        if (Normalize(FString(Pair.Key)) != TEXT("properties") || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) continue;
        const auto Properties = Pair.Value->AsObject();
        if (!Properties.IsValid()) continue;
        for (const auto& Property : Properties->Values)
        {
            const FString Key(Property.Key), Normal = Normalize(Key);
            if (Normal == TEXT("isexposed") || Normal == TEXT("instanceeditable") || Normal == TEXT("exposeonspawn"))
                Issue(Errors, Container, Unit, FString(Pair.Key) + TEXT(".") + Key, TEXT("MISPLACED_VARIABLE_EDITABILITY"),
                    Normal == TEXT("instanceeditable") ? TEXT("instance_editable") : Normal == TEXT("exposeonspawn") ? TEXT("expose_on_spawn") : TEXT("instance_editable or expose_on_spawn"),
                    TEXT("Variable editability options belong at variable top level, not inside properties"));
        }
    }
    if (Errors.Num() != InitialErrors) return false;
    Out = Decoded;
    return true;
}
static FString ValidateRequest(const TSharedPtr<FJsonObject>& Raw,FDecode Decode)
{
    if(!Raw.IsValid())return FString();
    TArray<TSharedPtr<FJsonValue>> Errors, Rejected, Skipped;
    int32 RequestedUnits = 0;
    bool bCountKnown = true;
    auto ValidateContainer = [&](const TSharedPtr<FJsonObject>& Object, const FString& Key, const FString& Context)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        if (!Decode(Object, Key, Items))
        {
            Issue(Errors, Context, INDEX_NONE, Key, TEXT("INVALID_VARIABLE_CONTAINER"), TEXT("add_variables"),
                TEXT("Use variable objects in an array, a single variable object, or the supported JSON-encoded form"));
            // The undecodable container makes the aggregate cardinality unknown.
            bCountKnown = false;
            Rejected.Add(Errors.Last());
            return;
        }
        for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
        {
            const int32 RequestUnit = RequestedUnits++;
            const int32 FirstIssue = Errors.Num();
            const auto& Item = Items[Unit];
            FString Name;
            if (!Item.IsValid() || Item->Type != EJson::Object || !Item->AsObject().IsValid())
                Issue(Errors, Context, Unit, TEXT("item"), TEXT("INVALID_VARIABLE_ITEM"), TEXT("add_variables"), TEXT("Every variable item must be an object"));
            else
            {
                Item->AsObject()->TryGetStringField(TEXT("name"), Name);
                FEditabilityOptions Options;
                DecodeItemOptions(Item->AsObject(), Context, Unit, Options, Errors);
            }
            if (Errors.Num() > FirstIssue)
            {
                // One operation and one entry per requested unit, including actions
                // concatenated by execution. Detailed issues retain raw container indices.
                const auto Diagnostic = MakeShared<FJsonObject>();
                Diagnostic->Values = Errors[FirstIssue]->AsObject()->Values;
                Diagnostic->SetNumberField(TEXT("unit"), RequestUnit);
                Diagnostic->SetNumberField(TEXT("containerUnit"), Unit);
                Diagnostic->SetStringField(TEXT("received"), Name);
                TArray<TSharedPtr<FJsonValue>> Issues;
                for (int32 Index = FirstIssue; Index < Errors.Num(); ++Index) Issues.Add(Errors[Index]);
                Diagnostic->SetArrayField(TEXT("issues"), Issues);
                Rejected.Add(MakeShared<FJsonValueObject>(Diagnostic));
            }
            else
            {
                const auto Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("op"), TEXT("add_variables"));
                Entry->SetNumberField(TEXT("unit"), RequestUnit);
                Entry->SetStringField(TEXT("container"), Context);
                Entry->SetNumberField(TEXT("containerUnit"), Unit);
                Entry->SetStringField(TEXT("name"), Name);
                Entry->SetBoolField(TEXT("asRequested"), false);
                Entry->SetStringField(TEXT("reason"), TEXT("whole request rejected before target access"));
                Skipped.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    };
    for (const TCHAR* Key : {TEXT("add_variables"), TEXT("variables"), TEXT("variable")})
        if (Raw->HasField(Key)) ValidateContainer(Raw, Key, Key);
    // Execution concatenates action payloads. Validate every payload before that
    // projection so a later action cannot hide an invalid earlier variable.
    TArray<TSharedPtr<FJsonValue>> Actions;
    if (Raw->HasField(TEXT("actions")) && Decode(Raw, TEXT("actions"), Actions))
    {
        for (int32 ActionIndex = 0; ActionIndex < Actions.Num(); ++ActionIndex)
        {
            const auto& Value = Actions[ActionIndex];
            if (!Value.IsValid() || Value->Type != EJson::Object) continue;
            const auto Action = Value->AsObject();
            FString Kind;
            if (!Action.IsValid() || !Action->TryGetStringField(TEXT("type"), Kind)) continue;
            for (const auto& Pair : Action->Values)
            {
                const FString Key(Pair.Key);
                if (Key == TEXT("type")) continue;
                if (Kind == TEXT("add_variables") || Key == TEXT("add_variables") || Key == TEXT("variables") || Key == TEXT("variable"))
                    ValidateContainer(Action, Key, FString::Printf(TEXT("actions[%d].%s"), ActionIndex, *Key));
            }
        }
    }
    if(Errors.IsEmpty())return FString();
    const auto Result=MakeShared<FJsonObject>();Result->SetBoolField(TEXT("success"),false);
    Result->SetStringField(TEXT("code"),TEXT("VARIABLE_EDITABILITY_REJECTED"));Result->SetArrayField(TEXT("errors"),Errors);
    const auto Ledger = MakeShared<FJsonObject>();
    Ledger->SetStringField(TEXT("op"), TEXT("add_variables"));
    if (bCountKnown) Ledger->SetNumberField(TEXT("requestUnits"), RequestedUnits);
    else Ledger->SetField(TEXT("requestUnits"), MakeShared<FJsonValueNull>());
    for (const TCHAR* Bucket : {TEXT("created"), TEXT("modified"), TEXT("removed")}) Ledger->SetArrayField(Bucket, {});
    Ledger->SetArrayField(TEXT("errors"), Rejected);
    Ledger->SetArrayField(TEXT("skipped"), Skipped);
    Result->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Ledger)});
    Result->SetBoolField(TEXT("savedToDisk"), false);
    Result->SetBoolField(TEXT("mutationAttempted"), false);
    Result->SetBoolField(TEXT("targetStateObserved"), false);
    Result->SetField(TEXT("dirtyPackages"), MakeShared<FJsonValueNull>());
    Result->SetStringField(TEXT("compileStatus"), TEXT("blocked"));
    Result->SetStringField(TEXT("validationLevel"), TEXT("preflight"));
    FString Target;
    Raw->TryGetStringField(TEXT("blueprint"), Target);
    Result->SetStringField(TEXT("blueprint"), Target);

    Result->SetStringField(TEXT("error"),TEXT("Variable options rejected before target access; use boolean instanceEditable/instance_editable and exposeOnSpawn/expose_on_spawn options"));
    Result->SetArrayField(TEXT("messages"), {MakeShared<FJsonValueString>(Result->GetStringField(TEXT("error")))});
    for(const TCHAR* Key:{TEXT("currentRequestMutation"),TEXT("targetAccessed"),TEXT("targetLoadRequested"),TEXT("targetMutationStarted"),TEXT("compiled"),TEXT("saveAttempted")})Result->SetBoolField(Key,false);
    FString Out;const auto Writer=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    if(!FJsonSerializer::Serialize(Result,Writer))return TEXT("{\"success\":false,\"code\":\"VARIABLE_EDITABILITY_REJECTED\",\"error\":\"Variable admission response serialization failed; no target accessed\",\"savedToDisk\":false,\"operations\":[],\"errors\":[],\"countsComplete\":false,\"serializationFailed\":true,\"messages\":[\"Variable admission response serialization failed; no target accessed\"],\"currentRequestMutation\":false,\"mutationAttempted\":false,\"targetAccessed\":false,\"targetLoadRequested\":false,\"targetMutationStarted\":false,\"targetStateObserved\":false,\"dirtyPackages\":null,\"compiled\":false,\"saveAttempted\":false,\"compileStatus\":\"blocked\",\"validationLevel\":\"preflight\"}");
    return Out;
}
}
