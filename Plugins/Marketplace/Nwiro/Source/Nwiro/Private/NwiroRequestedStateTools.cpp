// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroRequestedStateTools.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "EdGraphSchema_K2.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Json.h"
#include "Misc/DateTime.h"
#include "Containers/StringConv.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"
#include "Runtime/Launch/Resources/Version.h"

namespace NwiroRequestedState
{
using Object = TSharedPtr<FJsonObject>;
constexpr int32 MaxPayloadBytes = 65536;
constexpr double MaxSafeInteger = 9007199254740991.0;
static Object Obj() { return MakeShared<FJsonObject>(); }
static Object Unknown() { Object O = Obj(); O->SetBoolField(TEXT("complete"), false); return O; }
static Object Measured(const TSharedPtr<FJsonValue>& V)
{
	Object O = Obj(); O->SetBoolField(TEXT("complete"), true); O->SetField(TEXT("value"), V); return O;
}
static Object MeasuredString(const FString& V) { return Measured(MakeShared<FJsonValueString>(V)); }
static Object MeasuredBool(bool V) { return Measured(MakeShared<FJsonValueBoolean>(V)); }
static Object Collection(const TArray<TSharedPtr<FJsonValue>>& Items)
{
	Object O = Obj(); O->SetBoolField(TEXT("complete"), true); O->SetArrayField(TEXT("items"), Items); return O;
}
static FString Encode(const Object& O)
{
	FString Out; TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(O.ToSharedRef(), Writer); return Out;
}
static bool Alpha(TCHAR C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_'; }
static bool Digit(TCHAR C) { return C >= '0' && C <= '9'; }
static bool CanonicalName(const FString& S)
{
	if (S.IsEmpty() || S.Len() > 128 || !Alpha(S[0]) || S.EndsWith(TEXT(" "))) return false;
	for (TCHAR C : S) if (!Alpha(C) && !Digit(C) && C != ' ') return false;
	return true;
}
static bool CanonicalPath(const FString& S, bool bAsset)
{
	if (S.IsEmpty() || S.Len() > 512 || (!S.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive) && !S.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))) return false;
	if (bAsset && !S.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)) return false;
	FString Package, Name; if (!S.Split(TEXT("."), &Package, &Name) || Name.IsEmpty() || Name.Contains(TEXT("."))) return false;
	for (TCHAR C : Name) if (!Alpha(C) && !Digit(C)) return false;
	if (Package.EndsWith(TEXT("/")) || Package.Contains(TEXT("//"))) return false;
	for (TCHAR C : Package) if (C != '/' && !Alpha(C) && !Digit(C)) return false;
	FString Last; Package.Split(TEXT("/"), nullptr, &Last, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	return !bAsset || Last.Equals(Name, ESearchCase::CaseSensitive);
}
static bool UUID(const FString& S)
{
	FGuid Value; return S.Len() == 36 && FGuid::ParseExact(S, EGuidFormats::DigitsWithHyphens, Value)
		&& Value.ToString(EGuidFormats::DigitsWithHyphens).ToLower().Equals(S, ESearchCase::CaseSensitive);
}
static bool Sha256(const FString& S)
{
	if (S.Len() != 64) return false;
	for (TCHAR C : S) if (!Digit(C) && !(C >= 'a' && C <= 'f')) return false;
	return true;
}
static bool Usable(const UObject* O)
{
    if (!O) return false;
    EInternalObjectFlags NotReady = EInternalObjectFlags::Async | EInternalObjectFlags::AsyncLoadingPhase1
        | EInternalObjectFlags::AsyncLoadingPhase2 | EInternalObjectFlags::PendingConstruction;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
    NotReady |= EInternalObjectFlags::Remote;
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
    NotReady |= EInternalObjectFlags::LocalNotReady;
#endif
    return !O->HasAnyInternalFlags(NotReady) && IsValid(O)
        && !O->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects
            | RF_WillBeLoaded | RF_BeginDestroyed | RF_FinishDestroyed | RF_ClassDefaultObject);
}
// The string StaticFindObject overload can load during T3D import. This exact
// top-level overload searches only the existing package/object hash tables.
static UObject* FindLoaded(const FString& Path)
{
	FString Package, Name;
	if (!CanonicalPath(Path, true) || !Path.Split(TEXT("."), &Package, &Name)) return nullptr;
	return StaticFindObjectSafe(UObject::StaticClass(), FTopLevelAssetPath(FName(*Package), FName(*Name)), false);
}
static bool SafeClass(const UClass* C)
{
	return Usable(C) && !C->HasAnyClassFlags(CLASS_NewerVersionExists) && CanonicalPath(C->GetPathName(), false);
}
// UTC Unix milliseconds are provenance only. Host dispatch/receipt clocks gate freshness.
static double NativeMilliseconds()
{
	const FDateTime Now = FDateTime::UtcNow();
	return double(Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond());
}
static bool Request(const FString& Json, const FString& Family, Object& Out)
{
	FTCHARToUTF8 Bytes(*Json); if (Bytes.Length() > 8192) return false;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out) || !Out.IsValid()) return false;
	const TArray<FString> Keys = {TEXT("dispatchId"), TEXT("nonce"), TEXT("observer"), TEXT("observerVersion"), TEXT("taskId"), TEXT("turnId"), TEXT("generation"), TEXT("contractSha256"), TEXT("contractRevision"), TEXT("objectPath"), TEXT("assetClassPath")};
	if (Out->Values.Num() != Keys.Num()) return false;
	for (const FString& K : Keys) if (!Out->HasField(K)) return false;
	for (const TCHAR* K : {TEXT("dispatchId"), TEXT("nonce"), TEXT("taskId"), TEXT("turnId")})
	{
		FString V; if (!Out->TryGetStringField(K, V) || !UUID(V)) return false;
	}
	FString Hash, Observer, Path, ClassPath;
	if (!Out->TryGetStringField(TEXT("contractSha256"), Hash) || !Sha256(Hash)
		|| !Out->TryGetStringField(TEXT("observer"), Observer) || !Observer.Equals(Family, ESearchCase::CaseSensitive)
		|| !Out->TryGetStringField(TEXT("objectPath"), Path) || !CanonicalPath(Path, true)
		|| !Out->TryGetStringField(TEXT("assetClassPath"), ClassPath) || !CanonicalPath(ClassPath, false)) return false;
	for (const TCHAR* K : {TEXT("observerVersion"), TEXT("generation"), TEXT("contractRevision")})
	{
		double V; if (!Out->TryGetNumberField(K, V) || !FMath::IsFinite(V) || V < 0 || V > MaxSafeInteger || FMath::FloorToDouble(V) != V) return false;
		if (FString(K) == TEXT("observerVersion") && V != 2) return false;
		if (FString(K) == TEXT("contractRevision") && V != 1) return false;
	}
	return true;
}
// These atoms come only from actual FEdGraphPinType metadata. Unsupported flags,
// unresolved weak subtypes, self/bitmask/member references and unknown kinds stay unknown.
static Object Atom(FName Category, FName Subcategory, const TWeakObjectPtr<UObject>& WeakSubtype)
{
	UObject* Subtype = WeakSubtype.Get();
	if (!WeakSubtype.IsExplicitlyNull() && !Usable(Subtype)) return nullptr;
	FString Base;
	if (Category == UEdGraphSchema_K2::PC_Real)
	{
		if (Subtype) return nullptr;
		if (Subcategory == UEdGraphSchema_K2::PC_Float) Base = TEXT("float");
		else if (Subcategory == UEdGraphSchema_K2::PC_Double) Base = TEXT("double");
		else return nullptr;
	}
	else
	{
		if (!Subcategory.IsNone()) return nullptr;
		if (Category == UEdGraphSchema_K2::PC_Boolean) Base = TEXT("bool");
		else if (Category == UEdGraphSchema_K2::PC_Int) Base = TEXT("int32");
		else if (Category == UEdGraphSchema_K2::PC_Int64) Base = TEXT("int64");
		else if (Category == UEdGraphSchema_K2::PC_Float) Base = TEXT("float");
		else if (Category == UEdGraphSchema_K2::PC_Double) Base = TEXT("double");
		else if (Category == UEdGraphSchema_K2::PC_Name) Base = TEXT("name");
		else if (Category == UEdGraphSchema_K2::PC_String) Base = TEXT("string");
		else if (Category == UEdGraphSchema_K2::PC_Text) Base = TEXT("text");
		else if (Category == UEdGraphSchema_K2::PC_Byte) Base = Subtype ? TEXT("enum") : TEXT("byte");
		else if (Category == UEdGraphSchema_K2::PC_Enum) Base = TEXT("enum");
		else if (Category == UEdGraphSchema_K2::PC_Struct) Base = TEXT("struct");
		else if (Category == UEdGraphSchema_K2::PC_Object) Base = TEXT("object");
		else if (Category == UEdGraphSchema_K2::PC_Class) Base = TEXT("class");
		else if (Category == UEdGraphSchema_K2::PC_SoftObject) Base = TEXT("soft_object");
		else if (Category == UEdGraphSchema_K2::PC_SoftClass) Base = TEXT("soft_class");
		else return nullptr;
	}
	const bool bTyped = Base == TEXT("enum") || Base == TEXT("struct") || Base == TEXT("object") || Base == TEXT("class") || Base == TEXT("soft_object") || Base == TEXT("soft_class");
	if (bTyped)
	{
		if (!Usable(Subtype) || !CanonicalPath(Subtype->GetPathName(), false)) return nullptr;
		if (Base == TEXT("enum") ? !Subtype->IsA<UEnum>() : Base == TEXT("struct") ? !Subtype->IsA<UScriptStruct>() : !SafeClass(Cast<UClass>(Subtype))) return nullptr;
	}
	else if (Subtype) return nullptr;
	Object O = Obj(); O->SetStringField(TEXT("base"), Base);
	if (bTyped) O->SetStringField(TEXT("subtype"), Subtype->GetPathName());
	return O;
}
static Object Type(const FEdGraphPinType& T)
{
	if (T.bIsWeakPointer || T.bIsUObjectWrapper || T.bSerializeAsSinglePrecisionFloat
		|| T.PinSubCategoryMemberReference.MemberParent || !T.PinSubCategoryMemberReference.MemberName.IsNone() || T.PinSubCategoryMemberReference.MemberGuid.IsValid()) return Unknown();
	Object Primary = Atom(T.PinCategory, T.PinSubCategory, T.PinSubCategoryObject); if (!Primary) return Unknown();
	FString Container;
	switch (T.ContainerType)
	{
	case EPinContainerType::None: Container = TEXT("scalar"); break;
	case EPinContainerType::Array: Container = TEXT("array"); break;
	case EPinContainerType::Set: Container = TEXT("set"); break;
	case EPinContainerType::Map: Container = TEXT("map"); break;
	default: return Unknown();
	}
	Object O = Obj(); O->SetStringField(TEXT("container"), Container); O->SetBoolField(TEXT("isReference"), T.bIsReference); O->SetBoolField(TEXT("isConst"), T.bIsConst);
	if (T.ContainerType == EPinContainerType::Map)
	{
		const FEdGraphTerminalType& V = T.PinValueType;
		if (V.bTerminalIsConst || V.bTerminalIsWeakPointer || V.bTerminalIsUObjectWrapper) return Unknown();
		Object Value = Atom(V.TerminalCategory, V.TerminalSubCategory, V.TerminalSubCategoryObject); if (!Value) return Unknown();
		O->SetObjectField(TEXT("key"), Primary); O->SetObjectField(TEXT("value"), Value);
	}
	else
	{
		if (T.PinValueType != FEdGraphTerminalType()) return Unknown();
		O->SetObjectField(TEXT("value"), Primary);
	}
	return Measured(MakeShared<FJsonValueObject>(O));
}
static Object BlueprintFacts(UBlueprint* BP)
{
	// Read only the resolved raw class handle. TSubclassOf::Get additionally walks
	// IsChildOf/super handles and can resolve a lazy superclass before readiness checks.
	const TObjectPtr<UClass>& ParentHandle = BP->ParentClass.GetGCPtr();
	UClass* Parent = ParentHandle.IsResolved() ? ParentHandle.Get() : nullptr;
	Object Facts = Obj(); Facts->SetObjectField(TEXT("parentClassPath"), SafeClass(Parent) ? MeasuredString(Parent->GetPathName()) : Unknown());
	Object Variables = Unknown();
	if (BP->NewVariables.Num() <= 128)
	{
		TSet<FString> Names; TArray<TSharedPtr<FJsonValue>> Items; bool Complete = true;
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			const FString Name = V.VarName.ToString();
			if (!CanonicalName(Name) || Names.Contains(Name.ToLower())) { Complete = false; break; }
			Names.Add(Name.ToLower()); Object Item = Obj(); Item->SetStringField(TEXT("canonicalName"), Name); Item->SetObjectField(TEXT("type"), Type(V.VarType));
			// DefaultValue exports can clear on compile; never infer zero or inspect CDO.
			Item->SetObjectField(TEXT("authoredDefault"), Unknown()); Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		if (Complete) Variables = Collection(Items);
	}
	Facts->SetObjectField(TEXT("declaredVariables"), Variables); return Facts;
}
static Object ScalarBool(bool V) { Object O = Obj(); O->SetStringField(TEXT("kind"), TEXT("boolean")); O->SetBoolField(TEXT("value"), V); return Measured(MakeShared<FJsonValueObject>(O)); }
static Object ScalarNumber(double V) { if (!FMath::IsFinite(V)) return Unknown(); Object O = Obj(); O->SetStringField(TEXT("kind"), TEXT("number")); O->SetNumberField(TEXT("value"), V); return Measured(MakeShared<FJsonValueObject>(O)); }
static Object ScalarString(const FString& V) { Object O = Obj(); O->SetStringField(TEXT("kind"), TEXT("string")); O->SetStringField(TEXT("value"), V); return Measured(MakeShared<FJsonValueObject>(O)); }
static Object TriggerProperties(const UInputTrigger* T)
{
	if (T->GetClass() != UInputTriggerDown::StaticClass() && T->GetClass() != UInputTriggerPressed::StaticClass() && T->GetClass() != UInputTriggerReleased::StaticClass()) return nullptr;
	Object O = Obj(); O->SetObjectField(TEXT("ActuationThreshold"), ScalarNumber(T->ActuationThreshold)); O->SetObjectField(TEXT("bShouldAlwaysTick"), ScalarBool(T->bShouldAlwaysTick)); return O;
}
static Object ModifierProperties(const UInputModifier* M)
{
	Object O = Obj();
	if (M->GetClass() == UInputModifierNegate::StaticClass())
	{
		const auto* V = CastChecked<UInputModifierNegate>(M); O->SetObjectField(TEXT("bX"), ScalarBool(V->bX)); O->SetObjectField(TEXT("bY"), ScalarBool(V->bY)); O->SetObjectField(TEXT("bZ"), ScalarBool(V->bZ));
	}
	else if (M->GetClass() == UInputModifierSwizzleAxis::StaticClass())
	{
		FString Order; switch (CastChecked<UInputModifierSwizzleAxis>(M)->Order)
		{
		case EInputAxisSwizzle::YXZ: Order = TEXT("YXZ"); break;
		case EInputAxisSwizzle::ZYX: Order = TEXT("ZYX"); break;
		case EInputAxisSwizzle::XZY: Order = TEXT("XZY"); break;
		case EInputAxisSwizzle::YZX: Order = TEXT("YZX"); break;
		case EInputAxisSwizzle::ZXY: Order = TEXT("ZXY"); break;
		default: break;
		}
		O->SetObjectField(TEXT("Order"), Order.IsEmpty() ? Unknown() : ScalarString(Order));
	}
	else if (M->GetClass() == UInputModifierDeadZone::StaticClass())
	{
		const auto* V = CastChecked<UInputModifierDeadZone>(M); O->SetObjectField(TEXT("LowerThreshold"), ScalarNumber(V->LowerThreshold)); O->SetObjectField(TEXT("UpperThreshold"), ScalarNumber(V->UpperThreshold));
		O->SetObjectField(TEXT("Type"), V->Type == EDeadZoneType::Axial ? ScalarString(TEXT("Axial")) : V->Type == EDeadZoneType::Radial ? ScalarString(TEXT("Radial")) : Unknown());
	}
	else return nullptr;
	return O;
}
template <typename T, typename F> static Object Instances(const TArray<TObjectPtr<T>>& Values, const UInputAction* Owner, F Properties)
{
	if (Values.Num() > 32) return Unknown();
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const TObjectPtr<T>& Ptr : Values)
	{
		if (!Ptr.IsResolved()) return Unknown(); // never resolve a lazy TObjectPtr while observing
		const T* Value = Ptr.Get();
		if (!Usable(Value) || Value->GetOuter() != Owner || !SafeClass(Value->GetClass())) return Unknown();
		Object Props = Properties(Value); if (!Props || Props->Values.Num() > 16) return Unknown();
		Object Item = Obj(); Item->SetStringField(TEXT("classPath"), Value->GetClass()->GetPathName()); Item->SetObjectField(TEXT("properties"), Props); Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	return Collection(Items); // preserves order and duplicate pointers/instances, never sorts/dedupes
}
static Object InputFacts(const UInputAction* IA)
{
	Object Facts = Obj(); FString Value;
	switch (IA->ValueType)
	{
	case EInputActionValueType::Boolean: Value = TEXT("Boolean"); break;
	case EInputActionValueType::Axis1D: Value = TEXT("Axis1D"); break;
	case EInputActionValueType::Axis2D: Value = TEXT("Axis2D"); break;
	case EInputActionValueType::Axis3D: Value = TEXT("Axis3D"); break;
	default: break;
	}
	Facts->SetObjectField(TEXT("valueType"), Value.IsEmpty() ? Unknown() : MeasuredString(Value)); Facts->SetObjectField(TEXT("triggerWhenPaused"), MeasuredBool(IA->bTriggerWhenPaused));
	Facts->SetObjectField(TEXT("actionTriggers"), Instances(IA->Triggers, IA, TriggerProperties)); Facts->SetObjectField(TEXT("actionModifiers"), Instances(IA->Modifiers, IA, ModifierProperties)); return Facts;
}
static FString Observe(const FString& Json, const FString& Family)
{
	// No blocking worker-thread marshal. Successful reads run synchronously within
	// one game-thread dispatch; an unsupported caller receives no observation.
	if (!IsInGameThread()) return TEXT("{\"success\":false,\"error\":\"Requested-state observation requires game-thread dispatch\"}");
	Object Req; if (!Request(Json, Family, Req)) return TEXT("{\"success\":false,\"error\":\"Invalid requested-state observation identity\"}");
	const double Start = NativeMilliseconds(); Object Out = Obj();
	for (const TCHAR* K : {TEXT("dispatchId"), TEXT("nonce"), TEXT("observer"), TEXT("observerVersion"), TEXT("taskId"), TEXT("turnId"), TEXT("generation"), TEXT("contractSha256"), TEXT("contractRevision")}) Out->SetField(K, Req->Values[K]);
	Out->SetNumberField(TEXT("version"), 2); Out->SetStringField(TEXT("observationId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower()); Out->SetNumberField(TEXT("startedAtMs"), Start);
	const FString RequestedPath = Req->GetStringField(TEXT("objectPath")), RequestedClass = Req->GetStringField(TEXT("assetClassPath"));
	Object Requested = Obj(); Requested->SetStringField(TEXT("objectPath"), RequestedPath); Requested->SetStringField(TEXT("assetClassPath"), RequestedClass); Out->SetObjectField(TEXT("requested"), Requested);
	UObject* Asset = FindLoaded(RequestedPath);
	Object Resolved = Obj(); Resolved->SetStringField(TEXT("objectPath"), Usable(Asset) ? Asset->GetPathName() : FString()); Resolved->SetStringField(TEXT("assetClassPath"), Usable(Asset) ? Asset->GetClass()->GetPathName() : FString()); Out->SetObjectField(TEXT("resolved"), Resolved);
	Object Facts = Obj(); bool Completed = false;
	if (Usable(Asset) && SafeClass(Asset->GetClass()) && Asset->GetPathName().Equals(RequestedPath, ESearchCase::CaseSensitive) && Asset->GetClass()->GetPathName().Equals(RequestedClass, ESearchCase::CaseSensitive))
	{
		if (Family == TEXT("blueprint_schema") && (Asset->GetClass() == UBlueprint::StaticClass() || Asset->GetClass() == UWidgetBlueprint::StaticClass())) { Facts = BlueprintFacts(CastChecked<UBlueprint>(Asset)); Completed = true; }
		else if (Family == TEXT("input_action") && Asset->GetClass() == UInputAction::StaticClass()) { Facts = InputFacts(CastChecked<UInputAction>(Asset)); Completed = true; }
	}
	Out->SetStringField(TEXT("status"), Completed ? TEXT("completed") : TEXT("failed")); Out->SetBoolField(TEXT("compilePerformed"), false); Out->SetBoolField(TEXT("savePerformed"), false); Out->SetObjectField(TEXT("facts"), Facts); Out->SetNumberField(TEXT("completedAtMs"), NativeMilliseconds());
	FString Result = Encode(Out); FTCHARToUTF8 Bytes(*Result);
	if (Bytes.Length() > MaxPayloadBytes) { Out->SetStringField(TEXT("status"), TEXT("partial")); Out->SetObjectField(TEXT("facts"), Obj()); Result = Encode(Out); }
	return Result;
}
} // namespace NwiroRequestedState

FString FNwiroRequestedStateTools::ObserveBlueprintState(const FString& JsonCommand) { return NwiroRequestedState::Observe(JsonCommand, TEXT("blueprint_schema")); }
FString FNwiroRequestedStateTools::ObserveInputActionState(const FString& JsonCommand) { return NwiroRequestedState::Observe(JsonCommand, TEXT("input_action")); }

FString FNwiroRequestedStateTools::DefinitionsJson()
{
	return TEXT("{\"name\":\"observe_blueprint_state\",\"description\":\"Host-coordinated loaded UBlueprint or UWidgetBlueprint parent and NewVariables only; no WidgetTree, CDO, load, compile or save. Incomplete facts are unknown.\",\"risk\":\"read_only\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"dispatchId\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"nonce\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"taskId\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"turnId\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"observerVersion\":{\"type\":\"integer\",\"enum\":[2]},\"generation\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991},\"contractSha256\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{64}$\",\"maxLength\":64},\"contractRevision\":{\"type\":\"integer\",\"enum\":[1]},\"objectPath\":{\"type\":\"string\",\"maxLength\":512,\"description\":\"Exact /Game/Package.Asset of an already loaded asset; no loading fallback.\"},\"assetClassPath\":{\"type\":\"string\",\"maxLength\":512,\"enum\":[\"/Script/Engine.Blueprint\",\"/Script/UMGEditor.WidgetBlueprint\"]},\"observer\":{\"type\":\"string\",\"enum\":[\"blueprint_schema\"]}},\"required\":[\"dispatchId\",\"nonce\",\"taskId\",\"turnId\",\"observerVersion\",\"generation\",\"contractSha256\",\"contractRevision\",\"objectPath\",\"assetClassPath\",\"observer\"],\"x-nwiro-requested-state\":{\"observer\":\"blueprint_schema\",\"version\":2,\"readOnly\":true,\"canonicalIdentities\":true,\"atomicGameThreadRead\":true,\"completeCollections\":true,\"noCompile\":true,\"noSave\":true}}},{\"name\":\"observe_input_action_state\",\"description\":\"Host-coordinated loaded authored-state facts; requires exact host identity. No load/CDO/compile/save. Incomplete facts are unknown, not successful verification.\",\"risk\":\"read_only\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"dispatchId\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"nonce\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"taskId\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"turnId\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$\",\"maxLength\":36},\"observerVersion\":{\"type\":\"integer\",\"enum\":[2]},\"generation\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991},\"contractSha256\":{\"type\":\"string\",\"pattern\":\"^[a-f0-9]{64}$\",\"maxLength\":64},\"contractRevision\":{\"type\":\"integer\",\"enum\":[1]},\"objectPath\":{\"type\":\"string\",\"maxLength\":512,\"description\":\"Exact /Game/Package.Asset of an already loaded asset; no loading fallback.\"},\"assetClassPath\":{\"type\":\"string\",\"maxLength\":512,\"enum\":[\"/Script/EnhancedInput.InputAction\"]},\"observer\":{\"type\":\"string\",\"enum\":[\"input_action\"]}},\"required\":[\"dispatchId\",\"nonce\",\"taskId\",\"turnId\",\"observerVersion\",\"generation\",\"contractSha256\",\"contractRevision\",\"objectPath\",\"assetClassPath\",\"observer\"],\"x-nwiro-requested-state\":{\"observer\":\"input_action\",\"version\":2,\"readOnly\":true,\"canonicalIdentities\":true,\"atomicGameThreadRead\":true,\"completeCollections\":true,\"noCompile\":true,\"noSave\":true}}},");
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroRequestedStateToolsSpec.inl"
#endif
