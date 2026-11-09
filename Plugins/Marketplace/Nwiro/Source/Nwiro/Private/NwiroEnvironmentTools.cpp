// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroEnvironmentTools.h"
#include "NwiroTransactionHelper.h"
#include "UObject/Package.h"   // UPackage::IsDirty() behind the replies' dirtyAfter field
#include "Components/SkyAtmosphereComponent.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Editor.h"
#include "CoreGlobals.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SplineComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/Light.h"
#include "Components/LightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "EngineUtils.h"
#include "EditorAssetLibrary.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroEnv, Log, All);

static UWorld* GetEdWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// Get existing USplineComponent or attach a new one — LLM friendly.
static USplineComponent* GetOrAddSpline(AActor* Actor, bool bAutoAttach, FNwiroTransactionHelper* CallerTx)
{
	if (!Actor) return nullptr;
	USplineComponent* S = Actor->FindComponentByClass<USplineComponent>();
	if (S) return S;
	if (!bAutoAttach) return nullptr;
	// >>> NWIRO_PARITY BEGIN: spline-component-graft-dirty
	// Grafting a component onto the actor is itself an edit, so it runs inside the CALLER's
	// transaction: one tool call stays one undo step. Without the caller's transaction the graft
	// and the edit undid separately and a single Ctrl+Z left the component behind, so a caller
	// that has none gets no graft rather than an un-undoable one. The component is created
	// RF_Transactional so undo can drop it, and the actor is dirtied so the work is visible.
	if (!ensureMsgf(CallerTx, TEXT("GetOrAddSpline was asked to graft a spline component with no caller "
		"transaction: the graft would not be undoable, so it is refused."))) return nullptr;
	FNwiroTransactionHelper& Tx = *CallerTx;
	Tx.AlsoModify(Actor);
	S = NewObject<USplineComponent>(Actor, TEXT("SplineComponent"), RF_Transactional);
	if (!S) return nullptr;
	S->RegisterComponent();
	Actor->AddInstanceComponent(S);
	if (!Actor->GetRootComponent()) Actor->SetRootComponent(S);
	else { Tx.AlsoModify(Actor->GetRootComponent()); S->AttachToComponent(Actor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform); }
	S->ClearSplinePoints();
	Actor->MarkPackageDirty();
	// <<< NWIRO_PARITY END: spline-component-graft-dirty
	return S;
}

static AActor* FindActorByName(UWorld* World, const FString& Name)
{
	if (!World || Name.IsEmpty()) return nullptr;
	AActor *Exact = nullptr, *CI = nullptr, *Starts = nullptr, *Sub = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		const FString N = A->GetName(), L = A->GetActorLabel();
		if (N == Name || L == Name) { Exact = A; break; }
		if (!CI && (N.Equals(Name, ESearchCase::IgnoreCase) || L.Equals(Name, ESearchCase::IgnoreCase))) CI = A;
		if (!Starts && (N.StartsWith(Name, ESearchCase::IgnoreCase) || L.StartsWith(Name, ESearchCase::IgnoreCase))) Starts = A;
		if (!Sub && (N.Contains(Name, ESearchCase::IgnoreCase) || L.Contains(Name, ESearchCase::IgnoreCase))) Sub = A;
	}
	return Exact ? Exact : (CI ? CI : (Starts ? Starts : Sub));
}

// ============================================================
// SET POST PROCESS
// ============================================================

FString FNwiroEnvironmentTools::SetPostProcess(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	APostProcessVolume* PPV = nullptr;
	if (!ActorName.IsEmpty())
	{
		PPV = Cast<APostProcessVolume>(FindActorByName(World, ActorName));
	}
	if (!PPV)
	{
		TActorIterator<APostProcessVolume> It(World);
		if (It) PPV = *It;
	}
	if (!PPV)
	{
		FActorSpawnParameters Sp;
		Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PPV = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Sp);
		if (PPV) { PPV->bUnbound = true; PPV->SetActorLabel(TEXT("NwiroPostProcessVolume")); }
	}
	if (!PPV) return TEXT("{\"success\":false,\"error\":\"PostProcessVolume not found and could not auto-spawn one\"}");

	FPostProcessSettings& S = PPV->Settings;

	if (Cmd->HasField(TEXT("bloom_intensity"))) { S.bOverride_BloomIntensity = true; S.BloomIntensity = (float)Cmd->GetNumberField(TEXT("bloom_intensity")); }
	if (Cmd->HasField(TEXT("auto_exposure_min"))) { S.bOverride_AutoExposureMinBrightness = true; S.AutoExposureMinBrightness = (float)Cmd->GetNumberField(TEXT("auto_exposure_min")); }
	if (Cmd->HasField(TEXT("auto_exposure_max"))) { S.bOverride_AutoExposureMaxBrightness = true; S.AutoExposureMaxBrightness = (float)Cmd->GetNumberField(TEXT("auto_exposure_max")); }
	if (Cmd->HasField(TEXT("vignette_intensity"))) { S.bOverride_VignetteIntensity = true; S.VignetteIntensity = (float)Cmd->GetNumberField(TEXT("vignette_intensity")); }
	if (Cmd->HasField(TEXT("saturation"))) { float Sat = (float)Cmd->GetNumberField(TEXT("saturation")); S.bOverride_ColorSaturation = true; S.ColorSaturation = FVector4(Sat, Sat, Sat, Sat); }
	if (Cmd->HasField(TEXT("contrast"))) { float Con = (float)Cmd->GetNumberField(TEXT("contrast")); S.bOverride_ColorContrast = true; S.ColorContrast = FVector4(Con, Con, Con, Con); }
	if (Cmd->HasField(TEXT("infinite_extent"))) PPV->bUnbound = Cmd->GetBoolField(TEXT("infinite_extent"));

	PPV->MarkPackageDirty();
	// TODO: PPV is a level actor; persistence needs UEditorLevelLibrary::SaveCurrentLevel on the level package.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *PPV->GetActorLabel());
}

// ============================================================
// SET FOG
// ============================================================

FString FNwiroEnvironmentTools::SetFog(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AExponentialHeightFog* Fog = nullptr;
	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	if (!ActorName.IsEmpty())
		Fog = Cast<AExponentialHeightFog>(FindActorByName(World, ActorName));
	else
	{
		TActorIterator<AExponentialHeightFog> It(World);
		if (It) Fog = *It;
	}

	if (!Fog && ActorName.IsEmpty())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Fog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(),
			FVector(0, 0, 200), FRotator::ZeroRotator, Params);
		if (Fog) Fog->SetActorLabel(TEXT("NwiroExponentialHeightFog"));
	}

	if (!Fog || !Fog->GetComponent()) return TEXT("{\"success\":false,\"error\":\"ExponentialHeightFog not found and auto-spawn failed\"}");

	UExponentialHeightFogComponent* C = Fog->GetComponent();
	if (Cmd->HasField(TEXT("density"))) C->FogDensity = (float)Cmd->GetNumberField(TEXT("density"));
	if (Cmd->HasField(TEXT("height_falloff"))) C->FogHeightFalloff = (float)Cmd->GetNumberField(TEXT("height_falloff"));
	if (Cmd->HasField(TEXT("start_distance"))) C->StartDistance = (float)Cmd->GetNumberField(TEXT("start_distance"));
	if (Cmd->HasField(TEXT("volumetric"))) C->bEnableVolumetricFog = Cmd->GetBoolField(TEXT("volumetric"));

	C->MarkRenderStateDirty();
	Fog->MarkPackageDirty();
	// TODO: Fog actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *Fog->GetActorLabel());
}

// ============================================================
// SET SKY ATMOSPHERE
// ============================================================

namespace NwiroSkyAtmosphere
{
// The callback is invoked only after Apply validates the whole batch and
// rejects registered static components. UE 5.5 has these fields but no setters.
template<void (USkyAtmosphereComponent::*Setter)(float)>
static void CallNativeFloatSetter(USkyAtmosphereComponent* Sky, float Value)
{
	(Sky->*Setter)(Value);
}
static void SetAerialPerspectiveStartDepthCompat(USkyAtmosphereComponent* Sky, float Value)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 6
	if (Sky->AerialPerspectiveStartDepth != Value)
	{
		Sky->AerialPerspectiveStartDepth = Value;
		Sky->MarkRenderStateDirty();
	}
#else
	Sky->SetAerialPerspectiveStartDepth(Value);
#endif
}
static void SetTransmittanceMinLightElevationAngleCompat(USkyAtmosphereComponent* Sky, float Value)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 6
	if (Sky->TransmittanceMinLightElevationAngle != Value)
	{
		Sky->TransmittanceMinLightElevationAngle = Value;
		Sky->MarkRenderStateDirty();
	}
#else
	Sky->SetTransmittanceMinLightElevationAngle(Value);
#endif
}
struct FScalar
{
	const TCHAR* Name;
	float USkyAtmosphereComponent::* Member;
	void (*Set)(USkyAtmosphereComponent*, float);
	double Minimum;
	double Maximum;
};
static const FScalar Scalars[] = {
	{ TEXT("BottomRadius"), &USkyAtmosphereComponent::BottomRadius, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetBottomRadius>, 0.1, 10000.0 },
	{ TEXT("AtmosphereHeight"), &USkyAtmosphereComponent::AtmosphereHeight, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetAtmosphereHeight>, 0.1, MAX_flt },
	{ TEXT("MultiScatteringFactor"), &USkyAtmosphereComponent::MultiScatteringFactor, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetMultiScatteringFactor>, 0.0, 100.0 },
	{ TEXT("RayleighScatteringScale"), &USkyAtmosphereComponent::RayleighScatteringScale, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetRayleighScatteringScale>, 0.0, MAX_flt },
	{ TEXT("RayleighExponentialDistribution"), &USkyAtmosphereComponent::RayleighExponentialDistribution, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetRayleighExponentialDistribution>, 0.001, MAX_flt },
	{ TEXT("MieScatteringScale"), &USkyAtmosphereComponent::MieScatteringScale, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetMieScatteringScale>, 0.0, MAX_flt },
	{ TEXT("MieAbsorptionScale"), &USkyAtmosphereComponent::MieAbsorptionScale, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetMieAbsorptionScale>, 0.0, MAX_flt },
	{ TEXT("MieAnisotropy"), &USkyAtmosphereComponent::MieAnisotropy, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetMieAnisotropy>, 0.0, 0.999 },
	{ TEXT("MieExponentialDistribution"), &USkyAtmosphereComponent::MieExponentialDistribution, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetMieExponentialDistribution>, 0.001, MAX_flt },
	{ TEXT("OtherAbsorptionScale"), &USkyAtmosphereComponent::OtherAbsorptionScale, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetOtherAbsorptionScale>, 0.0, MAX_flt },
	{ TEXT("AerialPespectiveViewDistanceScale"), &USkyAtmosphereComponent::AerialPespectiveViewDistanceScale, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetAerialPespectiveViewDistanceScale>, 0.0, MAX_flt },
	{ TEXT("AerialPerspectiveStartDepth"), &USkyAtmosphereComponent::AerialPerspectiveStartDepth, &SetAerialPerspectiveStartDepthCompat, 0.001, MAX_flt },
	{ TEXT("HeightFogContribution"), &USkyAtmosphereComponent::HeightFogContribution, &CallNativeFloatSetter<&USkyAtmosphereComponent::SetHeightFogContribution>, 0.0, MAX_flt },
	{ TEXT("TransmittanceMinLightElevationAngle"), &USkyAtmosphereComponent::TransmittanceMinLightElevationAngle, &SetTransmittanceMinLightElevationAngleCompat, -90.0, 90.0 }
};
struct FColorProperty
{
	const TCHAR* Name;
	FLinearColor USkyAtmosphereComponent::* Member;
	void (USkyAtmosphereComponent::* Set)(FLinearColor);
};
static const FColorProperty Colors[] = {
	{ TEXT("RayleighScattering"), &USkyAtmosphereComponent::RayleighScattering, &USkyAtmosphereComponent::SetRayleighScattering },
	{ TEXT("MieScattering"), &USkyAtmosphereComponent::MieScattering, &USkyAtmosphereComponent::SetMieScattering },
	{ TEXT("MieAbsorption"), &USkyAtmosphereComponent::MieAbsorption, &USkyAtmosphereComponent::SetMieAbsorption },
	{ TEXT("OtherAbsorption"), &USkyAtmosphereComponent::OtherAbsorption, &USkyAtmosphereComponent::SetOtherAbsorption },
	{ TEXT("SkyLuminanceFactor"), &USkyAtmosphereComponent::SkyLuminanceFactor, &USkyAtmosphereComponent::SetSkyLuminanceFactor }
};
static FString Failure(const FString& Error)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetBoolField(TEXT("mutated"), false);
	Result->SetStringField(TEXT("error"), Error);
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}
static TSharedRef<FJsonObject> ColorJson(double R, double G, double B)
{
	TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetNumberField(TEXT("r"), R); Value->SetNumberField(TEXT("g"), G); Value->SetNumberField(TEXT("b"), B);
	return Value;
}
static FString Apply(USkyAtmosphereComponent* Sky, const TSharedPtr<FJsonObject>& Properties)
{
	if (!IsValid(Sky) || !Properties.IsValid() || Properties->Values.IsEmpty())
		return Failure(TEXT("A sky component and a nonempty properties object are required."));
	if (Sky->IsRegistered() && Sky->Mobility == EComponentMobility::Static)
		return Failure(TEXT("SkyAtmosphere setters require a movable or stationary component; static components are not changed by this tool."));
	struct FChange
	{
		FString Name;
		const FScalar* Scalar = nullptr;
		const FColorProperty* Color = nullptr;
		float Number = 0;
		FLinearColor Linear;
		FColor Bytes;
	};
	TArray<FChange> Changes;
	for (const auto& Property : Properties->Values)
	{
		FChange Change; Change.Name = Property.Key;
		for (const FScalar& Spec : Scalars) if (Property.Key == Spec.Name) Change.Scalar = &Spec;
		for (const FColorProperty& Spec : Colors) if (Property.Key == Spec.Name) Change.Color = &Spec;
		if (Change.Scalar)
		{
			double Value = 0;
			if (!Property.Value.IsValid() || Property.Value->Type != EJson::Number || !Property.Value->TryGetNumber(Value)
				|| !FMath::IsFinite(Value) || Value < Change.Scalar->Minimum || Value > Change.Scalar->Maximum)
				return Failure(FString::Printf(TEXT("%s requires a finite number in [%g, %g]."), *Property.Key, Change.Scalar->Minimum, Change.Scalar->Maximum));
			Change.Number = static_cast<float>(Value);
		}
		else if (Change.Color || Property.Key == TEXT("GroundAlbedo"))
		{
			if (!Property.Value.IsValid() || Property.Value->Type != EJson::Object || !Property.Value->AsObject().IsValid())
				return Failure(Change.Name + TEXT(" requires an object containing numeric r, g, b channels."));
			const auto& Value = Property.Value->AsObject();
			if (Value->Values.Num() != 3) return Failure(Change.Name + TEXT(" accepts exactly r, g, b channels."));
			double Channels[3];
			const TCHAR* Keys[] = { TEXT("r"), TEXT("g"), TEXT("b") };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!Value->HasTypedField<EJson::Number>(Keys[Index]) || !Value->TryGetNumberField(Keys[Index], Channels[Index])
					|| !FMath::IsFinite(Channels[Index]) || Channels[Index] < 0.0 || Channels[Index] > (Change.Color ? 1e38 : 255.0)
					|| (!Change.Color && FMath::FloorToDouble(Channels[Index]) != Channels[Index]))
					return Failure(Change.Name + (Change.Color ? TEXT(" requires finite nonnegative linear RGB channels at most 1e38.") : TEXT(" requires integer RGB channels from 0 to 255.")));
			}
			if (Change.Color) Change.Linear = FLinearColor(Channels[0], Channels[1], Channels[2], (Sky->*(Change.Color->Member)).A);
			else Change.Bytes = FColor(static_cast<uint8>(Channels[0]), static_cast<uint8>(Channels[1]), static_cast<uint8>(Channels[2]), Sky->GroundAlbedo.A);
		}
		else return Failure(TEXT("Unsupported SkyAtmosphere property: ") + Change.Name);
		Changes.Add(MoveTemp(Change));
	}

	// The complete batch is validated before Modify or any render-state change.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetSkyAtmosphere", "AI: Set Sky Atmosphere"), Sky);
	TSharedRef<FJsonObject> Observed = MakeShared<FJsonObject>();
	for (const FChange& Change : Changes)
	{
		if (Change.Scalar)
		{
			Change.Scalar->Set(Sky, Change.Number);
			Observed->SetNumberField(Change.Name, Sky->*(Change.Scalar->Member));
		}
		else if (Change.Color)
		{
			(Sky->*(Change.Color->Set))(Change.Linear);
			const FLinearColor& Color = Sky->*(Change.Color->Member);
			Observed->SetObjectField(Change.Name, ColorJson(Color.R, Color.G, Color.B));
		}
		else
		{
			Sky->SetGroundAlbedo(Change.Bytes);
			Observed->SetObjectField(Change.Name, ColorJson(Sky->GroundAlbedo.R, Sky->GroundAlbedo.G, Sky->GroundAlbedo.B));
		}
	}
	Sky->MarkPackageDirty();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("component"), Sky->GetPathName());
	if (Sky->GetOwner()) Result->SetStringField(TEXT("actor"), Sky->GetOwner()->GetPathName());
	Result->SetObjectField(TEXT("properties"), Observed);
	Result->SetNumberField(TEXT("applied"), Changes.Num());
	Result->SetBoolField(TEXT("savedToDisk"), false);
	Result->SetBoolField(TEXT("dirtyAfter"), Sky->GetOutermost()->IsDirty());
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}
}

FString FNwiroEnvironmentTools::SetSkyAtmosphere(const FString& JsonCommand)
{
	using namespace NwiroSkyAtmosphere;
	if (!IsInGameThread() || !GEditor || GEditor->PlayWorld || GIsTransacting || GEditor->IsTransactionActive())
		return Failure(TEXT("set_sky_atmosphere requires an idle editor game thread outside PIE and active transactions."));
	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		return Failure(TEXT("Invalid JSON object."));
	for (const auto& Field : Cmd->Values)
		if (Field.Key != TEXT("actor") && Field.Key != TEXT("properties")) return Failure(FString::Printf(TEXT("Unknown parameter: %s"), *Field.Key));
	FString ActorName;
	if (!Cmd->HasTypedField<EJson::String>(TEXT("actor")) || !Cmd->TryGetStringField(TEXT("actor"), ActorName) || ActorName.IsEmpty())
		return Failure(TEXT("actor must be an exact actor path, name, or unique label."));
	if (!Cmd->HasTypedField<EJson::Object>(TEXT("properties")))
		return Failure(TEXT("properties must be a nonempty object of supported SkyAtmosphere settings."));
	UWorld* World = GetEdWorld();
	if (!World) return Failure(TEXT("No editor world."));
	TArray<AActor*> Matches;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (IsValid(Actor) && (Actor->GetPathName() == ActorName || Actor->GetName() == ActorName || Actor->GetActorLabel() == ActorName))
			Matches.Add(Actor);
	}
	if (Matches.Num() != 1) return Failure(TEXT("actor must identify exactly one actor; use its full object path if labels are ambiguous."));
	TArray<USkyAtmosphereComponent*> Components;
	Matches[0]->GetComponents<USkyAtmosphereComponent>(Components);
	if (Components.Num() != 1) return Failure(TEXT("The actor must own exactly one SkyAtmosphere component."));
	return Apply(Components[0], Cmd->GetObjectField(TEXT("properties")));
}

// ============================================================
// SET LIGHT PROPERTIES
// ============================================================

FString FNwiroEnvironmentTools::SetLightProperties(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("name"));
	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	ULightComponent* Light = Actor->FindComponentByClass<ULightComponent>();
	if (!Light) return TEXT("{\"success\":false,\"error\":\"No light component found on actor\"}");

	if (Cmd->HasField(TEXT("intensity"))) Light->SetIntensity((float)Cmd->GetNumberField(TEXT("intensity")));
	if (Cmd->HasField(TEXT("color")))
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Cmd->TryGetObjectField(TEXT("color"), ColorObj))
		{
			float CR = (*ColorObj)->HasField(TEXT("r")) ? (float)(*ColorObj)->GetNumberField(TEXT("r")) : 1.0f;
			float CG = (*ColorObj)->HasField(TEXT("g")) ? (float)(*ColorObj)->GetNumberField(TEXT("g")) : 1.0f;
			float CB = (*ColorObj)->HasField(TEXT("b")) ? (float)(*ColorObj)->GetNumberField(TEXT("b")) : 1.0f;
			Light->SetLightColor(FLinearColor(CR, CG, CB));
		}
	}
	if (Cmd->HasField(TEXT("temperature"))) { Light->bUseTemperature = true; Light->SetTemperature((float)Cmd->GetNumberField(TEXT("temperature"))); }
	if (Cmd->HasField(TEXT("cast_shadows"))) Light->SetCastShadows(Cmd->GetBoolField(TEXT("cast_shadows")));
	if (Cmd->HasField(TEXT("source_radius")))
	{
		if (UPointLightComponent* PL = Cast<UPointLightComponent>(Light))
			PL->SourceRadius = (float)Cmd->GetNumberField(TEXT("source_radius"));
	}
	if (Cmd->HasField(TEXT("attenuation_radius")))
	{
		if (UPointLightComponent* PL = Cast<UPointLightComponent>(Light))
			PL->SetAttenuationRadius((float)Cmd->GetNumberField(TEXT("attenuation_radius")));
	}
	if (Cmd->HasField(TEXT("inner_cone_angle")))
	{
		if (USpotLightComponent* SL = Cast<USpotLightComponent>(Light))
			SL->SetInnerConeAngle((float)Cmd->GetNumberField(TEXT("inner_cone_angle")));
	}
	if (Cmd->HasField(TEXT("outer_cone_angle")))
	{
		if (USpotLightComponent* SL = Cast<USpotLightComponent>(Light))
			SL->SetOuterConeAngle((float)Cmd->GetNumberField(TEXT("outer_cone_angle")));
	}

	Light->MarkRenderStateDirty();
	Actor->MarkPackageDirty();
	// TODO: Light actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"lightType\":\"%s\"}"),
		*Actor->GetActorLabel(), *Light->GetClass()->GetName());
}

// ============================================================
// SET PHYSICS SIMULATION
// ============================================================

FString FNwiroEnvironmentTools::SetPhysicsSimulation(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>();
	if (!Prim) return TEXT("{\"success\":false,\"error\":\"No primitive component\"}");

	if (Cmd->HasField(TEXT("simulate"))) Prim->SetSimulatePhysics(Cmd->GetBoolField(TEXT("simulate")));
	if (Cmd->HasField(TEXT("gravity"))) Prim->SetEnableGravity(Cmd->GetBoolField(TEXT("gravity")));
	if (Cmd->HasField(TEXT("mass")))
	{
		Prim->SetMassOverrideInKg(NAME_None, (float)Cmd->GetNumberField(TEXT("mass")));
	}
	if (Cmd->HasField(TEXT("linear_damping"))) Prim->SetLinearDamping((float)Cmd->GetNumberField(TEXT("linear_damping")));
	if (Cmd->HasField(TEXT("angular_damping"))) Prim->SetAngularDamping((float)Cmd->GetNumberField(TEXT("angular_damping")));

	Actor->MarkPackageDirty();
	// TODO: physics actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *Actor->GetActorLabel());
}

// ============================================================
// SET COLLISION PROFILE
// ============================================================

FString FNwiroEnvironmentTools::SetCollisionProfile(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString Profile = Cmd->GetStringField(TEXT("profile"));

	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>();
	if (!Prim) return TEXT("{\"success\":false,\"error\":\"No primitive component\"}");

	Prim->SetCollisionProfileName(FName(*Profile));
	Actor->MarkPackageDirty();
	// TODO: actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"profile\":\"%s\"}"), *Actor->GetActorLabel(), *Profile);
}

// ============================================================
// ADD PHYSICS CONSTRAINT
// ============================================================

FString FNwiroEnvironmentTools::AddPhysicsConstraint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Actor1Name = Cmd->GetStringField(TEXT("actor1"));
	FString Actor2Name = Cmd->GetStringField(TEXT("actor2"));
	FString ConstraintType = Cmd->GetStringField(TEXT("type"));

	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* A1 = FindActorByName(World, Actor1Name);
	AActor* A2 = FindActorByName(World, Actor2Name);
	if (!A1) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor1 not found: %s\"}"), *Actor1Name);
	if (!A2) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor2 not found: %s\"}"), *Actor2Name);

	FVector MidPoint = (A1->GetActorLocation() + A2->GetActorLocation()) * 0.5f;
	FActorSpawnParameters Params;
	// >>> NWIRO_PARITY BEGIN: physics-constraint-transaction
	// SpawnActor dirties only an external (One File Per Actor) package, so the spawn joins the
	// transaction and the new actor is modified and dirtied explicitly, as the editor's spawn does.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddPhysicsConstraint", "AI: Add Physics Constraint"), /*no object: matches UEditorEngine::AddActor, so a World Partition map package stays clean*/ nullptr);
	APhysicsConstraintActor* ConstraintActor = World->SpawnActor<APhysicsConstraintActor>(MidPoint, FRotator::ZeroRotator, Params);
	if (!ConstraintActor) return TEXT("{\"success\":false,\"error\":\"Failed to spawn constraint actor\"}");
	Tx.AlsoModify(ConstraintActor);

	UPhysicsConstraintComponent* Constraint = ConstraintActor->GetConstraintComp();
	Tx.AlsoModify(Constraint);
	// <<< NWIRO_PARITY END: physics-constraint-transaction
	Constraint->ConstraintActor1 = A1;
	Constraint->ConstraintActor2 = A2;

	FString TypeLower = ConstraintType.ToLower();
	if (TypeLower == TEXT("fixed"))
	{
		Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0);
		Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
		Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
	}
	else if (TypeLower == TEXT("hinge"))
	{
		Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0);
		Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
		Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
	}
	else if (TypeLower == TEXT("ballsocket") || TypeLower == TEXT("ball"))
	{
		Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0);
		Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Free, 0);
		Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0);
	}
	// default: free

	Constraint->InitComponentConstraint();
	// >>> NWIRO_PARITY BEGIN: physics-constraint-dirty
	ConstraintActor->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"constraint\":\"%s\",\"type\":\"%s\",\"actor1\":\"%s\",\"actor2\":\"%s\",\"dirtyAfter\":%s}"),
		*ConstraintActor->GetActorLabel(), *ConstraintType, *Actor1Name, *Actor2Name,
		ConstraintActor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: physics-constraint-dirty
}

// ============================================================
// GET PHYSICS INFO
// ============================================================

FString FNwiroEnvironmentTools::GetPhysicsInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>();
	if (!Prim) return TEXT("{\"success\":false,\"error\":\"No primitive component\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetBoolField(TEXT("simulatePhysics"), Prim->IsSimulatingPhysics());
	Result->SetBoolField(TEXT("gravityEnabled"), Prim->IsGravityEnabled());
	Result->SetNumberField(TEXT("mass"), Prim->GetMass());
	Result->SetNumberField(TEXT("linearDamping"), Prim->GetLinearDamping());
	Result->SetNumberField(TEXT("angularDamping"), Prim->GetAngularDamping());
	Result->SetStringField(TEXT("collisionProfile"), Prim->GetCollisionProfileName().ToString());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CREATE SPLINE ACTOR
// ============================================================

FString FNwiroEnvironmentTools::CreateSplineActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Label = Cmd->GetStringField(TEXT("label"));
	if (Label.IsEmpty()) Label = TEXT("SplineActor");

	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	// >>> NWIRO_PARITY BEGIN: create-spline-actor-transaction
	// SpawnActor dirties only an external (One File Per Actor) package, so the spawn joins the
	// transaction and the new actor is modified and dirtied explicitly, as the editor's spawn does.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateSplineActor", "AI: Create Spline Actor"), /*no object: matches UEditorEngine::AddActor, so a World Partition map package stays clean*/ nullptr);
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass());
	if (!Actor) return TEXT("{\"success\":false,\"error\":\"Failed to spawn actor\"}");
	Tx.AlsoModify(Actor);

	Actor->SetActorLabel(Label);
	USplineComponent* Spline = NewObject<USplineComponent>(Actor, TEXT("SplineComponent"), RF_Transactional);
	// <<< NWIRO_PARITY END: create-spline-actor-transaction
	Spline->RegisterComponent();
	Actor->AddInstanceComponent(Spline);
	Actor->SetRootComponent(Spline);
	Spline->ClearSplinePoints();

	// Add initial points
	const TArray<TSharedPtr<FJsonValue>>* Points;
	if (Cmd->TryGetArrayField(TEXT("points"), Points))
	{
		for (const TSharedPtr<FJsonValue>& PVal : *Points)
		{
			const TSharedPtr<FJsonObject>& PObj = PVal->AsObject();
			if (!PObj.IsValid()) continue;
			double X = PObj->HasField(TEXT("x")) ? PObj->GetNumberField(TEXT("x")) : 0;
			double Y = PObj->HasField(TEXT("y")) ? PObj->GetNumberField(TEXT("y")) : 0;
			double Z = PObj->HasField(TEXT("z")) ? PObj->GetNumberField(TEXT("z")) : 0;
			Spline->AddSplinePoint(FVector(X, Y, Z), ESplineCoordinateSpace::World, true);
		}
	}
	else
	{
		// Default 2-point spline
		Spline->AddSplinePoint(FVector(0, 0, 0), ESplineCoordinateSpace::World, true);
		Spline->AddSplinePoint(FVector(500, 0, 0), ESplineCoordinateSpace::World, true);
	}

	Spline->UpdateSpline();
	// >>> NWIRO_PARITY BEGIN: create-spline-actor-dirty
	Actor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"pointCount\":%d,\"dirtyAfter\":%s}"),
		*Label, Spline->GetNumberOfSplinePoints(), Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: create-spline-actor-dirty
}

// ============================================================
// ADD SPLINE POINT
// ============================================================

FString FNwiroEnvironmentTools::AddSplinePoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// One transaction for the whole call so a graft and the edit undo together.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSplinePoint", "AI: Add Spline Point"), Actor);
	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true, &Tx);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;

	// >>> NWIRO_PARITY BEGIN: add-spline-point-dirty
	Tx.AlsoModify(Spline);
	Spline->AddSplinePoint(FVector(X, Y, Z), ESplineCoordinateSpace::World, true);
	Spline->UpdateSpline();
	Actor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"pointCount\":%d,\"dirtyAfter\":%s}"),
		Spline->GetNumberOfSplinePoints(), Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: add-spline-point-dirty
}

// ============================================================
// SET SPLINE POINT
// ============================================================

FString FNwiroEnvironmentTools::SetSplinePoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	int32 Index = (int32)Cmd->GetNumberField(TEXT("index"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// One transaction for the whole call so a graft and the edit undo together.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetSplinePoint", "AI: Set Spline Point"), Actor);
	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true, &Tx);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");
	if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
		return TEXT("{\"success\":false,\"error\":\"Index out of range\"}");

	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).X;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Y;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Z;

	// >>> NWIRO_PARITY BEGIN: set-spline-point-dirty
	Tx.AlsoModify(Spline);
	Spline->SetLocationAtSplinePoint(Index, FVector(X, Y, Z), ESplineCoordinateSpace::World, true);
	Spline->UpdateSpline();
	Actor->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"index\":%d,\"dirtyAfter\":%s}"),
		Index, Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: set-spline-point-dirty
}

// ============================================================
// REMOVE SPLINE POINT
// ============================================================

FString FNwiroEnvironmentTools::RemoveSplinePoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	int32 Index = (int32)Cmd->GetNumberField(TEXT("index"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// One transaction for the whole call so a graft and the edit undo together.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "RemoveSplinePoint", "AI: Remove Spline Point"), Actor);
	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true, &Tx);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	// >>> NWIRO_PARITY BEGIN: remove-spline-point-dirty
	Tx.AlsoModify(Spline);
	Spline->RemoveSplinePoint(Index, true);
	Spline->UpdateSpline();
	Actor->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"pointCount\":%d,\"dirtyAfter\":%s}"),
		Spline->GetNumberOfSplinePoints(), Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: remove-spline-point-dirty
}

// ============================================================
// GET SPLINE INFO
// ============================================================

FString FNwiroEnvironmentTools::GetSplineInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// >>> NWIRO_PARITY BEGIN: spline-info-pure-read
	// A read must not create unsaved work. Asking what a spline looks like used to graft a spline
	// component onto an actor that had none, which dirties that actor's package and puts an undo step
	// on the stack for a query - and would make the Layer 1 guard refuse a save because someone looked.
	// Reading with bAutoAttach=false leaves the actor untouched; add_spline_point still grafts one
	// inside its own transaction when the caller actually wants to edit.
	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/false, /*CallerTx=*/nullptr);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component on this actor (add_spline_point creates one)\"}");
	// <<< NWIRO_PARITY END: spline-info-pure-read

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("pointCount"), Spline->GetNumberOfSplinePoints());
	Result->SetNumberField(TEXT("length"), Spline->GetSplineLength());
	Result->SetBoolField(TEXT("closed"), Spline->IsClosedLoop());

	TArray<TSharedPtr<FJsonValue>> Pts;
	for (int32 i = 0; i < Spline->GetNumberOfSplinePoints(); i++)
	{
		FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
		P->SetNumberField(TEXT("index"), i);
		P->SetNumberField(TEXT("x"), Loc.X);
		P->SetNumberField(TEXT("y"), Loc.Y);
		P->SetNumberField(TEXT("z"), Loc.Z);
		Pts.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Result->SetArrayField(TEXT("points"), Pts);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SET SPLINE CLOSED
// ============================================================

FString FNwiroEnvironmentTools::SetSplineClosed(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	bool bClosed = Cmd->GetBoolField(TEXT("closed"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// One transaction for the whole call so a graft and the edit undo together.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetSplineClosed", "AI: Set Spline Closed"), Actor);
	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true, &Tx);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	// >>> NWIRO_PARITY BEGIN: set-spline-closed-dirty
	Tx.AlsoModify(Spline);
	Spline->SetClosedLoop(bClosed, true);
	Spline->UpdateSpline();
	Actor->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"closed\":%s,\"dirtyAfter\":%s}"),
		bClosed ? TEXT("true") : TEXT("false"), Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: set-spline-closed-dirty
}

// ============================================================
// SET SPLINE POINT TYPE
// ============================================================

FString FNwiroEnvironmentTools::SetSplinePointType(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	int32 Index = (int32)Cmd->GetNumberField(TEXT("index"));
	FString Type = Cmd->GetStringField(TEXT("type"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	// One transaction for the whole call so a graft and the edit undo together.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetSplinePointType", "AI: Set Spline Point Type"), Actor);
	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true, &Tx);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	ESplinePointType::Type PointType = ESplinePointType::Curve;
	FString TypeLower = Type.ToLower();
	if (TypeLower == TEXT("linear")) PointType = ESplinePointType::Linear;
	else if (TypeLower == TEXT("constant")) PointType = ESplinePointType::Constant;
	else if (TypeLower == TEXT("curveclamped") || TypeLower == TEXT("clamped")) PointType = ESplinePointType::CurveClamped;

	// >>> NWIRO_PARITY BEGIN: set-spline-point-type-dirty
	Tx.AlsoModify(Spline);
	Spline->SetSplinePointType(Index, PointType, true);
	Spline->UpdateSpline();
	Actor->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"index\":%d,\"type\":\"%s\",\"dirtyAfter\":%s}"),
		Index, *Type, Actor->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false"));
	// <<< NWIRO_PARITY END: set-spline-point-type-dirty
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroTruthfulMutationSpec.inl"
#endif
