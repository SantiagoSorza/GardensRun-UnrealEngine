// The agent service's `account` and `aura_sign_in` frames (docs/protocol.md, "Account").

#include "PlaytestAccount.h"

namespace
{
	FString StringOf(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		FString Value;
		if (Json.IsValid())
		{
			Json->TryGetStringField(Field, Value);  // (null or missing: "")
		}
		return Value.TrimStartAndEnd();
	}

	bool IsType(const TSharedPtr<FJsonObject>& Message, const TCHAR* Type)
	{
		return Message.IsValid() && StringOf(Message, TEXT("type")) == Type;
	}

	/** A sum of dollars: a finite number, not below 0 (false otherwise). */
	bool UsdOf(const TSharedPtr<FJsonObject>& Json, double& Out)
	{
		double Usd = -1.0;
		if (!Json.IsValid() || !Json->TryGetNumberField(TEXT("usd"), Usd) || !FMath::IsFinite(Usd) || Usd < 0.0)
		{
			return false;
		}
		Out = Usd;
		return true;
	}

	/** A run's bill: from the `bill` frame, or as saved with the run. */
	FPlaytestRunBill ReadBill(const TSharedPtr<FJsonObject>& Json)
	{
		FPlaytestRunBill Bill;
		if (!Json.IsValid())
		{
			return Bill;
		}
		Bill.bKnown = true;
		Json->TryGetBoolField(TEXT("billed"), Bill.bBilled);
		Bill.Reason = StringOf(Json, TEXT("reason"));
		if (!UsdOf(Json, Bill.Usd))
		{
			Bill.Usd = 0.0;
			Bill.bBilled = false;  // (no sum to show)
		}
		return Bill;
	}

	/** "2026-10-01" as its year, month and day; false when it isn't one. */
	bool DayOf(const FString& Text, int32& Year, int32& Month, int32& Day)
	{
		TArray<FString> Parts;
		Text.Left(10).ParseIntoArray(Parts, TEXT("-"));
		if (Parts.Num() != 3 || !Parts[0].IsNumeric() || !Parts[1].IsNumeric() || !Parts[2].IsNumeric())
		{
			return false;
		}
		Year = FCString::Atoi(*Parts[0]);
		Month = FCString::Atoi(*Parts[1]);
		Day = FCString::Atoi(*Parts[2]);
		return Year > 0 && Month >= 1 && Month <= 12 && Day >= 1 && Day <= 31;
	}
}

namespace PlaytestAccount
{
	EPlaytestAccountKind UsageKind(const FPlaytestAccount& Account, const FString& Access, bool bUseWithAura)
	{
		if (bUseWithAura)
		{
			return EPlaytestAccountKind::AuraApp;
		}
		if (Account.bKnown && Account.Mode == TEXT("aura"))
		{
			return Account.bSignedIn ? EPlaytestAccountKind::Aura : EPlaytestAccountKind::SignedOut;
		}
		if (Account.bKnown && Account.Mode == TEXT("own_keys"))
		{
			return EPlaytestAccountKind::OwnKeys;
		}
		if (Access == TEXT("aura"))
		{
			return EPlaytestAccountKind::Aura;
		}
		return Access == TEXT("own_keys") ? EPlaytestAccountKind::OwnKeys : EPlaytestAccountKind::NotSetUp;
	}

	bool ParseAccount(const TSharedPtr<FJsonObject>& Message, FPlaytestAccount& Out)
	{
		if (!IsType(Message, TEXT("account")))
		{
			return false;
		}
		FPlaytestAccount Account;
		Account.bKnown = true;
		Account.Mode = StringOf(Message, TEXT("mode")).ToLower();
		if (Account.Mode != TEXT("aura") && Account.Mode != TEXT("own_keys"))
		{
			Account.Mode = TEXT("none");
		}
		Account.Email = StringOf(Message, TEXT("email"));
		Message->TryGetBoolField(TEXT("signed_in"), Account.bSignedIn);
		const TSharedPtr<FJsonObject>* Keys = nullptr;
		if (Message->TryGetObjectField(TEXT("keys"), Keys) && Keys)
		{
			Account.DecisionKey = StringOf(*Keys, TEXT("decision"));
			Account.LlmKey = StringOf(*Keys, TEXT("llm"));
		}
		// The Coding Agent BYOK runs use (none, or one this plugin doesn't know: OpenCode, the agent service's default)
		const FString Agent = StringOf(Message, TEXT("coding_agent")).ToLower();
		Account.bCodingAgentSent = !Agent.IsEmpty();
		Account.CodingAgent = CodingAgents().Contains(Agent) ? Agent : FString(TEXT("opencode"));
		// Whose LLM key that is (none: an older agent service, which only had Anthropic's)
		const FString Provider = StringOf(Message, TEXT("llm_provider")).ToLower();
		Account.bLlmProviderSent = !Provider.IsEmpty();
		Account.LlmProvider = Provider.IsEmpty() ? FString(TEXT("anthropic")) : Provider;
		// What Autopilot billed the Aura account this period (null: Aura couldn't say)
		const TSharedPtr<FJsonObject>* Bill = nullptr;
		if (Message->TryGetObjectField(TEXT("bill"), Bill) && Bill && UsdOf(*Bill, Account.Bill.Usd))
		{
			Account.Bill.bKnown = true;
			double Runs = 0.0;
			(*Bill)->TryGetNumberField(TEXT("runs"), Runs);
			Account.Bill.Runs = FMath::IsFinite(Runs) ? (int32)FMath::Clamp(Runs, 0.0, (double)MAX_int32) : 0;
			Account.Bill.From = StringOf(*Bill, TEXT("from"));
			Account.Bill.To = StringOf(*Bill, TEXT("to"));
		}
		else
		{
			Account.Bill = FPlaytestPeriodBill();
		}
		// Aura's words when the team is out of credits (null, or none from an older agent service: it has some)
		const TSharedPtr<FJsonObject>* Credits = nullptr;
		if (Message->TryGetObjectField(TEXT("out_of_credits"), Credits) && Credits)
		{
			Account.OutOfCredits = StringOf(*Credits, TEXT("message")).TrimStartAndEnd();
			// (a web page only: it is opened in the browser)
			const FString Url = StringOf(*Credits, TEXT("url")).TrimStartAndEnd();
			const bool bWeb = Url.StartsWith(TEXT("https://")) || Url.StartsWith(TEXT("http://"));
			Account.AddCreditsUrl = !Account.OutOfCredits.IsEmpty() && bWeb ? Url : FString();
		}
		Out = Account;
		return true;
	}

	const TArray<FString>& CodingAgents()
	{
		static const TArray<FString> Agents = { TEXT("claude"), TEXT("codex"), TEXT("opencode") };
		return Agents;
	}

	FString AgentName(const FString& Agent)
	{
		const FString Id = Agent.TrimStartAndEnd().ToLower();
		if (Id == TEXT("claude"))
		{
			return TEXT("Claude Code");
		}
		return Id == TEXT("codex") ? FString(TEXT("Codex")) : FString(TEXT("OpenCode"));
	}

	FString AgentProvider(const FString& Agent)
	{
		const FString Id = Agent.TrimStartAndEnd().ToLower();
		if (Id == TEXT("claude"))
		{
			return TEXT("anthropic");  // (Claude Code takes Anthropic's keys only)
		}
		return Id == TEXT("codex") ? FString(TEXT("openai")) : FString();  // (Codex takes OpenAI's only; OpenCode any)
	}

	const TArray<FString>& LlmProviders()
	{
		static const TArray<FString> Providers = { TEXT("anthropic"), TEXT("openai"), TEXT("opencode"), TEXT("openrouter"), TEXT("deepseek"), TEXT("google") };
		return Providers;
	}

	FString ProviderName(const FString& Provider)
	{
		static const TMap<FString, FString> Names = { { TEXT("anthropic"), TEXT("Anthropic") }, { TEXT("openai"), TEXT("OpenAI") },
			{ TEXT("opencode"), TEXT("OpenCode Zen") }, { TEXT("openrouter"), TEXT("OpenRouter") }, { TEXT("deepseek"), TEXT("DeepSeek") },
			{ TEXT("google"), TEXT("Gemini") } };
		const FString Id = Provider.TrimStartAndEnd().ToLower();
		if (Id.IsEmpty())
		{
			return TEXT("Anthropic");
		}
		const FString* Name = Names.Find(Id);
		return Name ? *Name : Provider.TrimStartAndEnd();
	}

	FString PickAgent(const FPlaytestAccount& Account, const FString& Saved)
	{
		const FString Id = (Account.bKnown && Account.bCodingAgentSent ? Account.CodingAgent : Saved).TrimStartAndEnd().ToLower();
		return CodingAgents().Contains(Id) ? Id : FString(TEXT("opencode"));
	}

	FString PickProvider(const FPlaytestAccount& Account, const FString& Saved)
	{
		// (the frame's provider is OpenCode's only when the frame's agent is: Claude Code's and Codex's are fixed)
		const bool bFrames = Account.bKnown && Account.bLlmProviderSent && (!Account.bCodingAgentSent || Account.CodingAgent == TEXT("opencode"));
		const FString Id = (bFrames ? Account.LlmProvider : Saved).TrimStartAndEnd().ToLower();
		return LlmProviders().Contains(Id) ? Id : LlmProviders()[0];
	}

	FString SavedKeysLine(const FPlaytestAccount& Account)
	{
		TArray<FString> Saved;
		if (!Account.DecisionKey.IsEmpty())
		{
			Saved.Add(TEXT("Decision provider ") + Account.DecisionKey);
		}
		if (!Account.LlmKey.IsEmpty())
		{
			Saved.Add(FString::Printf(TEXT("%s (%s) %s"), *AgentName(Account.CodingAgent), *ProviderName(Account.LlmProvider), *Account.LlmKey));
		}
		return Saved.Num() ? TEXT("Saved on this computer: ") + FString::Join(Saved, TEXT(" · ")) : FString();
	}

	bool ParseSignIn(const TSharedPtr<FJsonObject>& Message, FPlaytestSignIn& Out)
	{
		if (!IsType(Message, TEXT("aura_sign_in")))
		{
			return false;
		}
		FPlaytestSignIn SignIn;
		SignIn.State = StringOf(Message, TEXT("state")).ToLower();
		SignIn.Url = StringOf(Message, TEXT("url"));
		SignIn.Email = StringOf(Message, TEXT("email"));
		SignIn.Reason = StringOf(Message, TEXT("reason")).ToLower();
		SignIn.Message = StringOf(Message, TEXT("message"));
		if (SignIn.State == TEXT("waiting") && SignIn.Url.IsEmpty())
		{
			SignIn.State = TEXT("asked");  // (nothing to show or open yet)
		}
		Out = SignIn;
		return true;
	}

	EPlaytestSignInPhase Phase(const FPlaytestSignIn& SignIn)
	{
		if (SignIn.State == TEXT("asked"))
		{
			return EPlaytestSignInPhase::Asked;
		}
		if (SignIn.State == TEXT("waiting"))
		{
			return EPlaytestSignInPhase::Waiting;
		}
		if (SignIn.State == TEXT("signed_in"))
		{
			return EPlaytestSignInPhase::SignedIn;
		}
		if (SignIn.State == TEXT("error"))
		{
			return SignIn.Reason == TEXT("cancelled") ? EPlaytestSignInPhase::Idle : EPlaytestSignInPhase::Error;
		}
		return EPlaytestSignInPhase::Idle;
	}

	bool IsSignInGoing(const FPlaytestSignIn& SignIn)
	{
		return SignIn.State == TEXT("asked") || SignIn.State == TEXT("waiting");
	}

	bool SignInFinishedAway(const FPlaytestSignIn& SignIn, const FPlaytestAccount& Account, FPlaytestSignIn& Out)
	{
		// (signed_in alone can be the Aura app running: a kept session has its email)
		if (!IsSignInGoing(SignIn) || !Account.bSignedIn || Account.Email.IsEmpty())
		{
			return false;
		}
		Out = FPlaytestSignIn();
		Out.State = TEXT("signed_in");
		Out.Email = Account.Email;
		return true;
	}

	FString ErrorLine(const FPlaytestSignIn& SignIn)
	{
		if (SignIn.Reason == TEXT("denied"))
		{
			return TEXT("Sign-in was cancelled in the browser.");
		}
		if (SignIn.Reason == TEXT("timeout"))
		{
			return TEXT("Nothing came back from the browser in 5 minutes.");
		}
		if (SignIn.Reason == TEXT("unreachable"))
		{
			return TEXT("Couldn’t reach Aura. Check your connection and try again.");
		}
		return SignIn.Message.IsEmpty() ? FString(TEXT("The sign-in didn’t finish. Try again.")) : SignIn.Message;
	}

	bool ParseBill(const TSharedPtr<FJsonObject>& Message, FPlaytestRunBill& Out)
	{
		if (!IsType(Message, TEXT("bill")))
		{
			return false;
		}
		Out = ReadBill(Message);
		return true;
	}

	TSharedRef<FJsonObject> BillToJson(const FPlaytestRunBill& Bill)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("usd"), Bill.Usd);
		Json->SetBoolField(TEXT("billed"), Bill.bBilled);
		if (!Bill.Reason.IsEmpty())
		{
			Json->SetStringField(TEXT("reason"), Bill.Reason);
		}
		return Json;
	}

	FPlaytestRunBill BillFromJson(const TSharedPtr<FJsonObject>& Json)
	{
		return ReadBill(Json);
	}

	FString Money(double Usd)
	{
		if (!FMath::IsFinite(Usd) || Usd < 0.0)
		{
			Usd = 0.0;
		}
		if (Usd > 0.0 && Usd < 0.01)
		{
			return TEXT("< $0.01");
		}
		const int64 Cents = (int64)FMath::RoundToDouble(FMath::Min(Usd, 1e12) * 100.0);
		FString Dollars = FString::Printf(TEXT("%lld"), (long long)(Cents / 100));
		for (int32 At = Dollars.Len() - 3; At > 0; At -= 3)
		{
			Dollars.InsertAt(At, TEXT(','));
		}
		return FString::Printf(TEXT("$%s.%02d"), *Dollars, (int32)(Cents % 100));
	}

	FString BilledLine(double Usd)
	{
		return TEXT("Billed ") + Money(Usd);
	}

	FString PeriodLine(const FPlaytestPeriodBill& Bill)
	{
		if (!Bill.bKnown)
		{
			return FString();
		}
		const FString Dates = PeriodDates(Bill);
		return Dates.IsEmpty() ? FString::Printf(TEXT("Billing period: %s USD"), *Money(Bill.Usd))
			: FString::Printf(TEXT("Billing period (%s): %s USD"), *Dates, *Money(Bill.Usd));
	}

	FString PeriodDates(const FPlaytestPeriodBill& Bill)
	{
		static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
			TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
		if (!Bill.bKnown || (Bill.From.IsEmpty() && Bill.To.IsEmpty()))
		{
			return FString();
		}
		int32 Y1 = 0, M1 = 0, D1 = 0, Y2 = 0, M2 = 0, D2 = 0;
		const bool bFrom = DayOf(Bill.From, Y1, M1, D1);
		const bool bTo = DayOf(Bill.To, Y2, M2, D2);
		if (!bFrom || !bTo)
		{
			// (not dates: as sent)
			return Bill.From.IsEmpty() || Bill.To.IsEmpty() ? Bill.From + Bill.To : Bill.From + TEXT(" – ") + Bill.To;
		}
		if (Y1 != Y2)
		{
			return FString::Printf(TEXT("%s %d, %d – %s %d, %d"), Months[M1 - 1], D1, Y1, Months[M2 - 1], D2, Y2);
		}
		return FString::Printf(TEXT("%s %d – %s %d"), Months[M1 - 1], D1, Months[M2 - 1], D2);
	}
}
