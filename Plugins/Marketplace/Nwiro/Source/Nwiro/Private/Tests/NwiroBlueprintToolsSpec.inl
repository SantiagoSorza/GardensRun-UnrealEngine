// Copyright 2026 Nwiro. All Rights Reserved.
//
// SHARED SPEC — the two copies in plugin/Nwiro and plugin/NwiroIntegrationKit differ ONLY in
// the file name and in the test-class identifiers below. The product plugins are deliberately separate trees
// with zero compile-time dependency on each other (see AGENTS.md), so the file is duplicated
// on disk rather than shared through a module; `scripts/check-plugin-parity.ts` compares the
// two copies after normalising those identifiers and fails on any other difference.
//
// Why the class names cannot be macro-parameterised like everything else: UE registers a test
// under TEXT(#TClass) (AutomationTest.h:4301) — the STRINGIZED macro argument. Passing a macro
// would register both products under the same literal name and the second registration is
// silently dropped ("Test with the same name is already registered"). Hand-rolling the engine
// macro to work around that would repeat the very anti-pattern defect D1 came from.
//
// The includer supplies four macros before including this file:
//   NWIRO_SPEC_TOOLS   — the Blueprint tools class (FNwiroBlueprintTools / FNwiroIKBlueprintTools)
//   NWIRO_SPEC_SCHEMA  — the class exposing GetEnabledToolDefinitionsJson()
//   NWIRO_SPEC_PREFIX  — automation-test path prefix, e.g. "Nwiro.BlueprintTools."
//   NWIRO_SPEC_PKG     — package root for fixtures, e.g. "/Game/__NwiroToolSpec/"
//
// RED-first specs for four defects in the Blueprint tool layer, plus two specs covering risks the
// fixes themselves introduced.
//
//   D1  a dispatcher created with no signature graph (SignatureFunction stays null)
//   D2  makeRoot silently ineffective when the existing root is user-named
//   D3  a required "variable" field neither declared in the schema nor validated
//   F4  an accepted "default" field for add_variables that the schema never advertised
//
//   E   the makeRoot refusal path added by the D2/F3 fix (a new branch needs its own spec)
//   F   the partial-success contract that E's refusal counter forces the tool to commit to
//
// The defect specs assert on ENGINE STATE, never on the tool's success envelope: every one of
// them reported success while producing an incomplete artifact, so the envelope is exactly the
// witness that cannot be trusted. The two risk specs invert that deliberately — there the
// envelope IS the behaviour under test, and each pairs its envelope assertion with a state
// assertion so a tool that merely learned to say "false" cannot pass.

#include "Misc/AutomationTest.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EditorAssetLibrary.h"
#include "UObject/UObjectIterator.h"

#if !defined(NWIRO_SPEC_TOOLS) || !defined(NWIRO_SPEC_SCHEMA) || !defined(NWIRO_SPEC_PREFIX) || !defined(NWIRO_SPEC_PKG)
#error "NwiroBlueprintToolsSpec.inl requires NWIRO_SPEC_TOOLS/SCHEMA/PREFIX/PKG to be defined by the includer"
#endif

#if WITH_EDITOR

// ---------------------------------------------------------------------------
// Fixture
//
// The tool layer resolves Blueprints through LoadBP -> LoadObject(path). A transient-package
// Blueprint is therefore NOT reachable by the code under test — the fixture must live in a
// named /Game package so the tool can find it by path.
// ---------------------------------------------------------------------------
namespace NwiroBPSpec
{
	/**
	 * Creates a compiled Actor Blueprint reachable by asset path. Returns null on failure,
	 * INCLUDING when a stale fixture from a previous run could not be removed — a spec measured
	 * against last run's end state produces confident nonsense (leftover components get
	 * uniquified names like "SpecPivot1" and every hierarchy assertion drifts).
	 */
	static UBlueprint* MakeActorBP(const FString& AssetName, FString& OutObjectPath)
	{
		const FString PackagePath = FString(TEXT(NWIRO_SPEC_PKG)) + AssetName;
		OutObjectPath = PackagePath + TEXT(".") + AssetName;

		// Idempotency matters more than it looks: the tool layer SAVES the Blueprint it edits, so
		// a fixture survives on disk between runs.
		if (UEditorAssetLibrary::DoesAssetExist(OutObjectPath))
		{
			UEditorAssetLibrary::DeleteAsset(OutObjectPath);
		}
		if (UObject* Stale = FindObject<UObject>(nullptr, *OutObjectPath))
		{
			Stale->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Stale->MarkAsGarbage();
		}
		// The delete is verified rather than assumed. DeleteAsset returns a bool that is easy to
		// drop, and a silently failed delete is indistinguishable from a clean fixture until the
		// assertions start reporting names that were never requested.
		if (UEditorAssetLibrary::DoesAssetExist(OutObjectPath) ||
			FindObject<UObject>(nullptr, *OutObjectPath) != nullptr)
		{
			// Known limitation, stated rather than papered over: UEditorAssetLibrary::DeleteAsset
			// does not reliably remove the fixture under `-unattended`, so a run that follows an
			// earlier one finds the previous fixtures still on disk. Bailing here is deliberate —
			// before this guard existed the specs silently measured last run's end state and
			// reported hierarchies containing names nobody asked for ("SpecPivot1").
			UE_LOG(LogTemp, Error,
				TEXT("[NwiroBPSpec] stale fixture '%s' could not be removed. Delete the folder ")
				TEXT("'<Project>/Content/%s' and re-run; the suite requires a clean fixture root."),
				*OutObjectPath, TEXT(NWIRO_SPEC_PKG));
			return nullptr;
		}

		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package) return nullptr;

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

		if (BP) FKismetEditorUtilities::CompileBlueprint(BP);
		return BP;
	}

	/** Parses a tool result envelope and reports its "success" field. */
	static bool EnvelopeSuccess(const FString& ResultJson)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) return false;
		bool bSuccess = false;
		return Obj->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess;
	}

	/** First ubergraph page (the "EventGraph"), or null. */
	static UEdGraph* EventGraph(UBlueprint* BP)
	{
		return (BP && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
	}

	/**
	 * The SCS node carrying VariableName, or null.
	 *
	 * GetVariableName(), never ComponentTemplate->GetFName(): the latter reads the
	 * "<name>_GEN_VARIABLE" template name and is the exact comparison that made the production
	 * code wrong. A finder built on it would agree with the bug instead of catching it.
	 */
	static USCS_Node* FindSCSNode(USimpleConstructionScript* SCS, const TCHAR* VariableName)
	{
		if (!SCS) return nullptr;
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName() == FName(VariableName)) return Node;
		}
		return nullptr;
	}

	/** True when a node with ChildVariableName sits directly under Parent. */
	static bool IsChildOfNode(USCS_Node* Parent, const TCHAR* ChildVariableName)
	{
		if (!Parent) return false;
		for (USCS_Node* Child : Parent->GetChildNodes())
		{
			if (Child && Child->GetVariableName() == FName(ChildVariableName)) return true;
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// SPEC 0 — harness self-check.
//
// Runs first by name. If this is RED, every other spec in this file is RED for setup reasons
// and its result carries no information about the product.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsHarnessSpec,
	NWIRO_SPEC_PREFIX "A_Harness.FixtureIsReachableByTheToolLayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsHarnessSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroHarnessSpec"), Path);

	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	// The whole suite depends on this: the code under test resolves Blueprints by path
	// (LoadBP -> LoadObject). LoadBP itself is private, so reachability is asserted through the
	// public seam instead — which is the seam the specs use.
	const FString Probe = FString::Printf(TEXT("{\"blueprint\":\"%s\",\"compile\":true}"), *Path);
	const FString ProbeResult = NWIRO_SPEC_TOOLS::EditBlueprint(Probe);
	TestFalse(TEXT("the tool layer can resolve the fixture by asset path"),
		ProbeResult.Contains(TEXT("Blueprint not found")));

	TestEqual(TEXT("a freshly created Actor Blueprint compiles clean"),
		BP->Status, EBlueprintStatus::BS_UpToDate);

	TestNotNull(TEXT("fixture has an event graph"), NwiroBPSpec::EventGraph(BP));

	return true;
}

// ---------------------------------------------------------------------------
// SPEC 1 — D1: a created dispatcher must be a fully formed multicast delegate.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsEventDispatcherSpec,
	NWIRO_SPEC_PREFIX "EventDispatcher.HasSignatureFunction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsEventDispatcherSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroDispatcherSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	const FName DispatcherName(TEXT("OnSpecEvent"));

	const FString Command = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"name\":\"OnSpecEvent\""
		"}"), *Path);

	const FString Result = NWIRO_SPEC_TOOLS::CreateEventDispatcher(Command);

	// The envelope is deliberately NOT the gate: it already reported success before the fix.
	// It is recorded only so a change that starts FAILING the call is visible.
	TestTrue(TEXT("tool reports success (context, not evidence)"),
		NwiroBPSpec::EnvelopeSuccess(Result));

	FKismetEditorUtilities::CompileBlueprint(BP);

	// A1 — precondition, not evidence: the delegate property exists at all.
	FMulticastDelegateProperty* Prop =
		FindFProperty<FMulticastDelegateProperty>(BP->GeneratedClass, DispatcherName);
	if (!TestNotNull(TEXT("A1 delegate property exists on the generated class"), Prop))
	{
		return false;
	}

	// A2 — THE GATE. Without a signature the delegate cannot be bound or broadcast:
	// UK2Node_CallDelegate::AllocateDefaultPins would call CreatePinsForFunctionInputs(nullptr).
	// .Get(): SignatureFunction is a TObjectPtr, which TestNotNull's template cannot bind.
	TestNotNull(TEXT("A2 the delegate property has a SignatureFunction"),
		Prop->SignatureFunction.Get());

	// A3 — THE GATE. The signature is stored as a real graph on the Blueprint. Asserted against
	// DelegateSignatureGraphs directly rather than GetAllGraphs(), which aggregates several
	// lists and is a weaker witness.
	bool bSignatureGraphRegistered = false;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetFName() == DispatcherName)
		{
			bSignatureGraphRegistered = true;
			break;
		}
	}
	TestTrue(TEXT("A3 a signature graph is registered in DelegateSignatureGraphs"),
		bSignatureGraphRegistered);

	// A4 — anti-fake. A2 alone could be satisfied by assigning any borrowed UFunction.
	if (Prop->SignatureFunction)
	{
		const uint32 Required = FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public;
		TestTrue(TEXT("A4 the signature function carries the delegate function flags"),
			(Prop->SignatureFunction->FunctionFlags & Required) == Required);
	}

	// A5 — the observable symptom: the compiler emits
	// "No SignatureFunction in MulticastDelegateProperty" and downgrades the status.
	TestEqual(TEXT("A5 the Blueprint still compiles without warnings"),
		BP->Status, EBlueprintStatus::BS_UpToDate);

	// A7 — the other half of the lifecycle a signature graph introduces: RENAMING a dispatcher.
	// Asserted before any fix is written for it, on purpose: the removal case looked broken by the
	// same reasoning and turned out to be handled elsewhere, so this one is measured rather than
	// assumed. Uses its own dispatcher so A6 below is unaffected.
	{
		const FString AddSecond = FString::Printf(TEXT(
			"{\"blueprint\":\"%s\",\"name\":\"OnSpecRenameMe\"}"), *Path);
		NWIRO_SPEC_TOOLS::CreateEventDispatcher(AddSecond);

		const FString RenameCommand = FString::Printf(TEXT(
			"{"
			"\"blueprint\":\"%s\","
			"\"rename_variables\":[{\"old\":\"OnSpecRenameMe\",\"new\":\"OnSpecRenamed\"}]"
			"}"), *Path);
		NWIRO_SPEC_TOOLS::EditBlueprint(RenameCommand);
		FKismetEditorUtilities::CompileBlueprint(BP);

		bool bGraphUnderOldName = false;
		bool bGraphUnderNewName = false;
		for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
		{
			if (!Graph) continue;
			if (Graph->GetFName() == FName(TEXT("OnSpecRenameMe"))) bGraphUnderOldName = true;
			if (Graph->GetFName() == FName(TEXT("OnSpecRenamed"))) bGraphUnderNewName = true;
		}
		TestFalse(TEXT("A7 no signature graph is left behind under the old dispatcher name"),
			bGraphUnderOldName);
		TestTrue(TEXT("A7 the signature graph follows the dispatcher to its new name"),
			bGraphUnderNewName);
	}

	// A6 — the companion of A3: once a dispatcher gains a signature graph, removing the dispatcher
	// must not leave that graph behind.
	//
	// This is a pure invariant guard, and its history is worth knowing. It looked like a gap this
	// change set had created: FBlueprintEditorUtils::RemoveMemberVariable provably does NOT touch
	// DelegateSignatureGraphs (read it — it only edits NewVariables, removes variable nodes and
	// marks the Blueprint modified), and the engine's own
	// UBlueprintEditorLibrary::RemoveEventDispatcher carries a comment saying exactly that before
	// cleaning up explicitly. An explicit cleanup was added here for the same reason — and then a
	// mutation test disabled it and A6 still passed, proving the graph is already gone by this
	// point on the remove_variables path (the compile that follows removal accounts for it).
	// The redundant code was therefore removed rather than shipped, and the assertion kept: it
	// costs nothing and will catch the day that implicit cleanup stops happening.
	const FString RemoveCommand = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"remove_variables\":[\"OnSpecEvent\"]"
		"}"), *Path);
	NWIRO_SPEC_TOOLS::EditBlueprint(RemoveCommand);
	FKismetEditorUtilities::CompileBlueprint(BP);

	bool bOrphanSignatureGraph = false;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetFName() == DispatcherName)
		{
			bOrphanSignatureGraph = true;
			break;
		}
	}
	TestFalse(TEXT("A6 removing the dispatcher also removed its signature graph"),
		bOrphanSignatureGraph);
	TestNull(TEXT("A6 the delegate property is gone from the generated class"),
		FindFProperty<FMulticastDelegateProperty>(BP->GeneratedClass, DispatcherName));

	return true;
}

// ---------------------------------------------------------------------------
// SPEC 2 — D2: makeRoot must re-home the previous root whatever it is NAMED.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsComponentRootSpec,
	NWIRO_SPEC_PREFIX "Components.MakeRootRehomesANamedRoot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsComponentRootSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroRootSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	USimpleConstructionScript* SeedSCS = BP->SimpleConstructionScript;
	if (!TestNotNull(TEXT("Blueprint has a SimpleConstructionScript"), SeedSCS)) return false;

	// The engine's generated placeholder, captured BEFORE any makeRoot runs. Capturing it up
	// front — and asserting here that it exists — is what makes the later absence checks mean
	// something: a pointer fetched only afterwards could be null for the wrong reason, and the
	// assertion would pass without testing anything.
	USCS_Node* const PlaceholderBefore = SeedSCS->GetDefaultSceneRootNode();
	TestNotNull(TEXT("control: a fresh Actor Blueprint starts with a generated DefaultSceneRoot"),
		PlaceholderBefore);

	// Positive control for the name scan used by B5b below. Without it, B5b would also pass if the
	// predicate simply never matched anything — an absence check is only worth as much as the
	// finder behind it.
	bool bScanFindsPlaceholderBefore = false;
	for (USCS_Node* Node : SeedSCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == USceneComponent::GetDefaultSceneRootVariableName())
		{
			bScanFindsPlaceholderBefore = true;
			break;
		}
	}
	TestTrue(TEXT("control: the placeholder scan can find a placeholder when one exists"),
		bScanFindsPlaceholderBefore);

	// Seed: a USER-NAMED root. makeRoot is used here too, and it succeeds, because at this point
	// the existing root still IS the engine's "DefaultSceneRoot" — the single case the original
	// implementation handled. That is what sets up the case it did NOT handle, exercised below.
	const FString Seed = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":[{\"name\":\"SpecMesh\",\"class\":\"StaticMeshComponent\",\"makeRoot\":true}]"
		"}"), *Path);
	NWIRO_SPEC_TOOLS::EditBlueprint(Seed);
	FKismetEditorUtilities::CompileBlueprint(BP);

	USimpleConstructionScript* SCS = SeedSCS;

	// B5 — THE GATE for the placeholder path, and the one a live test caught that every other
	// assertion here tolerated. The generated placeholder must be REMOVED by makeRoot, not
	// demoted: it used to survive as a CHILD of the new root, which leaves B1/B2/B3 green.
	//
	// Asserted two ways on purpose:
	//   * identity — the node captured before the operation must be gone from GetAllNodes();
	//   * name — no node may carry the placeholder's variable name, which also catches the case
	//     where the engine legitimately re-creates a fresh placeholder.
	// GetAllNodes(), not GetRootNodes(): the leftover is not a root. And GetVariableName(), never
	// ComponentTemplate->GetFName() — the latter reads the "<name>_GEN_VARIABLE" template name and
	// is the exact comparison that made the production code wrong; reusing it here would make this
	// assertion agree with the bug.
	TestFalse(TEXT("B5a makeRoot removed the captured DefaultSceneRoot node"),
		SCS->GetAllNodes().Contains(PlaceholderBefore));

	bool bAnyPlaceholderRemains = false;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == USceneComponent::GetDefaultSceneRootVariableName())
		{
			bAnyPlaceholderRemains = true;
			break;
		}
	}
	TestFalse(TEXT("B5b no DefaultSceneRoot placeholder remains anywhere in the SCS"),
		bAnyPlaceholderRemains);

	// Baseline asserted, not assumed — if the seed did not become the root, the scenario this
	// spec exists to exercise never happened.
	{
		const TArray<USCS_Node*>& SeedRoots = SCS->GetRootNodes();
		if (!TestEqual(TEXT("baseline: exactly one root after seeding"), SeedRoots.Num(), 1))
		{
			return false;
		}
		// GetVariableName(), not GetFName(): SCS component templates carry a "_GEN_VARIABLE"
		// suffix and a raw name comparison would never match.
		TestEqual(TEXT("baseline: the seeded component is the root"),
			SeedRoots[0]->GetVariableName(), FName(TEXT("SpecMesh")));
	}

	// Act: introduce a new root above it.
	const FString Act = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":[{\"name\":\"SpecPivot\",\"class\":\"SceneComponent\",\"makeRoot\":true}]"
		"}"), *Path);
	const FString Result = NWIRO_SPEC_TOOLS::EditBlueprint(Act);
	FKismetEditorUtilities::CompileBlueprint(BP);

	// B0 — blocks a pseudo-fix that simply REFUSES makeRoot when the existing root is not
	// "DefaultSceneRoot": refusing would satisfy "exactly one root" by doing nothing.
	TestTrue(TEXT("B0 the makeRoot request is accepted"),
		NwiroBPSpec::EnvelopeSuccess(Result));

	const TArray<USCS_Node*>& Roots = SCS->GetRootNodes();

	// B1 — THE GATE.
	TestEqual(TEXT("B1 the Blueprint has exactly one SCS root"), Roots.Num(), 1);

	if (Roots.Num() == 1)
	{
		// B2 — the new component is the one on top.
		TestEqual(TEXT("B2 the new component is the root"),
			Roots[0]->GetVariableName(), FName(TEXT("SpecPivot")));

		// B3 — THE GATE. The previous root was re-homed rather than dropped or kept alongside.
		bool bSeedIsChildOfPivot = false;
		for (USCS_Node* Child : Roots[0]->GetChildNodes())
		{
			if (Child && Child->GetVariableName() == FName(TEXT("SpecMesh")))
			{
				bSeedIsChildOfPivot = true;
				break;
			}
		}
		TestTrue(TEXT("B3 the previous root is now a child of the new root"), bSeedIsChildOfPivot);
	}

	// B4 — a second root is not merely untidy; it is a compile-time problem.
	TestEqual(TEXT("B4 the Blueprint compiles without warnings"),
		BP->Status, EBlueprintStatus::BS_UpToDate);

	return true;
}

// ---------------------------------------------------------------------------
// SPEC 3 — D3b: a VariableGet request without "variable" must be REJECTED.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsNodeVariableSpec,
	NWIRO_SPEC_PREFIX "Nodes.MissingVariableFieldIsRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsNodeVariableSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroNodeSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	const FString AddVar = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_variables\":[{\"name\":\"bSpecFlag\",\"type\":\"bool\"}]"
		"}"), *Path);
	NWIRO_SPEC_TOOLS::EditBlueprint(AddVar);
	FKismetEditorUtilities::CompileBlueprint(BP);

	UEdGraph* Graph = NwiroBPSpec::EventGraph(BP);
	if (!TestNotNull(TEXT("fixture has an event graph"), Graph)) return false;

	const int32 NodesBefore = Graph->Nodes.Num();

	// Act: ask for a VariableGet WITHOUT the required "variable" field.
	const FString BadRequest = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"graph\":\"EventGraph\","
		"\"add_nodes\":[{\"ref\":\"get1\",\"type\":\"VariableGet\"}]"
		"}"), *Path);
	const FString BadResult = NWIRO_SPEC_TOOLS::EditBlueprint(BadRequest);

	// C1 — THE GATE. Missing required input must be a loud failure, not a quiet success.
	TestFalse(TEXT("C1 a VariableGet without \"variable\" is reported as a failure"),
		NwiroBPSpec::EnvelopeSuccess(BadResult));

	// C2 — THE GATE. And nothing may be left behind in the graph.
	TestEqual(TEXT("C2 no node was created for the rejected request"),
		Graph->Nodes.Num(), NodesBefore);

	// C3 — positive control. Without it, C1/C2 would also pass if add_nodes were broken outright,
	// or if this spec were pointed at the wrong graph.
	//
	// Runs on its OWN Blueprint on purpose: before the fix the rejected request still leaked an
	// unbound node into the graph, so searching the shared graph would find THAT node and the
	// control would report a failure it did not cause.
	FString CleanPath;
	UBlueprint* CleanBP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroNodeSpecControl"), CleanPath);
	if (!TestNotNull(TEXT("control fixture Blueprint was created"), CleanBP)) return false;

	const FString CleanAddVar = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_variables\":[{\"name\":\"bSpecFlag\",\"type\":\"bool\"}]"
		"}"), *CleanPath);
	NWIRO_SPEC_TOOLS::EditBlueprint(CleanAddVar);
	FKismetEditorUtilities::CompileBlueprint(CleanBP);

	UEdGraph* CleanGraph = NwiroBPSpec::EventGraph(CleanBP);
	if (!TestNotNull(TEXT("control fixture has an event graph"), CleanGraph)) return false;

	const FString GoodRequest = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"graph\":\"EventGraph\","
		"\"add_nodes\":[{\"ref\":\"get1\",\"type\":\"VariableGet\",\"variable\":\"bSpecFlag\"}]"
		"}"), *CleanPath);
	const FString GoodResult = NWIRO_SPEC_TOOLS::EditBlueprint(GoodRequest);

	TestTrue(TEXT("C3a a well-formed VariableGet is accepted"),
		NwiroBPSpec::EnvelopeSuccess(GoodResult));

	UK2Node_VariableGet* Created = nullptr;
	for (UEdGraphNode* Node : CleanGraph->Nodes)
	{
		if (UK2Node_VariableGet* AsGet = Cast<UK2Node_VariableGet>(Node))
		{
			Created = AsGet;
			break;
		}
	}
	if (TestNotNull(TEXT("C3b the well-formed request created a VariableGet node"), Created))
	{
		// Bound, not merely present: an unbound node carries NAME_None and resolves to null.
		TestEqual(TEXT("C3c the node references the requested variable"),
			Created->VariableReference.GetMemberName(), FName(TEXT("bSpecFlag")));
		TestNotNull(TEXT("C3d the node's variable reference resolves against the Blueprint"),
			Created->VariableReference.ResolveMember<FProperty>(CleanBP->GeneratedClass));
	}

	return true;
}

// ---------------------------------------------------------------------------
// SPEC 4 — D3a: the schema handed to the model must declare "variable".
//
// Navigated structurally. A substring search for "variable" would pass even before the fix,
// because the edit_blueprint description prose already mentions "VariableGet".
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsSchemaSpec,
	NWIRO_SPEC_PREFIX "Schema.AddNodesDeclaresVariableField",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsSchemaSpec::RunTest(const FString& Parameters)
{
	// The ENABLED list deliberately, not the settings list: this spec is about what the model is
	// actually told. Its own doc comment calls the enabled list "the only list a model may ever see".
	const FString ToolsJson = NWIRO_SPEC_SCHEMA::GetEnabledToolDefinitionsJson();

	TArray<TSharedPtr<FJsonValue>> Tools;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolsJson);
	if (!TestTrue(TEXT("tool definitions parse as a JSON array"),
		FJsonSerializer::Deserialize(Reader, Tools)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> EditBlueprintTool;
	for (const TSharedPtr<FJsonValue>& Value : Tools)
	{
		const TSharedPtr<FJsonObject> Tool = Value->AsObject();
		if (Tool.IsValid() && Tool->GetStringField(TEXT("name")) == TEXT("edit_blueprint"))
		{
			EditBlueprintTool = Tool;
			break;
		}
	}
	// A missing tool would make the gate below vacuous. Fail loudly instead — and note this reads
	// as a configuration problem, not as defect D3a.
	if (!TestTrue(TEXT("the enabled tool list contains edit_blueprint (else: tool disabled in settings)"),
		EditBlueprintTool.IsValid()))
	{
		return false;
	}

	// inputSchema.properties.add_nodes -> oneOf[array].items -> general node schema
	const TSharedPtr<FJsonObject>* InputSchema = nullptr;
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	const TSharedPtr<FJsonObject>* AddNodes = nullptr;
	if (!TestTrue(TEXT("edit_blueprint declares an inputSchema"),
			EditBlueprintTool->TryGetObjectField(TEXT("inputSchema"), InputSchema))
		|| !TestTrue(TEXT("inputSchema declares properties"),
			(*InputSchema)->TryGetObjectField(TEXT("properties"), Properties))
		|| !TestTrue(TEXT("properties declares add_nodes"),
			(*Properties)->TryGetObjectField(TEXT("add_nodes"), AddNodes)))
	{
		return false;
	}

	// add_nodes is a oneOf[array, JSON-encoded string]; the array arm carries the item schema.
	// Held by value: AsObject() returns a temporary, so keeping a pointer to it would dangle.
	TSharedPtr<FJsonObject> ArrayArm = *AddNodes;
	const TArray<TSharedPtr<FJsonValue>>* OneOf = nullptr;
	if ((*AddNodes)->TryGetArrayField(TEXT("oneOf"), OneOf))
	{
		for (const TSharedPtr<FJsonValue>& Arm : *OneOf)
		{
			const TSharedPtr<FJsonObject> ArmObj = Arm->AsObject();
			if (ArmObj.IsValid() && ArmObj->GetStringField(TEXT("type")) == TEXT("array"))
			{
				ArrayArm = ArmObj;
				break;
			}
		}
	}

	const TSharedPtr<FJsonObject>* Items = nullptr;
	if (!TestTrue(TEXT("the add_nodes array arm declares items"),
			ArrayArm->TryGetObjectField(TEXT("items"), Items)))
	{
		return false;
	}

	// The item union separates the enum-constrained EnhancedInputAction recipe from
	// general nodes. VariableGet/VariableSet belong to the general branch; requiring
	// properties directly on the union wrapper would reject the valid public schema.
	TSharedPtr<FJsonObject> VariableNodeItems = *Items;
	const TArray<TSharedPtr<FJsonValue>>* ItemAlternatives = nullptr;
	if ((*Items)->TryGetArrayField(TEXT("oneOf"), ItemAlternatives))
	{
		VariableNodeItems.Reset();
		int32 GeneralBranches = 0;
		for (const TSharedPtr<FJsonValue>& Alternative : *ItemAlternatives)
		{
			if (!Alternative.IsValid() || Alternative->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> Candidate = Alternative->AsObject();
			const TSharedPtr<FJsonObject>* CandidateProperties = nullptr;
			const TSharedPtr<FJsonObject>* NodeType = nullptr;
			if (!Candidate.IsValid()
				|| !Candidate->TryGetObjectField(TEXT("properties"), CandidateProperties)
				|| !(*CandidateProperties)->TryGetObjectField(TEXT("type"), NodeType)
				|| (*NodeType)->HasField(TEXT("enum")) || (*NodeType)->HasField(TEXT("const")))
			{
				continue;
			}
			VariableNodeItems = Candidate;
			++GeneralBranches;
		}
		if (!TestEqual(TEXT("add_nodes item union has one general node branch"), GeneralBranches, 1))
		{
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* ItemProperties = nullptr;
	const TSharedPtr<FJsonObject>* VariableProperty = nullptr;
	if (!TestTrue(TEXT("general node items declare properties"),
			VariableNodeItems.IsValid()
				&& VariableNodeItems->TryGetObjectField(TEXT("properties"), ItemProperties))
		|| !TestTrue(TEXT("general node items declare the \"variable\" property"),
			(*ItemProperties)->TryGetObjectField(TEXT("variable"), VariableProperty)))
	{
		return false;
	}

	// THE GATE. The general-node schema must expose the string field consumed by
	// VariableGet/VariableSet. A mention in prose or in the specialized arm is insufficient.
	FString VariableType;
	TestTrue(TEXT("the variable property is declared as a string"),
		(*VariableProperty)->TryGetStringField(TEXT("type"), VariableType)
			&& VariableType == TEXT("string"));

	return true;
}

// ---------------------------------------------------------------------------
// SPEC 5 — a variable's requested default must both APPLY and be DISCOVERABLE.
//
// Found by a live test: asked for "FireRate with default 0.25 and bIsActive with default true",
// the model produced both variables with 0.0 and false, and every tool call returned success.
// The reader already accepts `default` (plus the aliases `defaultValue` / `initialValue`); the
// schema declared none of them, so a model cannot know the capability exists.
//
// The two halves are asserted together on purpose. Declaring a field that turns out to be inert
// would recreate the same silent-success defect one level up: the model would now emit `default`,
// the tool would still ignore it, and the envelope would still say success.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsVariableDefaultSpec,
	NWIRO_SPEC_PREFIX "Variables.DefaultIsAppliedAndDeclared",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsVariableDefaultSpec::RunTest(const FString& Parameters)
{
	// ---- half 1: the capability actually works -----------------------------------------
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroVarDefaultSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	const FString AddVars = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_variables\":["
		"{\"name\":\"SpecRate\",\"type\":\"float\",\"default\":\"0.25\"},"
		"{\"name\":\"bSpecActive\",\"type\":\"bool\",\"default\":\"true\"}"
		"]"
		"}"), *Path);
	const FString Result = NWIRO_SPEC_TOOLS::EditBlueprint(AddVars);
	TestTrue(TEXT("D1 the add_variables call is accepted"), NwiroBPSpec::EnvelopeSuccess(Result));

	FKismetEditorUtilities::CompileBlueprint(BP);

	// Read the CDO, not the tool's reply: the whole point is that the reply already says success.
	// .Get(): GeneratedClass is a TObjectPtr, which TestNotNull's template cannot bind.
	if (!TestNotNull(TEXT("D2 the Blueprint has a generated class"), BP->GeneratedClass.Get())) return false;
	UObject* const CDO = BP->GeneratedClass->GetDefaultObject();
	if (!TestNotNull(TEXT("D2 the compiled Blueprint has a CDO"), CDO)) return false;

	{
		float RateValue = 0.0f;
		if (FFloatProperty* RateProp = FindFProperty<FFloatProperty>(BP->GeneratedClass, TEXT("SpecRate")))
		{
			RateValue = RateProp->GetPropertyValue_InContainer(CDO);
		}
		else
		{
			AddError(TEXT("D2 float variable 'SpecRate' was not created"));
		}
		// THE GATE. Before this spec existed, a requested default was silently dropped.
		TestEqual(TEXT("D2 the requested float default reached the CDO"), RateValue, 0.25f);
	}

	{
		bool bActiveValue = false;
		if (FBoolProperty* ActiveProp = FindFProperty<FBoolProperty>(BP->GeneratedClass, TEXT("bSpecActive")))
		{
			bActiveValue = ActiveProp->GetPropertyValue_InContainer(CDO);
		}
		else
		{
			AddError(TEXT("D2 bool variable 'bSpecActive' was not created"));
		}
		// Deliberately `true`: false is a bool's zero value, so a `false` default would pass even
		// if the default were never applied at all.
		TestTrue(TEXT("D2 the requested bool default reached the CDO"), bActiveValue);
	}

	// ---- half 2: the capability is discoverable ------------------------------------------
	// A working-but-undeclared field is what produced this defect in the first place: the model
	// cannot emit a field the schema never mentions.
	const FString ToolsJson = NWIRO_SPEC_SCHEMA::GetEnabledToolDefinitionsJson();
	TArray<TSharedPtr<FJsonValue>> Tools;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolsJson);
	if (!TestTrue(TEXT("tool definitions parse as a JSON array"),
		FJsonSerializer::Deserialize(Reader, Tools)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> EditBlueprintTool;
	for (const TSharedPtr<FJsonValue>& Value : Tools)
	{
		const TSharedPtr<FJsonObject> Tool = Value->AsObject();
		if (Tool.IsValid() && Tool->GetStringField(TEXT("name")) == TEXT("edit_blueprint"))
		{
			EditBlueprintTool = Tool;
			break;
		}
	}
	if (!TestTrue(TEXT("the enabled tool list contains edit_blueprint"), EditBlueprintTool.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* InputSchema = nullptr;
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	const TSharedPtr<FJsonObject>* AddVariables = nullptr;
	if (!TestTrue(TEXT("edit_blueprint declares an inputSchema"),
			EditBlueprintTool->TryGetObjectField(TEXT("inputSchema"), InputSchema))
		|| !TestTrue(TEXT("inputSchema declares properties"),
			(*InputSchema)->TryGetObjectField(TEXT("properties"), Properties))
		|| !TestTrue(TEXT("properties declares add_variables"),
			(*Properties)->TryGetObjectField(TEXT("add_variables"), AddVariables)))
	{
		return false;
	}

	// oneOf[array].items.properties — same shape as add_nodes.
	TSharedPtr<FJsonObject> ArrayArm = *AddVariables;
	const TArray<TSharedPtr<FJsonValue>>* OneOf = nullptr;
	if ((*AddVariables)->TryGetArrayField(TEXT("oneOf"), OneOf))
	{
		for (const TSharedPtr<FJsonValue>& Arm : *OneOf)
		{
			const TSharedPtr<FJsonObject> ArmObj = Arm->AsObject();
			if (ArmObj.IsValid() && ArmObj->GetStringField(TEXT("type")) == TEXT("array"))
			{
				ArrayArm = ArmObj;
				break;
			}
		}
	}

	const TSharedPtr<FJsonObject>* Items = nullptr;
	const TSharedPtr<FJsonObject>* ItemProperties = nullptr;
	if (!TestTrue(TEXT("the add_variables array arm declares items"),
			ArrayArm->TryGetObjectField(TEXT("items"), Items))
		|| !TestTrue(TEXT("add_variables items declare properties"),
			(*Items)->TryGetObjectField(TEXT("properties"), ItemProperties)))
	{
		return false;
	}

	// THE GATE. Only the canonical spelling is required; `defaultValue` / `initialValue` stay
	// accepted by the reader but deliberately unadvertised, so the schema offers one way to do it.
	TestTrue(TEXT("D3 add_variables items declare the \"default\" property"),
		(*ItemProperties)->HasField(TEXT("default")));

	return true;
}


// ---------------------------------------------------------------------------
// SPEC 6 — the makeRoot REFUSAL path.
//
// Not one of the three reported defects: this covers a risk the D2/F3 fix introduced. Guarding
// makeRoot with a scene-component check creates a brand-new code path, and a guard is only as
// good as what it leaves behind — an early return placed one line too late still mutates.
//
// Refusing the WHOLE item is the deliberate choice. The alternative (create the component, drop
// only the makeRoot part) breaks the remedy the error message itself suggests: re-sending with a
// SceneComponent class would collide with a component the caller was never told had been created.
// E6 is what pins that reasoning down — it is the recovery the message promises.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsRootRefusalSpec,
	NWIRO_SPEC_PREFIX "Components.MakeRootRefusesNonSceneComponent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsRootRefusalSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroRootRefusalSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!TestNotNull(TEXT("Blueprint has a SimpleConstructionScript"), SCS)) return false;

	// Seed a hierarchy worth protecting: a named root with a child under it. A refusal that
	// silently flattened the tree would pass a bare "the bad component was not created" check.
	const FString Seed = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":["
		"{\"name\":\"SpecBase\",\"class\":\"StaticMeshComponent\",\"makeRoot\":true},"
		"{\"name\":\"SpecArm\",\"class\":\"SceneComponent\",\"parent\":\"SpecBase\"}"
		"]}"), *Path);
	NWIRO_SPEC_TOOLS::EditBlueprint(Seed);
	FKismetEditorUtilities::CompileBlueprint(BP);

	// Baseline asserted, not assumed. If the seed did not build the hierarchy, "the hierarchy
	// survived the refusal" would be measuring a hierarchy that never existed.
	USCS_Node* const BaseNode = NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecBase"));
	if (!TestNotNull(TEXT("baseline: the seeded root exists"), BaseNode)) return false;
	if (!TestEqual(TEXT("baseline: exactly one root after seeding"), SCS->GetRootNodes().Num(), 1))
	{
		return false;
	}
	if (!TestTrue(TEXT("baseline: the seeded child is attached under the root"),
			NwiroBPSpec::IsChildOfNode(BaseNode, TEXT("SpecArm"))))
	{
		return false;
	}

	// Act: ask for a non-scene component AS THE ROOT. Impossible by construction — a root IS a
	// USceneComponent — so the only question is how the tool says no.
	//
	// RotatingMovementComponent, not the obvious "ActorComponent": UActorComponent is declared
	// UCLASS(abstract), and the tool constructs the component template BEFORE this guard runs,
	// so an abstract class trips a handled engine ensure that fails the spec for a reason that
	// has nothing to do with makeRoot. A concrete non-scene component isolates the guard.
	//
	// A MIXED batch, on purpose. With ONLY the refused item this spec passes for the wrong
	// reason: DoAddComponents also fails when nothing was added at all, so E1 would be green
	// even with the refusal counter deleted — a mutation run proved exactly that. The valid
	// sibling drives Added > 0, which is the case the old code reported as success.
	const FString Act = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":["
		"{\"name\":\"SpecExtra\",\"class\":\"SceneComponent\",\"parent\":\"SpecBase\"},"
		"{\"name\":\"BadRoot\",\"class\":\"RotatingMovementComponent\",\"makeRoot\":true}"
		"]}"), *Path);
	const FString Result = NWIRO_SPEC_TOOLS::EditBlueprint(Act);
	FKismetEditorUtilities::CompileBlueprint(BP);

	// E1 — THE GATE. This is the whole defect class the change set exists to close: a request the
	// tool did not carry out must not come back as success. Before the refusal counter was wired
	// in, DoAddComponents returned Ok whenever anything at all had been added.
	TestFalse(TEXT("E1 a non-scene makeRoot request is reported as a failure"),
		NwiroBPSpec::EnvelopeSuccess(Result));

	// E2 — a batch failure the caller cannot localise is not actionable. Asserted on the raw
	// envelope text rather than a parsed field so it holds whichever field carries the message.
	TestTrue(TEXT("E2 the failure names the item that was refused"),
		Result.Contains(TEXT("BadRoot")));

	// E3 — THE GATE. "Refused" means nothing was created, not "created but not rooted".
	TestNull(TEXT("E3 no component was created for the refused item"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("BadRoot")));

	// E3b — the valid sibling in the same batch is KEPT. Same partial-success contract as
	// SPEC 7: a refusal sinks the envelope without rolling back work that did succeed.
	TestNotNull(TEXT("E3b the valid component in the same batch was still created"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecExtra")));

	// E4/E5 — the guard must not have run after a partial mutation.
	TestEqual(TEXT("E4 the pre-existing root is untouched"), SCS->GetRootNodes().Num(), 1);
	if (SCS->GetRootNodes().Num() == 1)
	{
		TestEqual(TEXT("E4b the root is still the seeded component"),
			SCS->GetRootNodes()[0]->GetVariableName(), FName(TEXT("SpecBase")));
	}
	TestTrue(TEXT("E5 the pre-existing child is still attached to it"),
		NwiroBPSpec::IsChildOfNode(NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecBase")), TEXT("SpecArm")));

	// E6 — recovery, and the control that keeps E1..E5 honest. A pseudo-fix that refused EVERY
	// makeRoot after the first would satisfy all of them; this proves the re-rooting path is still
	// live. It is also the exact remedy E2's message tells the caller to use, so a name collision
	// left behind by the refusal would surface here as "BadRoot1" or an outright failure.
	const FString Recover = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":[{\"name\":\"BadRoot\",\"class\":\"SceneComponent\",\"makeRoot\":true}]"
		"}"), *Path);
	const FString RecoverResult = NWIRO_SPEC_TOOLS::EditBlueprint(Recover);
	FKismetEditorUtilities::CompileBlueprint(BP);

	TestTrue(TEXT("E6a re-sending the corrected item is accepted"),
		NwiroBPSpec::EnvelopeSuccess(RecoverResult));
	if (TestEqual(TEXT("E6b still exactly one root"), SCS->GetRootNodes().Num(), 1))
	{
		// The exact name, not a prefix match: a leftover from the refusal would have forced the
		// engine to uniquify this one to "BadRoot1".
		TestEqual(TEXT("E6c the corrected component became the root under its requested name"),
			SCS->GetRootNodes()[0]->GetVariableName(), FName(TEXT("BadRoot")));
		TestTrue(TEXT("E6d the previous root was re-homed under it"),
			NwiroBPSpec::IsChildOfNode(SCS->GetRootNodes()[0], TEXT("SpecBase")));
	}
	TestEqual(TEXT("E6e the Blueprint still compiles clean"),
		BP->Status, EBlueprintStatus::BS_UpToDate);

	// E7 — positive control, on its own fixture so it cannot disturb the root assertions above
	// (a non-scene component becomes an extra SCS root and would break E4's count).
	//
	// Without this, E1 and E3 would ALSO pass if "ActorComponent" simply failed to resolve as a
	// class — the spec would be proving that a typo is rejected, not that makeRoot is guarded.
	FString ControlPath;
	UBlueprint* ControlBP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroRootRefusalControl"), ControlPath);
	if (!TestNotNull(TEXT("control fixture Blueprint was created"), ControlBP)) return false;

	const FString ControlAct = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":[{\"name\":\"SpecLogic\",\"class\":\"RotatingMovementComponent\"}]"
		"}"), *ControlPath);
	const FString ControlResult = NWIRO_SPEC_TOOLS::EditBlueprint(ControlAct);
	FKismetEditorUtilities::CompileBlueprint(ControlBP);

	TestTrue(TEXT("E7a the same class IS accepted when makeRoot is not requested"),
		NwiroBPSpec::EnvelopeSuccess(ControlResult));
	TestNotNull(TEXT("E7b the non-scene component was actually created"),
		NwiroBPSpec::FindSCSNode(ControlBP->SimpleConstructionScript, TEXT("SpecLogic")));

	return true;
}

// ---------------------------------------------------------------------------
// SPEC 7 — pin-bearing node batches reject invalid peers before target mutation.
// A repeat must preserve the same empty graph and stable failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsPartialBatchSpec,
	NWIRO_SPEC_PREFIX "Nodes.InvalidBatchLeavesNoNodesAndIsSafeToResend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsPartialBatchSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroPartialBatchSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	const FString AddVar = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_variables\":[{\"name\":\"bSpecFlag\",\"type\":\"bool\"}]"
		"}"), *Path);
	NWIRO_SPEC_TOOLS::EditBlueprint(AddVar);
	FKismetEditorUtilities::CompileBlueprint(BP);

	UEdGraph* Graph = NwiroBPSpec::EventGraph(BP);
	if (!TestNotNull(TEXT("fixture has an event graph"), Graph)) return false;

	const int32 NodesBefore = Graph->Nodes.Num();

	// Refs are deliberately distinctive. The dedup path also does a NORMALISED comparison across
	// every entry in the process-wide NodeRefs map, so a generic ref like "get1" could match an
	// entry left by another spec and be skipped for a reason this spec never intended.
	const FString Batch = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"graph\":\"EventGraph\","
		"\"add_nodes\":["
		"{\"ref\":\"SpecPartialGood\",\"type\":\"VariableGet\",\"variable\":\"bSpecFlag\"},"
		"{\"ref\":\"SpecPartialBad\",\"type\":\"VariableGet\"}"
		"]}"), *Path);

	const FString First = NWIRO_SPEC_TOOLS::EditBlueprint(Batch);

	auto CountVariableGets = [Graph]() -> int32
	{
		int32 Count = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UK2Node_VariableGet>(Node)) Count++;
		}
		return Count;
	};

	// F1 — THE GATE. One rejected item is enough to sink the envelope, even though another item
	// in the same batch succeeded. "Mostly worked" is the reading that let all three original
	// defects ship.
	TestFalse(TEXT("F1 a batch with one rejected item does not report success"),
		NwiroBPSpec::EnvelopeSuccess(First));

	TestTrue(TEXT("F2 the failure names the item that was rejected"),
		First.Contains(TEXT("SpecPartialBad")));

	// F3/F4 — neither requested node reaches the target on admission failure.
	TestEqual(TEXT("F3 invalid peer prevents creating the valid node"), CountVariableGets(), 0);
	TestEqual(TEXT("F4 no node was added for the rejected two-item batch"),
		Graph->Nodes.Num(), NodesBefore);

	// Act again with the byte-identical payload — the caller's natural response to a failure.
	const FString Second = NWIRO_SPEC_TOOLS::EditBlueprint(Batch);

	// F5 — the failure must be stable. A second call reporting success would tell the caller the
	// batch had been repaired by re-sending it.
	TestFalse(TEXT("F5 re-sending the same batch still reports failure"),
		NwiroBPSpec::EnvelopeSuccess(Second));

	// F6/F7 — rejected staging refs cannot change a retry.
	TestEqual(TEXT("F6 re-sending the rejected batch still creates no node"), CountVariableGets(), 0);
	TestEqual(TEXT("F7 the graph node count is unchanged by the re-send"),
		Graph->Nodes.Num(), NodesBefore);

	return true;
}


// ---------------------------------------------------------------------------
// SPEC 8 — an abstract component class is refused before anything is built.
//
// The class-resolution loop only asks IsChildOf(UActorComponent). UActorComponent itself is
// declared UCLASS(..., abstract, ...), so a caller naming it passes that filter and reaches
// SimpleConstructionScript::CreateNode, which builds a template of a class the engine refuses
// to instantiate: "Class which was marked abstract was trying to be loaded in Outer
// /Engine/Transient. It will be nulled out on save."
//
// The assertions below deliberately do NOT look for that engine ensure. Its static latch lives
// in CoreUObject, which BOTH plugin modules share, so whichever product runs first consumes it
// and the second executes the identical bad path in silence — an oracle that reports the defect
// for one product and green for the other is worse than no oracle. Every gate here is observable
// tool behaviour instead: the envelope, the absence of a node, and the surviving hierarchy.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsAbstractClassSpec,
	NWIRO_SPEC_PREFIX "Components.AbstractClassIsRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsAbstractClassSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroAbstractClassSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!TestNotNull(TEXT("Blueprint has a SimpleConstructionScript"), SCS)) return false;

	const FString Seed = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":["
		"{\"name\":\"SpecBase\",\"class\":\"StaticMeshComponent\",\"makeRoot\":true},"
		"{\"name\":\"SpecArm\",\"class\":\"SceneComponent\",\"parent\":\"SpecBase\"}"
		"]}"), *Path);
	NWIRO_SPEC_TOOLS::EditBlueprint(Seed);
	FKismetEditorUtilities::CompileBlueprint(BP);

	USCS_Node* const BaseNode = NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecBase"));
	if (!TestNotNull(TEXT("baseline: the seeded root exists"), BaseNode)) return false;
	if (!TestEqual(TEXT("baseline: exactly one root after seeding"), SCS->GetRootNodes().Num(), 1))
	{
		return false;
	}
	if (!TestTrue(TEXT("baseline: the seeded child is attached under the root"),
			NwiroBPSpec::IsChildOfNode(BaseNode, TEXT("SpecArm"))))
	{
		return false;
	}

	// A MIXED batch, for the reason SPEC 6 records: with the refused item alone, DoAddComponents
	// already fails through its Added == 0 fallback and G1 would be green whether or not the
	// abstract class was ever inspected. The valid sibling drives Added > 0.
	const FString Act = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":["
		"{\"name\":\"SpecExtra\",\"class\":\"SceneComponent\",\"parent\":\"SpecBase\"},"
		"{\"name\":\"SpecAbstract\",\"class\":\"ActorComponent\"}"
		"]}"), *Path);
	const FString Result = NWIRO_SPEC_TOOLS::EditBlueprint(Act);

	FKismetEditorUtilities::CompileBlueprint(BP);

	// G1 — THE GATE. A class the engine cannot instantiate is a request the tool cannot carry
	// out, so the envelope must not claim success.
	TestFalse(TEXT("G1 a request naming an abstract component class is reported as a failure"),
		NwiroBPSpec::EnvelopeSuccess(Result));

	TestTrue(TEXT("G2 the failure names the item that was refused"),
		Result.Contains(TEXT("SpecAbstract")));

	// G3 — THE GATE for the refusal itself: no component reaches the Blueprint.
	//
	// It does NOT pin down WHERE the guard sits, and no assertion here does. Two mutation runs
	// went looking: a guard moved after CreateNode leaves nothing observable behind — no listed
	// node, and no orphan reachable from the SCS — so G3 stays green while the engine ensure
	// fires. That ensure is what actually catches the misplacement, because the automation
	// framework fails any test that logs an error. Corroborating evidence, not a gate: the
	// ensure latches once inside CoreUObject, which both plugin modules share, so if BOTH
	// products regressed only the product that runs first would report it.
	TestNull(TEXT("G3 no component was created for the abstract class"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecAbstract")));

	// G4 — the partial-success contract: refusing one item does not roll back a valid sibling.
	TestNotNull(TEXT("G4 the valid component in the same batch was still created"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecExtra")));

	// G5 — the refusal must not have happened after a partial mutation of the hierarchy.
	TestEqual(TEXT("G5 the pre-existing root is untouched"), SCS->GetRootNodes().Num(), 1);
	if (SCS->GetRootNodes().Num() == 1)
	{
		TestEqual(TEXT("G5b the root is still the seeded component"),
			SCS->GetRootNodes()[0]->GetVariableName(), FName(TEXT("SpecBase")));
	}
	TestTrue(TEXT("G5c the pre-existing child is still attached to it"),
		NwiroBPSpec::IsChildOfNode(NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecBase")), TEXT("SpecArm")));

	// G6 — recovery, and the positive control in one act. RotatingMovementComponent is a CONCRETE
	// descendant of the very class that was refused, so this proves the guard rejects abstractness
	// rather than rejecting non-scene components, an unresolvable name, or that item position.
	// Reusing the refused NAME is the point: a leftover from the refusal would force the engine to
	// uniquify this one to "SpecAbstract1".
	const FString Recover = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":[{\"name\":\"SpecAbstract\",\"class\":\"RotatingMovementComponent\"}]"
		"}"), *Path);
	const FString RecoverResult = NWIRO_SPEC_TOOLS::EditBlueprint(Recover);
	FKismetEditorUtilities::CompileBlueprint(BP);

	TestTrue(TEXT("G6a a concrete descendant of the refused class IS accepted"),
		NwiroBPSpec::EnvelopeSuccess(RecoverResult));
	TestNotNull(TEXT("G6b it was created under the exact name the refusal left free"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecAbstract")));

	TestEqual(TEXT("G7 the Blueprint still compiles clean"),
		BP->Status, EBlueprintStatus::BS_UpToDate);

	return true;
}


// ---------------------------------------------------------------------------
// SPEC 9 — a class name that resolves to nothing is refused the same way.
//
// Added after a review pointed out that the branch three lines above the abstract guard had the
// identical defect and had simply never been noticed: it recorded an error and fell through to
// the older `Added > 0 || Errors.Num() == 0` return, so a batch naming one real component and
// one class that does not exist came back as success. Nothing in the code marked that as
// deliberate — the branch predates the refusal counter entirely.
//
// Kept as its own spec rather than a third item in SPEC 8's batch: two refusals for different
// reasons in one call cannot show WHICH one sank the envelope.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroBPToolsUnresolvedClassSpec,
	NWIRO_SPEC_PREFIX "Components.UnresolvedClassIsRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroBPToolsUnresolvedClassSpec::RunTest(const FString& Parameters)
{
	FString Path;
	UBlueprint* BP = NwiroBPSpec::MakeActorBP(TEXT("BP_NwiroUnresolvedClassSpec"), Path);
	if (!TestNotNull(TEXT("fixture Blueprint was created"), BP)) return false;

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!TestNotNull(TEXT("Blueprint has a SimpleConstructionScript"), SCS)) return false;

	// Mixed, for the reason SPEC 6's M1 run established: with only the bad item, Added == 0 makes
	// the old fallback fail anyway and H1 would pass without the counter being consulted at all.
	//
	// The bogus name is deliberately unlike any real class. The resolver tries four spellings
	// (X, UX, XComponent, UXComponent), so a plausible-looking stem risks accidentally resolving.
	const FString Act = FString::Printf(TEXT(
		"{"
		"\"blueprint\":\"%s\","
		"\"add_components\":["
		"{\"name\":\"SpecReal\",\"class\":\"SceneComponent\",\"properties\":{\"nwiro_merge_unknown_property\":\"ignored\"}},"
		"{\"name\":\"SpecBogus\",\"class\":\"NwiroNoSuchThingXyzzy\"}"
		"]}"), *Path);
	const FString Result = NWIRO_SPEC_TOOLS::EditBlueprint(Act);
	FKismetEditorUtilities::CompileBlueprint(BP);

	// H1 — THE GATE. The whole point of the review finding: this used to be success.
	TestFalse(TEXT("H1 a batch naming an unresolvable class is reported as a failure"),
		NwiroBPSpec::EnvelopeSuccess(Result));

	TestTrue(TEXT("H2 the failure names the item that could not be resolved"),
		Result.Contains(TEXT("NwiroNoSuchThingXyzzy")));

	TestNull(TEXT("H3 no component was created for the unresolvable class"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecBogus")));

	// The valid sibling also reports an ignored property. A failed batch must retain
	// that machine-readable warning, not only the free-text message or the failure flag.
	TSharedPtr<FJsonObject> Envelope;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result);
	const bool bParsed = FJsonSerializer::Deserialize(Reader, Envelope) && Envelope.IsValid();
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	bool bHasPropertyWarning = false;
	if (bParsed && Envelope->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
	{
		for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
		{
			FString Text;
			if (Warning.IsValid() && Warning->TryGetString(Text)
				&& Text.Contains(TEXT("nwiro_merge_unknown_property"))
				&& Text.Contains(TEXT("SpecReal")))
			{
				bHasPropertyWarning = true;
			}
		}
	}
	TestTrue(TEXT("H3b rejected batch retains its machine-readable property warning"),
		bHasPropertyWarning);

	// H4 — positive control AND the partial-success contract in one: the valid sibling is kept,
	// which also proves H1 is not simply "the whole batch was thrown away".
	TestNotNull(TEXT("H4 the valid component in the same batch was still created"),
		NwiroBPSpec::FindSCSNode(SCS, TEXT("SpecReal")));

	TestEqual(TEXT("H5 the Blueprint still compiles clean"),
		BP->Status, EBlueprintStatus::BS_UpToDate);

	return true;
}

// ---------------------------------------------------------------------------
// SPEC P1 — create_blueprint must refuse a package path that is not a mounted, /-rooted long
// package name BEFORE the object system sees it.
//
// Liveness scan run 3 (NS3-1): a Windows path reached the asset registry's path tree and killed
// the editor on check(PathView[0] == '/') (PathTree.cpp:39), 36 times. Two siblings share the
// door: '//' is a Fatal in CreatePackage (UObjectGlobals.cpp:1096), and a joined package name of
// NAME_SIZE characters or more is a checkf in FName (UnrealNames.cpp:3280) — the per-field cap
// (path <= 1024) never sees the joined name.
//
// On the unfixed code every refusal case is a process abort, so this is a complex test: each case
// has its own name and can be run alone ("...P_CreateBlueprintPath.<Case>"). Refusals assert the
// envelope AND engine state (no package carrying the case's marker exists). One refusal enters
// through edit_blueprint's create sub-op, the second route to the same function. The accepted case
// is the compatibility control: content paths that worked before the guard must still work.
// ---------------------------------------------------------------------------
IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FNwiroBPToolsCreateBlueprintPathSpec,
	NWIRO_SPEC_PREFIX "P_CreateBlueprintPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

void FNwiroBPToolsCreateBlueprintPathSpec::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const TCHAR* Case : { TEXT("RefusesWindowsDrivePath"), TEXT("RefusesDoubleSlash"),
		TEXT("RefusesPathWithoutLeadingSlash"), TEXT("RefusesJoinedNameAtNameSize"),
		TEXT("RefusesUnmountedRoot"), TEXT("RefusesWindowsPathThroughEditBlueprint"),
		TEXT("AcceptsContentPaths") })
	{
		OutBeautifiedNames.Add(Case);
		OutTestCommands.Add(Case);
	}
}

bool FNwiroBPToolsCreateBlueprintPathSpec::RunTest(const FString& Case)
{
	const FString Pkg = TEXT(NWIRO_SPEC_PKG);

	auto Create = [](const FString& Path, const FString& Name)
	{
		const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("name"), Name);
		Args->SetStringField(TEXT("path"), Path);
		FString Json;
		FJsonSerializer::Serialize(Args, TJsonWriterFactory<>::Create(&Json));
		return NWIRO_SPEC_TOOLS::CreateBlueprint(Json);
	};

	// Looked up without handing the malformed string to the object system: FindPackage on these
	// inputs walks the same resolver the defect crashes in. Matches are named in the report, so a
	// failure says which package it found.
	auto PackageExists = [this](const FString& Marker)
	{
		bool bFound = false;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName().Contains(Marker))
			{
				AddInfo(FString::Printf(TEXT("package carrying '%s': %s"), *Marker, *It->GetName()));
				bFound = true;
			}
		}
		return bFound;
	};

	auto ExpectRefused = [&](const FString& Path, const FString& Name)
	{
		const FString Result = Create(Path, Name);
		TestFalse(TEXT("the call is reported as a failure"), NwiroBPSpec::EnvelopeSuccess(Result));
		TestTrue(TEXT("the failure carries an error message"), Result.Contains(TEXT("\"error\"")));
		TestFalse(TEXT("no package was created"), PackageExists(Name));
		return true;
	};

	if (Case == TEXT("RefusesWindowsDrivePath"))
	{
		return ExpectRefused(TEXT("C:/Temp/NwiroCreatePathSpec"), TEXT("BP_NwiroCreatePath_Drive"));
	}
	if (Case == TEXT("RefusesDoubleSlash"))
	{
		return ExpectRefused(Pkg + TEXT("/CreatePath"), TEXT("BP_NwiroCreatePath_DoubleSlash"));
	}
	if (Case == TEXT("RefusesPathWithoutLeadingSlash"))
	{
		return ExpectRefused(TEXT("Game/NwiroCreatePathSpec"), TEXT("BP_NwiroCreatePath_Relative"));
	}
	if (Case == TEXT("RefusesJoinedNameAtNameSize"))
	{
		// Exactly at the per-field cap, so the existing length check lets it through and only the
		// joined package name crosses NAME_SIZE.
		const FString LongPath = Pkg + FString::ChrN(1024 - Pkg.Len(), TEXT('A'));
		TestEqual(TEXT("the path sits exactly at the per-field cap"), LongPath.Len(), 1024);
		return ExpectRefused(LongPath, TEXT("BP_NwiroCreatePath_Long"));
	}
	if (Case == TEXT("RefusesUnmountedRoot"))
	{
		return ExpectRefused(TEXT("/NwiroNoSuchMount/CreatePath"), TEXT("BP_NwiroCreatePath_Unmounted"));
	}
	if (Case == TEXT("RefusesWindowsPathThroughEditBlueprint"))
	{
		// The second route in: edit_blueprint's create sub-op formats the path into JSON and hands it
		// to CreateBlueprint, so the same guard has to hold when the call arrives this way.
		const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("action"), TEXT("CreateBlueprint"));
		Args->SetStringField(TEXT("name"), TEXT("BP_NwiroCreatePath_EditRoute"));
		Args->SetStringField(TEXT("parentClass"), TEXT("Actor"));
		Args->SetStringField(TEXT("path"), TEXT("C:/Temp/NwiroCreatePathSpec"));
		FString Json;
		FJsonSerializer::Serialize(Args, TJsonWriterFactory<>::Create(&Json));
		const FString Result = NWIRO_SPEC_TOOLS::EditBlueprint(Json);
		TestFalse(TEXT("the call is reported as a failure"), NwiroBPSpec::EnvelopeSuccess(Result));
		TestTrue(TEXT("the failure carries an error message"), Result.Contains(TEXT("\"error\"")));
		// The sub-op then looks the bare name up (LoadBP), and that failed load leaves an empty package
		// at /Game/BP_NwiroCreatePath_EditRoute — the known LoadObject side effect, at a valid root.
		// The defect is a package at the malformed path, so the marker here is the malformed folder.
		TestFalse(TEXT("no package was created under the malformed path"), PackageExists(TEXT("NwiroCreatePathSpec")));
		return true;
	}
	if (Case == TEXT("AcceptsContentPaths"))
	{
		// A plain folder, a trailing slash (FString's path join does not double it) and the empty
		// path that defaults to /Game. All three produced a working Blueprint before the guard.
		const struct { FString Path, Name, Object; } Accepted[] = {
			{ Pkg + TEXT("CreatePath"), TEXT("BP_NwiroCreatePath_Ok"),
				Pkg + TEXT("CreatePath/BP_NwiroCreatePath_Ok.BP_NwiroCreatePath_Ok") },
			{ Pkg + TEXT("CreatePath/"), TEXT("BP_NwiroCreatePath_Trailing"),
				Pkg + TEXT("CreatePath/BP_NwiroCreatePath_Trailing.BP_NwiroCreatePath_Trailing") },
			{ FString(), TEXT("BP_NwiroCreatePath_Default"),
				TEXT("/Game/BP_NwiroCreatePath_Default.BP_NwiroCreatePath_Default") },
		};
		for (const auto& A : Accepted)
		{
			const FString Result = Create(A.Path, A.Name);
			TestTrue(*FString::Printf(TEXT("'%s' is accepted"), *A.Path), NwiroBPSpec::EnvelopeSuccess(Result));
			TestNotNull(*FString::Printf(TEXT("%s exists"), *A.Object), LoadObject<UBlueprint>(nullptr, *A.Object));
		}
		return true;
	}

	AddError(FString::Printf(TEXT("unknown case '%s'"), *Case));
	return false;
}

#endif // WITH_EDITOR
