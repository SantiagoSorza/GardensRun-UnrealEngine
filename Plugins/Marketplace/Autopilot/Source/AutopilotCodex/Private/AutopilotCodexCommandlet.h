#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AutopilotCodexCommandlet.generated.h"

/**
 * Exports a project's game logic and data as plain text for a language model to research:
 * Blueprints as pseudo-code, input assets, data tables/assets, the asset list and the maps' actors.
 *
 * UnrealEditor-Cmd.exe <project>.uproject -run=AutopilotCodex -out=<dir> [-root=/Game[,/Other]] [-maxmaps=N] [-nomaps]
 *     [-progress=<file>]
 *
 * -progress: a file rewritten as the export goes, "<done> <total>": the assets and maps exported so far, of all those it
 * exports (the editor's Autopilot tab shows it).
 */
UCLASS()
class UAutopilotCodexCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAutopilotCodexCommandlet();

	virtual int32 Main(const FString& Params) override;
};
