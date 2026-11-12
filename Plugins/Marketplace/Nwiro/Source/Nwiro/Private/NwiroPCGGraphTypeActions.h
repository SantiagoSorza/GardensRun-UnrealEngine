// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

/**
 * Content Browser / Details panel integration for UNwiroPCGGraph.
 *
 * PRIMARY defense against opening Nwiro-generated PCG graphs in the editor.
 * When UE detects the user trying to open a UNwiroPCGGraph (e.g., clicking
 * the graph thumbnail in a PCGComponent's Details panel), this TypeActions
 * intercepts and shows a "locked" dialog instead of the PCG Graph Editor.
 */
class FNwiroPCGGraphTypeActions : public FAssetTypeActions_Base
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
