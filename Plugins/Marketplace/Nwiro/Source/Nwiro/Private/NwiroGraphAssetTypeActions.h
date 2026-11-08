// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

/**
 * Content Browser integration for UNwiroGraphAsset.
 *
 * Controls how the asset appears and behaves:
 * - Custom name: "Nwiro Generation"
 * - Custom color: Purple (128, 0, 255)
 * - Double-click: Shows read-only metadata dialog (NOT the PCG graph editor)
 * - Right-click: No custom actions
 */
class FNwiroGraphAssetTypeActions : public FAssetTypeActions_Base
{
public:
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;

	virtual void OpenAssetEditor(
		const TArray<UObject*>& InObjects,
		TSharedPtr<class IToolkitHost> EditWithinLevelEditor) override;

	virtual bool HasActions(const TArray<UObject*>& InObjects) const override
	{ return false; }
};
