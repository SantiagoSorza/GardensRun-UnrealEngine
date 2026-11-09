// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroPCGTools.h"
#include "NwiroTransactionHelper.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGVolume.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroPCG, Log, All);

namespace
{
	UWorld* GetEditorWorldSafe()
	{
		if (!GEditor) return nullptr;
		return GEditor->GetEditorWorldContext().World();
	}

	FString JsonToString(const TSharedRef<FJsonObject>& Json)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Json, W);
		return Out;
	}

	TSharedRef<FJsonObject> Fail(const FString& Error)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetBoolField(TEXT("success"), false);
		J->SetStringField(TEXT("error"), Error);
		return J;
	}

	TSharedPtr<FJsonObject> ParseArgs(const FString& JsonCommand)
	{
		TSharedPtr<FJsonObject> Cmd;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
		FJsonSerializer::Deserialize(Reader, Cmd);
		return Cmd;
	}
}

// ============================================================
// CREATE PCG GRAPH
// ============================================================

FString FNwiroPCGTools::CreatePcgGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid())
	{
		return JsonToString(Fail(TEXT("Invalid JSON")));
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->HasField(TEXT("path")) ? Cmd->GetStringField(TEXT("path")) : TEXT("/Game/PCG");
	if (Name.IsEmpty()) return JsonToString(Fail(TEXT("'name' is required")));

	// Strip .uasset / leading slashes
	Name.RemoveFromEnd(TEXT(".uasset"));
	if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/") + Path;
	while (Path.EndsWith(TEXT("/"))) Path.RemoveAt(Path.Len() - 1);

	const FString PackagePath = Path + TEXT("/") + Name;

	// Bail if it already exists
	if (UObject* Existing = LoadObject<UPCGGraph>(nullptr, *PackagePath))
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetBoolField(TEXT("success"), true);
		J->SetBoolField(TEXT("alreadyExists"), true);
		J->SetStringField(TEXT("name"), Name);
		J->SetStringField(TEXT("path"), Existing->GetPathName());
		return JsonToString(J);
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package) return JsonToString(Fail(TEXT("Failed to create package")));

	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, *Name, RF_Public | RF_Standalone);
	if (!Graph) return JsonToString(Fail(TEXT("Failed to create UPCGGraph")));

	FAssetRegistryModule::AssetCreated(Graph);
	Graph->MarkPackageDirty();

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), true);
	J->SetStringField(TEXT("name"), Name);
	J->SetStringField(TEXT("path"), Graph->GetPathName());
	return JsonToString(J);
}

// ============================================================
// FIND PCG GRAPHS
// ============================================================

FString FNwiroPCGTools::FindPcgGraphs(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	const FString Query = (Cmd.IsValid() && Cmd->HasField(TEXT("query"))) ? Cmd->GetStringField(TEXT("query")) : FString();

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FARFilter Filter;
	Filter.ClassPaths.Add(UPCGGraph::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));

	TArray<FAssetData> Found;
	ARM.Get().GetAssets(Filter, Found);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FAssetData& A : Found)
	{
		const FString Name = A.AssetName.ToString();
		if (!Query.IsEmpty() && !Name.Contains(Query, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), A.GetObjectPathString());
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("success"), true);
	Out->SetNumberField(TEXT("count"), Arr.Num());
	Out->SetArrayField(TEXT("graphs"), Arr);
	return JsonToString(Out);
}

// ============================================================
// SPAWN PCG VOLUME
// ============================================================

FString FNwiroPCGTools::SpawnPcgVolume(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid()) return JsonToString(Fail(TEXT("Invalid JSON")));

	FString GraphPath = Cmd->GetStringField(TEXT("graph"));
	FString Label;
	for (const TCHAR* K : { TEXT("label"), TEXT("name"), TEXT("actor") })
	{
		FString V; if (Cmd->TryGetStringField(K, V) && !V.IsEmpty()) { Label = V; break; }
	}
	if (Label.IsEmpty()) Label = TEXT("PCG_Volume");
	if (GraphPath.IsEmpty()) return JsonToString(Fail(TEXT("'graph' (PCG graph asset path) is required")));

	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
	if (!Graph) return JsonToString(Fail(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath)));

	UWorld* World = GetEditorWorldSafe();
	if (!World) return JsonToString(Fail(TEXT("No editor world")));

	FVector Location(0, 0, 0);
	FVector Scale(20, 20, 5);
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	if (Cmd->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
		Location = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
	if (Cmd->HasField(TEXT("x"))) Location.X = Cmd->GetNumberField(TEXT("x"));
	if (Cmd->HasField(TEXT("y"))) Location.Y = Cmd->GetNumberField(TEXT("y"));
	if (Cmd->HasField(TEXT("z"))) Location.Z = Cmd->GetNumberField(TEXT("z"));
	if (Cmd->HasField(TEXT("size")))
	{
		const double S = Cmd->GetNumberField(TEXT("size")) / 100.0;
		Scale = FVector(S, S, S);
	}
	if (Cmd->HasField(TEXT("scaleX"))) Scale.X = Cmd->GetNumberField(TEXT("scaleX"));
	if (Cmd->HasField(TEXT("scaleY"))) Scale.Y = Cmd->GetNumberField(TEXT("scaleY"));
	if (Cmd->HasField(TEXT("scaleZ"))) Scale.Z = Cmd->GetNumberField(TEXT("scaleZ"));

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// >>> NWIRO_PARITY BEGIN: spawn-pcg-volume-transaction
	// Validation is complete — the helper is constructed only now, because its
	// Modify() dirties the package even on a path that then fails.
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SpawnPCGVolume", "AI: Spawn PCG Volume"), /*no object: matches UEditorEngine::AddActor, so a World Partition map package stays clean*/ nullptr);

	APCGVolume* Volume = World->SpawnActor<APCGVolume>(APCGVolume::StaticClass(), Location, FRotator::ZeroRotator, Params);
	// No Tx.Cancel() on failure: Cancel restores no state and clears no dirty flag.
	if (!Volume) return JsonToString(Fail(TEXT("Failed to spawn APCGVolume")));

	// SpawnActor dirties ONLY the actor's external package, so on a classic
	// (non-OFPA) .umap this spawn was invisible and the next level switch threw
	// it away. Modify + MarkPackageDirty is what the editor's own spawn does.
	Tx.AlsoModify(Volume);
	// <<< NWIRO_PARITY END: spawn-pcg-volume-transaction
	Volume->SetActorScale3D(Scale);
	Volume->SetActorLabel(Label);

	if (UPCGComponent* Comp = Volume->FindComponentByClass<UPCGComponent>())
	{
		// >>> NWIRO_PARITY BEGIN: spawn-pcg-volume-component
		Tx.AlsoModify(Comp);
		Comp->SetGraph(Graph);
		// <<< NWIRO_PARITY END: spawn-pcg-volume-component
		// See IK mirror — auto-Generate on spawn crashed PCG on empty graphs.
		// Comp->Generate();
	}

	// >>> NWIRO_PARITY BEGIN: spawn-pcg-volume-dirty
	Volume->MarkPackageDirty();

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), true);
	J->SetStringField(TEXT("actor"), Volume->GetActorLabel());
	J->SetStringField(TEXT("graph"), GraphPath);
	J->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Location.X, Location.Y, Location.Z));
	J->SetBoolField(TEXT("dirtyAfter"), Volume->GetOutermost()->IsDirty());
	// <<< NWIRO_PARITY END: spawn-pcg-volume-dirty
	return JsonToString(J);
}

// ============================================================
// PCG GENERATE (force re-run on an existing volume)
// ============================================================

FString FNwiroPCGTools::PcgGenerate(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid()) return JsonToString(Fail(TEXT("Invalid JSON")));

	FString TargetLabel;
	for (const TCHAR* K : { TEXT("actor"), TEXT("name"), TEXT("label"), TEXT("volume") })
	{
		FString V;
		if (Cmd->TryGetStringField(K, V) && !V.IsEmpty()) { TargetLabel = V; break; }
	}
	if (TargetLabel.IsEmpty()) return JsonToString(Fail(TEXT("'actor' (label of the PCG volume) is required (also tried name/label/volume)")));

	UWorld* World = GetEditorWorldSafe();
	if (!World) return JsonToString(Fail(TEXT("No editor world")));

	APCGVolume* Vol = nullptr;
	for (TActorIterator<APCGVolume> It(World); It; ++It)
	{
		APCGVolume* V = *It;
		if (V && V->GetActorLabel().Equals(TargetLabel, ESearchCase::IgnoreCase)) { Vol = V; break; }
	}
	if (!Vol)
	{
		for (TActorIterator<APCGVolume> It(World); It; ++It)
		{
			APCGVolume* V = *It;
			if (V && (V->GetActorLabel().Contains(TargetLabel, ESearchCase::IgnoreCase)
				|| V->GetName().Contains(TargetLabel, ESearchCase::IgnoreCase)))
			{ Vol = V; break; }
		}
	}

	int32 Hits = 0;
	FString SkipReason;
	if (Vol)
	{
		if (UPCGComponent* Comp = Vol->FindComponentByClass<UPCGComponent>())
		{
			UPCGGraphInterface* GI = Comp->GetGraphInstance();
			UPCGGraph* G = GI ? GI->GetGraph() : nullptr;
			const bool bHasNodes = G && G->GetNodes().Num() > 0;
			if (!bHasNodes)
			{
				SkipReason = TEXT("PCGVolume's graph is empty (no nodes) — Generate would crash. Wire SpawnPoints / StaticMeshSpawner first via the PCG editor or execute_python.");
			}
			else
			{
				// >>> NWIRO_PARITY BEGIN: pcg-generate-not-transacted
				// B1 Layer 0 — OPEN QUESTION, deliberately left unchanged until a
				// runtime check settles it: Generate() runs the graph and creates
				// managed resources (ISM components, partition actors) on the
				// volume. Whether any of that Modifies or dirties the volume's
				// package is unverified, so this tool gets no transaction and no
				// MarkPackageDirty yet. Verify on a classic (non-OFPA) .umap —
				// generate, then read the volume package's IsDirty — before
				// deciding whether the transaction helper belongs here.
				// <<< NWIRO_PARITY END: pcg-generate-not-transacted
				Comp->Generate();
				Hits++;
			}
		}
	}

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), Hits > 0);
	J->SetNumberField(TEXT("regenerated"), Hits);
	if (Hits == 0)
	{
		if (!SkipReason.IsEmpty()) J->SetStringField(TEXT("error"), SkipReason);
		else J->SetStringField(TEXT("error"), FString::Printf(TEXT("No PCGVolume with label '%s' (tried exact + substring on label + UE internal name)"), *TargetLabel));
	}
	else J->SetStringField(TEXT("matched"), Vol->GetActorLabel());
	return JsonToString(J);
}

// ============================================================
// ADD PCG NODE
// ============================================================

static UClass* ResolvePcgSettingsClassPro(const FString& Requested)
{
	if (Requested.IsEmpty()) return nullptr;
	const FString WantLower = Requested.ToLower().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
	UClass* BestMatch = nullptr;
	int32 BestScore = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* C = *It;
		if (!C) continue;
		const FString CN = C->GetName();
		if (!CN.StartsWith(TEXT("PCG"), ESearchCase::IgnoreCase)) continue;
		if (!CN.EndsWith(TEXT("Settings"), ESearchCase::IgnoreCase)) continue;
		if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
		FString CnCore = CN.ToLower();
		CnCore.RemoveFromStart(TEXT("pcg"));
		CnCore.RemoveFromEnd(TEXT("settings"));
		int32 Score = 0;
		if (CnCore == WantLower) Score = 100;
		else if (CnCore.EndsWith(WantLower)) Score = 80;
		else if (CnCore.StartsWith(WantLower)) Score = 70;
		else if (CnCore.Contains(WantLower)) Score = 50;
		else continue;
		if (Score > BestScore) { BestScore = Score; BestMatch = C; }
	}
	return BestMatch;
}

FString FNwiroPCGTools::AddPcgNode(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid()) return JsonToString(Fail(TEXT("Invalid JSON")));

	auto Pick = [&](std::initializer_list<const TCHAR*> Names) -> FString {
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	const FString GraphPath = Pick({ TEXT("graph"), TEXT("assetPath"), TEXT("path"), TEXT("pcgGraph") });
	const FString NodeKind  = Pick({ TEXT("node"), TEXT("nodeType"), TEXT("type"), TEXT("kind"), TEXT("settings"), TEXT("class") });
	if (GraphPath.IsEmpty()) return JsonToString(Fail(TEXT("'graph' is required")));
	if (NodeKind.IsEmpty())  return JsonToString(Fail(TEXT("'node' is required")));

	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
	if (!Graph) return JsonToString(Fail(FString::Printf(TEXT("UPCGGraph not found: %s"), *GraphPath)));

	UClass* SettingsClass = ResolvePcgSettingsClassPro(NodeKind);
	if (!SettingsClass)
		return JsonToString(Fail(FString::Printf(TEXT("Could not resolve PCG settings class for '%s'"), *NodeKind)));

	UPCGSettings* Settings = NewObject<UPCGSettings>(Graph, SettingsClass, NAME_None, RF_Transactional);
	if (!Settings) return JsonToString(Fail(TEXT("NewObject<UPCGSettings> failed")));

	UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, Settings);
	if (!NewNode)
	{
		NewNode = NewObject<UPCGNode>(Graph, NAME_None, RF_Transactional);
		if (NewNode)
		{
			NewNode->SetSettingsInterface(Settings);
			Graph->AddNode(NewNode);
		}
	}
	if (!NewNode) return JsonToString(Fail(TEXT("AddNode failed")));

	double X = 0, Y = 0;
	if (Cmd->TryGetNumberField(TEXT("x"), X)) NewNode->PositionX = X;
	if (Cmd->TryGetNumberField(TEXT("y"), Y)) NewNode->PositionY = Y;
	Graph->MarkPackageDirty();

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), true);
	J->SetStringField(TEXT("graph"), Graph->GetPathName());
	J->SetStringField(TEXT("settingsClass"), SettingsClass->GetName());
	J->SetStringField(TEXT("nodeName"), NewNode->GetName());
	J->SetNumberField(TEXT("totalNodes"), Graph->GetNodes().Num());
	return JsonToString(J);
}
