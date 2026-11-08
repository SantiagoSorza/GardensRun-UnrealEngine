#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Narrow contract for the advertised EnhancedInputAction recipe only.
// JSON projection/validation is completed before EditBlueprint loads a target.
namespace NwiroEnhancedInputContract
{
struct FRecipeItem
{
    TSharedPtr<FJsonObject> Object;
    FString Container;
    int32 Unit = INDEX_NONE;
};

using FDecode = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, const FString&,
    TArray<TSharedPtr<FJsonValue>>&)>;

static const TCHAR* AllowedKeys[] = { TEXT("type"), TEXT("ref"), TEXT("action"), TEXT("x"), TEXT("y") };
static const TCHAR* TypeKeys[] = { TEXT("type"), TEXT("node"), TEXT("nodeType"), TEXT("node_type"),
    TEXT("nodetype"), TEXT("kind"), TEXT("nodeClass"), TEXT("class") };

static bool IsAllowed(const FString& Key)
{
    for (const TCHAR* Allowed : AllowedKeys) if (Key == Allowed) return true;
    return false;
}

static bool SelectsRecipe(const TSharedPtr<FJsonObject>& Obj)
{
    // Match the native node-type precedence; never relabel a different node type.
    for (const TCHAR* Key : TypeKeys)
    {
        if (Obj->HasField(Key))
        {
            FString Value;
            return Obj->TryGetStringField(Key, Value)
                && Value.Equals(TEXT("EnhancedInputAction"), ESearchCase::IgnoreCase);
        }
    }
    return false;
}

static void Issue(TArray<TSharedPtr<FJsonValue>>& Errors, const FString& Container,
    int32 Unit, const FString& Field, const FString& Code, const FString& Message)
{
    TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
    E->SetStringField(TEXT("op"), TEXT("add_nodes"));
    E->SetStringField(TEXT("container"), Container);
    E->SetNumberField(TEXT("unit"), Unit);
    E->SetStringField(TEXT("field"), Field);
    E->SetStringField(TEXT("code"), Code);
    E->SetStringField(TEXT("message"), Message);
    Errors.Add(MakeShared<FJsonValueObject>(E));
}

static void ValidateItem(const TSharedPtr<FJsonObject>& Obj, const FString& Container,
    int32 Unit, TArray<TSharedPtr<FJsonValue>>& Errors)
{
    if (!Obj.IsValid() || !SelectsRecipe(Obj)) return;
    TArray<FString> Keys;
    // JSON keys may use FString or shared-string storage depending on the engine.
    for (const auto& Pair : Obj->Values) Keys.Add(FString(Pair.Key));
    Keys.Sort();
    for (const FString& Key : Keys)
    {
        if (!IsAllowed(Key))
            Issue(Errors, Container, Unit, Key, TEXT("UNKNOWN_ENHANCED_INPUT_KEY"),
                TEXT("EnhancedInputAction accepts only type, ref, action, x, y; aliases and event/property settings are not applied"));
    }
    for (const TCHAR* Required : { TEXT("type"), TEXT("ref"), TEXT("action") })
    {
        FString Value;
        if (!Obj->TryGetStringField(Required, Value) || Value.TrimStartAndEnd().IsEmpty()
            || (FString(Required) == TEXT("type") && Value != TEXT("EnhancedInputAction")))
            Issue(Errors, Container, Unit, Required, TEXT("INVALID_ENHANCED_INPUT_FIELD"),
                TEXT("Use canonical type EnhancedInputAction and nonempty string ref and action fields"));
    }
    for (const TCHAR* Position : { TEXT("x"), TEXT("y") })
    {
        if (!Obj->HasField(Position)) continue;
        double Value = 0;
        if (!Obj->TryGetNumberField(Position, Value) || !FMath::IsFinite(Value)
            || Value < static_cast<double>(MIN_int32) || Value > static_cast<double>(MAX_int32)
            || Value != static_cast<double>(static_cast<int32>(Value)))
            Issue(Errors, Container, Unit, Position, TEXT("INVALID_ENHANCED_INPUT_FIELD"),
                TEXT("Node coordinates must be finite int32 integers"));
    }
}

static FString ValidateRequest(const TSharedPtr<FJsonObject>& Raw, FDecode Decode,
    TArray<FRecipeItem>* OutRecipes = nullptr)
{
    if (OutRecipes) OutRecipes->Reset();
    if (!Raw.IsValid()) return FString(); // The entry parser owns malformed JSON.
    // Only this new root object is changed. Action objects and their items are read-only.
    TSharedPtr<FJsonObject> Projected = MakeShared<FJsonObject>();
    Projected->Values = Raw->Values;
    if (Raw->HasField(TEXT("actions")))
    {
        TArray<TSharedPtr<FJsonValue>> Actions;
        // A malformed actions container must not bypass independently present node recipes.
        if (Decode(Raw, TEXT("actions"), Actions))
        for (const TSharedPtr<FJsonValue>& V : Actions)
        {
            if (!V.IsValid() || V->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> A = V->AsObject();
            FString Kind;
            if (!A.IsValid() || !A->TryGetStringField(TEXT("type"), Kind) || Kind.IsEmpty()) continue;
            // Exactly the existing action projection: non-type fields are copied;
            // its first array also becomes the operation named by type.
            for (const auto& Pair : A->Values)
                if (Pair.Key != TEXT("type")) Projected->SetField(Pair.Key, Pair.Value);
            for (const auto& Pair : A->Values)
            {
                if (Pair.Key != TEXT("type") && Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
                {
                    Projected->SetField(Kind, Pair.Value);
                    break;
                }
            }
        }
    }
    TArray<TSharedPtr<FJsonValue>> Errors;
    // All possible node operation containers are checked. Conflict rejection stays
    // with the existing normalizer; this does not introduce general unknown-key policy.
    for (const TCHAR* Key : { TEXT("add_nodes"), TEXT("nodes"), TEXT("node") })
    {
        if (!Projected->HasField(Key)) continue;
        TArray<TSharedPtr<FJsonValue>> Items;
        if (!Decode(Projected, Key, Items)) continue; // Existing invalid-container handling.
        for (int32 Unit = 0; Unit < Items.Num(); ++Unit)
        {
            const auto& Item = Items[Unit];
            if (Item.IsValid() && Item->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Object = Item->AsObject();
                ValidateItem(Object, Key, Unit, Errors);
                if (OutRecipes && Object.IsValid() && SelectsRecipe(Object))
                {
                    FRecipeItem Recipe;
                    Recipe.Object = Object;
                    Recipe.Container = Key;
                    Recipe.Unit = Unit;
                    OutRecipes->Add(MoveTemp(Recipe));
                }
            }
        }
    }
    if (Errors.IsEmpty()) return FString();
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("error"), TEXT("EnhancedInputAction request rejected before target access; use only type, ref, action, x, y"));
    Result->SetStringField(TEXT("code"), TEXT("ENHANCED_INPUT_CONTRACT_REJECTED"));
    Result->SetArrayField(TEXT("errors"), Errors);
    Result->SetBoolField(TEXT("currentRequestMutation"), false);
    Result->SetBoolField(TEXT("targetAccessed"), false);
    Result->SetBoolField(TEXT("compiled"), false);
    Result->SetBoolField(TEXT("saveAttempted"), false);
    FString Out;
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    if (!FJsonSerializer::Serialize(Result, Writer))
        return TEXT("{\"success\":false,\"code\":\"ENHANCED_INPUT_CONTRACT_REJECTED\",\"targetAccessed\":false,\"compiled\":false,\"saveAttempted\":false}");
    return Out;
}

static void AddDiscovery(const FString& Query, const TSharedRef<FJsonObject>& Result)
{
    if (!Query.IsEmpty() && !Query.Contains(TEXT("input"), ESearchCase::IgnoreCase)
        && !Query.Contains(TEXT("action"), ESearchCase::IgnoreCase)
        && !Query.Contains(TEXT("enhanced"), ESearchCase::IgnoreCase)
        && !Query.StartsWith(TEXT("IA_"), ESearchCase::IgnoreCase)) return;
    TSharedRef<FJsonObject> Recipe = MakeShared<FJsonObject>();
    Recipe->SetStringField(TEXT("type"), TEXT("EnhancedInputAction"));
    Recipe->SetStringField(TEXT("tool"), TEXT("edit_blueprint"));
    Recipe->SetStringField(TEXT("operation"), TEXT("add_nodes"));
    Recipe->SetStringField(TEXT("description"), TEXT("Enhanced Input event node. Use type, ref, action and optional integer x/y. action must be an exact /Game InputAction object or package path already loaded and initialized. Cold or missing assets return ACTION_NOT_LOADED; bare names and redirects are rejected before target work. Execution outputs include Started, Triggered and Completed; ActionValue type follows the action. Trigger support depends on the action. A mapping context must separately be active for the local player."));
    TArray<TSharedPtr<FJsonValue>> Allowed;
    for (const TCHAR* Key : AllowedKeys) Allowed.Add(MakeShared<FJsonValueString>(Key));
    Recipe->SetArrayField(TEXT("allowedKeys"), Allowed);
    TArray<TSharedPtr<FJsonValue>> Recipes;
    Recipes.Add(MakeShared<FJsonValueObject>(Recipe));
    Result->SetArrayField(TEXT("nodeRecipes"), Recipes);
}
} // namespace NwiroEnhancedInputContract
