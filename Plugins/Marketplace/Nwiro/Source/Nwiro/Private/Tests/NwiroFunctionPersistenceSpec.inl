// Copyright 2026 Nwiro. All Rights Reserved.
// Included after NwiroFunctionAcceptanceSpec.inl in its existing automation guard.
// Explicit writer/reader processes only. Fresh GUID fixture paths; no deletion or historical assets.
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "UObject/SavePackage.h"
#include "UObject/LinkerLoad.h"
#include "Curves/CurveFloat.h"

namespace NwiroFunctionPersistence
{
	using namespace NwiroFunctionEvolutionProof;
	static const TCHAR* Names[] = { TEXT("Enum"), TEXT("Curve"), TEXT("Provider"), TEXT("Caller"), TEXT("DefaultCaller"), TEXT("IndirectChild") };
	static constexpr const TCHAR* Product = TEXT("Nwiro");
	struct FContext
	{
		FString Run, Root, Evidence;
		bool bValid = false;
		explicit FContext(FAutomationTestBase& Test)
		{
			FParse::Value(FCommandLine::Get(), TEXT("NwiroFunctionRun="), Run);
			bValid = Run.Len() == 32;
			for (TCHAR C : Run) bValid &= FChar::IsHexDigit(C);
			if (!Test.TestTrue(TEXT("explicit 32-hex NwiroFunctionRun required"), bValid)) return;
			Root = TEXT("/Game/NwiroFunctionPersistence/") + Run + TEXT("/") + Product + TEXT("/");
			Evidence = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NwiroFunctionPersistence") / Run / Product);
		}
		FString Object(int32 I) const { return Root + Names[I] + TEXT(".") + Names[I]; }
		FString File(int32 I) const { return FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Root + Names[I], FPackageName::GetAssetPackageExtension())); }
		FString Receipt(const TCHAR* Name) const { return Evidence / Name; }
	};

	static FString Hash(const FString& File)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.IsEmpty()) return FString();
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
		return BytesToHex(Digest, FSHA1::DigestSize);
	}

	static bool CompleteDiscovery(FAutomationTestBase& Test)
	{
		// Fixture setup may search; the production preflight is forbidden from doing so.
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SearchAllAssets(true);
		Registry.WaitForCompletion();
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		// Fixture setup only. Initial completion never authorizes production redefinition on this engine.
		return Test.TestTrue(TEXT("fixture initial all-assets discovery completed"), Registry.IsSearchAllAssets() && !Registry.IsLoadingAssets());
#else
		return Test.TestTrue(TEXT("fixture all-assets discovery completed"), Registry.IsSearchAllAssets() && !Registry.IsGathering());
#endif
	}

	static TArray<TSharedPtr<FJsonValue>> Defaults(UUserDefinedEnum* Enum, UCurveFloat* Curve)
	{
		return {
			DefaultPin(TEXT("Amount"), TEXT("int"), TEXT("42")),
			DefaultPin(TEXT("Enabled"), TEXT("bool"), TEXT("true")),
			DefaultPin(TEXT("Ratio"), TEXT("float"), TEXT("1.25")),
			DefaultPin(TEXT("Precise"), TEXT("double"), TEXT("2.5")),
			DefaultPin(TEXT("Large"), TEXT("int64"), TEXT("9223372036854775807")),
			DefaultPin(TEXT("Small"), TEXT("byte"), TEXT("255")),
			DefaultPin(TEXT("Label"), TEXT("name"), TEXT("Seed")),
			DefaultPin(TEXT("Message"), TEXT("string"), TEXT("hello\nworld")),
			DefaultPin(TEXT("Title"), TEXT("text"), TEXT("plain title")),
			DefaultPin(TEXT("Offset"), TEXT("vector"), TEXT("1,2,3")),
			DefaultPin(TEXT("ActorType"), TEXT("class"), TEXT("/Script/Engine.Actor"), TEXT("Actor")),
			DefaultPin(TEXT("Kind"), TEXT("enum"), Enum->GetNameStringByIndex(0), Enum->GetPathName()),
			DefaultPin(TEXT("Curve"), TEXT("object"), Curve->GetPathName(), TEXT("/Script/Engine.CurveFloat"))
		};
	}

	static TSharedPtr<FJsonValue> Update(const TCHAR* Keep = TEXT("17"))
	{
		auto Item = Function(TEXT("Mutable Function"));
		Item->AsObject()->SetBoolField(TEXT("redefine"), true);
		Item->AsObject()->SetArrayField(TEXT("inputs"), {
			DefaultPin(TEXT("Keep"), TEXT("int"), Keep), Pin(TEXT("Linked"), TEXT("int")),
			Pin(TEXT("Change"), TEXT("double")), Pin(TEXT("Record"), TEXT("vector")) });
		Item->AsObject()->SetArrayField(TEXT("outputs"), {
			Pin(TEXT("Result"), TEXT("int")), Pin(TEXT("Records"), TEXT("vector"), FString(), TEXT("array")) });
		return Item;
	}

	static UK2Node_CallFunction* CallerNode(UBlueprint* BP)
	{
		UEdGraph* G = Graph(BP, TEXT("Call Provider"));
		UK2Node_CallFunction* Result = nullptr;
		if (G) for (UEdGraphNode* N : G->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N))
		{ if (Result) return nullptr; Result = C; }
		return Result;
	}

	static bool ExactLink(FAutomationTestBase& Test, UEdGraphNode* A, const TCHAR* APin, UEdGraphNode* B, const TCHAR* BPin)
	{
		UEdGraphPin* AP = A ? A->FindPin(APin) : nullptr;
		UEdGraphPin* BP = B ? B->FindPin(BPin) : nullptr;
		return Test.TestTrue(FString(TEXT("exact reciprocal link ")) + APin + TEXT(" -> ") + BPin,
			AP && BP && AP->LinkedTo.Num() == 1 && BP->LinkedTo.Num() == 1 && AP->LinkedTo[0] == BP && BP->LinkedTo[0] == AP);
	}

	static bool Verify(FAutomationTestBase& Test, const TArray<TStrongObjectPtr<UObject>>& Assets, bool bRequireSkeleton = true)
	{
		auto* Enum = Cast<UUserDefinedEnum>(Assets[0].Get());
		auto* Curve = Cast<UCurveFloat>(Assets[1].Get());
		auto* Provider = Cast<UBlueprint>(Assets[2].Get());
		auto* Caller = Cast<UBlueprint>(Assets[3].Get());
		auto* DefaultCaller = Cast<UBlueprint>(Assets[4].Get());
		auto* Child = Assets.Num() == 6 ? Cast<UBlueprint>(Assets[5].Get()) : nullptr;
		if (!Test.TestTrue(TEXT("exact six persisted asset types"), Enum && Curve && Provider && Caller && DefaultCaller && Child)) return false;
		// GeneratedClass is serialized; SkeletonGeneratedClass is transient and
		// belongs to the post-compile oracle, not the first cold disk inspection.
		TArray<UClass*> ProviderClasses{Provider->GeneratedClass.Get()};
		if (bRequireSkeleton) ProviderClasses.Add(Provider->SkeletonGeneratedClass.Get());
		Test.TestTrue(TEXT("indirect child exact saved caller parent"), Child->ParentClass == Caller->GeneratedClass.Get());
		auto* E = Entry(Provider, TEXT("Mutable Function"));
		auto* R = Exit(Provider, TEXT("Mutable Function"));
		auto* C = CallerNode(Caller);
		auto* D = CallerNode(DefaultCaller);
		if (!Test.TestTrue(TEXT("provider and both actual callers exist"), E && R && C && D)) return false;
		auto* DE = Entry(Provider, TEXT("Defaults"));
		if (!Test.TestNotNull(TEXT("persisted defaults entry"), DE)) return false;
		const auto Expected = Defaults(Enum, Curve);
		Test.TestEqual(TEXT("complete defaults declaration count"), DE->UserDefinedPins.Num(), Expected.Num());
		for (int32 I = 0; I < Expected.Num(); ++I)
		{
			auto Definition = Expected[I]->AsObject();
			const FString Name = Definition->GetStringField(TEXT("name"));
			const FString Value = Definition->GetStringField(TEXT("default"));
			Test.TestTrue(TEXT("exact default parameter order"), DE->UserDefinedPins.IsValidIndex(I)
				&& DE->UserDefinedPins[I].IsValid() && DE->UserDefinedPins[I]->PinName == FName(*Name));
			for (UClass* Class : ProviderClasses)
			{
				UFunction* F = Class ? Class->FindFunctionByName(TEXT("Defaults"), EIncludeSuperFlag::ExcludeSuper) : nullptr;
				FProperty* P = F ? FindFProperty<FProperty>(F, FName(*Name)) : nullptr;
				bool Typed = P && P->HasAnyPropertyFlags(CPF_Parm) && !P->HasAnyPropertyFlags(CPF_OutParm);
				if (Name == TEXT("Amount")) Typed &= CastField<FIntProperty>(P) != nullptr;
				else if (Name == TEXT("Enabled")) Typed &= CastField<FBoolProperty>(P) != nullptr;
				else if (Name == TEXT("Ratio")) Typed &= CastField<FFloatProperty>(P) != nullptr;
				else if (Name == TEXT("Precise")) Typed &= CastField<FDoubleProperty>(P) != nullptr;
				else if (Name == TEXT("Large")) Typed &= CastField<FInt64Property>(P) != nullptr;
				else if (Name == TEXT("Small")) { auto* B = CastField<FByteProperty>(P); Typed &= B && !B->Enum; }
				else if (Name == TEXT("Label")) Typed &= CastField<FNameProperty>(P) != nullptr;
				else if (Name == TEXT("Message")) Typed &= CastField<FStrProperty>(P) != nullptr;
				else if (Name == TEXT("Title")) Typed &= CastField<FTextProperty>(P) != nullptr;
				else if (Name == TEXT("Offset")) { auto* S = CastField<FStructProperty>(P); Typed &= S && S->Struct == TBaseStructure<FVector>::Get(); }
				else if (Name == TEXT("ActorType")) { auto* C = CastField<FClassProperty>(P); Typed &= C && C->MetaClass == AActor::StaticClass(); }
				else if (Name == TEXT("Curve")) { auto* O = CastField<FObjectPropertyBase>(P); Typed &= O && !CastField<FClassProperty>(P) && O->PropertyClass == UCurveFloat::StaticClass(); }
				else if (Name == TEXT("Kind")) { auto* B = CastField<FByteProperty>(P); auto* E = CastField<FEnumProperty>(P); Typed &= (B && B->Enum == Enum) || (E && E->GetEnum() == Enum); }
				else Typed = false;
				Test.TestTrue(TEXT("exact reflected default parameter shape: ") + Name, Typed);
				if (Name == TEXT("ActorType") || Name == TEXT("Curve"))
				{
					const FString Key = TEXT("CPP_Default_") + Name;
					Test.TestTrue(TEXT("exact serialized reference-default metadata: ") + Name,
						F && F->HasMetaData(*Key) && F->GetMetaData(*Key) == Value);
				}
			}
			for (UEdGraphNode* N : { static_cast<UEdGraphNode*>(DE), static_cast<UEdGraphNode*>(D) })
			{
				auto* P = N->FindPin(*Name);
				if (!Test.TestNotNull(TEXT("expected default editor/caller pin"), P)) return false;
				if (Name == TEXT("Offset"))
				{
					FVector Actual;
					Test.TestTrue(TEXT("exact vector default components"), FDefaultValueHelper::ParseVector(PinValue(P), Actual) && Actual == FVector(1,2,3));
				}
				else if (Name == TEXT("ActorType")) Test.TestTrue(TEXT("exact native class default identity"), P->DefaultObject == AActor::StaticClass());
				else if (Name == TEXT("Curve")) Test.TestTrue(TEXT("exact persisted object default identity"), P->DefaultObject == Curve);
				else Test.TestEqual(TEXT("exact scalar/enum/text default"), PinValue(P), Value);
			}
		}
		const TArray<FName> ExpectedNames{ TEXT("Keep"), TEXT("Linked"), TEXT("Change"), TEXT("Record"), TEXT("Result"), TEXT("Records") };
		for (UClass* Class : ProviderClasses)
		{
			if (!Test.TestNotNull(TEXT("provider reflected class"), Class)) return false;
			UFunction* F = Class->FindFunctionByName(TEXT("Mutable Function"), EIncludeSuperFlag::ExcludeSuper);
			if (!Test.TestTrue(TEXT("exact persisted function owner"), F && F->GetOuter() == Class)) return false;
			TArray<FName> Actual;
			for (TFieldIterator<FProperty> It(F); It; ++It) if (It->HasAnyPropertyFlags(CPF_Parm)) Actual.Add(It->GetFName());
			Test.TestTrue(TEXT("exact reflected persisted order; no extra fields"), Actual == ExpectedNames);
			Test.TestTrue(TEXT("Keep exact scalar int input"), ReflectedPin(F, TEXT("Keep"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::None, false));
			Test.TestTrue(TEXT("Linked exact scalar int input"), ReflectedPin(F, TEXT("Linked"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::None, false));
			Test.TestTrue(TEXT("Result exact scalar int output"), ReflectedPin(F, TEXT("Result"), UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::None, true));
			Test.TestNotNull(TEXT("Change persisted as double"), FindFProperty<FDoubleProperty>(F, TEXT("Change")));
			Test.TestTrue(TEXT("new vector input exact native struct"), ReflectedPin(F, TEXT("Record"), UEdGraphSchema_K2::PC_Struct, TBaseStructure<FVector>::Get(), EPinContainerType::None, false));
			Test.TestTrue(TEXT("new vector array output exact native struct"), ReflectedPin(F, TEXT("Records"), UEdGraphSchema_K2::PC_Struct, TBaseStructure<FVector>::Get(), EPinContainerType::Array, true));
		}
		for (UEdGraphNode* N : { static_cast<UEdGraphNode*>(E), static_cast<UEdGraphNode*>(C) })
			Test.TestEqual(TEXT("declared final default persists as17"), PinValue(N->FindPin(TEXT("Keep"))), FString(TEXT("17")));
		Test.TestTrue(TEXT("external caller exact provider class and function"), !C->FunctionReference.IsSelfContext()
			&& C->FunctionReference.GetMemberParentClass() == Provider->GeneratedClass.Get()
			&& C->FunctionReference.GetMemberName() == TEXT("Mutable Function"));
		Test.TestTrue(TEXT("defaults caller exact provider/function"), D->FunctionReference.GetMemberParentClass() == Provider->GeneratedClass.Get()
			&& D->FunctionReference.GetMemberName() == TEXT("Defaults"));
		ExactLink(Test, E, TEXT("Linked"), R, TEXT("Result"));
		ExactLink(Test, E, TEXT("then"), R, TEXT("execute"));
		auto* CE = Entry(Caller, TEXT("Call Provider"));
		auto* CR = Exit(Caller, TEXT("Call Provider"));
		ExactLink(Test, CE, TEXT("Provider"), C, TEXT("self"));
		ExactLink(Test, CE, TEXT("Linked"), C, TEXT("Linked"));
		ExactLink(Test, CE, TEXT("then"), C, TEXT("execute"));
		ExactLink(Test, C, TEXT("then"), CR, TEXT("execute"));
		ExactLink(Test, C, TEXT("Result"), CR, TEXT("Result"));
		for (auto* N : { static_cast<UEdGraphNode*>(E), static_cast<UEdGraphNode*>(R), static_cast<UEdGraphNode*>(C) })
			for (const auto* P : N->Pins) Test.TestFalse(TEXT("no orphan pin after redefine/persistence"), P->bOrphanedPin);
		return !Test.HasAnyErrors();
	}

	static TArray<TSharedPtr<FJsonValue>> Identities(const TArray<TStrongObjectPtr<UObject>>& Assets)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (int32 I : { 2, 3, 4, 5 })
		{
			auto* BP = CastChecked<UBlueprint>(Assets[I].Get());
			for (UEdGraph* G : BP->FunctionGraphs)
			{
				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("asset"), Names[I]); Row->SetStringField(TEXT("graph"), G->GetName());
				Row->SetStringField(TEXT("graphGuid"), G->GraphGuid.ToString(EGuidFormats::Digits));
				TArray<TSharedPtr<FJsonValue>> Nodes;
				for (UEdGraphNode* N : G->Nodes)
				{
					auto Node = MakeShared<FJsonObject>();
					Node->SetStringField(TEXT("class"), N->GetClass()->GetName());
					Node->SetStringField(TEXT("guid"), N->NodeGuid.ToString(EGuidFormats::Digits));
					Nodes.Add(MakeShared<FJsonValueObject>(Node));
				}
				Row->SetArrayField(TEXT("nodes"), Nodes); Result.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		return Result;
	}

	static bool SaveFixture(FAutomationTestBase& Test, const FContext& Context, const TArray<TStrongObjectPtr<UObject>>& Assets)
	{
		for (int32 I = 0; I < Assets.Num(); ++I)
		{
			FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Context.File(I)), true);
			if (!Test.TestTrue(TEXT("explicit owned fixture package save"), UPackage::SavePackage(Assets[I]->GetOutermost(), Assets[I].Get(), *Context.File(I), Args)
				&& IFileManager::Get().FileSize(*Context.File(I)) > 0)) return false;
		}
		return true;
	}

	static bool Writer(FAutomationTestBase& Test)
	{
		FContext Context(Test);
		if (!Context.bValid || !CompleteDiscovery(Test)) return false;
		for (int32 I = 0; I < UE_ARRAY_COUNT(Names); ++I)
			if (!Test.TestTrue(TEXT("writer requires absent owned file/package"), !FindPackage(nullptr, *(Context.Root + Names[I])) && !IFileManager::Get().FileExists(*Context.File(I)))) return false;
		if (!Test.TestFalse(TEXT("writer receipt must not preexist"), IFileManager::Get().FileExists(*Context.Receipt(TEXT("writer.json"))))) return false;
		FNeverSaveOnCompile NeverSave;
		TArray<TStrongObjectPtr<UObject>> Assets;
		auto* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(CreatePackage(*(Context.Root + Names[0])), FName(Names[0]), RF_Public | RF_Standalone));
		if (!Test.TestNotNull(TEXT("owned enum"), Enum)) return false;
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
		Assets.Emplace(Enum);
		auto* Curve = NewObject<UCurveFloat>(CreatePackage(*(Context.Root + Names[1])), FName(Names[1]), RF_Public | RF_Standalone);
		Assets.Emplace(Curve);
		for (int32 I : { 2, 3, 4 })
		{
			UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*(Context.Root + Names[I])), FName(Names[I]),
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("NwiroFunctionPersistence"));
			if (!Test.TestNotNull(TEXT("owned Blueprint"), BP)) return false;
			BP->BlueprintDescription = TEXT("NwiroFunctionPersistence:") + Context.Run + TEXT(":") + Product;
			Assets.Emplace(BP);
		}
		auto* Provider = CastChecked<UBlueprint>(Assets[2].Get());
		auto* Caller = CastChecked<UBlueprint>(Assets[3].Get());
		auto* DefaultCaller = CastChecked<UBlueprint>(Assets[4].Get());
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		// Keep the same strong cold-read oracle, reached through supported creation rather than redefinition.
		auto Initial = Update();
		Initial->AsObject()->RemoveField(TEXT("redefine"));
#else
		auto Initial = Function(TEXT("Mutable Function"));
		Initial->AsObject()->SetArrayField(TEXT("inputs"), { Pin(TEXT("Linked"), TEXT("int")), DefaultPin(TEXT("Keep"), TEXT("int"), TEXT("5")), Pin(TEXT("Change"), TEXT("int")), Pin(TEXT("Remove"), TEXT("bool")) });
		Initial->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")), Pin(TEXT("Discard"), TEXT("float")) });
#endif
		auto DefaultFunction = Function(TEXT("Defaults"));
		DefaultFunction->AsObject()->SetArrayField(TEXT("inputs"), Defaults(Enum, Curve));
		DefaultFunction->AsObject()->SetArrayField(TEXT("outputs"), { Pin(TEXT("Result"), TEXT("int")) });
		if (!Test.TestTrue(TEXT("public initial signatures"), Succeeded(Call(Provider, { Initial, DefaultFunction }, false)))
			|| !MakeBody(Test, Provider, TEXT("Mutable Function"), TEXT("Linked"))
			|| !MakeBody(Test, Provider, TEXT("Defaults"), TEXT("Amount"))
			|| !MakeCaller(Test, Caller, Provider, TEXT("Mutable Function"), true)
			|| !MakeCaller(Test, DefaultCaller, Provider, TEXT("Defaults"), false)) return false;
		UBlueprint* Child = FKismetEditorUtilities::CreateBlueprint(Caller->GeneratedClass.Get(), CreatePackage(*(Context.Root + Names[5])), FName(Names[5]),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("NwiroFunctionPersistence"));
		if (!Test.TestNotNull(TEXT("owned indirect child"), Child) || !Compile(Test, Child, TEXT("indirect child initial compile"))) return false;
		Child->BlueprintDescription = TEXT("NwiroFunctionPersistence:") + Context.Run + TEXT(":") + Product;
		Assets.Emplace(Child);
		const auto BeforeNodes = MakeShared<FJsonObject>(); BeforeNodes->SetArrayField(TEXT("identities"), Identities(Assets));
		if (!SaveFixture(Test, Context, Assets)) return false;

#if UE_VERSION_OLDER_THAN(5, 6, 0)
		{
			// The final signature was created directly; a saved-provider mutation must still fail closed.
			const NwiroVariableProof::FSnapshot ProviderBefore(Provider), CallerBefore(Caller), DefaultsBefore(DefaultCaller), ChildBefore(Child);
			const FString ProviderGraphBefore = ReadGraph(Provider, TEXT("Mutable Function"));
			const FString CallerGraphBefore = ReadGraph(Caller, TEXT("Call Provider"));
			TArray<FString> Hashes;
			for (int32 I = 0; I < Assets.Num(); ++I) Hashes.Add(Hash(Context.File(I)));
			FObservedCall ProviderObserve(Provider), CallerObserve(Caller), DefaultObserve(DefaultCaller), ChildObserve(Child);
			if (!ExpectUnsupportedRedefinition(Test, Call(Provider, { Update(TEXT("18")) }, true, true), TEXT("saved UE 5.5 provider"))) return false;
			auto NoOp = Function(TEXT("Mutable Function")); NoOp->AsObject()->SetBoolField(TEXT("redefine"), true);
			if (!Test.TestTrue(TEXT("saved UE 5.5 provider accepts identical redefine"), Succeeded(Call(Provider, { NoOp }, true, true)))) return false;
			Test.TestTrue(TEXT("saved-provider rejection/no-op preserves all Blueprint states"), ProviderBefore.Matches(Provider)
				&& CallerBefore.Matches(Caller) && DefaultsBefore.Matches(DefaultCaller) && ChildBefore.Matches(Child));
			Test.TestEqual(TEXT("saved provider pins/defaults/links unchanged"), ReadGraph(Provider, TEXT("Mutable Function")), ProviderGraphBefore);
			Test.TestEqual(TEXT("saved caller pins/defaults/links unchanged"), ReadGraph(Caller, TEXT("Call Provider")), CallerGraphBefore);
			Test.TestEqual(TEXT("legacy saved-provider calls compile nothing"), ProviderObserve.Compiles + CallerObserve.Compiles + DefaultObserve.Compiles + ChildObserve.Compiles, 0);
			Test.TestEqual(TEXT("legacy saved-provider calls save nothing"), ProviderObserve.Saves + CallerObserve.Saves + DefaultObserve.Saves + ChildObserve.Saves, 0);
			for (int32 I = 0; I < Assets.Num(); ++I)
				Test.TestEqual(TEXT("legacy saved-provider calls preserve every fixture file"), Hash(Context.File(I)), Hashes[I]);
			if (!Verify(Test, Assets)) return false;
		}
#else
		// Both public routes must be exercised in the suite: edit for creation, standalone for replacement.
		if (!Test.TestTrue(TEXT("public redefine previously saved provider"), Succeeded(Call(Provider, { Update() }, true, true)))
			|| !Compile(Test, Caller, TEXT("caller after changed signature"))
			|| !Compile(Test, DefaultCaller, TEXT("defaults caller after provider compile"))
			|| !Compile(Test, Child, TEXT("indirect child after caller compile")) || !Verify(Test, Assets)) return false;
#endif
		const auto AfterNodes = MakeShared<FJsonObject>(); AfterNodes->SetArrayField(TEXT("identities"), Identities(Assets));
		Test.TestEqual(TEXT("public calls retain exact graph/node identities"), Serialize(AfterNodes), Serialize(BeforeNodes));
		if (!SaveFixture(Test, Context, Assets)) return false;
		auto Receipt = MakeShared<FJsonObject>();
		Receipt->SetNumberField(TEXT("schemaVersion"), 1); Receipt->SetStringField(TEXT("run"), Context.Run);
		Receipt->SetStringField(TEXT("product"), Product); Receipt->SetStringField(TEXT("root"), Context.Root);
		Receipt->SetNumberField(TEXT("writerPid"), FPlatformProcess::GetCurrentProcessId());
		Receipt->SetBoolField(TEXT("redefinitionSupported"), !UE_VERSION_OLDER_THAN(5, 6, 0));
		Receipt->SetArrayField(TEXT("identities"), Identities(Assets));
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 I=0; I<Assets.Num(); ++I)
		{
			auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("object"), Context.Object(I));
			Row->SetStringField(TEXT("sha1"), Hash(Context.File(I))); Row->SetNumberField(TEXT("bytes"), IFileManager::Get().FileSize(*Context.File(I)));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Receipt->SetArrayField(TEXT("assets"), Rows);
		if (Test.HasAnyErrors()) return false;
		IFileManager::Get().MakeDirectory(*Context.Evidence, true);
		return Test.TestTrue(TEXT("writer receipt after all assertions/save"), FFileHelper::SaveStringToFile(Serialize(Receipt), *Context.Receipt(TEXT("writer.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	static bool ReadReceipt(FAutomationTestBase& Test, const FContext& Context, TSharedPtr<FJsonObject>& Receipt)
	{
		FString Text;
		const FString Path = Context.Receipt(TEXT("writer.json"));
		if (!Test.TestTrue(TEXT("bounded writer receipt exists"), IFileManager::Get().FileSize(*Path) > 0 && IFileManager::Get().FileSize(*Path) < 512*1024 && FFileHelper::LoadFileToString(Text, *Path)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Receipt) && Receipt.IsValid())) return false;
		FString S; double Version=0, PID=0; bool RedefinitionSupported=false; const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
		if (!Test.TestTrue(TEXT("exact receipt schema/run/product/root/asset count and different process"),
			Receipt->TryGetNumberField(TEXT("schemaVersion"), Version) && Version==1
			&& Receipt->TryGetBoolField(TEXT("redefinitionSupported"), RedefinitionSupported) && RedefinitionSupported == !UE_VERSION_OLDER_THAN(5, 6, 0)
			&& Receipt->TryGetStringField(TEXT("run"), S) && S==Context.Run
			&& Receipt->TryGetStringField(TEXT("product"), S) && S==Product
			&& Receipt->TryGetStringField(TEXT("root"), S) && S==Context.Root
			&& Receipt->TryGetNumberField(TEXT("writerPid"), PID) && PID>0 && PID!=FPlatformProcess::GetCurrentProcessId()
			&& Receipt->TryGetArrayField(TEXT("assets"), Rows) && Rows->Num()==UE_ARRAY_COUNT(Names))) return false;
		for (int32 I=0; I<Rows->Num(); ++I)
		{
			auto Row=(*Rows)[I]->AsObject(); double Size=0;
			if (!Test.TestTrue(TEXT("exact declared asset identity and writer bytes"), Row.IsValid()
				&& Row->TryGetStringField(TEXT("object"), S) && S==Context.Object(I)
				&& Row->TryGetStringField(TEXT("sha1"), S) && S.Len()==40 && S==Hash(Context.File(I))
				&& Row->TryGetNumberField(TEXT("bytes"), Size) && Size>0 && Size==IFileManager::Get().FileSize(*Context.File(I)))) return false;
		}
		return true;
	}

	static bool Reader(FAutomationTestBase& Test)
	{
		FContext Context(Test); TSharedPtr<FJsonObject> Receipt;
		if (!Context.bValid || !ReadReceipt(Test, Context, Receipt)) return false;
		if (!Test.TestFalse(TEXT("reader receipt must be fresh"), IFileManager::Get().FileExists(*Context.Receipt(TEXT("reader.json"))))) return false;
		for (int32 I=0; I<UE_ARRAY_COUNT(Names); ++I)
			if (!Test.TestNull(TEXT("cold reader fixture package starts unloaded"), FindPackage(nullptr, *(Context.Root+Names[I])))) return false;
		FNeverSaveOnCompile NeverSave;
		int32 Saves=0, LoadCompiles=0;
		FDelegateHandle SaveHandle=FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject*, FObjectPreSaveContext){++Saves;});
		bool bInspectingPersistedState = true;
		FDelegateHandle CompileHandle=GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* Blueprint)
		{
			++LoadCompiles;
			const FLinkerLoad* Linker = Blueprint ? Blueprint->GetLinker() : nullptr;
			UE_LOG(LogTemp, Display, TEXT("NWIRO_FUNCTION_READER_COMPILE phase=%s path=%s linkerFlags=0x%08x"),
				bInspectingPersistedState ? TEXT("persisted") : TEXT("explicit"), *GetPathNameSafe(Blueprint), Linker ? Linker->LoadFlags : 0u);
		});
		struct FRemove
		{
			FDelegateHandle Save, Compile;
			~FRemove(){FCoreUObjectDelegates::OnObjectPreSave.Remove(Save); GEditor->OnBlueprintPreCompile().Remove(Compile);}
		} Remove{SaveHandle, CompileHandle};
		// LOAD_DisableCompileOnLoad is intentionally non-recursive (ObjectMacros.h).
		// This fixture has a closed dependency order: Enum+Curve, Provider,
		// Caller+DefaultCaller, IndirectChild. Loading each under the flag keeps
		// its dependencies already available without modifying global loader state.
		TArray<TStrongObjectPtr<UObject>> Assets;
		for (int32 I=0; I<UE_ARRAY_COUNT(Names); ++I)
		{
			const FString Label = FString(TEXT("strict cold load ")) + Names[I];
			if (!Test.TestNull(Label + TEXT(" starts with its object unloaded"), FindObject<UObject>(nullptr,*Context.Object(I)))) return false;
			// LoadObject resolves a cold outer with masked flags first (UE5.8
			// UObjectGlobals.cpp). Load the exact package directly, then only find
			// its object; never retry through the unflagged object-resolution path.
			UPackage* Package = LoadPackage(nullptr, *(Context.Root+Names[I]), LOAD_DisableCompileOnLoad);
			if (!Test.TestTrue(Label + TEXT(" resolves exact package"), Package && Package->GetName() == Context.Root+Names[I])) return false;
			Assets.Emplace(FindObject<UObject>(nullptr,*Context.Object(I)));
			if (!Test.TestNotNull(Label + TEXT(" resolves exact saved asset"), Assets.Last().Get())) return false;
			const FLinkerLoad* Linker = Assets.Last()->GetLinker();
			if (!Test.TestTrue(Label + TEXT(" retained compile-disable flag on actual linker"), Linker && (Linker->LoadFlags & LOAD_DisableCompileOnLoad) != 0)) return false;
			if (!Test.TestEqual(Label + TEXT(" did not compile or repair any Blueprint"), LoadCompiles, 0)) return false;
		}
		if (!Verify(Test,Assets,false)) return false;
		if (!Test.TestEqual(TEXT("serialized graph/generated-class inspection did not compile"),LoadCompiles,0)) return false;
		const TArray<TSharedPtr<FJsonValue>>* ExpectedNodes=nullptr;
		if (!Test.TestTrue(TEXT("writer recorded graph/node identities"), Receipt->TryGetArrayField(TEXT("identities"),ExpectedNodes))) return false;
		auto Expected=MakeShared<FJsonObject>(); Expected->SetArrayField(TEXT("identities"),*ExpectedNodes);
		auto Actual=MakeShared<FJsonObject>(); Actual->SetArrayField(TEXT("identities"),Identities(Assets));
		Test.TestEqual(TEXT("cold exact graph/node identity preservation"), Serialize(Actual),Serialize(Expected));
		if (!Test.TestEqual(TEXT("all persisted-state oracles completed without compilation"),LoadCompiles,0)) return false;
		const int32 ObservedLoadCompiles = LoadCompiles;
		bInspectingPersistedState = false;
		int32 ExplicitCompileRequests = 0;
		for(int32 I:{2,3,4,5})
		{
			++ExplicitCompileRequests;
			if(!Compile(Test,CastChecked<UBlueprint>(Assets[I].Get()),TEXT("cold explicit compile"))) return false;
		}
		if(!Verify(Test,Assets)) return false;
		Test.TestEqual(TEXT("reader made no save attempts"),Saves,0);
		TSharedPtr<FJsonObject> FinalReceipt;
		if(!ReadReceipt(Test,Context,FinalReceipt) || Test.HasAnyErrors()) return false;
		auto Result=MakeShared<FJsonObject>(); Result->SetStringField(TEXT("run"),Context.Run); Result->SetStringField(TEXT("product"),Product);
		Result->SetNumberField(TEXT("readerPid"),FPlatformProcess::GetCurrentProcessId()); Result->SetBoolField(TEXT("passed"),true);
		Result->SetNumberField(TEXT("loadTimeCompiles"),ObservedLoadCompiles); Result->SetNumberField(TEXT("saveAttempts"),Saves);
		Result->SetNumberField(TEXT("explicitCompileRequests"),ExplicitCompileRequests);
		Result->SetNumberField(TEXT("observedPostLoadCompileEvents"),LoadCompiles-ObservedLoadCompiles);
		Result->SetStringField(TEXT("loadFlags"),TEXT("LOAD_DisableCompileOnLoad"));
		TArray<TSharedPtr<FJsonValue>> LoadOrder;
		for (const TCHAR* Name : Names) LoadOrder.Add(MakeShared<FJsonValueString>(Name));
		Result->SetArrayField(TEXT("loadOrder"),LoadOrder);
		return Test.TestTrue(TEXT("cold reader receipt after independent semantic checks"),FFileHelper::SaveStringToFile(Serialize(Result),*Context.Receipt(TEXT("reader.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}
	static bool GuardReader(FAutomationTestBase& Test)
	{
		FContext Context(Test); TSharedPtr<FJsonObject> Receipt;
		if (!Context.bValid || !ReadReceipt(Test, Context, Receipt) || !CompleteDiscovery(Test)) return false;
		if (!Test.TestFalse(TEXT("guard receipt must be fresh"), IFileManager::Get().FileExists(*Context.Receipt(TEXT("guard-reader.json"))))) return false;
		for (int32 I=0; I<UE_ARRAY_COUNT(Names); ++I)
			if (!Test.TestNull(TEXT("guard reader begins with unloaded owned packages"), FindPackage(nullptr, *(Context.Root+Names[I])))) return false;
		FNeverSaveOnCompile NeverSave;
		TStrongObjectPtr<UBlueprint> Provider(LoadObject<UBlueprint>(nullptr,*Context.Object(2)));
		if (!Test.TestNotNull(TEXT("guard target explicitly loaded"), Provider.Get())) return false;
		IAssetRegistry& Registry=FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FName> Direct, Indirect;
		Registry.GetReferencers(FName(*(Context.Root+Names[2])),Direct,UE::AssetRegistry::EDependencyCategory::Package);
		Registry.GetReferencers(FName(*(Context.Root+Names[3])),Indirect,UE::AssetRegistry::EDependencyCategory::Package);
		if (!Test.TestTrue(TEXT("real disk registry has both direct caller edges"),Direct.Contains(FName(*(Context.Root+Names[3]))) && Direct.Contains(FName(*(Context.Root+Names[4]))))) return false;
		if (!Test.TestFalse(TEXT("fixture child has no direct provider dependency"),Direct.Contains(FName(*(Context.Root+Names[5]))))) return false;
		if (!Test.TestTrue(TEXT("real disk registry has indirect inherited edge"),Indirect.Contains(FName(*(Context.Root+Names[5]))))) return false;
		auto Attempt = [&](const TCHAR* ContextLabel, const TCHAR* ExpectedPackage)
		{
			const NwiroVariableProof::FSnapshot Before(Provider.Get());
			const FString BeforeGraph=ReadGraph(Provider.Get(),TEXT("Mutable Function"));
			FObservedCall Observe(Provider.Get());
			const auto Result=Call(Provider.Get(),{Update(TEXT("18"))},true,true);
			FString Error, Value;
			bool Success = true, MutationAttempted = true;
			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			bool Rejected = Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Success) && !Success
				&& Result->TryGetBoolField(TEXT("mutationAttempted"), MutationAttempted) && !MutationAttempted
				&& Result->TryGetStringField(TEXT("validationLevel"), Value) && Value == TEXT("preflight")
				&& Result->TryGetArrayField(TEXT("operations"), Operations) && Operations->Num() == 1;
			const TSharedPtr<FJsonObject> Operation = Rejected ? (*Operations)[0]->AsObject() : nullptr;
			Rejected = Rejected && Operation.IsValid() && Operation->TryGetArrayField(TEXT("errors"), Errors) && Errors->Num() == 1;
			const TSharedPtr<FJsonObject> Detail = Rejected ? (*Errors)[0]->AsObject() : nullptr;
			double Unit = -1;
			Rejected = Rejected && Detail.IsValid()
				&& Detail->TryGetStringField(TEXT("op"), Value) && Value == TEXT("add_functions")
				&& Detail->TryGetStringField(TEXT("code"), Value) && Value == TEXT("INVALID_TARGET")
				&& Detail->TryGetStringField(TEXT("field"), Value) && Value == TEXT("name")
				&& Detail->TryGetNumberField(TEXT("unit"), Unit) && Unit == 0
				&& Detail->TryGetStringField(TEXT("received"), Value) && Value == TEXT("Mutable Function")
				&& Detail->TryGetStringField(TEXT("message"), Error);
#if UE_VERSION_OLDER_THAN(5, 6, 0)
			(void)ExpectedPackage;
			Rejected = Rejected && Error.Contains(TEXT("requires Unreal Engine 5.6 or newer"));
#else
			Rejected = Rejected && Error.Contains(Context.Root + ExpectedPackage);
#endif
			Test.TestTrue(FString(ContextLabel)+TEXT(" exact structured dependent rejection"), Rejected);
			Test.TestTrue(TEXT("rejected target graph/declarations/status/dirty remain unchanged"),Before.Matches(Provider.Get()));
			Test.TestEqual(TEXT("rejected graph full readback unchanged"),ReadGraph(Provider.Get(),TEXT("Mutable Function")),BeforeGraph);
			Test.TestEqual(TEXT("rejected target no compile"),Observe.Compiles,0);
			Test.TestEqual(TEXT("rejected target no save"),Observe.Saves,0);
			return Rejected && Before.Matches(Provider.Get()) && !Test.HasAnyErrors();
		};
		// Lexical order makes Caller the first direct referencer in this owned scope.
		if(!Attempt(TEXT("unloaded direct caller"),Names[3])) return false;
		Test.TestNull(TEXT("preflight did not load direct caller"),FindPackage(nullptr,*(Context.Root+Names[3])));
		Test.TestNull(TEXT("preflight did not load second caller"),FindPackage(nullptr,*(Context.Root+Names[4])));
		Test.TestNull(TEXT("preflight did not load indirect child"),FindPackage(nullptr,*(Context.Root+Names[5])));
		const NwiroVariableProof::FSnapshot NoOpBefore(Provider.Get()); FObservedCall NoOpObserve(Provider.Get());
		auto NoOp=Function(TEXT("Mutable Function")); NoOp->AsObject()->SetBoolField(TEXT("redefine"),true);
		Test.TestTrue(TEXT("identical public redefine succeeds with unloaded dependents"),Succeeded(Call(Provider.Get(),{NoOp},true,true)));
		Test.TestTrue(TEXT("no-op keeps target untouched"),NoOpBefore.Matches(Provider.Get()));
		Test.TestEqual(TEXT("no-op did not compile"),NoOpObserve.Compiles,0);
		Test.TestEqual(TEXT("no-op did not save"),NoOpObserve.Saves,0);
		UPackage* Partial=CreatePackage(*(Context.Root+Names[3]));
		if(!Test.TestTrue(TEXT("owned saved caller package placeholder is not fully loaded"),Partial && !Partial->IsFullyLoaded())) return false;
		if(!Attempt(TEXT("partially loaded direct caller"),Names[3])) return false;
		Test.TestNull(TEXT("partial-package rejection did not instantiate caller asset"),FindObject<UBlueprint>(nullptr,*Context.Object(3)));
		TStrongObjectPtr<UBlueprint> Caller(LoadObject<UBlueprint>(nullptr,*Context.Object(3)));
		TStrongObjectPtr<UBlueprint> DefaultCaller(LoadObject<UBlueprint>(nullptr,*Context.Object(4)));
		if(!Test.TestTrue(TEXT("both direct callers explicitly loaded"),Caller.IsValid() && DefaultCaller.IsValid())) return false;
		const NwiroVariableProof::FSnapshot CallerBefore(Caller.Get()), DefaultBefore(DefaultCaller.Get());
		if(!Test.TestNull(TEXT("indirect child stays unloaded after both direct callers load"),FindPackage(nullptr,*(Context.Root+Names[5])))) return false;
		if(!Attempt(TEXT("unloaded transitive child"),Names[5])) return false;
		Test.TestTrue(TEXT("transitive rejection preserves both direct caller states"),CallerBefore.Matches(Caller.Get()) && DefaultBefore.Matches(DefaultCaller.Get()));
		Test.TestNull(TEXT("transitive preflight did not load child"),FindPackage(nullptr,*(Context.Root+Names[5])));
		TStrongObjectPtr<UBlueprint> Child(LoadObject<UBlueprint>(nullptr,*Context.Object(5)));
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		if (!Test.TestTrue(TEXT("legacy guard explicitly loads the final dependent"), Child.IsValid())
			|| !Attempt(TEXT("UE 5.5 still rejects with every dependent loaded"), Names[5])) return false;
#else
		FString Error;
		Test.TestTrue(TEXT("fully loaded reverse closure preflight succeeds"),Child.IsValid() && NwiroFunctions::CheckLoadedReferencerClosure(Provider.Get(),Error));
#endif
		TSharedPtr<FJsonObject> FinalReceipt;
		if(!ReadReceipt(Test,Context,FinalReceipt) || Test.HasAnyErrors()) return false;
		auto Result=MakeShared<FJsonObject>(); Result->SetStringField(TEXT("run"),Context.Run); Result->SetStringField(TEXT("product"),Product);
		Result->SetNumberField(TEXT("readerPid"),FPlatformProcess::GetCurrentProcessId()); Result->SetBoolField(TEXT("passed"),true);
		return Test.TestTrue(TEXT("dependency guard reader receipt"),FFileHelper::SaveStringToFile(Serialize(Result),*Context.Receipt(TEXT("guard-reader.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionPersistenceWriterSpec,
	"Nwiro.Pro.Blueprint.Function.Persistence.Writer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionPersistenceWriterSpec::RunTest(const FString& Parameters) { return NwiroFunctionPersistence::Writer(*this); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionPersistenceReaderSpec,
	"Nwiro.Pro.Blueprint.Function.Persistence.Reader", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionPersistenceReaderSpec::RunTest(const FString& Parameters) { return NwiroFunctionPersistence::Reader(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFunctionPersistenceGuardReaderSpec,
	"Nwiro.Pro.Blueprint.Function.Persistence.GuardReader", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroFunctionPersistenceGuardReaderSpec::RunTest(const FString& Parameters) { return NwiroFunctionPersistence::GuardReader(*this); }
