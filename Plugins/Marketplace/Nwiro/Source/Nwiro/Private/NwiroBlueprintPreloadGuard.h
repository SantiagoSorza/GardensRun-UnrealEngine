// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

// Mirrored in Pro and Integration Kit. Inspection never loads an asset, class,
// package, CDO, or dependency.
// Use on the game thread; returned UObject pointers are immediate-use locals,
// not retained references. It detects one known metadata fault, not universal safety.
namespace NwiroBlueprintPreload
{
	enum class EDisposition : uint8
	{
		AlreadyLoaded,
		Rejected,
		MetadataUnknown,
		NotFlagged
	};

	struct FInspection
	{
		EDisposition Disposition = EDisposition::MetadataUnknown;
		const TCHAR* Code = TEXT("metadata_unknown");
		UObject* ExistingObject = nullptr;
	};

	inline bool NormalizeExactAssetPath(const FString& Input, FString& OutObjectPath)
	{
		// Validate strings before constructing FName/FSoftObjectPath. NAME_SIZE
		// applies to names; bounding the whole final path also bounds its pieces.
		OutObjectPath.Reset();
		if (Input.IsEmpty() || Input.Len() >= NAME_SIZE) return false;
		for (TCHAR Ch : Input)
		{
			if (Ch < 0x20 || Ch == TEXT('\\')) return false;
		}
		FString Path = FPackageName::ExportTextPathToObjectPath(Input);
		if (!Path.StartsWith(TEXT("/")) || Path.Contains(TEXT(":"))) return false;
		if (!Path.Contains(TEXT(".")))
		{
			if (!FPackageName::IsValidLongPackageName(Path, true)) return false;
			Path += TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		}
		if (Path.Len() >= NAME_SIZE || !FPackageName::IsValidObjectPath(Path)) return false;
		OutObjectPath = MoveTemp(Path);
		return true;
	}

	inline bool TryBlueprintType(const FString& Tag, EBlueprintType& OutType)
	{
		// The native enum lookup does not load a Blueprint. Do not infer a type
		// from the asset name, class-name substring, or a missing/malformed tag.
		if (Tag.IsEmpty() || Tag.Len() > 128) return false;
		const UEnum* Types = StaticEnum<EBlueprintType>();
		const int64 Value = Types ? Types->GetValueByNameString(Tag) : INDEX_NONE;
		if (Value < BPTYPE_Normal || Value >= BPTYPE_MAX) return false;
		OutType = static_cast<EBlueprintType>(Value);
		return true;
	}

	inline FInspection InspectLoaded(UObject* Object)
	{
		if (!Object || !IsValid(Object) || Object->HasAnyFlags(
			RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return {EDisposition::Rejected, TEXT("object_not_ready"), nullptr};
		}
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
		{
			// Inspect existing memory before stale disk tags. No CDO creation.
			if (!Blueprint->GeneratedClass && FBlueprintEditorUtils::SupportsConstructionScript(Blueprint))
			{
				return {EDisposition::Rejected, TEXT("loaded_blueprint_missing_generated_class"), nullptr};
			}
		}
		return {EDisposition::AlreadyLoaded, TEXT("already_loaded"), Object};
	}

	inline FInspection InspectDiskMetadata(const FAssetData& Asset, IAssetRegistry& Registry)
	{
		if (!Asset.IsValid()) return {};
		const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();
		bool bBlueprintFamily = Asset.AssetClassPath == BlueprintClassPath;
		if (!bBlueprintFamily)
		{
			if (const UClass* KnownClass = FindObject<UClass>(nullptr, *Asset.AssetClassPath.ToString()))
			{
				bBlueprintFamily = KnownClass->IsChildOf(UBlueprint::StaticClass());
			}
			else
			{
				TArray<FTopLevelAssetPath> Ancestors;
				if (!Registry.GetAncestorClassNames(Asset.AssetClassPath, Ancestors)) return {};
				bBlueprintFamily = Ancestors.Contains(BlueprintClassPath);
			}
		}
		if (!bBlueprintFamily) return {EDisposition::NotFlagged, TEXT("not_blueprint"), nullptr};

		FString GeneratedClass;
		if (!Asset.GetTagValue(TEXT("GeneratedClass"), GeneratedClass) || GeneratedClass.IsEmpty() || GeneratedClass.Len() >= NAME_SIZE) return {};
		if (!GeneratedClass.Equals(TEXT("None"), ESearchCase::CaseSensitive))
		{
			// A plausible reference still says nothing about transitive load safety.
			const FString ClassPath = FPackageName::ExportTextPathToObjectPath(GeneratedClass);
			return FPackageName::IsValidObjectPath(ClassPath)
				? FInspection{EDisposition::NotFlagged, TEXT("generated_class_recorded"), nullptr}
				: FInspection{};
		}

		FString TypeTag;
		EBlueprintType Type = BPTYPE_MAX;
		if (!Asset.GetTagValue(TEXT("BlueprintType"), TypeTag) || !TryBlueprintType(TypeTag, Type)) return {};
		if (Type != BPTYPE_Normal)
		{
			// Const, Interface, LevelScript, MacroLibrary and FunctionLibrary are
			// excluded by UE5.8 SupportsConstructionScript. Not a safety promise.
			return {EDisposition::NotFlagged, TEXT("construction_script_exempt_type"), nullptr};
		}

		FString NativeParent;
		if (!Asset.GetTagValue(TEXT("NativeParentClass"), NativeParent)) return {};
		const FString ParentPath = FPackageName::ExportTextPathToObjectPath(NativeParent);
		if (ParentPath.Len() >= NAME_SIZE || !FPackageName::IsValidObjectPath(ParentPath)) return {};
		const UClass* ParentClass = FindObject<UClass>(nullptr, *ParentPath);
		if (!ParentClass) return {}; // Never LoadClass to decide whether a load is safe.
		if (!ParentClass->IsChildOf(AActor::StaticClass()))
		{
			return {EDisposition::NotFlagged, TEXT("construction_script_non_actor"), nullptr};
		}
		return {EDisposition::Rejected, TEXT("blueprint_generated_class_explicit_none"), nullptr};
	}

	inline FInspection InspectRegistryCandidate(const FAssetData& Asset, IAssetRegistry& Registry)
	{
		if (!Asset.IsValid()) return {};
		FString Path;
		Asset.AppendObjectPath(Path);
		FString ObjectPath;
		if (!NormalizeExactAssetPath(Path, ObjectPath))
		{
			return {EDisposition::Rejected, TEXT("invalid_asset_path"), nullptr};
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return InspectLoaded(Existing);
		}
		return InspectDiskMetadata(Asset, Registry);
	}

	inline FInspection InspectExactAssetPath(const FString& Input, IAssetRegistry& Registry,
		FString& OutObjectPath)
	{
		if (!NormalizeExactAssetPath(Input, OutObjectPath))
		{
			return {EDisposition::Rejected, TEXT("invalid_asset_path"), nullptr};
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *OutObjectPath))
		{
			return InspectLoaded(Existing);
		}
		// DiskGatheredData only: do not synthesize tags by visiting loaded UObjects.
		const FAssetData Disk = Registry.GetAssetByObjectPath(FSoftObjectPath(OutObjectPath), true);
		return InspectDiskMetadata(Disk, Registry);
	}
}
