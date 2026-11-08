// The project's levels (docs/protocol.md, `levels`), and the one a run asks for by path or name: the agent service
// plays a level other than the one open in the editor through the `session` op's `map` (the editor's Play In Editor
// map override: what is open in the editor stays as it is).

#pragma once

#include "CoreMinimal.h"

namespace PlaytestLevels
{
	/** The project's levels (World assets in /Game and the project's own plugins), by package name, sorted. */
	AUTOPILOTPLAYTEST_API TArray<FString> List();

	/** The level `Name` means: a package name (/Game/Maps/Arena, its .Arena object part or .umap ending allowed) or
	 *  a level's own name (Arena), as the project has it. False, with why in OutError, when there is none or several
	 *  have that name. */
	AUTOPILOTPLAYTEST_API bool Resolve(const FString& Name, FString& OutPackage, FString& OutError);

	/** A level's object path for loading it (/Game/Maps/Arena -> /Game/Maps/Arena.Arena). */
	AUTOPILOTPLAYTEST_API FString ObjectPath(const FString& Package);

	/** A world's package name without Play In Editor's prefix (/Game/Maps/UEDPIE_0_Arena -> /Game/Maps/Arena). */
	AUTOPILOTPLAYTEST_API FString PackageOf(const class UWorld* World);

	/** The level open in the editor (its package name; empty in a game). */
	AUTOPILOTPLAYTEST_API FString EditorLevel();
}
