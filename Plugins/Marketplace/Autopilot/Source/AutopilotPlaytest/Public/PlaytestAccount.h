#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * How the agent service runs (its `account` frame, docs/protocol.md "Account"): through Aura, with the user's own keys,
 * or not set up. The plugin never holds Aura's tokens: the agent service signs in and keeps the session for the computer.
 */
/** What Autopilot billed this Aura account in the current period: the account frame's "bill" (mode aura, signed in). */
struct AUTOPILOTPLAYTEST_API FPlaytestPeriodBill
{
	/** The frame had one (absent or null: Aura couldn't say, and the account card shows nothing). */
	bool bKnown = false;
	double Usd = 0.0;
	int32 Runs = 0;
	/** The period's first and last days, as sent ("2026-10-01"). */
	FString From;
	FString To;
};

/** The agent service's `bill` frame: what Autopilot was billed for a run through Aura (after the run, before its result). */
struct AUTOPILOTPLAYTEST_API FPlaytestRunBill
{
	bool bKnown = false;
	double Usd = 0.0;
	/** False when Aura bills it later or didn't (Reason says why): nothing shows for it. */
	bool bBilled = false;
	FString Reason;

	/** "Billed $0.59" shows for it. */
	bool Shows() const { return bKnown && bBilled; }
};

struct AUTOPILOTPLAYTEST_API FPlaytestAccount
{
	/** An account frame came (on this connection or an earlier one). */
	bool bKnown = false;
	/** "aura", "own_keys" or "none". */
	FString Mode;
	FString Email;
	bool bSignedIn = false;
	/** The last characters of the keys the agent service has ("••••a91f"), "" when it has none. */
	FString DecisionKey;
	FString LlmKey;
	/**
	 * The Coding Agent BYOK runs use ("coding_agent"): "opencode", "claude" or "codex"; "opencode" when the frame has
	 * none (an older agent service) or names another. bCodingAgentSent says whether it had one.
	 */
	FString CodingAgent = TEXT("opencode");
	bool bCodingAgentSent = false;
	/**
	 * The LLM provider whose key LlmKey is ("llm_provider"): one of PlaytestAccount::LlmProviders, or another the agent
	 * service named. "anthropic" when the frame has none (an older agent service); bLlmProviderSent says whether it had one.
	 */
	FString LlmProvider = TEXT("anthropic");
	bool bLlmProviderSent = false;
	/** What Autopilot billed this account in the current period (Aura only). */
	FPlaytestPeriodBill Bill;
	/**
	 * Aura's words when the team has no credits left for a run ("out_of_credits": Aura only, signed in), "" while it has
	 * some (or an older agent service): shown over the prompt box and on the Settings page's account card.
	 */
	FString OutOfCredits;
	/** Where Aura says to add credits ("" when it names nowhere): the notices' Add credits. */
	FString AddCreditsUrl;
};

/** The last `aura_sign_in` frame: the sign-in on Aura's website that the plugin asked the agent service for. */
struct AUTOPILOTPLAYTEST_API FPlaytestSignIn
{
	/** "" (none asked), "asked" (sent, nothing back yet), "waiting", "signed_in" or "error". */
	FString State;
	/** The page the browser was sent to (waiting). */
	FString Url;
	/** Who signed in (signed_in). */
	FString Email;
	/** Why not (error): "cancelled", "denied", "timeout", "unreachable" or "refused". */
	FString Reason;
	FString Message;
};

/** What the tab shows for a sign-in. */
enum class EPlaytestSignInPhase : uint8
{
	Idle,      // nothing asked, or cancelled by the user (shows nothing)
	Asked,     // asked, the agent service hasn't answered yet
	Waiting,   // the browser is open on Aura's sign-in
	SignedIn,
	Error,
};

/** How the Settings page's account corner says runs go. */
enum class EPlaytestAccountKind : uint8
{
	NotSetUp,
	Aura,       // signed in to Aura through the agent service
	AuraApp,    // Use with Aura: the Aura app's agent service
	OwnKeys,
	SignedOut,  // Aura chosen, its sign-in on this computer ended
};

namespace PlaytestAccount
{
	/**
	 * What the account corner shows: Use with Aura first; then the agent service's account frame (mode aura: signed in
	 * or not; own_keys); a frame saying none (or none yet) falls back to the setup's choice (preferences.json access).
	 */
	AUTOPILOTPLAYTEST_API EPlaytestAccountKind UsageKind(const FPlaytestAccount& Account, const FString& Access, bool bUseWithAura);

	/** Reads an `account` frame into Out. False when Message isn't one. */
	AUTOPILOTPLAYTEST_API bool ParseAccount(const TSharedPtr<FJsonObject>& Message, FPlaytestAccount& Out);

	/** The Coding Agents the first-run setup offers, in its dropdown's order: "claude", "codex", "opencode" (the default). */
	AUTOPILOTPLAYTEST_API const TArray<FString>& CodingAgents();

	/** An agent as the tab names it: "Claude Code", "Codex", "OpenCode" (another, or "", reads as OpenCode). */
	AUTOPILOTPLAYTEST_API FString AgentName(const FString& Agent);

	/** The one LLM provider an agent takes: "anthropic" for Claude Code, "openai" for Codex; "" for OpenCode (any). */
	AUTOPILOTPLAYTEST_API FString AgentProvider(const FString& Agent);

	/**
	 * The LLM providers the first-run setup offers OpenCode, in its dropdown's order: "anthropic" (the default),
	 * "openai", "opencode" (OpenCode Zen), "openrouter", "deepseek", "google" (Gemini).
	 */
	AUTOPILOTPLAYTEST_API const TArray<FString>& LlmProviders();

	/** A provider as the tab names it: "Anthropic", "OpenAI", "OpenCode Zen", ..., another as sent ("" reads as Anthropic). */
	AUTOPILOTPLAYTEST_API FString ProviderName(const FString& Provider);

	/**
	 * The agent the setup's Coding Agent dropdown starts on: the account frame's coding_agent when the agent service
	 * sent one, else Saved (autopilot.env's AUTOPILOT_AGENT), else "opencode". One the dropdown doesn't offer: "opencode".
	 */
	AUTOPILOTPLAYTEST_API FString PickAgent(const FPlaytestAccount& Account, const FString& Saved);

	/**
	 * The provider the setup's dropdown starts on for OpenCode: the account frame's llm_provider when the agent service
	 * sent one for OpenCode (its coding_agent is OpenCode, or it sent none), else Saved (autopilot.env's
	 * AUTOPILOT_OPENCODE_PROVIDER), else "anthropic". One the dropdown doesn't offer: "anthropic".
	 */
	AUTOPILOTPLAYTEST_API FString PickProvider(const FPlaytestAccount& Account, const FString& Saved);

	/**
	 * The keys the agent service has, as the account corner says them in BYOK: "Saved on this computer: Decision
	 * provider ••••a91f · OpenCode (Anthropic) ••••3c2e" (a key it hasn't is left out; "" when it has neither).
	 */
	AUTOPILOTPLAYTEST_API FString SavedKeysLine(const FPlaytestAccount& Account);

	/** Reads an `aura_sign_in` frame into Out. False when Message isn't one. */
	AUTOPILOTPLAYTEST_API bool ParseSignIn(const TSharedPtr<FJsonObject>& Message, FPlaytestSignIn& Out);

	/** The phase the tab shows for a sign-in (an error the user caused by cancelling shows nothing). */
	AUTOPILOTPLAYTEST_API EPlaytestSignInPhase Phase(const FPlaytestSignIn& SignIn);

	/**
	 * A sign-in going on (asked or waiting) at an account frame on a connection it wasn't asked on, whose replies may
	 * have gone with the old one: true when the frame has the session it was for (it finished meanwhile; Out is the
	 * signed_in it said), else false: asked again, the agent service says where it is (or starts it again).
	 */
	AUTOPILOTPLAYTEST_API bool SignInFinishedAway(const FPlaytestSignIn& SignIn, const FPlaytestAccount& Account, FPlaytestSignIn& Out);

	/** A sign-in sent and not answered for good yet: asked or waiting. */
	AUTOPILOTPLAYTEST_API bool IsSignInGoing(const FPlaytestSignIn& SignIn);

	/** The one plain line an error shows: by its reason, else the agent service's words. */
	AUTOPILOTPLAYTEST_API FString ErrorLine(const FPlaytestSignIn& SignIn);

	/** Reads a `bill` frame into Out. False when Message isn't one. */
	AUTOPILOTPLAYTEST_API bool ParseBill(const TSharedPtr<FJsonObject>& Message, FPlaytestRunBill& Out);

	/** A run's bill as saved with it ({"usd", "billed", "reason"?}), and read back (a run saved without one: none). */
	AUTOPILOTPLAYTEST_API TSharedRef<FJsonObject> BillToJson(const FPlaytestRunBill& Bill);
	AUTOPILOTPLAYTEST_API FPlaytestRunBill BillFromJson(const TSharedPtr<FJsonObject>& Json);

	/** Dollars as the Settings page shows them: "$0.59", "$1,234.50" (2 decimals); "< $0.01" below a cent (not 0). */
	AUTOPILOTPLAYTEST_API FString Money(double Usd);

	/** "Billed $0.59". */
	AUTOPILOTPLAYTEST_API FString BilledLine(double Usd);

	/** "Billing period (Oct 1 – Oct 31): $12.34 USD" ("Billing period: $12.34 USD" without its days); "" when the bill isn't known. */
	AUTOPILOTPLAYTEST_API FString PeriodLine(const FPlaytestPeriodBill& Bill);

	/** The period's days: "Oct 1 – Oct 31" ("Dec 1, 2026 – Jan 31, 2027" across years; as sent when not dates; "" for none). */
	AUTOPILOTPLAYTEST_API FString PeriodDates(const FPlaytestPeriodBill& Bill);
}
