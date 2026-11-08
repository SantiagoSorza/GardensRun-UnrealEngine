// The user's choices (preferences.json), the first-run setup's keys (autopilot.env) and the agent service's token counts
// (`usage`): docs/protocol.md, "Recording, stills and usage" and "Sharing a run's data".

#include "PlaytestPreferences.h"

#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PlaytestAccount.h"
#include "PlaytestAura.h"
#include "PlaytestInternal.h"
#include "PlaytestJson.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

namespace
{
	const TCHAR* PreferencesFile = TEXT("preferences.json");
	const TCHAR* KeysFile = TEXT("autopilot.env");

	FString DirOr(const FString& Dir)
	{
		return Dir.IsEmpty() ? PlaytestAura::SettingsDir() : Dir;
	}

	/** The key an autopilot.env line sets ("" for a comment, a blank line or anything else). */
	FString KeyOf(const FString& Line)
	{
		FString T = Line.TrimStartAndEnd();
		if (T.IsEmpty() || T.StartsWith(TEXT("#")))
		{
			return FString();
		}
		if (T.StartsWith(TEXT("export ")))
		{
			T = T.Mid(7).TrimStart();
		}
		int32 Equals = INDEX_NONE;
		if (!T.FindChar(TEXT('='), Equals) || Equals <= 0)
		{
			return FString();
		}
		return T.Left(Equals).TrimEnd();
	}

	/** A key's value as one line: no line breaks, no spaces round it. */
	FString OneLine(const FString& Value)
	{
		FString Out = Value;
		Out.ReplaceInline(TEXT("\r"), TEXT(""));
		Out.ReplaceInline(TEXT("\n"), TEXT(""));
		return Out.TrimStartAndEnd();
	}

	int64 Count(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		double Value = 0.0;
		if (Json.IsValid() && Json->TryGetNumberField(Field, Value) && FMath::IsFinite(Value))
		{
			return FMath::Max<int64>(0, (int64)Value);
		}
		return 0;
	}
}

namespace PlaytestPreferences
{
	bool WriteFileAtomic(const FString& Path, const FString& Text, bool bPrivate)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		const FString Temp = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
#if PLATFORM_MAC || PLATFORM_LINUX
		if (bPrivate)
		{
			chmod(TCHAR_TO_UTF8(*Temp), S_IRUSR | S_IWUSR);  // (readable by the user only)
		}
#endif
		if (!IFileManager::Get().Move(*Path, *Temp, true, true))
		{
			IFileManager::Get().Delete(*Temp, false, true, true);
			return false;
		}
		return true;
	}

	FPlaytestPreferences Load(const FString& Dir)
	{
		FPlaytestPreferences Prefs;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(DirOr(Dir), PreferencesFile)))
		{
			return Prefs;
		}
		const TSharedPtr<FJsonObject> Json = PlaytestJson::Parse(Text);
		if (!Json.IsValid())
		{
			return Prefs;  // (unreadable: the defaults, and it is written afresh on the next save)
		}
		Prefs.Raw = Json;
		Json->TryGetBoolField(TEXT("onboarded"), Prefs.bOnboarded);
		Json->TryGetBoolField(TEXT("share_data"), Prefs.bShareData);
		bool bResearch = false;
		if (Json->TryGetBoolField(TEXT("research_agent"), bResearch))
		{
			Prefs.ResearchChoice = bResearch;
		}
		Json->TryGetStringField(TEXT("access"), Prefs.Access);
		return Prefs;
	}

	FString PreferencesPath(const FString& Dir)
	{
		return FPaths::Combine(DirOr(Dir), PreferencesFile);
	}

	bool Save(const FPlaytestPreferences& Prefs, const FString& Dir)
	{
		// Merged with what is there now (another engine's plugin may have written it since this one read it)
		const FPlaytestPreferences Now = Load(Dir);
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (Now.Raw.IsValid())
		{
			Json->Values = Now.Raw->Values;
		}
		Json->SetNumberField(TEXT("version"), 1);
		Json->SetBoolField(TEXT("onboarded"), Prefs.bOnboarded);
		Json->SetBoolField(TEXT("share_data"), Prefs.bShareData);
		if (Prefs.ResearchChoice.IsSet())
		{
			Json->SetBoolField(TEXT("research_agent"), Prefs.ResearchChoice.GetValue());
		}
		if (!Prefs.Access.IsEmpty())
		{
			Json->SetStringField(TEXT("access"), Prefs.Access);
		}
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Json, Writer))
		{
			return false;
		}
		const bool bOk = WriteFileAtomic(FPaths::Combine(DirOr(Dir), PreferencesFile), Text + TEXT("\n"));
		if (!bOk)
		{
			UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] Couldn't write %s"), *FPaths::Combine(DirOr(Dir), PreferencesFile));
		}
		return bOk;
	}

	namespace
	{
		/** -Name=0|1 (or false/true, no/yes, off/on) on the command line: 0 or 1; -1 when it isn't there or says neither. */
		int32 CommandLineSwitch(const TCHAR* Name)
		{
			FString Given;
			if (FParse::Value(FCommandLine::Get(), Name, Given))
			{
				Given = Given.TrimQuotes().TrimStartAndEnd().ToLower();
				if (Given == TEXT("0") || Given == TEXT("false") || Given == TEXT("no") || Given == TEXT("off"))
				{
					return 0;
				}
				if (Given == TEXT("1") || Given == TEXT("true") || Given == TEXT("yes") || Given == TEXT("on"))
				{
					return 1;
				}
			}
			return -1;
		}
	}

	bool ShareData()
	{
		const int32 Given = CommandLineSwitch(TEXT("PlaytestShare="));
		return Given >= 0 ? Given == 1 : Load().bShareData;
	}

	TOptional<bool> ResearchAgent()
	{
		const int32 Given = CommandLineSwitch(TEXT("PlaytestResearch="));
		return Given >= 0 ? TOptional<bool>(Given == 1) : Load().ResearchChoice;
	}

	FString MergeKeys(const FString& Existing, const TArray<TPair<FString, FString>>& Keys)
	{
		TMap<FString, FString> Wanted;
		TArray<FString> Order;
		for (const TPair<FString, FString>& Key : Keys)
		{
			const FString Name = Key.Key.TrimStartAndEnd();
			const FString Value = OneLine(Key.Value);
			if (Name.IsEmpty() || Value.IsEmpty())
			{
				continue;
			}
			if (!Wanted.Contains(Name))
			{
				Order.Add(Name);
			}
			Wanted.Add(Name, Value);
		}
		TArray<FString> Lines;
		FString Normalized = Existing;
		Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Normalized.ParseIntoArray(Lines, TEXT("\n"), false);
		while (Lines.Num() && Lines.Last().TrimStartAndEnd().IsEmpty())
		{
			Lines.Pop();
		}
		TSet<FString> Written;
		TArray<FString> Out;
		for (const FString& Line : Lines)
		{
			const FString Name = KeyOf(Line);
			if (const FString* Value = Wanted.Find(Name))
			{
				if (!Written.Contains(Name))
				{
					Out.Add(FString::Printf(TEXT("%s=%s"), *Name, **Value));  // (the same key again is left out)
					Written.Add(Name);
				}
				continue;
			}
			Out.Add(Line);
		}
		for (const FString& Name : Order)
		{
			if (!Written.Contains(Name))
			{
				Out.Add(FString::Printf(TEXT("%s=%s"), *Name, *Wanted[Name]));
			}
		}
		return Out.Num() ? FString::Join(Out, TEXT("\n")) + TEXT("\n") : FString();
	}

	FString WriteKeys(const TArray<TPair<FString, FString>>& Keys, const FString& Dir)
	{
		bool bAny = false;
		for (const TPair<FString, FString>& Key : Keys)
		{
			bAny |= !OneLine(Key.Value).IsEmpty();
		}
		if (!bAny)
		{
			return FString();
		}
		const FString Path = FPaths::Combine(DirOr(Dir), KeysFile);
		FString Existing;
		FFileHelper::LoadFileToString(Existing, *Path);
		if (!WriteFileAtomic(Path, MergeKeys(Existing, Keys), true))
		{
			return FString::Printf(TEXT("Couldn’t save the keys to %s (error %u)"), *FPaths::ConvertRelativePathToFull(Path), FPlatformMisc::GetLastError());
		}
		return FString();
	}

	FString LlmKeyName(const FString& Provider)
	{
		static const TMap<FString, FString> Names = { { TEXT("anthropic"), TEXT("ANTHROPIC_API_KEY") }, { TEXT("openai"), TEXT("OPENAI_API_KEY") },
			{ TEXT("opencode"), TEXT("OPENCODE_API_KEY") }, { TEXT("openrouter"), TEXT("OPENROUTER_API_KEY") },
			{ TEXT("deepseek"), TEXT("DEEPSEEK_API_KEY") }, { TEXT("google"), TEXT("GEMINI_API_KEY") } };
		const FString* Name = Names.Find(Provider.TrimStartAndEnd().ToLower());
		return Name ? *Name : FString(TEXT("ANTHROPIC_API_KEY"));
	}

	TArray<TPair<FString, FString>> SetupKeys(const FString& DecisionKey, const FString& Agent, const FString& Provider, const FString& LlmKey)
	{
		FString AgentId = Agent.TrimStartAndEnd().ToLower();
		if (!PlaytestAccount::CodingAgents().Contains(AgentId))
		{
			AgentId = TEXT("opencode");
		}
		// Claude Code and Codex take one provider each; OpenCode the one picked (one it doesn't offer: Anthropic)
		FString ProviderId = PlaytestAccount::AgentProvider(AgentId);
		const bool bOpenCode = ProviderId.IsEmpty();
		if (bOpenCode)
		{
			ProviderId = Provider.TrimStartAndEnd().ToLower();
			if (!PlaytestAccount::LlmProviders().Contains(ProviderId))
			{
				ProviderId = PlaytestAccount::LlmProviders()[0];
			}
		}
		TArray<TPair<FString, FString>> Keys;
		Keys.Emplace(TEXT("TYPESAFE_API_KEY"), DecisionKey);
		Keys.Emplace(TEXT("AUTOPILOT_AGENT"), AgentId);
		if (bOpenCode)
		{
			Keys.Emplace(TEXT("AUTOPILOT_OPENCODE_PROVIDER"), ProviderId);  // (Claude Code's or Codex's: OpenCode's line stays as it was)
		}
		Keys.Emplace(LlmKeyName(ProviderId), LlmKey);
		return Keys;
	}

	FString KeyIn(const FString& Text, const FString& Name)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		FString Value;
		for (const FString& Line : Lines)
		{
			if (KeyOf(Line) != Name)
			{
				continue;
			}
			int32 Equals = INDEX_NONE;
			Line.FindChar(TEXT('='), Equals);
			Value = Line.Mid(Equals + 1).TrimStartAndEnd();
			if (Value.Len() >= 2 && (Value[0] == TEXT('"') || Value[0] == TEXT('\'')) && Value[Value.Len() - 1] == Value[0])
			{
				Value = Value.Mid(1, Value.Len() - 2);
			}
		}
		return Value;
	}

	FString ReadKey(const FString& Name, const FString& Dir)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FPaths::Combine(DirOr(Dir), KeysFile));
		return KeyIn(Text, Name);
	}
}

namespace PlaytestUsage
{
	TArray<FPlaytestModelUsage> Parse(const TSharedPtr<FJsonObject>& Message, bool* bOutFinal)
	{
		TArray<FPlaytestModelUsage> Out;
		if (bOutFinal)
		{
			*bOutFinal = false;
		}
		if (!Message.IsValid())
		{
			return Out;
		}
		if (bOutFinal)
		{
			Message->TryGetBoolField(TEXT("final"), *bOutFinal);
		}
		const TArray<TSharedPtr<FJsonValue>>* Models = nullptr;
		if (!Message->TryGetArrayField(TEXT("models"), Models) || !Models)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Models)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Item) || !Item || !Item->IsValid())
			{
				continue;
			}
			FPlaytestModelUsage Model;
			(*Item)->TryGetStringField(TEXT("kind"), Model.Kind);
			(*Item)->TryGetStringField(TEXT("model"), Model.Model);
			Model.Kind = Model.Kind.ToLower();
			Model.Calls = Count(*Item, TEXT("calls"));
			Model.Input = Count(*Item, TEXT("input"));
			Model.Output = Count(*Item, TEXT("output"));
			Model.CacheRead = Count(*Item, TEXT("cache_read"));
			Model.CacheWrite = Count(*Item, TEXT("cache_write"));
			Out.Add(Model);
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ToJson(const TArray<FPlaytestModelUsage>& Models)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FPlaytestModelUsage& Model : Models)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("kind"), Model.Kind);
			Item->SetStringField(TEXT("model"), Model.Model);
			Item->SetNumberField(TEXT("calls"), (double)Model.Calls);
			Item->SetNumberField(TEXT("input"), (double)Model.Input);
			Item->SetNumberField(TEXT("output"), (double)Model.Output);
			Item->SetNumberField(TEXT("cache_read"), (double)Model.CacheRead);
			Item->SetNumberField(TEXT("cache_write"), (double)Model.CacheWrite);
			Out.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Out;
	}

	void Add(TArray<FPlaytestModelUsage>& Total, const TArray<FPlaytestModelUsage>& Run)
	{
		for (const FPlaytestModelUsage& Model : Run)
		{
			FPlaytestModelUsage* Same = Total.FindByPredicate([&Model](const FPlaytestModelUsage& M)
			{
				return M.Kind == Model.Kind && M.Model == Model.Model;
			});
			if (!Same)
			{
				FPlaytestModelUsage& Added = Total.AddDefaulted_GetRef();
				Added.Kind = Model.Kind;
				Added.Model = Model.Model;
				Same = &Added;
			}
			Same->Calls += Model.Calls;
			Same->Input += Model.Input;
			Same->Output += Model.Output;
			Same->CacheRead += Model.CacheRead;
			Same->CacheWrite += Model.CacheWrite;
		}
		// LLMs first, then decision models, each in the order they first came
		Total.StableSort([](const FPlaytestModelUsage& A, const FPlaytestModelUsage& B)
		{
			return A.IsLlm() && !B.IsLlm();
		});
	}

	FString Label(const FPlaytestModelUsage& Model)
	{
		return Model.Model;
	}

	int64 Tokens(const TArray<FPlaytestModelUsage>& Models, const FString& Kind)
	{
		int64 Sum = 0;
		for (const FPlaytestModelUsage& Model : Models)
		{
			if (Model.Kind == Kind)
			{
				Sum += Model.Input + Model.Output;
			}
		}
		return Sum;
	}
}
