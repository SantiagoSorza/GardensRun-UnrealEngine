// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKTools.h"
#include "NwiroAssetGuard.h"
#include "Editor.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if NWIRO_HAS_IK_TOOLS
#include "Rig/IKRigDefinition.h"
#include "Retargeter/IKRetargeter.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RigEditor/IKRigController.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearchDatabaseFactory.h"
#include "PoseSearchSchemaFactory.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Json.h"
#include "NwiroTypeValidation.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "Retargeter/IKRetargetChainMapping.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroIK, Log, All);

// FIKRigController / FIKRetargeterController only exist in UE 5.7+.
// On older engines these tools return an unsupported error. The tool
// definitions are still listed (the LLM sees them) but execution is
// gated here at compile time.

// ============================================================
// HELPERS
// ============================================================

static USkeleton* FindSkeletonByPath(const FString& PathOrName)
{
	UObject* Asset = UEditorAssetLibrary::LoadAsset(PathOrName);
	if (USkeleton* Skel = Cast<USkeleton>(Asset)) return Skel;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(USkeleton::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<USkeleton>(A.GetAsset());
		}
	}
	return nullptr;
}

// ============================================================
// CREATE IK RIG
// ============================================================

FString FNwiroIKTools::CreateIKRig(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString SkelPath = Cmd->GetStringField(TEXT("skeleton"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Animation");

	// Add IK_ prefix if missing
	if (!Name.StartsWith(TEXT("IK_"))) Name = TEXT("IK_") + Name;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UIKRigDefinitionFactory* Factory = NewObject<UIKRigDefinitionFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UIKRigDefinition::StaticClass(), Factory);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(NewAsset);

	if (!IKRig)
		return TEXT("{\"success\":false,\"error\":\"Failed to create IKRig\"}");

	// Set skeleton via controller
	if (!SkelPath.IsEmpty())
	{
		USkeleton* Skeleton = FindSkeletonByPath(SkelPath);
		if (Skeleton)
		{
			// Use the skeletal mesh from the skeleton's preview mesh
			USkeletalMesh* PreviewMesh = Skeleton->GetPreviewMesh();
			if (PreviewMesh)
			{
				UIKRigController* Controller = UIKRigController::GetController(IKRig);
				if (Controller) Controller->SetSkeletalMesh(PreviewMesh);
			}
		}
	}

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *IKRig->GetPathName());
}

// ============================================================
// READ IK RIG
// ============================================================

FString FNwiroIKTools::ReadIKRig(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);

	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), IKRig->GetName());
	Result->SetStringField(TEXT("path"), IKRig->GetPathName());

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Rig controller unavailable\"}");
	UIKRigController& Controller = *ControllerPtr;

	// Skeletal mesh
	USkeletalMesh* Mesh = Controller.GetSkeletalMesh();
	Result->SetStringField(TEXT("skeletalMesh"), Mesh ? Mesh->GetPathName() : TEXT("None"));

	// Goals
	TArray<TSharedPtr<FJsonValue>> Goals;
	for (const UIKRigEffectorGoal* Goal : Controller.GetAllGoals())
	{
		if (!Goal) continue;
		TSharedRef<FJsonObject> G = MakeShareable(new FJsonObject());
		G->SetStringField(TEXT("name"), Goal->GoalName.ToString());
		G->SetStringField(TEXT("boneName"), Goal->BoneName.ToString());
		Goals.Add(MakeShareable(new FJsonValueObject(G)));
	}
	Result->SetArrayField(TEXT("goals"), Goals);

	// Solvers
	TArray<TSharedPtr<FJsonValue>> Solvers;
	for (const FIKRigSolverBase* Solver : Controller.GetSolverArray())
	{
		if (!Solver) continue;
		TSharedRef<FJsonObject> S = MakeShareable(new FJsonObject());
		S->SetStringField(TEXT("name"), Solver->GetNiceName().ToString());
		S->SetStringField(TEXT("class"), Solver->GetSolverSettingsType() ? Solver->GetSolverSettingsType()->GetName() : TEXT("Unknown"));
		S->SetBoolField(TEXT("enabled"), Solver->IsEnabled());
		Solvers.Add(MakeShareable(new FJsonValueObject(S)));
	}
	Result->SetArrayField(TEXT("solvers"), Solvers);

	// Retarget chains
	TArray<TSharedPtr<FJsonValue>> Chains;
	for (const FBoneChain& Chain : Controller.GetRetargetChains())
	{
		TSharedRef<FJsonObject> C = MakeShareable(new FJsonObject());
		C->SetStringField(TEXT("name"), Chain.ChainName.ToString());
		C->SetStringField(TEXT("startBone"), Chain.StartBone.BoneName.ToString());
		C->SetStringField(TEXT("endBone"), Chain.EndBone.BoneName.ToString());
		Chains.Add(MakeShareable(new FJsonValueObject(C)));
	}
	Result->SetArrayField(TEXT("retargetChains"), Chains);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD IK GOAL
// ============================================================

FString FNwiroIKTools::AddIKGoal(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RigPath = Cmd->GetStringField(TEXT("rig"));
	FString GoalName = Cmd->GetStringField(TEXT("name"));
	FString BoneName = Cmd->GetStringField(TEXT("bone"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RigPath);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);
	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *RigPath);

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Rig controller unavailable\"}");
	UIKRigController& Controller = *ControllerPtr;
	FName GoalResult = Controller.AddNewGoal(FName(*GoalName), FName(*BoneName));

	if (GoalResult == NAME_None)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to add goal '%s' on bone '%s'\"}"), *GoalName, *BoneName);

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"goal\":\"%s\",\"bone\":\"%s\"}"), *GoalName, *BoneName);
}

// ============================================================
// CREATE IK RETARGETER
// ============================================================

FString FNwiroIKTools::CreateIKRetargeter(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString SourceIKRigPath = Cmd->GetStringField(TEXT("sourceIKRig"));
	FString TargetIKRigPath = Cmd->GetStringField(TEXT("targetIKRig"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Animation");

	// Add RTG_ prefix if missing
	if (!Name.StartsWith(TEXT("RTG_"))) Name = TEXT("RTG_") + Name;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UIKRetargeter::StaticClass(), Factory);
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(NewAsset);

	if (!Retargeter)
		return TEXT("{\"success\":false,\"error\":\"Failed to create IKRetargeter\"}");

	// Set source and target IK Rigs
	UIKRetargeterController* ControllerPtr = UIKRetargeterController::GetController(Retargeter);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Retargeter controller unavailable\"}");
	UIKRetargeterController& Controller = *ControllerPtr;

	if (!SourceIKRigPath.IsEmpty())
	{
		UIKRigDefinition* SourceRig = Cast<UIKRigDefinition>(UEditorAssetLibrary::LoadAsset(SourceIKRigPath));
		if (SourceRig)
		{
			Controller.SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
		}
	}

	if (!TargetIKRigPath.IsEmpty())
	{
		UIKRigDefinition* TargetRig = Cast<UIKRigDefinition>(UEditorAssetLibrary::LoadAsset(TargetIKRigPath));
		if (TargetRig)
		{
			Controller.SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
		}
	}

	Retargeter->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *Retargeter->GetPathName());
}

// ============================================================
// READ IK RETARGETER
// ============================================================

FString FNwiroIKTools::ReadIKRetargeter(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Asset);

	if (!Retargeter)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRetargeter not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Retargeter->GetName());
	Result->SetStringField(TEXT("path"), Retargeter->GetPathName());

	UIKRetargeterController* ControllerPtr = UIKRetargeterController::GetController(Retargeter);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Retargeter controller unavailable\"}");
	UIKRetargeterController& Controller = *ControllerPtr;

	// Source/Target IK Rigs
	const UIKRigDefinition* SourceRig = Controller.GetIKRig(ERetargetSourceOrTarget::Source);
	const UIKRigDefinition* TargetRig = Controller.GetIKRig(ERetargetSourceOrTarget::Target);
	Result->SetStringField(TEXT("sourceIKRig"), SourceRig ? SourceRig->GetPathName() : TEXT("None"));
	Result->SetStringField(TEXT("targetIKRig"), TargetRig ? TargetRig->GetPathName() : TEXT("None"));

	// Chain mappings
	TArray<TSharedPtr<FJsonValue>> Mappings;
	for (int32 OpIndex = 0; OpIndex < Controller.GetNumRetargetOps(); ++OpIndex)
	{
		const FName OpName = Controller.GetOpName(OpIndex);
		const FRetargetChainMapping* ChainMapping = Controller.GetChainMapping(OpName);
		if (!ChainMapping) continue;
		for (const FRetargetChainPair& Pair : ChainMapping->GetChainPairs())
		{
			TSharedRef<FJsonObject> M = MakeShareable(new FJsonObject());
			M->SetStringField(TEXT("operation"), OpName.ToString());
			M->SetNumberField(TEXT("operationIndex"), OpIndex);
			M->SetStringField(TEXT("sourceChain"), Pair.SourceChainName.ToString());
			M->SetStringField(TEXT("targetChain"), Pair.TargetChainName.ToString());
			Mappings.Add(MakeShareable(new FJsonValueObject(M)));
		}
	}
	Result->SetArrayField(TEXT("chainMappings"), Mappings);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CREATE POSE SEARCH SCHEMA
// ============================================================

namespace NwiroPoseSearchCreation
{
static FString Json(const TSharedRef<FJsonObject>& Result)
{
 FString Out; FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out)); return Out;
}
static FString Failure(const FString& Error, const TCHAR* Code = TEXT("pose_search_creation_rejected"))
{
 TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
 Result->SetBoolField(TEXT("success"), false); Result->SetBoolField(TEXT("created"), false);
 Result->SetBoolField(TEXT("ready"), false); Result->SetBoolField(TEXT("savedToDisk"), false);
 Result->SetStringField(TEXT("error"), Error); Result->SetStringField(TEXT("code"), Code); return Json(Result);
}
static bool DestinationAvailable(const FString& Name, const FString& Path)
{
 const FString PackagePath = Path / Name;
 const FString ObjectPath = PackagePath + TEXT(".") + Name;
 IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
 return !StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath)
  && !Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true).IsValid() && !FPackageName::DoesPackageExist(PackagePath);
}
static bool ReadRequest(const FString& Input, const TCHAR* TargetKey, FString& Name, FString& Path, FString& Target, FString& Error)
{
 if (!IsInGameThread() || !GEditor || GEditor->PlayWorld || GIsTransacting || GEditor->IsTransactionActive())
 { Error = TEXT("PoseSearch creation requires an idle editor game thread outside PIE and active transactions."); return false; }
 TSharedPtr<FJsonObject> Cmd;
 if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Input), Cmd) || !Cmd.IsValid())
 { Error = TEXT("Invalid JSON object."); return false; }
 if (!Cmd->HasTypedField<EJson::String>(TEXT("name")) || !Cmd->TryGetStringField(TEXT("name"), Name)
  || Name.IsEmpty() || Name.Len() >= NAME_SIZE || Name.TrimStartAndEnd() != Name || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)
  || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
 { Error = TEXT("name must be a valid nonempty Unreal asset name."); return false; }
 Path = TEXT("/Game/Animation/MotionMatching");
 if (Cmd->HasField(TEXT("path")) && (!Cmd->HasTypedField<EJson::String>(TEXT("path")) || !Cmd->TryGetStringField(TEXT("path"), Path)))
 { Error = TEXT("path must be a content folder string."); return false; }
 if (Path.IsEmpty()) Path = TEXT("/Game/Animation/MotionMatching");
 if ((Path / Name).Len() >= NAME_SIZE || !FPackageName::IsValidLongPackageName(Path / Name))
 { Error = TEXT("Invalid PoseSearch destination package."); return false; }
 if (!Cmd->HasTypedField<EJson::String>(TargetKey) || !Cmd->TryGetStringField(TargetKey, Target) || Target.IsEmpty())
 { Error = FString(TargetKey) + TEXT(" must identify an exact existing asset."); return false; }
 if (!DestinationAvailable(Name, Path))
 { Error = TEXT("PoseSearch destination already exists; existing asset was not changed."); return false; }
 return true;
}
static UObject* ExactAsset(const FString& Input, UClass* ExpectedClass, FString& Error)
{
 FString ObjectPath;
 if (!NwiroBlueprintPreload::NormalizeExactAssetPath(Input, ObjectPath))
 { Error = TEXT("Expected an exact mounted asset package or object path."); return nullptr; }
 if (UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
 {
  if (IsValid(Existing) && Existing->IsA(ExpectedClass) && !Existing->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed)) return Existing;
  Error = TEXT("Existing input has the wrong asset type or is not ready."); return nullptr;
 }
 IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
 const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true);
 if (!Asset.IsValid() || !Asset.IsInstanceOf(ExpectedClass))
 { Error = TEXT("No exact existing asset of the required type: ") + ExpectedClass->GetName() + TEXT(" at ") + ObjectPath; return nullptr; }
 UObject* Loaded = NwiroSafeExactAssetLoad(ObjectPath, &Error);
 if (!IsValid(Loaded) || !Loaded->IsA(ExpectedClass))
 { if (Error.IsEmpty()) Error = TEXT("Could not load the exact asset with the required type."); return nullptr; }
 return Loaded;
}
static bool SchemaReady(const UPoseSearchSchema* Schema)
{
 return Schema && !Schema->GetRoledSkeletons().IsEmpty() && !Schema->GetChannels().IsEmpty() && Schema->SchemaCardinality > 0;
}
static TSharedRef<FJsonObject> Created(UObject* Asset)
{
 TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
 Result->SetBoolField(TEXT("success"), true); Result->SetBoolField(TEXT("created"), true);
 Result->SetStringField(TEXT("name"), Asset->GetName()); Result->SetStringField(TEXT("path"), Asset->GetPathName());
 Result->SetBoolField(TEXT("savedToDisk"), false); Result->SetBoolField(TEXT("dirtyAfter"), Asset->GetOutermost()->IsDirty()); return Result;
}
}

FString FNwiroIKTools::CreatePoseSearchSchema(const FString& JsonCommand)
{
 using namespace NwiroPoseSearchCreation;
 FString Name, Path, SkeletonPath, Error;
 if (!ReadRequest(JsonCommand, TEXT("skeleton"), Name, Path, SkeletonPath, Error)) return Failure(Error);
 TStrongObjectPtr<USkeleton> Skeleton(Cast<USkeleton>(ExactAsset(SkeletonPath, USkeleton::StaticClass(), Error)));
 if (!Skeleton.IsValid()) return Failure(Error);
 TStrongObjectPtr<UPoseSearchSchemaFactory> Factory(NewObject<UPoseSearchSchemaFactory>());
 Factory->TargetSkeleton = Skeleton.Get();
 // Run the real engine factory/default-channel validation in unpublished storage.
 // No assumed humanoid bone names or private channel access determines readiness.
 TStrongObjectPtr<UPoseSearchSchema> Staged(Cast<UPoseSearchSchema>(Factory->FactoryCreateNew(UPoseSearchSchema::StaticClass(),
  GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPoseSearchSchema::StaticClass(), TEXT("NwiroPoseSearchValidation")), RF_Transient, nullptr, GWarn)));
 if (!SchemaReady(Staged.Get()) || Staged->GetSkeleton(Staged->GetDefaultRole()) != Skeleton.Get())
  return Failure(TEXT("The engine's default PoseSearch channels could not finalize for the requested skeleton; no destination asset was created. See PoseSearch diagnostics for the incompatible channel/bone."), TEXT("pose_search_defaults_incompatible"));
 // Recheck after the engine factory callbacks before standard asset publication.
 if (!DestinationAvailable(Name, Path)) return Failure(TEXT("PoseSearch destination became occupied during validation; no asset was replaced."));
 IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
 UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(AssetTools.CreateAsset(Name, Path, UPoseSearchSchema::StaticClass(), Factory.Get()));
 if (!Schema) return Failure(TEXT("Failed to create PoseSearchSchema after default-channel validation."));
 Schema->MarkPackageDirty();
 const bool Ready = SchemaReady(Schema) && Schema->GetSkeleton(Schema->GetDefaultRole()) == Skeleton.Get();
 const auto Result = Created(Schema);
 Result->SetBoolField(TEXT("success"), Ready); Result->SetBoolField(TEXT("ready"), Ready);
 Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
 Result->SetNumberField(TEXT("channelCount"), Schema->GetChannels().Num());
 Result->SetNumberField(TEXT("schemaCardinality"), Schema->SchemaCardinality);
 if (!Ready) Result->SetStringField(TEXT("error"), TEXT("The published schema unexpectedly failed readiness verification; created:true and path identify the asset requiring inspection."));
 return Json(Result);
}

// ============================================================
// CREATE POSE SEARCH DATABASE
// ============================================================

FString FNwiroIKTools::CreatePoseSearchDatabase(const FString& JsonCommand)
{
 using namespace NwiroPoseSearchCreation;
 FString Name, Path, SchemaPath, Error;
 if (!ReadRequest(JsonCommand, TEXT("schema"), Name, Path, SchemaPath, Error)) return Failure(Error);
 TStrongObjectPtr<UPoseSearchSchema> Schema(Cast<UPoseSearchSchema>(ExactAsset(SchemaPath, UPoseSearchSchema::StaticClass(), Error)));
 if (!Schema.IsValid()) return Failure(Error);
 if (!SchemaReady(Schema.Get())) return Failure(TEXT("The supplied PoseSearch schema has no finalized usable channels; database creation was not attempted."), TEXT("pose_search_schema_not_ready"));
 IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
 TStrongObjectPtr<UPoseSearchDatabaseFactory> Factory(NewObject<UPoseSearchDatabaseFactory>());
 Factory->TargetSchema = Schema.Get();
 UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(AssetTools.CreateAsset(Name, Path, UPoseSearchDatabase::StaticClass(), Factory.Get()));
 if (!DB) return Failure(TEXT("Failed to create PoseSearchDatabase."));
 DB->MarkPackageDirty();
 const auto Result = Created(DB);
 const bool Configured = DB->Schema == Schema.Get();
 Result->SetBoolField(TEXT("success"), Configured); Result->SetBoolField(TEXT("schemaReady"), Configured && SchemaReady(Schema.Get()));
 Result->SetStringField(TEXT("schema"), DB->Schema ? DB->Schema->GetPathName() : TEXT("None"));
 if (!Configured) Result->SetStringField(TEXT("error"), TEXT("The published database did not retain its target schema; created:true and path identify the asset requiring inspection."));
 return Json(Result);
}

// ============================================================
// READ POSE SEARCH DATABASE
// ============================================================

FString FNwiroIKTools::ReadPoseSearchDatabase(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(Asset);

	if (!DB)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PoseSearchDatabase not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), DB->GetName());
	Result->SetStringField(TEXT("path"), DB->GetPathName());
	Result->SetStringField(TEXT("schema"), DB->Schema ? DB->Schema->GetPathName() : TEXT("None"));

	// Animation assets in database
	TArray<TSharedPtr<FJsonValue>> Anims;
	const int32 NumAnims = DB->GetNumAnimationAssets();
	for (int32 i = 0; i < NumAnims; ++i)
	{
		const FPoseSearchDatabaseAnimationAsset* AnimAsset = DB->GetDatabaseAnimationAsset(i);
		if (!AnimAsset) continue;
		TSharedRef<FJsonObject> A = MakeShareable(new FJsonObject());
		UObject* AnimObj = AnimAsset->AnimAsset.Get();
		A->SetStringField(TEXT("asset"), AnimObj ? AnimObj->GetPathName() : TEXT("None"));
		Anims.Add(MakeShareable(new FJsonValueObject(A)));
	}
	Result->SetArrayField(TEXT("animations"), Anims);
	Result->SetNumberField(TEXT("animationCount"), Anims.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD IK SOLVER
// ============================================================

FString FNwiroIKTools::AddIKSolver(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RigPath = Cmd->GetStringField(TEXT("rig"));
	FString SolverType = Cmd->GetStringField(TEXT("type"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RigPath);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);
	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *RigPath);

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Rig controller unavailable\"}");
	UIKRigController& Controller = *ControllerPtr;

	// UE 5.7+ stores solvers as script structs. Legacy UObject solver classes
	// remain loaded for migration, but cannot be passed to the current controller.
	const TMap<FString, FString> Aliases = {
		{TEXT("fbiksolver"), TEXT("IKRigFullBodyIKSolver")},
		{TEXT("limbsolver"), TEXT("IKRigLimbSolver")},
		{TEXT("settransform"), TEXT("IKRigSetTransform")},
		{TEXT("polesolver"), TEXT("IKRigPoleSolver")}
	};
	const FString* Alias = Aliases.Find(SolverType.ToLower());
	FString Error;
	UScriptStruct* SolverStruct = Cast<UScriptStruct>(NwiroTypeValidation::ResolveLoadedType(
		Alias ? *Alias : SolverType, UScriptStruct::StaticClass(), Error));
	if (!SolverStruct || SolverStruct == FIKRigSolverBase::StaticStruct() || !SolverStruct->IsChildOf(FIKRigSolverBase::StaticStruct()))
	{
		TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetBoolField(TEXT("success"), false);
		Failure->SetStringField(TEXT("error"), TEXT("Invalid IK solver type. Use FBIKSolver, LimbSolver, SetTransform, PoleSolver or an exact loaded FIKRigSolverBase-derived struct path. ") + Error);
		FString Out; FJsonSerializer::Serialize(Failure, TJsonWriterFactory<>::Create(&Out)); return Out;
	}
	const int32 Before = Controller.GetNumSolvers();
	const int32 SolverIndex = Controller.AddSolver(SolverStruct);
	if (SolverIndex == INDEX_NONE || Controller.GetNumSolvers() != Before + 1 || !Controller.GetSolverAtIndex(SolverIndex))
		return TEXT("{\"success\":false,\"error\":\"IK controller did not add the requested solver\"}");
	return FString::Printf(TEXT("{\"success\":true,\"solverType\":\"%s\",\"solverIndex\":%d,\"solverCount\":%d}"),
		*SolverStruct->GetName(), SolverIndex, Controller.GetNumSolvers());
}

// ============================================================
// ADD RETARGET CHAIN
// ============================================================

FString FNwiroIKTools::AddRetargetChain(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RigPath = Cmd->GetStringField(TEXT("rig"));
	FString ChainName = Cmd->GetStringField(TEXT("name"));
	FString StartBone = Cmd->GetStringField(TEXT("startBone"));
	FString EndBone = Cmd->GetStringField(TEXT("endBone"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RigPath);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);
	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *RigPath);

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Rig controller unavailable\"}");
	UIKRigController& Controller = *ControllerPtr;
	FName ChainResult = Controller.AddRetargetChain(FName(*ChainName), FName(*StartBone), FName(*EndBone), NAME_None);

	if (ChainResult == NAME_None)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to add retarget chain '%s' (%s -> %s)\"}"), *ChainName, *StartBone, *EndBone);

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"chain\":\"%s\",\"startBone\":\"%s\",\"endBone\":\"%s\"}"),
		*ChainName, *StartBone, *EndBone);
}

// ============================================================
// SET CHAIN MAPPING (IK Retargeter)
// ============================================================

FString FNwiroIKTools::SetChainMapping(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RetargetPath = Cmd->GetStringField(TEXT("retargeter"));
	FString SourceChain = Cmd->GetStringField(TEXT("sourceChain"));
	FString TargetChain = Cmd->GetStringField(TEXT("targetChain"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RetargetPath);
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Asset);
	if (!Retargeter)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRetargeter not found: %s\"}"), *RetargetPath);

	UIKRetargeterController* ControllerPtr = UIKRetargeterController::GetController(Retargeter);
	if (!ControllerPtr) return TEXT("{\"success\":false,\"error\":\"IK Retargeter controller unavailable\"}");
	UIKRetargeterController& Controller = *ControllerPtr;
	if (SourceChain.IsEmpty() || TargetChain.IsEmpty() || SourceChain.Len() >= NAME_SIZE || TargetChain.Len() >= NAME_SIZE)
		return TEXT("{\"success\":false,\"error\":\"sourceChain and targetChain must be nonempty chain names\"}");
	const FName SourceName(*SourceChain), TargetName(*TargetChain);
	if (TargetName.IsNone()) return TEXT("{\"success\":false,\"error\":\"targetChain cannot be None\"}");
	TArray<FName> MatchingOps;
	bool bChanged = false;
	// Validate every affected operation before the controller starts its transaction.
	for (int32 OpIndex = 0; OpIndex < Controller.GetNumRetargetOps(); ++OpIndex)
	{
		const FName OpName = Controller.GetOpName(OpIndex);
		const FRetargetChainMapping* Mapping = Controller.GetChainMapping(OpName);
		if (!Mapping || !Mapping->HasChain(TargetName, ERetargetSourceOrTarget::Target)) continue;
		if (!SourceName.IsNone() && !Mapping->HasChain(SourceName, ERetargetSourceOrTarget::Source))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Source chain is absent from operation %s; no mappings changed\"}"), *OpName.ToString().ReplaceCharWithEscapedChar());
		MatchingOps.Add(OpName);
		bChanged |= Mapping->GetChainMappedTo(TargetName, ERetargetSourceOrTarget::Target) != SourceName;
	}
	if (MatchingOps.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Target chain has no mapping in this retargeter; configure source/target rigs and a retarget operation first\"}");
	if (bChanged && !Controller.SetSourceChain(SourceName, TargetName, NAME_None))
		return TEXT("{\"success\":false,\"error\":\"IK controller rejected the chain mapping\"}");
	for (FName OpName : MatchingOps)
	{
		const FRetargetChainMapping* Mapping = Controller.GetChainMapping(OpName);
		if (!Mapping || Mapping->GetChainMappedTo(TargetName, ERetargetSourceOrTarget::Target) != SourceName)
			return TEXT("{\"success\":false,\"error\":\"Chain mapping postcondition failed; inspect the retargeter before retrying\"}");
	}
	return FString::Printf(TEXT("{\"success\":true,\"changed\":%s,\"operationCount\":%d,\"sourceChain\":\"%s\",\"targetChain\":\"%s\"}"),
		bChanged ? TEXT("true") : TEXT("false"), MatchingOps.Num(), *SourceChain.ReplaceCharWithEscapedChar(), *TargetChain.ReplaceCharWithEscapedChar());
}

// ============================================================
// ADD POSE SEARCH ANIMATION
// ============================================================

FString FNwiroIKTools::AddPoseSearchAnimation(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString DBPath = Cmd->GetStringField(TEXT("database"));
	FString AnimPath = Cmd->GetStringField(TEXT("animation"));

	UObject* DBAsset = UEditorAssetLibrary::LoadAsset(DBPath);
	UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(DBAsset);
	if (!DB)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PoseSearchDatabase not found: %s\"}"), *DBPath);

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(UEditorAssetLibrary::LoadAsset(AnimPath));
	if (!AnimSeq)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Animation not found: %s\"}"), *AnimPath);

	// Add animation via the database's public API
	FPoseSearchDatabaseAnimationAsset NewAsset;
	NewAsset.AnimAsset = AnimSeq;
	DB->AddAnimationAsset(NewAsset);

	DB->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"database\":\"%s\",\"animation\":\"%s\"}"),
		*DB->GetName(), *AnimSeq->GetName());
}

#endif // NWIRO_HAS_IK_TOOLS
