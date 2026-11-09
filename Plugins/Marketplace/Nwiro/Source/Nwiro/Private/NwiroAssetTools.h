// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Asset inspection and explicit persistence tools.
 * Reading uses UObject reflection; saving supports ready Blueprint, struct and enum assets.
 */
class FNwiroAssetTools
{
public:
	// Read any asset's properties via reflection
	static FString ReadAsset(const FString& JsonCommand);

	// Explicitly save one ready loaded project asset package
	static FString SaveAsset(const FString& JsonCommand);

	// Search assets by name/class across the project
	static FString FindAssets(const FString& JsonCommand);

	// Render an asset's editor thumbnail to file
	static FString GetAssetThumbnail(const FString& JsonCommand);
};
