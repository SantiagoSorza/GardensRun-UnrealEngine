// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "NwiroGraphAsset.generated.h"

class UPCGGraph;
class UNwiroPCGGraph;

/**
 * UNwiroGraphAsset - A locked wrapper around a PCG Graph.
 *
 * This asset contains a PCG graph internally but does not expose it
 * to the editor. Users can see metadata (prompt, version, node count)
 * but cannot view or edit the graph structure.
 *
 * Only Nwiro's generation pipeline creates these assets programmatically.
 */
UCLASS(BlueprintType)
class NWIRORUNTIME_API UNwiroGraphAsset : public UObject
{
	GENERATED_BODY()

public:
	UNwiroGraphAsset();

	// --- Metadata (read-only in editor Details panel) ---

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Nwiro")
	FString OriginalPrompt;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Nwiro")
	FString GeneratedAt;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Nwiro")
	int32 NodeCount;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Nwiro")
	FString VersionId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Nwiro")
	FString ProjectName;

	// --- Internal Graph (serialized for persistence, NOT visible in editor) ---

	UPROPERTY()
	TObjectPtr<UNwiroPCGGraph> InternalGraph;

	// --- Pipeline Access (callable from Python and Blueprint) ---

	/** Get the internal PCG graph. */
	UFUNCTION(BlueprintCallable, Category = "Nwiro")
	UPCGGraph* GetGraph() const;

	/** Set the internal PCG graph. */
	UFUNCTION(BlueprintCallable, Category = "Nwiro")
	void SetGraph(UPCGGraph* NewGraph);

	/** Set all metadata at once. */
	UFUNCTION(BlueprintCallable, Category = "Nwiro")
	void SetMetadata(const FString& Prompt, const FString& Version,
		const FString& Project, int32 NumNodes);
};
