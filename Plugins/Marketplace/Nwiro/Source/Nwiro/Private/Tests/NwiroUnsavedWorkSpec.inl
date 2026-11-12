// Copyright 2026 Nwiro. All Rights Reserved.
//
// Included at file scope from the product's SettingsTools.cpp under WITH_DEV_AUTOMATION_TESTS.
//
// B1 Layer 0 regression net for the editor-world mutating tools. Before the fix those tools
// mutated the level with neither a transaction nor a dirty flag: the edit was invisible to
// Unreal's unsaved-work check, so the next level switch threw it away with no prompt, and
// Ctrl+Z popped the USER's last transaction instead of the agent's edit.
//
// Every case proves both halves of the repaired contract, and every one of them fails on the
// old behaviour:
//   1. the edited ACTOR's own package is dirty afterwards. GetOutermost() follows the One File
//      Per Actor package, so this is correct on a classic .umap and on a World Partition map
//      alike - and it is the ACTOR's package that is asserted, never merely the world's;
//   2. the call left one undo step carrying the tool's own transaction title, and
//      GEditor->UndoTransaction() puts the previous state back.
//
// Assertion 1 is the discriminating one on a classic .umap, where the old code dirtied nothing
// at all. On a World Partition map a spawn already dirtied the new actor's external package by
// itself, so there assertion 2 - the titled transaction - is what separates old from new. Both
// are asserted everywhere so the pair is decisive on either map layout.
//
// The tools all resolve GEditor->GetEditorWorldContext().World(), so the fixtures live in the
// test host's own editor world. FWorldGuard puts that world back: every actor a case created is
// destroyed and every dirty flag a case raised is cleared again, so a passing run never leaves
// the host map marked as unsaved work.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Json.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "PCGGraph.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "NwiroToolDispatch.h"

/** Each product reaches its own tool dispatcher through this local wrapper. In the Integration
 *  Kit the dispatcher's entry point is private and this struct is its friend. */
struct FNwiroUnsavedWorkDispatch
{
	static FString DispatchTool(const FString& Tool, const FString& Json)
	{ return FNwiroToolDispatch::DispatchTool(Tool, Json); }
};

namespace NwiroUnsavedWorkSpec
{

/** The one world every mutating tool resolves, through GEditor->GetEditorWorldContext(). */
static UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

/** The transaction currently on top of the undo stack, or an invalid id when there is none. */
static FGuid UndoId()
{
	return (GEditor && GEditor->Trans) ? GEditor->Trans->GetUndoContext().TransactionId : FGuid();
}

/**
 * Every fixture label is unique. The tools resolve actors with a four-tier fuzzy match whose
 * last tier is a substring test, so two fixtures sharing a prefix would let a tool edit the
 * wrong actor and still report success.
 */
static FString UniqueLabel(const TCHAR* Prefix)
{
	return FString::Printf(TEXT("NwUnsaved_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

static AActor* FindActor(const FString& Label)
{
	UWorld* World = EditorWorld();
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label || (*It)->GetName() == Label) return *It;
	}
	return nullptr;
}

static TSet<FName> ActorNames()
{
	TSet<FName> Names;
	if (UWorld* World = EditorWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It) Names.Add((*It)->GetFName());
	}
	return Names;
}

/** Every actor in the editor world that is absent from the given snapshot. */
static TArray<AActor*> ActorsAddedSince(const TSet<FName>& Known)
{
	TArray<AActor*> Added;
	if (UWorld* World = EditorWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!Known.Contains((*It)->GetFName())) Added.Add(*It);
		}
	}
	return Added;
}

/**
 * Restores the test host's editor world. Records the actors present at construction; on scope
 * exit destroys everything that appeared since and clears the dirty flag of every package the
 * case touched. The destroys themselves dirty the map package, so the flags are cleared last.
 */
struct FWorldGuard
{
	TSet<FName> Baseline;
	TArray<TWeakObjectPtr<UPackage>> Touched;

	FWorldGuard() : Baseline(ActorNames())
	{
		if (UWorld* World = EditorWorld()) Remember(World);
	}

	void Remember(UObject* Object)
	{
		if (!Object) return;
		UPackage* Package = Object->GetOutermost();
		if (!Package) return;
		for (const TWeakObjectPtr<UPackage>& Known : Touched)
		{
			if (Known.Get() == Package) return;
		}
		Touched.Add(Package);
	}

	/**
	 * The clean baseline: clear the dirty flag of every package the call can reach, so the
	 * assertion that follows proves THIS call dirtied it rather than some earlier fixture step.
	 * On a World Partition map a spawned actor's external package does not exist yet, which is
	 * why the titled-transaction assertion carries the spawn cases there.
	 */
	void Clean(AActor* Actor = nullptr)
	{
		if (UWorld* World = EditorWorld())
		{
			Remember(World);
			World->GetOutermost()->SetDirtyFlag(false);
		}
		if (Actor)
		{
			Remember(Actor);
			Actor->GetOutermost()->SetDirtyFlag(false);
		}
	}

	~FWorldGuard()
	{
		if (UWorld* World = EditorWorld())
		{
			for (AActor* Stray : ActorsAddedSince(Baseline))
			{
				Remember(Stray);
				World->DestroyActor(Stray);
			}
			Remember(World);
		}
		for (const TWeakObjectPtr<UPackage>& Package : Touched)
		{
			if (Package.IsValid()) Package->SetDirtyFlag(false);
		}
	}
};

/** An in-memory asset fixture: registered so path lookups resolve it, torn down on scope exit. */
struct FAssetFixture
{
	UPackage* Package = nullptr;
	TStrongObjectPtr<UObject> Asset;

	FAssetFixture(UClass* Class, const TCHAR* Prefix)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/NwUnsaved_%s_%s"), Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Asset.Reset(NewObject<UObject>(Package, Class, FName(Prefix), RF_Public | RF_Standalone | RF_Transactional));
		if (Asset.IsValid()) FAssetRegistryModule::AssetCreated(Asset.Get());
	}

	FString Path() const { return Asset.IsValid() ? Asset->GetPathName() : FString(); }

	~FAssetFixture()
	{
		if (Asset.IsValid())
		{
			FAssetRegistryModule::AssetDeleted(Asset.Get());
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}
};

/** The shared precondition: a real editor world and an undo stack this test owns. */
static bool Ready(FAutomationTestBase& Test)
{
	if (!Test.TestTrue(TEXT("test owns an idle editor transaction"),
		GEditor && GEditor->Trans && !GEditor->PlayWorld && !GEditor->IsTransactionActive() && !GIsTransacting)) return false;
	return Test.TestNotNull(TEXT("test host has an editor world"), EditorWorld());
}

static AActor* SpawnFixture(FAutomationTestBase& Test, UClass* Class, const FString& Label)
{
	UWorld* World = EditorWorld();
	if (!World) return nullptr;
	const FVector Origin = FVector::ZeroVector;
	const FRotator NoRotation = FRotator::ZeroRotator;
	AActor* Actor = World->SpawnActor(Class, &Origin, &NoRotation);
	if (!Test.TestNotNull(*FString::Printf(TEXT("fixture actor '%s' spawns"), *Label), Actor)) return nullptr;
	Actor->SetActorLabel(Label);
	return Actor;
}

/** JSON string in, JSON string out - the dispatcher's own entry point, called as it calls it. */
static TSharedPtr<FJsonObject> Dispatch(FAutomationTestBase& Test, const TCHAR* Tool, const FString& Args)
{
	TSharedPtr<FJsonObject> Reply;
	const FString Raw = FNwiroUnsavedWorkDispatch::DispatchTool(Tool, Args);
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) || !Reply.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("%s did not return JSON: %s"), Tool, *Raw));
		return TSharedPtr<FJsonObject>();
	}
	bool bSuccess = false;
	Reply->TryGetBoolField(TEXT("success"), bSuccess);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s succeeds"), Tool), bSuccess))
	{
		Test.AddError(FString::Printf(TEXT("%s replied %s"), Tool, *Raw));
		return TSharedPtr<FJsonObject>();
	}
	// The reply's own receipt. It is the tool reporting what it observed; the package check at
	// the call site is the independent one, and both must agree.
	bool bDirtyAfter = false;
	Test.TestTrue(*FString::Printf(TEXT("%s reports dirtyAfter"), Tool),
		Reply->TryGetBoolField(TEXT("dirtyAfter"), bDirtyAfter) && bDirtyAfter);
	return Reply;
}

/** The ground truth: the touched actor's OWN package, not the world's, carries the unsaved work. */
static void ExpectActorDirty(FAutomationTestBase& Test, const TCHAR* Tool, AActor* Actor)
{
	if (!Actor) return;
	Test.TestTrue(*FString::Printf(TEXT("%s dirties the actor's own package"), Tool),
		Actor->GetOutermost()->IsDirty());
}

/**
 * The call must have left one undo step of its own, titled by the tool's transaction text. This
 * is what the old code never did: with no transaction open, GEditor->UndoTransaction() popped
 * whatever the user had done last.
 */
static bool OwnsUndoStep(FAutomationTestBase& Test, const TCHAR* Tool, const FText& Title, const FGuid& Before)
{
	const FTransactionContext Applied = GEditor->Trans->GetUndoContext();
	return Test.TestTrue(*FString::Printf(TEXT("%s opened its own transaction '%s'"), Tool, *Title.ToString()),
		Applied.TransactionId.IsValid() && Applied.TransactionId != Before && Applied.Title.EqualTo(Title));
}

/** OwnsUndoStep plus the undo itself; false when either half did not happen. */
static bool UndoOwnStep(FAutomationTestBase& Test, const TCHAR* Tool, const FText& Title, const FGuid& Before)
{
	if (!OwnsUndoStep(Test, Tool, Title, Before)) return false;
	return Test.TestTrue(*FString::Printf(TEXT("%s can be undone"), Tool), GEditor->UndoTransaction());
}

} // namespace NwiroUnsavedWorkSpec

// ============================================================
// transform_actor
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkTransformActorSpec,
	"Nwiro.UnsavedWork.TransformActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkTransformActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("Transform"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	const FVector Before = Actor->GetActorLocation();

	Guard.Clean(Actor);
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("transform_actor"),
		FString::Printf(TEXT("{\"name\":\"%s\",\"x\":100,\"y\":200,\"z\":300}"), *Label)).IsValid()) return false;

	TestEqual(TEXT("transform_actor moved the actor"), Actor->GetActorLocation(), FVector(100, 200, 300));
	ExpectActorDirty(*this, TEXT("transform_actor"), Actor);
	if (UndoOwnStep(*this, TEXT("transform_actor"), NSLOCTEXT("Nwiro", "TransformActor", "AI: Transform Actor"), UndoBefore))
	{
		TestEqual(TEXT("undo restores the position"), Actor->GetActorLocation(), Before);
	}
	return true;
}

// The pre-check we added. A call with no transform field mutates nothing, so it must be refused
// BEFORE the transaction opens: the helper's Modify() dirties the package even when the value it
// records is unchanged, which would put a clean level into the unsaved-work list for free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkTransformActorNoFieldSpec,
	"Nwiro.UnsavedWork.TransformActorWithoutFieldIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkTransformActorNoFieldSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("NoField"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	const FVector Before = Actor->GetActorLocation();

	// Both starting states: a refusal must neither raise the flag nor clear one it found raised.
	for (bool bWasDirty : { false, true })
	{
		Guard.Clean(Actor);
		Actor->GetOutermost()->SetDirtyFlag(bWasDirty);
		const FGuid UndoBefore = UndoId();

		TSharedPtr<FJsonObject> Reply;
		const FString Raw = FNwiroUnsavedWorkDispatch::DispatchTool(TEXT("transform_actor"),
			FString::Printf(TEXT("{\"name\":\"%s\"}"), *Label));
		if (!TestTrue(TEXT("the refusal is JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) && Reply.IsValid())) return false;

		bool bSuccess = true;
		Reply->TryGetBoolField(TEXT("success"), bSuccess);
		TestFalse(TEXT("a transform with no location, rotation or scale is refused"), bSuccess);
		FString Error;
		TestTrue(TEXT("the refusal names the fields it wanted"),
			Reply->TryGetStringField(TEXT("error"), Error) && !Error.IsEmpty());
		TestFalse(TEXT("a refusal claims no dirty state"), Reply->HasField(TEXT("dirtyAfter")));
		TestEqual(TEXT("a refusal leaves the package's dirty flag exactly as it found it"),
			Actor->GetOutermost()->IsDirty(), bWasDirty);
		TestEqual(TEXT("a refusal opens no transaction"), UndoId(), UndoBefore);
		TestEqual(TEXT("a refusal moves nothing"), Actor->GetActorLocation(), Before);
	}
	return true;
}

// ============================================================
// spawn_actor / duplicate_actor / attach_actor
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkSpawnActorSpec,
	"Nwiro.UnsavedWork.SpawnActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkSpawnActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("Spawn"));
	Guard.Clean();
	const TSet<FName> Known = ActorNames();
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class\":\"StaticMeshActor\",\"label\":\"%s\",\"x\":0,\"y\":0,\"z\":0}"), *Label)).IsValid()) return false;

	AActor* Spawned = FindActor(Label);
	if (!TestNotNull(TEXT("spawn_actor put the actor in the editor world"), Spawned)) return false;
	Guard.Remember(Spawned);
	ExpectActorDirty(*this, TEXT("spawn_actor"), Spawned);
	if (UndoOwnStep(*this, TEXT("spawn_actor"), NSLOCTEXT("Nwiro", "SpawnActor", "AI: Spawn Actor"), UndoBefore))
	{
		TestNull(TEXT("undo removes the spawned actor"), FindActor(Label));
		TestEqual(TEXT("undo leaves no other actor behind"), ActorsAddedSince(Known).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkDuplicateActorSpec,
	"Nwiro.UnsavedWork.DuplicateActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkDuplicateActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("DupSource"));
	if (!SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label)) return false;

	Guard.Clean();
	const TSet<FName> Known = ActorNames();
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("duplicate_actor"),
		FString::Printf(TEXT("{\"name\":\"%s\"}"), *Label)).IsValid()) return false;

	const TArray<AActor*> Added = ActorsAddedSince(Known);
	if (!TestEqual(TEXT("duplicate_actor adds exactly one actor"), Added.Num(), 1)) return false;
	Guard.Remember(Added[0]);
	ExpectActorDirty(*this, TEXT("duplicate_actor"), Added[0]);
	if (UndoOwnStep(*this, TEXT("duplicate_actor"), NSLOCTEXT("Nwiro", "DuplicateActor", "AI: Duplicate Actor"), UndoBefore))
	{
		TestEqual(TEXT("undo removes the duplicate"), ActorsAddedSince(Known).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkAttachActorSpec,
	"Nwiro.UnsavedWork.AttachActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkAttachActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString ChildLabel = UniqueLabel(TEXT("AttachChild"));
	const FString ParentLabel = UniqueLabel(TEXT("AttachParent"));
	AActor* Child = SpawnFixture(*this, AStaticMeshActor::StaticClass(), ChildLabel);
	AActor* Parent = SpawnFixture(*this, AStaticMeshActor::StaticClass(), ParentLabel);
	if (!Child || !Parent) return false;
	if (!TestNull(TEXT("the child starts unattached"), Child->GetAttachParentActor())) return false;

	Guard.Clean(Child);
	Guard.Clean(Parent);
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("attach_actor"),
		FString::Printf(TEXT("{\"child\":\"%s\",\"parent\":\"%s\"}"), *ChildLabel, *ParentLabel)).IsValid()) return false;

	TestTrue(TEXT("attach_actor attached the child"), Child->GetAttachParentActor() == Parent);
	// The attachment persists on the CHILD, so the child's package is the one that carries the
	// unsaved work. The parent joins the undo step through Modify(false) - engine parity with
	// UEditorEngine::ParentActors - and must not be dragged into the unsaved-work list with it.
	ExpectActorDirty(*this, TEXT("attach_actor"), Child);
	TestFalse(TEXT("attach_actor does not dirty the unchanged parent package"),
		Parent->GetOutermost() != Child->GetOutermost() && Parent->GetOutermost()->IsDirty());
	if (UndoOwnStep(*this, TEXT("attach_actor"), NSLOCTEXT("Nwiro", "AttachActor", "AI: Attach Actor"), UndoBefore))
	{
		TestNull(TEXT("undo restores the detached child"), Child->GetAttachParentActor());
	}
	return true;
}

// ============================================================
// apply_material
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkApplyMaterialSpec,
	"Nwiro.UnsavedWork.ApplyMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkApplyMaterialSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("Material"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	UStaticMeshComponent* Mesh = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
	if (!TestNotNull(TEXT("the fixture has a mesh component"), Mesh)) return false;
	UMaterialInterface* Before = Mesh->GetMaterial(0);

	Guard.Clean(Actor);
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("apply_material"),
		FString::Printf(TEXT("{\"actor\":\"%s\",\"materialPath\":\"/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial\"}"), *Label)).IsValid()) return false;

	TestNotNull(TEXT("apply_material assigned a material"), Mesh->GetMaterial(0));
	TestTrue(TEXT("apply_material changed the slot"), Mesh->GetMaterial(0) != Before);
	ExpectActorDirty(*this, TEXT("apply_material"), Actor);
	if (UndoOwnStep(*this, TEXT("apply_material"), NSLOCTEXT("Nwiro", "ApplyMaterial", "AI: Apply Material"), UndoBefore))
	{
		TestTrue(TEXT("undo restores the previous material"), Mesh->GetMaterial(0) == Before);
	}
	return true;
}

// ============================================================
// the six spline tools
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkSplineToolsSpec,
	"Nwiro.UnsavedWork.SplineTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkSplineToolsSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	// --- create_spline_actor, proven and then undone in full ---
	{
		const FString Label = UniqueLabel(TEXT("SplineNew"));
		Guard.Clean();
		const TSet<FName> Known = ActorNames();
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, TEXT("create_spline_actor"),
			FString::Printf(TEXT("{\"label\":\"%s\"}"), *Label)).IsValid()) return false;

		AActor* Created = FindActor(Label);
		if (!TestNotNull(TEXT("create_spline_actor put the actor in the editor world"), Created)) return false;
		Guard.Remember(Created);
		ExpectActorDirty(*this, TEXT("create_spline_actor"), Created);
		if (UndoOwnStep(*this, TEXT("create_spline_actor"), NSLOCTEXT("Nwiro", "CreateSplineActor", "AI: Create Spline Actor"), UndoBefore))
		{
			TestEqual(TEXT("undo removes the spline actor"), ActorsAddedSince(Known).Num(), 0);
		}
	}

	// --- the point tools, on a spline the same tool builds (default two points) ---
	const FString Label = UniqueLabel(TEXT("Spline"));
	if (!Dispatch(*this, TEXT("create_spline_actor"),
		FString::Printf(TEXT("{\"label\":\"%s\"}"), *Label)).IsValid()) return false;
	AActor* Actor = FindActor(Label);
	if (!TestNotNull(TEXT("the spline fixture exists"), Actor)) return false;
	USplineComponent* Spline = Actor->FindComponentByClass<USplineComponent>();
	if (!TestNotNull(TEXT("the spline fixture has a spline component"), Spline)) return false;
	if (!TestEqual(TEXT("the spline fixture starts with two points"), Spline->GetNumberOfSplinePoints(), 2)) return false;

	// add_spline_point
	{
		const int32 Before = Spline->GetNumberOfSplinePoints();
		Guard.Clean(Actor);
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, TEXT("add_spline_point"),
			FString::Printf(TEXT("{\"actor\":\"%s\",\"x\":900,\"y\":0,\"z\":0}"), *Label)).IsValid()) return false;
		TestEqual(TEXT("add_spline_point added a point"), Spline->GetNumberOfSplinePoints(), Before + 1);
		ExpectActorDirty(*this, TEXT("add_spline_point"), Actor);
		if (UndoOwnStep(*this, TEXT("add_spline_point"), NSLOCTEXT("Nwiro", "AddSplinePoint", "AI: Add Spline Point"), UndoBefore))
		{
			TestEqual(TEXT("undo restores the point count"), Spline->GetNumberOfSplinePoints(), Before);
		}
	}

	// set_spline_point
	{
		const FVector Before = Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
		Guard.Clean(Actor);
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, TEXT("set_spline_point"),
			FString::Printf(TEXT("{\"actor\":\"%s\",\"index\":0,\"x\":250,\"y\":350,\"z\":450}"), *Label)).IsValid()) return false;
		TestEqual(TEXT("set_spline_point moved the point"),
			Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World), FVector(250, 350, 450));
		ExpectActorDirty(*this, TEXT("set_spline_point"), Actor);
		if (UndoOwnStep(*this, TEXT("set_spline_point"), NSLOCTEXT("Nwiro", "SetSplinePoint", "AI: Set Spline Point"), UndoBefore))
		{
			TestEqual(TEXT("undo restores the point location"),
				Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World), Before);
		}
	}

	// remove_spline_point
	{
		const int32 Before = Spline->GetNumberOfSplinePoints();
		Guard.Clean(Actor);
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, TEXT("remove_spline_point"),
			FString::Printf(TEXT("{\"actor\":\"%s\",\"index\":1}"), *Label)).IsValid()) return false;
		TestEqual(TEXT("remove_spline_point removed a point"), Spline->GetNumberOfSplinePoints(), Before - 1);
		ExpectActorDirty(*this, TEXT("remove_spline_point"), Actor);
		if (UndoOwnStep(*this, TEXT("remove_spline_point"), NSLOCTEXT("Nwiro", "RemoveSplinePoint", "AI: Remove Spline Point"), UndoBefore))
		{
			TestEqual(TEXT("undo restores the removed point"), Spline->GetNumberOfSplinePoints(), Before);
		}
	}

	// set_spline_closed
	{
		const bool bBefore = Spline->IsClosedLoop();
		Guard.Clean(Actor);
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, TEXT("set_spline_closed"),
			FString::Printf(TEXT("{\"actor\":\"%s\",\"closed\":true}"), *Label)).IsValid()) return false;
		TestTrue(TEXT("set_spline_closed closed the loop"), Spline->IsClosedLoop());
		ExpectActorDirty(*this, TEXT("set_spline_closed"), Actor);
		if (UndoOwnStep(*this, TEXT("set_spline_closed"), NSLOCTEXT("Nwiro", "SetSplineClosed", "AI: Set Spline Closed"), UndoBefore))
		{
			TestEqual(TEXT("undo restores the open loop"), Spline->IsClosedLoop(), bBefore);
		}
	}

	// set_spline_point_type
	{
		const int32 Before = static_cast<int32>(Spline->GetSplinePointType(0));
		Guard.Clean(Actor);
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, TEXT("set_spline_point_type"),
			FString::Printf(TEXT("{\"actor\":\"%s\",\"index\":0,\"type\":\"linear\"}"), *Label)).IsValid()) return false;
		TestEqual(TEXT("set_spline_point_type changed the point type"),
			static_cast<int32>(Spline->GetSplinePointType(0)), static_cast<int32>(ESplinePointType::Linear));
		ExpectActorDirty(*this, TEXT("set_spline_point_type"), Actor);
		if (UndoOwnStep(*this, TEXT("set_spline_point_type"), NSLOCTEXT("Nwiro", "SetSplinePointType", "AI: Set Spline Point Type"), UndoBefore))
		{
			TestEqual(TEXT("undo restores the point type"), static_cast<int32>(Spline->GetSplinePointType(0)), Before);
		}
	}
	return true;
}

// The spline tools graft a spline component onto an actor that has none, which is itself an edit.
// It is transacted and it dirties the actor, so the unsaved-work half of the contract holds.
//
// The graft runs inside the caller's transaction (GetOrAddSpline takes the caller's helper), so
// one tool call is one undo step and a single Ctrl+Z takes back the point AND the component.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkSplineGraftSpec,
	"Nwiro.UnsavedWork.SplineComponentGraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkSplineGraftSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("Graft"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	if (!TestNull(TEXT("the fixture starts without a spline component"),
		Actor->FindComponentByClass<USplineComponent>())) return false;

	Guard.Clean(Actor);
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("add_spline_point"),
		FString::Printf(TEXT("{\"actor\":\"%s\",\"x\":100,\"y\":0,\"z\":0}"), *Label)).IsValid()) return false;

	USplineComponent* Spline = Actor->FindComponentByClass<USplineComponent>();
	if (!TestNotNull(TEXT("add_spline_point grafted a spline component"), Spline)) return false;
	ExpectActorDirty(*this, TEXT("add_spline_point on a plain actor"), Actor);
	TestEqual(TEXT("the grafted spline carries the requested point"), Spline->GetNumberOfSplinePoints(), 1);

	// One call, one step: the graft runs inside the caller's transaction, so a single undo takes
	// back both the point and the component it grafted. Before that contract a Ctrl+Z removed the
	// point and left the component behind.
	if (UndoOwnStep(*this, TEXT("add_spline_point"), NSLOCTEXT("Nwiro", "AddSplinePoint", "AI: Add Spline Point"), UndoBefore))
	{
		// The undo destroys the grafted component, so the point goes with it. Never read the dead
		// object's state: it shows the two default points of a freshly constructed USplineComponent.
		TestNull(TEXT("one undo drops the grafted component, and its point with it"),
			Actor->FindComponentByClass<USplineComponent>());
		TestFalse(TEXT("the undo destroys the grafted component object itself"), IsValid(Spline));
		TestFalse(TEXT("no second nwiro step is left on the undo stack"),
			GEditor->Trans->GetUndoContext().Title.EqualTo(NSLOCTEXT("Nwiro", "AddSplineComponent", "AI: Add Spline Component")));
	}
	return true;
}

// ============================================================
// add_physics_constraint / spawn_sound / spawn_pcg_volume
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkPhysicsConstraintSpec,
	"Nwiro.UnsavedWork.AddPhysicsConstraint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkPhysicsConstraintSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString FirstLabel = UniqueLabel(TEXT("ConstraintA"));
	const FString SecondLabel = UniqueLabel(TEXT("ConstraintB"));
	if (!SpawnFixture(*this, AStaticMeshActor::StaticClass(), FirstLabel)) return false;
	if (!SpawnFixture(*this, AStaticMeshActor::StaticClass(), SecondLabel)) return false;

	Guard.Clean();
	const TSet<FName> Known = ActorNames();
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("add_physics_constraint"),
		FString::Printf(TEXT("{\"actor1\":\"%s\",\"actor2\":\"%s\",\"type\":\"hinge\"}"), *FirstLabel, *SecondLabel)).IsValid()) return false;

	const TArray<AActor*> Added = ActorsAddedSince(Known);
	if (!TestEqual(TEXT("add_physics_constraint adds exactly one actor"), Added.Num(), 1)) return false;
	Guard.Remember(Added[0]);
	ExpectActorDirty(*this, TEXT("add_physics_constraint"), Added[0]);
	if (UndoOwnStep(*this, TEXT("add_physics_constraint"), NSLOCTEXT("Nwiro", "AddPhysicsConstraint", "AI: Add Physics Constraint"), UndoBefore))
	{
		TestEqual(TEXT("undo removes the constraint actor"), ActorsAddedSince(Known).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkSpawnSoundSpec,
	"Nwiro.UnsavedWork.SpawnSound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkSpawnSoundSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	// An owned in-memory sound asset: the tool resolves it through the asset registry, so a
	// registered fixture is enough and no project content is required.
	FAssetFixture Sound(USoundWave::StaticClass(), TEXT("Sound"));
	if (!TestFalse(TEXT("the sound fixture has a path"), Sound.Path().IsEmpty())) return false;
	const FString Label = UniqueLabel(TEXT("SoundActor"));

	Guard.Clean();
	const TSet<FName> Known = ActorNames();
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("spawn_sound"),
		FString::Printf(TEXT("{\"sound\":\"%s\",\"label\":\"%s\",\"x\":0,\"y\":0,\"z\":0}"), *Sound.Path(), *Label)).IsValid()) return false;

	AActor* Spawned = FindActor(Label);
	if (!TestNotNull(TEXT("spawn_sound put the actor in the editor world"), Spawned)) return false;
	Guard.Remember(Spawned);
	ExpectActorDirty(*this, TEXT("spawn_sound"), Spawned);
	if (UndoOwnStep(*this, TEXT("spawn_sound"), NSLOCTEXT("Nwiro", "SpawnSound", "AI: Spawn Sound"), UndoBefore))
	{
		TestEqual(TEXT("undo removes the sound actor"), ActorsAddedSince(Known).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkSpawnPcgVolumeSpec,
	"Nwiro.UnsavedWork.SpawnPcgVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkSpawnPcgVolumeSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	FAssetFixture Graph(UPCGGraph::StaticClass(), TEXT("PcgGraph"));
	if (!TestFalse(TEXT("the PCG graph fixture has a path"), Graph.Path().IsEmpty())) return false;
	const FString Label = UniqueLabel(TEXT("PcgVolume"));

	Guard.Clean();
	const TSet<FName> Known = ActorNames();
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("spawn_pcg_volume"),
		FString::Printf(TEXT("{\"graph\":\"%s\",\"label\":\"%s\"}"), *Graph.Path(), *Label)).IsValid()) return false;

	AActor* Spawned = FindActor(Label);
	if (!TestNotNull(TEXT("spawn_pcg_volume put the volume in the editor world"), Spawned)) return false;
	Guard.Remember(Spawned);
	ExpectActorDirty(*this, TEXT("spawn_pcg_volume"), Spawned);
	if (UndoOwnStep(*this, TEXT("spawn_pcg_volume"), NSLOCTEXT("Nwiro", "SpawnPCGVolume", "AI: Spawn PCG Volume"), UndoBefore))
	{
		TestEqual(TEXT("undo removes the PCG volume"), ActorsAddedSince(Known).Num(), 0);
	}
	return true;
}

// ============================================================
// the level macros
// ============================================================

// create_basic_level, create_light_rig, create_grid_layout and create_ring_layout each spawn a
// batch of actors into the CURRENT editor world - none of them switches worlds - so each one is
// expected to leave a single compound undo step covering the whole batch, and every actor in the
// batch must carry the unsaved work on its own package.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkLevelMacroSpec,
	"Nwiro.UnsavedWork.LevelMacros",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkLevelMacroSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;

	struct FMacro
	{
		const TCHAR* Tool;
		const TCHAR* Args;
		int32 Expected;
		FText Title;
	};
	const FMacro Macros[] = {
		{ TEXT("create_basic_level"), TEXT("{}"), 4,
			NSLOCTEXT("Nwiro", "CreateBasicLevel", "AI: Create Basic Level") },
		{ TEXT("create_light_rig"), TEXT("{\"x\":0,\"y\":0,\"z\":300,\"radius\":500}"), 4,
			NSLOCTEXT("Nwiro", "CreateLightRig", "AI: Create Light Rig") },
		{ TEXT("create_grid_layout"), TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"rows\":2,\"cols\":2,\"spacing\":200}"), 4,
			NSLOCTEXT("Nwiro", "CreateGridLayout", "AI: Create Grid Layout") },
		{ TEXT("create_ring_layout"), TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":3,\"radius\":500}"), 3,
			NSLOCTEXT("Nwiro", "CreateRingLayout", "AI: Create Ring Layout") },
	};

	for (const FMacro& Macro : Macros)
	{
		FWorldGuard Guard;
		Guard.Clean();
		const TSet<FName> Known = ActorNames();
		const FGuid UndoBefore = UndoId();
		if (!Dispatch(*this, Macro.Tool, Macro.Args).IsValid()) continue;

		const TArray<AActor*> Added = ActorsAddedSince(Known);
		TestEqual(*FString::Printf(TEXT("%s spawns its whole batch"), Macro.Tool), Added.Num(), Macro.Expected);
		for (AActor* Actor : Added)
		{
			Guard.Remember(Actor);
			ExpectActorDirty(*this, Macro.Tool, Actor);
		}
		// One transaction for the batch: the whole macro undoes as a single step.
		if (UndoOwnStep(*this, Macro.Tool, Macro.Title, UndoBefore))
		{
			TestEqual(*FString::Printf(TEXT("one undo takes back the whole %s batch"), Macro.Tool),
				ActorsAddedSince(Known).Num(), 0);
		}
	}
	return true;
}

// ============================================================
// delete_actor
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkDeleteActorSpec,
	"Nwiro.UnsavedWork.DeleteActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkDeleteActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("Delete"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	// The package outlives the actor, and it is where the deletion has to show up as unsaved work.
	UPackage* const Package = Actor->GetOutermost();

	Guard.Clean(Actor);
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("delete_actor"),
		FString::Printf(TEXT("{\"name\":\"%s\"}"), *Label)).IsValid()) return false;

	TestNull(TEXT("delete_actor removes the actor"), FindActor(Label));
	// The engine dirties this by itself; what was missing was a transaction to record it in, so
	// the undo assertion below is the one that fails on the old behaviour.
	TestTrue(TEXT("delete_actor leaves the deletion as unsaved work on the actor's package"),
		Package->IsDirty());
	if (UndoOwnStep(*this, TEXT("delete_actor"), NSLOCTEXT("Nwiro", "DeleteActor", "AI: Delete Actor"), UndoBefore))
	{
		TestNotNull(TEXT("one undo brings the deleted actor back"), FindActor(Label));
	}
	return true;
}

// ============================================================
// rename_actor
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkRenameActorSpec,
	"Nwiro.UnsavedWork.RenameActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkRenameActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("Rename"));
	const FString NewLabel = UniqueLabel(TEXT("Renamed"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;

	Guard.Clean(Actor);
	const FGuid UndoBefore = UndoId();
	TSharedPtr<FJsonObject> Reply = Dispatch(*this, TEXT("rename_actor"),
		FString::Printf(TEXT("{\"name\":\"%s\",\"newName\":\"%s\"}"), *Label, *NewLabel));
	if (!Reply.IsValid()) return false;

	TestEqual(TEXT("rename_actor applies the new label"), Actor->GetActorLabel(), NewLabel);
	// The reply reports the label read back from the actor, so a rename the editor rejected can
	// never answer success with a name the actor does not carry.
	FString Reported;
	TestTrue(TEXT("rename_actor reports the label the actor now has"),
		Reply->TryGetStringField(TEXT("newLabel"), Reported) && Reported == Actor->GetActorLabel());
	bool bChanged = false;
	TestTrue(TEXT("rename_actor reports that it changed something"),
		Reply->TryGetBoolField(TEXT("changed"), bChanged) && bChanged);
	ExpectActorDirty(*this, TEXT("rename_actor"), Actor);
	if (UndoOwnStep(*this, TEXT("rename_actor"), NSLOCTEXT("Nwiro", "RenameActor", "AI: Rename Actor"), UndoBefore))
	{
		TestEqual(TEXT("one undo restores the old label"), Actor->GetActorLabel(), Label);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkRenameActorNoChangeSpec,
	"Nwiro.UnsavedWork.RenameActorToTheSameLabelChangesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkRenameActorNoChangeSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("RenameNoop"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;

	// SetActorLabel only Modify()s when the trimmed label really differs, so a rename to the label
	// the actor already has must not raise the flag - otherwise the guard would later refuse a save
	// over work nobody did.
	for (bool bWasDirty : { false, true })
	{
		Guard.Clean(Actor);
		Actor->GetOutermost()->SetDirtyFlag(bWasDirty);
		const FGuid UndoBefore = UndoId();

		TSharedPtr<FJsonObject> Reply;
		const FString Raw = FNwiroUnsavedWorkDispatch::DispatchTool(TEXT("rename_actor"),
			FString::Printf(TEXT("{\"name\":\"%s\",\"newName\":\"  %s  \"}"), *Label, *Label));
		if (!TestTrue(TEXT("the reply is JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) && Reply.IsValid())) return false;

		bool bSuccess = false;
		Reply->TryGetBoolField(TEXT("success"), bSuccess);
		TestTrue(TEXT("renaming to the same label is not an error"), bSuccess);
		bool bChanged = true;
		TestTrue(TEXT("the reply says nothing changed"),
			Reply->TryGetBoolField(TEXT("changed"), bChanged) && !bChanged);
		TestEqual(TEXT("a no-op rename leaves the dirty flag exactly as it found it"),
			Actor->GetOutermost()->IsDirty(), bWasDirty);
		TestEqual(TEXT("a no-op rename opens no transaction"), UndoId(), UndoBefore);
		TestEqual(TEXT("a no-op rename keeps the label"), Actor->GetActorLabel(), Label);
	}

	// An empty target is refused before anything is touched.
	Guard.Clean(Actor);
	const FGuid UndoBefore = UndoId();
	TSharedPtr<FJsonObject> Refusal;
	const FString Raw = FNwiroUnsavedWorkDispatch::DispatchTool(TEXT("rename_actor"),
		FString::Printf(TEXT("{\"name\":\"%s\",\"newName\":\"   \"}"), *Label));
	if (!TestTrue(TEXT("the refusal is JSON"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Refusal) && Refusal.IsValid())) return false;
	bool bSuccess = true;
	Refusal->TryGetBoolField(TEXT("success"), bSuccess);
	TestFalse(TEXT("an empty new label is refused"), bSuccess);
	TestFalse(TEXT("a refusal claims no dirty state"), Refusal->HasField(TEXT("dirtyAfter")));
	TestFalse(TEXT("a refusal dirties nothing"), Actor->GetOutermost()->IsDirty());
	TestEqual(TEXT("a refusal opens no transaction"), UndoId(), UndoBefore);
	return true;
}

// ============================================================
// detach_actor
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkDetachActorSpec,
	"Nwiro.UnsavedWork.DetachActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkDetachActorSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString ChildLabel = UniqueLabel(TEXT("DetachChild"));
	const FString ParentLabel = UniqueLabel(TEXT("DetachParent"));
	AActor* Child = SpawnFixture(*this, AStaticMeshActor::StaticClass(), ChildLabel);
	AActor* Parent = SpawnFixture(*this, AStaticMeshActor::StaticClass(), ParentLabel);
	if (!Child || !Parent) return false;
	Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
	if (!TestTrue(TEXT("the fixture child starts attached"), Child->GetAttachParentActor() == Parent)) return false;

	Guard.Clean(Child);
	Guard.Clean(Parent);
	const FGuid UndoBefore = UndoId();
	if (!Dispatch(*this, TEXT("detach_actor"),
		FString::Printf(TEXT("{\"name\":\"%s\"}"), *ChildLabel)).IsValid()) return false;

	TestNull(TEXT("detach_actor detached the child"), Child->GetAttachParentActor());
	// Attachment is persisted on the child, so only the child's package carries the change; the
	// engine records the old parent with Modify(false) and must not dirty it.
	ExpectActorDirty(*this, TEXT("detach_actor"), Child);
	TestFalse(TEXT("detach_actor does not dirty the unchanged parent package"),
		Parent->GetOutermost() != Child->GetOutermost() && Parent->GetOutermost()->IsDirty());
	if (UndoOwnStep(*this, TEXT("detach_actor"), NSLOCTEXT("Nwiro", "DetachActor", "AI: Detach Actor"), UndoBefore))
	{
		TestTrue(TEXT("one undo re-attaches the child"), Child->GetAttachParentActor() == Parent);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkDetachUnattachedSpec,
	"Nwiro.UnsavedWork.DetachingAnUnattachedActorChangesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkDetachUnattachedSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("DetachNoop"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	if (!TestNull(TEXT("the fixture actor starts unattached"), Actor->GetAttachParentActor())) return false;

	// DetachFromComponent early-outs with no attach parent, so nothing is modified. The reply must
	// say so instead of claiming work, and the flag must stay exactly where it was.
	for (bool bWasDirty : { false, true })
	{
		Guard.Clean(Actor);
		Actor->GetOutermost()->SetDirtyFlag(bWasDirty);
		const FGuid UndoBefore = UndoId();

		TSharedPtr<FJsonObject> Reply;
		const FString Raw = FNwiroUnsavedWorkDispatch::DispatchTool(TEXT("detach_actor"),
			FString::Printf(TEXT("{\"name\":\"%s\"}"), *Label));
		if (!TestTrue(TEXT("the reply is JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) && Reply.IsValid())) return false;

		bool bSuccess = false;
		Reply->TryGetBoolField(TEXT("success"), bSuccess);
		TestTrue(TEXT("detaching an unattached actor is not an error"), bSuccess);
		bool bChanged = true;
		TestTrue(TEXT("the reply says nothing changed"),
			Reply->TryGetBoolField(TEXT("changed"), bChanged) && !bChanged);
		TestEqual(TEXT("it leaves the dirty flag exactly as it found it"),
			Actor->GetOutermost()->IsDirty(), bWasDirty);
		TestEqual(TEXT("it opens no transaction"), UndoId(), UndoBefore);
	}
	return true;
}

// ============================================================
// get_spline_info - a read that must not create work
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsavedWorkSplineInfoReadOnlySpec,
	"Nwiro.UnsavedWork.SplineInfoNeverCreatesWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroUnsavedWorkSplineInfoReadOnlySpec::RunTest(const FString& Parameters)
{
	using namespace NwiroUnsavedWorkSpec;
	if (!Ready(*this)) return false;
	FWorldGuard Guard;

	const FString Label = UniqueLabel(TEXT("SplineRead"));
	AActor* Actor = SpawnFixture(*this, AStaticMeshActor::StaticClass(), Label);
	if (!Actor) return false;
	if (!TestNull(TEXT("the fixture actor has no spline"), Actor->FindComponentByClass<USplineComponent>())) return false;

	// Asking what a spline looks like used to graft one onto an actor that had none: a query that
	// dirtied a package and put a step on the undo stack. Both starting states are checked because
	// a read must not clear a flag either.
	for (bool bWasDirty : { false, true })
	{
		Guard.Clean(Actor);
		Actor->GetOutermost()->SetDirtyFlag(bWasDirty);
		const FGuid UndoBefore = UndoId();

		TSharedPtr<FJsonObject> Reply;
		const FString Raw = FNwiroUnsavedWorkDispatch::DispatchTool(TEXT("get_spline_info"),
			FString::Printf(TEXT("{\"actor\":\"%s\"}"), *Label));
		if (!TestTrue(TEXT("the reply is JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) && Reply.IsValid())) return false;

		bool bSuccess = true;
		Reply->TryGetBoolField(TEXT("success"), bSuccess);
		TestFalse(TEXT("reading a spline the actor does not have is reported, not invented"), bSuccess);
		TestNull(TEXT("the read grafts no component"), Actor->FindComponentByClass<USplineComponent>());
		TestEqual(TEXT("the read leaves the dirty flag exactly as it found it"),
			Actor->GetOutermost()->IsDirty(), bWasDirty);
		TestEqual(TEXT("the read opens no transaction"), UndoId(), UndoBefore);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
