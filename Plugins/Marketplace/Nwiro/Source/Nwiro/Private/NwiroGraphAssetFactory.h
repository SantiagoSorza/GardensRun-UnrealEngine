// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "NwiroGraphAssetFactory.generated.h"

/**
 * UNwiroGraphAssetFactory
 *
 * Factory that lets UE's asset tools create UNwiroGraphAsset instances.
 * CanCreateNew() returns false so the asset does NOT appear in the
 * Content Browser "Add New" menu — it is only created programmatically
 * by the Python pipeline.
 */
UCLASS()
class UNwiroGraphAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UNwiroGraphAssetFactory();

	// UFactory interface
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent,
		FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;

	virtual bool CanCreateNew() const override { return false; }
	virtual bool ShouldShowInNewMenu() const override { return false; }
};
