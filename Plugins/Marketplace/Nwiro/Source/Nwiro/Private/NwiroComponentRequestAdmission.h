#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Pure request-shape admission: no target lookup, mutation or normalization.
// Parent existence and inherited native-root semantics remain owned by the handler.
namespace NwiroComponentRequestAdmission
{
using FDecode = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, const FString&, TArray<TSharedPtr<FJsonValue>>&)>;
static FString Normalize(const FString& Key) { return Key.Replace(TEXT("_"), TEXT("")).ToLower(); }
static void Issue(TArray<TSharedPtr<FJsonValue>>& Errors, const FString& Container, int32 Unit,
    const FString& Field, const FString& Code, const FString& Canonical, const FString& Message)
{
    const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
    E->SetStringField(TEXT("op"), TEXT("add_components")); E->SetStringField(TEXT("container"), Container);
    E->SetNumberField(TEXT("unit"), Unit); E->SetStringField(TEXT("field"), Field);
    E->SetStringField(TEXT("code"), Code); E->SetStringField(TEXT("canonicalField"), Canonical);
    E->SetStringField(TEXT("message"), Message); Errors.Add(MakeShared<FJsonValueObject>(E));
}
static void ValidateItem(const TSharedPtr<FJsonObject>& Item, const FString& Container, int32 Unit,
    TArray<TSharedPtr<FJsonValue>>& Errors)
{
    TArray<FString> Keys;
    for (const auto& Pair : Item->Values) Keys.Add(FString(Pair.Key));
    Keys.Sort();
    for (const FString& Key : Keys)
    {
        const FString Normal = Normalize(Key);
        const bool RootAlias = Normal == TEXT("isroot")
            || (Normal == TEXT("makeroot") && !Key.Equals(TEXT("makeRoot"), ESearchCase::CaseSensitive));
        const bool ParentAlias = Normal == TEXT("attachparent")
            || (Normal == TEXT("parent") && !Key.Equals(TEXT("parent"), ESearchCase::CaseSensitive));
        if (RootAlias || ParentAlias)
            Issue(Errors, Container, Unit, Key, TEXT("UNSUPPORTED_COMPONENT_PLACEMENT_ALIAS"),
                RootAlias ? TEXT("makeRoot") : TEXT("parent"),
                RootAlias ? TEXT("Use top-level makeRoot with a JSON boolean; isRoot and alternate spellings are not applied")
                          : TEXT("Use top-level parent with the component name; attachment aliases are not applied"));
    }
    const TSharedPtr<FJsonValue> Root = Item->TryGetField(TEXT("makeRoot"));
    if (Root.IsValid() && Root->Type != EJson::Boolean)
        Issue(Errors, Container, Unit, TEXT("makeRoot"), TEXT("INVALID_COMPONENT_ROOT"), TEXT("makeRoot"),
            TEXT("makeRoot must be a JSON boolean, not a string, number, null, object or array"));
    const TSharedPtr<FJsonValue> Parent = Item->TryGetField(TEXT("parent"));
    bool ParentHasName = false;
    if (Parent.IsValid())
    {
        bool Valid = Parent->Type == EJson::String;
        if (Valid)
        {
            const FString Name = Parent->AsString();
            // Explicit empty parent retains default attachment. Do not trim/coerce names.
            Valid = Name.Equals(Name.TrimStartAndEnd(), ESearchCase::CaseSensitive);
            for (TCHAR Character : Name) if (Character < 0x20 || Character == 0x7f) Valid = false;
            ParentHasName = !Name.IsEmpty();
        }
        if (!Valid)
            Issue(Errors, Container, Unit, TEXT("parent"), TEXT("INVALID_COMPONENT_PARENT"), TEXT("parent"),
                TEXT("parent must be a string component name without surrounding whitespace or control characters; omit it or use an empty string for default attachment"));
    }
    if (Root.IsValid() && Root->Type == EJson::Boolean && Root->AsBool() && ParentHasName)
        Issue(Errors, Container, Unit, TEXT("parent"), TEXT("CONFLICTING_COMPONENT_PLACEMENT"), TEXT("makeRoot or parent"),
            TEXT("makeRoot:true and a named parent cannot both be applied; choose root placement or top-level parent"));
    const TSharedPtr<FJsonValue> Properties = Item->TryGetField(TEXT("properties"));
    if (Properties.IsValid())
    {
        if (Properties->Type != EJson::Object || !Properties->AsObject().IsValid())
            Issue(Errors, Container, Unit, TEXT("properties"), TEXT("INVALID_COMPONENT_PROPERTIES"), TEXT("properties"),
                TEXT("properties must be a JSON object; root and attachment fields belong at component top level"));
        else
        {
            TArray<FString> PropertyKeys;
            for (const auto& Pair : Properties->AsObject()->Values) PropertyKeys.Add(FString(Pair.Key));
            PropertyKeys.Sort();
            for (const FString& Key : PropertyKeys)
            {
                const FString Normal = Normalize(Key);
                const bool RootField = Normal == TEXT("isroot") || Normal == TEXT("makeroot");
                const bool ParentField = Normal == TEXT("parent") || Normal == TEXT("attachparent");
                if (RootField || ParentField)
                    Issue(Errors, Container, Unit, TEXT("properties.") + Key, TEXT("MISPLACED_COMPONENT_PLACEMENT"),
                        RootField ? TEXT("makeRoot") : TEXT("parent"),
                        TEXT("Root and attachment settings inside properties are not applied; move the request to its canonical top-level field"));
            }
        }
    }
}
static FString ValidateRequest(const TSharedPtr<FJsonObject>& Raw, FDecode Decode)
{
    if (!Raw.IsValid()) return FString(); // Entry parser owns invalid JSON.
    // Mirror existing actions projection without changing Raw or any shared item.
    TSharedPtr<FJsonObject> Projected = MakeShared<FJsonObject>(); Projected->Values = Raw->Values;
    if (Raw->HasField(TEXT("actions")))
    {
        TArray<TSharedPtr<FJsonValue>> Actions;
        // Bad actions cannot suppress independently present component validation.
        if (Decode(Raw, TEXT("actions"), Actions))
        for (const TSharedPtr<FJsonValue>& V : Actions)
        {
            if (!V.IsValid() || V->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> Action = V->AsObject(); FString Kind;
            if (!Action.IsValid() || !Action->TryGetStringField(TEXT("type"), Kind) || Kind.IsEmpty()) continue;
            for (const auto& Pair : Action->Values)
                if (Pair.Key != TEXT("type")) Projected->SetField(Pair.Key, Pair.Value);
            for (const auto& Pair : Action->Values)
                if (Pair.Key != TEXT("type") && Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
                { Projected->SetField(Kind, Pair.Value); break; }
        }
    }
    TArray<TSharedPtr<FJsonValue>> Errors;
    for (const TCHAR* Key : { TEXT("add_components"), TEXT("components"), TEXT("component") })
    {
        if (!Projected->HasField(Key)) continue;
        TArray<TSharedPtr<FJsonValue>> Items;
        if (!Decode(Projected, Key, Items))
        {
            Issue(Errors, Key, INDEX_NONE, Key, TEXT("INVALID_COMPONENT_CONTAINER"), TEXT("add_components"),
                TEXT("Use an array of component objects, one component object, or the supported JSON-encoded form"));
            continue;
        }
        for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
        {
            const TSharedPtr<FJsonValue>& Item = Items[Unit];
            if (!Item.IsValid() || Item->Type != EJson::Object || !Item->AsObject().IsValid())
                Issue(Errors, Key, Unit, TEXT("item"), TEXT("INVALID_COMPONENT_ITEM"), TEXT("add_components"),
                    TEXT("Every component item must be a JSON object; invalid items are not silently skipped"));
            else ValidateItem(Item->AsObject(), Key, Unit, Errors);
        }
    }
    if (Errors.IsEmpty()) return FString();
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("code"), TEXT("COMPONENT_REQUEST_REJECTED"));
    Result->SetStringField(TEXT("error"), TEXT("Component request rejected before target access; use canonical top-level makeRoot and parent fields"));
    Result->SetArrayField(TEXT("errors"), Errors);
    for (const TCHAR* Field : { TEXT("currentRequestMutation"), TEXT("targetAccessed"), TEXT("targetLoadRequested"),
        TEXT("targetMutationStarted"), TEXT("compiled"), TEXT("saveAttempted") }) Result->SetBoolField(Field, false);
    FString Out; const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    if (!FJsonSerializer::Serialize(Result, Writer))
        return TEXT("{\"success\":false,\"code\":\"COMPONENT_REQUEST_REJECTED\",\"targetAccessed\":false,\"targetLoadRequested\":false,\"targetMutationStarted\":false,\"currentRequestMutation\":false,\"compiled\":false,\"saveAttempted\":false}");
    return Out;
}
} // namespace NwiroComponentRequestAdmission
