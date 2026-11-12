// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroGraphAssetFactory.h"
#include "NwiroGraphAsset.h"
#include "NwiroPCGGraph.h"

UNwiroGraphAssetFactory::UNwiroGraphAssetFactory()
{
	SupportedClass = UNwiroGraphAsset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = false;
}

UObject* UNwiroGraphAssetFactory::FactoryCreateNew(UClass* InClass, UObject* InParent,
	FName InName, EObjectFlags Flags,
	UObject* Context, FFeedbackContext* Warn)
{
	UNwiroGraphAsset* NewAsset = NewObject<UNwiroGraphAsset>(InParent, InClass, InName, Flags);

	// Auto-create the inner PCG graph so get_graph() is never None
	NewAsset->InternalGraph = NewObject<UNwiroPCGGraph>(
		InParent,
		UNwiroPCGGraph::StaticClass(),
		*FString::Printf(TEXT("%s_Graph"), *InName.ToString()),
		RF_Public);

	return NewAsset;
}
