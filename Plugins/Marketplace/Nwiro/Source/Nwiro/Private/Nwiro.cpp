// Copyright 2026 Nwiro. All Rights Reserved.

#include "Nwiro.h"
#include "NwiroIsolatedPython.h"
#include "NwiroPanel.h"
#include "NwiroToolRegistry.h"
#include "NwiroStyle.h"
#include "NwiroEditorTools.h"
#include "NwiroGraphAssetTypeActions.h"
#include "NwiroPCGGraphTypeActions.h"
#include "NwiroPCGGraph.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Json.h"
#include "AssetToolsModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"

#define LOCTEXT_NAMESPACE "FNwiroModule"

// ============================================================
// Config — Saved/Nwiro/config.json
// ============================================================

static FString GetConfigPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Nwiro"), TEXT("config.json"));
}

static TSharedPtr<FJsonObject> LoadConfig()
{
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *GetConfigPath()))
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			return Json;
		}
	}
	return MakeShareable(new FJsonObject);
}

static void SaveConfig(TSharedPtr<FJsonObject> Json)
{
	FString ConfigPath = GetConfigPath();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::GetPath(ConfigPath));

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(Output, *ConfigPath);
}

static int32 LoadSavedPort()
{
	auto Json = LoadConfig();
	return Json->HasField(TEXT("mcpPort")) ? (int32)Json->GetNumberField(TEXT("mcpPort")) : 5353;
}

void SaveMCPConfig(int32 Port)
{
	auto Json = LoadConfig();
	Json->SetNumberField(TEXT("mcpPort"), Port);
	SaveConfig(Json);
}

// ============================================================
// Secrets — Saved/Nwiro/config.json (gitignored via Saved/)
// ============================================================

FString LoadNwiroSecret(const FString& Key)
{
	auto Json = LoadConfig();
	if (Json->HasTypedField<EJson::Object>(TEXT("secrets")))
	{
		auto Secrets = Json->GetObjectField(TEXT("secrets"));
		if (Secrets->HasField(Key))
		{
			return Secrets->GetStringField(Key);
		}
	}
	// Fallback: environment variable (NWIRO_<KEY>)
	FString EnvKey = FPlatformMisc::GetEnvironmentVariable(*FString::Printf(TEXT("NWIRO_%s"), *Key.ToUpper()));
	return EnvKey;
}

void SaveNwiroSecret(const FString& Key, const FString& Value)
{
	auto Json = LoadConfig();
	TSharedPtr<FJsonObject> Secrets;
	if (Json->HasTypedField<EJson::Object>(TEXT("secrets")))
	{
		Secrets = Json->GetObjectField(TEXT("secrets"));
	}
	else
	{
		Secrets = MakeShareable(new FJsonObject);
	}

	// Empty value = remove key
	if (Value.IsEmpty())
	{
		Secrets->RemoveField(Key);
		Json->SetObjectField(TEXT("secrets"), Secrets);
		SaveConfig(Json);
		return;
	}
	Secrets->SetStringField(Key, Value);
	Json->SetObjectField(TEXT("secrets"), Secrets);
	SaveConfig(Json);
}

// ============================================================
// Module
// ============================================================

void FNwiroModule::StartupModule()
{
	if (NwiroIsolatedPython::IsWorker()) return;
	FNwiroStyle::Initialize();
	FNwiroPanel::Initialize();

	// Tear down Slate-owned panel state from OnEnginePreExit (fires before core
	// modules shut down, while Slate is alive) — robust against shutdown
	// ordering. FNwiroPanel::Shutdown() is idempotent, so the ShutdownModule()
	// call is a harmless fallback.
	EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic([]()
	{
		// Settle in-flight ue.* work and unregister its ticker BEFORE the panel
		// goes away: the completion sinks push through the bridge, which the
		// panel owns. Ordered first for that reason.
		NwiroIsolatedPython::Shutdown();
		FNwiroToolRegistry::CancelPendingEpicCalls(TEXT("Editor shutting down"));
		FNwiroPanel::Shutdown();
	});

	FNwiroLogCapture::Get().Register();

	// Register NwiroGraphAsset and NwiroPCGGraph type actions (locked PCG graph locker)
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	NwiroGraphAssetTypeActions = MakeShareable(new FNwiroGraphAssetTypeActions());
	AssetTools.RegisterAssetTypeActions(NwiroGraphAssetTypeActions.ToSharedRef());

	NwiroPCGGraphTypeActions = MakeShareable(new FNwiroPCGGraphTypeActions());
	AssetTools.RegisterAssetTypeActions(NwiroPCGGraphTypeActions.ToSharedRef());

	// Asset editor monitor (safety net — closes editor if it somehow opens despite TypeActions)
	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem =
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			AssetEditorMonitorHandle =
				AssetEditorSubsystem->OnAssetOpenedInEditor().AddRaw(
					this, &FNwiroModule::OnAssetOpenedInEditor);
		}
	}
}

void FNwiroModule::ShutdownModule()
{
	if (NwiroIsolatedPython::IsWorker()) return;
	NwiroIsolatedPython::Shutdown();
	// Idempotent, and required on BOTH teardown paths: a hot reload / plugin
	// disable unloads this DLL without OnEnginePreExit ever firing, and the
	// core ticker would still hold a delegate pointing into freed code.
	FNwiroToolRegistry::CancelPendingEpicCalls(TEXT("Nwiro module unloading"));

	if (EnginePreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
		EnginePreExitHandle.Reset();
	}

	// Unbind asset editor monitor
	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem =
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem && AssetEditorMonitorHandle.IsValid())
		{
			AssetEditorSubsystem->OnAssetOpenedInEditor().Remove(AssetEditorMonitorHandle);
		}
	}

	// Unregister NwiroGraphAsset and NwiroPCGGraph type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();

		if (NwiroGraphAssetTypeActions.IsValid())
		{
			AssetTools.UnregisterAssetTypeActions(NwiroGraphAssetTypeActions.ToSharedRef());
			NwiroGraphAssetTypeActions.Reset();
		}

		if (NwiroPCGGraphTypeActions.IsValid())
		{
			AssetTools.UnregisterAssetTypeActions(NwiroPCGGraphTypeActions.ToSharedRef());
			NwiroPCGGraphTypeActions.Reset();
		}
	}

	FNwiroLogCapture::Get().Unregister();
	FNwiroPanel::Shutdown();
	FNwiroStyle::Shutdown();

}

void FNwiroModule::OnAssetOpenedInEditor(UObject* Asset, IAssetEditorInstance* EditorInstance)
{
	// Safety net: if a UNwiroPCGGraph is opened despite TypeActions, close it immediately
	if (Asset && Asset->IsA<UNwiroPCGGraph>())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro: Blocked attempt to open locked PCG graph '%s' in editor"),
			*Asset->GetName());

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem =
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNwiroModule, Nwiro)
