#pragma once

#include "CoreMinimal.h"

/**
 * Autopilot with Aura (UPlaytestSettings::bUseWithAura): instead of an agent service of your own, the one the Aura app
 * runs on this machine, so runs go through Aura (on your Aura plan) and what the agent learns about each game is kept
 * with Aura across your team's machines. Off by default: Autopilot on its own never connects to Aura.
 */
namespace PlaytestAura
{
	/** Where to get Aura. */
	AUTOPILOTPLAYTEST_API extern const TCHAR* DownloadUrl;

	/**
	 * Aura's account creation page (<server>/auth/signup) on the Aura server the sign-in goes to: autopilot.env's
	 * AUTOPILOT_AURA_SERVER (in KeysDir, "": the user settings folder), else the one the download was built for (the
	 * plugin's AgentService/settings.env, or AgentServiceDir's), else Aura's own (https://app.tryaura.dev).
	 */
	AUTOPILOTPLAYTEST_API FString SignUpUrl(const FString& KeysDir = FString(), const FString& AgentServiceDir = FString());

	/** Whether Aura is on this machine: its app installed, or its plugin in this editor. */
	AUTOPILOTPLAYTEST_API bool IsInstalled();

	/** Start the Aura app (it starts its agent service). Returns whether it was started. */
	AUTOPILOTPLAYTEST_API bool Launch();

	/** The agent service the Aura app runs, from where it says it listens (Autopilot/aura-brain.json in the user settings folder); "" when it isn't running. */
	AUTOPILOTPLAYTEST_API FString ServiceUrl();

	/**
	 * The address a running agent service wrote to Autopilot/<File> in the user settings folder (autopilot/bridge.py
	 * brain_file: brain.json for an Autopilot of your own, aura-brain.json for Aura's). "" when none did, the one that did has
	 * ended, or it names anything but this machine.
	 */
	AUTOPILOTPLAYTEST_API FString AnnouncedUrl(const TCHAR* File);

	/**
	 * Autopilot's folder in the user settings folder (autopilot/bridge.py brain_file): %LOCALAPPDATA%/Autopilot on Windows,
	 * ~/Library/Application Support/Autopilot on macOS, ~/.config/Autopilot elsewhere. Holds brain.json,
	 * aura-brain.json, preferences.json and autopilot.env.
	 */
	AUTOPILOTPLAYTEST_API FString SettingsDir();
}
