// Automation tests of the user's choices, the setup's keys and the usage counts (Session Frontend > Automation:
// AutopilotPlaytest.Preferences, AutopilotPlaytest.Usage).

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PlaytestJson.h"
#include "PlaytestPreferences.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A folder of its own under Saved/Automation, removed when done. */
	struct FScratchDir
	{
		FString Path;
		FScratchDir()
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Autopilot"),
				FGuid::NewGuid().ToString()));
			IFileManager::Get().MakeDirectory(*Path, true);
		}
		~FScratchDir()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestPreferencesFileTest, "AutopilotPlaytest.Preferences.File",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestPreferencesFileTest::RunTest(const FString& Parameters)
{
	FScratchDir Dir;
	const FString File = FPaths::Combine(Dir.Path, TEXT("preferences.json"));

	// No file: the defaults
	FPlaytestPreferences Prefs = PlaytestPreferences::Load(Dir.Path);
	TestFalse(TEXT("not onboarded"), Prefs.bOnboarded);
	TestTrue(TEXT("sharing by default"), Prefs.bShareData);
	TestFalse(TEXT("the research agent not chosen"), Prefs.ResearchChoice.IsSet());
	TestTrue(TEXT("no access"), Prefs.Access.IsEmpty());

	// Corrupt: the defaults too
	FFileHelper::SaveStringToFile(TEXT("{not json"), *File);
	Prefs = PlaytestPreferences::Load(Dir.Path);
	TestFalse(TEXT("corrupt: not onboarded"), Prefs.bOnboarded);
	TestTrue(TEXT("corrupt: sharing"), Prefs.bShareData);
	TestFalse(TEXT("corrupt: the research agent not chosen"), Prefs.ResearchChoice.IsSet());

	// Written, merged with keys another plugin keeps there
	FFileHelper::SaveStringToFile(TEXT("{\"version\": 1, \"theme\": \"dark\", \"access\": \"aura\"}"), *File);
	Prefs = PlaytestPreferences::Load(Dir.Path);
	TestEqual(TEXT("access read"), Prefs.Access, FString(TEXT("aura")));
	Prefs.bOnboarded = true;
	Prefs.bShareData = false;
	// (the research agent not chosen: nothing written for it, so the agent service decides)
	PlaytestPreferences::Save(Prefs, Dir.Path);
	FString Unchosen;
	FFileHelper::LoadFileToString(Unchosen, *File);
	TestFalse(TEXT("nothing chosen: no research_agent written"), Unchosen.Contains(TEXT("research_agent")));
	Prefs.ResearchChoice = false;
	Prefs.Access.Empty();  // (skipped: the choice made before stays)
	TestTrue(TEXT("saved"), PlaytestPreferences::Save(Prefs, Dir.Path));
	TestFalse(TEXT("no temp file left"), FPaths::FileExists(File + TEXT(".tmp")));
	FString Text;
	FFileHelper::LoadFileToString(Text, *File);
	const TSharedPtr<FJsonObject> Json = PlaytestJson::Parse(Text);
	if (!TestTrue(TEXT("valid JSON"), Json.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("unknown key kept"), Json->GetStringField(TEXT("theme")), FString(TEXT("dark")));
	TestEqual(TEXT("version"), (int32)Json->GetNumberField(TEXT("version")), 1);
	TestTrue(TEXT("onboarded"), Json->GetBoolField(TEXT("onboarded")));
	TestFalse(TEXT("share_data"), Json->GetBoolField(TEXT("share_data")));
	TestFalse(TEXT("research_agent"), Json->GetBoolField(TEXT("research_agent")));
	TestEqual(TEXT("access kept"), Json->GetStringField(TEXT("access")), FString(TEXT("aura")));
	Prefs = PlaytestPreferences::Load(Dir.Path);
	TestTrue(TEXT("read back: onboarded"), Prefs.bOnboarded);
	TestFalse(TEXT("read back: not sharing"), Prefs.bShareData);
	TestTrue(TEXT("read back: the research agent off"), Prefs.ResearchChoice.IsSet() && !Prefs.ResearchChoice.GetValue());

	Prefs.Access = TEXT("own_keys");
	PlaytestPreferences::Save(Prefs, Dir.Path);
	TestEqual(TEXT("access changed"), PlaytestPreferences::Load(Dir.Path).Access, FString(TEXT("own_keys")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestPreferencesKeysTest, "AutopilotPlaytest.Preferences.Keys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestPreferencesKeysTest::RunTest(const FString& Parameters)
{
	using FKeys = TArray<TPair<FString, FString>>;
	// Into nothing
	TestEqual(TEXT("new file"), PlaytestPreferences::MergeKeys(FString(), FKeys{{TEXT("TYPESAFE_API_KEY"), TEXT("ts-1")}, {TEXT("ANTHROPIC_API_KEY"), TEXT("")}}),
		FString(TEXT("TYPESAFE_API_KEY=ts-1\n")));
	// Replaced in place, other lines kept, duplicates of the key dropped, a new key appended
	const FString Existing = TEXT("# my keys\r\nOTHER=1\r\nexport ANTHROPIC_API_KEY=old\r\n\r\nANTHROPIC_API_KEY = older\r\n");
	const FString Merged = PlaytestPreferences::MergeKeys(Existing, FKeys{{TEXT("ANTHROPIC_API_KEY"), TEXT("  sk-new\n")}, {TEXT("TYPESAFE_API_KEY"), TEXT("ts-2")}});
	TestEqual(TEXT("merged"), Merged, FString(TEXT("# my keys\nOTHER=1\nANTHROPIC_API_KEY=sk-new\n\nTYPESAFE_API_KEY=ts-2\n")));
	// Nothing given: unchanged
	TestEqual(TEXT("nothing to write"), PlaytestPreferences::MergeKeys(TEXT("A=1\n"), FKeys{{TEXT("A"), TEXT(" ")}}), FString(TEXT("A=1\n")));

	FScratchDir Dir;
	TestTrue(TEXT("no keys: nothing written"), PlaytestPreferences::WriteKeys(FKeys{{TEXT("ANTHROPIC_API_KEY"), TEXT("")}}, Dir.Path).IsEmpty());
	TestFalse(TEXT("no file for no keys"), FPaths::FileExists(FPaths::Combine(Dir.Path, TEXT("autopilot.env"))));
	const FString Sub = FPaths::Combine(Dir.Path, TEXT("made"));
	TestTrue(TEXT("written"), PlaytestPreferences::WriteKeys(FKeys{{TEXT("ANTHROPIC_API_KEY"), TEXT("sk-1")}}, Sub).IsEmpty());
	PlaytestPreferences::WriteKeys(FKeys{{TEXT("TYPESAFE_API_KEY"), TEXT("ts-1")}}, Sub);
	FString Text;
	FFileHelper::LoadFileToString(Text, *FPaths::Combine(Sub, TEXT("autopilot.env")));
	TestEqual(TEXT("both keys, folder made"), Text, FString(TEXT("ANTHROPIC_API_KEY=sk-1\nTYPESAFE_API_KEY=ts-1\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestPreferencesProviderTest, "AutopilotPlaytest.Preferences.LlmProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestPreferencesProviderTest::RunTest(const FString& Parameters)
{
	using FKeys = TArray<TPair<FString, FString>>;
	auto Names = [](const FKeys& Keys)
	{
		TArray<FString> Out;
		for (const TPair<FString, FString>& Key : Keys)
		{
			Out.Add(Key.Key);
		}
		return FString::Join(Out, TEXT(","));
	};
	// Each provider's key name (another: Anthropic's)
	TestEqual(TEXT("anthropic's key"), PlaytestPreferences::LlmKeyName(TEXT("anthropic")), FString(TEXT("ANTHROPIC_API_KEY")));
	TestEqual(TEXT("openai's key"), PlaytestPreferences::LlmKeyName(TEXT("openai")), FString(TEXT("OPENAI_API_KEY")));
	TestEqual(TEXT("opencode zen's key"), PlaytestPreferences::LlmKeyName(TEXT("opencode")), FString(TEXT("OPENCODE_API_KEY")));
	TestEqual(TEXT("openrouter's key"), PlaytestPreferences::LlmKeyName(TEXT("openrouter")), FString(TEXT("OPENROUTER_API_KEY")));
	TestEqual(TEXT("deepseek's key"), PlaytestPreferences::LlmKeyName(TEXT("deepseek")), FString(TEXT("DEEPSEEK_API_KEY")));
	TestEqual(TEXT("gemini's key"), PlaytestPreferences::LlmKeyName(TEXT(" Google ")), FString(TEXT("GEMINI_API_KEY")));
	TestEqual(TEXT("another's key: anthropic's"), PlaytestPreferences::LlmKeyName(TEXT("mistral")), FString(TEXT("ANTHROPIC_API_KEY")));

	// What "Use my keys" writes: the agent always, OpenCode's provider for OpenCode only, the LLM key under its provider's name
	TestEqual(TEXT("opencode anthropic's names"), Names(PlaytestPreferences::SetupKeys(TEXT("ts"), TEXT("opencode"), TEXT("anthropic"), TEXT("sk"))),
		FString(TEXT("TYPESAFE_API_KEY,AUTOPILOT_AGENT,AUTOPILOT_OPENCODE_PROVIDER,ANTHROPIC_API_KEY")));
	TestEqual(TEXT("opencode openai's names"), Names(PlaytestPreferences::SetupKeys(TEXT("ts"), TEXT("opencode"), TEXT("openai"), TEXT("sk"))),
		FString(TEXT("TYPESAFE_API_KEY,AUTOPILOT_AGENT,AUTOPILOT_OPENCODE_PROVIDER,OPENAI_API_KEY")));
	TestEqual(TEXT("opencode gemini's names"), Names(PlaytestPreferences::SetupKeys(TEXT("ts"), TEXT("opencode"), TEXT("google"), TEXT("sk"))),
		FString(TEXT("TYPESAFE_API_KEY,AUTOPILOT_AGENT,AUTOPILOT_OPENCODE_PROVIDER,GEMINI_API_KEY")));
	TestEqual(TEXT("another provider: anthropic"), Names(PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("mistral"), TEXT("sk"))),
		FString(TEXT("TYPESAFE_API_KEY,AUTOPILOT_AGENT,AUTOPILOT_OPENCODE_PROVIDER,ANTHROPIC_API_KEY")));
	TestEqual(TEXT("claude code's names (Anthropic's, whatever was given)"), Names(PlaytestPreferences::SetupKeys(TEXT("ts"), TEXT("claude"), TEXT("openrouter"), TEXT("sk"))),
		FString(TEXT("TYPESAFE_API_KEY,AUTOPILOT_AGENT,ANTHROPIC_API_KEY")));
	TestEqual(TEXT("codex's names (OpenAI's)"), Names(PlaytestPreferences::SetupKeys(TEXT("ts"), TEXT("codex"), TEXT("anthropic"), TEXT("sk"))),
		FString(TEXT("TYPESAFE_API_KEY,AUTOPILOT_AGENT,OPENAI_API_KEY")));

	// Into nothing
	TestEqual(TEXT("anthropic, new file"), PlaytestPreferences::MergeKeys(FString(), PlaytestPreferences::SetupKeys(TEXT("ts-1"), TEXT("opencode"), TEXT("anthropic"), TEXT("sk-ant"))),
		FString(TEXT("TYPESAFE_API_KEY=ts-1\nAUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=anthropic\nANTHROPIC_API_KEY=sk-ant\n")));
	TestEqual(TEXT("openai, new file"), PlaytestPreferences::MergeKeys(FString(), PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("openai"), TEXT("sk-oai"))),
		FString(TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=openai\nOPENAI_API_KEY=sk-oai\n")));
	TestEqual(TEXT("an unknown agent: opencode"), PlaytestPreferences::MergeKeys(FString(), PlaytestPreferences::SetupKeys(TEXT(""), TEXT("aider"), TEXT("deepseek"), TEXT("sk-ds"))),
		FString(TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=deepseek\nDEEPSEEK_API_KEY=sk-ds\n")));
	// Switched to OpenAI: the provider replaced in place, Anthropic's key line kept, OpenAI's added
	const FString Existing = TEXT("# mine\nTYPESAFE_API_KEY=ts-1\nAUTOPILOT_OPENCODE_PROVIDER=anthropic\nANTHROPIC_API_KEY=sk-ant\nOTHER=1\n");
	TestEqual(TEXT("to openai"), PlaytestPreferences::MergeKeys(Existing, PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("openai"), TEXT("sk-oai"))),
		FString(TEXT("# mine\nTYPESAFE_API_KEY=ts-1\nAUTOPILOT_OPENCODE_PROVIDER=openai\nANTHROPIC_API_KEY=sk-ant\nOTHER=1\nAUTOPILOT_AGENT=opencode\nOPENAI_API_KEY=sk-oai\n")));
	// Back to Anthropic with its key empty: its line kept as it was, OpenAI's kept too
	const FString Both = TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=openai\nANTHROPIC_API_KEY=sk-ant\nOPENAI_API_KEY=sk-oai\n");
	TestEqual(TEXT("to anthropic, key empty"), PlaytestPreferences::MergeKeys(Both, PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("anthropic"), TEXT(" "))),
		FString(TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=anthropic\nANTHROPIC_API_KEY=sk-ant\nOPENAI_API_KEY=sk-oai\n")));
	// OpenAI's key changed, Anthropic's line untouched
	TestEqual(TEXT("openai key replaced"), PlaytestPreferences::MergeKeys(Both, PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("openai"), TEXT("sk-oai-2"))),
		FString(TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=openai\nANTHROPIC_API_KEY=sk-ant\nOPENAI_API_KEY=sk-oai-2\n")));
	// Claude Code: the agent replaced, OpenCode's provider line left as it was, Anthropic's key replaced
	TestEqual(TEXT("to claude code"), PlaytestPreferences::MergeKeys(Both, PlaytestPreferences::SetupKeys(TEXT(""), TEXT("claude"), TEXT("openai"), TEXT("sk-ant-2"))),
		FString(TEXT("AUTOPILOT_AGENT=claude\nAUTOPILOT_OPENCODE_PROVIDER=openai\nANTHROPIC_API_KEY=sk-ant-2\nOPENAI_API_KEY=sk-oai\n")));
	// Codex with its key empty: only the agent changes
	TestEqual(TEXT("to codex, key empty"), PlaytestPreferences::MergeKeys(Both, PlaytestPreferences::SetupKeys(TEXT(""), TEXT("codex"), TEXT(""), TEXT(""))),
		FString(TEXT("AUTOPILOT_AGENT=codex\nAUTOPILOT_OPENCODE_PROVIDER=openai\nANTHROPIC_API_KEY=sk-ant\nOPENAI_API_KEY=sk-oai\n")));
	// OpenRouter: its own key line added, the others kept
	TestEqual(TEXT("to openrouter"), PlaytestPreferences::MergeKeys(Both, PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("openrouter"), TEXT("sk-or"))),
		FString(TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=openrouter\nANTHROPIC_API_KEY=sk-ant\nOPENAI_API_KEY=sk-oai\nOPENROUTER_API_KEY=sk-or\n")));

	// Read back: the last line setting it, quotes off, "" for none
	TestEqual(TEXT("read"), PlaytestPreferences::KeyIn(TEXT("A=1\r\nAUTOPILOT_OPENCODE_PROVIDER=anthropic\r\n"), TEXT("AUTOPILOT_OPENCODE_PROVIDER")), FString(TEXT("anthropic")));
	TestEqual(TEXT("last wins, export, quotes"), PlaytestPreferences::KeyIn(TEXT("AUTOPILOT_OPENCODE_PROVIDER=anthropic\nexport AUTOPILOT_OPENCODE_PROVIDER = \"openai\"\n"),
		TEXT("AUTOPILOT_OPENCODE_PROVIDER")), FString(TEXT("openai")));
	TestTrue(TEXT("a comment isn't read"), PlaytestPreferences::KeyIn(TEXT("# AUTOPILOT_OPENCODE_PROVIDER=openai\n"), TEXT("AUTOPILOT_OPENCODE_PROVIDER")).IsEmpty());
	TestTrue(TEXT("none"), PlaytestPreferences::KeyIn(TEXT("OTHER=1\n"), TEXT("AUTOPILOT_OPENCODE_PROVIDER")).IsEmpty());

	// The file: written with the provider, read back; no file reads as none
	FScratchDir Dir;
	TestTrue(TEXT("no file: none"), PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_OPENCODE_PROVIDER"), Dir.Path).IsEmpty());
	TestTrue(TEXT("written"), PlaytestPreferences::WriteKeys(PlaytestPreferences::SetupKeys(TEXT(""), TEXT("opencode"), TEXT("openai"), TEXT("")), Dir.Path).IsEmpty());
	FString Text;
	FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir.Path, TEXT("autopilot.env")));
	TestEqual(TEXT("only the agent and provider, no empty key"), Text, FString(TEXT("AUTOPILOT_AGENT=opencode\nAUTOPILOT_OPENCODE_PROVIDER=openai\n")));
	TestEqual(TEXT("read back"), PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_OPENCODE_PROVIDER"), Dir.Path), FString(TEXT("openai")));
	TestEqual(TEXT("agent read back"), PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_AGENT"), Dir.Path), FString(TEXT("opencode")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestUsageTest, "AutopilotPlaytest.Usage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestUsageTest::RunTest(const FString& Parameters)
{
	bool bFinal = false;
	const TArray<FPlaytestModelUsage> Run = PlaytestUsage::Parse(PlaytestJson::Parse(TEXT(
		"{\"type\": \"usage\", \"final\": true, \"models\": ["
		"{\"kind\": \"decision\", \"model\": \"dm-1\", \"calls\": 2310, \"input\": 1450000, \"output\": 4620},"
		"{\"kind\": \"llm\", \"model\": \"claude-opus-5-5\", \"calls\": 12, \"input\": 30120, \"output\": 8211,"
		" \"cache_read\": 912000, \"cache_write\": 41000}, \"junk\"]}")), &bFinal);
	TestTrue(TEXT("final"), bFinal);
	if (!TestEqual(TEXT("two models"), Run.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("decision"), Run[0].IsDecision());
	TestEqual(TEXT("decision input"), Run[0].Input, (int64)1450000);
	TestEqual(TEXT("no cache: 0"), Run[0].CacheRead, (int64)0);
	TestEqual(TEXT("cache read"), Run[1].CacheRead, (int64)912000);
	TestEqual(TEXT("label llm: its id"), PlaytestUsage::Label(Run[1]), FString(TEXT("claude-opus-5-5")));
	TestEqual(TEXT("label decision: its id"), PlaytestUsage::Label(Run[0]), FString(TEXT("dm-1")));
	TestEqual(TEXT("llm tokens"), PlaytestUsage::Tokens(Run, TEXT("llm")), (int64)(30120 + 8211));

	// Saved with a run and read back
	TSharedRef<FJsonObject> Saved = MakeShared<FJsonObject>();
	Saved->SetArrayField(TEXT("models"), PlaytestUsage::ToJson(Run));
	const TArray<FPlaytestModelUsage> Back = PlaytestUsage::Parse(PlaytestJson::Parse(PlaytestJson::Write(Saved)));
	TestEqual(TEXT("round trip"), Back.Num(), 2);
	TestEqual(TEXT("round trip cache write"), Back[1].CacheWrite, (int64)41000);

	// Summed over runs: the same model adds up, LLMs first
	TArray<FPlaytestModelUsage> Total;
	PlaytestUsage::Add(Total, Run);
	PlaytestUsage::Add(Total, Back);
	TestEqual(TEXT("two rows"), Total.Num(), 2);
	TestTrue(TEXT("llm first"), Total[0].IsLlm());
	TestEqual(TEXT("calls summed"), Total[0].Calls, (int64)24);
	TestEqual(TEXT("decision summed"), Total[1].Input, (int64)2900000);

	// No message, no models
	TestEqual(TEXT("nothing"), PlaytestUsage::Parse(nullptr).Num(), 0);
	return true;
}

#endif
