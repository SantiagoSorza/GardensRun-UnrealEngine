// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroGraphAsset.h"
#include "NwiroPCGGraph.h"
#include "PCGGraph.h"

UNwiroGraphAsset::UNwiroGraphAsset()
	: NodeCount(0)
	, InternalGraph(nullptr)
{
	GeneratedAt = FDateTime::Now().ToString();
}

UPCGGraph* UNwiroGraphAsset::GetGraph() const
{
	return InternalGraph;
}

void UNwiroGraphAsset::SetGraph(UPCGGraph* NewGraph)
{
	InternalGraph = Cast<UNwiroPCGGraph>(NewGraph);
}

void UNwiroGraphAsset::SetMetadata(const FString& Prompt, const FString& Version,
	const FString& Project, int32 NumNodes)
{
	OriginalPrompt = Prompt;
	VersionId = Version;
	ProjectName = Project;
	NodeCount = NumNodes;
	GeneratedAt = FDateTime::Now().ToString();
}
