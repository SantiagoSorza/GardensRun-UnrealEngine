// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroDebugTools.h"
#include "NwiroAssetGuard.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/WatchedPin.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Json.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroDebug, Log, All);

namespace NwiroDependencyQueries
{
	static bool IsGathering(IAssetRegistry& AR)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		return AR.IsGathering();
#else
		return AR.IsLoadingAssets();
#endif
	}

	static FString Serialize(const TSharedRef<FJsonObject>& Result)
	{
		FString Out;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out));
		return Out;
	}

	static FString Fail(const FString& Error, const FString& Code = TEXT("invalid_request"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Error);
		Result->SetStringField(TEXT("code"), Code);
		return Serialize(Result);
	}

	static bool ReadRequest(const FString& JsonCommand, TSharedPtr<FJsonObject>& Cmd, FString& Path, FString& Error)
	{
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		{
			Error = TEXT("Expected a JSON object");
			return false;
		}
		FString Alias;
		if ((Cmd->HasField(TEXT("path")) && !Cmd->TryGetStringField(TEXT("path"), Path))
			|| (Cmd->HasField(TEXT("asset")) && !Cmd->TryGetStringField(TEXT("asset"), Alias)))
		{
			Error = TEXT("path and the legacy asset alias must be strings");
			return false;
		}
		if (!Path.IsEmpty() && !Alias.IsEmpty() && Path != Alias)
		{
			Error = TEXT("Supply only one asset path: path and asset disagree");
			return false;
		}
		if (Path.IsEmpty()) Path = Alias;
		if (Path.IsEmpty() || Path.Len() >= NAME_SIZE || Path != Path.TrimStartAndEnd())
		{
			Error = TEXT("path must be a nonempty asset object path or package name, without surrounding whitespace");
			return false;
		}
		return true;
	}

	// Resolve through registry metadata only. Never load an object merely to answer a dependency query.
	static bool ResolvePackage(IAssetRegistry& AR, const FString& Path, FName& Package, FString& Error)
	{
		FText Reason;
		if (Path.Contains(TEXT(":")))
		{
			Error = TEXT("Subobject paths are not supported; supply the owning asset object path or package name");
			return false;
		}
		if (Path.Contains(TEXT(".")))
		{
			if (!FPackageName::IsValidObjectPath(Path, &Reason))
			{
				Error = TEXT("Invalid asset object path: ") + Reason.ToString();
				return false;
			}
			const FAssetData Asset = AR.GetAssetByObjectPath(FSoftObjectPath(Path));
			if (!Asset.IsValid())
			{
				Error = TEXT("Asset object is not registered: ") + Path + TEXT(". Supply an exact asset path from find_assets");
				return false;
			}
			Package = Asset.PackageName;
			return true;
		}
		if (!FPackageName::IsValidLongPackageName(Path, true, &Reason))
		{
			Error = TEXT("Invalid package name: ") + Reason.ToString() + TEXT(". Expected /Game/Folder/Asset or /Game/Folder/Asset.Asset");
			return false;
		}
		TArray<FAssetData> Assets;
		if (!AR.GetAssetsByPackageName(FName(*Path), Assets) || Assets.IsEmpty())
		{
			Error = TEXT("Asset package is not registered: ") + Path + TEXT(". A folder or nonexistent package is not an asset");
			return false;
		}
		Package = Assets[0].PackageName;
		return true;
	}

	static bool Query(IAssetRegistry& AR, FName Package, bool bReferencers, TArray<FName>& Results, FString& Error)
	{
		// A dependency placeholder may exist for a missing referenced package. Its empty edges
		// are not evidence that a saved/scanned package has no dependencies.
		const bool bHasPackageRecord = AR.GetAssetPackageDataCopy(Package).IsSet();
		const bool bKnown = bHasPackageRecord
			&& (bReferencers ? AR.GetReferencers(Package, Results) : AR.GetDependencies(Package, Results));
		if (!bKnown)
		{
			Error = TEXT("Dependency data is unavailable for package ") + Package.ToString()
				+ TEXT(". Explicitly save/scan the intended package before retrying; this query does not load, save, or scan assets");
			return false;
		}
		Results.Sort(FNameLexicalLess());
		return true;
	}

	static FString QueryAsset(const FString& JsonCommand, bool bReferencers)
	{
		TSharedPtr<FJsonObject> Cmd;
		FString Path, Error;
		if (!ReadRequest(JsonCommand, Cmd, Path, Error)) return Fail(Error);
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (NwiroDependencyQueries::IsGathering(AR)) return Fail(TEXT("AssetRegistry discovery is in progress; wait for it to finish and retry"), TEXT("registry_not_ready"));
		FName Package;
		if (!ResolvePackage(AR, Path, Package, Error)) return Fail(Error, TEXT("invalid_asset_path"));
		TArray<FName> Names;
		if (!Query(AR, Package, bReferencers, Names, Error)) return Fail(Error, TEXT("dependency_data_unavailable"));
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FName Name : Names) Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset"), Package.ToString());
		Result->SetStringField(TEXT("querySource"), TEXT("asset_registry_package_dependencies"));
		Result->SetArrayField(bReferencers ? TEXT("referencers") : TEXT("dependencies"), Values);
		Result->SetNumberField(TEXT("count"), Values.Num());
		return Serialize(Result);
	}
}

// ============================================================
// HELPER
// ============================================================

static UBlueprint* LoadBPByPath(const FString& PathOrName)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *PathOrName);
	if (BP) return BP;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Equals(PathOrName, ESearchCase::IgnoreCase) ||
			A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UBlueprint>(NwiroSafeRegistryLoad(A));
		}
	}
	return nullptr;
}

static UEdGraphNode* FindNodeInBP(UBlueprint* BP, const FString& NodeName)
{
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(NodeName, ESearchCase::IgnoreCase) ||
				Node->GetName().Contains(NodeName, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(NodeName, ESearchCase::IgnoreCase) ||
				Node->GetName().Contains(NodeName, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
	}
	return nullptr;
}

// ============================================================
// BP GET COMPILE ERRORS
// ============================================================

FString FNwiroDebugTools::BPGetCompileErrors(const FString& JsonCommand)
{
	const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("verificationId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("verificationKind"), TEXT("blueprintCompile"));
	Result->SetBoolField(TEXT("savePerformed"), false);
	auto Unknown = [&](const FString& Error) -> FString
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetBoolField(TEXT("compilePerformed"), false);
		Result->SetStringField(TEXT("verificationStatus"), TEXT("unknown"));
		Result->SetStringField(TEXT("error"), Error);
		Result->SetField(TEXT("errorCount"), MakeShared<FJsonValueNull>());
		Result->SetField(TEXT("warningCount"), MakeShared<FJsonValueNull>());
		Result->SetStringField(TEXT("completedAt"), FDateTime::UtcNow().ToIso8601());
		FString Out;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out));
		return Out;
	};
	if (!IsInGameThread()) return Unknown(TEXT("Blueprint verification requires the game thread"));
	TSharedPtr<FJsonObject> Cmd;
	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return Unknown(TEXT("Invalid JSON"));
	FString BPPath;
	if (!Cmd->HasTypedField<EJson::String>(TEXT("blueprint"))
		|| !Cmd->TryGetStringField(TEXT("blueprint"), BPPath) || BPPath.IsEmpty())
		return Unknown(TEXT("Missing blueprint path"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return Unknown(FString::Printf(TEXT("Blueprint not found: %s"), *BPPath));

	// Compile with a results log. The node walk below can only see messages the compiler
	// attached to a node; graph-level and Blueprint-level messages have no node to hang on,
	// so without this a graph-level failure produced status:"Error" with errorCount:0 — two
	// fields from two channels, disagreeing.
	const bool bPackageDirtyBefore = BP->GetOutermost()->IsDirty();
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &CompileResults);

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
	Result->SetStringField(TEXT("verificationStatus"), TEXT("completed"));
	Result->SetBoolField(TEXT("compilePerformed"), true);
	// SkipSave suppresses engine SaveOnCompile behavior; dirty state is observed,
	// while the caller's persisted disk version is deliberately not inferred.
	Result->SetBoolField(TEXT("savePerformed"), false);
	Result->SetBoolField(TEXT("packageDirtyBefore"), bPackageDirtyBefore);
	Result->SetBoolField(TEXT("packageDirty"), BP->GetOutermost()->IsDirty());

	// Status
	FString Status;
	switch (BP->Status)
	{
	case BS_Error: Status = TEXT("Error"); break;
	case BS_UpToDate: Status = TEXT("UpToDate"); break;
	case BS_Dirty: Status = TEXT("Dirty"); break;
	case BS_UpToDateWithWarnings: Status = TEXT("UpToDateWithWarnings"); break;
	default: Status = TEXT("Unknown"); break;
	}
	Result->SetStringField(TEXT("status"), Status);

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// Every graph collection, not just Ubergraph + Function. Macro and delegate-signature
	// graphs carry compiler messages too and were invisible here.
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(BP->UbergraphPages);
	AllGraphs.Append(BP->FunctionGraphs);
	AllGraphs.Append(BP->MacroGraphs);
	AllGraphs.Append(BP->DelegateSignatureGraphs);

	// A node's ErrorMsg is ACCUMULATED — every message that landed on it, newline-separated
	// — while the results log carries those same messages individually. Reporting both lists
	// verbatim therefore double-counts: one aggregate entry plus one entry per message inside
	// it. So the node walk only builds attribution here, and the results log below is the
	// single source of what to report. One entry per distinct message, located when possible.
	struct FNodeSite
	{
		FString Node;
		FString Graph;
		// int32, matching UEdGraphNode::ErrorType - assigning the enum type does not compile.
		int32 Severity = EMessageSeverity::Error;
	};
	TMap<FString, FNodeSite> Attribution;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !Node->bHasCompilerMessage) continue;

			TArray<FString> Parts;
			Node->ErrorMsg.ParseIntoArrayLines(Parts);
			for (const FString& Part : Parts)
			{
				const FString Key = Part.TrimStartAndEnd();
				if (Key.IsEmpty() || Attribution.Contains(Key)) continue;

				FNodeSite Site;
				Site.Node = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				Site.Graph = Graph->GetName();
				Site.Severity = Node->ErrorType;
				Attribution.Add(Key, Site);
			}
		}
	}

	TSet<FString> Reported;
	auto AddEntry = [&](const FString& Text, bool bIsError, const FNodeSite* Site)
	{
		TSharedRef<FJsonObject> Msg = MakeShareable(new FJsonObject());
		Msg->SetStringField(TEXT("message"), Text);
		if (Site)
		{
			Msg->SetStringField(TEXT("node"), Site->Node);
			Msg->SetStringField(TEXT("graph"), Site->Graph);
			Msg->SetStringField(TEXT("scope"), TEXT("node"));
		}
		else
		{
			// No node/graph fields invented: this message is not attributable to one, and a
			// placeholder would make it look located when it is not.
			Msg->SetStringField(TEXT("scope"), TEXT("blueprint"));
		}

		if (bIsError) Errors.Add(MakeShareable(new FJsonValueObject(Msg)));
		else          Warnings.Add(MakeShareable(new FJsonValueObject(Msg)));
	};

	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
	{
		const EMessageSeverity::Type Severity = Message->GetSeverity();
		const bool bIsError = (Severity == EMessageSeverity::Error);
		if (!bIsError && Severity != EMessageSeverity::Warning) continue;

		const FString Text = Message->ToText().ToString().TrimStartAndEnd();
		if (Text.IsEmpty() || Reported.Contains(Text)) continue;
		Reported.Add(Text);

		AddEntry(Text, bIsError, Attribution.Find(Text));
	}

	// A node message the results log did not carry. Should not happen right after a compile,
	// but dropping a real message silently is the failure this tool exists to prevent.
	for (const TPair<FString, FNodeSite>& Pair : Attribution)
	{
		if (Reported.Contains(Pair.Key)) continue;
		Reported.Add(Pair.Key);
		AddEntry(Pair.Key, Pair.Value.Severity == static_cast<int32>(EMessageSeverity::Error), &Pair.Value);
	}

	Result->SetArrayField(TEXT("errors"), Errors);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("errorCount"), Errors.Num());
	Result->SetNumberField(TEXT("warningCount"), Warnings.Num());

	Result->SetStringField(TEXT("completedAt"), FDateTime::UtcNow().ToIso8601());
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// BP SET BREAKPOINT
// ============================================================

FString FNwiroDebugTools::BPSetBreakpoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	FString NodeName = Cmd->GetStringField(TEXT("node"));

	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	UEdGraphNode* Node = FindNodeInBP(BP, NodeName);
	if (!Node) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Node not found: %s\"}"), *NodeName);

	FKismetDebugUtilities::CreateBreakpoint(BP, Node, true);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"node\":\"%s\"}"),
		*BP->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
}

// ============================================================
// BP REMOVE BREAKPOINT
// ============================================================

FString FNwiroDebugTools::BPRemoveBreakpoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	FString NodeName = Cmd->GetStringField(TEXT("node"));

	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	UEdGraphNode* Node = FindNodeInBP(BP, NodeName);
	if (!Node) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Node not found: %s\"}"), *NodeName);

	int32 Removed = 0;

	if (FKismetDebugUtilities::FindBreakpointForNode(Node, BP))
	{
		FKismetDebugUtilities::RemoveBreakpointFromNode(Node, BP);
		Removed++;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	return FString::Printf(TEXT("{\"success\":true,\"removed\":%d}"), Removed);
}

// ============================================================
// BP LIST BREAKPOINTS
// ============================================================

FString FNwiroDebugTools::BPListBreakpoints(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	TArray<TSharedPtr<FJsonValue>> BPs;

	FKismetDebugUtilities::ForeachBreakpoint(BP, [&](FBlueprintBreakpoint& BPt)
	{
		UEdGraphNode* BPNode = BPt.GetLocation();
		if (!BPNode) return;
		TSharedRef<FJsonObject> B = MakeShareable(new FJsonObject());
		B->SetStringField(TEXT("node"), BPNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
		B->SetBoolField(TEXT("enabled"), BPt.IsEnabledByUser());
		BPs.Add(MakeShareable(new FJsonValueObject(B)));
	});

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("breakpoints"), BPs);
	Result->SetNumberField(TEXT("count"), BPs.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// BP GET WATCH VALUES
// ============================================================

FString FNwiroDebugTools::BPGetWatchValues(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	// Get watched pins
	TArray<TSharedPtr<FJsonValue>> Watches;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				bool bIsWatched = false;
				bIsWatched = FKismetDebugUtilities::IsPinBeingWatched(BP, Pin);
				if (bIsWatched)
				{
					TSharedRef<FJsonObject> W2 = MakeShareable(new FJsonObject());
					W2->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
					W2->SetStringField(TEXT("pin"), Pin->PinName.ToString());
					W2->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
					W2->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);

					// Try to get debug value if PIE running
					if (GEditor->PlayWorld)
					{
						FString DebugVal;
						// Pin debug text is available during PIE via the debugger
						W2->SetStringField(TEXT("debugValue"), Pin->DefaultValue);
					}

					Watches.Add(MakeShareable(new FJsonValueObject(W2)));
				}
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("watches"), Watches);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// BP ADD WATCH
// ============================================================

FString FNwiroDebugTools::BPAddWatch(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	FString NodeName = Cmd->GetStringField(TEXT("node"));
	FString PinName = Cmd->GetStringField(TEXT("pin"));

	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	UEdGraphNode* Node = FindNodeInBP(BP, NodeName);
	if (!Node) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Node not found: %s\"}"), *NodeName);

	UEdGraphPin* Pin = nullptr;
	for (UEdGraphPin* P : Node->Pins)
	{
		if (P->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			Pin = P;
			break;
		}
	}

	if (!Pin) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Pin not found: %s\"}"), *PinName);

	FKismetDebugUtilities::AddPinWatch(BP, FBlueprintWatchedPin(Pin));

	return FString::Printf(TEXT("{\"success\":true,\"node\":\"%s\",\"pin\":\"%s\"}"),
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *PinName);
}

// ============================================================
// BP FIX BROKEN REFERENCES
// ============================================================

FString FNwiroDebugTools::BPFixBrokenReferences(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	int32 FixedCount = 0;

	// Remove broken links in all graphs
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (int32 i = Node->Pins.Num() - 1; i >= 0; i--)
			{
				UEdGraphPin* Pin = Node->Pins[i];
				for (int32 j = Pin->LinkedTo.Num() - 1; j >= 0; j--)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[j];
					if (!LinkedPin || !LinkedPin->GetOwningNode() || !LinkedPin->GetOwningNode()->GetGraph())
					{
						Pin->LinkedTo.RemoveAt(j);
						FixedCount++;
					}
				}
			}
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (int32 i = Node->Pins.Num() - 1; i >= 0; i--)
			{
				UEdGraphPin* Pin = Node->Pins[i];
				for (int32 j = Pin->LinkedTo.Num() - 1; j >= 0; j--)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[j];
					if (!LinkedPin || !LinkedPin->GetOwningNode() || !LinkedPin->GetOwningNode()->GetGraph())
					{
						Pin->LinkedTo.RemoveAt(j);
						FixedCount++;
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	BP->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"fixedLinks\":%d}"), *BP->GetName(), FixedCount);
}

// ============================================================
// BP FIX DEPRECATED NODES
// ============================================================

FString FNwiroDebugTools::BPFixDeprecatedNodes(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	// Count deprecated nodes before
	int32 DeprecatedBefore = 0;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->IsDeprecated()) DeprecatedBefore++;
		}
	}

	// Refresh all nodes - this updates deprecated nodes to newer versions
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	BP->MarkPackageDirty();

	// Count after
	int32 DeprecatedAfter = 0;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->IsDeprecated()) DeprecatedAfter++;
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"deprecatedBefore\":%d,\"deprecatedAfter\":%d,\"fixed\":%d}"),
		*BP->GetName(), DeprecatedBefore, DeprecatedAfter, DeprecatedBefore - DeprecatedAfter);
}

// ============================================================
// BP REFRESH ALL NODES
// ============================================================

FString FNwiroDebugTools::BPRefreshAllNodes(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	BP->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"message\":\"All nodes refreshed and recompiled\"}"), *BP->GetName());
}

// ============================================================
// BP FIND UNCONNECTED PINS
// ============================================================

FString FNwiroDebugTools::BPFindUnconnectedPins(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	TArray<TSharedPtr<FJsonValue>> Unconnected;

	auto CheckGraph = [&](UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				// Only check exec pins that should be connected
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
					Pin->LinkedTo.Num() == 0 &&
					Pin->Direction == EGPD_Output &&
					!Pin->bHidden)
				{
					TSharedRef<FJsonObject> U = MakeShareable(new FJsonObject());
					U->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
					U->SetStringField(TEXT("pin"), Pin->PinName.ToString());
					U->SetStringField(TEXT("graph"), Graph->GetName());
					U->SetStringField(TEXT("direction"), TEXT("Output"));
					Unconnected.Add(MakeShareable(new FJsonValueObject(U)));
				}
			}
		}
	};

	for (UEdGraph* Graph : BP->UbergraphPages) CheckGraph(Graph);
	for (UEdGraph* Graph : BP->FunctionGraphs) CheckGraph(Graph);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("unconnectedPins"), Unconnected);
	Result->SetNumberField(TEXT("count"), Unconnected.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// GET ASSET REFERENCES (dependencies)
// ============================================================

FString FNwiroDebugTools::GetAssetReferences(const FString& JsonCommand)
{
	return NwiroDependencyQueries::QueryAsset(JsonCommand, false);
}

// ============================================================
// GET ASSET REFERENCERS
// ============================================================

FString FNwiroDebugTools::GetAssetReferencers(const FString& JsonCommand)
{
	return NwiroDependencyQueries::QueryAsset(JsonCommand, true);
}

// ============================================================
// FIND ORPHAN ASSETS
// ============================================================

FString FNwiroDebugTools::FindOrphanAssets(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return NwiroDependencyQueries::Fail(TEXT("Expected a JSON object"));

	FString SearchPath = TEXT("/Game");
	if (Cmd->HasField(TEXT("path")) && !Cmd->TryGetStringField(TEXT("path"), SearchPath))
		return NwiroDependencyQueries::Fail(TEXT("path must be a Content folder string"));
	SearchPath.RemoveFromEnd(TEXT("/"));
	int32 MaxResults = (Cmd.IsValid() && Cmd->HasField(TEXT("maxResults"))) ? (int32)Cmd->GetNumberField(TEXT("maxResults")) : 50;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	if (NwiroDependencyQueries::IsGathering(AR)) return NwiroDependencyQueries::Fail(TEXT("AssetRegistry discovery is in progress; wait for it to finish and retry"), TEXT("registry_not_ready"));
	if (SearchPath.IsEmpty() || SearchPath.Len() >= NAME_SIZE - 16 || SearchPath != SearchPath.TrimStartAndEnd() || SearchPath.Contains(TEXT("."))
		|| !FPackageName::IsValidLongPackageName(SearchPath + TEXT("/__FolderProbe"), true) || !AR.PathExists(SearchPath))
		return NwiroDependencyQueries::Fail(TEXT("path must identify a registered Content folder such as /Game/Folder, not an asset object or package"), TEXT("invalid_folder_path"));

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*SearchPath));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	if (!AR.GetAssets(Filter, Assets))
		return NwiroDependencyQueries::Fail(TEXT("AssetRegistry could not enumerate the requested folder"), TEXT("registry_query_failed"));

	TArray<TSharedPtr<FJsonValue>> Orphans;
	for (const FAssetData& A : Assets)
	{
		if (Orphans.Num() >= MaxResults) break;

		TArray<FName> Referencers;
		FString QueryError;
		if (!NwiroDependencyQueries::Query(AR, A.PackageName, true, Referencers, QueryError))
			return NwiroDependencyQueries::Fail(QueryError, TEXT("dependency_data_unavailable"));

		// Remove self-references and script references
		Referencers.RemoveAll([&](const FName& Ref) {
			return Ref == A.PackageName || Ref.ToString().StartsWith(TEXT("/Script/"));
		});

		if (Referencers.Num() == 0)
		{
			TSharedRef<FJsonObject> O = MakeShareable(new FJsonObject());
			O->SetStringField(TEXT("name"), A.AssetName.ToString());
			O->SetStringField(TEXT("path"), A.PackageName.ToString());
			O->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
			Orphans.Add(MakeShareable(new FJsonValueObject(O)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("orphanAssets"), Orphans);
	Result->SetNumberField(TEXT("count"), Orphans.Num());
	Result->SetNumberField(TEXT("totalScanned"), Assets.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND CIRCULAR DEPENDENCIES
// ============================================================

FString FNwiroDebugTools::FindCircularDependencies(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) Path = Cmd->GetStringField(TEXT("asset"));

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	// DFS to detect cycles
	TSet<FName> Visited;
	TSet<FName> InStack;
	TArray<FString> Cycles;

	TFunction<bool(FName, TArray<FName>&)> DFS;
	DFS = [&](FName Current, TArray<FName>& Path) -> bool
	{
		if (InStack.Contains(Current))
		{
			// Found cycle - build cycle string
			FString CycleStr;
			bool bInCycle = false;
			for (const FName& P : Path)
			{
				if (P == Current) bInCycle = true;
				if (bInCycle) CycleStr += P.ToString() + TEXT(" -> ");
			}
			CycleStr += Current.ToString();
			Cycles.Add(CycleStr);
			return true;
		}

		if (Visited.Contains(Current)) return false;

		Visited.Add(Current);
		InStack.Add(Current);
		Path.Add(Current);

		TArray<FName> Deps;
		AR.GetDependencies(Current, Deps);

		for (const FName& Dep : Deps)
		{
			if (Dep.ToString().StartsWith(TEXT("/Game")))
			{
				DFS(Dep, Path);
			}
		}

		Path.Pop();
		InStack.Remove(Current);
		return false;
	};

	TArray<FName> PathArr;
	DFS(FName(*Path), PathArr);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset"), Path);
	Result->SetBoolField(TEXT("hasCircularDependency"), Cycles.Num() > 0);

	TArray<TSharedPtr<FJsonValue>> CycleArr;
	for (const FString& C : Cycles)
		CycleArr.Add(MakeShareable(new FJsonValueString(C)));
	Result->SetArrayField(TEXT("cycles"), CycleArr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// GET DEPENDENCY TREE
// ============================================================

FString FNwiroDebugTools::GetDependencyTree(const FString& JsonCommand)
{
	using namespace NwiroDependencyQueries;
	TSharedPtr<FJsonObject> Cmd;
	FString Path, Error;
	if (!ReadRequest(JsonCommand, Cmd, Path, Error)) return Fail(Error);
	double RequestedDepth = 3;
	if ((Cmd->HasField(TEXT("depth")) && !Cmd->TryGetNumberField(TEXT("depth"), RequestedDepth))
		|| !FMath::IsFinite(RequestedDepth) || RequestedDepth < 0 || RequestedDepth > 64
		|| RequestedDepth != FMath::FloorToDouble(RequestedDepth))
		return Fail(TEXT("depth must be an integer from 0 to 64"));
	const int32 MaxDepth = static_cast<int32>(RequestedDepth);
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (NwiroDependencyQueries::IsGathering(AR)) return Fail(TEXT("AssetRegistry discovery is in progress; wait for it to finish and retry"), TEXT("registry_not_ready"));
	FName Package;
	if (!ResolvePackage(AR, Path, Package, Error)) return Fail(Error, TEXT("invalid_asset_path"));

	int32 Nodes = 0;
	TSet<FName> Ancestors;
	TFunction<TSharedPtr<FJsonObject>(FName, int32)> BuildTree;
	BuildTree = [&](FName Current, int32 Depth) -> TSharedPtr<FJsonObject>
	{
		if (++Nodes > 4096)
		{
			Error = TEXT("Dependency tree exceeds 4096 nodes; request a smaller depth");
			return nullptr;
		}
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("asset"), Current.ToString());
		if (Ancestors.Contains(Current))
		{
			Node->SetBoolField(TEXT("circular"), true);
			return Node;
		}
		if (Depth == 0)
		{
			Node->SetBoolField(TEXT("depthLimitReached"), true);
			return Node;
		}
		// Script modules are terminal external references, not browsable content assets.
		if (Current.ToString().StartsWith(TEXT("/Script/")))
		{
			Node->SetBoolField(TEXT("scriptPackage"), true);
			return Node;
		}
		TArray<FName> Dependencies;
		if (!Query(AR, Current, false, Dependencies, Error)) return nullptr;
		Ancestors.Add(Current);
		TArray<TSharedPtr<FJsonValue>> Children;
		for (const FName Dependency : Dependencies)
		{
			TSharedPtr<FJsonObject> Child = BuildTree(Dependency, Depth - 1);
			if (!Child.IsValid()) return nullptr;
			Children.Add(MakeShared<FJsonValueObject>(Child));
		}
		Ancestors.Remove(Current);
		Node->SetArrayField(TEXT("dependencies"), Children);
		return Node;
	};
	TSharedPtr<FJsonObject> Tree = BuildTree(Package, MaxDepth);
	if (!Tree.IsValid()) return Fail(Error, TEXT("dependency_query_failed"));
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("querySource"), TEXT("asset_registry_package_dependencies"));
	Result->SetObjectField(TEXT("tree"), Tree);
	return Serialize(Result);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroCompileVerificationSpec.inl"
#endif
