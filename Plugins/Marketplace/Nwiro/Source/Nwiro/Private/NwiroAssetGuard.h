// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "NwiroBlueprintPreloadGuard.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Guard against the "Cannot replace existing object of a different class" fatal
// (UObjectGlobals StaticAllocateObject): creating an asset where an object of a
// DIFFERENT class already lives at the same object path crashes the editor.
// Returns a JSON error string if such a conflict exists; empty string => safe.
// Discovered via fuzz: create_struct then create_enum at the same name+path.
inline FString NwiroCheckCreateConflict(const FString& PackagePath, const FString& AssetName, const UClass* DesiredClass)
{
	if (AssetName.IsEmpty()) return FString();
	// Reject names containing characters illegal in UE object names: they crash
	// downstream FName / package creation and corrupt object-path lookups (so the
	// collision check below cannot even find a conflicting asset). (fuzz: sql-inject name)
	{
		const FString InvalidChars = TEXT("\"' ,;.:/\\*?<>|\n\r\t");
		int32 _Idx;
		for (const TCHAR Ch : AssetName)
			if (Ch < 0x20 || InvalidChars.FindChar(Ch, _Idx))
				return TEXT("{\"success\":false,\"error\":\"Invalid asset name: contains characters not allowed in an asset name (quotes, spaces, punctuation, or path separators).\"}");
	}
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	const UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
	if (Existing && DesiredClass && !Existing->IsA(DesiredClass))
	{
		return FString::Printf(
			TEXT("{\"success\":false,\"error\":\"An asset of a different type ('%s') already exists at '%s'. Pick a different name or delete the existing asset first.\"}"),
			*Existing->GetClass()->GetName(), *ObjectPath);
	}
	return FString();
}

// Keep the registry-only known-fault check ahead of every loading operation.
// Missing metadata is not a safety verdict; other engine load failures remain possible.
inline UObject* NwiroSafeRegistryLoad(const FAssetData& A, FString* OutError = nullptr)
{
	if (OutError) OutError->Reset();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const NwiroBlueprintPreload::FInspection Inspection = NwiroBlueprintPreload::InspectRegistryCandidate(A, Registry);
	if (Inspection.Disposition == NwiroBlueprintPreload::EDisposition::Rejected)
	{
		if (OutError) *OutError = Inspection.Code;
		return nullptr;
	}
	if (Inspection.ExistingObject) return Inspection.ExistingObject;

	// Preserve the separate native-parent availability guard for unloaded assets.
	const FString AssetClass = A.AssetClassPath.GetAssetName().ToString();
	if (AssetClass.Contains(TEXT("Blueprint")))
	{
		FString NativeParent;
		if (A.GetTagValue(TEXT("NativeParentClass"), NativeParent) && !NativeParent.IsEmpty())
		{
			const FString ClassPath = FPackageName::ExportTextPathToObjectPath(NativeParent);
			if (!ClassPath.IsEmpty() && (ClassPath.Len() >= NAME_SIZE || !FindObject<UClass>(nullptr, *ClassPath)))
			{
				if (OutError) *OutError = TEXT("blueprint_native_parent_unavailable");
				return nullptr;
			}
		}
	}
	return A.GetAsset();
}

inline UObject* NwiroSafeExactAssetLoad(const FString& Input, FString* OutError = nullptr)
{
	if (OutError) OutError->Reset();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FString ObjectPath;
	const NwiroBlueprintPreload::FInspection Inspection = NwiroBlueprintPreload::InspectExactAssetPath(Input, Registry, ObjectPath);
	if (Inspection.Disposition == NwiroBlueprintPreload::EDisposition::Rejected)
	{
		if (OutError) *OutError = Inspection.Code;
		return nullptr;
	}
	if (Inspection.ExistingObject) return Inspection.ExistingObject;
	const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true);
	if (Asset.IsValid()) return NwiroSafeRegistryLoad(Asset, OutError);
	// An absent registry record is unknown. Preserve exact-path loading without
	// retrying aliases or a similar asset if this exact identity cannot be loaded.
	return LoadObject<UObject>(nullptr, *ObjectPath);
}

inline UObject* NwiroSafeAssetLoad(const FString& Input, FString* OutError = nullptr)
{
	if (OutError) OutError->Reset();
	if (Input.Contains(TEXT("/")))
	{
		const FString Path = Input.StartsWith(TEXT("/")) || Input.Contains(TEXT("'")) ? Input : TEXT("/Game/") + Input;
		return NwiroSafeExactAssetLoad(Path, OutError);
	}
	FString ValidatedPath;
	if (!NwiroBlueprintPreload::NormalizeExactAssetPath(TEXT("/Game/") + Input, ValidatedPath))
	{
		if (OutError) *OutError = TEXT("invalid_asset_path");
		return nullptr;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase)) return NwiroSafeRegistryLoad(Asset, OutError);
	}
	for (const FAssetData& Asset : Assets)
	{
		if (Input.Len() >= 3 && Asset.AssetName.ToString().Contains(Input, ESearchCase::IgnoreCase)) return NwiroSafeRegistryLoad(Asset, OutError);
	}
	return NwiroSafeExactAssetLoad(ValidatedPath, OutError);
}

inline FString NwiroAssetLoadFailure(const FString& Path, const FString& Code, const TCHAR* Kind = TEXT("Asset"))
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Code.IsEmpty()
		? FString::Printf(TEXT("%s not found: %s"), Kind, *Path)
		: FString::Printf(TEXT("%s load rejected before loading: %s"), Kind, *Code));
	Result->SetStringField(TEXT("errorCode"), Code.IsEmpty() ? TEXT("asset_not_found") : *Code);
	FString ObjectPath;
	Result->SetStringField(TEXT("path"), NwiroBlueprintPreload::NormalizeExactAssetPath(Path, ObjectPath) ? ObjectPath : Path);
	FString Out;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out));
	return Out;
}
