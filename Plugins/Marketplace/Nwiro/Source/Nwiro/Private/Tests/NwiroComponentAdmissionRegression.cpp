// Proposed public-dispatch native tests. Not built or run in the artifact phase.
#include "../NwiroToolDispatch.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/SceneComponent.h"
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

namespace NwiroComponentAdmissionTest
{
static FString Json(const TSharedRef<FJsonObject>& Object)
{
    FString Out; const auto Writer=TJsonWriterFactory<>::Create(&Out); FJsonSerializer::Serialize(Object,Writer); return Out;
}
static FString Encoded(const TArray<TSharedPtr<FJsonValue>>& Values)
{
    FString Out; const auto Writer=TJsonWriterFactory<>::Create(&Out); FJsonSerializer::Serialize(Values,Writer); return Out;
}
static TSharedPtr<FJsonValue> Value(const FString& Raw)
{
    TSharedPtr<FJsonObject> Wrapper;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"item\":")+Raw+TEXT("}")),Wrapper) || !Wrapper.IsValid()) return nullptr;
    return Wrapper->TryGetField(TEXT("item"));
}
static TSharedRef<FJsonObject> MakeComponentJson(const FString& Name)
{
    const auto C=MakeShared<FJsonObject>(); C->SetStringField(TEXT("name"),Name); C->SetStringField(TEXT("class"),TEXT("SceneComponent")); return C;
}
static void Container(const TSharedRef<FJsonObject>& R,const TArray<TSharedPtr<FJsonValue>>& Items,int32 Form)
{
    if(Form==0) R->SetArrayField(TEXT("add_components"),Items);
    if(Form==1) R->SetArrayField(TEXT("components"),Items);
    if(Form==2) R->SetStringField(TEXT("add_components"),Encoded(Items));
    if(Form==3 || Form==4 || Form==8)
    {
        const auto A=MakeShared<FJsonObject>(); A->SetStringField(TEXT("type"),TEXT("add_components")); A->SetArrayField(TEXT("components"),Items);
        const TArray<TSharedPtr<FJsonValue>> Actions{MakeShared<FJsonValueObject>(A)};
        if(Form==3) R->SetArrayField(TEXT("actions"),Actions);
        if(Form==4) R->SetStringField(TEXT("actions"),Encoded(Actions));
        if(Form==8) R->SetObjectField(TEXT("actions"),A);
    }
    // Single-item compatibility forms have a valid variable/node peer in Command.
    if(Form==5) R->SetField(TEXT("component"),Items[0]);
    if(Form==6 && Items[0]->Type==EJson::Object) R->SetStringField(TEXT("component"),Json(Items[0]->AsObject().ToSharedRef()));
    if(Form==6 && Items[0]->Type!=EJson::Object) R->SetStringField(TEXT("component"),TEXT("[null]"));
    if(Form==7) {R->SetArrayField(TEXT("add_components"),Items);R->SetNumberField(TEXT("actions"),123);}
}
static TSharedRef<FJsonObject> Command(const FString& Target,const TSharedPtr<FJsonValue>& Bad,int32 Form,bool BadFirst)
{
    const auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("blueprint"),Target);
    // Deliberately omit compile: a rejection must not reach its default-true flush.
    const TSharedPtr<FJsonValue> Good=MakeShared<FJsonValueObject>(MakeComponentJson(TEXT("MustNotAddComponent")));
    const TArray<TSharedPtr<FJsonValue>> Items{BadFirst?Bad:Good,BadFirst?Good:Bad};
    Container(R,(Form==5 || Form==6)?TArray<TSharedPtr<FJsonValue>>{Bad}:Items,Form);
    const auto V=MakeShared<FJsonObject>();V->SetStringField(TEXT("name"),TEXT("MustNotAddVariable"));V->SetStringField(TEXT("type"),TEXT("bool"));
    R->SetArrayField(TEXT("add_variables"),{MakeShared<FJsonValueObject>(V)});
    const auto N=MakeShared<FJsonObject>();N->SetStringField(TEXT("type"),TEXT("Branch"));N->SetStringField(TEXT("ref"),TEXT("mustNotAddNode"));
    R->SetArrayField(TEXT("add_nodes"),{MakeShared<FJsonValueObject>(N)});return R;
}
static FString State(UBlueprint* BP)
{
    TArray<FString> Rows;
    if(BP->SimpleConstructionScript)
    {
        for(const USCS_Node* Root:BP->SimpleConstructionScript->GetRootNodes()) if(Root) Rows.Add(TEXT("Root:")+Root->GetPathName());
        for(const USCS_Node* Node:BP->SimpleConstructionScript->GetAllNodes()) if(Node)
        {
            Rows.Add(FString::Printf(TEXT("SCS:%s:%s:%s:%s:%s:%d"),*Node->GetPathName(),*Node->GetVariableName().ToString(),
                *GetPathNameSafe(Node->ComponentTemplate),*Node->ParentComponentOrVariableName.ToString(),*Node->ParentComponentOwnerClassName.ToString(),Node->bIsParentComponentNative));
            for(const USCS_Node* Child:Node->GetChildNodes()) if(Child) Rows.Add(TEXT("Child:")+Node->GetPathName()+TEXT(":")+Child->GetPathName());
        }
    }
    for(const FBPVariableDescription& V:BP->NewVariables) Rows.Add(TEXT("Var:")+V.VarGuid.ToString()+TEXT(":")+V.VarName.ToString()+TEXT(":")+V.DefaultValue);
    for(const UEdGraph* Graph:BP->UbergraphPages) if(Graph)
    {
        Rows.Add(TEXT("Graph:")+Graph->GetPathName());
        for(const UEdGraphNode* Node:Graph->Nodes) if(Node)
        {
            Rows.Add(TEXT("Node:")+Node->NodeGuid.ToString()+TEXT(":")+Node->GetClass()->GetPathName());
            for(const UEdGraphPin* Pin:Node->Pins) if(Pin)
            {
                Rows.Add(TEXT("Pin:")+Pin->PinId.ToString()+TEXT(":")+Pin->DefaultValue);
                for(const UEdGraphPin* Other:Pin->LinkedTo) if(Other) Rows.Add(TEXT("Link:")+Pin->PinId.ToString()+TEXT(":")+Other->PinId.ToString());
            }
        }
    }
    Rows.Sort();return FString::Join(Rows,TEXT("\n"));
}
static FString Dispatch(const TSharedRef<FJsonObject>& Request) { return FNwiroToolDispatch::DispatchTool(TEXT("edit_blueprint"),Json(Request)); }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroComponentAdmissionNoTouch,"Nwiro.Pro.ComponentAdmission.RejectedWholeRequestNoTouch",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNwiroComponentAdmissionNoTouch::RunTest(const FString& Parameters)
{
    using namespace NwiroComponentAdmissionTest;
    if(!GEditor || !IsInGameThread()) {AddError(TEXT("Requires actual editor game-thread dispatch"));return false;}
    const FString Root=TEXT("/Game/__NwiroComponentAdmission_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UPackage> Package(CreatePackage(*(Root+TEXT("/BP_Target"))));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),FName(TEXT("BP_Target")),BPTYPE_Normal));
    if(!BP.IsValid() || !BP->SimpleConstructionScript) {AddError(TEXT("Real Blueprint factory failed"));return false;}
    const FString TargetFile=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
    TestFalse(TEXT("Owned target starts unsaved"),FPaths::FileExists(TargetFile));
    int32 Compiles=0,Saves=0;
    const FDelegateHandle CH=GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* Seen){if(Seen && Seen->GetOutermost()->GetName().StartsWith(Root))++Compiles;});
    const FDelegateHandle SH=UPackage::PreSavePackageWithContextEvent.AddLambda([&](UPackage* Seen,FObjectPreSaveContext){if(Seen && Seen->GetName().StartsWith(Root))++Saves;});
    ON_SCOPE_EXIT {GEditor->OnBlueprintPreCompile().Remove(CH);UPackage::PreSavePackageWithContextEvent.Remove(SH);};
    const TArray<FString> BadJson{
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"isRoot\":true}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"is_root\":false}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"make_root\":true}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"MakeRoot\":true}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"Parent\":\"Root\"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"attach_parent\":\"Root\"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"makeRoot\":\"true\"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"makeRoot\":1}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"makeRoot\":null}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"makeRoot\":{}}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"makeRoot\":[]}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":1}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":null}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":false}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":{}}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":[]}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":\"  \"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":\" Root\"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"parent\":\"Root\\n\"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":{\"attach_parent\":\"Root\"}}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":{\"parent\":\"Root\"}}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":{\"makeRoot\":true}}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":{\"is_root\":true}}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":null}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":[]}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":true}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"properties\":\"{}\"}"),
        TEXT("{\"name\":\"BadComponent\",\"class\":\"SceneComponent\",\"makeRoot\":true,\"parent\":\"Root\"}"),
        TEXT("null"),
        TEXT("42"),
        TEXT("\"not-an-object\""),
    };
    for(bool Dirty:{false,true})
    {
        Package->SetDirtyFlag(Dirty); // Owned fixture only. No user package dirty flags touched.
        for(const FString& BadRaw:BadJson) for(int32 Form=0;Form<9;++Form) for(bool BadFirst:{false,true})
        {
            const auto Bad=Value(BadRaw);if(!Bad.IsValid()) {AddError(TEXT("Malformed test fixture"));return false;}
            const FString Before=State(BP.Get());const EBlueprintStatus Status=BP->Status;const bool Dependencies=BP->bCachedDependenciesUpToDate;
            const FString Raw=Dispatch(Command(BP->GetPathName(),Bad,Form,BadFirst));TSharedPtr<FJsonObject> Reply;
            if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw),Reply)||!Reply.IsValid()) {AddError(TEXT("Native dispatch must return JSON"));return false;}
            TestFalse(TEXT("Invalid component request rejected"),Reply->GetBoolField(TEXT("success")));
            TestEqual(TEXT("Early actionable rejection code"),Reply->GetStringField(TEXT("code")),FString(TEXT("COMPONENT_REQUEST_REJECTED")));
            for(const TCHAR* Key:{TEXT("targetAccessed"),TEXT("targetLoadRequested"),TEXT("targetMutationStarted"),TEXT("currentRequestMutation"),TEXT("compiled"),TEXT("saveAttempted")}) TestFalse(Key,Reply->GetBoolField(Key));
            TestEqual(TEXT("Native SCS graph and members unchanged"),State(BP.Get()),Before);TestEqual(TEXT("Pre-existing dirtiness retained"),Package->IsDirty(),Dirty);
            TestTrue(TEXT("Blueprint status unchanged"),BP->Status==Status);TestEqual(TEXT("Dependency state unchanged"),static_cast<bool>(BP->bCachedDependenciesUpToDate),Dependencies);
            TestEqual(TEXT("No rejected target compile"),Compiles,0);TestEqual(TEXT("No rejected package save"),Saves,0);TestFalse(TEXT("No target disk write"),FPaths::FileExists(TargetFile));
            const FString Missing=Root+TEXT("/BP_Absent_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
            const FString MissingFile=FPackageName::LongPackageNameToFilename(Missing,FPackageName::GetAssetPackageExtension());
            TestNull(TEXT("Auto-create target absent before"),FindObject<UPackage>(nullptr,*Missing));
            const FString MissingRaw=Dispatch(Command(Missing,Bad,Form,BadFirst));
            TestTrue(TEXT("Absent target rejected before auto-create"),MissingRaw.Contains(TEXT("COMPONENT_REQUEST_REJECTED")));
            TestNull(TEXT("Auto-create package still absent"),FindObject<UPackage>(nullptr,*Missing));TestFalse(TEXT("No auto-create asset file"),FPaths::FileExists(MissingFile));
            // Also cover the earlier explicit CreateBlueprint branch with no blueprint field.
            const auto Create=Command(Missing,Bad,Form,BadFirst);Create->RemoveField(TEXT("blueprint"));
            Create->SetStringField(TEXT("action"),TEXT("CreateBlueprint"));Create->SetStringField(TEXT("name"),FPackageName::GetShortName(Missing));
            Create->SetStringField(TEXT("path"),Root);Create->SetStringField(TEXT("parent_class"),TEXT("Actor"));
            TestTrue(TEXT("Explicit create also rejected before factory"),Dispatch(Create).Contains(TEXT("COMPONENT_REQUEST_REJECTED")));
            TestNull(TEXT("Explicit create package remains absent"),FindObject<UPackage>(nullptr,*Missing));TestFalse(TEXT("Explicit create writes no disk file"),FPaths::FileExists(MissingFile));
            TestEqual(TEXT("All refused targets compile zero times"),Compiles,0);TestEqual(TEXT("All refused packages save zero times"),Saves,0);
        }
    }
    // Invalid containers must reject the entire request, including valid peer operations.
    for(const FString& ContainerRaw:{FString(TEXT("null")),FString(TEXT("false")),FString(TEXT("42")),FString(TEXT("\"not-json\""))})
    {
        const auto Request=Command(BP->GetPathName(),Value(BadJson[0]),0,false);Request->SetField(TEXT("add_components"),Value(ContainerRaw));
        const FString Before=State(BP.Get());TestTrue(TEXT("Malformed component container rejected early"),Dispatch(Request).Contains(TEXT("INVALID_COMPONENT_CONTAINER")));
        TestEqual(TEXT("Malformed container does not run peers"),State(BP.Get()),Before);TestEqual(TEXT("Malformed container no compile"),Compiles,0);TestEqual(TEXT("Malformed container no save"),Saves,0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroComponentAdmissionCanonical,"Nwiro.Pro.ComponentAdmission.CanonicalRootParentCompatibility",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNwiroComponentAdmissionCanonical::RunTest(const FString& Parameters)
{
    using namespace NwiroComponentAdmissionTest;
    if(!GEditor || !IsInGameThread()) {AddError(TEXT("Requires actual editor game-thread dispatch"));return false;}
    for(int32 Form:{0,1,2,3,4,8})
    {
        const FString P=TEXT("/Game/__NwiroComponentPositive_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT("/BP_Target");
        TStrongObjectPtr<UPackage> Package(CreatePackage(*P));
        TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),FName(TEXT("BP_Target")),BPTYPE_Normal));
        if(!BP.IsValid()||!BP->SimpleConstructionScript) {AddError(TEXT("Positive Blueprint setup failed"));return false;}
        const auto Root=MakeComponentJson(TEXT("RequestedRoot"));Root->SetBoolField(TEXT("makeRoot"),true);Root->SetStringField(TEXT("parent"),TEXT(""));
        const auto Child=MakeComponentJson(TEXT("RequestedChild"));Child->SetBoolField(TEXT("makeRoot"),false);Child->SetStringField(TEXT("parent"),TEXT("RequestedRoot"));
        const auto Request=MakeShared<FJsonObject>();Request->SetStringField(TEXT("blueprint"),BP->GetPathName());Request->SetBoolField(TEXT("compile"),false);
        Container(Request,{MakeShared<FJsonValueObject>(Root),MakeShared<FJsonValueObject>(Child)},Form);
        TSharedPtr<FJsonObject> Reply;const FString Raw=Dispatch(Request);
        AddInfo(FString::Printf(TEXT("Canonical component form=%d receipt=%s"),Form,*Raw));
        if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw),Reply)||!Reply.IsValid()) {AddError(TEXT("Canonical result not JSON"));return false;}
        TestTrue(TEXT("Canonical root and parent accepted"),Reply->GetBoolField(TEXT("success")));
        USCS_Node* RootNode=nullptr;USCS_Node* ChildNode=nullptr;
        for(USCS_Node* N:BP->SimpleConstructionScript->GetAllNodes()) if(N)
        {if(N->GetVariableName()==FName(TEXT("RequestedRoot")))RootNode=N;if(N->GetVariableName()==FName(TEXT("RequestedChild")))ChildNode=N;}
        if(!RootNode||!ChildNode) {AddError(TEXT("Requested native SCS nodes absent"));return false;}
        TestTrue(TEXT("Actor fixture has requested root identity"),BP->SimpleConstructionScript->GetRootNodes().Contains(RootNode));
        TestTrue(TEXT("Named child actually attaches to requested root"),RootNode->GetChildNodes().Contains(ChildNode));
        // Single object and stringified object compatibility preserve canonical parent.
        for(int32 SingleForm:{5,6})
        {
            const auto Single=MakeComponentJson(SingleForm==5?TEXT("SingleChild"):TEXT("EncodedChild"));Single->SetStringField(TEXT("parent"),TEXT("RequestedRoot"));
            const auto One=MakeShared<FJsonObject>();One->SetStringField(TEXT("blueprint"),BP->GetPathName());One->SetBoolField(TEXT("compile"),false);
            Container(One,{MakeShared<FJsonValueObject>(Single)},SingleForm);
            TSharedPtr<FJsonObject> SingleReply;const FString OneRaw=Dispatch(One);
            AddInfo(FString::Printf(TEXT("Canonical component outerForm=%d singleForm=%d receipt=%s"),Form,SingleForm,*OneRaw));
            if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OneRaw),SingleReply)||!SingleReply.IsValid()) {AddError(TEXT("Single-item result not JSON"));return false;}
            TestTrue(TEXT("Single-object canonical parent accepted"),SingleReply->GetBoolField(TEXT("success")));
            int32 Matches=0;for(const USCS_Node* N:RootNode->GetChildNodes()) if(N && N->GetVariableName()==FName(*Single->GetStringField(TEXT("name"))))++Matches;
            TestEqual(TEXT("Single-item attachment identity verified"),Matches,1);
        }
        TestFalse(TEXT("Positive compile:false fixture stays unsaved"),FPaths::FileExists(FPackageName::LongPackageNameToFilename(P,FPackageName::GetAssetPackageExtension())));
    }
    return true;
}
#endif
