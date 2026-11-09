#define NWIRO_PIN_DISPATCH FNwiroToolDispatch::DispatchTool
// >>> NWIRO_PARITY BEGIN: pin-batch-fixture
// Proposed native regression suite. Included in the owning tools TU so the pure
// resolver can also be tested; behavioral claims below use the real tool dispatcher.
#include "UObject/ObjectSaveContext.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/AutomationTest.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Algo/Reverse.h"
#include "BlueprintEditorSettings.h"

namespace NwiroPinBatchSpec
{
    static TSharedRef<FJsonObject> Object(const FString& Json)
    {
        TSharedPtr<FJsonObject> Out;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        check(FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid());
        return Out.ToSharedRef();
    }
    static FString Dispatch(UBlueprint* BP, const FString& Fields)
    {
        const auto Cmd = Object(TEXT("{") + Fields + TEXT("}"));
        Cmd->SetStringField(TEXT("blueprint"), BP->GetPathName());
        return NWIRO_PIN_DISPATCH(TEXT("edit_blueprint"), NwiroBlueprintBatch::Json(Cmd));
    }
    static UBlueprint* Fixture()
    {
        const FString Name = TEXT("BP_PinBatch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        UPackage* Package = CreatePackage(*(FString(TEXT(NWIRO_SPEC_PKG)) + Name));
        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
            FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        if (!BP || BP->UbergraphPages.IsEmpty()) return nullptr;
        UEdGraph* Graph = BP->UbergraphPages[0];
        for (const TCHAR* FunctionName : {TEXT("MakeVector"), TEXT("VInterpTo")})
        {
            UFunction* Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(FunctionName));
            if (!Function) return nullptr;
            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            UK2Node_CallFunction* Node = Creator.CreateNode();
            Node->SetFromFunction(Function);
            Creator.Finalize();
        }
        FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
        if (!UEditorAssetLibrary::SaveLoadedAsset(BP, false)) return nullptr;
        return BP;
    }
    static UK2Node_CallFunction* Function(UBlueprint* BP, const TCHAR* Name)
    {
        for (UEdGraphNode* Node : BP->UbergraphPages[0]->Nodes)
            if (auto Call = Cast<UK2Node_CallFunction>(Node))
                if (Call->FunctionReference.GetMemberName() == FName(Name)) return Call;
        return nullptr;
    }
    static FString Endpoint(UEdGraphNode* Node, const TCHAR* Pin)
    { return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) + TEXT(".") + Pin; }
    static FString Snapshot(UBlueprint* BP)
    {
        TArray<FString> Rows;
        for (const auto& Variable : BP->NewVariables)
            Rows.Add(TEXT("var:") + Variable.VarName.ToString() + Variable.VarGuid.ToString()
                + Variable.VarType.PinCategory.ToString() + Variable.DefaultValue
                + FString::Printf(TEXT(":%llu"), Variable.PropertyFlags));
        if (BP->SimpleConstructionScript) for (const USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
        {
            Rows.Add(TEXT("component:") + Node->GetVariableName().ToString());
            for (const USCS_Node* Child : Node->GetChildNodes())
                Rows.Add(TEXT("child:") + Node->GetVariableName().ToString() + TEXT("/") + Child->GetVariableName().ToString());
        }
        TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            Rows.Add(TEXT("graph:") + Graph->GetPathName());
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                Rows.Add(TEXT("node:") + Node->NodeGuid.ToString());
                for (const UEdGraphPin* Pin : Node->Pins) if (Pin)
                {
                    FString Row = Pin->PinId.ToString() + TEXT(":") + Pin->PinName.ToString()
                        + TEXT(":") + Pin->PinType.PinCategory.ToString() + TEXT(":") + Pin->PinType.PinSubCategory.ToString()
                        + TEXT(":") + GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get())
                        + FString::Printf(TEXT(":%d:%d"), static_cast<int32>(Pin->PinType.ContainerType), static_cast<int32>(Pin->Direction))
                        + TEXT(":") + Pin->DefaultValue + TEXT(":") + Pin->DefaultTextValue.ToString()
                        + TEXT(":") + GetPathNameSafe(Pin->DefaultObject);
                    TArray<FString> Links;
                    for (const UEdGraphPin* Peer : Pin->LinkedTo) if (Peer)
                        Links.Add(Peer->GetOwningNode()->NodeGuid.ToString() + Peer->PinId.ToString());
                    Links.Sort(); Row += TEXT("->") + FString::Join(Links, TEXT(","));
                    Rows.Add(Row);
                }
            }
        }
        return FString::Join(Rows, TEXT("\n"));
    }
    struct FWatch
    {
        int32 Compiles = 0, Saves = 0;
        FDelegateHandle CompileHandle, SaveHandle;
        explicit FWatch(UBlueprint* BP)
        {
            CompileHandle = GEditor->OnBlueprintPreCompile().AddLambda([this, BP](UBlueprint* Item) { if (Item == BP) ++Compiles; });
            SaveHandle = UPackage::PreSavePackageWithContextEvent.AddLambda([this, BP](UPackage* Package, FObjectPreSaveContext)
            { if (Package == BP->GetOutermost()) ++Saves; });
        }
        ~FWatch() { GEditor->OnBlueprintPreCompile().Remove(CompileHandle); UPackage::PreSavePackageWithContextEvent.Remove(SaveHandle); }
    };
}

// <<< NWIRO_PARITY END: pin-batch-fixture
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchReversedPeersSpec, NWIRO_SPEC_PREFIX "PinBatch.ReversedInvalidPeersNoTouch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchReversedPeersSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    int32 MeasuredCases = 0;
    const int32 PreviousStrict = CVarStrictConnectPins.GetValueOnGameThread();
    ON_SCOPE_EXIT { CVarStrictConnectPins->Set(PreviousStrict, ECVF_SetByCode); };
    CVarStrictConnectPins->Set(0, ECVF_SetByCode); // legacy permissive mode cannot bypass admission
    for (const TCHAR* Operation : {TEXT("connect_pins"), TEXT("set_pin_defaults"), TEXT("break_connections")})
    for (bool Dirty : {false, true}) for (bool Reverse : {false, true}) for (int32 CompileMode : {0, 1, 2})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Real compiled and saved Blueprint fixture"), BP.Get())) return false;
        auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
        if (!TestNotNull(TEXT("MakeVector"), A) || !TestNotNull(TEXT("VInterpTo"), B)) return false;
        FString Valid, Invalid;
        if (FString(Operation) == TEXT("connect_pins"))
        {
            Valid = FString::Printf(TEXT("{\"from\":\"%s\",\"to\":\"%s\"}"), *Endpoint(A,TEXT("ReturnValue")), *Endpoint(B,TEXT("Target")));
            Invalid = FString::Printf(TEXT("{\"from\":\"%s\",\"to\":\"%s\"}"), *Endpoint(A,TEXT("ReturnValue")), *Endpoint(B,TEXT("NoSuchPin")));
        }
        else if (FString(Operation) == TEXT("set_pin_defaults"))
        {
            Valid = FString::Printf(TEXT("{\"ref\":\"%s\",\"pin\":\"X\",\"value\":\"43\"}"), *A->NodeGuid.ToString());
            Invalid = FString::Printf(TEXT("{\"ref\":\"%s\",\"pin\":\"NoSuchPin\",\"value\":\"77\"}"), *A->NodeGuid.ToString());
        }
        else
        {
            GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A->FindPin(TEXT("ReturnValue")), B->FindPin(TEXT("Target")));
            Valid = FString::Printf(TEXT("{\"ref\":\"%s\",\"pin\":\"Target\"}"), *B->NodeGuid.ToString());
            Invalid = FString::Printf(TEXT("{\"ref\":\"%s\",\"pin\":\"NoSuchPin\"}"), *B->NodeGuid.ToString());
        }
        if (Dirty)
        {
            // A genuine unsaved authored change, distinct from the on-disk fixture.
            GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*A->FindPin(TEXT("Y")), TEXT("173.25"));
        }
        BP->GetOutermost()->SetDirtyFlag(Dirty);
        const FString Before = Snapshot(BP.Get()); const auto Status = BP->Status;
        const bool Dependencies = BP->bCachedDependenciesUpToDate;
        const FString Filename = FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
        TArray<uint8> BytesBefore, BytesAfter;
        if (!TestTrue(TEXT("Saved fixture readable"), FFileHelper::LoadFileToArray(BytesBefore, *Filename))) return false;
        FWatch Watch(BP.Get());
        const FString Fields = (CompileMode == 2 ? FString() : FString::Printf(TEXT("\"compile\":%s,"), CompileMode ? TEXT("true") : TEXT("false")))
            + FString::Printf(TEXT("\"%s\":[%s,%s]"), Operation, *(Reverse ? Invalid : Valid), *(Reverse ? Valid : Invalid));
        const auto Receipt = Object(Dispatch(BP.Get(), Fields));
        TestFalse(TEXT("Whole invalid request rejected"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("Authored graph, defaults, links, members unchanged"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("Existing dirty flag preserved"), BP->GetOutermost()->IsDirty(), Dirty);
        TestEqual(TEXT("Blueprint status preserved"), BP->Status, Status);
        TestEqual(TEXT("Dependency status preserved"), static_cast<bool>(BP->bCachedDependenciesUpToDate), Dependencies);
        TestEqual(TEXT("Zero target compile attempts"), Watch.Compiles, 0);
        TestEqual(TEXT("Zero target save attempts"), Watch.Saves, 0);
        TestTrue(TEXT("Saved bytes readable after rejection"), FFileHelper::LoadFileToArray(BytesAfter, *Filename));
        TestTrue(TEXT("Saved bytes unchanged"), BytesAfter == BytesBefore);
        TestEqual(TEXT("Two units rejected before target mutation"), Receipt->GetObjectField(TEXT("batchAdmission"))->GetNumberField(TEXT("rejectedUnits")), 2.0);
        ++MeasuredCases;
    }
    TestEqual(TEXT("Every operation/order/dirty/compile combination ran"), MeasuredCases, 36);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchCanonicalAndMixedSpec, NWIRO_SPEC_PREFIX "PinBatch.CanonicalTargetAndMixedGeneratedRefs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchCanonicalAndMixedSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (bool ReversePins : {false, true})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
        auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
        UEdGraphPin* Target = B->FindPin(TEXT("Target")); UEdGraphPin* Self = B->FindPin(TEXT("self"));
        if (!TestNotNull(TEXT("Canonical vector Target"), Target) || !TestNotNull(TEXT("Library self pin"), Self)) return false;
        Self->PinFriendlyName = FText::FromString(TEXT("Target"));
        if (ReversePins) Algo::Reverse(B->Pins);
        const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"compile\":false,\"connect_pins\":[{\"from\":\"%s\",\"to\":\"%s\"}]"),
            *Endpoint(A,TEXT("ReturnValue")), *Endpoint(B,TEXT("Target")))));
        TestTrue(TEXT("Canonical Target connect admitted"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("Vector Target linked"), Target->LinkedTo.Contains(A->FindPin(TEXT("ReturnValue"))));
        TestEqual(TEXT("Friendly library self unchanged"), Self->LinkedTo.Num(), 0);
        const auto Mixed = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"compile\":false,\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"newVector\"}],\"connect_pins\":[{\"from\":\"newVector.ReturnValue\",\"to\":\"%s\"}]"), *Endpoint(B,TEXT("Target")))));
        TestTrue(TEXT("Valid new-node plus connect batch remains supported"), Mixed->GetBoolField(TEXT("success")));
        TestEqual(TEXT("Single resolved generated peer linked to Target"), Target->LinkedTo.Num(), 1);
        TestTrue(TEXT("Mixed link uses new node rather than old MakeVector"), Target->LinkedTo.Num() == 1 && Target->LinkedTo[0]->GetOwningNode() != A);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchMixedInvalidSpec, NWIRO_SPEC_PREFIX "PinBatch.MixedInvalidAndMissingSubtypeNoTouch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchMixedInvalidSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (const TCHAR* Variables : {TEXT("{\"name\":\"PeerValue\",\"type\":\"float\"}"),
        TEXT("{\"name\":\"PeerValue\",\"type\":{\"base\":\"object\",\"subtype\":\"NoSuchBatchSubtype\"}}")})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
        const FString Before = Snapshot(BP.Get()); FWatch Watch(BP.Get());
        const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"add_variables\":[%s],\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"newVector\"}],\"connect_pins\":[{\"from\":\"newVector.ReturnValue\",\"to\":\"newVector.NoSuchPin\"}]"), Variables)));
        TestFalse(TEXT("Invalid later peer/subtype rejects whole request"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("No earlier variable/node mutation"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
        const auto Probe = Object(Dispatch(BP.Get(), TEXT("\"compile\":false,\"set_pin_defaults\":[{\"ref\":\"newVector\",\"pin\":\"X\",\"value\":\"9\"}]")));
        TestFalse(TEXT("Rejected staging refs did not escape into target session"), Probe->GetBoolField(TEXT("success")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchCommitFailureSpec, NWIRO_SPEC_PREFIX "PinBatch.UnexpectedCommitFailureIsTruthful",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchCommitFailureSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
    auto Previous = MoveTemp(NwiroBlueprintBatch::BeforeCommitOperation);
    ON_SCOPE_EXIT { NwiroBlueprintBatch::BeforeCommitOperation = MoveTemp(Previous); };
    int32 Injected = 0;
    NwiroBlueprintBatch::BeforeCommitOperation = [&](UBlueprint* Current, const FString& Key)
    {
        if (Current == BP.Get() && Key == TEXT("set_pin_defaults"))
        { ++Injected; A->RemovePin(A->FindPin(TEXT("X"))); }
    };
    FWatch Watch(BP.Get());
    const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"connect_pins\":[{\"from\":\"%s\",\"to\":\"%s\"}],\"set_pin_defaults\":[{\"ref\":\"%s\",\"pin\":\"X\",\"value\":\"5\"}]"),
        *Endpoint(A,TEXT("ReturnValue")), *Endpoint(B,TEXT("Target")), *A->NodeGuid.ToString())));
    TestEqual(TEXT("Failure injected after real admission and first operation"), Injected, 1);
    TestFalse(TEXT("Unexpected commit failure is not success"), Receipt->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Earlier admitted connection really landed"), B->FindPin(TEXT("Target"))->LinkedTo.Contains(A->FindPin(TEXT("ReturnValue"))));
    TestEqual(TEXT("Partial state reported truthfully"), Receipt->GetObjectField(TEXT("batchAdmission"))->GetStringField(TEXT("currentRequestMutation")), FString(TEXT("partial_or_unknown")));
    TestEqual(TEXT("Commit failure blocks wrapper compile"), Watch.Compiles, 0);
    TestEqual(TEXT("Commit failure blocks save"), Watch.Saves, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchResolverSpec, NWIRO_SPEC_PREFIX "PinBatch.ResolverTierAmbiguityAndDirection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchResolverSpec::RunTest(const FString&)
{
    TStrongObjectPtr<UEdGraphNode> Node(NewObject<UEdGraphNode>(GetTransientPackage()));
    UEdGraphPin* Literal = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("Target_Value"));
    UEdGraphPin* Other = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("TargetValue"));
    UEdGraphPin* Opposite = Node->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("Target_Value"));
    FString Error;
    TestTrue(TEXT("Exact literal remains independently addressable"), NwiroPinResolution::Resolve(Node.Get(), TEXT("Target_Value"), EGPD_Input, &Error) == Literal);
    TestTrue(TEXT("Opposite direction is filtered before ranking"), NwiroPinResolution::Resolve(Node.Get(), TEXT("Target_Value"), EGPD_Output, &Error) == Opposite);
    TestNull(TEXT("Normalized identity collision rejects even without diagnostic sink"), NwiroPinResolution::Resolve(Node.Get(), TEXT("target-value"), EGPD_Input, nullptr));
    Literal->PinFriendlyName = FText::FromString(TEXT("Result")); Other->PinFriendlyName = Literal->PinFriendlyName;
    TestNull(TEXT("Friendly collision rejects"), NwiroPinResolution::Resolve(Node.Get(), TEXT("Result"), EGPD_Input, &Error));
    TestTrue(TEXT("Ambiguity diagnostic is explicit"), Error.Contains(TEXT("AMBIGUOUS_PIN")));
    Algo::Reverse(Node->Pins);
    TestTrue(TEXT("Canonical result does not depend on array order"), NwiroPinResolution::Resolve(Node.Get(), TEXT("Target_Value"), EGPD_Input, nullptr) == Literal);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchGeneratedAmbiguitySpec, NWIRO_SPEC_PREFIX "PinBatch.GeneratedPinAmbiguityStaysOnStage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchGeneratedAmbiguitySpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (const TCHAR* Kind : {TEXT("AddMappingContext"), TEXT("CreateWidget")})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Real factory fixture"), BP.Get())) return false;
        const FString Before = Snapshot(BP.Get());
        auto Previous = MoveTemp(NwiroBlueprintBatch::BeforeGeneratedPinLookup);
        ON_SCOPE_EXIT { NwiroBlueprintBatch::BeforeGeneratedPinLookup = MoveTemp(Previous); };
        int32 Injected = 0;
        NwiroBlueprintBatch::BeforeGeneratedPinLookup = [&](UEdGraphNode* Node, const FString& Name)
        {
            const FString Expected = FString(Kind) == TEXT("CreateWidget") ? TEXT("WidgetType") : TEXT("Class");
            if (Injected || !NwiroBlueprintBatch::Active || !NwiroBlueprintBatch::Active->bStaging
                || Node->GetOutermost() != GetTransientPackage() || !Name.Equals(Expected, ESearchCase::CaseSensitive)) return;
            UEdGraphPin* Original = Node->FindPin(FName(*Name));
            if (!Original) return;
            ++Injected;
            Node->CreatePin(Original->Direction, Original->PinType, Original->PinName);
        };
        FWatch Watch(BP.Get());
        const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"add_nodes\":[{\"type\":\"%s\",\"widgetClass\":\"/Script/UMG.UserWidget\",\"ref\":\"ambiguousGenerated\"}]"), Kind)));
        TestEqual(TEXT("Real generated pin lookup was reached exactly once"), Injected, 1);
        TestFalse(TEXT("Generated ambiguity rejects actual dispatch"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("Failure names pin ambiguity"), Receipt->GetStringField(TEXT("error")).Contains(TEXT("AMBIGUOUS_PIN")));
        TestTrue(TEXT("All generated helper nodes remain off target"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchDefaultCallbackSpec, NWIRO_SPEC_PREFIX "PinBatch.PostSetterCaseAndPinLifetimeDivergence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchDefaultCallbackSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (int32 Divergence : {0, 1, 2, 3})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
        FGraphNodeCreator<UK2Node_CallFunction> Creator(*BP->UbergraphPages[0]);
        UK2Node_CallFunction* Print = Creator.CreateNode();
        Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
        Creator.Finalize();
        const FString Before = Snapshot(BP.Get());
        auto Previous = MoveTemp(NwiroBlueprintBatch::AfterDefaultSetter);
        ON_SCOPE_EXIT { NwiroBlueprintBatch::AfterDefaultSetter = MoveTemp(Previous); };
        int32 Injected = 0;
        NwiroBlueprintBatch::AfterDefaultSetter = [&](UEdGraphNode* Node, const FGuid& Id)
        {
            if (Injected || !NwiroBlueprintBatch::Active || !NwiroBlueprintBatch::Active->bStaging) return;
            UEdGraphPin* Pin = Node->FindPinById(Id);
            if (!Pin || Pin->PinName != TEXT("InString")) return;
            ++Injected;
            if (Divergence == 0) Pin->DefaultValue = Pin->DefaultValue.ToUpper();
            else if (Divergence == 1) Node->RemovePin(Pin);
            else if (Divergence == 2) Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
            else Node->GetGraph()->RemoveNode(Node);
        };
        FWatch Watch(BP.Get());
        const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"set_pin_defaults\":[{\"ref\":\"%s\",\"pin\":\"InString\",\"value\":\"CamelCase\"}]"), *Print->NodeGuid.ToString())));
        TestEqual(TEXT("Real setter completed before injected callback divergence"), Injected, 1);
        TestFalse(TEXT("Case-only or lifetime divergence cannot become an accepted default"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("Source default and graph unchanged"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchEmptyAndMissingTargetSpec, NWIRO_SPEC_PREFIX "PinBatch.ExplicitWholeNodeBreakAndMissingTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchEmptyAndMissingTargetSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A->FindPin(TEXT("ReturnValue")), B->FindPin(TEXT("Target")));
    const auto Break = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"compile\":false,\"break_connections\":[{\"ref\":\"%s\",\"pin\":\"\"}]"), *B->NodeGuid.ToString())));
    TestTrue(TEXT("Explicit empty pin preserves whole-node break"), Break->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Whole-node links actually removed"), B->FindPin(TEXT("Target"))->LinkedTo.Num(), 0);
    const FString Missing = FString(TEXT(NWIRO_SPEC_PKG)) + TEXT("MustNotCreate_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const auto Cmd = Object(TEXT("{\"set_pin_defaults\":[{\"ref\":\"Missing\",\"pin\":\"X\",\"value\":\"3\"}]}"));
    Cmd->SetStringField(TEXT("blueprint"), Missing);
    const auto Refusal = Object(NWIRO_PIN_DISPATCH(TEXT("edit_blueprint"), NwiroBlueprintBatch::Json(Cmd)));
    TestFalse(TEXT("Pin-only missing target rejected"), Refusal->GetBoolField(TEXT("success")));
    TestFalse(TEXT("No asset auto-created"), UEditorAssetLibrary::DoesAssetExist(Missing));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchCreationSpec, NWIRO_SPEC_PREFIX "PinBatch.NewTargetCreationEffectsAreTruthful",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchCreationSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
    const auto PreviousSavePolicy = Settings->SaveOnCompile;
    ON_SCOPE_EXIT { Settings->SaveOnCompile = PreviousSavePolicy; };
    for (const auto Policy : {SoC_Never, SoC_Always})
    for (bool FailCommit : {false, true})
    {
        Settings->SaveOnCompile = Policy; // process-only fixture policy, never SaveConfig
        const FString Name = TEXT("BP_NewPinBatch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Package = FString(TEXT(NWIRO_SPEC_PKG)) + Name;
        const FString ObjectPath = Package + TEXT(".") + Name;
        auto Previous = MoveTemp(NwiroBlueprintBatch::BeforeCommitOperation);
        ON_SCOPE_EXIT { NwiroBlueprintBatch::BeforeCommitOperation = MoveTemp(Previous); };
        int32 Injected = 0, Compiles = 0, Saves = 0;
        const FDelegateHandle CompileHandle = GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* BP)
        { if (BP && BP->GetPathName().Equals(ObjectPath, ESearchCase::CaseSensitive)) ++Compiles; });
        const FDelegateHandle SaveHandle = UPackage::PreSavePackageWithContextEvent.AddLambda([&](UPackage* BP, FObjectPreSaveContext)
        { if (BP && BP->GetName().Equals(Package, ESearchCase::CaseSensitive)) ++Saves; });
        ON_SCOPE_EXIT { GEditor->OnBlueprintPreCompile().Remove(CompileHandle); UPackage::PreSavePackageWithContextEvent.Remove(SaveHandle); };
        NwiroBlueprintBatch::BeforeCommitOperation = [&](UBlueprint* BP, const FString& Key)
        {
            if (FailCommit && Key == TEXT("connect_pins"))
            {
                auto Node = Function(BP, TEXT("MakeVector"));
                if (Node) { ++Injected; Node->RemovePin(Node->FindPin(TEXT("ReturnValue"))); }
            }
        };
        const auto Cmd = Object(TEXT("{\"compile\":false,\"parentClass\":\"Actor\",\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"v\"},{\"type\":\"CallFunction\",\"function\":\"VInterpTo\",\"ref\":\"interp\"}],\"connect_pins\":[{\"from\":\"v.ReturnValue\",\"to\":\"interp.Target\"}]}"));
        Cmd->SetStringField(TEXT("blueprint"), ObjectPath);
        const auto Receipt = Object(NWIRO_PIN_DISPATCH(TEXT("edit_blueprint"), NwiroBlueprintBatch::Json(Cmd)));
        TStrongObjectPtr<UBlueprint> Created(FindObject<UBlueprint>(nullptr, *ObjectPath));
        if (!TestNotNull(TEXT("Admitted exact-path new Blueprint created"), Created.Get())) return false;
        TestEqual(TEXT("Commit verdict matches injected divergence"), Receipt->GetBoolField(TEXT("success")), !FailCommit);
        TestEqual(TEXT("Fault only in requested failed case"), Injected, FailCommit ? 1 : 0);
        TestTrue(TEXT("Factory compilation occurred despite compile:false"), Compiles > 0);
        TestTrue(TEXT("Receipt reports actual target compilation"), Receipt->GetBoolField(TEXT("compiled")));
        const auto Batch = Receipt->GetObjectField(TEXT("batchAdmission"));
        TestEqual(TEXT("Creation compile events independently counted"), Batch->GetNumberField(TEXT("creationCompileAttempts")), static_cast<double>(Compiles));
        TestTrue(TEXT("Staging compilation separately visible"), Batch->GetNumberField(TEXT("stagingCompileAttempts")) > 0);
        TestEqual(TEXT("Receipt save-attempt flag matches actual package events"), Receipt->GetBoolField(TEXT("saveAttempted")), Saves > 0);
        if (Policy == SoC_Never)
        {
            TestEqual(TEXT("No explicit creation save escapes Never policy and compile:false"), Saves, 0);
            TestFalse(TEXT("No saved asset claimed"), Receipt->GetBoolField(TEXT("savedToDisk")));
            TestFalse(TEXT("No disk file created"), FPaths::FileExists(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension())));
        }
        else
        {
            TestTrue(TEXT("Always policy exercised a real factory save attempt"), Saves > 0);
            const TSharedPtr<FJsonValue>* Saved = Receipt->Values.Find(TEXT("savedToDisk"));
            TestTrue(TEXT("Observed implicit save is truthfully unknown without confirmed save return"), Saved && (*Saved)->Type == EJson::Null);
        }
        TestTrue(TEXT("Created authored state remains dirty"), Created->GetOutermost()->IsDirty());
        if (FailCommit) TestTrue(TEXT("Admitted failure reports partial/unknown"), Batch->GetStringField(TEXT("currentRequestMutation")).Equals(TEXT("partial_or_unknown"), ESearchCase::CaseSensitive));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchAbsentRejectedSpec, NWIRO_SPEC_PREFIX "PinBatch.InvalidNewTargetBatchNeverCreatesAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchAbsentRejectedSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (bool Reverse : {false, true})
    {
        const FString Package = FString(TEXT(NWIRO_SPEC_PKG)) + TEXT("MustNotCreateBatch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Name = FPackageName::GetShortName(Package), Path = Package + TEXT(".") + Name;
        const FString Good = TEXT("{\"from\":\"v.ReturnValue\",\"to\":\"interp.Target\"}");
        const FString Bad = TEXT("{\"from\":\"v.ReturnValue\",\"to\":\"interp.Missing\"}");
        const auto Cmd = Object(FString::Printf(TEXT("{\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"v\"},{\"type\":\"CallFunction\",\"function\":\"VInterpTo\",\"ref\":\"interp\"}],\"connect_pins\":[%s,%s]}"), *(Reverse ? Bad : Good), *(Reverse ? Good : Bad)));
        Cmd->SetStringField(TEXT("blueprint"), Path);
        const auto Receipt = Object(NWIRO_PIN_DISPATCH(TEXT("edit_blueprint"), NwiroBlueprintBatch::Json(Cmd)));
        TestFalse(TEXT("Whole absent-target request rejected"), Receipt->GetBoolField(TEXT("success")));
        TestNull(TEXT("Target package was never created"), FindPackage(nullptr, *Package));
        TestNull(TEXT("Target Blueprint was never created"), FindObject<UBlueprint>(nullptr, *Path));
        TestFalse(TEXT("Target disk file absent"), FPaths::FileExists(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension())));
        TestFalse(TEXT("No target compilation"), Receipt->GetBoolField(TEXT("compiled")));
        TestFalse(TEXT("No target save"), Receipt->GetBoolField(TEXT("saveAttempted")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchLedgerFailureSpec, NWIRO_SPEC_PREFIX "PinBatch.LegacyOkWithUnitErrorsStopsCommit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchLedgerFailureSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
    const FString RemoveRef = A->NodeGuid.ToString(), To = Endpoint(B, TEXT("Target"));
    auto Previous = MoveTemp(NwiroBlueprintBatch::BeforeCommitOperation);
    ON_SCOPE_EXIT { NwiroBlueprintBatch::BeforeCommitOperation = MoveTemp(Previous); };
    int32 RemovedBeforeHandler = 0, LaterConnectCalls = 0;
    NwiroBlueprintBatch::BeforeCommitOperation = [&](UBlueprint* Current, const FString& Key)
    {
        if (Key == TEXT("remove_nodes")) { Current->UbergraphPages[0]->RemoveNode(A); ++RemovedBeforeHandler; }
        if (Key == TEXT("connect_pins")) ++LaterConnectCalls;
    };
    FWatch Watch(BP.Get());
    const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"survivor\"}],\"remove_nodes\":[{\"ref\":\"%s\"}],\"connect_pins\":[{\"from\":\"survivor.ReturnValue\",\"to\":\"%s\"}]"), *RemoveRef, *To)));
    TestEqual(TEXT("Failure follows successful staged lookup"), RemovedBeforeHandler, 1);
    TestFalse(TEXT("Legacy Ok with Level2 errors is a failed commit"), Receipt->GetBoolField(TEXT("success")));
    TestEqual(TEXT("Later operation never starts"), LaterConnectCalls, 0);
    TestEqual(TEXT("No target flush compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
    TestTrue(TEXT("Partial effects are explicit"), Receipt->GetObjectField(TEXT("batchAdmission"))->GetStringField(TEXT("currentRequestMutation")).Equals(TEXT("partial_or_unknown"), ESearchCase::CaseSensitive));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchMathDefaultsSpec, NWIRO_SPEC_PREFIX "PinBatch.K2RotatorAndVectorLiteralGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchMathDefaultsSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    FGraphNodeCreator<UK2Node_CallFunction> Creator(*BP->UbergraphPages[0]);
    UK2Node_CallFunction* Rotator = Creator.CreateNode();
    Rotator->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_RotatorToVector")));
    Creator.Finalize();
    if (!TestNotNull(TEXT("Real Rotator input"), Rotator->FindPin(TEXT("InRot")))) return false;
    auto V = Function(BP.Get(), TEXT("VInterpTo"));
    const auto Good = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"compile\":false,\"set_pin_defaults\":[{\"ref\":\"%s\",\"pin\":\"InRot\",\"value\":\"1,2,3\"},{\"ref\":\"%s\",\"pin\":\"Current\",\"value\":\"4,5,6\"}]"), *Rotator->NodeGuid.ToString(), *V->NodeGuid.ToString())));
    TestTrue(TEXT("K2 numeric math literals remain valid"), Good->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Rotator raw triple preserved exactly"), Rotator->FindPin(TEXT("InRot"))->DefaultValue.Equals(TEXT("1,2,3"), ESearchCase::CaseSensitive));
    const FString Before = Snapshot(BP.Get());
    const auto Bad = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"set_pin_defaults\":[{\"ref\":\"%s\",\"pin\":\"InRot\",\"value\":\"(Pitch=1,Yaw=2,Roll=3)\"}]"), *Rotator->NodeGuid.ToString())));
    TestFalse(TEXT("Unsupported Rotator named grammar is rejected without rewriting"), Bad->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Invalid named literal leaves authored state exact"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchConnectionCallbackSpec, NWIRO_SPEC_PREFIX "PinBatch.ConnectionCallbackCannotEraseClaimedLink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchConnectionCallbackSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (bool DeletePin : {false, true})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
        auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
        const FString Before = Snapshot(BP.Get());
        auto Previous = MoveTemp(NwiroBlueprintBatch::AfterConnectionSchema);
        ON_SCOPE_EXIT { NwiroBlueprintBatch::AfterConnectionSchema = MoveTemp(Previous); };
        int32 Injected = 0;
        NwiroBlueprintBatch::AfterConnectionSchema = [&](UEdGraphNode* NodeA, const FGuid& AId, UEdGraphNode* NodeB, const FGuid& BId)
        {
            if (Injected || !NwiroBlueprintBatch::Active || !NwiroBlueprintBatch::Active->bStaging) return;
            auto PinA = NodeA->FindPinById(AId); auto PinB = NodeB->FindPinById(BId);
            if (!PinA || !PinB) return;
            ++Injected;
            if (DeletePin) NodeB->RemovePin(PinB); else PinA->BreakAllPinLinks();
        };
        FWatch Watch(BP.Get());
        const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"connect_pins\":[{\"from\":\"%s\",\"to\":\"%s\"}]"), *Endpoint(A,TEXT("ReturnValue")), *Endpoint(B,TEXT("Target")))));
        TestEqual(TEXT("Real schema call preceded divergence"), Injected, 1);
        TestFalse(TEXT("Removed pin/edge cannot be applied-as-requested"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("Target stays exact"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchBreakCallbackSpec, NWIRO_SPEC_PREFIX "PinBatch.WholeNodeBreakCallbackCannotRestoreClaimedLinks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchBreakCallbackSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    auto A = Function(BP.Get(), TEXT("MakeVector")); auto B = Function(BP.Get(), TEXT("VInterpTo"));
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A->FindPin(TEXT("ReturnValue")), B->FindPin(TEXT("Target")));
    const FString Before = Snapshot(BP.Get());
    auto Previous = MoveTemp(NwiroBlueprintBatch::AfterWholeNodeBreak);
    ON_SCOPE_EXIT { NwiroBlueprintBatch::AfterWholeNodeBreak = MoveTemp(Previous); };
    int32 Injected = 0;
    NwiroBlueprintBatch::AfterWholeNodeBreak = [&](UEdGraphNode* Node)
    {
        if (Injected || !NwiroBlueprintBatch::Active || !NwiroBlueprintBatch::Active->bStaging) return;
        UEdGraphNode* Peer = nullptr;
        for (UEdGraphNode* Candidate : Node->GetGraph()->Nodes) if (Candidate && Candidate->NodeGuid == A->NodeGuid) Peer = Candidate;
        if (!Peer || !Peer->FindPin(TEXT("ReturnValue")) || !Node->FindPin(TEXT("Target"))) return;
        ++Injected;
        Peer->FindPin(TEXT("ReturnValue"))->MakeLinkTo(Node->FindPin(TEXT("Target")));
    };
    FWatch Watch(BP.Get());
    const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"break_connections\":[{\"ref\":\"%s\",\"pin\":\"\"}]"), *B->NodeGuid.ToString())));
    TestEqual(TEXT("Real whole-node break completed before divergence"), Injected, 1);
    TestFalse(TEXT("Restored link cannot become successful break"), Receipt->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Authored links remain exact"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
    TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchConversionSpec, NWIRO_SPEC_PREFIX "PinBatch.ValidSchemaConversionRouteRemainsSupported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchConversionSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    auto A = Function(BP.Get(), TEXT("MakeVector"));
    FGraphNodeCreator<UK2Node_CallFunction> Creator(*BP->UbergraphPages[0]);
    UK2Node_CallFunction* Print = Creator.CreateNode();
    Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
    Creator.Finalize();
    const int32 BeforeNodes = BP->UbergraphPages[0]->Nodes.Num();
    const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"compile\":false,\"connect_pins\":[{\"from\":\"%s\",\"to\":\"%s\"}]"), *Endpoint(A,TEXT("ReturnValue")), *Endpoint(Print,TEXT("InString")))));
    TestTrue(TEXT("Vector-to-string schema conversion is admitted"), Receipt->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Schema introduced a conversion node"), BP->UbergraphPages[0]->Nodes.Num() > BeforeNodes);
    UEdGraphPin* Input = Print->FindPin(TEXT("InString"));
    if (!TestNotNull(TEXT("Destination input remains live"), Input) || Input->LinkedTo.Num() != 1) return false;
    UEdGraphNode* Conversion = Input->LinkedTo[0]->GetOwningNode();
    bool SourceReachesConversion = false;
    for (const UEdGraphPin* Pin : Conversion->Pins)
        if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Contains(A->FindPin(TEXT("ReturnValue")))) SourceReachesConversion = true;
    TestTrue(TEXT("Actual source-conversion-destination path exists"), Conversion != A && SourceReachesConversion);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchTypeShapeSpec, NWIRO_SPEC_PREFIX "PinBatch.VariableSubtypeAndContainerAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchTypeShapeSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    for (const TCHAR* BadType : {TEXT("{\"base\":\"object\",\"subtype\":\"NoSuchPinBatchClass\"}"),
        TEXT("{\"base\":\"struct\",\"subtype\":\"NoSuchPinBatchStruct\"}"),
        TEXT("{\"base\":\"enum\",\"subtype\":\"NoSuchPinBatchEnum\"}"),
        TEXT("{\"base\":\"float\",\"container\":\"NoSuchContainer\"}")})
    for (bool Reverse : {false, true})
    {
        TStrongObjectPtr<UBlueprint> BP(Fixture());
        if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
        const FString Good = TEXT("{\"name\":\"GoodArray\",\"type\":{\"base\":\"float\",\"container\":\"array\"}}");
        const FString Bad = FString::Printf(TEXT("{\"name\":\"InvalidPeer\",\"type\":%s}"), BadType);
        const FString Before = Snapshot(BP.Get()); FWatch Watch(BP.Get());
        const auto Receipt = Object(Dispatch(BP.Get(), FString::Printf(TEXT("\"add_variables\":[%s,%s],\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"typedBatch\"}]"), *(Reverse ? Bad : Good), *(Reverse ? Good : Bad))));
        TestFalse(TEXT("Invalid subtype/container rejects both peer orders"), Receipt->GetBoolField(TEXT("success")));
        TestTrue(TEXT("No earlier typed member/node mutation"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
    }
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Positive fixture"), BP.Get())) return false;
    const auto Good = Object(Dispatch(BP.Get(), TEXT("\"compile\":false,\"add_variables\":[{\"name\":\"ActorPeer\",\"type\":\"object\",\"subtype\":\"Actor\"},{\"name\":\"FloatArray\",\"type\":{\"base\":\"float\",\"container\":\"array\"}}],\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"typedBatch\"}]")));
    TestTrue(TEXT("Resolvable legacy subtype plus structured array remains supported"), Good->GetBoolField(TEXT("success")));
    const int32 ActorIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP.Get(), TEXT("ActorPeer"));
    const int32 ArrayIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP.Get(), TEXT("FloatArray"));
    if (ActorIndex == INDEX_NONE || ArrayIndex == INDEX_NONE) { AddError(TEXT("Missing admitted typed members")); return false; }
    TestTrue(TEXT("Actor subtype retained exactly"), BP->NewVariables[ActorIndex].VarType.PinSubCategoryObject.Get() == AActor::StaticClass());
    TestTrue(TEXT("Array container retained exactly"), BP->NewVariables[ArrayIndex].VarType.ContainerType == EPinContainerType::Array);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroProPinBatchEarlyAdmissionSpec, NWIRO_SPEC_PREFIX "PinBatch.EarlyMalformedCountsAndAbsentIdentityAreTruthful",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroProPinBatchEarlyAdmissionSpec::RunTest(const FString&)
{
    using namespace NwiroPinBatchSpec;
    TStrongObjectPtr<UBlueprint> BP(Fixture());
    if (!TestNotNull(TEXT("Fixture"), BP.Get())) return false;
    // Specialized pure admission may reject before the generic batch context exists.
    // Exercise both contracts without assuming every failure has batchAdmission.
    int32 MeasuredCases = 0;
    for (bool SpecializedVariables : {true, false})
    for (bool ReverseKeys : {false, true}) for (bool MalformedContainer : {false, true})
    {
        const TCHAR* Operation = SpecializedVariables ? TEXT("add_variables") : TEXT("add_functions");
        const FString Bad = FString::Printf(TEXT("\"%s\":%s"), Operation,
            MalformedContainer ? TEXT("123") : TEXT("[\"bad\"]"));
        const FString Good = TEXT("\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"countPeer1\"},{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"countPeer2\"}]");
        const FString Before = Snapshot(BP.Get());
        const bool DirtyBefore = BP->GetOutermost()->IsDirty();
        FWatch Watch(BP.Get());
        const FString RawReceipt = Dispatch(BP.Get(), ReverseKeys ? Good + TEXT(",") + Bad : Bad + TEXT(",") + Good);
        AddInfo(FString::Printf(TEXT("Early admission operation=%s reverse=%d malformedContainer=%d receipt=%s"),
            Operation, ReverseKeys, MalformedContainer, *RawReceipt));
        TSharedPtr<FJsonObject> Receipt;
        const auto ReceiptReader = TJsonReaderFactory<>::Create(RawReceipt);
        if (TestTrue(TEXT("Dispatch returns a JSON object"), FJsonSerializer::Deserialize(ReceiptReader, Receipt) && Receipt.IsValid()))
        {
            auto ExpectFalse = [this](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
            {
                bool Value = true;
                TestTrue(FString::Printf(TEXT("%s is present and false"), Field), Object->TryGetBoolField(Field, Value) && !Value);
            };
            ExpectFalse(Receipt, TEXT("success"));
            FString Code;
            if (SpecializedVariables)
            {
                TestTrue(TEXT("Specialized variable rejection code is preserved"), Receipt->TryGetStringField(TEXT("code"), Code)
                    && Code.Equals(TEXT("VARIABLE_EDITABILITY_REJECTED"), ESearchCase::CaseSensitive));
                const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
                bool FoundExpectedIssue = false;
                if (Receipt->TryGetArrayField(TEXT("errors"), Errors) && Errors)
                    for (const auto& Error : *Errors)
                    {
                        if (!Error.IsValid() || Error->Type != EJson::Object || !Error->AsObject().IsValid()) continue;
                        FString IssueCode;
                        FoundExpectedIssue |= Error->AsObject()->TryGetStringField(TEXT("code"), IssueCode)
                            && IssueCode.Equals(MalformedContainer ? TEXT("INVALID_VARIABLE_CONTAINER") : TEXT("INVALID_VARIABLE_ITEM"), ESearchCase::CaseSensitive);
                    }
                TestTrue(TEXT("Specialized error identifies the malformed container or item"), FoundExpectedIssue);
                for (const TCHAR* Field : {TEXT("currentRequestMutation"), TEXT("targetAccessed"), TEXT("targetLoadRequested"), TEXT("targetMutationStarted"), TEXT("compiled"), TEXT("saveAttempted")})
                    ExpectFalse(Receipt, Field);
            }
            else
            {
                TestTrue(TEXT("Generic normalization rejection code is preserved"), Receipt->TryGetStringField(TEXT("errorCode"), Code)
                    && Code.Equals(TEXT("BLUEPRINT_BATCH_NOT_ADMITTED"), ESearchCase::CaseSensitive));
                const TSharedPtr<FJsonObject>* Batch = nullptr;
                if (TestTrue(TEXT("Generic admission includes a batch object"), Receipt->TryGetObjectField(TEXT("batchAdmission"), Batch) && Batch && Batch->IsValid()))
                {
                    ExpectFalse(*Batch, TEXT("countsComplete"));
                    for (const TCHAR* Field : {TEXT("requestedUnits"), TEXT("rejectedUnits")})
                    {
                        const TSharedPtr<FJsonValue> Value = (*Batch)->TryGetField(Field);
                        TestTrue(FString::Printf(TEXT("%s is explicitly null while normalization is incomplete"), Field), Value.IsValid() && Value->Type == EJson::Null);
                    }
                }
            }
        }
        // Receipt failures must not skip the independent no-touch oracle.
        TestTrue(TEXT("No earlier or later authored change"), Snapshot(BP.Get()).Equals(Before, ESearchCase::CaseSensitive));
        TestEqual(TEXT("Existing dirty flag preserved"), BP->GetOutermost()->IsDirty(), DirtyBefore);
        TestEqual(TEXT("No target compile"), Watch.Compiles, 0); TestEqual(TEXT("No target save"), Watch.Saves, 0);
        ++MeasuredCases;
    }
    TestEqual(TEXT("Both admission contracts cover both key orders and malformed shapes"), MeasuredCases, 8);
    for (const TCHAR* Leaf : {TEXT("WrongLeaf"), TEXT("WrongLeaf.Extra"), TEXT("Subobject:Child")})
    {
        const FString Package = FString(TEXT(NWIRO_SPEC_PKG)) + TEXT("MustNotRewriteLeaf_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const auto Cmd = Object(TEXT("{\"add_nodes\":[{\"type\":\"CallFunction\",\"function\":\"MakeVector\",\"ref\":\"v\"}]}"));
        Cmd->SetStringField(TEXT("blueprint"), Package + TEXT(".") + Leaf);
        const auto Receipt = Object(NWIRO_PIN_DISPATCH(TEXT("edit_blueprint"), NwiroBlueprintBatch::Json(Cmd)));
        TestFalse(TEXT("Absent mismatched/extra/subobject leaf cannot be normalized into another asset"), Receipt->GetBoolField(TEXT("success")));
        TestNull(TEXT("No target package created"), FindPackage(nullptr, *Package));
        TestFalse(TEXT("No target file created"), FPaths::FileExists(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension())));
    }
    return true;
}

#undef NWIRO_PIN_DISPATCH
