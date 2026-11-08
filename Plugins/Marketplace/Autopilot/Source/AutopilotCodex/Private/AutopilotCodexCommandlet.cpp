// The AutopilotCodex commandlet (see AutopilotCodexCommandlet.h): Main finds the project's assets and writes the export.
// What each kind of asset reads as: AutopilotCodexBlueprints.cpp, AutopilotCodexInputData.cpp, AutopilotCodexMaps.cpp; the
// shared text helpers: AutopilotCodexText.cpp.

#include "AutopilotCodexCommandlet.h"
#include "Misc/EngineVersionComparison.h"

#include "AutopilotCodexInternal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Curves/CurveBase.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/CurveTable.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "Engine/UserDefinedStruct.h"
#else
#include "StructUtils/UserDefinedStruct.h"
#endif
#include "UObject/Package.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogAutopilotCodex, Log, All);

UAutopilotCodexCommandlet::UAutopilotCodexCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = false;
	// The game's own assets may log errors on load (broken Blueprints); those go to summary.txt, not the exit code.
	// (Before 5.3 the commandlet's result is the exit code already.)
#if !UE_VERSION_OLDER_THAN(5, 3, 0)
	UseCommandletResultAsExitCode = true;
#endif
}

// ---------------------------------------------------------------- crash guard

#if PLATFORM_WINDOWS
namespace AutopilotCodexSeh
{
	static void Invoke(void* Ctx)
	{
		(*static_cast<TFunctionRef<void()>*>(Ctx))();
	}

	static bool Call(void (*Fn)(void*), void* Ctx)
	{
		__try
		{
			Fn(Ctx);
			return true;
		}
		__except (1 /* EXCEPTION_EXECUTE_HANDLER */)
		{
			return false;
		}
	}
}
#endif

static bool Guarded(TFunctionRef<void()> F)
{
#if PLATFORM_WINDOWS
	return AutopilotCodexSeh::Call(&AutopilotCodexSeh::Invoke, &F);
#else
	F();
	return true;
#endif
}

// ---------------------------------------------------------------- commandlet

int32 UAutopilotCodexCommandlet::Main(const FString& Params)
{
	using namespace AutopilotCodex;
	const double Start = FPlatformTime::Seconds();

	FString OutDir;
	if (!FParse::Value(*Params, TEXT("out="), OutDir) || OutDir.IsEmpty())
	{
		UE_LOG(LogAutopilotCodex, Error, TEXT("AutopilotCodex: -out=<dir> is required"));
		return 1;
	}
	OutDir = FPaths::ConvertRelativePathToFull(OutDir);
	FString RootsParam;
	TArray<FString> Roots;
	if (FParse::Value(*Params, TEXT("root="), RootsParam) && !RootsParam.IsEmpty())
	{
		RootsParam.ParseIntoArray(Roots, TEXT(","), true);
	}
	else
	{
		Roots.Add(TEXT("/Game"));
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
		{
			if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project && Plugin->GetName() != TEXT("Autopilot"))
			{
				Roots.Add(TEXT("/") + Plugin->GetMountedAssetPath().TrimChar(TEXT('/')));
			}
		}
	}
	int32 MaxMaps = 50;
	FParse::Value(*Params, TEXT("maxmaps="), MaxMaps);
	const bool bNoMaps = FParse::Param(*Params, TEXT("nomaps"));
	const bool bNoWpLoad = FParse::Param(*Params, TEXT("nowpload"));
	FString ProgressFile;
	FParse::Value(*Params, TEXT("progress="), ProgressFile);
	// The level the run plays (a package or object path): it and what it uses are exported first
	FString FirstMap;
	FParse::Value(*Params, TEXT("first="), FirstMap);

	// Replace an earlier export: only the folders and files this commandlet writes. Its files stay until this export has
	// written its own (the brain reads them while it runs: an unchanged asset's file is the same); those it didn't write
	// again go at the end. summary.txt goes now: it is there only when an export has finished.
	IFileManager::Get().Delete(*FPaths::Combine(OutDir, TEXT("summary.txt")), false, true, true);
	if (bNoMaps)
	{
		IFileManager::Get().Delete(*FPaths::Combine(OutDir, TEXT("maps.txt")), false, true, true);
	}
	TSet<FString> Earlier;
	for (const TCHAR* Sub : { TEXT("blueprints"), TEXT("input"), TEXT("data") })
	{
		IFileManager::Get().IterateDirectoryRecursively(*FPaths::Combine(OutDir, Sub), [&Earlier](const TCHAR* Found, bool bIsDirectory)
		{
			if (!bIsDirectory)
			{
				Earlier.Add(FPaths::ConvertRelativePathToFull(Found));
			}
			return true;
		});
	}
	IFileManager::Get().MakeDirectory(*OutDir, true);
	TArray<FString> Failures;
	TMap<FString, int32> Counts;
	int64 BytesWritten = 0;

	auto Write = [&](const FString& Rel, const FString& Text)
	{
		const FString Path = FPaths::Combine(OutDir, Rel);
		Earlier.Remove(FPaths::ConvertRelativePathToFull(Path));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			BytesWritten += Text.Len();
		}
		else
		{
			Failures.Add(Rel + TEXT(": could not write file"));
		}
	};

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	AR.SearchAllAssets(true);

	FARFilter Filter;
	for (const FString& R : Roots)
	{
		Filter.PackagePaths.Add(FName(*R));
	}
	Filter.bRecursivePaths = true;
	Filter.bIncludeOnlyOnDiskAssets = true;
	TArray<FAssetData> All;
	AR.GetAssets(Filter, All);
	TArray<FAssetData> Assets;
	for (const FAssetData& A : All)
	{
		const FString P = A.PackageName.ToString();
		if (!P.Contains(TEXT("/__ExternalActors__/")) && !P.Contains(TEXT("/__ExternalObjects__/")))
		{
			Assets.Add(A);
		}
	}
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

	{
		TArray<FString> Lines;
		for (const FAssetData& A : Assets)
		{
			Lines.Add(A.PackageName.ToString() + TEXT("  ") + A.AssetClassPath.GetAssetName().ToString());
		}
		Write(TEXT("assets.txt"), FString::Join(Lines, TEXT("\n")) + TEXT("\n"));
		Counts.Add(TEXT("assets"), Assets.Num());
	}
	UE_LOG(LogAutopilotCodex, Display, TEXT("AutopilotCodex: %d assets under %s"), Assets.Num(), *FString::Join(Roots, TEXT(",")));

	// Resolve each asset's class (loading Blueprint classes for Blueprint-derived data assets).
	TMap<FTopLevelAssetPath, UClass*> ClassCache;
	auto ResolveClass = [&ClassCache](const FTopLevelAssetPath& Path) -> UClass*
	{
		if (UClass** Found = ClassCache.Find(Path))
		{
			return *Found;
		}
		UClass* C = FindObject<UClass>(Path);
		if (!C && !Path.GetPackageName().ToString().StartsWith(TEXT("/Script/")))
		{
			C = LoadObject<UClass>(nullptr, *Path.ToString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (C)
		{
			C->AddToRoot();  // a handful of classes; keeps the cache valid across GC
		}
		ClassCache.Add(Path, C);
		return C;
	};

	// The assets to export (of the kinds read below) and the maps, known before any asset is loaded: how far the export
	// is can be told (-progress)
	TArray<FAssetData> Maps;
	TArray<FAssetData> ToExport;
	int32 SinceGc = 0;
	for (const FAssetData& A : Assets)
	{
		const FTopLevelAssetPath ClassPath = A.AssetClassPath;
		if (ClassPath == UWorld::StaticClass()->GetClassPathName())
		{
			Maps.Add(A);
		}
		else if (!ClassPath.GetPackageName().ToString().StartsWith(TEXT("/Script/")))
		{
			// A Blueprint class's instance: exported when it is a data asset. The class is loaded to know (a crash
			// here keeps the asset: its export meets it again below, and notes it)
			bool bDataAsset = true;
			const bool bLoads = !ClassCache.Contains(ClassPath);
			Guarded([&]()
			{
				UClass* C = ResolveClass(ClassPath);
				bDataAsset = C && C->IsChildOf(UDataAsset::StaticClass());
			});
			if (bDataAsset)
			{
				ToExport.Add(A);
			}
			if (bLoads && ++SinceGc >= GcEvery)
			{
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				SinceGc = 0;
			}
		}
		else if (UClass* C = ResolveClass(ClassPath))
		{
			if (C->IsChildOf(UBlueprint::StaticClass()) || C->IsChildOf(UInputAction::StaticClass())
				|| C->IsChildOf(UInputMappingContext::StaticClass()) || C->IsChildOf(UDataTable::StaticClass())
				|| C->IsChildOf(UCurveTable::StaticClass()) || C->IsChildOf(UUserDefinedEnum::StaticClass())
				|| C->IsChildOf(UUserDefinedStruct::StaticClass()) || C->IsChildOf(UCurveBase::StaticClass())
				|| C->IsChildOf(UDataAsset::StaticClass()))
			{
				ToExport.Add(A);
			}
		}
	}
	// What the game plays comes first: the level the run plays, the project's default and startup maps and game mode, and
	// the packages they use, nearest first (by the asset registry's dependencies; a World Partition map's actors are
	// packages of their own). The rest (asset packs' demos and the like) follow in name order. The brain reads the export
	// as it is written, so the order decides what it has early, not what it gets
	{
		// (an object path's package: /Game/Maps/Arena.Arena -> /Game/Maps/Arena)
		auto PackageOf = [](FString Path)
		{
			Path.TrimStartAndEndInline();
			int32 Slash = INDEX_NONE;
			Path.FindLastChar(TEXT('/'), Slash);
			const int32 Dot = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, FMath::Max(Slash, 0));
			if (Dot != INDEX_NONE)
			{
				Path.LeftInline(Dot);
			}
			return Path;
		};
		TArray<FString> Starts;
		if (!FirstMap.IsEmpty())
		{
			Starts.Add(PackageOf(FirstMap));
		}
		for (const TCHAR* Key : { TEXT("GameDefaultMap"), TEXT("EditorStartupMap"), TEXT("GlobalDefaultGameMode") })
		{
			FString Value;
			if (GConfig && GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), Key, Value, GEngineIni) && Value.StartsWith(TEXT("/")))
			{
				Starts.Add(PackageOf(Value));
			}
		}
		TMap<FName, int32> Depth;
		TArray<FName> Queue;
		auto Reach = [&Depth, &Queue](const FName Pkg, int32 D)
		{
			if (!Depth.Contains(Pkg))
			{
				Depth.Add(Pkg, D);
				Queue.Add(Pkg);
			}
		};
		for (const FString& S : Starts)
		{
			if (S.StartsWith(TEXT("/Script/")))
			{
				continue;
			}
			Reach(FName(*S), 0);
			FARFilter AF;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
			for (const FString& Path : TArray<FString>{ULevel::GetExternalActorsPath(S)})
#else
			for (const FString& Path : ULevel::GetExternalActorsPaths(S))
#endif
			{
				AF.PackagePaths.Add(FName(*Path));
			}
			AF.bRecursivePaths = true;
			AF.bIncludeOnlyOnDiskAssets = true;
			TArray<FAssetData> ActorAssets;
			AR.GetAssets(AF, ActorAssets);
			for (const FAssetData& AA : ActorAssets)
			{
				Reach(AA.PackageName, 1);
			}
		}
		for (int32 i = 0; i < Queue.Num(); ++i)
		{
			TArray<FName> Deps;
			AR.GetDependencies(Queue[i], Deps);
			const int32 D = Depth[Queue[i]] + 1;
			for (const FName Dep : Deps)
			{
				const FString P = Dep.ToString();
				if (!P.StartsWith(TEXT("/Script/")) && !P.StartsWith(TEXT("/Engine/")))
				{
					Reach(Dep, D);
				}
			}
		}
		auto Rank = [&Depth](const FAssetData& A)
		{
			const int32* D = Depth.Find(A.PackageName);
			return D ? *D : MAX_int32;
		};
		ToExport.StableSort([&Rank](const FAssetData& A, const FAssetData& B) { return Rank(A) < Rank(B); });
		Maps.StableSort([&Rank](const FAssetData& A, const FAssetData& B) { return Rank(A) < Rank(B); });
		UE_LOG(LogAutopilotCodex, Display, TEXT("AutopilotCodex: %d packages reached from %s, exported first"), Depth.Num(), *FString::Join(Starts, TEXT(", ")));
	}
	const int32 Total = ToExport.Num() + (bNoMaps ? 0 : FMath::Min(Maps.Num(), FMath::Max(MaxMaps, 0)));
	int32 Done = 0;
	double ProgressAt = 0;
	// (at most 4 times a second, and at the start and the end)
	auto Progress = [&](bool bNow)
	{
		const double Now = FPlatformTime::Seconds();
		if (ProgressFile.IsEmpty() || (!bNow && Now - ProgressAt < 0.25))
		{
			return;
		}
		ProgressAt = Now;
		// (a map that failed to load lets one more be tried than counted: never more done than the total)
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d %d"), FMath::Min(Done, Total), Total), *ProgressFile);
	};
	UE_LOG(LogAutopilotCodex, Display, TEXT("AutopilotCodex: %d files to export"), Total);
	Progress(true);

	for (const FAssetData& A : ToExport)
	{
		const FString Pkg = A.PackageName.ToString();
		FString Category;
		bool bOk = true;
		const bool bNoCrash = Guarded([&]()
		{
			UObject* Obj = A.GetAsset();
			if (!Obj)
			{
				bOk = false;
				Failures.Add(Pkg + TEXT(": failed to load"));
				return;
			}
			++SinceGc;
			const FString Rel = RelPath(Pkg);
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				Category = TEXT("blueprints");
				Write(TEXT("blueprints/") + Rel + TEXT(".txt"), ExportBlueprint(BP, Pkg));
			}
			else if (UInputAction* IA = Cast<UInputAction>(Obj))
			{
				Category = TEXT("input actions");
				Write(TEXT("input/") + Rel + TEXT(".txt"), ExportInputAction(IA, Pkg));
			}
			else if (UInputMappingContext* IMC = Cast<UInputMappingContext>(Obj))
			{
				Category = TEXT("input mapping contexts");
				Write(TEXT("input/") + Rel + TEXT(".txt"), ExportMappingContext(IMC, Pkg));
			}
			else if (UDataTable* DT = Cast<UDataTable>(Obj))
			{
				Category = TEXT("data tables");
				Write(TEXT("data/") + Rel + TEXT(".csv"), DT->GetTableAsCSV());
				if (DT->GetRowStruct())
				{
					Write(TEXT("data/") + Rel + TEXT(".rowstruct.txt"), TEXT("DataTable ") + Pkg + TEXT(" rows are ") + DT->GetRowStruct()->GetPathName() + TEXT("\n"));
				}
			}
			else if (UCurveTable* CT = Cast<UCurveTable>(Obj))
			{
				Category = TEXT("curve tables");
				Write(TEXT("data/") + Rel + TEXT(".csv"), CT->GetTableAsCSV());
			}
			else if (UUserDefinedEnum* UE = Cast<UUserDefinedEnum>(Obj))
			{
				Category = TEXT("enums");
				Write(TEXT("data/") + Rel + TEXT(".txt"), ExportUserEnum(UE, Pkg));
			}
			else if (UUserDefinedStruct* US = Cast<UUserDefinedStruct>(Obj))
			{
				Category = TEXT("structs");
				Write(TEXT("data/") + Rel + TEXT(".txt"), ExportUserStruct(US, Pkg));
			}
			else if (Cast<UCurveBase>(Obj))
			{
				Category = TEXT("curves");
				Write(TEXT("data/") + Rel + TEXT(".txt"), ExportGenericObject(Obj, TEXT("Curve"), Pkg));
			}
			else if (Cast<UDataAsset>(Obj))
			{
				Category = TEXT("data assets");
				Write(TEXT("data/") + Rel + TEXT(".txt"), ExportGenericObject(Obj, TEXT("DataAsset"), Pkg));
			}
		});
		if (!bNoCrash)
		{
			Failures.Add(Pkg + TEXT(": crashed while exporting (skipped)"));
		}
		else if (bOk && !Category.IsEmpty())
		{
			Counts.FindOrAdd(Category) += 1;
		}
		if (SinceGc >= GcEvery)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			SinceGc = 0;
		}
		++Done;
		Progress(false);
		if (Done % 200 == 0)
		{
			UE_LOG(LogAutopilotCodex, Display, TEXT("AutopilotCodex: %d/%d files"), Done, Total);
		}
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	// Maps: each map's placed actors, one map at a time.
	FOut MapOut;
	MapOut.Max = 200000;
	int32 MapsDone = 0;
	int32 MapsSkipped = 0;
	if (!bNoMaps)
	{
		for (const FAssetData& A : Maps)
		{
			if (MapsDone >= MaxMaps)
			{
				++MapsSkipped;
				continue;
			}
			const FString Pkg = A.PackageName.ToString();
			UE_LOG(LogAutopilotCodex, Display, TEXT("AutopilotCodex: map %s"), *Pkg);
			FString LevelBpText;
			const bool bNoCrash = Guarded([&]()
			{
				UPackage* Package = LoadPackage(nullptr, *Pkg, LOAD_None);
				UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
				if (!World || !World->PersistentLevel)
				{
					Failures.Add(Pkg + TEXT(": failed to load map"));
					return;
				}
				++MapsDone;
				MapOut.Add(0, TEXT("Map ") + Pkg);
				if (AWorldSettings* WS = World->GetWorldSettings(false, false))
				{
					MapOut.Add(1, TEXT("GameMode override: ") + (WS->DefaultGameMode ? WS->DefaultGameMode->GetPathName() : FString(TEXT("none (project default)"))));
				}
				const bool bPartitioned = World->IsPartitionedWorld();
				if (bPartitioned)
				{
					MapOut.Add(1, TEXT("World Partition: yes (actors read from their external packages)"));
				}
				if (World->GetStreamingLevels().Num() > 0)
				{
					TArray<FString> S;
					for (const ULevelStreaming* L : World->GetStreamingLevels())
					{
						if (L)
						{
							S.Add(L->GetWorldAssetPackageName());
						}
					}
					MapOut.Add(1, TEXT("Streaming levels: ") + FString::Join(S, TEXT(", ")));
				}

				TArray<FActorRow> Rows;
				TSet<FString> Seen;
				for (AActor* Actor : World->PersistentLevel->Actors)
				{
					if (!Actor || !IsValid(Actor))
					{
						continue;
					}
					FActorRow Row;
					Row.Class = Actor->GetClass()->GetName();
					Row.Name = Actor->GetActorLabel();
					if (Row.Name.IsEmpty())
					{
						Row.Name = Actor->GetName();
					}
					Row.Text = ActorDetails(Actor);
					Seen.Add(Actor->GetName());
					Rows.Add(Row);
				}
				if (bPartitioned)
				{
					FARFilter AF;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
					for (const FString& Path : TArray<FString>{ULevel::GetExternalActorsPath(Pkg)})
#else
					for (const FString& Path : ULevel::GetExternalActorsPaths(Pkg))
#endif
					{
						AF.PackagePaths.Add(FName(*Path));
					}
					AF.bRecursivePaths = true;
					AF.bIncludeOnlyOnDiskAssets = true;
					TArray<FAssetData> ActorAssets;
					AR.GetAssets(AF, ActorAssets);
					int32 Loaded = 0;
					for (const FAssetData& AA : ActorAssets)
					{
						TUniquePtr<FWorldPartitionActorDesc> Desc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(AA);
						if (!Desc.IsValid())
						{
							continue;
						}
#if UE_VERSION_OLDER_THAN(5, 5, 0)
						const FString ActorName = Desc->GetActorName().ToString();
#else
						const FString ActorName = Desc->GetActorNameString();
#endif
						if (Seen.Contains(ActorName))
						{
							continue;
						}
						FActorRow Row;
						const FTopLevelAssetPath Base = Desc->GetBaseClass().IsValid() ? Desc->GetBaseClass() : Desc->GetNativeClass();
						Row.Class = Base.GetAssetName().ToString();
#if UE_VERSION_OLDER_THAN(5, 5, 0)
						Row.Name = Desc->GetActorLabel().IsNone() ? FString() : Desc->GetActorLabel().ToString();
#else
						Row.Name = Desc->GetActorLabelString();
#endif
						if (Row.Name.IsEmpty())
						{
							Row.Name = ActorName;
						}
						const FBox Bounds = Desc->GetEditorBounds();
#if UE_VERSION_OLDER_THAN(5, 4, 0)
						const FVector Where = Bounds.IsValid ? Bounds.GetCenter() : Desc->GetRuntimeBounds().GetCenter();
#else
						const FVector Where = Bounds.IsValid ? Bounds.GetCenter() : Desc->GetActorTransform().GetLocation();
#endif
						Row.Text = TEXT(" at ") + Vec(Where) + TEXT(" (bounds center)");
						if (!bNoWpLoad && Loaded < 600)
						{
							++Loaded;
							if (UPackage* AP = LoadPackage(nullptr, *Desc->GetActorPackage().ToString(), LOAD_NoWarn | LOAD_Quiet))
							{
								ForEachObjectWithPackage(AP, [&Row](UObject* O)
								{
									if (AActor* Act = Cast<AActor>(O))
									{
										if (!Act->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
										{
											Row.Text = ActorDetails(Act);
											return false;
										}
									}
									return true;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
								}, false);  // (bIncludeNestedObjects; 5.8 made it EGetObjectsFlags)
#else
								}, EGetObjectsFlags::None);
#endif
							}
						}
						Rows.Add(Row);
					}
				}
				Rows.Sort([](const FActorRow& X, const FActorRow& Y) { return X.Class != Y.Class ? X.Class < Y.Class : X.Name < Y.Name; });
				MapOut.Add(1, FString::Printf(TEXT("Actors (%d):"), Rows.Num()));
				for (int32 i = 0; i < Rows.Num();)
				{
					int32 j = i;
					while (j < Rows.Num() && Rows[j].Class == Rows[i].Class)
					{
						++j;
					}
					MapOut.Add(2, FString::Printf(TEXT("%s (%d):"), *Rows[i].Class, j - i));
					for (int32 k = i; k < j && k < i + MaxActorsPerClass; ++k)
					{
						MapOut.Add(3, Rows[k].Name + Rows[k].Text);
					}
					if (j - i > MaxActorsPerClass)
					{
						MapOut.Add(3, FString::Printf(TEXT("... and %d more"), j - i - MaxActorsPerClass));
					}
					i = j;
				}
				MapOut.Add(0, FString());

				if (ULevelScriptBlueprint* LBP = World->PersistentLevel->GetLevelScriptBlueprint(true))
				{
					bool bHasNodes = false;
					for (const UEdGraph* G : LBP->UbergraphPages)
					{
						for (const UEdGraphNode* N : G ? G->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
						{
							if (N && !N->IsA<UEdGraphNode_Comment>() && (N->Pins.ContainsByPredicate([](const UEdGraphPin* P) { return P && P->LinkedTo.Num() > 0; })))
							{
								bHasNodes = true;
							}
						}
					}
					if (bHasNodes || LBP->FunctionGraphs.Num() > 0 || LBP->NewVariables.Num() > 0)
					{
						Write(TEXT("blueprints/") + RelPath(Pkg) + TEXT(".LevelBlueprint.txt"), ExportBlueprint(LBP, Pkg + TEXT(" (Level Blueprint)")));
						Counts.FindOrAdd(TEXT("level blueprints")) += 1;
					}
				}
			});
			if (!bNoCrash)
			{
				Failures.Add(Pkg + TEXT(": crashed while exporting map (skipped)"));
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			++Done;
			Progress(false);
			Write(TEXT("maps.txt"), MapOut.Join());  // (the maps so far: read while the rest are exported)
		}
		Write(TEXT("maps.txt"), MapOut.Join());
		Counts.Add(TEXT("maps"), MapsDone);
	}
	// The earlier export's files this one didn't write again: their assets are gone (or no longer exported)
	for (const FString& Gone : Earlier)
	{
		IFileManager::Get().Delete(*Gone, false, true, true);
	}
	Done = Total;
	Progress(true);

	const double Seconds = FPlatformTime::Seconds() - Start;
	FOut Summary;
	Summary.Max = 100000;
	Summary.Add(0, TEXT("AutopilotCodex export"));
	Summary.Add(0, TEXT("Project: ") + FString(FApp::GetProjectName()));
	Summary.Add(0, TEXT("Roots: ") + FString::Join(Roots, TEXT(", ")));
	Summary.Add(0, FString::Printf(TEXT("Time: %.1f s"), Seconds));
	Summary.Add(0, FString::Printf(TEXT("Written: %.1f MB of text"), BytesWritten / (1024.0 * 1024.0)));
	TArray<FString> Keys;
	Counts.GetKeys(Keys);
	Keys.Sort();
	for (const FString& K : Keys)
	{
		Summary.Add(0, FString::Printf(TEXT("%s: %d"), *K, Counts[K]));
	}
	if (bNoMaps)
	{
		Summary.Add(0, TEXT("Maps: skipped (-nomaps)"));
	}
	else if (MapsSkipped > 0)
	{
		Summary.Add(0, FString::Printf(TEXT("Maps not exported (over -maxmaps=%d): %d"), MaxMaps, MapsSkipped));
	}
	Summary.Add(0, FString::Printf(TEXT("Failures: %d"), Failures.Num()));
	for (const FString& F : Failures)
	{
		Summary.Add(1, F);
	}
	TArray<FString> EngineErrors;
	GWarn->GetErrors(EngineErrors);
	if (EngineErrors.Num() > 0)
	{
		Summary.Add(0, FString::Printf(TEXT("Errors the engine logged while loading the game's assets: %d"), EngineErrors.Num()));
		for (int32 i = 0; i < EngineErrors.Num() && i < 40; ++i)
		{
			Summary.Add(1, Cap(OneLine(EngineErrors[i]), 400));
		}
	}
	Write(TEXT("summary.txt"), Summary.Join());
	UE_LOG(LogAutopilotCodex, Display, TEXT("AutopilotCodex: done in %.1f s, %d failures, output in %s"), Seconds, Failures.Num(), *OutDir);
	return 0;
}
