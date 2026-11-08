// The project's levels and the one a run asks for (PlaytestLevels.h).

#include "PlaytestLevels.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace PlaytestLevels
{
namespace
{
	/** Where the project's own levels are: /Game and its project plugins' content (never the engine's). */
	TArray<FString> Roots()
	{
		TArray<FString> Out = { TEXT("/Game/") };
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
		{
			if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project)
			{
				Out.Add(Plugin->GetMountedAssetPath());
			}
		}
		return Out;
	}

	/** A package name from what a run gave: the .Object part and a .umap ending left out, slashes forward. */
	FString PackageName(FString Name)
	{
		Name.TrimStartAndEndInline();
		Name.ReplaceInline(TEXT("\\"), TEXT("/"));
		Name.RemoveFromEnd(TEXT(".umap"));
		int32 Dot;
		if (Name.FindLastChar(TEXT('.'), Dot) && Dot > Name.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			Name.LeftInline(Dot);
		}
		return Name;
	}

	/** Whether the package holds a level (a World asset). */
	bool IsLevel(const FString& Package)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(FName(*Package), Assets);
		return Assets.ContainsByPredicate([](const FAssetData& Asset)
		{
			return Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName();
		});
	}
}

TArray<FString> List()
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (Registry.IsLoadingAssets())
	{
		Registry.WaitForCompletion();  // (the editor's first scan, right after it opened)
	}
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UWorld::StaticClass()->GetClassPathName(), Assets, true);
	const TArray<FString> Under = Roots();
	TArray<FString> Out;
	for (const FAssetData& Asset : Assets)
	{
		const FString Package = Asset.PackageName.ToString();
		if (Under.ContainsByPredicate([&Package](const FString& Root) { return Package.StartsWith(Root); }))
		{
			Out.AddUnique(Package);
		}
	}
	Out.Sort();
	return Out;
}

bool Resolve(const FString& Name, FString& OutPackage, FString& OutError)
{
	const FString Package = PackageName(Name);
	if (Package.IsEmpty())
	{
		OutError = TEXT("No level named");
		return false;
	}
	if (Package.StartsWith(TEXT("/")))
	{
		if (!FPackageName::DoesPackageExist(Package) || !IsLevel(Package))  // (a Blueprint's path is no level)
		{
			OutError = FString::Printf(TEXT("No level %s in the project"), *Package);
			return false;
		}
		OutPackage = Package;
		return true;
	}
	TArray<FString> Found;
	for (const FString& Level : List())
	{
		if (FPackageName::GetShortName(Level).Equals(Package, ESearchCase::IgnoreCase))
		{
			Found.Add(Level);
		}
	}
	if (Found.Num() == 1)
	{
		OutPackage = Found[0];
		return true;
	}
	OutError = Found.Num() == 0
		? FString::Printf(TEXT("No level named %s in the project"), *Package)
		: FString::Printf(TEXT("Several levels are named %s: %s"), *Package, *FString::Join(Found, TEXT(", ")));
	return false;
}

FString ObjectPath(const FString& Package)
{
	// (the world's own name, which a level renamed on disk may not share with its package)
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(FName(*Package), Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
		{
			return Asset.GetObjectPathString();
		}
	}
	return Package + TEXT(".") + FPackageName::GetShortName(Package);
}

FString PackageOf(const UWorld* World)
{
	return World ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString();
}

FString EditorLevel()
{
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Editor && Context.World())
			{
				return PackageOf(Context.World());
			}
		}
	}
	return FString();
}
}
