// Proposed real native regression; not built or executed in this artifact phase.
#include "../NwiroBlueprintTools.h"
#include "InputAction.h"
#include "K2Node_EnhancedInputAction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectGlobals.h"
#include "../NwiroToolDispatch.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace NwiroEnhancedInputContractTest
{
static FString Json(const TSharedRef<FJsonObject>& Object)
{
    FString Out;
    const auto Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Object, Writer);
    return Out;
}

static FString GraphState(UBlueprint* BP)
{
    TArray<FString> Rows;
    for (const UEdGraph* Graph : BP->UbergraphPages)
    {
        if (!Graph) continue;
        Rows.Add(TEXT("Graph:") + Graph->GetPathName());
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            Rows.Add(FString::Printf(TEXT("Node:%s:%s:%d:%d"), *Node->NodeGuid.ToString(), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY));
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                Rows.Add(FString::Printf(TEXT("Pin:%s:%s:%s:%s:%s"), *Pin->PinId.ToString(), *Pin->PinName.ToString(), *Pin->DefaultValue,
                    *GetPathNameSafe(Pin->DefaultObject), *Pin->DefaultTextValue.ToString()));
                for (const UEdGraphPin* Other : Pin->LinkedTo)
                    if (Other) Rows.Add(TEXT("Link:") + Pin->PinId.ToString() + TEXT(":") + Other->PinId.ToString());
            }
        }
    }
    for (const FBPVariableDescription& V : BP->NewVariables)
        Rows.Add(TEXT("Variable:") + V.VarGuid.ToString() + TEXT(":") + V.VarName.ToString() + TEXT(":") + V.DefaultValue);
    Rows.Sort();
    return FString::Join(Rows, TEXT("\n"));
}

static TSharedRef<FJsonObject> Item(const FString& BadKey)
{
    TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("type"), TEXT("EnhancedInputAction"));
    Node->SetStringField(TEXT("ref"), TEXT("inputEvent"));
    // This deliberately absent action must never be loaded by a rejected request.
    Node->SetStringField(TEXT("action"), TEXT("/Game/__NwiroAdmissionMissing/IA_Absent.IA_Absent"));
    if (BadKey == TEXT("node_type"))
    {
        Node->RemoveField(TEXT("type"));
        Node->SetStringField(BadKey, TEXT("EnhancedInputAction"));
    }
    else Node->SetStringField(BadKey, TEXT("ignored-alias-must-not-be-applied"));
    return Node;
}

static TSharedRef<FJsonObject> Command(const FString& Target, const FString& BadKey, int32 Form, bool bInvalidFirst)
{
    TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("blueprint"), Target);
    // compile is intentionally omitted: the real wrapper normally defaults true.
    TSharedRef<FJsonObject> Peer = MakeShared<FJsonObject>();
    Peer->SetStringField(TEXT("type"), TEXT("Branch"));
    Peer->SetStringField(TEXT("ref"), TEXT("mustNotCreatePeer"));
    TArray<TSharedPtr<FJsonValue>> Items;
    Items.Add(MakeShared<FJsonValueObject>(bInvalidFirst ? Item(BadKey) : Peer));
    Items.Add(MakeShared<FJsonValueObject>(bInvalidFirst ? Peer : Item(BadKey)));
    if (Form == 0) Request->SetArrayField(TEXT("add_nodes"), Items);
    if (Form == 1) Request->SetArrayField(TEXT("nodes"), Items);
    if (Form == 2)
    {
        FString Encoded;
        const auto Writer = TJsonWriterFactory<>::Create(&Encoded);
        FJsonSerializer::Serialize(Items, Writer);
        Request->SetStringField(TEXT("add_nodes"), Encoded);
    }
    if (Form == 3 || Form == 4)
    {
        TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
        Action->SetStringField(TEXT("type"), TEXT("add_nodes"));
        Action->SetArrayField(TEXT("nodes"), Items);
        TArray<TSharedPtr<FJsonValue>> Actions{ MakeShared<FJsonValueObject>(Action) };
        if (Form == 3) Request->SetArrayField(TEXT("actions"), Actions);
        else
        {
            FString Encoded;
            const auto Writer = TJsonWriterFactory<>::Create(&Encoded);
            FJsonSerializer::Serialize(Actions, Writer);
            Request->SetStringField(TEXT("actions"), Encoded);
        }
    }
    TSharedRef<FJsonObject> Variable = MakeShared<FJsonObject>();
    Variable->SetStringField(TEXT("name"), TEXT("MustNotCreateVariable"));
    Variable->SetStringField(TEXT("type"), TEXT("bool"));
    Request->SetArrayField(TEXT("add_variables"), { MakeShared<FJsonValueObject>(Variable) });
    return Request;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroEnhancedInputContractNoTouch, "Nwiro.Pro.EnhancedInputContract.UnknownKeysBeforeTargetWork",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroEnhancedInputContractNoTouch::RunTest(const FString& Parameters)
{
    using namespace NwiroEnhancedInputContractTest;
    if (!GEditor) { AddError(TEXT("Requires an actual editor host")); return false; }
    const FString PackageName = TEXT("/Temp/NwiroEnhancedInputAdmission_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UPackage> Package(CreatePackage(*PackageName));
    if (!Package.IsValid()) { AddError(TEXT("Owned package creation failed")); return false; }
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package.Get(),
        FName(TEXT("BP_Admission")), BPTYPE_Normal));
    if (!BP.IsValid()) { AddError(TEXT("Owned Blueprint factory failed")); return false; }
    // Factory setup is outside the measured request. No explicit save is made.
    int32 Compiles = 0, SaveAttempts = 0;
    const FDelegateHandle CompileHandle = GEditor->OnBlueprintPreCompile().AddLambda(
        [&](UBlueprint* Seen) { if (Seen == BP.Get()) ++Compiles; });
    const FDelegateHandle SaveHandle = UPackage::PreSavePackageWithContextEvent.AddLambda(
        [&](UPackage* Seen, FObjectPreSaveContext) { if (Seen == Package.Get()) ++SaveAttempts; });
    ON_SCOPE_EXIT
    {
        GEditor->OnBlueprintPreCompile().Remove(CompileHandle);
        UPackage::PreSavePackageWithContextEvent.Remove(SaveHandle);
    };
    const TArray<FString> BadKeys{ TEXT("inputAction"), TEXT("input_action"), TEXT("trigger_event"),
        TEXT("consume_input"), TEXT("component"), TEXT("properties"), TEXT("function"), TEXT("name"), TEXT("node_type") };
    for (bool Dirty : { false, true })
    {
        Package->SetDirtyFlag(Dirty); // Only this test-owned package.
        for (const FString& BadKey : BadKeys)
        for (int32 Form = 0; Form < 5; ++Form)
        for (bool InvalidFirst : { false, true })
        {
            const FString Before = GraphState(BP.Get());
            const EBlueprintStatus BeforeStatus = BP->Status;
            const bool BeforeDependencies = BP->bCachedDependenciesUpToDate;
            const FString Out = FNwiroBlueprintTools::EditBlueprint(Json(Command(BP->GetPathName(), BadKey, Form, InvalidFirst)));
            TSharedPtr<FJsonObject> Reply;
            if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Out), Reply) || !Reply.IsValid())
            { AddError(TEXT("Rejection must return JSON")); return false; }
            TestFalse(TEXT("Unknown alias rejected"), Reply->GetBoolField(TEXT("success")));
            TestEqual(TEXT("Specific early contract rejection"), Reply->GetStringField(TEXT("code")), FString(TEXT("ENHANCED_INPUT_CONTRACT_REJECTED")));
            TestFalse(TEXT("Target access blocked"), Reply->GetBoolField(TEXT("targetAccessed")));
            TestEqual(TEXT("Native graph/member state unchanged"), GraphState(BP.Get()), Before);
            TestEqual(TEXT("Initial dirty state preserved"), Package->IsDirty(), Dirty);
            TestTrue(TEXT("Blueprint status preserved"), BP->Status == BeforeStatus);
            TestEqual(TEXT("Dependency state preserved"), static_cast<bool>(BP->bCachedDependenciesUpToDate), BeforeDependencies);
            TestEqual(TEXT("No measured target compiler calls"), Compiles, 0);
            TestEqual(TEXT("No measured target save attempts"), SaveAttempts, 0);
        }
    }
    const FString MissingPackage = TEXT("/Game/__NwiroAdmissionMissing/BP_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TestNull(TEXT("Missing target absent before rejection"), FindObject<UPackage>(nullptr, *MissingPackage));
    const FString MissingOut = FNwiroBlueprintTools::EditBlueprint(Json(Command(MissingPackage, TEXT("inputAction"), 0, false)));
    TestTrue(TEXT("Missing target request is rejected by the same guard"), MissingOut.Contains(TEXT("ENHANCED_INPUT_CONTRACT_REJECTED")));
    TestNull(TEXT("No auto-created target package"), FindObject<UPackage>(nullptr, *MissingPackage));
    return true;
}
// Additional v2 native dispatcher regressions; proposed, uncompiled and unexecuted.
namespace NwiroEnhancedInputContractTest
{
static TSharedRef<FJsonObject> ActionNode(const FString& Path, const FString& Ref)
{
    TSharedRef<FJsonObject> O=MakeShared<FJsonObject>();
    O->SetStringField(TEXT("type"), TEXT("EnhancedInputAction"));
    O->SetStringField(TEXT("ref"), Ref); O->SetStringField(TEXT("action"), Path); return O;
}
static TSharedRef<FJsonObject> AssetCommand(const FString& Target, const FString& Good, const FString& Bad, int32 Form, bool BadFirst)
{
    TSharedRef<FJsonObject> O=Command(Target, TEXT("input_action"), Form, BadFirst);
    const TArray<TSharedPtr<FJsonValue>> Items{MakeShared<FJsonValueObject>(ActionNode(BadFirst?Bad:Good,TEXT("first"))),
        MakeShared<FJsonValueObject>(ActionNode(BadFirst?Good:Bad,TEXT("second")))};
    O->RemoveField(TEXT("add_nodes")); O->RemoveField(TEXT("nodes")); O->RemoveField(TEXT("actions"));
    if(Form==0) O->SetArrayField(TEXT("add_nodes"), Items);
    if(Form==1) O->SetArrayField(TEXT("nodes"), Items);
    if(Form==5) { O->SetArrayField(TEXT("add_nodes"),Items); O->SetNumberField(TEXT("actions"),123); }
    if(Form==2) { FString Encoded; const auto Writer=TJsonWriterFactory<>::Create(&Encoded); FJsonSerializer::Serialize(Items,Writer); O->SetStringField(TEXT("add_nodes"),Encoded); }
    if(Form==3 || Form==4)
    {
        TSharedRef<FJsonObject> A=MakeShared<FJsonObject>(); A->SetStringField(TEXT("type"),TEXT("add_nodes")); A->SetArrayField(TEXT("nodes"),Items);
        const TArray<TSharedPtr<FJsonValue>> Actions{MakeShared<FJsonValueObject>(A)};
        if(Form==3) O->SetArrayField(TEXT("actions"),Actions);
        else { FString Encoded; const auto Writer=TJsonWriterFactory<>::Create(&Encoded); FJsonSerializer::Serialize(Actions,Writer); O->SetStringField(TEXT("actions"),Encoded); }
    }
    return O;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroEnhancedInputAssetsNoTouch, "Nwiro.Pro.EnhancedInputContract.AssetsBeforeTargetWork",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroEnhancedInputAssetsNoTouch::RunTest(const FString& Parameters)
{
    using namespace NwiroEnhancedInputContractTest;
    if (!GEditor || !IsInGameThread()) { AddError(TEXT("Requires an actual editor game-thread dispatcher")); return false; }
    const FString Root=TEXT("/Game/__NwiroEnhancedInputAdmission_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TArray<TStrongObjectPtr<UPackage>> OwnedPackages; TArray<TStrongObjectPtr<UInputAction>> Actions;
    auto NewAction=[&](const FString& Suffix,const FName Name)
    {
        UPackage* P=CreatePackage(*(Root+Suffix)); OwnedPackages.Emplace(P);
        UInputAction* A=NewObject<UInputAction>(P,Name,RF_Public|RF_Standalone|RF_Transactional);
        Actions.Emplace(A); return A;
    };
    UInputAction* Good=NewAction(TEXT("/IA_Good"),FName(TEXT("IA_Good")));
    UInputAction* DuplicateA=NewAction(TEXT("/A/IA_Duplicate"),FName(TEXT("IA_Duplicate")));
    UInputAction* DuplicateB=NewAction(TEXT("/B/IA_Duplicate"),FName(TEXT("IA_Duplicate")));
    if (!Good || !DuplicateA || !DuplicateB) { AddError(TEXT("Owned InputAction setup failed")); return false; }
    Good->ValueType=EInputActionValueType::Axis2D;
    TStrongObjectPtr<UPackage> Package(CreatePackage(*(Root+TEXT("/BP_Target"))));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),FName(TEXT("BP_Target")),BPTYPE_Normal));
    if (!BP.IsValid()) { AddError(TEXT("Owned Blueprint setup failed")); return false; }
    const FString TargetFile=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
    TestFalse(TEXT("Owned unsaved target has no initial disk asset"),FPaths::FileExists(TargetFile));
    int32 Compiles=0,SaveAttempts=0;
    const FDelegateHandle CH=GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* Seen){if(Seen==BP.Get())++Compiles;});
    const FDelegateHandle SH=UPackage::PreSavePackageWithContextEvent.AddLambda([&](UPackage* Seen,FObjectPreSaveContext){if(Seen==Package.Get())++SaveAttempts;});
    ON_SCOPE_EXIT {GEditor->OnBlueprintPreCompile().Remove(CH);UPackage::PreSavePackageWithContextEvent.Remove(SH);};
    // Wrong type intentionally points to this existing target: class inspection is allowed,
    // but target load, graph edits, dirtying, compile and save must not occur.
    UInputAction* NotReady = NewAction(TEXT("/IA_NotReady"), FName(TEXT("IA_NotReady")));
    if (!NotReady) { AddError(TEXT("Owned not-ready fixture setup failed")); return false; }
    // Simulated object-local lifecycle flags only; this is not an async-loader test.
    NotReady->SetFlags(RF_NeedPostLoad);
    ON_SCOPE_EXIT { NotReady->ClearFlags(RF_NeedPostLoad); };
    UInputAction* Initializing = NewAction(TEXT("/IA_Initializing"), FName(TEXT("IA_Initializing")));
    if (!Initializing) { AddError(TEXT("Owned initializing fixture setup failed")); return false; }
    Initializing->SetFlags(RF_NeedInitialization);
    ON_SCOPE_EXIT { Initializing->ClearFlags(RF_NeedInitialization); };
    UPackage* RedirectPackage = CreatePackage(*(Root + TEXT("/IA_Redirect"))); OwnedPackages.Emplace(RedirectPackage);
    TStrongObjectPtr<UObjectRedirector> Redirect(NewObject<UObjectRedirector>(RedirectPackage, FName(TEXT("IA_Redirect")), RF_Public | RF_Standalone));
    if (!Redirect.IsValid()) { AddError(TEXT("Owned redirect fixture setup failed")); return false; }
    Redirect->DestinationObject = Good;
    const TArray<FString> BadPaths{Root+TEXT("/IA_Missing.IA_Missing"),BP->GetPathName(),TEXT("IA_Duplicate"),TEXT("IA_Good"),Good->GetPathName()+TEXT(":Subobject"),
        NotReady->GetPathName(), Redirect->GetPathName(), Initializing->GetPathName(), Root + TEXT("/IA_Good.ia_Good")};
    for (bool Dirty : {false,true})
    {
        Package->SetDirtyFlag(Dirty);
        for(const FString& Bad:BadPaths) for(int32 Form=0;Form<6;++Form) for(bool BadFirst:{false,true})
        {
            const FString Before=GraphState(BP.Get()); const EBlueprintStatus Status=BP->Status;
            const bool Dependencies=BP->bCachedDependenciesUpToDate;
            const FString Raw=FNwiroToolDispatch::DispatchTool(TEXT("edit_blueprint"),Json(AssetCommand(BP->GetPathName(),Good->GetPathName(),Bad,Form,BadFirst)));
            TSharedPtr<FJsonObject> Reply;
            if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw),Reply)||!Reply.IsValid()) {AddError(TEXT("Real dispatcher returned invalid JSON"));return false;}
            TestFalse(TEXT("Bad asset batch is refused"),Reply->GetBoolField(TEXT("success")));
            TestEqual(TEXT("Asset admission code"),Reply->GetStringField(TEXT("code")),FString(TEXT("ENHANCED_INPUT_ASSET_REJECTED")));
            TestFalse(TEXT("Target load not requested"),Reply->GetBoolField(TEXT("targetLoadRequested")));
            TestFalse(TEXT("Target mutation not started"),Reply->GetBoolField(TEXT("targetMutationStarted")));
            TestFalse(TEXT("Rejected batch reports no current request mutation"), Reply->GetBoolField(TEXT("currentRequestMutation")));
            if (Bad == BadPaths[0]) TestTrue(TEXT("Missing loaded object has explicit ACTION_NOT_LOADED"), Raw.Contains(TEXT("ACTION_NOT_LOADED")));
            if (Bad == NotReady->GetPathName()) TestTrue(TEXT("Unfinished object has explicit not-ready refusal"), Raw.Contains(TEXT("INPUT_ACTION_NOT_READY")));
            if (Bad == Initializing->GetPathName()) TestTrue(TEXT("Uninitialized object has explicit not-ready refusal"), Raw.Contains(TEXT("INPUT_ACTION_NOT_READY")));
            if (Bad == Redirect->GetPathName()) TestTrue(TEXT("Redirect is rejected, never followed"), Raw.Contains(TEXT("NOT_AN_INPUT_ACTION")));
            if (Bad == BadPaths.Last()) TestTrue(TEXT("Case-altered identity is refused"), Raw.Contains(TEXT("CANONICAL_ACTION_IDENTITY_REQUIRED")));
            TestEqual(TEXT("Graph/member state unchanged despite valid peers"),GraphState(BP.Get()),Before);
            TestEqual(TEXT("Existing dirty state preserved"),Package->IsDirty(),Dirty);
            TestTrue(TEXT("Status preserved"),BP->Status==Status);
            TestEqual(TEXT("Dependencies preserved"),static_cast<bool>(BP->bCachedDependenciesUpToDate),Dependencies);
            TestEqual(TEXT("No target compile during refusal"),Compiles,0);
            TestEqual(TEXT("No target save during refusal"),SaveAttempts,0);
            TestFalse(TEXT("No disk asset created during refusal"),FPaths::FileExists(TargetFile));
            const FString Missing=Root+TEXT("/BP_Auto_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
            const FString MissingFile=FPackageName::LongPackageNameToFilename(Missing,FPackageName::GetAssetPackageExtension());
            TestNull(TEXT("Auto-create package absent before request"),FindObject<UPackage>(nullptr,*Missing));
            const FString MissingRaw=FNwiroToolDispatch::DispatchTool(TEXT("edit_blueprint"),Json(AssetCommand(Missing,Good->GetPathName(),Bad,Form,BadFirst)));
            TestTrue(TEXT("Auto-create request rejected before wrapper creates anything"),MissingRaw.Contains(TEXT("ENHANCED_INPUT_ASSET_REJECTED")));
            TestNull(TEXT("Auto-create package remains absent"),FindObject<UPackage>(nullptr,*Missing));
            TestFalse(TEXT("No auto-create disk asset"),FPaths::FileExists(MissingFile));
        }
    }
    // Independent positive control: exact object AND package spelling select their
    // specified assets even though two loaded assets share an identical leaf name.
    TSharedRef<FJsonObject> Positive=MakeShared<FJsonObject>(); Positive->SetStringField(TEXT("blueprint"),BP->GetPathName());
    Positive->SetBoolField(TEXT("compile"),false);
    Positive->SetArrayField(TEXT("add_nodes"),{MakeShared<FJsonValueObject>(ActionNode(DuplicateA->GetPathName(),TEXT("A"))),
        MakeShared<FJsonValueObject>(ActionNode(DuplicateB->GetOutermost()->GetName(),TEXT("B"))),
        MakeShared<FJsonValueObject>(ActionNode(Good->GetPathName(),TEXT("move")))});
    const FString PositiveRaw=FNwiroToolDispatch::DispatchTool(TEXT("edit_blueprint"),Json(Positive)); TSharedPtr<FJsonObject> PositiveReply;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PositiveRaw),PositiveReply)||!PositiveReply.IsValid()) {AddError(TEXT("Positive native result invalid"));return false;}
    TestTrue(TEXT("Exact loaded action recipes succeed"),PositiveReply->GetBoolField(TEXT("success")));
    FObjectProperty* Property=CastField<FObjectProperty>(UK2Node_EnhancedInputAction::StaticClass()->FindPropertyByName(FName(TEXT("InputAction"))));
    if(!Property) {AddError(TEXT("Actual node action property unavailable"));return false;}
    int32 SeenA=0,SeenB=0,SeenGood=0;
    for(UEdGraph* Graph:BP->UbergraphPages) if(Graph) for(UEdGraphNode* Node:Graph->Nodes)
    {
        UK2Node_EnhancedInputAction* Event=Cast<UK2Node_EnhancedInputAction>(Node); if(!Event) continue;
        UObject* Bound=Property->GetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Event));
        if(Bound==DuplicateA) ++SeenA; if(Bound==DuplicateB) ++SeenB;
        if(Bound==Good)
        {
            ++SeenGood;
            TestNotNull(TEXT("Real ActionValue pin allocated"),Event->FindPin(FName(TEXT("ActionValue"))));
            TestNotNull(TEXT("Real Triggered output allocated"),Event->FindPin(FName(TEXT("Triggered"))));
        }
    }
    TestEqual(TEXT("Exact duplicate A binding"),SeenA,1); TestEqual(TEXT("Exact duplicate B binding"),SeenB,1); TestEqual(TEXT("Exact move binding"),SeenGood,1);
    TestEqual(TEXT("Positive compile:false made no target compile"),Compiles,0); TestEqual(TEXT("Positive compile:false made no target save"),SaveAttempts,0);
    TestFalse(TEXT("Positive fixture remains unsaved"),FPaths::FileExists(TargetFile));
    return true;
}

#endif
