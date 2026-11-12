// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Editor utility tools: screenshot, log reading, PIE control.
 */

// Custom output device to capture recent log entries
class FNwiroLogCapture : public FOutputDevice
{
public:
	static FNwiroLogCapture& Get();

	void Register();
	void Unregister();

	TArray<FString> GetRecentLines(int32 Count, const FString& SeverityFilter = TEXT(""), const FString& CategoryFilter = TEXT("")) const;

protected:
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

private:
	mutable FCriticalSection Lock;

	// The producing category is kept out of band so eviction can reason about it without
	// re-parsing the formatted text.
	struct FCapturedLine
	{
		FString Text;
		FName Category;
	};
	TArray<FCapturedLine> Lines;
	TMap<FName, int32> CategoryCounts;
	static const int32 MaxLines = 1000;
};

class FNwiroEditorTools
{
public:
	// Capture editor viewport screenshot
	static FString TakeScreenshot(const FString& JsonCommand);

	// Read recent output log entries
	static FString ReadLog(const FString& JsonCommand);

	// Start Play in Editor
	static FString PlayInEditor(const FString& JsonCommand);

	// Stop Play in Editor
	static FString StopPIE(const FString& JsonCommand);
};
