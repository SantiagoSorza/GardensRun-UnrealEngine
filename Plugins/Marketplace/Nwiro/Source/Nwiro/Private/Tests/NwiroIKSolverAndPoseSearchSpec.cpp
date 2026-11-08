// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroIKTools.h"

#if WITH_DEV_AUTOMATION_TESTS && NWIRO_HAS_IK_TOOLS
#include "Misc/AutomationTest.h"
#include "UObject/ObjectSaveContext.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Json.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "StructUtils/InstancedStruct.h"

namespace NwiroIKContractProof
{
using FTool = FString (*)(const FString&);
static TSharedPtr<FJsonObject> Call(FTool Tool, const TMap<FString, FString>& Fields)
{
 TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
 for (const auto& Pair : Fields) Args->SetStringField(Pair.Key, Pair.Value);
 FString Input; FJsonSerializer::Serialize(Args, TJsonWriterFactory<>::Create(&Input));
 TSharedPtr<FJsonObject> Result; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Tool(Input)), Result);
 return Result;
}
static bool Success(const TSharedPtr<FJsonObject>& R)
{
 bool Value = false; return R.IsValid() && R->TryGetBoolField(TEXT("success"), Value) && Value;
}
static FString Folder()
{
 return TEXT("/Game/__NwiroIKContract_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSolverStructContract, "Nwiro.Pro.IK.SolverStructsAndMappings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSolverStructContract::RunTest(const FString& Parameters)
{
 using namespace NwiroIKContractProof;
 const FString Base = Folder();
 auto Created = Call(&FNwiroIKTools::CreateIKRig, {{TEXT("name"),TEXT("IK_Proof")},{TEXT("path"),Base}});
 if (!TestTrue(TEXT("fixture rig created"), Success(Created))) return false;
 TStrongObjectPtr<UIKRigDefinition> Rig(Cast<UIKRigDefinition>(UEditorAssetLibrary::LoadAsset(Created->GetStringField(TEXT("path")))));
 UIKRigController* Controller = Rig ? UIKRigController::GetController(Rig.Get()) : nullptr;
 if (!TestNotNull(TEXT("rig controller"), Controller)) return false;
 int32 Expected = Controller->GetNumSolvers();
 for (const TCHAR* Type : {TEXT("LimbSolver"),TEXT("SetTransform"),TEXT("FBIKSolver"),TEXT("PoleSolver"),TEXT("/Script/IKRig.IKRigLimbSolver")})
 {
  auto R = Call(&FNwiroIKTools::AddIKSolver, {{TEXT("rig"),Rig->GetPathName()},{TEXT("type"),Type}});
  TestTrue(FString::Printf(TEXT("solver %s"),Type),Success(R));
  TestEqual(TEXT("native stack grew once"),Controller->GetNumSolvers(),++Expected);
  TestNotNull(TEXT("native solver exists"),Controller->GetSolverAtIndex(Expected-1));
  const TArray<FString> ExpectedTypes = {TEXT("IKRigLimbSolver"),TEXT("IKRigSetTransform"),TEXT("IKRigFullBodyIKSolver"),TEXT("IKRigPoleSolver"),TEXT("IKRigLimbSolver")};
  const FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(Expected-1);
  if (TestNotNull(TEXT("native solver struct"),SolverStruct) && TestNotNull(TEXT("native solver struct type"),SolverStruct->GetScriptStruct()))
   TestEqual(TEXT("exact native solver type"),SolverStruct->GetScriptStruct()->GetName(),ExpectedTypes[Expected-1]);
 }
 for (const TCHAR* Type : {TEXT("MissingSolver0908"),TEXT("/Script/Engine.Actor"),TEXT("/Script/IKRig.IKRigSolverBase")})
 {
  Rig->GetOutermost()->SetDirtyFlag(false);
  auto R = Call(&FNwiroIKTools::AddIKSolver, {{TEXT("rig"),Rig->GetPathName()},{TEXT("type"),Type}});
  TestFalse(TEXT("invalid solver rejected"),Success(R));
  TestEqual(TEXT("invalid solver preserves native stack"),Controller->GetNumSolvers(),Expected);
  TestFalse(TEXT("invalid solver does not dirty target"),Rig->GetOutermost()->IsDirty());
 }
 auto RetargetResult = Call(&FNwiroIKTools::CreateIKRetargeter,{{TEXT("name"),TEXT("RTG_Proof")},{TEXT("path"),Base}});
 if (!TestTrue(TEXT("fixture retargeter created"),Success(RetargetResult))) return false;
 TStrongObjectPtr<UIKRetargeter> Retargeter(Cast<UIKRetargeter>(UEditorAssetLibrary::LoadAsset(RetargetResult->GetStringField(TEXT("path")))));
 auto RetargetController = UIKRetargeterController::GetController(Retargeter.Get());
 if (!TestNotNull(TEXT("retarget controller"),RetargetController)) return false;
 Retargeter->GetOutermost()->SetDirtyFlag(false);
 auto Missing = Call(&FNwiroIKTools::SetChainMapping,{{TEXT("retargeter"),Retargeter->GetPathName()},{TEXT("sourceChain"),TEXT("Missing")},{TEXT("targetChain"),TEXT("Missing")}});
 TestFalse(TEXT("nonexistent chain rejects"),Success(Missing));
 TestFalse(TEXT("nonexistent mapping does not dirty"),Retargeter->GetOutermost()->IsDirty());
 USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
 if (!TestNotNull(TEXT("engine fixture mesh"),Mesh) || !TestTrue(TEXT("fixture bone exists"),Mesh->GetRefSkeleton().GetNum()>0)) return false;
 Controller->SetSkeletalMesh(Mesh);
 const FName Bone = Mesh->GetRefSkeleton().GetBoneName(0);
 Controller->AddRetargetChain(TEXT("SourceChain"),Bone,Bone,NAME_None);
 Controller->AddRetargetChain(TEXT("TargetChain"),Bone,Bone,NAME_None);
 RetargetController->SetIKRig(ERetargetSourceOrTarget::Source,Rig.Get());
 RetargetController->SetIKRig(ERetargetSourceOrTarget::Target,Rig.Get());
 if (RetargetController->GetNumRetargetOps()==0) RetargetController->AddDefaultOps();
 int32 Applicable=0;
 for (int32 Index=0;Index<RetargetController->GetNumRetargetOps();++Index)
 {
  const auto* Map=RetargetController->GetChainMapping(RetargetController->GetOpName(Index));
  if (Map && Map->HasChain(TEXT("TargetChain"),ERetargetSourceOrTarget::Target)) ++Applicable;
 }
 if (!TestTrue(TEXT("fixture has applicable mappings"),Applicable>0)) return false;
 auto Mapped=Call(&FNwiroIKTools::SetChainMapping,{{TEXT("retargeter"),Retargeter->GetPathName()},{TEXT("sourceChain"),TEXT("SourceChain")},{TEXT("targetChain"),TEXT("TargetChain")}});
 TestTrue(TEXT("mapping succeeded"),Success(Mapped));
 for (int32 Index=0;Index<RetargetController->GetNumRetargetOps();++Index)
 {
  const auto* Map=RetargetController->GetChainMapping(RetargetController->GetOpName(Index));
  if (Map && Map->HasChain(TEXT("TargetChain"),ERetargetSourceOrTarget::Target))
   TestEqual(TEXT("actual mapping changed"),Map->GetChainMappedTo(TEXT("TargetChain"),ERetargetSourceOrTarget::Target),FName(TEXT("SourceChain")));
 }
 auto Read=Call(&FNwiroIKTools::ReadIKRetargeter,{{TEXT("path"),Retargeter->GetPathName()}});
 TestTrue(TEXT("reader includes operation mappings"),Success(Read) && Read->GetArrayField(TEXT("chainMappings")).Num()>=Applicable);
 Rig->GetOutermost()->SetDirtyFlag(false);Retargeter->GetOutermost()->SetDirtyFlag(false);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroPoseFactoryContract, "Nwiro.Pro.IK.PoseSearchFactoryTargets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroPoseFactoryContract::RunTest(const FString& Parameters)
{
 using namespace NwiroIKContractProof;
 const FString Base = Folder();
 const FString SkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
 TStrongObjectPtr<USkeleton> Skeleton(LoadObject<USkeleton>(nullptr, *SkeletonPath));
 if (!TestNotNull(TEXT("installed compatible skeleton fixture"), Skeleton.Get())) return false;
 const bool SkeletonDirty = Skeleton->GetOutermost()->IsDirty();
 int32 Saves = 0;
 const FDelegateHandle SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
 { if (Object && Object->GetOutermost()->GetName().StartsWith(Base)) ++Saves; });
 ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle); };
 auto Invalid = Call(&FNwiroIKTools::CreatePoseSearchSchema, {{TEXT("name"), TEXT("PSS_Invalid")}, {TEXT("path"), Base}, {TEXT("skeleton"), TEXT("/Game/DefinitelyMissing.Skeleton")}});
 TestFalse(TEXT("missing skeleton rejected without loading it"), Success(Invalid));
 TestFalse(TEXT("missing skeleton creates no destination"), UEditorAssetLibrary::DoesAssetExist(Base / TEXT("PSS_Invalid")));
 auto WrongType = Call(&FNwiroIKTools::CreatePoseSearchSchema, {{TEXT("name"), TEXT("PSS_WrongType")}, {TEXT("path"), Base}, {TEXT("skeleton"), TEXT("/Engine/BasicShapes/Cube.Cube")}});
 TestFalse(TEXT("wrong asset type rejected before loading"), Success(WrongType));
 TestFalse(TEXT("wrong type creates no destination"), UEditorAssetLibrary::DoesAssetExist(Base / TEXT("PSS_WrongType")));
 // Real engine factory validation must diagnose the incompatible skeleton.
 // These are expected errors only for this negative case; success is still forbidden.
 AddExpectedError(TEXT("UPoseSearchSchema::AddBoneReference: couldn't initialize FBoneReference"), EAutomationExpectedErrorFlags::Contains, 1);
 AddExpectedError(TEXT("UPoseSearchSchema::Finalize: couldn't Finalize"), EAutomationExpectedErrorFlags::Contains, 1);
 auto Incompatible = Call(&FNwiroIKTools::CreatePoseSearchSchema, {{TEXT("name"), TEXT("PSS_Incompatible")}, {TEXT("path"), Base},
  {TEXT("skeleton"), TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton")}});
 if (!TestTrue(TEXT("incompatible response exists"), Incompatible.IsValid())) return false;
 TestFalse(TEXT("unfinalized defaults never report success"), Success(Incompatible));
 TestEqual(TEXT("incompatible defaults have explicit failure code"), Incompatible->GetStringField(TEXT("code")), FString(TEXT("pose_search_defaults_incompatible")));
 TestFalse(TEXT("incompatible response reports no creation"), Incompatible->GetBoolField(TEXT("created")));
 TestFalse(TEXT("incompatible response reports not ready"), Incompatible->GetBoolField(TEXT("ready")));
 const FString RejectedPackage = Base / TEXT("PSS_Incompatible");
 TestNull(TEXT("incompatible validation creates no destination package"), FindPackage(nullptr, *RejectedPackage));
 TestFalse(TEXT("incompatible validation registers no asset"), UEditorAssetLibrary::DoesAssetExist(RejectedPackage));
 TestFalse(TEXT("incompatible validation saves no asset"), FPackageName::DoesPackageExist(RejectedPackage));
 auto SchemaResult = Call(&FNwiroIKTools::CreatePoseSearchSchema, {{TEXT("name"), TEXT("PSS_Proof")}, {TEXT("path"), Base}, {TEXT("skeleton"), SkeletonPath}});
 if (!TestTrue(TEXT("compatible schema created"), Success(SchemaResult))) return false;
 TStrongObjectPtr<UPoseSearchSchema> Schema(Cast<UPoseSearchSchema>(UEditorAssetLibrary::LoadAsset(SchemaResult->GetStringField(TEXT("path")))));
 if (!TestNotNull(TEXT("actual schema"), Schema.Get())) return false;
 TestTrue(TEXT("schema retains exact requested skeleton"), Schema->GetSkeleton(Schema->GetDefaultRole()) == Skeleton.Get());
 TestTrue(TEXT("real factory authored and finalized default channels"), Schema->GetChannels().Num() >= 2 && Schema->SchemaCardinality > 0);
 TestTrue(TEXT("response reports verified default readiness"), SchemaResult->GetBoolField(TEXT("ready")));
 TestEqual(TEXT("reported cardinality is actual native state"), SchemaResult->GetIntegerField(TEXT("schemaCardinality")), Schema->SchemaCardinality);
 TestEqual(TEXT("reported finalized channel count is actual state"), SchemaResult->GetIntegerField(TEXT("channelCount")), Schema->GetChannels().Num());
 const bool SchemaDirty = Schema->GetOutermost()->IsDirty();
 auto DBResult = Call(&FNwiroIKTools::CreatePoseSearchDatabase, {{TEXT("name"), TEXT("PSD_Proof")}, {TEXT("path"), Base}, {TEXT("schema"), Schema->GetPathName()}});
 if (!TestTrue(TEXT("database factory target configured"), Success(DBResult))) return false;
 UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(UEditorAssetLibrary::LoadAsset(DBResult->GetStringField(TEXT("path"))));
 if (!TestNotNull(TEXT("actual database"), DB)) return false;
 TestTrue(TEXT("database has exact intended schema"), DB->Schema == Schema.Get());
 TestTrue(TEXT("database reports usable schema"), DBResult->GetBoolField(TEXT("schemaReady")));
 TestEqual(TEXT("database creation preserves input schema dirty state"), Schema->GetOutermost()->IsDirty(), SchemaDirty);
 auto Duplicate = Call(&FNwiroIKTools::CreatePoseSearchSchema, {{TEXT("name"), TEXT("PSS_Proof")}, {TEXT("path"), Base}, {TEXT("skeleton"), SkeletonPath}});
 TestFalse(TEXT("existing destination rejected"), Success(Duplicate));
 TestTrue(TEXT("existing schema identity unchanged"), StaticFindObject(UPoseSearchSchema::StaticClass(), nullptr, *Schema->GetPathName()) == Schema.Get());
 // An incomplete schema is a legitimate editable asset, but cannot be offered
 // as the ready schema for a new database by this creation tool.
 UPackage* EmptyPackage = CreatePackage(*(Base / TEXT("PSS_Empty")));
 TStrongObjectPtr<UPoseSearchSchema> Empty(NewObject<UPoseSearchSchema>(EmptyPackage, TEXT("PSS_Empty"), RF_Public | RF_Standalone));
 Empty->AddSkeleton(Skeleton.Get()); FAssetRegistryModule::AssetCreated(Empty.Get());
 EmptyPackage->SetDirtyFlag(false);
 auto BadDB = Call(&FNwiroIKTools::CreatePoseSearchDatabase, {{TEXT("name"), TEXT("PSD_Unready")}, {TEXT("path"), Base}, {TEXT("schema"), Empty->GetPathName()}});
 TestFalse(TEXT("unready schema blocks database creation"), Success(BadDB));
 TestFalse(TEXT("unready schema produces no database"), UEditorAssetLibrary::DoesAssetExist(Base / TEXT("PSD_Unready")));
 TestFalse(TEXT("unready source package remains clean"), EmptyPackage->IsDirty());
 TestEqual(TEXT("unready source channels are not modified"), Empty->GetChannels().Num(), 0);
 TestEqual(TEXT("input skeleton dirtiness preserved"), Skeleton->GetOutermost()->IsDirty(), SkeletonDirty);
 TestEqual(TEXT("no PoseSearch operation saved a package"), Saves, 0);
 Schema->GetOutermost()->SetDirtyFlag(false); DB->GetOutermost()->SetDirtyFlag(false);
 Empty->ClearFlags(RF_Public | RF_Standalone); FAssetRegistryModule::AssetDeleted(Empty.Get()); Empty->MarkAsGarbage(); EmptyPackage->MarkAsGarbage();
 return true;
}
#endif
