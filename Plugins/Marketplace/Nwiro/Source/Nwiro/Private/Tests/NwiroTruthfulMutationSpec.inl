// Copyright 2026 Nwiro. All Rights Reserved.
// Included by EnvironmentTools.cpp so the same validated setter batch is exercised.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Editor/Transactor.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "NwiroAITools.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSkyMutationSpec,
	"Nwiro.Pro.Environment.SkyAtmosphere.ValidatedMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroSkyMutationSpec::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("test owns an idle editor transaction"), GEditor && !GEditor->PlayWorld && !GEditor->IsTransactionActive() && !GIsTransacting)) return false;
	UPackage* Package = CreatePackage(*(TEXT("/Game/NwiroSkyMutation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<USkyAtmosphereComponent> Sky(NewObject<USkyAtmosphereComponent>(Package, NAME_None, RF_Transactional));
	ON_SCOPE_EXIT { Sky->MarkAsGarbage(); Package->SetDirtyFlag(false); Package->MarkAsGarbage(); };
	const float DepthBefore = Sky->AerialPerspectiveStartDepth;
	const float ElevationBefore = Sky->TransmittanceMinLightElevationAngle;
	const float RadiusBefore = Sky->BottomRadius;
	const float ScaleBefore = Sky->RayleighScatteringScale;
	const FColor AlbedoBefore = Sky->GroundAlbedo;
	const FLinearColor RayleighBefore = Sky->RayleighScattering;
	const auto CallProperties = [&](const TSharedPtr<FJsonObject>& Parsed, bool Expected)
	{
		TSharedPtr<FJsonObject> Result;
		const FString Raw = NwiroSkyAtmosphere::Apply(Sky.Get(), Parsed);
		if (!TestTrue(TEXT("sky response is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result) && Result.IsValid())) return Result;
		bool Success = !Expected;
		TestTrue(TEXT("sky response has success"), Result->TryGetBoolField(TEXT("success"), Success));
		TestEqual(TEXT("sky outcome matches request validity"), Success, Expected);
		if (Success != Expected) AddError(Raw);
		return Result;
	};
	const auto Call = [&](const TCHAR* Properties, bool Expected)
	{
		TSharedPtr<FJsonObject> Parsed;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Properties), Parsed)) { AddError(TEXT("Invalid test fixture JSON")); return TSharedPtr<FJsonObject>(); }
		return CallProperties(Parsed, Expected);
	};
	const auto CheckNoTouch = [&](bool WasDirty, const FGuid& UndoBefore)
	{
		TestEqual(TEXT("rejected batch preserves aerial depth"), Sky->AerialPerspectiveStartDepth, DepthBefore);
		TestEqual(TEXT("rejected batch preserves elevation"), Sky->TransmittanceMinLightElevationAngle, ElevationBefore);
		TestEqual(TEXT("rejected batch preserves radius"), Sky->BottomRadius, RadiusBefore);
		TestEqual(TEXT("rejected batch preserves scale"), Sky->RayleighScatteringScale, ScaleBefore);
		TestEqual(TEXT("rejected batch preserves byte color"), Sky->GroundAlbedo, AlbedoBefore);
		TestEqual(TEXT("rejected batch preserves linear color"), Sky->RayleighScattering, RayleighBefore);
		TestEqual(TEXT("rejected batch preserves prior dirty flag"), Package->IsDirty(), WasDirty);
		TestEqual(TEXT("rejected batch creates no undo step"), GEditor->Trans->GetUndoContext().TransactionId, UndoBefore);
	};
	const TCHAR* Invalid[] = {
		TEXT("{}"), TEXT("{\"BottomRadius\":\"6500\"}"), TEXT("{\"BottomRadius\":null}"),
		TEXT("{\"AerialPerspectiveStartDepth\":0}"), TEXT("{\"AerialPerspectiveStartDepth\":\"2\"}"),
		TEXT("{\"TransmittanceMinLightElevationAngle\":-90.01}"), TEXT("{\"TransmittanceMinLightElevationAngle\":90.01}"),
		TEXT("{\"AerialPerspectiveStartDepth\":25,\"TransmittanceMinLightElevationAngle\":91}"),
		TEXT("{\"TransmittanceMinLightElevationAngle\":91,\"AerialPerspectiveStartDepth\":25}"),
		TEXT("{\"TransmittanceMinLightElevationAngle\":-20,\"AerialPerspectiveStartDepth\":0}"),
		TEXT("{\"AerialPerspectiveStartDepth\":0,\"TransmittanceMinLightElevationAngle\":-20}"),
		TEXT("{\"BottomRadius\":0}"), TEXT("{\"BottomRadius\":10001}"),
		TEXT("{\"BottomRadius\":6500,\"UnknownSetting\":1}"),
		TEXT("{\"BottomRadius\":6500,\"MieAnisotropy\":1}"),
		TEXT("{\"BottomRadius\":6500,\"RayleighScattering\":{\"r\":0.1,\"g\":0.2}}"),
		TEXT("{\"BottomRadius\":6500,\"RayleighScattering\":{\"r\":0.1,\"g\":0.2,\"b\":-1}}"),
		TEXT("{\"BottomRadius\":6500,\"RayleighScattering\":{\"r\":0.1,\"g\":0.2,\"b\":1e39}}"),
		TEXT("{\"BottomRadius\":6500,\"GroundAlbedo\":{\"r\":20.5,\"g\":80,\"b\":40}}"),
		TEXT("{\"BottomRadius\":6500,\"GroundAlbedo\":{\"r\":256,\"g\":80,\"b\":40}}"),
		TEXT("{\"BottomRadius\":6500,\"GroundAlbedo\":{\"r\":20,\"g\":80,\"b\":40,\"a\":255}}")
	};
	for (bool WasDirty : { false, true })
	{
		Package->SetDirtyFlag(WasDirty);
		for (const TCHAR* Bad : Invalid)
		{
			const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
			Call(Bad, false);
			CheckNoTouch(WasDirty, UndoBefore);
		}
		// JSON cannot encode NaN/Infinity, but the validated batch must reject
		// nonfinite FJson numbers too, before either scalar setter is invoked.
		for (double InvalidNumber : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() })
		{
			for (bool DepthIsInvalid : { false, true })
			{
				for (bool InvalidFirst : { false, true })
				{
					TSharedRef<FJsonObject> Mixed = MakeShared<FJsonObject>();
					const TCHAR* InvalidKey = DepthIsInvalid ? TEXT("AerialPerspectiveStartDepth") : TEXT("TransmittanceMinLightElevationAngle");
					const TCHAR* ValidKey = DepthIsInvalid ? TEXT("TransmittanceMinLightElevationAngle") : TEXT("AerialPerspectiveStartDepth");
					if (InvalidFirst) Mixed->SetNumberField(InvalidKey, InvalidNumber);
					Mixed->SetNumberField(ValidKey, 25.0);
					if (!InvalidFirst) Mixed->SetNumberField(InvalidKey, InvalidNumber);
					const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
					CallProperties(Mixed, false);
					CheckNoTouch(WasDirty, UndoBefore);
				}
			}
		}
	}
	Package->SetDirtyFlag(false);
	const float HeightBefore = Sky->AtmosphereHeight;
	const auto Result = Call(TEXT("{\"AerialPerspectiveStartDepth\":25,\"TransmittanceMinLightElevationAngle\":-20,\"BottomRadius\":6500,\"RayleighScatteringScale\":0.5,\"GroundAlbedo\":{\"r\":20,\"g\":80,\"b\":40},\"RayleighScattering\":{\"r\":0.25,\"g\":0.5,\"b\":0.75}}"), true);
	TestEqual(TEXT("aerial depth changed"), Sky->AerialPerspectiveStartDepth, 25.0f);
	TestEqual(TEXT("elevation changed"), Sky->TransmittanceMinLightElevationAngle, -20.0f);
	TestEqual(TEXT("native radius changed"), Sky->BottomRadius, 6500.0f);
	TestEqual(TEXT("native scale changed"), Sky->RayleighScatteringScale, 0.5f);
	TestEqual(TEXT("native byte channels changed"), Sky->GroundAlbedo, FColor(20, 80, 40, AlbedoBefore.A));
	TestEqual(TEXT("native linear channels changed"), Sky->RayleighScattering, FLinearColor(0.25f, 0.5f, 0.75f, RayleighBefore.A));
	TestEqual(TEXT("unrequested property is retained"), Sky->AtmosphereHeight, HeightBefore);
	TestTrue(TEXT("valid batch marks package dirty"), Package->IsDirty());
	if (Result)
	{
		bool Saved = true;
		TestTrue(TEXT("result explicitly reports persistence"), Result->TryGetBoolField(TEXT("savedToDisk"), Saved));
		TestFalse(TEXT("setter does not claim a save"), Saved);
		TestEqual(TEXT("reported aerial depth is readback"), Result->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("AerialPerspectiveStartDepth")), static_cast<double>(Sky->AerialPerspectiveStartDepth));
		TestEqual(TEXT("reported elevation is readback"), Result->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("TransmittanceMinLightElevationAngle")), static_cast<double>(Sky->TransmittanceMinLightElevationAngle));
		TestEqual(TEXT("reported radius is native readback"), Result->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("BottomRadius")), static_cast<double>(Sky->BottomRadius));
	}
	const FTransactionContext Applied = GEditor->Trans->GetUndoContext();
	if (TestTrue(TEXT("sky owns undo transaction"), Applied.TransactionId.IsValid() && Applied.Title.EqualTo(NSLOCTEXT("Nwiro", "SetSkyAtmosphere", "AI: Set Sky Atmosphere"))))
	{
		if (TestTrue(TEXT("sky mutation can undo"), GEditor->UndoTransaction()))
		{
			TestEqual(TEXT("undo restores aerial depth"), Sky->AerialPerspectiveStartDepth, DepthBefore);
			TestEqual(TEXT("undo restores elevation"), Sky->TransmittanceMinLightElevationAngle, ElevationBefore);
			TestEqual(TEXT("undo restores radius"), Sky->BottomRadius, RadiusBefore);
			TestEqual(TEXT("undo restores linear color"), Sky->RayleighScattering, RayleighBefore);
			if (TestTrue(TEXT("redo belongs to sky mutation"), GEditor->Trans->GetRedoContext().TransactionId == Applied.TransactionId) && TestTrue(TEXT("sky mutation can redo"), GEditor->RedoTransaction()))
			{
				TestEqual(TEXT("redo restores aerial depth"), Sky->AerialPerspectiveStartDepth, 25.0f);
				TestEqual(TEXT("redo restores elevation"), Sky->TransmittanceMinLightElevationAngle, -20.0f);
				TestEqual(TEXT("redo restores radius"), Sky->BottomRadius, 6500.0f);
			}
		}
	}
	Call(TEXT("{\"AerialPerspectiveStartDepth\":0.001,\"TransmittanceMinLightElevationAngle\":-90}"), true);
	TestEqual(TEXT("aerial depth accepts minimum"), Sky->AerialPerspectiveStartDepth, 0.001f);
	TestEqual(TEXT("elevation accepts minimum"), Sky->TransmittanceMinLightElevationAngle, -90.0f);
	TSharedRef<FJsonObject> UpperBounds = MakeShared<FJsonObject>();
	UpperBounds->SetNumberField(TEXT("AerialPerspectiveStartDepth"), static_cast<double>(MAX_flt));
	UpperBounds->SetNumberField(TEXT("TransmittanceMinLightElevationAngle"), 90.0);
	CallProperties(UpperBounds, true);
	TestEqual(TEXT("aerial depth accepts maximum finite float"), Sky->AerialPerspectiveStartDepth, MAX_flt);
	TestEqual(TEXT("elevation accepts maximum"), Sky->TransmittanceMinLightElevationAngle, 90.0f);
	return true;
}


namespace NwiroSkyMutationTests
{
static bool RegisteredMutation(FAutomationTestBase& Test, bool CheckRenderInvalidation)
{
	if (!Test.TestTrue(TEXT("test owns an idle editor transaction"), GEditor && !GEditor->PlayWorld && !GEditor->IsTransactionActive() && !GIsTransacting)) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false,
		FName(*(TEXT("NwiroSkyRegistered_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	if (!Test.TestNotNull(TEXT("isolated editor world exists"), World)) return false;
	ON_SCOPE_EXIT { World->GetOutermost()->SetDirtyFlag(false); World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!Test.TestNotNull(TEXT("owned sky actor exists"), Owner)) return false;
	USkyAtmosphereComponent* Sky = NewObject<USkyAtmosphereComponent>(Owner, NAME_None, RF_Transactional);
	Owner->AddInstanceComponent(Sky);
	Owner->SetRootComponent(Sky);
	Sky->SetMobility(EComponentMobility::Movable);
	Sky->RegisterComponent();
	if (!Test.TestTrue(TEXT("movable sky is registered"), Sky->IsRegistered())) return false;
	if (CheckRenderInvalidation && !Test.TestTrue(TEXT("render test has a real created render state"), Sky->IsRenderStateCreated())) return false;
	const auto Apply = [&](const TSharedPtr<FJsonObject>& Properties, bool Expected)
	{
		TSharedPtr<FJsonObject> Result;
		const FString Raw = NwiroSkyAtmosphere::Apply(Sky, Properties);
		if (!Test.TestTrue(TEXT("registered response is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result) && Result.IsValid())) return;
		Test.TestEqual(TEXT("registered mutation outcome"), Result->GetBoolField(TEXT("success")), Expected);
	};
	struct FScalarCase { const TCHAR* Name; float USkyAtmosphereComponent::*Member; float Value; };
	const FScalarCase Cases[] = {
		{ TEXT("AerialPerspectiveStartDepth"), &USkyAtmosphereComponent::AerialPerspectiveStartDepth, 20.0f },
		{ TEXT("TransmittanceMinLightElevationAngle"), &USkyAtmosphereComponent::TransmittanceMinLightElevationAngle, -20.0f }
	};
	for (const FScalarCase& Case : Cases)
	{
		if (CheckRenderInvalidation) Sky->RecreateRenderState_Concurrent();
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(Case.Name, Case.Value);
		Apply(Properties, true);
		Test.TestEqual(TEXT("registered movable scalar changes"), Sky->*(Case.Member), Case.Value);
		if (CheckRenderInvalidation)
		{
			Test.TestTrue(TEXT("changed compatibility scalar invalidates render state"), Sky->IsRenderStateDirty());
			Sky->RecreateRenderState_Concurrent();
			Apply(Properties, true);
			Test.TestFalse(TEXT("unchanged compatibility scalar does not invalidate render state"), Sky->IsRenderStateDirty());
		}
	}
	Sky->SetMobility(EComponentMobility::Static);
	const float DepthBefore = Sky->AerialPerspectiveStartDepth;
	const float ElevationBefore = Sky->TransmittanceMinLightElevationAngle;
	for (bool WasDirty : { false, true })
	{
		World->GetOutermost()->SetDirtyFlag(WasDirty);
		const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
		const bool RenderDirtyBefore = Sky->IsRenderStateDirty();
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("AerialPerspectiveStartDepth"), 30.0);
		Properties->SetNumberField(TEXT("TransmittanceMinLightElevationAngle"), 30.0);
		Apply(Properties, false);
		Test.TestEqual(TEXT("static rejection preserves aerial depth"), Sky->AerialPerspectiveStartDepth, DepthBefore);
		Test.TestEqual(TEXT("static rejection preserves elevation"), Sky->TransmittanceMinLightElevationAngle, ElevationBefore);
		Test.TestEqual(TEXT("static rejection preserves dirty state"), World->GetOutermost()->IsDirty(), WasDirty);
		Test.TestEqual(TEXT("static rejection preserves render state"), Sky->IsRenderStateDirty(), RenderDirtyBefore);
		Test.TestEqual(TEXT("static rejection creates no undo step"), GEditor->Trans->GetUndoContext().TransactionId, UndoBefore);
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSkyRegisteredMutationSpec,
	"Nwiro.Pro.Environment.SkyAtmosphere.RegisteredMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSkyRegisteredMutationSpec::RunTest(const FString& Parameters)
{
	return NwiroSkyMutationTests::RegisteredMutation(*this, false);
}

// UE 5.6+ owns render/state-stream notifications in its native setters.
// This non-null-RHI test verifies the UE 5.5 compatibility path specifically.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 6
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSkyCompatibilityRenderSpec,
	"Nwiro.Pro.Environment.SkyAtmosphere.CompatibilityRenderInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FNwiroSkyCompatibilityRenderSpec::RunTest(const FString& Parameters)
{
	return NwiroSkyMutationTests::RegisteredMutation(*this, true);
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroUnsupportedBehaviorTreeSpec,
	"Nwiro.Pro.AI.BehaviorTree.UnsupportedNoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroUnsupportedBehaviorTreeSpec::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("test owns an idle editor transaction"), GEditor && !GEditor->PlayWorld && !GEditor->IsTransactionActive() && !GIsTransacting)) return false;
	UPackage* Package = CreatePackage(*(TEXT("/Game/NwiroBTReject_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBehaviorTree> Tree(NewObject<UBehaviorTree>(Package, TEXT("Tree"), RF_Public | RF_Standalone | RF_Transactional));
	TStrongObjectPtr<UBlackboardData> Blackboard(NewObject<UBlackboardData>(Package, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional));
	FAssetRegistryModule::AssetCreated(Tree.Get()); FAssetRegistryModule::AssetCreated(Blackboard.Get());
	ON_SCOPE_EXIT
	{
		for (UObject* Object : { static_cast<UObject*>(Tree.Get()), static_cast<UObject*>(Blackboard.Get()) })
		{ FAssetRegistryModule::AssetDeleted(Object); Object->ClearFlags(RF_Public | RF_Standalone); Object->MarkAsGarbage(); }
		Package->SetDirtyFlag(false); Package->MarkAsGarbage();
	};
	const FString Command = FString::Printf(TEXT("{\"path\":\"%s\",\"blackboard\":\"%s\",\"nodes\":[{\"nodeType\":\"composite\",\"nodeClass\":\"BTComposite_Sequence\"}]}"), *Tree->GetPathName(), *Blackboard->GetPathName());
	for (bool WasDirty : { false, true })
	{
		Package->SetDirtyFlag(WasDirty);
		const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
		TSharedPtr<FJsonObject> Result;
		const FString Raw = FNwiroAITools::AddBehaviorTreeNodes(Command);
		if (!TestTrue(TEXT("unsupported response parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result) && Result.IsValid())) continue;
		TestFalse(TEXT("unsupported authoring is not success"), Result->GetBoolField(TEXT("success")));
		TestEqual(TEXT("explicit unsupported contract"), Result->GetStringField(TEXT("errorCode")), FString(TEXT("unsupported_operation")));
		TestFalse(TEXT("reports no mutation"), Result->GetBoolField(TEXT("mutated")));
		TestEqual(TEXT("reports no nodes created"), Result->GetNumberField(TEXT("nodesCreated")), 0.0);
		TestNull(TEXT("runtime root remains absent"), Tree->RootNode.Get());
		TestNull(TEXT("editor graph remains absent"), Tree->BTGraph.Get());
		TestNull(TEXT("blackboard is not partially bound"), Tree->BlackboardAsset.Get());
		TestEqual(TEXT("rejection preserves dirty flag"), Package->IsDirty(), WasDirty);
		TestEqual(TEXT("rejection adds no undo step"), GEditor->Trans->GetUndoContext().TransactionId, UndoBefore);
	}
	return true;
}
