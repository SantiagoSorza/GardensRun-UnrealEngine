// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PCGGraph.h"
#include "NwiroPCGGraph.generated.h"

/**
 * UNwiroPCGGraph - A locked PCG Graph subclass.
 *
 * Identical to UPCGGraph in functionality. Exists as a distinct type
 * so that editor TypeActions can intercept attempts to open it
 * and show a "locked" dialog instead of the PCG Graph Editor.
 *
 * Created internally by UNwiroGraphAssetFactory. Never created manually.
 */
UCLASS()
class NWIRORUNTIME_API UNwiroPCGGraph : public UPCGGraph
{
	GENERATED_BODY()
};
