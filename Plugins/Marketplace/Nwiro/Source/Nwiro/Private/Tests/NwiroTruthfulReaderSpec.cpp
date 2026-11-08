// Copyright 2026 Nwiro. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "NwiroSequencerTools.h"
#include "NwiroGASTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Engine/World.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/StrongObjectPtr.h"
#include "Json.h"

namespace NwiroReaderProof
{
static TSharedPtr<FJsonObject> Call(FAutomationTestBase& Test, FString (*Tool)(const FString&), const TCHAR* Key, const FString& Target)
{
	TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>(); Command->SetStringField(Key, Target);
	FString Json; FJsonSerializer::Serialize(Command, TJsonWriterFactory<>::Create(&Json));
	const FString Raw = Tool(Json); TSharedPtr<FJsonObject> Result;
	Test.TestTrue(TEXT("reader response parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result) && Result.IsValid());
	return Result;
}
static void Cleanup(UPackage* Package)
{
	TArray<UObject*> Objects; GetObjectsWithOuter(Package, Objects);
	for (UObject* Object : Objects)
	{
		if (Object->IsAsset()) FAssetRegistryModule::AssetDeleted(Object);
		Object->ClearFlags(RF_Public | RF_Standalone); Object->MarkAsGarbage();
	}
	Package->SetDirtyFlag(false); Package->MarkAsGarbage();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSequenceReaderSpec,
	"Nwiro.Pro.Sequencer.ReadSequence.BindingTracks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroSequenceReaderSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroReaderProof;
	if (!TestTrue(TEXT("idle editor available"), GEditor && !GEditor->PlayWorld && !GEditor->IsTransactionActive())) return false;
	UPackage* Package = CreatePackage(*(TEXT("/Game/NwiroSequenceRead_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<ULevelSequence> Sequence(NewObject<ULevelSequence>(Package, TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
	FAssetRegistryModule::AssetCreated(Sequence.Get());
	ON_SCOPE_EXIT { Cleanup(Package); };
	Sequence->Initialize(); UMovieScene* Scene = Sequence->GetMovieScene();
	if (!TestNotNull(TEXT("MovieScene fixture created"), Scene)) return false;
	Scene->SetDisplayRate(FFrameRate(30000, 1001));
	const FGuid Bound = Scene->AddPossessable(TEXT("BoundActor"), AActor::StaticClass());
	const FGuid Empty = Scene->AddPossessable(TEXT("EmptyActor"), AActor::StaticClass());
	// Spawnable templates are concrete actors owned by the MovieScene, as in
	// MovieSceneHelpers::MakeSpawnableTemplateFromInstance. UObject is abstract.
	AActor* Template = NewObject<AActor>(Scene, TEXT("SpawnableTemplate"), RF_Transactional);
	if (!TestNotNull(TEXT("concrete spawnable actor template created"), Template)) return false;
	TestTrue(TEXT("spawnable template belongs to the owned MovieScene"), Template->GetOuter() == Scene);
	const FGuid Spawned = Scene->AddSpawnable(TEXT("SpawnedObject"), *Template);
	UMovieScene3DTransformTrack* Transform = Scene->AddTrack<UMovieScene3DTransformTrack>(Bound);
	if (!TestNotNull(TEXT("bound transform created"), Transform)) return false;
	UMovieSceneSection* Section = Transform->CreateNewSection(); Transform->AddSection(*Section);
	UMovieSceneAudioTrack* Master = Scene->AddTrack<UMovieSceneAudioTrack>();
	UMovieSceneTrack* Camera = Scene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
	const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
	for (bool WasDirty : { false, true })
	{
		Package->SetDirtyFlag(WasDirty);
		const auto Result = Call(*this, FNwiroSequencerTools::ReadSequence, TEXT("path"), Sequence->GetPathName());
		if (!Result || !TestTrue(TEXT("sequence query succeeds"), Result->GetBoolField(TEXT("success")))) continue;
		TestEqual(TEXT("fractional fps is actual ratio"), Result->GetNumberField(TEXT("fps")), 30000.0 / 1001.0);
		TestEqual(TEXT("denominator is retained"), Result->GetObjectField(TEXT("displayRate"))->GetNumberField(TEXT("denominator")), 1001.0);
		const auto& Bindings = Result->GetArrayField(TEXT("bindings"));
		TestEqual(TEXT("all possessable and spawnable bindings returned"), Bindings.Num(), 3);
		bool FoundBound = false, FoundEmpty = false, FoundSpawned = false;
		for (const auto& Value : Bindings)
		{
			const auto Binding = Value->AsObject(); const FString Guid = Binding->GetStringField(TEXT("guid"));
			if (Guid == Bound.ToString())
			{
				FoundBound = true;
				const auto& Tracks = Binding->GetArrayField(TEXT("tracks"));
				if (TestEqual(TEXT("binding reports actual track"), Tracks.Num(), 1))
				{
					TestEqual(TEXT("track identity matches native object"), Tracks[0]->AsObject()->GetStringField(TEXT("path")), Transform->GetPathName());
					TestEqual(TEXT("section count matches actual track"), Tracks[0]->AsObject()->GetNumberField(TEXT("sectionCount")), 1.0);
				}
			}
			if (Guid == Empty.ToString()) { FoundEmpty = true; TestEqual(TEXT("empty binding remains explicitly empty"), Binding->GetArrayField(TEXT("tracks")).Num(), 0); }
			if (Guid == Spawned.ToString()) { FoundSpawned = true; TestEqual(TEXT("spawnable is classified"), Binding->GetStringField(TEXT("kind")), FString(TEXT("spawnable"))); }
		}
		TestTrue(TEXT("all exact binding GUIDs found"), FoundBound && FoundEmpty && FoundSpawned);
		const auto& Tracks = Result->GetArrayField(TEXT("masterTracks"));
		if (Master && TestEqual(TEXT("master track retained separately"), Tracks.Num(), 1)) TestEqual(TEXT("master identity"), Tracks[0]->AsObject()->GetStringField(TEXT("path")), Master->GetPathName());
		if (Camera) TestEqual(TEXT("camera cut track retained"), Result->GetObjectField(TEXT("cameraCutTrack"))->GetStringField(TEXT("path")), Camera->GetPathName());
		TestEqual(TEXT("reader preserves dirty state"), Package->IsDirty(), WasDirty);
		TestEqual(TEXT("reader adds no undo step"), GEditor->Trans->GetUndoContext().TransactionId, UndoBefore);
	}
	const auto EmptyResult = Call(*this, FNwiroSequencerTools::ReadSequence, TEXT("path"), TEXT(""));
	if (EmptyResult) TestFalse(TEXT("empty selector never picks first sequence"), EmptyResult->GetBoolField(TEXT("success")));
	const auto Partial = Call(*this, FNwiroSequencerTools::ReadSequence, TEXT("path"), TEXT("Sequenc"));
	if (Partial) TestFalse(TEXT("substring selector never picks an unrelated sequence"), Partial->GetBoolField(TEXT("success")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroGASReaderSpec,
	"Nwiro.Pro.GAS.ReadInfo.ActorAndDeclaration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroGASReaderSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroReaderProof;
	if (!TestTrue(TEXT("idle editor available"), GEditor && !GEditor->PlayWorld && !GEditor->IsTransactionActive())) return false;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("editor world available"), World)) return false;
	const bool WorldWasDirty = World->GetOutermost()->IsDirty();
	FActorSpawnParameters Spawn; Spawn.Name = FName(*(TEXT("NwiroGASReader_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); Spawn.ObjectFlags = RF_Transient;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("owned actor fixture created"), Actor)) return false;
	ON_SCOPE_EXIT { World->DestroyActor(Actor); World->GetOutermost()->SetDirtyFlag(WorldWasDirty); };
	World->GetOutermost()->SetDirtyFlag(WorldWasDirty);
	const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
	const auto Live = Call(*this, FNwiroGASTools::GetGASInfo, TEXT("actor"), Actor->GetPathName());
	if (Live && TestTrue(TEXT("actual actor resolves without asset load"), Live->GetBoolField(TEXT("success"))))
	{
		TestEqual(TEXT("readback uses exact actor"), Live->GetStringField(TEXT("path")), Actor->GetPathName());
		TestEqual(TEXT("actor scope is explicit"), Live->GetStringField(TEXT("scope")), FString(TEXT("editor_actor")));
		TestEqual(TEXT("reader preserves world dirty state"), World->GetOutermost()->IsDirty(), WorldWasDirty);
		TestEqual(TEXT("reader adds no undo step"), GEditor->Trans->GetUndoContext().TransactionId, UndoBefore);
	}
	const auto Missing = Call(*this, FNwiroGASTools::GetGASInfo, TEXT("actor"), Actor->GetName() + TEXT("_missing"));
	if (Missing) TestFalse(TEXT("missing exact actor is rejected"), Missing->GetBoolField(TEXT("success")));
	UClass* ASC = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.AbilitySystemComponent"));
	if (!ASC) { AddInfo(TEXT("Optional GAS classes are not loaded: actor resolution tested; GAS declaration/populated-component verification requires the GAS-enabled MCP fixture.")); return true; }
	// Query a real owned ASC through the production reader. Its public per-class
	// lookup cannot prove an exhaustive inventory (distinct same-class instances
	// are legal), even when this particular fixture has no attribute instances.
	UActorComponent* RuntimeASC = NewObject<UActorComponent>(Actor, ASC, TEXT("ObservedAbilitySystem"), RF_Transient);
	if (!TestNotNull(TEXT("owned native ASC created"), RuntimeASC)) return false;
	Actor->AddInstanceComponent(RuntimeASC);
	World->GetOutermost()->SetDirtyFlag(WorldWasDirty);
	const int32 ComponentsBefore = Actor->GetComponents().Num();
	const FGuid ASCUndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
	const auto Observed = Call(*this, FNwiroGASTools::GetGASInfo, TEXT("actor"), Actor->GetPathName());
	if (!Observed || !TestTrue(TEXT("actual ASC query succeeds"), Observed->GetBoolField(TEXT("success")))) return false;
	const TArray<TSharedPtr<FJsonValue>>* ActualComponents = nullptr;
	if (!TestTrue(TEXT("reader exposes actual component inventory"), Observed->TryGetArrayField(TEXT("abilitySystemComponents"), ActualComponents) && ActualComponents) ||
		!TestEqual(TEXT("one actual ASC reported"), ActualComponents->Num(), 1)) return false;
	const TSharedPtr<FJsonObject> Details = (*ActualComponents)[0]->AsObject();
	if (!TestTrue(TEXT("component result is an object"), Details.IsValid())) return false;
	TestEqual(TEXT("reader names exact runtime component"), Details->GetStringField(TEXT("path")), RuntimeASC->GetPathName());
	bool InventoryComplete = true, Complete = true;
	TestTrue(TEXT("attribute inventory completeness is explicit"), Details->TryGetBoolField(TEXT("attributeSetInventoryComplete"), InventoryComplete));
	TestFalse(TEXT("per-class lookup is not claimed exhaustive"), InventoryComplete);
	TestTrue(TEXT("component completeness is explicit"), Details->TryGetBoolField(TEXT("complete"), Complete));
	TestFalse(TEXT("component cannot claim completeness from class lookup"), Complete);
	const TSharedPtr<FJsonObject>* Capabilities = nullptr;
	if (!TestTrue(TEXT("component capabilities are explicit"), Details->TryGetObjectField(TEXT("capabilities"), Capabilities) && Capabilities && Capabilities->IsValid())) return false;
	bool HasAttributes = false;
	TestTrue(TEXT("attribute lookup capability is explicit"), (*Capabilities)->TryGetBoolField(TEXT("attributeSets"), HasAttributes));
	TestEqual(TEXT("lookup scope matches capability"), Details->GetStringField(TEXT("attributeSetLookupScope")),
		FString(HasAttributes ? TEXT("first_matching_instance_per_loaded_class") : TEXT("unavailable")));
	TestEqual(TEXT("observations retained only when getter is available"), Details->HasTypedField<EJson::Array>(TEXT("attributeSets")), HasAttributes);
	TestTrue(TEXT("incomplete result explains its limit"), !Details->GetStringField(TEXT("incompleteReason")).IsEmpty());
	TestEqual(TEXT("GAS observation preserves owned components"), Actor->GetComponents().Num(), ComponentsBefore);
	TestEqual(TEXT("GAS observation preserves world dirty state"), World->GetOutermost()->IsDirty(), WorldWasDirty);
	TestEqual(TEXT("GAS observation creates no undo step"), GEditor->Trans->GetUndoContext().TransactionId, ASCUndoBefore);
	UPackage* Package = CreatePackage(*(TEXT("/Game/NwiroGASDeclaration_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { Cleanup(Package); };
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("BP_DeclaredASC"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("owned declaration Blueprint created"), Blueprint) || !TestNotNull(TEXT("SCS available"), Blueprint->SimpleConstructionScript.Get())) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ASC, TEXT("DeclaredAbilitySystem"));
	Blueprint->SimpleConstructionScript->AddNode(Node);
	Blueprint->Status = BS_Dirty; // Fixture is deliberately uncompiled; reader must preserve this.
	const EBlueprintStatus StatusBefore = Blueprint->Status;
	const UObject* CDOBefore = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
	Package->SetDirtyFlag(false);
	const auto Declared = Call(*this, FNwiroGASTools::GetGASInfo, TEXT("path"), Blueprint->GetPathName());
	if (Declared && TestTrue(TEXT("declaration query succeeds"), Declared->GetBoolField(TEXT("success"))))
	{
		TestFalse(TEXT("dirty Blueprint defaults are not presented as current"), Declared->GetBoolField(TEXT("defaultObjectAvailable")));
		TestTrue(TEXT("uninstantiated ASC declaration is reported"), Declared->GetBoolField(TEXT("hasDeclaredAbilitySystem")));
		TestEqual(TEXT("one declared template reported"), Declared->GetArrayField(TEXT("declaredAbilitySystemComponents")).Num(), 1);
		TestEqual(TEXT("no live/default components are invented"), Declared->GetArrayField(TEXT("abilitySystemComponents")).Num(), 0);
		TestEqual(TEXT("reader does not compile Blueprint"), Blueprint->Status, StatusBefore);
		TestTrue(TEXT("reader does not replace/create target CDO"), !Blueprint->GeneratedClass || Blueprint->GeneratedClass->GetDefaultObject(false) == CDOBefore);
		TestFalse(TEXT("reader leaves declaration package clean"), Package->IsDirty());
	}
	return true;
}
#endif
