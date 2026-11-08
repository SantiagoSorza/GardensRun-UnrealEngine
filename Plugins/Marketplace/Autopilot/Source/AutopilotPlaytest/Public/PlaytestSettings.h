#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PlaytestSettings.generated.h"

/**
 * Autopilot's settings for you on this project (kept in Saved/Config, never committed with the project): where the agent
 * service is, how runs go, and whether the Autopilot tab shows the policy log.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Autopilot"))
class AUTOPILOTPLAYTEST_API UPlaytestSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPlaytestSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** The agent service to connect to. Left at the default (ws://127.0.0.1:8765/v1/bridge), the one running on this machine, on whatever port it says it uses; or one you host (wss://<host>/v1/bridge). */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Agent Service URL"))
	FString ServiceUrl;

	/**
	 * Optional. Sent as "Authorization: Bearer <token>" when connecting, for an agent service that asks for one.
	 * Leave empty for one on this machine. The AUTOPILOT_PLAYTEST_TOKEN environment variable is used when this is empty.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (PasswordField = true))
	FString AccessToken;

	/**
	 * Use Autopilot with Aura: connect to the agent service the Aura app runs on this machine instead of the one above.
	 * Runs then go through Aura, on your Aura plan, and what the agent learns about each game is kept with Aura across
	 * your team's machines. Needs Aura (https://www.tryaura.dev/) installed and running.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Use with Aura"))
	bool bUseWithAura = false;

	/**
	 * The command that runs the agent service, without "serve" (e.g. C:\autopilot\.venv\Scripts\python.exe -m autopilot).
	 * Empty: the autopilot command on PATH, else where its installer puts it (%USERPROFILE%\.local\bin\autopilot.exe).
	 * Autopilot connects by itself, and starts the agent service on this computer when none is running (Agent Service
	 * URL left at the default, not with Aura): hidden, its output in Autopilot/logs in your user settings folder. It
	 * ends with the editor that started it; other editors use the same one.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Agent Service", meta = (DisplayName = "Agent service command"))
	FString AgentServiceCommand;

	/** Minutes of play of the first-run setup's Explore run. */
	UPROPERTY(config, EditAnywhere, Category = "Runs", meta = (ClampMin = "1", ClampMax = "600"))
	int32 DefaultExploreMinutes = 10;

	/**
	 * The Autopilot tab's Time limit as last set, in minutes: how long a verify or goal run may take in all (from
	 * reading the game to its report), and an exploration's minutes of play. Set in the tab's composer, by each run.
	 */
	UPROPERTY(config, meta = (ClampMin = "1", ClampMax = "600"))
	int32 RunTimeLimitMinutes = 30;

	/**
	 * How the game is meant to work: the design, the rules players are told (e.g. "the player has three lives;
	 * each coin is worth 10 points"). Exploration reports behavior that differs from it as a bug, even where the
	 * game's own values agree with each other. Optional; the Autopilot tab's composer starts from it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Runs", meta = (MultiLine = true))
	FString DesignBrief;


	/**
	 * Show the policy log in the Autopilot tab: every policy step (the observation, the action probabilities, the action
	 * taken and how long it took). Autopilot asks the agent service for them when it connects; an agent service on this
	 * machine sends them. Takes effect at the next connect.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Policy Log", meta = (DisplayName = "Show Policy Log"))
	bool bShowInspector = true;
};
