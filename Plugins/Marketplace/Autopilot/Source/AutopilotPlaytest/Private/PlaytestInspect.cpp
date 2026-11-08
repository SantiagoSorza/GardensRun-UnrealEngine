#include "PlaytestInspect.h"

#include "Algo/AllOf.h"
#include "Misc/Compression.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersionComparison.h"

namespace
{
	constexpr int32 PackFrom = 1024;  // shorter texts aren't worth compressing
	constexpr int32 FlatLine = 100;   // a list or object of plain values this short goes on one line

	FString NumberText(double V)
	{
		if (!FMath::IsFinite(V))
		{
			return TEXT("null");
		}
		if (V == FMath::FloorToDouble(V) && FMath::Abs(V) < 1e15)
		{
			return FString::Printf(TEXT("%lld"), (long long)V);
		}
		// As short as reads back exactly (as the brain wrote it, for most numbers)
		FString S = FString::Printf(TEXT("%.6g"), V);
		if (FCString::Atod(*S) == V)
		{
			return S;
		}
		S = FString::Printf(TEXT("%.9g"), V);
		if (FCString::Atod(*S) == V)
		{
			return S;
		}
		S = FString::Printf(TEXT("%.12g"), V);
		if (FCString::Atod(*S) == V)
		{
			return S;
		}
		S = FString::Printf(TEXT("%.15g"), V);
		if (FCString::Atod(*S) == V)
		{
			return S;
		}
		return FString::Printf(TEXT("%.17g"), V);
	}

	void Quote(FString& Out, const FString& S)
	{
		Out.AppendChar(TEXT('"'));
		for (const TCHAR C : S)
		{
			switch (C)
			{
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), (int32)C);
				}
				else
				{
					Out.AppendChar(C);
				}
			}
		}
		Out.AppendChar(TEXT('"'));
	}

	bool IsPlain(const TSharedPtr<FJsonValue>& V)
	{
		return !V.IsValid() || (V->Type != EJson::Array && V->Type != EJson::Object);
	}

	void Indent(FString& Out, int32 Depth)
	{
		for (int32 i = 0; i < Depth; ++i)
		{
			Out += TEXT("  ");
		}
	}

	void WritePretty(FString& Out, const TSharedPtr<FJsonValue>& V, int32 Depth);

	void WritePlain(FString& Out, const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid())
		{
			Out += TEXT("null");
			return;
		}
		switch (V->Type)
		{
		case EJson::Boolean: Out += V->AsBool() ? TEXT("true") : TEXT("false"); break;
		case EJson::Number: Out += NumberText(V->AsNumber()); break;
		case EJson::String: Quote(Out, V->AsString()); break;
		default: Out += TEXT("null"); break;
		}
	}

	void WriteArray(FString& Out, const TArray<TSharedPtr<FJsonValue>>& Items, int32 Depth)
	{
		if (Items.Num() == 0)
		{
			Out += TEXT("[]");
			return;
		}
		if (Items.Num() <= 20 && Algo::AllOf(Items, &IsPlain))
		{
			FString Line(TEXT("["));
			for (int32 i = 0; i < Items.Num(); ++i)
			{
				if (i)
				{
					Line += TEXT(", ");
				}
				WritePlain(Line, Items[i]);
			}
			Line += TEXT("]");
			if (Line.Len() <= FlatLine)
			{
				Out += Line;
				return;
			}
		}
		Out += TEXT("[\n");
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			Indent(Out, Depth + 1);
			WritePretty(Out, Items[i], Depth + 1);
			if (i + 1 < Items.Num())
			{
				Out.AppendChar(TEXT(','));
			}
			Out.AppendChar(TEXT('\n'));
		}
		Indent(Out, Depth);
		Out.AppendChar(TEXT(']'));
	}

	void WriteObjectAt(FString& Out, const TSharedPtr<FJsonObject>& Object, int32 Depth)
	{
		if (!Object.IsValid() || Object->Values.Num() == 0)
		{
			Out += TEXT("{}");
			return;
		}
		bool bAllPlain = Object->Values.Num() <= 6;
		for (const auto& Pair : Object->Values)
		{
			bAllPlain = bAllPlain && IsPlain(Pair.Value);
		}
		if (bAllPlain)
		{
			FString Line(TEXT("{"));
			bool bFirst = true;
			for (const auto& Pair : Object->Values)
			{
				if (!bFirst)
				{
					Line += TEXT(", ");
				}
				bFirst = false;
				Quote(Line, FString(*Pair.Key));
				Line += TEXT(": ");
				WritePlain(Line, Pair.Value);
			}
			Line += TEXT("}");
			if (Line.Len() <= FlatLine)
			{
				Out += Line;
				return;
			}
		}
		Out += TEXT("{\n");
		int32 i = 0;
		for (const auto& Pair : Object->Values)
		{
			Indent(Out, Depth + 1);
			Quote(Out, FString(*Pair.Key));
			Out += TEXT(": ");
			WritePretty(Out, Pair.Value, Depth + 1);
			if (++i < Object->Values.Num())
			{
				Out.AppendChar(TEXT(','));
			}
			Out.AppendChar(TEXT('\n'));
		}
		Indent(Out, Depth);
		Out.AppendChar(TEXT('}'));
	}

	void WritePretty(FString& Out, const TSharedPtr<FJsonValue>& V, int32 Depth)
	{
		if (!V.IsValid())
		{
			Out += TEXT("null");
			return;
		}
		switch (V->Type)
		{
		case EJson::Array:
			WriteArray(Out, V->AsArray(), Depth);
			break;
		case EJson::Object:
			WriteObjectAt(Out, V->AsObject(), Depth);
			break;
		default:
			WritePlain(Out, V);
			break;
		}
	}

	TSharedPtr<FJsonObject> ChildObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Object.IsValid() && Object->TryGetObjectField(Name, Found) && Found && Found->IsValid())
		{
			return *Found;
		}
		return nullptr;
	}

	/** Names, the first Most of them, then how many more. */
	FString NamesText(const TArray<FString>& Names, int32 Most)
	{
		TArray<FString> Shown(Names.GetData(), FMath::Min(Names.Num(), Most));
		FString Text = FString::Join(Shown, TEXT(", "));
		if (Names.Num() > Most)
		{
			Text += FString::Printf(TEXT(", ... (%d more)"), Names.Num() - Most);
		}
		return Text;
	}

	TArray<FString> StringsField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Name, Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				FString S;
				if (Item.IsValid() && Item->TryGetString(S))
				{
					Out.Add(S);
				}
			}
		}
		return Out;
	}
}

void FPlaytestPackedText::Pack(const FString& Text)
{
	Data.Reset();
	bCompressed = false;
	FTCHARToUTF8 Utf8(*Text, Text.Len());
	RawSize = Utf8.Length();
	if (RawSize <= 0)
	{
		RawSize = 0;
		return;
	}
	if (RawSize >= PackFrom)
	{
		int32 Size = FCompression::CompressMemoryBound(NAME_Zlib, RawSize);
		Data.SetNumUninitialized(Size);
		if (FCompression::CompressMemory(NAME_Zlib, Data.GetData(), Size, Utf8.Get(), RawSize) && Size < RawSize)
		{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
			Data.SetNum(Size, true);
#else
			Data.SetNum(Size, EAllowShrinking::Yes);
#endif
			bCompressed = true;
			return;
		}
	}
	Data.SetNumUninitialized(RawSize);
	FMemory::Memcpy(Data.GetData(), Utf8.Get(), RawSize);
}

FString FPlaytestPackedText::Unpack() const
{
	if (RawSize <= 0)
	{
		return FString();
	}
	TArray<uint8> Raw;
	const uint8* Bytes = Data.GetData();
	if (bCompressed)
	{
		Raw.SetNumUninitialized(RawSize);
		if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize, Data.GetData(), Data.Num()))
		{
			return TEXT("(couldn't unpack this text)");
		}
		Bytes = Raw.GetData();
	}
	FUTF8ToTCHAR Wide(reinterpret_cast<const ANSICHAR*>(Bytes), RawSize);
	return FString(FStringView(Wide.Get(), Wide.Length()));
}

namespace PlaytestInspect
{
FString PeekType(const FString& Text)
{
	const TCHAR* P = *Text;
	const TCHAR* End = P + Text.Len();
	auto Skip = [&P, End]()
	{
		while (P < End && FChar::IsWhitespace(*P))
		{
			++P;
		}
	};
	Skip();
	if (P >= End || *P != TEXT('{'))
	{
		return FString();
	}
	++P;
	Skip();
	static const TCHAR Key[] = TEXT("\"type\"");
	constexpr int32 KeyLen = UE_ARRAY_COUNT(Key) - 1;
	if (End - P < KeyLen || FCString::Strncmp(P, Key, KeyLen) != 0)
	{
		return FString();
	}
	P += KeyLen;
	Skip();
	if (P >= End || *P != TEXT(':'))
	{
		return FString();
	}
	++P;
	Skip();
	if (P >= End || *P != TEXT('"'))
	{
		return FString();
	}
	const TCHAR* Start = ++P;
	while (P < End && *P != TEXT('"') && *P != TEXT('\\') && P - Start < 64)
	{
		++P;
	}
	if (P >= End || *P != TEXT('"'))
	{
		return FString();
	}
	return FString(FStringView(Start, (int32)(P - Start)));
}

bool IsInspectType(const FString& Type)
{
	return Type == TEXT("jev") || Type == TEXT("memory") || Type == TEXT("memories");
}

FString Pretty(const TSharedPtr<FJsonValue>& Value)
{
	FString Out;
	WritePretty(Out, Value, 0);
	return Out;
}

FString Pretty(const TSharedPtr<FJsonObject>& Object)
{
	FString Out;
	WriteObjectAt(Out, Object, 0);
	return Out;
}

TSharedPtr<FPlaytestJevCall> ParseJev(const TSharedPtr<FJsonObject>& Message, FQuestionMemo& Memo)
{
	if (!Message.IsValid())
	{
		return nullptr;
	}
	TSharedPtr<FPlaytestJevCall> Call = MakeShared<FPlaytestJevCall>();
	{
		// The frame as received, without the instructions each question carries (Copy step JSON copies this)
		TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
		Copy->Values = Message->Values;
		const TSharedPtr<FJsonObject>* InputObject = nullptr;
		const TSharedPtr<FJsonObject>* QuestionsObject = nullptr;
		if (Message->TryGetObjectField(TEXT("input"), InputObject) && InputObject && InputObject->IsValid()
			&& (*InputObject)->TryGetObjectField(TEXT("questions"), QuestionsObject) && QuestionsObject && QuestionsObject->IsValid())
		{
			TSharedRef<FJsonObject> InputCopy = MakeShared<FJsonObject>();
			InputCopy->Values = (*InputObject)->Values;
			TSharedRef<FJsonObject> QuestionsCopy = MakeShared<FJsonObject>();
			for (const auto& Pair : (*QuestionsObject)->Values)
			{
				const TSharedPtr<FJsonObject>* Question = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Question) && Question && Question->IsValid())
				{
					TSharedRef<FJsonObject> QuestionCopy = MakeShared<FJsonObject>();
					QuestionCopy->Values = (*Question)->Values;
					QuestionCopy->RemoveField(TEXT("instructions"));
					QuestionsCopy->SetObjectField(Pair.Key, QuestionCopy);
				}
				else
				{
					QuestionsCopy->SetField(Pair.Key, Pair.Value);
				}
			}
			InputCopy->SetObjectField(TEXT("questions"), QuestionsCopy);
			Copy->SetObjectField(TEXT("input"), InputCopy);
		}
		Call->Frame.Pack(Pretty(TSharedPtr<FJsonObject>(Copy)));
	}
	double Number = 0;
	if (Message->TryGetNumberField(TEXT("call"), Number))
	{
		Call->Call = (int64)Number;
	}
	if (Message->TryGetNumberField(TEXT("step"), Number))
	{
		Call->Step = (int32)Number;
	}
	if (Message->TryGetNumberField(TEXT("t"), Number))
	{
		Call->GameTime = Number;
	}
	if (Message->TryGetNumberField(TEXT("wall"), Number))
	{
		Call->WallTime = Number;
	}
	if (Message->TryGetNumberField(TEXT("dropped"), Number))
	{
		Call->Dropped = (int32)Number;
	}
	Message->TryGetBoolField(TEXT("truncated"), Call->bTruncated);
	Message->TryGetStringField(TEXT("error"), Call->Error);  // (a call that failed: no answers, no step)
	Message->TryGetStringField(TEXT("kind"), Call->Kind);
	Message->TryGetStringField(TEXT("goal"), Call->Goal);
	Message->TryGetStringField(TEXT("plan_step"), Call->PlanStep);
	const TArray<TSharedPtr<FJsonValue>>* PlanAt = nullptr;
	if (Message->TryGetArrayField(TEXT("plan_at"), PlanAt) && PlanAt && PlanAt->Num() == 2)
	{
		double At = 0;
		double Of = 0;
		if ((*PlanAt)[0].IsValid() && (*PlanAt)[0]->TryGetNumber(At) && (*PlanAt)[1].IsValid() && (*PlanAt)[1]->TryGetNumber(Of))
		{
			Call->PlanAt = (int32)At;
			Call->PlanSteps = (int32)Of;
		}
	}
	const TSharedPtr<FJsonObject> Seconds = ChildObject(Message, TEXT("seconds"));
	if (Seconds.IsValid())
	{
		Seconds->TryGetNumberField(TEXT("jev"), Call->JevSeconds);
		Seconds->TryGetNumberField(TEXT("decide"), Call->DecideSeconds);
		Seconds->TryGetNumberField(TEXT("perceive"), Call->PerceiveSeconds);
	}

	// The input as Jev got it: the questions named unchanged since the call before are that call's
	TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> Given = ChildObject(Message, TEXT("input"));
	const TSharedPtr<FJsonObject> Sent = ChildObject(Given, TEXT("questions"));
	if (Given.IsValid())
	{
		for (const auto& Pair : Given->Values)
		{
			const FString Name(*Pair.Key);
			if (Name != TEXT("questions"))
			{
				Input->SetField(Name, Pair.Value);
			}
		}
	}
	const TArray<FString> Same = StringsField(Message, TEXT("questions_same"));
	TArray<FString> Order = StringsField(Message, TEXT("questions_order"));
	if (Sent.IsValid())
	{
		for (const auto& Pair : Sent->Values)
		{
			Order.AddUnique(FString(*Pair.Key));
		}
	}
	for (const FString& Name : Same)
	{
		Order.AddUnique(Name);
	}
	TSharedRef<FJsonObject> Questions = MakeShared<FJsonObject>();
	TMap<FString, TSharedPtr<FJsonValue>> Now;
	for (const FString& Name : Order)
	{
		TSharedPtr<FJsonValue> Question = Sent.IsValid() ? Sent->TryGetField(Name) : nullptr;
		if (!Question.IsValid() && Same.Contains(Name))
		{
			const TSharedPtr<FJsonValue>* Before = Memo.Questions.Find(Name);
			Question = Before ? *Before : MakeShared<FJsonValueString>(TEXT("(unchanged since an earlier call this editor didn't receive)"));
		}
		if (Question.IsValid())
		{
			Questions->SetField(Name, Question);
			Now.Add(Name, Question);
		}
	}
	Memo.Questions = MoveTemp(Now);
	Memo.Order = Order;
	if (Given.IsValid() || Order.Num())
	{
		Input->SetObjectField(TEXT("questions"), Questions);
	}
	// What the input pane shows of a large input while following (see SPlaytestPolicyLog)
	TArray<FString> StateFields;
	if (const TSharedPtr<FJsonObject> State = ChildObject(Given, TEXT("state")))
	{
		for (const auto& Pair : State->Values)
		{
			StateFields.Add(FString(*Pair.Key));
		}
	}
	Call->InputSummary = FString::Printf(TEXT("State: %d fields (%s)\nQuestions (%d): %s"), StateFields.Num(),
		*NamesText(StateFields, 40), Order.Num(), *NamesText(Order, 40));

	const TSharedPtr<FJsonObject> Output = ChildObject(Message, TEXT("output"));
	const TSharedPtr<FJsonObject> Decided = ChildObject(Message, TEXT("decided"));
	if (Decided.IsValid() && !Decided->TryGetStringField(TEXT("summary"), Call->Summary))
	{
		Call->Summary = FString::Join(StringsField(Decided, TEXT("chosen")), TEXT(", "));
	}
	if (Call->Failed() && Call->Summary.IsEmpty())
	{
		Call->Summary = FString::Printf(TEXT("Jev call failed: %s"), *Call->Error);
	}
	// What the step chose, for the policy log's table
	if (const TSharedPtr<FJsonObject> Doing = ChildObject(ChildObject(Given, TEXT("state")), TEXT("now_doing")))
	{
		Doing->TryGetStringField(TEXT("option"), Call->NowDoing);
		Doing->TryGetNumberField(TEXT("for_seconds"), Call->NowDoingSeconds);
	}
	if (Decided.IsValid())
	{
		FString Verb;
		FString Named;
		FString Summary;
		if (Decided->TryGetStringField(TEXT("summary"), Summary) && Summary.Split(TEXT(": "), &Verb, &Named))
		{
			Call->Action = Verb.TrimStartAndEnd();
		}
		const TArray<FString> Chosen = StringsField(Decided, TEXT("chosen"));
		Call->Chosen = Chosen.Num() ? Chosen[0] : Named.TrimStartAndEnd();
		if (Call->Chosen == TEXT("None") || Call->Chosen == TEXT("null"))
		{
			Call->Chosen.Reset();
		}
	}
	if (const TSharedPtr<FJsonObject> Pick = ChildObject(ChildObject(Output, TEXT("answers")), TEXT("pick")))
	{
		for (const auto& Pair : Pick->Values)
		{
			double P = 0;
			if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(P))
			{
				Call->Pick.Add({ FString(*Pair.Key), (float)P });
			}
		}
		Call->Pick.Sort([](const FPlaytestPick& A, const FPlaytestPick& B) { return A.P > B.P; });
	}
	const TSharedPtr<FJsonObject> InputObject = Input;
	Call->Input.Pack(Pretty(InputObject));
	Call->Output.Pack(Output.IsValid() ? Pretty(Output) : FString(TEXT("{}")));
	// What the step decided, and every timing the step took (the act before it, how old its look was, ...)
	const TSharedPtr<FJsonObject> Shown = MakeShared<FJsonObject>();
	if (Decided.IsValid())
	{
		for (const auto& Pair : Decided->Values)
		{
			Shown->SetField(FString(*Pair.Key), Pair.Value);
		}
	}
	if (Seconds.IsValid())
	{
		Shown->SetObjectField(TEXT("seconds"), Seconds);
	}
	Call->Decided.Pack(Pretty(Shown));
	return Call;
}

TSharedPtr<FPlaytestMemoryItem> ParseMemory(const TSharedPtr<FJsonObject>& Entry)
{
	if (!Entry.IsValid())
	{
		return nullptr;
	}
	TSharedPtr<FPlaytestMemoryItem> Item = MakeShared<FPlaytestMemoryItem>();
	double Number = 0;
	if (Entry->TryGetNumberField(TEXT("t"), Number))
	{
		Item->GameTime = Number;
	}
	if (Entry->TryGetNumberField(TEXT("wall"), Number))
	{
		Item->WallTime = Number;
	}
	Entry->TryGetStringField(TEXT("change"), Item->Change);
	Entry->TryGetStringField(TEXT("kind"), Item->Kind);
	Entry->TryGetStringField(TEXT("title"), Item->Title);
	Entry->TryGetStringField(TEXT("key"), Item->Key);
	Entry->TryGetStringField(TEXT("file"), Item->File);
	Entry->TryGetStringField(TEXT("source"), Item->Source);
	Entry->TryGetBoolField(TEXT("truncated"), Item->bTruncated);
	if (Item->Change.IsEmpty())
	{
		Item->Change = TEXT("kept");
	}
	const TSharedPtr<FJsonValue> Content = Entry->TryGetField(TEXT("content"));
	if (!Content.IsValid() || Content->IsNull())
	{
		Item->Content.Pack(Item->Change == TEXT("forgotten") ? TEXT("(no longer in memory)") : TEXT("(not sent: too large)"));
	}
	else if (Content->Type == EJson::String)
	{
		Item->Content.Pack(Content->AsString());
	}
	else
	{
		Item->Content.Pack(Pretty(Content));
	}
	return Item;
}

void AddToStats(FPlaytestJevStats& Stats, const TArray<TSharedPtr<FPlaytestJevCall>>& Calls)
{
	if (Calls.Num() == 0 || !Calls.Last().IsValid())
	{
		return;
	}
	const FPlaytestJevCall& Call = *Calls.Last();
	++Stats.Count;
	Stats.Dropped += Call.Dropped;
	if (Call.Failed())
	{
		++Stats.Failed;  // (how long a failure took, a timeout, says nothing of how long Jev takes to answer)
		return;
	}
	const double Seconds = Call.Seconds();
	if (Seconds >= 0)
	{
		++Stats.Timed;
		Stats.Sum += Seconds;
		Stats.Last = Seconds;
	}
	if (Call.DecideSeconds >= 0)
	{
		++Stats.DecideTimed;
		Stats.DecideSum += Call.DecideSeconds;
	}
	double Sum = 0;
	double Max = 0;
	int32 N = 0;
	for (int32 i = Calls.Num() - 1; i >= 0 && N < FPlaytestJevStats::RecentWindow; --i)
	{
		const double X = Calls[i].IsValid() && !Calls[i]->Failed() ? Calls[i]->Seconds() : -1.0;
		if (X >= 0)
		{
			Sum += X;
			Max = FMath::Max(Max, X);
			++N;
		}
	}
	Stats.RecentAverage = N ? Sum / N : 0.0;
	Stats.RecentMax = Max;
}

FString ClockText(double UnixSeconds)
{
	if (UnixSeconds <= 0)
	{
		return TEXT("--:--:--");
	}
	const double Whole = FMath::FloorToDouble(UnixSeconds);
	const FDateTime Utc = FDateTime::FromUnixTimestamp((int64)Whole) + FTimespan::FromMilliseconds((UnixSeconds - Whole) * 1000.0);
	const FDateTime Local = Utc + (FDateTime::Now() - FDateTime::UtcNow());
	return FString::Printf(TEXT("%02d:%02d:%02d.%03d"), Local.GetHour(), Local.GetMinute(), Local.GetSecond(), Local.GetMillisecond());
}
}
