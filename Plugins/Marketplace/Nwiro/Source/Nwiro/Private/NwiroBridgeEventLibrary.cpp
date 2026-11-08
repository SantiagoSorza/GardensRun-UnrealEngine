// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroBridgeEventLibrary.h"

#include "NwiroBridge.h"

bool UNwiroBridgeEventLibrary::EmitHdriResult(const FString& JsonString)
{
	if (UNwiroBridge::Instance)
	{
		UNwiroBridge::Instance->PushEvent(TEXT("hdri_result"), JsonString);
		return true;
	}
	return false;
}

bool UNwiroBridgeEventLibrary::EmitLandscapeResult(const FString& JsonString)
{
	if (UNwiroBridge::Instance)
	{
		UNwiroBridge::Instance->PushEvent(TEXT("landscape_result"), JsonString);
		return true;
	}
	return false;
}
