// Proposed native automation only; never run at module load or by observation.
#include "Misc/AutomationTest.h"
#include "NwiroToolDispatch.h"
#include "Blueprint/UserWidget.h"
// >>> NWIRO_PARITY BEGIN: requested-state-spec
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Class.h"
#include "Misc/ScopeExit.h"
#include "Editor.h"

namespace NwiroRequestedStateTest
{
using namespace NwiroRequestedState;
static Object RequestFor(const FString& Path, const FString& Class, const FString& Family)
{
	Object O = Obj();
	for (const TCHAR* Key : {TEXT("dispatchId"), TEXT("nonce"), TEXT("taskId"), TEXT("turnId")}) O->SetStringField(Key, FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower());
	O->SetStringField(TEXT("observer"), Family); O->SetNumberField(TEXT("observerVersion"), 2); O->SetNumberField(TEXT("generation"), 7); O->SetStringField(TEXT("contractSha256"), FString::ChrN(64, 'a')); O->SetNumberField(TEXT("contractRevision"), 1); O->SetStringField(TEXT("objectPath"), Path); O->SetStringField(TEXT("assetClassPath"), Class); return O;
}
static Object Read(const Object& Q, const FString& Family)
{
	Object O; const FString Raw = Observe(Encode(Q), Family); FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), O); return O;
}
static bool Complete(const Object& O) { return O.IsValid() && O->GetBoolField(TEXT("complete")); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroRequestedTypeSpec, "Nwiro.RequestedState.CanonicalTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroRequestedTypeSpec::RunTest(const FString&)
{
	using namespace NwiroRequestedState;
	FEdGraphPinType T; T.PinCategory = UEdGraphSchema_K2::PC_Int64;
	for (EPinContainerType C : {EPinContainerType::None, EPinContainerType::Array, EPinContainerType::Set})
	{
		T.ContainerType = C; const Object R = Type(T); TestTrue(TEXT("actual int64/container representable"), R->GetBoolField(TEXT("complete")));
		TestEqual(TEXT("int64 not collapsed to int32"), R->GetObjectField(TEXT("value"))->GetObjectField(TEXT("value"))->GetStringField(TEXT("base")), FString(TEXT("int64")));
	}
	T.ContainerType = EPinContainerType::Map; T.PinCategory = UEdGraphSchema_K2::PC_Name; T.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Real; T.PinValueType.TerminalSubCategory = UEdGraphSchema_K2::PC_Double;
	Object Map = Type(T)->GetObjectField(TEXT("value")); TestEqual(TEXT("map key is primary"), Map->GetObjectField(TEXT("key"))->GetStringField(TEXT("base")), FString(TEXT("name"))); TestEqual(TEXT("map value preserves double"), Map->GetObjectField(TEXT("value"))->GetStringField(TEXT("base")), FString(TEXT("double")));
	T.PinValueType.bTerminalIsConst = true; TestFalse(TEXT("map terminal const unrepresented"), Type(T)->GetBoolField(TEXT("complete"))); T.PinValueType.bTerminalIsConst = false;
	T.PinValueType.bTerminalIsWeakPointer = true; TestFalse(TEXT("map weak unrepresented"), Type(T)->GetBoolField(TEXT("complete"))); T.PinValueType.bTerminalIsWeakPointer = false;
	T.PinValueType.bTerminalIsUObjectWrapper = true; TestFalse(TEXT("map wrapper unrepresented"), Type(T)->GetBoolField(TEXT("complete")));
	T = FEdGraphPinType(); T.PinCategory = UEdGraphSchema_K2::PC_Object; T.PinSubCategoryObject = UObject::StaticClass(); T.bIsReference = true; T.bIsConst = true;
	Object ObjectType = Type(T)->GetObjectField(TEXT("value")); TestTrue(TEXT("reference retained"), ObjectType->GetBoolField(TEXT("isReference"))); TestTrue(TEXT("const retained"), ObjectType->GetBoolField(TEXT("isConst"))); TestEqual(TEXT("canonical object subtype"), ObjectType->GetObjectField(TEXT("value"))->GetStringField(TEXT("subtype")), FString(TEXT("/Script/CoreUObject.Object")));
	T.bIsUObjectWrapper = true; TestFalse(TEXT("wrapper unknown"), Type(T)->GetBoolField(TEXT("complete"))); T.bIsUObjectWrapper = false; T.bIsWeakPointer = true; TestFalse(TEXT("weak unknown"), Type(T)->GetBoolField(TEXT("complete"))); T.bIsWeakPointer = false;
	T.PinSubCategory = UEdGraphSchema_K2::PSC_Self; TestFalse(TEXT("self cannot invent subtype"), Type(T)->GetBoolField(TEXT("complete")));
	T = FEdGraphPinType(); T.PinCategory = UEdGraphSchema_K2::PC_Struct; TestFalse(TEXT("missing struct is not generic object"), Type(T)->GetBoolField(TEXT("complete")));
	T.PinCategory = UEdGraphSchema_K2::PC_Byte; T.PinSubCategoryObject = UObject::StaticClass(); TestFalse(TEXT("wrong enum UObject class rejected"), Type(T)->GetBoolField(TEXT("complete")));
	T.PinCategory = UEdGraphSchema_K2::PC_MCDelegate; TestFalse(TEXT("delegate remains incomplete"), Type(T)->GetBoolField(TEXT("complete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroRequestedLoadedBlueprintSpec, "Nwiro.RequestedState.LoadedBlueprintBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroRequestedLoadedBlueprintSpec::RunTest(const FString&)
{
	using namespace NwiroRequestedState; using namespace NwiroRequestedStateTest;
	const FString PackagePath = TEXT("/Game/_NwiroRequestedStateTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/BP_Facts");
	TStrongObjectPtr<UPackage> Package(CreatePackage(*PackagePath)); Package->SetFlags(RF_Transient);
	TStrongObjectPtr<UBlueprint> BP(NewObject<UBlueprint>(Package.Get(), TEXT("BP_Facts"), RF_Transient)); BP->ParentClass = UObject::StaticClass();
	FBPVariableDescription V; V.VarName = TEXT("Count"); V.VarType.PinCategory = UEdGraphSchema_K2::PC_Int; V.DefaultValue = TEXT("45"); BP->NewVariables.Add(V);
	V.VarName = TEXT("Changed"); V.VarType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate; BP->NewVariables.Add(V);
	Package->SetDirtyFlag(false); const FString Before = BP->NewVariables[0].DefaultValue;
	Object Q = RequestFor(BP->GetPathName(), BP->GetClass()->GetPathName(), TEXT("blueprint_schema")); Object A = Read(Q, TEXT("blueprint_schema")); Object B = Read(Q, TEXT("blueprint_schema"));
	TestEqual(TEXT("completed authored read"), A->GetStringField(TEXT("status")), FString(TEXT("completed"))); TestEqual(TEXT("exact path"), A->GetObjectField(TEXT("resolved"))->GetStringField(TEXT("objectPath")), BP->GetPathName());
	TestNotEqual(TEXT("fresh observation ID per call"), A->GetStringField(TEXT("observationId")), B->GetStringField(TEXT("observationId")));
	const Object Facts = A->GetObjectField(TEXT("facts")); const auto& Members = Facts->GetObjectField(TEXT("declaredVariables"))->GetArrayField(TEXT("items"));
	TestEqual(TEXT("all declared names including dispatcher"), Members.Num(), 2); TestFalse(TEXT("default export not runtime default proof"), Members[0]->AsObject()->GetObjectField(TEXT("authoredDefault"))->GetBoolField(TEXT("complete"))); TestFalse(TEXT("delegate type unknown"), Members[1]->AsObject()->GetObjectField(TEXT("type"))->GetBoolField(TEXT("complete")));
	TestEqual(TEXT("authored default unchanged"), BP->NewVariables[0].DefaultValue, Before); TestFalse(TEXT("no dirtiness introduced"), Package->IsDirty()); TestFalse(TEXT("no compile"), A->GetBoolField(TEXT("compilePerformed"))); TestFalse(TEXT("no save"), A->GetBoolField(TEXT("savePerformed")));
	BP->SetFlags(RF_NeedPostLoad); TestEqual(TEXT("postload pending object is not ready"), Read(Q, TEXT("blueprint_schema"))->GetStringField(TEXT("status")), FString(TEXT("failed"))); BP->ClearFlags(RF_NeedPostLoad);
	BP->SetInternalFlags(EInternalObjectFlags::AsyncLoadingPhase1); TestEqual(TEXT("async-owned object is not a completed snapshot"), Read(Q, TEXT("blueprint_schema"))->GetStringField(TEXT("status")), FString(TEXT("failed"))); BP->ClearInternalFlags(EInternalObjectFlags::AsyncLoadingPhase1);
	BP->NewVariables.SetNum(129); Object Over = Read(Q, TEXT("blueprint_schema"))->GetObjectField(TEXT("facts")); TestFalse(TEXT("overflow not truncated"), Over->GetObjectField(TEXT("declaredVariables"))->GetBoolField(TEXT("complete"))); TestTrue(TEXT("overflow does not erase parent fact"), Over->GetObjectField(TEXT("parentClassPath"))->GetBoolField(TEXT("complete")));
	Q->SetStringField(TEXT("objectPath"), PackagePath + TEXT(".Wrong")); Object Bad; TestFalse(TEXT("package/object mismatch admission rejected"), Request(Encode(Q), TEXT("blueprint_schema"), Bad));
	Q = RequestFor(TEXT("/Game/_NwiroAbsent/Absent.Absent"), TEXT("/Script/Engine.Blueprint"), TEXT("blueprint_schema")); TestNull(TEXT("target absent before"), FindLoaded(TEXT("/Game/_NwiroAbsent/Absent.Absent"))); Object Missing = Read(Q, TEXT("blueprint_schema")); TestEqual(TEXT("not loaded failed, not invented absence proof"), Missing->GetStringField(TEXT("status")), FString(TEXT("failed"))); TestTrue(TEXT("resolved identity not copied from request"), Missing->GetObjectField(TEXT("resolved"))->GetStringField(TEXT("objectPath")).IsEmpty()); TestNull(TEXT("no asset creation by observer"), FindLoaded(TEXT("/Game/_NwiroAbsent/Absent.Absent")));
	Q->SetStringField(TEXT("expectedParent"), TEXT("/Script/Engine.Actor")); TestFalse(TEXT("expected-outcome injection rejected"), Request(Encode(Q), TEXT("blueprint_schema"), Bad));
	Q = RequestFor(BP->GetPathName(), TEXT("/Script/EnhancedInput.InputAction"), TEXT("blueprint_schema"));
	TestEqual(TEXT("foreign requested class cannot relabel a Blueprint"), Read(Q, TEXT("blueprint_schema"))->GetStringField(TEXT("status")), FString(TEXT("failed")));
	Q->SetStringField(TEXT("dispatchId"), TEXT("not-a-uuid")); TestFalse(TEXT("malformed host identity rejected"), Request(Encode(Q), TEXT("blueprint_schema"), Bad));
	Q = RequestFor(BP->GetPathName(), BP->GetClass()->GetPathName(), TEXT("blueprint_schema")); Q->SetNumberField(TEXT("generation"), 7.5); TestFalse(TEXT("fractional generation rejected"), Request(Encode(Q), TEXT("blueprint_schema"), Bad));
	BP->NewVariables.SetNum(2); BP->NewVariables[0].VarName = TEXT("Same"); BP->NewVariables[1].VarName = TEXT("same"); TestFalse(TEXT("case-colliding declaration set cannot be complete"), Read(RequestFor(BP->GetPathName(), BP->GetClass()->GetPathName(), TEXT("blueprint_schema")), TEXT("blueprint_schema"))->GetObjectField(TEXT("facts"))->GetObjectField(TEXT("declaredVariables"))->GetBoolField(TEXT("complete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroRequestedInputSpec, "Nwiro.RequestedState.ActionProperties", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroRequestedInputSpec::RunTest(const FString&)
{
	using namespace NwiroRequestedState; using namespace NwiroRequestedStateTest;
	const FString PackagePath = TEXT("/Game/_NwiroRequestedStateTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/IA_Facts");
	TStrongObjectPtr<UPackage> Package(CreatePackage(*PackagePath)); Package->SetFlags(RF_Transient);
	TStrongObjectPtr<UInputAction> IA(NewObject<UInputAction>(Package.Get(), TEXT("IA_Facts"), RF_Transient)); IA->ValueType = EInputActionValueType::Axis2D; IA->bTriggerWhenPaused = true;
	UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(IA.Get()); Negate->bX = false; Negate->bY = true; Negate->bZ = false;
	UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(IA.Get()); Swizzle->Order = EInputAxisSwizzle::ZXY;
	IA->Modifiers = {Negate, Swizzle, Negate}; UInputTriggerPressed* Pressed = NewObject<UInputTriggerPressed>(IA.Get()); Pressed->ActuationThreshold = 0.75f; IA->Triggers.Add(Pressed); Package->SetDirtyFlag(false);
	Object Q = RequestFor(IA->GetPathName(), IA->GetClass()->GetPathName(), TEXT("input_action")); Object R = Read(Q, TEXT("input_action")); Object Facts = R->GetObjectField(TEXT("facts"));
	TestEqual(TEXT("actual value type"), Facts->GetObjectField(TEXT("valueType"))->GetStringField(TEXT("value")), FString(TEXT("Axis2D"))); TestTrue(TEXT("actual paused flag"), Facts->GetObjectField(TEXT("triggerWhenPaused"))->GetBoolField(TEXT("value")));
	const auto& Mods = Facts->GetObjectField(TEXT("actionModifiers"))->GetArrayField(TEXT("items")); TestEqual(TEXT("ordered duplicates retained"), Mods.Num(), 3); TestEqual(TEXT("exact swizzle class"), Mods[1]->AsObject()->GetStringField(TEXT("classPath")), Swizzle->GetClass()->GetPathName()); TestEqual(TEXT("actual nondefault order"), Mods[1]->AsObject()->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("Order"))->GetObjectField(TEXT("value"))->GetStringField(TEXT("value")), FString(TEXT("ZXY")));
	TestFalse(TEXT("actual false negation"), Mods[0]->AsObject()->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("bX"))->GetObjectField(TEXT("value"))->GetBoolField(TEXT("value"))); TestFalse(TEXT("observation not dirty"), Package->IsDirty());
	IA->Modifiers.Add(NewObject<UInputModifierScalar>(IA.Get())); TestFalse(TEXT("unreviewed modifier class makes collection incomplete"), Read(Q, TEXT("input_action"))->GetObjectField(TEXT("facts"))->GetObjectField(TEXT("actionModifiers"))->GetBoolField(TEXT("complete")));
	IA->Modifiers.Reset(); IA->Modifiers.Add(nullptr); TestFalse(TEXT("null instance not dropped"), InputFacts(IA.Get())->GetObjectField(TEXT("actionModifiers"))->GetBoolField(TEXT("complete")));
	IA->Modifiers.Reset(); IA->Modifiers.Add(NewObject<UInputModifierNegate>(Package.Get())); TestFalse(TEXT("foreign outer not accepted as action instance"), InputFacts(IA.Get())->GetObjectField(TEXT("actionModifiers"))->GetBoolField(TEXT("complete")));
	IA->Modifiers.Init(Negate, 33); TestFalse(TEXT("33 instances not truncated to32"), InputFacts(IA.Get())->GetObjectField(TEXT("actionModifiers"))->GetBoolField(TEXT("complete")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroRequestedDispatchSpec, "Nwiro.RequestedState.DispatchSpine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroRequestedDispatchSpec::RunTest(const FString&)
{
	using namespace NwiroRequestedState; using namespace NwiroRequestedStateTest;
	TArray<TSharedPtr<FJsonValue>> Definitions;
	if (!TestTrue(TEXT("actual tool inventory decodes"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroToolDispatch::GetEnabledToolDefinitionsJson()), Definitions))) return false;
	for (const FString Name : {FString(TEXT("observe_blueprint_state")), FString(TEXT("observe_input_action_state"))})
	{
		int32 Matches = 0;
		for (const auto& Entry : Definitions) if (Entry->AsObject()->GetStringField(TEXT("name")) == Name)
		{
			++Matches; Object Schema = Entry->AsObject()->GetObjectField(TEXT("inputSchema"));
			TestFalse(TEXT("no unknown request fields"), Schema->GetBoolField(TEXT("additionalProperties"))); TestEqual(TEXT("eleven host identity fields"), Schema->GetArrayField(TEXT("required")).Num(), 11);
			TestTrue(TEXT("read-only capability retained by inventory"), Schema->GetObjectField(TEXT("x-nwiro-requested-state"))->GetBoolField(TEXT("readOnly")));
		}
		TestEqual(TEXT("one exact tool definition"), Matches, 1);
	}
	const FString Base = TEXT("/Game/_NwiroRequestedStateDispatch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPackage> WidgetPackage(CreatePackage(*(Base + TEXT("/WBP_Facts")))); WidgetPackage->SetFlags(RF_Transient);
	TStrongObjectPtr<UWidgetBlueprint> Widget(NewObject<UWidgetBlueprint>(WidgetPackage.Get(), TEXT("WBP_Facts"), RF_Transient)); Widget->ParentClass = UUserWidget::StaticClass();
	FBPVariableDescription Member; Member.VarName = TEXT("LabelString"); Member.VarType.PinCategory = UEdGraphSchema_K2::PC_String; Member.DefaultValue = TEXT("Preserved"); Widget->NewVariables.Add(Member); WidgetPackage->SetDirtyFlag(false);
	TStrongObjectPtr<UPackage> ActionPackage(CreatePackage(*(Base + TEXT("/IA_Facts")))); ActionPackage->SetFlags(RF_Transient);
	TStrongObjectPtr<UInputAction> Action(NewObject<UInputAction>(ActionPackage.Get(), TEXT("IA_Facts"), RF_Transient)); Action->ValueType = EInputActionValueType::Axis1D; Action->bTriggerWhenPaused = true; ActionPackage->SetDirtyFlag(false);
	const UObject* WidgetClassBefore = Widget->GetClass(); const auto ParentHandleBefore = Widget->ParentClass.GetGCPtr(); const auto GeneratedHandleBefore = Widget->GeneratedClass;
	const EObjectFlags WidgetFlagsBefore = Widget->GetFlags(); const EObjectFlags ActionFlagsBefore = Action->GetFlags();
	auto Dispatch = [](const FString& Tool, const Object& Q) { Object Out; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroToolDispatch::DispatchTool(Tool, Encode(Q))), Out); return Out; };
	Object WQ = RequestFor(Widget->GetPathName(), Widget->GetClass()->GetPathName(), TEXT("blueprint_schema"));
	Object WR = Dispatch(TEXT("observe_blueprint_state"), WQ);
	if (!TestTrue(TEXT("widget dispatch returns JSON"), WR.IsValid())) return false;
	TestEqual(TEXT("loaded widget authored subset supported"), WR->GetStringField(TEXT("status")), FString(TEXT("completed")));
	TestEqual(TEXT("dispatch identity retained"), WR->GetStringField(TEXT("dispatchId")), WQ->GetStringField(TEXT("dispatchId")));
	TestEqual(TEXT("actual canonical widget class"), WR->GetObjectField(TEXT("resolved"))->GetStringField(TEXT("assetClassPath")), FString(TEXT("/Script/UMGEditor.WidgetBlueprint")));
	const Object WF = WR->GetObjectField(TEXT("facts")); TestEqual(TEXT("actual widget parent"), WF->GetObjectField(TEXT("parentClassPath"))->GetStringField(TEXT("value")), UUserWidget::StaticClass()->GetPathName());
	TestEqual(TEXT("only declared variable measured"), WF->GetObjectField(TEXT("declaredVariables"))->GetArrayField(TEXT("items")).Num(), 1);
	TestFalse(TEXT("no widget tree facts"), WF->HasField(TEXT("widgetTree")));
	Object AQ = RequestFor(Action->GetPathName(), Action->GetClass()->GetPathName(), TEXT("input_action")); Object AR = Dispatch(TEXT("observe_input_action_state"), AQ);
	if (!TestTrue(TEXT("action dispatch returns JSON"), AR.IsValid())) return false;
	TestEqual(TEXT("action dispatch exact resolved path"), AR->GetObjectField(TEXT("resolved"))->GetStringField(TEXT("objectPath")), Action->GetPathName());
	TestEqual(TEXT("action actual valueType via dispatch"), AR->GetObjectField(TEXT("facts"))->GetObjectField(TEXT("valueType"))->GetStringField(TEXT("value")), FString(TEXT("Axis1D")));
	TestEqual(TEXT("widget member default export unchanged"), Widget->NewVariables[0].DefaultValue, FString(TEXT("Preserved"))); TestEqual(TEXT("widget declaration count unchanged"), Widget->NewVariables.Num(), 1);
	TestTrue(TEXT("parent handle unchanged"), Widget->ParentClass.GetGCPtr() == ParentHandleBefore); TestTrue(TEXT("generated handle unchanged without resolving"), Widget->GeneratedClass == GeneratedHandleBefore); TestTrue(TEXT("asset class pointer unchanged"), Widget->GetClass() == WidgetClassBefore);
	TestTrue(TEXT("widget object flags unchanged"), Widget->GetFlags() == WidgetFlagsBefore); TestTrue(TEXT("action object flags unchanged"), Action->GetFlags() == ActionFlagsBefore); TestFalse(TEXT("widget package stays clean"), WidgetPackage->IsDirty()); TestFalse(TEXT("action package stays clean"), ActionPackage->IsDirty());
	WQ->SetStringField(TEXT("assetClassPath"), Action->GetClass()->GetPathName()); TestEqual(TEXT("public dispatch rejects class relabeling"), Dispatch(TEXT("observe_blueprint_state"), WQ)->GetStringField(TEXT("status")), FString(TEXT("failed")));
	WQ->SetStringField(TEXT("expectedParent"), TEXT("/Script/Engine.Actor")); TestFalse(TEXT("foreign fields rejected through dispatch"), Dispatch(TEXT("observe_blueprint_state"), WQ)->GetBoolField(TEXT("success")));
	Object Missing = RequestFor(Base + TEXT("/Absent.Absent"), TEXT("/Script/Engine.Blueprint"), TEXT("blueprint_schema"));
	TestNull(TEXT("absent before public dispatch"), FindLoaded(Base + TEXT("/Absent.Absent"))); TestEqual(TEXT("missing stays failed"), Dispatch(TEXT("observe_blueprint_state"), Missing)->GetStringField(TEXT("status")), FString(TEXT("failed"))); TestNull(TEXT("no missing object created"), FindLoaded(Base + TEXT("/Absent.Absent")));
	return true;
}


// V2 public-dispatch negatives. These are proposed C++ tests, not executed evidence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroRequestedAdmissionSpec, "Nwiro.RequestedState.AdmissionReadinessAndIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroRequestedAdmissionSpec::RunTest(const FString&)
{
    using namespace NwiroRequestedState; using namespace NwiroRequestedStateTest;
    if (!GEditor || !IsInGameThread()) { AddError(TEXT("Requires actual editor game-thread dispatch")); return false; }
    const FString Base = TEXT("/Game/_NwiroRequestedAdmission_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UPackage> BPPackage(CreatePackage(*(Base + TEXT("/BP_Facts")))); BPPackage->SetFlags(RF_Transient);
    TStrongObjectPtr<UBlueprint> BP(NewObject<UBlueprint>(BPPackage.Get(), TEXT("BP_Facts"), RF_Transient)); BP->ParentClass = UObject::StaticClass();
    FBPVariableDescription Member; Member.VarName = TEXT("Count"); Member.VarType.PinCategory = UEdGraphSchema_K2::PC_Int; Member.DefaultValue = TEXT("42"); BP->NewVariables.Add(Member);
    TStrongObjectPtr<UPackage> IAPackage(CreatePackage(*(Base + TEXT("/IA_Facts")))); IAPackage->SetFlags(RF_Transient);
    TStrongObjectPtr<UInputAction> IA(NewObject<UInputAction>(IAPackage.Get(), TEXT("IA_Facts"), RF_Transient)); IA->ValueType = EInputActionValueType::Axis2D;
    UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(IA.Get()); IA->Modifiers.Add(Negate);
    TStrongObjectPtr<UClass> OwnedParent(NewObject<UClass>(BPPackage.Get(), TEXT("OwnedParent"), RF_Transient));
    TStrongObjectPtr<UScriptStruct> OwnedSubtype(NewObject<UScriptStruct>(BPPackage.Get(), TEXT("OwnedSubtype"), RF_Transient));
    int32 TargetCompiles = 0, TargetSaves = 0;
    const FDelegateHandle CH = GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* Seen) { if (Seen == BP.Get()) ++TargetCompiles; });
    const FDelegateHandle SH = UPackage::PreSavePackageWithContextEvent.AddLambda([&](UPackage* Seen, FObjectPreSaveContext) { if (Seen == BPPackage.Get() || Seen == IAPackage.Get()) ++TargetSaves; });
    ON_SCOPE_EXIT { GEditor->OnBlueprintPreCompile().Remove(CH); UPackage::PreSavePackageWithContextEvent.Remove(SH); };
    auto Dispatch = [](const FString& Tool, const Object& Q) { Object Out; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroToolDispatch::DispatchTool(Tool, Encode(Q))), Out); return Out; };
    const Object BQ = RequestFor(BP->GetPathName(), BP->GetClass()->GetPathName(), TEXT("blueprint_schema"));
    const Object AQ = RequestFor(IA->GetPathName(), IA->GetClass()->GetPathName(), TEXT("input_action"));
    for (bool Dirty : {false, true})
    {
        BPPackage->SetDirtyFlag(Dirty); IAPackage->SetDirtyFlag(Dirty);
        for (EObjectFlags Flag : {RF_NeedInitialization, RF_WillBeLoaded})
        {
            // Object-local lifecycle simulation; this does not exercise a real async loader.
            BP->SetFlags(Flag); IA->SetFlags(Flag);
            ON_SCOPE_EXIT { BP->ClearFlags(Flag); IA->ClearFlags(Flag); };
            const EObjectFlags BeforeBP = BP->GetFlags(), BeforeIA = IA->GetFlags();
            const Object BR = Dispatch(TEXT("observe_blueprint_state"), BQ), AR = Dispatch(TEXT("observe_input_action_state"), AQ);
            if (!BR || !AR) { AddError(TEXT("Actual dispatch must return JSON")); return false; }
            TestEqual(TEXT("Unready Blueprint cannot publish completed facts"), BR->GetStringField(TEXT("status")), FString(TEXT("failed")));
            TestEqual(TEXT("Unready InputAction cannot publish completed facts"), AR->GetStringField(TEXT("status")), FString(TEXT("failed")));
            TestEqual(TEXT("No Blueprint facts from unready object"), BR->GetObjectField(TEXT("facts"))->Values.Num(), 0);
            TestEqual(TEXT("No InputAction facts from unready object"), AR->GetObjectField(TEXT("facts"))->Values.Num(), 0);
            TestTrue(TEXT("Observer leaves Blueprint lifecycle flags unchanged"), BP->GetFlags() == BeforeBP);
            TestTrue(TEXT("Observer leaves action lifecycle flags unchanged"), IA->GetFlags() == BeforeIA);
            TestEqual(TEXT("Existing Blueprint dirty state preserved"), BPPackage->IsDirty(), Dirty);
            TestEqual(TEXT("Existing action dirty state preserved"), IAPackage->IsDirty(), Dirty);
            TestEqual(TEXT("No target compiler delegate during refusal"), TargetCompiles, 0);
            TestEqual(TEXT("No target save delegate during refusal"), TargetSaves, 0);
        }
    }
    for (EObjectFlags Flag : {RF_NeedInitialization, RF_WillBeLoaded})
    {
        Negate->SetFlags(Flag); OwnedParent->SetFlags(Flag); OwnedSubtype->SetFlags(Flag);
        // Assign the already loaded raw handle, avoiding TSubclassOf runtime hierarchy traversal in setup.
        BP->ParentClass.GetGCPtr() = OwnedParent.Get(); BP->NewVariables[0].VarType.PinCategory = UEdGraphSchema_K2::PC_Struct; BP->NewVariables[0].VarType.PinSubCategoryObject = OwnedSubtype.Get();
        ON_SCOPE_EXIT { Negate->ClearFlags(Flag); OwnedParent->ClearFlags(Flag); OwnedSubtype->ClearFlags(Flag); BP->ParentClass = UObject::StaticClass(); BP->NewVariables[0].VarType = Member.VarType; };
        const Object BF = Dispatch(TEXT("observe_blueprint_state"), BQ)->GetObjectField(TEXT("facts"));
        const Object AF = Dispatch(TEXT("observe_input_action_state"), AQ)->GetObjectField(TEXT("facts"));
        TestFalse(TEXT("Unready raw parent yields unknown without subclass walk"), BF->GetObjectField(TEXT("parentClassPath"))->GetBoolField(TEXT("complete")));
        TestTrue(TEXT("Authored declaration names remain independently measured"), BF->GetObjectField(TEXT("declaredVariables"))->GetBoolField(TEXT("complete")));
        TestFalse(TEXT("Unready subtype never becomes measured canonical type"), BF->GetObjectField(TEXT("declaredVariables"))->GetArrayField(TEXT("items"))[0]->AsObject()->GetObjectField(TEXT("type"))->GetBoolField(TEXT("complete")));
        TestFalse(TEXT("Unready nested modifier makes collection incomplete"), AF->GetObjectField(TEXT("actionModifiers"))->GetBoolField(TEXT("complete")));
        TestTrue(TEXT("Independent action value type remains measured"), AF->GetObjectField(TEXT("valueType"))->GetBoolField(TEXT("complete")));
    }
    auto CopyRequest = [](const Object& Q) { Object Copy = Obj(); Copy->Values = Q->Values; return Copy; };
    for (const TCHAR* Key : {TEXT("dispatchId"), TEXT("nonce"), TEXT("taskId"), TEXT("turnId")})
    {
        Object Q = CopyRequest(BQ); Q->SetStringField(Key, TEXT("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
        TestFalse(TEXT("Uppercase UUID rejected through real dispatch"), Dispatch(TEXT("observe_blueprint_state"), Q)->GetBoolField(TEXT("success")));
    }
    Object CaseObserver = CopyRequest(BQ); CaseObserver->SetStringField(TEXT("observer"), TEXT("BLUEPRINT_SCHEMA"));
    TestFalse(TEXT("Observer enum is case-sensitive"), Dispatch(TEXT("observe_blueprint_state"), CaseObserver)->GetBoolField(TEXT("success")));
    Object CaseMount = CopyRequest(BQ); CaseMount->SetStringField(TEXT("objectPath"), BP->GetPathName().Replace(TEXT("/Game/"), TEXT("/game/"), ESearchCase::CaseSensitive));
    TestFalse(TEXT("Mount case is canonical"), Dispatch(TEXT("observe_blueprint_state"), CaseMount)->GetBoolField(TEXT("success")));
    Object CaseLeaf = CopyRequest(BQ); CaseLeaf->SetStringField(TEXT("objectPath"), Base + TEXT("/BP_Facts.bp_facts"));
    TestFalse(TEXT("Package/object leaf comparison is exact"), Dispatch(TEXT("observe_blueprint_state"), CaseLeaf)->GetBoolField(TEXT("success")));
    Object CaseIdentity = CopyRequest(BQ); CaseIdentity->SetStringField(TEXT("objectPath"), Base + TEXT("/bp_facts.bp_facts"));
    TestEqual(TEXT("Case-folded hash lookup cannot satisfy exact object identity"), Dispatch(TEXT("observe_blueprint_state"), CaseIdentity)->GetStringField(TEXT("status")), FString(TEXT("failed")));
    Object CaseClass = CopyRequest(BQ); CaseClass->SetStringField(TEXT("assetClassPath"), TEXT("/Script/Engine.blueprint"));
    TestEqual(TEXT("Resolved class identity is case-sensitive"), Dispatch(TEXT("observe_blueprint_state"), CaseClass)->GetStringField(TEXT("status")), FString(TEXT("failed")));
    TestEqual(TEXT("Authored default remains unchanged"), BP->NewVariables[0].DefaultValue, FString(TEXT("42")));
    TestEqual(TEXT("No measured target compiles"), TargetCompiles, 0); TestEqual(TEXT("No measured target saves"), TargetSaves, 0);
    return true;
}
// <<< NWIRO_PARITY END: requested-state-spec
