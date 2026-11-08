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
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NwiroVariableEditabilityTest
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
static TSharedRef<FJsonObject> Variable(const FString& Name)
{
    const auto V=MakeShared<FJsonObject>();V->SetStringField(TEXT("name"),Name);V->SetStringField(TEXT("type"),TEXT("string"));return V;
}
static void Container(const TSharedRef<FJsonObject>& R,const TArray<TSharedPtr<FJsonValue>>& Items,int32 Form)
{
    if(Form==0)R->SetArrayField(TEXT("add_variables"),Items);
    if(Form==1)R->SetArrayField(TEXT("variables"),Items);
    if(Form==2)R->SetStringField(TEXT("add_variables"),Encoded(Items));
    if(Form==3||Form==4||Form==8)
    {
        const auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("type"),TEXT("add_variables"));A->SetArrayField(TEXT("variables"),Items);
        const TArray<TSharedPtr<FJsonValue>> Actions{MakeShared<FJsonValueObject>(A)};
        if(Form==3)R->SetArrayField(TEXT("actions"),Actions);
        if(Form==4)R->SetStringField(TEXT("actions"),Encoded(Actions));
        if(Form==8)R->SetObjectField(TEXT("actions"),A);
    }
    if(Form==5)R->SetField(TEXT("variable"),Items[0]);
    if(Form==6&&Items[0]->Type==EJson::Object)R->SetStringField(TEXT("variable"),Json(Items[0]->AsObject().ToSharedRef()));
    if(Form==6&&Items[0]->Type!=EJson::Object)R->SetStringField(TEXT("variable"),TEXT("[null]"));
    if(Form==7){R->SetArrayField(TEXT("add_variables"),Items);R->SetNumberField(TEXT("actions"),123);}
}
static TSharedRef<FJsonObject> Command(const FString& Target,const TSharedPtr<FJsonValue>& Bad,int32 Form,bool BadFirst)
{
    const auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("blueprint"),Target);
    // Omit compile so failed admission must block the actual default-true flush.
    const TSharedPtr<FJsonValue> Good=MakeShared<FJsonValueObject>(Variable(TEXT("MustNotAddVariable")));
    const TArray<TSharedPtr<FJsonValue>> Items{BadFirst?Bad:Good,BadFirst?Good:Bad};
    Container(R,(Form==5||Form==6)?TArray<TSharedPtr<FJsonValue>>{Bad}:Items,Form);
    const auto C=MakeShared<FJsonObject>();C->SetStringField(TEXT("name"),TEXT("MustNotAddComponent"));C->SetStringField(TEXT("class"),TEXT("SceneComponent"));
    R->SetArrayField(TEXT("add_components"),{MakeShared<FJsonValueObject>(C)});
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
    for(const FBPVariableDescription& V:BP->NewVariables)
    {
        Rows.Add(FString::Printf(TEXT("Var:%s:%s:%s:%llu"),*V.VarGuid.ToString(),*V.VarName.ToString(),*V.DefaultValue,static_cast<unsigned long long>(V.PropertyFlags)));
        for(const FBPVariableMetaDataEntry& M:V.MetaDataArray)Rows.Add(TEXT("Meta:")+V.VarName.ToString()+TEXT(":")+M.DataKey.ToString()+TEXT(":")+M.DataValue);
    }
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroVariableEditabilityNoTouch,"Nwiro.Pro.VariableEditability.RejectedWholeRequestNoTouch",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNwiroVariableEditabilityNoTouch::RunTest(const FString& Parameters)
{
    using namespace NwiroVariableEditabilityTest;
    if(!GEditor || !IsInGameThread()) {AddError(TEXT("Requires actual editor game-thread dispatch"));return false;}
    const FString Root=TEXT("/Game/__NwiroVariableEditability_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UPackage> Package(CreatePackage(*(Root+TEXT("/BP_Target"))));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),FName(TEXT("BP_Target")),BPTYPE_Normal));
    if(!BP.IsValid() || !BP->SimpleConstructionScript) {AddError(TEXT("Real Blueprint factory failed"));return false;}
    const FString TargetFile=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
    TestFalse(TEXT("Owned target starts unsaved"),FPaths::FileExists(TargetFile));
    int32 Compiles=0,Saves=0;
    const FDelegateHandle CH=GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* Seen){if(Seen && Seen->GetOutermost()->GetName().StartsWith(Root))++Compiles;});
    const FDelegateHandle SH=UPackage::PreSavePackageWithContextEvent.AddLambda([&](UPackage* Seen,FObjectPreSaveContext){if(Seen && Seen->GetName().StartsWith(Root))++Saves;});
    ON_SCOPE_EXIT {GEditor->OnBlueprintPreCompile().Remove(CH);UPackage::PreSavePackageWithContextEvent.Remove(SH);};
    TArray<FString> BadJson{
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"isExposed\":true}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"is_exposed\":false}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"ISEXPOSED\":true}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":\"true\"}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":\"false\"}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":0}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":1}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":null}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":{}}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":[]}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":\"true\"}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":\"false\"}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":0}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":1}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":null}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":{}}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"expose_on_spawn\":[]}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":false,\"expose_on_spawn\":true}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"instance_editable\":false,\"instanceEditable\":true}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"properties\":{\"isExposed\":true}}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"properties\":{\"instance_editable\":true}}"),
        TEXT("{\"name\":\"BadVariable\",\"type\":\"string\",\"properties\":{\"expose_on_spawn\":true}}"),
        TEXT("null"),
        TEXT("42"),
        TEXT("\"not-an-object\""),
    };
    const TArray<FString> InstanceAliases{TEXT("instanceEditable"), TEXT("Instance_Editable"), TEXT("_i_N_s_T_a_N_c_E_e_D_i_T_a_B_l_E_"), TEXT("instance_editable")};
    const TArray<FString> SpawnAliases{TEXT("exposeOnSpawn"), TEXT("Expose_On_Spawn"), TEXT("_e_X_p_O_s_E_o_N_s_P_a_W_n_"), TEXT("expose_on_spawn")};
    for (const auto* Aliases : {&InstanceAliases, &SpawnAliases})
    {
        for (const FString& Alias : *Aliases)
        {
            for (const FString& Invalid : {FString(TEXT("\"true\"")), FString(TEXT("1")), FString(TEXT("null")), FString(TEXT("{}")), FString(TEXT("[]"))})
            {
                const auto V = Variable(TEXT("BadVariable")); V->SetField(Alias, Value(Invalid)); BadJson.Add(Json(V));
            }
            if (Alias == Aliases->Last()) continue;
            for (bool Same : {false, true})
            {
                const auto V = Variable(TEXT("BadVariable")); V->SetBoolField(Alias, true); V->SetBoolField(Aliases->Last(), Same); BadJson.Add(Json(V));
            }
        }
    }
    for (const FString& Instance : InstanceAliases) for (const FString& Spawn : SpawnAliases)
    {
        const auto V = Variable(TEXT("BadVariable")); V->SetBoolField(Instance, false); V->SetBoolField(Spawn, true); BadJson.Add(Json(V));
    }
    for(bool Dirty:{false,true})
    {
        Package->SetDirtyFlag(Dirty); // Owned fixture only. No user package dirty flags touched.
        for(const FString& BadRaw:BadJson) for(int32 Form=0;Form<9;++Form) for(bool BadFirst:{false,true})
        {
            const auto Bad=Value(BadRaw);if(!Bad.IsValid()) {AddError(TEXT("Malformed test fixture"));return false;}
            const FString Before=State(BP.Get());const EBlueprintStatus Status=BP->Status;const bool Dependencies=BP->bCachedDependenciesUpToDate;
            const FString Raw=Dispatch(Command(BP->GetPathName(),Bad,Form,BadFirst));TSharedPtr<FJsonObject> Reply;
            if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw),Reply)||!Reply.IsValid()) {AddError(TEXT("Native dispatch must return JSON"));return false;}
            TestFalse(TEXT("Invalid variable options rejected"),Reply->GetBoolField(TEXT("success")));
            TestEqual(TEXT("Early actionable rejection code"),Reply->GetStringField(TEXT("code")),FString(TEXT("VARIABLE_EDITABILITY_REJECTED")));
            bool bSaved = true;
            TestTrue(TEXT("Early rejection explicitly reports no disk save"), Reply->TryGetBoolField(TEXT("savedToDisk"), bSaved) && !bSaved);
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!TestTrue(TEXT("Early rejection retains an operation ledger"), Reply->TryGetArrayField(TEXT("operations"), Operations)
                && Operations && Operations->Num() == 1 && (*Operations)[0].IsValid()
                && (*Operations)[0]->Type == EJson::Object && (*Operations)[0]->AsObject().IsValid())) return false;
            const auto Ledger = (*Operations)[0]->AsObject();
            const bool bUnknownCardinality = Form == 5 && Bad->Type != EJson::Object;
            if (bUnknownCardinality)
            {
                const auto Count = Ledger->TryGetField(TEXT("requestUnits"));
                TestTrue(TEXT("Undecodable singular container has explicitly unknown cardinality"), Count.IsValid() && Count->Type == EJson::Null);
                const TArray<TSharedPtr<FJsonValue>>* ContainerErrors = nullptr;
                TestTrue(TEXT("Undecodable container still has an actionable diagnostic"), Ledger->TryGetArrayField(TEXT("errors"), ContainerErrors) && ContainerErrors && ContainerErrors->Num() > 0);
            }
            else
            {
                const int32 ExpectedUnits = (Form == 5 || Form == 6) ? 1 : 2;
                TestEqual(TEXT("Early rejection knows decoded request cardinality"), Ledger->GetNumberField(TEXT("requestUnits")), static_cast<double>(ExpectedUnits));
                TSet<int32> Accounted;
                int32 Entries = 0;
                for (const TCHAR* Bucket : {TEXT("created"), TEXT("modified"), TEXT("removed"), TEXT("errors"), TEXT("skipped")})
                {
                    const TArray<TSharedPtr<FJsonValue>>* Units = nullptr;
                    if (!TestTrue(TEXT("Every ledger bucket is present"), Ledger->TryGetArrayField(Bucket, Units) && Units)) return false;
                    const bool Applied = FString(Bucket) == TEXT("created") || FString(Bucket) == TEXT("modified") || FString(Bucket) == TEXT("removed");
                    if (Applied) TestEqual(TEXT("Rejected ledger applies zero units"), Units->Num(), 0);
                    for (const auto& Unit : *Units)
                    {
                        double Index = -1;
                        if (!TestTrue(TEXT("Every unit has a numeric index"), Unit.IsValid() && Unit->Type == EJson::Object
                            && Unit->AsObject()->TryGetNumberField(TEXT("unit"), Index))) return false;
                        TestTrue(TEXT("Ledger index belongs to this decoded request"), Index >= 0 && Index < ExpectedUnits && Index == static_cast<int32>(Index));
                        Accounted.Add(static_cast<int32>(Index)); ++Entries;
                    }
                }
                TestEqual(TEXT("One ledger entry per requested unit, even with multiple option issues"), Entries, ExpectedUnits);
                TestEqual(TEXT("Every requested unit has exactly one ledger entry"), Accounted.Num(), ExpectedUnits);
            }
            for(const TCHAR* Key:{TEXT("targetAccessed"),TEXT("targetLoadRequested"),TEXT("targetMutationStarted"),TEXT("currentRequestMutation"),TEXT("compiled"),TEXT("saveAttempted")}) TestFalse(Key,Reply->GetBoolField(Key));
            TestEqual(TEXT("Native SCS graph and members unchanged"),State(BP.Get()),Before);TestEqual(TEXT("Pre-existing dirtiness retained"),Package->IsDirty(),Dirty);
            TestTrue(TEXT("Blueprint status unchanged"),BP->Status==Status);TestEqual(TEXT("Dependency state unchanged"),static_cast<bool>(BP->bCachedDependenciesUpToDate),Dependencies);
            TestEqual(TEXT("No rejected target compile"),Compiles,0);TestEqual(TEXT("No rejected package save"),Saves,0);TestFalse(TEXT("No target disk write"),FPaths::FileExists(TargetFile));
            const FString Missing=Root+TEXT("/BP_Absent_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
            const FString MissingFile=FPackageName::LongPackageNameToFilename(Missing,FPackageName::GetAssetPackageExtension());
            TestNull(TEXT("Auto-create target absent before"),FindObject<UPackage>(nullptr,*Missing));
            const FString MissingRaw=Dispatch(Command(Missing,Bad,Form,BadFirst));
            TestTrue(TEXT("Absent target rejected before auto-create"),MissingRaw.Contains(TEXT("VARIABLE_EDITABILITY_REJECTED")));
            TestNull(TEXT("Auto-create package still absent"),FindObject<UPackage>(nullptr,*Missing));TestFalse(TEXT("No auto-create asset file"),FPaths::FileExists(MissingFile));
            // Also cover the earlier explicit CreateBlueprint branch with no blueprint field.
            const auto Create=Command(Missing,Bad,Form,BadFirst);Create->RemoveField(TEXT("blueprint"));
            Create->SetStringField(TEXT("action"),TEXT("CreateBlueprint"));Create->SetStringField(TEXT("name"),FPackageName::GetShortName(Missing));
            Create->SetStringField(TEXT("path"),Root);Create->SetStringField(TEXT("parent_class"),TEXT("Actor"));
            TestTrue(TEXT("Explicit create also rejected before factory"),Dispatch(Create).Contains(TEXT("VARIABLE_EDITABILITY_REJECTED")));
            TestNull(TEXT("Explicit create package remains absent"),FindObject<UPackage>(nullptr,*Missing));TestFalse(TEXT("Explicit create writes no disk file"),FPaths::FileExists(MissingFile));
            TestEqual(TEXT("All refused targets compile zero times"),Compiles,0);TestEqual(TEXT("All refused packages save zero times"),Saves,0);
        }
    }
    {
        const auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("blueprint"), BP->GetPathName());
        const auto First = MakeShared<FJsonObject>(); First->SetStringField(TEXT("type"), TEXT("add_variables"));
        First->SetArrayField(TEXT("variables"), {Value(BadJson[0])});
        const auto Last = MakeShared<FJsonObject>(); Last->SetStringField(TEXT("type"), TEXT("add_variables"));
        Last->SetArrayField(TEXT("variables"), {MakeShared<FJsonValueObject>(Variable(TEXT("MustNotHideEarlierBad")))});
        Request->SetArrayField(TEXT("actions"), {MakeShared<FJsonValueObject>(First), MakeShared<FJsonValueObject>(Last)});
        const FString Before = State(BP.Get());
        const FString Raw = Dispatch(Request);
        TestTrue(TEXT("Later action cannot hide earlier invalid variable"), Raw.Contains(TEXT("VARIABLE_EDITABILITY_REJECTED")));
        TSharedPtr<FJsonObject> Reply;
        if (!TestTrue(TEXT("Concatenated rejection parses JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) && Reply.IsValid())) return false;
        const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
        if (!TestTrue(TEXT("Concatenated variable actions retain one operation"), Reply->TryGetArrayField(TEXT("operations"), Operations)
            && Operations && Operations->Num() == 1 && (*Operations)[0].IsValid()
            && (*Operations)[0]->Type == EJson::Object && (*Operations)[0]->AsObject().IsValid())) return false;
        const auto Ledger = (*Operations)[0]->AsObject();
        TestEqual(TEXT("Concatenated action cardinality retained"), Ledger->GetNumberField(TEXT("requestUnits")), 2.0);
        const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Skipped = nullptr;
        if (!TestTrue(TEXT("Concatenated actions reject one unit and skip one"), Ledger->TryGetArrayField(TEXT("errors"), Errors)
            && Errors && Errors->Num() == 1 && Ledger->TryGetArrayField(TEXT("skipped"), Skipped) && Skipped && Skipped->Num() == 1)) return false;
        TestEqual(TEXT("First action is rejected global unit zero"), (*Errors)[0]->AsObject()->GetNumberField(TEXT("unit")), 0.0);
        TestEqual(TEXT("Second action is skipped global unit one"), (*Skipped)[0]->AsObject()->GetNumberField(TEXT("unit")), 1.0);
        const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
        TestTrue(TEXT("Legacy rejection message is retained"), Reply->TryGetArrayField(TEXT("messages"), Messages) && Messages && Messages->Num() > 0);

        TestEqual(TEXT("Rejected concatenated actions preserve target"), State(BP.Get()), Before);
        TestEqual(TEXT("Rejected concatenated actions do not compile"), Compiles, 0);
        TestEqual(TEXT("Rejected concatenated actions do not save"), Saves, 0);
    }
    // Invalid containers must reject the entire request, including valid peer operations.
    for(const FString& ContainerRaw:{FString(TEXT("null")),FString(TEXT("false")),FString(TEXT("42")),FString(TEXT("\"not-json\""))})
    {
        const auto Request=Command(BP->GetPathName(),Value(BadJson[0]),0,false);Request->SetField(TEXT("add_variables"),Value(ContainerRaw));
        const FString Before=State(BP.Get());TestTrue(TEXT("Malformed variable container rejected early"),Dispatch(Request).Contains(TEXT("INVALID_VARIABLE_CONTAINER")));
        TestEqual(TEXT("Malformed container does not run peers"),State(BP.Get()),Before);TestEqual(TEXT("Malformed container no compile"),Compiles,0);TestEqual(TEXT("Malformed container no save"),Saves,0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroVariableEditabilityFlags,"Nwiro.Pro.VariableEditability.CanonicalFlagsAndOmission",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNwiroVariableEditabilityFlags::RunTest(const FString& Parameters)
{
    using namespace NwiroVariableEditabilityTest;
    if(!GEditor||!IsInGameThread()){AddError(TEXT("Requires actual editor game-thread dispatch"));return false;}
    struct FAliases { const TCHAR* Instance; const TCHAR* Spawn; };
    const FAliases Spellings[]{
        {TEXT("instance_editable"), TEXT("expose_on_spawn")},
        {TEXT("instanceEditable"), TEXT("exposeOnSpawn")},
        {TEXT("Instance_Editable"), TEXT("Expose_On_Spawn")},
        {TEXT("_i_N_s_T_a_N_c_E_e_D_i_T_a_B_l_E_"), TEXT("_e_X_p_O_s_E_o_N_s_P_a_W_n_")}
    };
    for (const FAliases& Spelling : Spellings) for(int32 Form:{0,1,2,3,4,5,6,8})
    {
        const FString P=TEXT("/Game/__NwiroVariableFlags_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT("/BP_Target");
        TStrongObjectPtr<UPackage> Package(CreatePackage(*P));
        TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),FName(TEXT("BP_Target")),BPTYPE_Normal));
        if(!BP.IsValid()){AddError(TEXT("Real positive Blueprint factory failed"));return false;}
        // -1 means omit. Explicit false retains editable Blueprint defaults.
        struct FCase{const TCHAR* Name;int32 Instance;int32 Spawn;bool Editable;};
        const FCase Cases[]{
            {TEXT("Label"),1,-1,true},{TEXT("Description"),1,0,true},{TEXT("ExplicitFalse"),0,-1,false},
            {TEXT("LegacyOmitted"),-1,-1,false},{TEXT("LegacySpawnOnly"),-1,1,true},{TEXT("TrueAndSpawn"),1,1,true},
            {TEXT("FalseAndNoSpawn"),0,0,false},{TEXT("OmittedAndNoSpawn"),-1,0,false}
        };
        for(const FCase& C:Cases)
        {
            const auto V=Variable(C.Name);V->SetStringField(TEXT("default"),TEXT("Requested string value"));
            if(C.Instance!=-1)V->SetBoolField(Spelling.Instance,C.Instance==1);
            if(C.Spawn!=-1)V->SetBoolField(Spelling.Spawn,C.Spawn==1);
            const auto Request=MakeShared<FJsonObject>();Request->SetStringField(TEXT("blueprint"),BP->GetPathName());Request->SetBoolField(TEXT("compile"),false);
            Container(Request,{MakeShared<FJsonValueObject>(V)},Form);
            // Exercise the merged prepared variable/function handlers through Pro's
            // mixed-node staging path as well as the legacy variable-only path.
            if (Form == 0 && FString(C.Name) == TEXT("Label"))
            {
                const auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("type"), TEXT("Branch")); Node->SetStringField(TEXT("ref"), TEXT("aliasBranch"));
                Request->SetArrayField(TEXT("add_nodes"), {MakeShared<FJsonValueObject>(Node)});
                const auto Function = MakeShared<FJsonObject>(); Function->SetStringField(TEXT("name"), TEXT("PreparedAliasFunction"));
                Request->SetArrayField(TEXT("add_functions"), {MakeShared<FJsonValueObject>(Function)});
            }
            const FString Raw=Dispatch(Request);TSharedPtr<FJsonObject> Reply;
            if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw),Reply)||!Reply.IsValid()){AddError(TEXT("Positive dispatcher result must be JSON"));return false;}
            TestTrue(TEXT("Valid editability request accepted"),Reply->GetBoolField(TEXT("success")));
            const int32 Index=FBlueprintEditorUtils::FindNewVariableIndex(BP.Get(),FName(C.Name));
            if(Index==INDEX_NONE){AddError(TEXT("Requested variable missing from actual Blueprint"));return false;}
            const FBPVariableDescription& Found=BP->NewVariables[Index];
            TestTrue(TEXT("Blueprint defaults remain editable"),(Found.PropertyFlags&CPF_Edit)!=0);
            TestEqual(TEXT("Explicit native instance-editability flags"),(Found.PropertyFlags&CPF_DisableEditOnInstance)==0,C.Editable);
            TestEqual(TEXT("String type retained"),Found.VarType.PinCategory,UEdGraphSchema_K2::PC_String);
            TestEqual(TEXT("Requested default retained"),Found.DefaultValue,FString(TEXT("Requested string value")));
            TestEqual(TEXT("Spawn metadata stays independent"),Found.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn),C.Spawn==1);
            if(C.Spawn==1 && Found.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn))TestEqual(TEXT("Explicit spawn metadata true"),Found.GetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn),FString(TEXT("true")));
        }
        // An explicit test-owned compile checks generated FProperty flags. This is
        // outside the compile:false tool request; it must not save the fixture.
        FKismetEditorUtilities::CompileBlueprint(BP.Get(), EBlueprintCompileOptions::SkipSave);
        if(!BP->GeneratedClass){AddError(TEXT("Native compilation did not produce a class"));return false;}
        for(const FCase& C:Cases)
        {
            const FProperty* Property=FindFProperty<FProperty>(BP->GeneratedClass,FName(C.Name));
            if(!Property){AddError(TEXT("Generated variable property missing"));return false;}
            TestTrue(TEXT("Generated class default edit flag"),Property->HasAnyPropertyFlags(CPF_Edit));
            TestEqual(TEXT("Generated class instance flag matches requested state"),!Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance),C.Editable);
            TestEqual(TEXT("Generated spawn metadata matches requested state"),Property->HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn),C.Spawn==1);
        }
        TestFalse(TEXT("No fixture disk save"),FPaths::FileExists(FPackageName::LongPackageNameToFilename(P,FPackageName::GetAssetPackageExtension())));
    }
    return true;
}
#endif
