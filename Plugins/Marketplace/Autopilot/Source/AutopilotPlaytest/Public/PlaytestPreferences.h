#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * The user's Autopilot choices for every engine and project on this machine (docs/protocol.md, "Sharing a run's data"):
 * Autopilot/preferences.json in the user settings folder (PlaytestAura::SettingsDir), and the model keys the first-run
 * setup writes to Autopilot/autopilot.env there.
 *
 *     {"version": 1, "onboarded": true, "share_data": true, "research_agent": true, "access": "aura" | "own_keys"}
 *
 * A missing or unreadable file reads as the defaults (not onboarded, sharing on, the research agent not chosen). Files are written whole, then put in
 * place, and keys the plugin doesn't know are kept.
 */
struct AUTOPILOTPLAYTEST_API FPlaytestPreferences
{
	/** The first-run setup was finished (or skipped). */
	bool bOnboarded = false;
	/** Share runs' data to improve Autopilot (absent: true). */
	bool bShareData = true;
	/**
	 * Whether runs have the research agent (the brain's background reader of the game's code), once the user chose on the
	 * Settings page. Unset (never chosen): the agent service decides, off in the Aura app's and on in the user's own.
	 */
	TOptional<bool> ResearchChoice;
	/** How the user chose to run it: "aura", "own_keys", or "" (not chosen). */
	FString Access;
	/** The file as read (unknown keys are written back as they were). */
	TSharedPtr<FJsonObject> Raw;
};

namespace PlaytestPreferences
{
	/** preferences.json in Dir (default: PlaytestAura::SettingsDir()). */
	AUTOPILOTPLAYTEST_API FPlaytestPreferences Load(const FString& Dir = FString());

	/** Writes Prefs (version 1) into preferences.json, merged with what is there. False when it couldn't be written. */
	AUTOPILOTPLAYTEST_API bool Save(const FPlaytestPreferences& Prefs, const FString& Dir = FString());
	/** Where preferences.json is (Dir: its folder, "" the user settings folder). */
	AUTOPILOTPLAYTEST_API FString PreferencesPath(const FString& Dir = FString());

	/** What every hello says as "share": -PlaytestShare=0|1 on the command line, else preferences.json (default true). */
	AUTOPILOTPLAYTEST_API bool ShareData();

	/** What a hello says as "research": -PlaytestResearch=0|1 on the command line, else preferences.json's choice; unset when
	 *  neither says (the hello leaves it out and the agent service decides). */
	AUTOPILOTPLAYTEST_API TOptional<bool> ResearchAgent();

	/**
	 * Puts KEY=value lines into autopilot.env in Dir (default: PlaytestAura::SettingsDir()), made if missing: a key already
	 * there is replaced, every other line is kept. Empty values are skipped. Values are never logged. Returns "" or what
	 * went wrong.
	 */
	AUTOPILOTPLAYTEST_API FString WriteKeys(const TArray<TPair<FString, FString>>& Keys, const FString& Dir = FString());

	/** autopilot.env's text with Keys merged in (WriteKeys without the file): for tests. */
	AUTOPILOTPLAYTEST_API FString MergeKeys(const FString& Existing, const TArray<TPair<FString, FString>>& Keys);

	/**
	 * The autopilot.env name an LLM provider's key goes under: ANTHROPIC_API_KEY, OPENAI_API_KEY, OPENCODE_API_KEY (OpenCode
	 * Zen), OPENROUTER_API_KEY, DEEPSEEK_API_KEY, GEMINI_API_KEY ("google"); another provider reads as "anthropic".
	 */
	AUTOPILOTPLAYTEST_API FString LlmKeyName(const FString& Provider);

	/**
	 * What "Use my keys" writes: TYPESAFE_API_KEY, AUTOPILOT_AGENT (Agent: "claude", "codex" or "opencode"; another reads as
	 * "opencode"), AUTOPILOT_OPENCODE_PROVIDER for OpenCode only (Provider: one of PlaytestAccount::LlmProviders; another
	 * reads as "anthropic") and the LLM key under its provider's name (LlmKeyName; Claude Code's is always Anthropic's,
	 * Codex's OpenAI's). An empty key is skipped by WriteKeys (its line stays); other providers' key lines, and
	 * AUTOPILOT_OPENCODE_PROVIDER for Claude Code or Codex, are never touched.
	 */
	AUTOPILOTPLAYTEST_API TArray<TPair<FString, FString>> SetupKeys(const FString& DecisionKey, const FString& Agent, const FString& Provider, const FString& LlmKey);

	/** The value autopilot.env's Text gives Name (the last line setting it wins, quotes taken off), "" when none. */
	AUTOPILOTPLAYTEST_API FString KeyIn(const FString& Text, const FString& Name);

	/** The value autopilot.env in Dir (default: PlaytestAura::SettingsDir()) gives Name, "" when none or no file. Never logged. */
	AUTOPILOTPLAYTEST_API FString ReadKey(const FString& Name, const FString& Dir = FString());

	/** Writes Text to Path whole, then puts it in place (a crash mid-write never leaves half a file). */
	AUTOPILOTPLAYTEST_API bool WriteFileAtomic(const FString& Path, const FString& Text, bool bPrivate = false);
}

/** One model's tokens in a run, as the agent service's `usage` message counts them (docs/protocol.md, "Usage"). */
struct AUTOPILOTPLAYTEST_API FPlaytestModelUsage
{
	FString Kind;   // "llm" or "decision"
	FString Model;  // the model's id, as the message gives it
	int64 Calls = 0;
	int64 Input = 0;
	int64 Output = 0;
	int64 CacheRead = 0;
	int64 CacheWrite = 0;

	bool IsLlm() const { return Kind == TEXT("llm"); }
	bool IsDecision() const { return Kind == TEXT("decision"); }
};

namespace PlaytestUsage
{
	/** The models of a `usage` message (or of the list saved with a run: {"models": [...]}); bOutFinal: its "final". */
	AUTOPILOTPLAYTEST_API TArray<FPlaytestModelUsage> Parse(const TSharedPtr<FJsonObject>& Message, bool* bOutFinal = nullptr);

	/** The models as the message's "models" list (to save with a run). */
	AUTOPILOTPLAYTEST_API TArray<TSharedPtr<FJsonValue>> ToJson(const TArray<FPlaytestModelUsage>& Models);

	/** Adds Run's counts to Total, model by model (the same kind and id add up); LLMs first, then decision models. */
	AUTOPILOTPLAYTEST_API void Add(TArray<FPlaytestModelUsage>& Total, const TArray<FPlaytestModelUsage>& Run);

	/** The model's id, as the Settings page shows it. */
	AUTOPILOTPLAYTEST_API FString Label(const FPlaytestModelUsage& Model);

	/** The input and output tokens of one kind of model ("llm" or "decision"), summed. */
	AUTOPILOTPLAYTEST_API int64 Tokens(const TArray<FPlaytestModelUsage>& Models, const FString& Kind);
}
