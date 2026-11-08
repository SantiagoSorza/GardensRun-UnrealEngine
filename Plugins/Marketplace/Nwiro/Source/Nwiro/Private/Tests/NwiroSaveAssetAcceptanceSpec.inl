// Copyright 2026 Nwiro. All Rights Reserved.
// Explicit persistence extension. Included after SaveAssetProofSpec.inl.
#include "Editor.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"
#include "KismetCompiler.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Misc/SecureHash.h"
#include "NwiroBridge.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace NwiroSaveAssetAcceptance
{
	using namespace NwiroSaveAssetProof;

	static FString FileName(UObject* Asset)
	{
		FString Name = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(
			Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
		FPaths::NormalizeFilename(Name);
		return Name;
	}

	static TSharedPtr<FJsonObject> Invoke(const TSharedRef<FJsonObject>& Command, bool bStandalone)
	{
		const FString Raw = bStandalone ? FNwiroBlueprintTools::CreateFunctionGraph(Serialize(Command))
			: FNwiroBlueprintTools::EditBlueprint(Serialize(Command));
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result);
		return Result;
	}

	static TSharedPtr<FJsonObject> Tool(FAutomationTestBase& Test, const TCHAR* Name, const TSharedPtr<FJsonObject>& Command)
	{
		const FString Raw = FNwiroToolDispatch::DispatchTool(Name, Serialize(Command));
		Test.AddInfo(FString(Name) + TEXT(" fixture response ") + Raw);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result);
		return Result;
	}

	static TSharedPtr<FJsonObject> SaveRoute(FAutomationTestBase& Test, const FString& Json, bool bBridge)
	{
		if (!bBridge) return Call(Test, Json);
		// Native UFUNCTION forwarding only: no initialization, Instance mutation,
		// CEF renderer, chat state, asynchronous execution or permission UI.
		TStrongObjectPtr<UNwiroBridge> Bridge(NewObject<UNwiroBridge>(GetTransientPackage()));
		if (!Test.TestNotNull(TEXT("fresh native Pro bridge"), Bridge.Get())) return nullptr;
		const FString Raw = Bridge->ExecuteTool(TEXT("save_asset"), Json);
		Test.AddInfo(TEXT("native bridge save response ") + Raw);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result);
		return Result;
	}

	static bool SaveComplete(FAutomationTestBase& Test, UObject* Asset, bool bBridge = false)
	{
		const bool bBefore = Asset->GetOutermost()->IsDirty();
		FObservedCall Observation;
		const auto Result = SaveRoute(Test, Request(Asset), bBridge);
		Outcome(Test, Result, TEXT("saved"), true, true, true, Asset, bBefore, false, true);
		Test.TestEqual(TEXT("completed explicit save has one package callback"), Observation.Saved.Num(), 1);
		if (Observation.Saved.Num() == 1) Test.TestEqual(TEXT("completed save does not write dependencies"), Observation.Saved[0], Asset->GetOutermost()->GetName());
		Test.TestEqual(TEXT("completed save does not compile"), Observation.Compiles, 0);
		Test.TestEqual(TEXT("completed save does not load"), Observation.Loads, 0);
		Test.TestFalse(TEXT("completed save observed clean"), Asset->GetOutermost()->IsDirty());
		Test.TestTrue(TEXT("completed save has nonempty bytes"), IFileManager::Get().FileSize(*FileName(Asset)) > 0);
		return Succeeded(Result) && Observation.Saved.Num() == 1;
	}

	static bool RunExtendedOutcomes(FAutomationTestBase& Test)
	{
		FContext Context(Test);
		if (!Context.bValid) return false;
		FNeverSaveOnCompile NeverSave;
		const EKind Kinds[] = { EKind::Actor, EKind::SaveGame, EKind::Interface, EKind::Widget };
		const TCHAR* Names[] = { TEXT("Actor"), TEXT("SaveGame"), TEXT("Interface"), TEXT("Widget") };
		for (int32 Kind = 0; Kind < UE_ARRAY_COUNT(Kinds); ++Kind)
		{
			FFixture Fixture(Test, Context, FString(TEXT("Extended_")) + Names[Kind], Kinds[Kind]);
			if (!Fixture.bReady) return false;
			Fixture.Package->SetDirtyFlag(true);
			if (!SaveComplete(Test, Fixture.Asset.Get(), Kind == 0)) return false;
			const FState Before(Fixture.Package);
			FObservedCall Observation;
			Outcome(Test, SaveRoute(Test, Request(Fixture.Asset.Get()), Kind == 0), TEXT("no_op"), true, false, false,
				Fixture.Asset.Get(), false, false, true);
			NoSideEffects(Test, Observation);
			Test.TestTrue(TEXT("Blueprint no-op preserves complete state"), Before.Matches());
		}
		{
			FObservedCall Observation;
			Outcome(Test, SaveRoute(Test, TEXT("{}"), true), TEXT("rejected"), false, false, false, nullptr, false, false, false);
			NoSideEffects(Test, Observation);
		}
		FFixture Placeholder(Test, Context, TEXT("Extended_Placeholder"), EKind::Enum);
		if (!Placeholder.bReady) return false;
		Placeholder.Package->SetDirtyFlag(false);
		Test.TestTrue(TEXT("placeholder fixture fully loaded before file creation"), Placeholder.Package->IsFullyLoaded());
		const FString PlaceholderFile = FileName(Placeholder.Asset.Get());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(PlaceholderFile), true);
		if (!Test.TestTrue(TEXT("owned stale placeholder created"), FFileHelper::SaveStringToFile(TEXT("owned stale placeholder; not an Unreal package"), *PlaceholderFile))) return false;
		TArray<uint8> BeforeBytes, AfterBytes;
		if (!Test.TestTrue(TEXT("owned placeholder bytes readable"), FFileHelper::LoadFileToArray(BeforeBytes, *PlaceholderFile))) return false;
		if (!SaveComplete(Test, Placeholder.Asset.Get())) return false;
		Test.TestTrue(TEXT("replacement package bytes readable"), FFileHelper::LoadFileToArray(AfterBytes, *PlaceholderFile));
		Test.TestTrue(TEXT("fresh package replaces stale existing bytes"), BeforeBytes != AfterBytes);
		{
			FFixture Redirtied(Test, Context, TEXT("Extended_Redirtied"), EKind::Enum);
			if (!Redirtied.bReady) return false;
			Redirtied.Package->SetDirtyFlag(true);
			bool bDirtiedOnce = false;
			struct FRemoveSavedDelegate
			{
				FDelegateHandle Handle;
				~FRemoveSavedDelegate() { UPackage::PackageSavedWithContextEvent.Remove(Handle); }
			};
			FRemoveSavedDelegate Hook{ UPackage::PackageSavedWithContextEvent.AddLambda(
				[&](const FString&, UPackage* Package, FObjectPostSaveContext SaveContext)
				{
					if (Package == Redirtied.Package && SaveContext.SaveSucceeded() && !bDirtiedOnce)
					{ bDirtiedOnce = true; Package->SetDirtyFlag(true); }
				}) };
			FObservedCall Observation;
			Outcome(Test, Call(Test, Request(Redirtied.Asset.Get())), TEXT("failed"), false, true, true,
				Redirtied.Asset.Get(), true, true, true);
			Test.TestTrue(TEXT("owned post-save callback dirtied exactly once"), bDirtiedOnce);
			Test.TestEqual(TEXT("dirty-after failure still completed one write"), Observation.Saved.Num(), 1);
			Test.TestTrue(TEXT("dirty-after failure retains observed dirty state"), Redirtied.Package->IsDirty());
			Test.TestTrue(TEXT("dirty-after failure has real saved bytes"), IFileManager::Get().FileSize(*FileName(Redirtied.Asset.Get())) > 0);
			Test.TestEqual(TEXT("dirty-after save does not compile"), Observation.Compiles, 0);
			Test.TestEqual(TEXT("dirty-after save does not load"), Observation.Loads, 0);
		}
		FFixture Unpublished(Test, Context, TEXT("Extended_Unpublished"), EKind::Enum, false);
		if (!Unpublished.bReady) return false;
		// In-memory registry identity does not expose AssetCreated call history.
		return SaveComplete(Test, Unpublished.Asset.Get());
	}

	static bool RunObstructedSave(FAutomationTestBase& Test)
	{
#if PLATFORM_WINDOWS
		FContext Context(Test);
		if (!Context.bValid) return false;
		FNeverSaveOnCompile NeverSave;
		FFixture Fixture(Test, Context, TEXT("Obstructed_Enum"), EKind::Enum);
		if (!Fixture.bReady || !SaveComplete(Test, Fixture.Asset.Get())) return false;
		const FString Path = FileName(Fixture.Asset.Get());
		TArray<uint8> Before, After;
		if (!Test.TestTrue(TEXT("obstruction original bytes readable"), FFileHelper::LoadFileToArray(Before, *Path))) return false;
		const DWORD Attributes = GetFileAttributesW(*Path);
		if (!Test.TestTrue(TEXT("owned obstruction attributes available"), Attributes != INVALID_FILE_ATTRIBUTES)) return false;
		struct FRestoreAttributes
		{
			FString Path;
			DWORD Original;
			~FRestoreAttributes() { SetFileAttributesW(*Path, Original); }
		};
		{
			FRestoreAttributes Restore{ Path, Attributes };
			if (!Test.TestTrue(TEXT("only owned target made read-only"), SetFileAttributesW(*Path, Attributes | FILE_ATTRIBUTE_READONLY) != 0)) return false;
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(CastChecked<UUserDefinedEnum>(Fixture.Asset.Get()));
			Fixture.Package->SetDirtyFlag(true);
			Test.AddExpectedError(FString::Printf(TEXT("Cannot remove '%s' as it is read only!"), *Path), EAutomationExpectedErrorFlags::Contains, 1);
			FObservedCall Observation;
			Outcome(Test, Call(Test, Request(Fixture.Asset.Get())), TEXT("failed"), false, true, false, Fixture.Asset.Get(), true, true, true);
			Test.TestEqual(TEXT("obstructed save has no completed save"), Observation.Saved.Num(), 0);
			Test.TestEqual(TEXT("obstructed save does not compile"), Observation.Compiles, 0);
			Test.TestEqual(TEXT("obstructed save does not load"), Observation.Loads, 0);
			Test.TestTrue(TEXT("obstructed target remains dirty"), Fixture.Package->IsDirty());
			Test.TestTrue(TEXT("obstructed bytes remain readable"), FFileHelper::LoadFileToArray(After, *Path));
			Test.TestTrue(TEXT("obstructed save preserves old disk bytes"), Before == After);
		}
		const DWORD RestoredAttributes = GetFileAttributesW(*Path);
		Test.TestEqual(TEXT("all owned original file attributes restored"), RestoredAttributes, Attributes);
		Test.AddInfo(FString::Printf(TEXT("owned obstruction attributes: path=%s original=%u restored=%u"), *Path, Attributes, RestoredAttributes));
		return true;
#else
		Test.AddError(TEXT("Read-only persistence proof requires the supported Windows host"));
		return false;
#endif
	}

	static TSharedPtr<FJsonValue> Function(const FString& Name)
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Name);
		return MakeShared<FJsonValueObject>(Object);
	}

	static TSharedPtr<FJsonValue> Pin(const FString& Name, const FString& Base,
		const FString& Subtype = FString(), const FString& Container = TEXT("none"), bool bNested = false)
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Name);
		const auto Type = bNested ? MakeShared<FJsonObject>() : Object;
		Type->SetStringField(bNested ? TEXT("base") : TEXT("type"), Base);
		if (!Subtype.IsEmpty()) Type->SetStringField(TEXT("subtype"), Subtype);
		Type->SetStringField(TEXT("container"), Container);
		if (bNested) Object->SetObjectField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Object);
	}

	static UEdGraph* Graph(UBlueprint* BP, FName Name)
	{
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		for (UEdGraph* Candidate : Graphs) if (Candidate && Candidate->GetFName() == Name) return Candidate;
		return nullptr;
	}

	static bool ReflectedPin(UFunction* Function, FName Name, FName Category,
		UObject* Subtype, EPinContainerType Container, bool bOutput)
	{
		FProperty* Property = Function ? FindFProperty<FProperty>(Function, Name) : nullptr;
		FEdGraphPinType Type;
		return Property && Property->HasAnyPropertyFlags(CPF_Parm)
			&& (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm)) == bOutput
			&& GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, Type)
			&& Type.PinCategory == Category && Type.PinSubCategoryObject.Get() == Subtype && Type.ContainerType == Container;
	}

	static TSharedRef<FJsonObject> EditCommand(UBlueprint* BP, const FString& GraphName = FString())
	{
		const auto Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Command->SetBoolField(TEXT("compile"), false);
		if (!GraphName.IsEmpty()) Command->SetStringField(TEXT("graph"), GraphName);
		return Command;
	}

	static bool Compile(FAutomationTestBase& Test, UBlueprint* BP, const FString& Context)
	{
		FCompilerResultsLog Compiler;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Compiler);
		const bool bClean = Test.TestEqual(Context + TEXT(" compiler errors"), Compiler.NumErrors, 0);
		return Test.TestEqual(Context + TEXT(" compiled status"), BP->Status.GetValue(), BS_UpToDate) && bClean;
	}

	static FString PublicNodeGuid(UBlueprint* BP, const FString& GraphName, const FString& ClassName)
	{
		const auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Request->SetStringField(TEXT("graph"), GraphName);
		TSharedPtr<FJsonObject> Response;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroBlueprintTools::ReadBlueprint(Serialize(Request))), Response);
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("nodes"), Nodes)) return FString();
		for (const auto& Node : *Nodes)
		{
			const auto Object = Node->AsObject();
			if (Object.IsValid() && Object->GetStringField(TEXT("class")) == ClassName) return Object->GetStringField(TEXT("guid"));
		}
		return FString();
	}

	static TSharedPtr<FJsonObject> AddFunction(UBlueprint* BP, const TSharedPtr<FJsonValue>& Signature)
	{
		const auto Command = EditCommand(BP);
		Command->SetArrayField(TEXT("add_functions"), { Signature });
		return Invoke(Command, false);
	}

	static bool BuildCaller(FAutomationTestBase& Test, UBlueprint* Provider, UBlueprint* BP,
		const TArray<TSharedPtr<FJsonValue>>& Inputs, const TArray<TSharedPtr<FJsonValue>>& Outputs,
		UScriptStruct* Struct)
	{
		if (!Test.TestNotNull(TEXT("owned caller target"), BP)) return false;
		FObservedCall Observation;
		const auto Signature = Function(TEXT("Consume Records"));
		TArray<TSharedPtr<FJsonValue>> CallerInputs = Inputs;
		CallerInputs.Insert(Pin(TEXT("Provider"), TEXT("object"), Provider->GeneratedClass->GetPathName(), TEXT("none"), true), 0);
		Signature->AsObject()->SetArrayField(TEXT("inputs"), CallerInputs);
		Signature->AsObject()->SetArrayField(TEXT("outputs"), Outputs);
		if (!Test.TestTrue(TEXT("public caller signature created"), Succeeded(AddFunction(BP, Signature)))) return false;
		const FString Entry = PublicNodeGuid(BP, TEXT("Consume Records"), TEXT("K2Node_FunctionEntry"));
		const FString Result = PublicNodeGuid(BP, TEXT("Consume Records"), TEXT("K2Node_FunctionResult"));
		if (!Test.TestFalse(TEXT("public caller entry GUID available"), Entry.IsEmpty())
			|| !Test.TestFalse(TEXT("public caller result GUID available"), Result.IsEmpty())) return false;
		const auto CallNode = MakeShared<FJsonObject>();
		CallNode->SetStringField(TEXT("type"), TEXT("CallFunction"));
		CallNode->SetStringField(TEXT("function"), TEXT("Get Records"));
		CallNode->SetStringField(TEXT("target"), Provider->GeneratedClass->GetPathName());
		CallNode->SetStringField(TEXT("ref"), TEXT("TypedCall"));
		const auto Command = EditCommand(BP, TEXT("Consume Records"));
		Command->SetArrayField(TEXT("add_nodes"), { MakeShared<FJsonValueObject>(CallNode) });
		TArray<TSharedPtr<FJsonValue>> Links;
		auto Link = [&](const FString& From, const FString& To)
		{
			const auto Connection = MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("from"), From); Connection->SetStringField(TEXT("to"), To);
			Links.Add(MakeShared<FJsonValueObject>(Connection));
		};
		Link(Entry + TEXT(".Provider"), TEXT("TypedCall.self"));
		Link(Entry + TEXT(".then"), TEXT("TypedCall.execute"));
		Link(TEXT("TypedCall.then"), Result + TEXT(".execute"));
		for (const auto& Input : Inputs)
		{
			const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
			Link(Entry + TEXT(".") + Name, TEXT("TypedCall.") + Name);
		}
		for (const auto& Output : Outputs)
		{
			const FString Name = Output->AsObject()->GetStringField(TEXT("name"));
			Link(TEXT("TypedCall.") + Name, Result + TEXT(".") + Name);
		}
		Command->SetArrayField(TEXT("connect_pins"), Links);
		const auto Response = Invoke(Command, false);
		if (Response.IsValid()) Test.AddInfo(TEXT("public caller wiring response ") + Serialize(Response.ToSharedRef()));
		if (!Test.TestTrue(TEXT("public caller nodes and links succeed"), Succeeded(Response))) return false;
		if (!Compile(Test, BP, TEXT("public typed caller"))) return false;
		UFunction* Consumer = BP->GeneratedClass->FindFunctionByName(TEXT("Consume Records"));
		Test.TestTrue(TEXT("compiled caller exact custom struct array output"), ReflectedPin(Consumer, TEXT("Records"),
			UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::Array, true));
		UEdGraph* CallerGraph = Graph(BP, TEXT("Consume Records"));
		UK2Node_CallFunction* ActualCall = nullptr;
		if (CallerGraph) for (UEdGraphNode* Node : CallerGraph->Nodes)
			if (UK2Node_CallFunction* Candidate = Cast<UK2Node_CallFunction>(Node)) ActualCall = Candidate;
		if (!Test.TestNotNull(TEXT("compiled graph has real call node"), ActualCall)) return false;
		// Editor node lookup uses the caller skeleton; generated-scope lookup resolves the compiled function.
		UFunction* EditorFunction = ActualCall->GetTargetFunction();
		UFunction* GeneratedFunction = ActualCall->FunctionReference.ResolveMember<UFunction>(BP->GeneratedClass.Get());
		UFunction* ExpectedGenerated = Provider->GeneratedClass->FindFunctionByName(TEXT("Get Records"));
		UFunction* ExpectedSkeleton = Provider->SkeletonGeneratedClass
			? Provider->SkeletonGeneratedClass->FindFunctionByName(TEXT("Get Records")) : nullptr;
		Test.AddInfo(FString::Printf(TEXT("caller function binding: editor=%s editorOwner=%s generated=%s generatedOwner=%s expectedSkeleton=%s expectedGenerated=%s referenceParent=%s"),
			*GetPathNameSafe(EditorFunction), *GetPathNameSafe(EditorFunction ? EditorFunction->GetOuter() : nullptr),
			*GetPathNameSafe(GeneratedFunction), *GetPathNameSafe(GeneratedFunction ? GeneratedFunction->GetOuter() : nullptr),
			*GetPathNameSafe(ExpectedSkeleton), *GetPathNameSafe(ExpectedGenerated), *GetPathNameSafe(ActualCall->FunctionReference.GetMemberParentClass())));
		Test.TestTrue(TEXT("editor caller resolves exact provider skeleton function and owner"),
			EditorFunction && EditorFunction == ExpectedSkeleton && EditorFunction->GetOuter() == Provider->SkeletonGeneratedClass.Get());
		Test.TestTrue(TEXT("generated caller resolves exact provider generated function and owner"),
			GeneratedFunction && GeneratedFunction == ExpectedGenerated && GeneratedFunction->GetOuter() == Provider->GeneratedClass.Get());
		Test.TestTrue(TEXT("caller external reference retains exact provider generated class"),
			!ActualCall->FunctionReference.IsSelfContext() && ActualCall->FunctionReference.GetMemberParentClass() == Provider->GeneratedClass.Get());
		for (const auto& Input : CallerInputs)
		{
			const FString Name = Input->AsObject()->GetStringField(TEXT("name"));
			UEdGraphPin* Pin = ActualCall->FindPin(Name == TEXT("Provider") ? FName(TEXT("self")) : FName(*Name));
			Test.TestTrue(TEXT("caller input connected: ") + Name, Pin && Pin->LinkedTo.Num() == 1);
		}
		UEdGraphPin* Returned = ActualCall->FindPin(TEXT("Records"));
		Test.TestTrue(TEXT("caller output connected to result"), Returned && Returned->LinkedTo.Num() == 1);
		Test.TestEqual(TEXT("public caller never saves"), Observation.Attempts.Num(), 0);
		Test.TestFalse(TEXT("public caller absent on disk"), IFileManager::Get().FileExists(*FileName(BP)));
		return true;
	}

	static const TCHAR* ChainNames[] = { TEXT("E_RecordKind"), TEXT("BP_ReferenceActor"), TEXT("S_Record"),
		TEXT("BPI_Records"), TEXT("BP_RecordSaveGame"), TEXT("BP_RecordCaller") };

	static FString ObjectPath(const FContext& Context, const TCHAR* Name)
	{
		return Context.Root + Name + TEXT(".") + Name;
	}

	static bool ManifestPath(FAutomationTestBase& Test, const FContext& Context, FString& Path)
	{
		FParse::Value(FCommandLine::Get(), TEXT("NwiroSaveProofManifest="), Path);
		return Test.TestTrue(TEXT("manifest path is exact runner-owned artifact"), !Path.IsEmpty() && !FPaths::IsRelative(Path)
			&& FPaths::GetCleanFilename(Path) == TEXT("save-proof-") + Context.RunId + TEXT(".json"));
	}

	static FString DiskHash(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return FString();
		return FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString();
	}

	struct FChain
	{
		TArray<TStrongObjectPtr<UObject>> Objects;
		TArray<TUniquePtr<FFixture>> Blueprints;
		UUserDefinedEnum* Enum = nullptr;
		UBlueprint* Reference = nullptr;
		UUserDefinedStruct* Struct = nullptr;
		UBlueprint* Interface = nullptr;
		UBlueprint* Provider = nullptr;
		UBlueprint* Caller = nullptr;
		~FChain()
		{
			for (const auto& Object : Objects)
			{
				if (!Object.IsValid() || Object->IsA<UBlueprint>()) continue;
				UPackage* Package = Object->GetOutermost();
				ForEachObjectWithPackage(Package, [](UObject* Child)
				{
					Child->ClearFlags(RF_Public | RF_Standalone);
					return true;
				}, true);
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}
		UBlueprint* Blueprint(FAutomationTestBase& Test, const FContext& Context, const TCHAR* Name, EKind Kind)
		{
			auto Fixture = MakeUnique<FFixture>(Test, Context, Name, Kind);
			if (!Fixture->bReady) return nullptr;
			UBlueprint* BP = CastChecked<UBlueprint>(Fixture->Asset.Get());
			Objects.Emplace(BP);
			Blueprints.Add(MoveTemp(Fixture));
			return BP;
		}
	};

	static bool PrepareChain(FAutomationTestBase& Test, const FContext& Context, FChain& Chain)
	{
		for (const TCHAR* Name : ChainNames)
		{
			const FString Package = Context.Root + Name;
			if (!Test.TestNull(TEXT("writer package initially absent"), FindPackage(nullptr, *Package))) return false;
			const FString File = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
			if (!Test.TestFalse(TEXT("writer file initially absent"), IFileManager::Get().FileExists(*File))) return false;
		}
		FObservedCall Preparation;
		const auto EnumCommand = MakeShared<FJsonObject>();
		EnumCommand->SetStringField(TEXT("name"), ChainNames[0]);
		EnumCommand->SetStringField(TEXT("path"), Context.Root.LeftChop(1));
		EnumCommand->SetArrayField(TEXT("values"), { MakeShared<FJsonValueString>(TEXT("Idle")), MakeShared<FJsonValueString>(TEXT("Active")) });
		if (!Test.TestTrue(TEXT("public chain enum creation"), Succeeded(Tool(Test, TEXT("create_enum"), EnumCommand)))) return false;
		Chain.Enum = FindObject<UUserDefinedEnum>(nullptr, *ObjectPath(Context, ChainNames[0]));
		if (!Test.TestNotNull(TEXT("exact created chain enum"), Chain.Enum)) return false;
		Chain.Objects.Emplace(Chain.Enum);
		Chain.Reference = Chain.Blueprint(Test, Context, ChainNames[1], EKind::Actor);
		if (!Chain.Reference) return false;
		const auto StructCommand = MakeShared<FJsonObject>();
		StructCommand->SetStringField(TEXT("name"), ChainNames[2]);
		StructCommand->SetStringField(TEXT("path"), Context.Root.LeftChop(1));
		StructCommand->SetArrayField(TEXT("fields"), {
			Pin(TEXT("Kind"), TEXT("enum"), Chain.Enum->GetPathName(), TEXT("none"), true),
			Pin(TEXT("ActorClass"), TEXT("class"), Chain.Reference->GeneratedClass->GetPathName(), TEXT("none"), true),
			Pin(TEXT("ActorInstance"), TEXT("object"), Chain.Reference->GeneratedClass->GetPathName()),
			Pin(TEXT("Transform"), TEXT("transform")) });
		if (!Test.TestTrue(TEXT("public chain struct creation"), Succeeded(Tool(Test, TEXT("create_struct"), StructCommand)))) return false;
		Chain.Struct = FindObject<UUserDefinedStruct>(nullptr, *ObjectPath(Context, ChainNames[2]));
		if (!Test.TestNotNull(TEXT("exact created chain struct"), Chain.Struct)) return false;
		Chain.Objects.Emplace(Chain.Struct);
		Chain.Interface = Chain.Blueprint(Test, Context, ChainNames[3], EKind::Interface);
		Chain.Provider = Chain.Blueprint(Test, Context, ChainNames[4], EKind::SaveGame);
		Chain.Caller = Chain.Blueprint(Test, Context, ChainNames[5], EKind::Actor);
		if (!Chain.Interface || !Chain.Provider || !Chain.Caller) return false;
		const TArray<TSharedPtr<FJsonValue>> Inputs = { Pin(TEXT("Record"), TEXT("struct"), Chain.Struct->GetPathName(), TEXT("none"), true) };
		const TArray<TSharedPtr<FJsonValue>> Outputs = { Pin(TEXT("Records"), TEXT("struct"), Chain.Struct->GetPathName(), TEXT("array"), true) };
		const auto Signature = Function(TEXT("Get Records"));
		Signature->AsObject()->SetArrayField(TEXT("inputs"), Inputs);
		Signature->AsObject()->SetArrayField(TEXT("outputs"), Outputs);
		if (!Test.TestTrue(TEXT("public persisted interface signature"), Succeeded(AddFunction(Chain.Interface, Signature)))
			|| !Compile(Test, Chain.Interface, TEXT("persisted interface signature"))) return false;
		const auto Variable = Pin(TEXT("SavedCount"), TEXT("int"));
		Variable->AsObject()->SetStringField(TEXT("default"), TEXT("7"));
		Variable->AsObject()->SetBoolField(TEXT("save_game"), true);
		const auto Variables = EditCommand(Chain.Provider);
		Variables->SetArrayField(TEXT("add_variables"), { Variable });
		if (!Test.TestTrue(TEXT("public persisted SaveGame default"), Succeeded(Invoke(Variables, false)))) return false;
		const auto Attach = MakeShared<FJsonObject>();
		Attach->SetStringField(TEXT("blueprint"), Chain.Provider->GetPathName());
		Attach->SetStringField(TEXT("interface"), Chain.Interface->GeneratedClass->GetPathName());
		if (!Test.TestTrue(TEXT("public persisted interface implementation"), Succeeded(Tool(Test, TEXT("add_interface"), Attach)))
			|| !Compile(Test, Chain.Provider, TEXT("persisted provider"))) return false;
		if (!BuildCaller(Test, Chain.Provider, Chain.Caller, Inputs, Outputs, Chain.Struct)) return false;
		Test.TestEqual(TEXT("chain preparation never saves implicitly"), Preparation.Attempts.Num(), 0);
		Test.TestEqual(TEXT("chain preparation has no completed saves"), Preparation.Saved.Num(), 0);
		for (const auto& Object : Chain.Objects)
		{
			Object->GetOutermost()->MarkAsFullyLoaded();
			Test.TestFalse(TEXT("prepared chain remains off disk"), IFileManager::Get().FileExists(*FileName(Object.Get())));
		}
		return Preparation.Attempts.IsEmpty();
	}

	static bool CheckSignature(FAutomationTestBase& Test, UBlueprint* BP, const FName Name, UUserDefinedStruct* Struct, bool bCaller, UClass* ProviderClass = nullptr)
	{
		bool bValid = true;
		for (UClass* Owner : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
		{
			UFunction* Function = Owner ? Owner->FindFunctionByName(Name) : nullptr;
			if (!Test.TestTrue(TEXT("persisted function has exact generated/skeleton owner"), Function && Function->GetOuter() == Owner)) { bValid = false; continue; }
			Test.TestTrue(TEXT("persisted function exact struct input"), ReflectedPin(Function, TEXT("Record"), UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::None, false));
			Test.TestTrue(TEXT("persisted function exact struct-array output"), ReflectedPin(Function, TEXT("Records"), UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::Array, true));
			if (bCaller) Test.TestTrue(TEXT("persisted caller exact provider object constraint"), ProviderClass && ReflectedPin(Function,
				TEXT("Provider"), UEdGraphSchema_K2::PC_Object, ProviderClass, EPinContainerType::None, false));
			TArray<FName> Params;
			for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				if (It->HasAnyPropertyFlags(CPF_Parm)) Params.Add(It->GetFName());
			const TArray<FName> Expected = bCaller ? TArray<FName>{ TEXT("Provider"), TEXT("Record"), TEXT("Records") }
				: TArray<FName>{ TEXT("Record"), TEXT("Records") };
			Test.TestTrue(TEXT("persisted parameter order and names"), Params == Expected);
		}
		return bValid;
	}

	static TSharedPtr<FJsonObject> RecordManifest(FAutomationTestBase& Test, const FContext& Context, const FChain& Chain)
	{
		const auto Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("schemaVersion"), 1);
		Manifest->SetStringField(TEXT("product"), TEXT("Nwiro"));
		Manifest->SetStringField(TEXT("runId"), Context.RunId);
		Manifest->SetStringField(TEXT("root"), Context.Root);
		Manifest->SetNumberField(TEXT("expectedSaveGameCount"), 31);
		TArray<TSharedPtr<FJsonValue>> EnumEntries;
		for (int32 Index = 0; Index < Chain.Enum->NumEnums(); ++Index)
		{
			const auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("internalName"), Chain.Enum->GetNameStringByIndex(Index));
			Entry->SetStringField(TEXT("displayName"), Chain.Enum->GetDisplayNameTextByIndex(Index).ToString());
			Entry->SetNumberField(TEXT("value"), Chain.Enum->GetValueByIndex(Index));
			EnumEntries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Manifest->SetArrayField(TEXT("enumEntries"), EnumEntries);
		TArray<TSharedPtr<FJsonValue>> Assets;
		for (int32 Index = 0; Index < Chain.Objects.Num(); ++Index)
		{
			UObject* Asset = Chain.Objects[Index].Get();
			const auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), ChainNames[Index]);
			Row->SetStringField(TEXT("packagePath"), Asset->GetOutermost()->GetName());
			Row->SetStringField(TEXT("objectPath"), Asset->GetPathName());
			Row->SetStringField(TEXT("fileName"), FileName(Asset));
			Row->SetStringField(TEXT("sha1"), DiskHash(FileName(Asset)));
			Row->SetNumberField(TEXT("byteCount"), IFileManager::Get().FileSize(*FileName(Asset)));
			if (UBlueprint* BP = Cast<UBlueprint>(Asset)) Row->SetStringField(TEXT("blueprintGuid"), BP->GetBlueprintGuid().ToString(EGuidFormats::Digits));
			Assets.Add(MakeShared<FJsonValueObject>(Row));
		}
		Manifest->SetArrayField(TEXT("assets"), Assets);
		TArray<TSharedPtr<FJsonValue>> Fields;
		for (const auto& Field : FStructureEditorUtils::GetVarDesc(Chain.Struct))
		{
			const auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("friendlyName"), Field.FriendlyName);
			Row->SetStringField(TEXT("reflectedName"), Field.VarName.ToString());
			Row->SetStringField(TEXT("guid"), Field.VarGuid.ToString(EGuidFormats::Digits));
			Fields.Add(MakeShared<FJsonValueObject>(Row));
		}
		Manifest->SetArrayField(TEXT("structFields"), Fields);
		const auto Nodes = MakeShared<FJsonObject>();
		UEdGraph* CallerGraph = Graph(Chain.Caller, TEXT("Consume Records"));
		if (!Test.TestNotNull(TEXT("writer caller graph for manifest"), CallerGraph)) return nullptr;
		for (UEdGraphNode* Node : CallerGraph->Nodes)
		{
			const TCHAR* Role = Node->IsA<UK2Node_FunctionEntry>() ? TEXT("entry") : Node->IsA<UK2Node_FunctionResult>() ? TEXT("result")
				: Node->IsA<UK2Node_CallFunction>() ? TEXT("call") : nullptr;
			if (Role) Nodes->SetStringField(Role, Node->NodeGuid.ToString(EGuidFormats::Digits));
		}
		Manifest->SetObjectField(TEXT("callerNodes"), Nodes);
		return Manifest;
	}

	static bool RunWriter(FAutomationTestBase& Test)
	{
		FContext Context(Test);
		FString Path;
		if (!Context.bValid || !ManifestPath(Test, Context, Path)) return false;
		if (!Test.TestFalse(TEXT("writer manifest initially absent"), IFileManager::Get().FileExists(*Path))) return false;
		FNeverSaveOnCompile NeverSave;
		FChain Chain;
		if (!PrepareChain(Test, Context, Chain)) return false;
		for (const auto& Asset : Chain.Objects) if (!SaveComplete(Test, Asset.Get())) return false;
		FIntProperty* Count = FindFProperty<FIntProperty>(Chain.Provider->GeneratedClass.Get(), TEXT("SavedCount"));
		if (!Test.TestNotNull(TEXT("persisted SaveGame count property"), Count)) return false;
		UObject* Default = Chain.Provider->GeneratedClass->GetDefaultObject(false);
		if (!Test.TestNotNull(TEXT("persisted SaveGame existing CDO"), Default)) return false;
		Test.TestEqual(TEXT("first SaveGame default is seven"), Count->GetPropertyValue_InContainer(Default), 7);
		Test.TestTrue(TEXT("SaveGame property flag before persistence"), Count->HasAnyPropertyFlags(CPF_SaveGame));
		const FString InitialHash = DiskHash(FileName(Chain.Provider));
		// Owned fixture changes the CDO without compiling or changing its signature.
		Default->Modify();
		Count->SetPropertyValue_InContainer(Default, 31);
		Chain.Provider->GetOutermost()->SetDirtyFlag(true);
		if (!SaveComplete(Test, Chain.Provider)) return false;
		Test.TestTrue(TEXT("explicit second write changes serialized default bytes"), InitialHash != DiskHash(FileName(Chain.Provider)));
		const auto Manifest = RecordManifest(Test, Context, Chain);
		if (!Manifest) return false;
		return Test.TestTrue(TEXT("immutable writer manifest written"), FFileHelper::SaveStringToFile(Serialize(Manifest), *Path));
	}

	static bool ReadManifest(FAutomationTestBase& Test, const FContext& Context, const FString& Path,
		TSharedPtr<FJsonObject>& Manifest, TArray<TSharedPtr<FJsonValue>>& Assets)
	{
		FString Text;
		if (!Test.TestTrue(TEXT("writer manifest is bounded readable evidence"), IFileManager::Get().FileSize(*Path) > 0
			&& IFileManager::Get().FileSize(*Path) < 512 * 1024 && FFileHelper::LoadFileToString(Text, *Path))) return false;
		if (!Test.TestTrue(TEXT("writer manifest is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) && Manifest.IsValid())) return false;
		FString Value;
		double Version = 0;
		bool bValid = Manifest->TryGetNumberField(TEXT("schemaVersion"), Version) && Version == 1;
		bValid &= Manifest->TryGetStringField(TEXT("product"), Value) && Value == TEXT("Nwiro");
		bValid &= Manifest->TryGetStringField(TEXT("runId"), Value) && Value == Context.RunId;
		bValid &= Manifest->TryGetStringField(TEXT("root"), Value) && Value == Context.Root;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		bValid &= Manifest->TryGetArrayField(TEXT("assets"), Rows) && Rows && Rows->Num() == UE_ARRAY_COUNT(ChainNames);
		if (!Test.TestTrue(TEXT("writer manifest exact schema/product/run/six-asset scope"), bValid)) return false;
		for (int32 Index = 0; Index < Rows->Num(); ++Index)
		{
			const auto Row = (*Rows)[Index]->AsObject();
			if (!Test.TestTrue(TEXT("manifest asset is an object"), Row.IsValid())) return false;
			const FString Package = Context.Root + ChainNames[Index];
			FString ExpectedFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()));
			FPaths::NormalizeFilename(ExpectedFile);
			bValid = Row->TryGetStringField(TEXT("name"), Value) && Value == ChainNames[Index];
			bValid &= Row->TryGetStringField(TEXT("packagePath"), Value) && Value == Package;
			bValid &= Row->TryGetStringField(TEXT("objectPath"), Value) && Value == ObjectPath(Context, ChainNames[Index]);
			bValid &= Row->TryGetStringField(TEXT("fileName"), Value) && Value == ExpectedFile;
			FString Hash;
			bValid &= Row->TryGetStringField(TEXT("sha1"), Hash) && Hash.Len() == 40 && Hash == DiskHash(ExpectedFile);
			double Size = 0;
			bValid &= Row->TryGetNumberField(TEXT("byteCount"), Size) && Size > 0 && Size == IFileManager::Get().FileSize(*ExpectedFile);
			if (!Test.TestTrue(TEXT("manifest exact declared path and writer bytes"), bValid)) return false;
		}
		Assets = *Rows;
		return true;
	}

	static bool CheckStruct(FAutomationTestBase& Test, UUserDefinedStruct* Struct, UUserDefinedEnum* Enum,
		UBlueprint* Reference, const TSharedPtr<FJsonObject>& Manifest)
	{
		const auto& Fields = FStructureEditorUtils::GetVarDesc(Struct);
		const TArray<TSharedPtr<FJsonValue>>* Recorded = nullptr;
		if (!Test.TestTrue(TEXT("persisted struct has exactly four recorded fields"), Fields.Num() == 4
			&& Manifest->TryGetArrayField(TEXT("structFields"), Recorded) && Recorded->Num() == 4)) return false;
		const TCHAR* Names[] = { TEXT("Kind"), TEXT("ActorClass"), TEXT("ActorInstance"), TEXT("Transform") };
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			const auto Row = (*Recorded)[Index]->AsObject();
			if (!Test.TestTrue(TEXT("recorded struct field is an object"), Row.IsValid())) return false;
			FString Value;
			Test.TestEqual(TEXT("persisted exact friendly field name"), Fields[Index].FriendlyName, FString(Names[Index]));
			Test.TestTrue(TEXT("persisted friendly field matches writer"), Row->TryGetStringField(TEXT("friendlyName"), Value) && Value == Fields[Index].FriendlyName);
			Test.TestTrue(TEXT("persisted reflected field name matches writer"), Row->TryGetStringField(TEXT("reflectedName"), Value) && Value == Fields[Index].VarName.ToString());
			Test.TestTrue(TEXT("persisted field GUID matches writer"), Row->TryGetStringField(TEXT("guid"), Value) && Value == Fields[Index].VarGuid.ToString(EGuidFormats::Digits));
			FProperty* Property = FindFProperty<FProperty>(Struct, Fields[Index].VarName);
			if (!Test.TestNotNull(TEXT("persisted reflected field exists"), Property)) return false;
			Test.TestNull(TEXT("persisted scalar field is not an array"), CastField<FArrayProperty>(Property));
			if (Index == 0)
			{
				const auto* Byte = CastField<FByteProperty>(Property);
				const auto* EnumProperty = CastField<FEnumProperty>(Property);
				Test.TestTrue(TEXT("persisted enum reference exact identity"), (Byte && Byte->Enum == Enum) || (EnumProperty && EnumProperty->GetEnum() == Enum));
			}
			if (Index == 1)
			{
				const auto* Class = CastField<FClassProperty>(Property);
				Test.TestTrue(TEXT("persisted class constraint exact generated actor"), Class && Class->MetaClass == Reference->GeneratedClass && Class->PropertyClass == UClass::StaticClass());
			}
			if (Index == 2)
			{
				const auto* Object = CastField<FObjectPropertyBase>(Property);
				Test.TestTrue(TEXT("persisted object constraint exact generated actor"), Object && !CastField<FClassProperty>(Property) && Object->PropertyClass == Reference->GeneratedClass);
			}
			if (Index == 3)
			{
				const auto* Transform = CastField<FStructProperty>(Property);
				Test.TestTrue(TEXT("persisted Transform exact native identity"), Transform && Transform->Struct == TBaseStructure<FTransform>::Get());
			}
		}
		return true;
	}

	static bool CheckCaller(FAutomationTestBase& Test, UBlueprint* Caller, UBlueprint* Provider,
		UUserDefinedStruct* Struct, const TSharedPtr<FJsonObject>& Manifest)
	{
		if (!CheckSignature(Test, Caller, TEXT("Consume Records"), Struct, true, Provider->GeneratedClass.Get())) return false;
		UEdGraph* GraphValue = Graph(Caller, TEXT("Consume Records"));
		if (!Test.TestNotNull(TEXT("persisted caller graph"), GraphValue)) return false;
		UK2Node_CallFunction* CallNode = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Result = nullptr;
		int32 Calls = 0, Entries = 0, Results = 0;
		for (UEdGraphNode* Node : GraphValue->Nodes)
		{
			if (auto* Found = Cast<UK2Node_CallFunction>(Node)) { CallNode = Found; ++Calls; }
			if (auto* Found = Cast<UK2Node_FunctionEntry>(Node)) { Entry = Found; ++Entries; }
			if (auto* Found = Cast<UK2Node_FunctionResult>(Node)) { Result = Found; ++Results; }
		}
		if (!Test.TestTrue(TEXT("persisted caller has exact entry/call/result nodes"), Calls == 1 && Entries == 1 && Results == 1)) return false;
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Test.TestTrue(TEXT("writer caller node identities available"), Manifest->TryGetObjectField(TEXT("callerNodes"), Nodes))) return false;
		FString Value;
		Test.TestTrue(TEXT("persisted caller entry GUID"), (*Nodes)->TryGetStringField(TEXT("entry"), Value) && Value == Entry->NodeGuid.ToString(EGuidFormats::Digits));
		Test.TestTrue(TEXT("persisted caller call GUID"), (*Nodes)->TryGetStringField(TEXT("call"), Value) && Value == CallNode->NodeGuid.ToString(EGuidFormats::Digits));
		Test.TestTrue(TEXT("persisted caller result GUID"), (*Nodes)->TryGetStringField(TEXT("result"), Value) && Value == Result->NodeGuid.ToString(EGuidFormats::Digits));
		UFunction* EditorFunction = CallNode->GetTargetFunction();
		UFunction* GeneratedFunction = CallNode->FunctionReference.ResolveMember<UFunction>(Caller->GeneratedClass.Get());
		UFunction* ExpectedSkeleton = Provider->SkeletonGeneratedClass ? Provider->SkeletonGeneratedClass->FindFunctionByName(TEXT("Get Records")) : nullptr;
		UFunction* ExpectedGenerated = Provider->GeneratedClass->FindFunctionByName(TEXT("Get Records"));
		Test.TestTrue(TEXT("persisted editor call resolves exact provider skeleton owner"), EditorFunction && EditorFunction == ExpectedSkeleton && EditorFunction->GetOuter() == Provider->SkeletonGeneratedClass.Get());
		Test.TestTrue(TEXT("persisted generated call resolves exact provider generated owner"), GeneratedFunction && GeneratedFunction == ExpectedGenerated && GeneratedFunction->GetOuter() == Provider->GeneratedClass.Get());
		Test.TestTrue(TEXT("persisted external call keeps exact provider generated class"), !CallNode->FunctionReference.IsSelfContext() && CallNode->FunctionReference.GetMemberParentClass() == Provider->GeneratedClass.Get());
		if (!ExpectedGenerated) return false;
		auto Typed = [&](UEdGraphNode* Node, const TCHAR* Name, EEdGraphPinDirection Direction,
			FName Category, UObject* Subtype, EPinContainerType Container)
		{
			const UEdGraphPin* PinValue = Node->FindPin(Name);
			Test.TestTrue(FString(TEXT("persisted exact editor pin ")) + Node->GetClass()->GetName() + TEXT(".") + Name,
				PinValue && PinValue->Direction == Direction && PinValue->PinType.PinCategory == Category
				&& PinValue->PinType.PinSubCategoryObject.Get() == Subtype && PinValue->PinType.ContainerType == Container);
		};
		Typed(Entry, TEXT("Provider"), EGPD_Output, UEdGraphSchema_K2::PC_Object, Provider->GeneratedClass.Get(), EPinContainerType::None);
		Typed(Entry, TEXT("Record"), EGPD_Output, UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::None);
		Typed(Result, TEXT("Records"), EGPD_Input, UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::Array);
		Typed(CallNode, TEXT("Record"), EGPD_Input, UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::None);
		Typed(CallNode, TEXT("Records"), EGPD_Output, UEdGraphSchema_K2::PC_Struct, Struct, EPinContainerType::Array);
		// The engine gives self the first declaring function's authoritative class.
		// Provider itself remains independently constrained to the exact generated class.
		UFunction* Declaration = ExpectedGenerated;
		while (Declaration->GetSuperFunction()) Declaration = Declaration->GetSuperFunction();
		UClass* DeclaringClass = Declaration->GetOuterUClass();
		if (!Test.TestNotNull(TEXT("persisted self declaring class"), DeclaringClass)) return false;
		DeclaringClass = DeclaringClass->GetAuthoritativeClass();
		Typed(CallNode, TEXT("self"), EGPD_Input,
			DeclaringClass->HasAnyClassFlags(CLASS_Interface) ? UEdGraphSchema_K2::PC_Interface : UEdGraphSchema_K2::PC_Object,
			DeclaringClass, EPinContainerType::None);
		auto Linked = [&](UEdGraphNode* From, const TCHAR* FromName, UEdGraphNode* To, const TCHAR* ToName)
		{
			UEdGraphPin* FromPin = From->FindPin(FromName);
			UEdGraphPin* ToPin = To->FindPin(ToName);
			Test.TestTrue(FString(TEXT("persisted exact link ")) + FromName + TEXT(" -> ") + ToName,
				FromPin && ToPin && FromPin->LinkedTo.Num() == 1 && ToPin->LinkedTo.Num() == 1
				&& FromPin->LinkedTo[0] == ToPin && ToPin->LinkedTo[0] == FromPin);
		};
		Linked(Entry, TEXT("Provider"), CallNode, TEXT("self"));
		Linked(Entry, TEXT("Record"), CallNode, TEXT("Record"));
		Linked(Entry, TEXT("then"), CallNode, TEXT("execute"));
		Linked(CallNode, TEXT("then"), Result, TEXT("execute"));
		Linked(CallNode, TEXT("Records"), Result, TEXT("Records"));
		return true;
	}

	static bool RunReader(FAutomationTestBase& Test)
	{
		FContext Context(Test);
		FString Path;
		if (!Context.bValid || !ManifestPath(Test, Context, Path)) return false;
		TSharedPtr<FJsonObject> Manifest;
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (!ReadManifest(Test, Context, Path, Manifest, Rows)) return false;
		const FString OriginalManifestHash = DiskHash(Path);
		for (const TCHAR* Name : ChainNames)
			if (!Test.TestNull(TEXT("fresh reader owned package starts unloaded"), FindPackage(nullptr, *(Context.Root + Name)))) return false;
		FNeverSaveOnCompile NeverSave;
		{
			const auto RequestValue = MakeShared<FJsonObject>();
			RequestValue->SetStringField(TEXT("assetPath"), ObjectPath(Context, ChainNames[0]));
			FObservedCall Rejection;
			Outcome(Test, Call(Test, Serialize(RequestValue)), TEXT("rejected"), false, false, false, nullptr, false, false, false);
			NoSideEffects(Test, Rejection);
			for (const TCHAR* Name : ChainNames)
				if (!Test.TestNull(TEXT("unloaded save rejection did not load dependency chain"), FindPackage(nullptr, *(Context.Root + Name)))) return false;
		}
		int32 LoadTimeCompiles = 0;
		struct FRemoveCompileDelegate
		{
			FDelegateHandle Handle;
			void Remove() { GEditor->OnBlueprintPreCompile().Remove(Handle); Handle.Reset(); }
			~FRemoveCompileDelegate() { Remove(); }
		} CompileCleanup{ GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint*) { ++LoadTimeCompiles; }) };
		FObservedCall Observation;
		TArray<TStrongObjectPtr<UObject>> Loaded;
		// Load the consumer first. Its serialized references must resolve every declared dependency.
		TStrongObjectPtr<UObject> FirstCaller(StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Context, ChainNames[5])));
		if (!Test.TestNotNull(TEXT("fresh reader loads caller first"), FirstCaller.Get())) return false;
		for (const TCHAR* Name : ChainNames)
		{
			UObject* Object = FindObject<UObject>(nullptr, *ObjectPath(Context, Name));
			if (!Test.TestNotNull(TEXT("caller load resolved exact declared dependency"), Object)) return false;
			Test.TestEqual(TEXT("fresh reader exact asset object path"), Object->GetPathName(), ObjectPath(Context, Name));
			Loaded.Emplace(Object);
		}
		CompileCleanup.Remove();
		auto VerifyLoaded = [&](const TCHAR* Stage)
		{
			Test.AddInfo(FString(TEXT("fresh reader semantic stage: ")) + Stage);
			auto* Enum = Cast<UUserDefinedEnum>(Loaded[0].Get());
			auto* Reference = Cast<UBlueprint>(Loaded[1].Get());
			auto* Struct = Cast<UUserDefinedStruct>(Loaded[2].Get());
			auto* Interface = Cast<UBlueprint>(Loaded[3].Get());
			auto* Provider = Cast<UBlueprint>(Loaded[4].Get());
			auto* Caller = Cast<UBlueprint>(Loaded[5].Get());
			if (!Test.TestTrue(TEXT("fresh reader exact asset kinds"), Enum && Reference && Struct && Interface && Provider && Caller)) return false;
			Test.TestEqual(TEXT("persisted enum has two entries and MAX"), Enum->NumEnums(), 3);
			Test.TestEqual(TEXT("persisted enum Idle display label"), Enum->GetDisplayNameTextByIndex(0).ToString(), FString(TEXT("Idle")));
			Test.TestEqual(TEXT("persisted enum Active display label"), Enum->GetDisplayNameTextByIndex(1).ToString(), FString(TEXT("Active")));
			Test.TestEqual(TEXT("persisted enum Idle value"), Enum->GetValueByIndex(0), int64(0));
			Test.TestEqual(TEXT("persisted enum Active value"), Enum->GetValueByIndex(1), int64(1));
			const TArray<TSharedPtr<FJsonValue>>* EnumEntries = nullptr;
			if (!Test.TestTrue(TEXT("persisted enum entries recorded"), Manifest->TryGetArrayField(TEXT("enumEntries"), EnumEntries)
				&& EnumEntries->Num() == Enum->NumEnums())) return false;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				const auto Entry = (*EnumEntries)[Index]->AsObject();
				if (!Test.TestTrue(TEXT("recorded enum entry is an object"), Entry.IsValid())) return false;
				FString Text;
				double Value = 0;
				Test.TestTrue(TEXT("persisted enum internal identity matches writer"), Entry->TryGetStringField(TEXT("internalName"), Text) && Text == Enum->GetNameStringByIndex(Index));
				Test.TestTrue(TEXT("persisted enum display label matches writer"), Entry->TryGetStringField(TEXT("displayName"), Text) && Text == Enum->GetDisplayNameTextByIndex(Index).ToString());
				Test.TestTrue(TEXT("persisted enum value matches writer"), Entry->TryGetNumberField(TEXT("value"), Value) && Value == Enum->GetValueByIndex(Index));
			}
			for (int32 Index : { 1, 3, 4, 5 })
			{
				UBlueprint* BP = CastChecked<UBlueprint>(Loaded[Index].Get());
				FString Guid;
				Test.TestTrue(TEXT("persisted Blueprint GUID matches writer"), Rows[Index]->AsObject()->TryGetStringField(TEXT("blueprintGuid"), Guid) && Guid == BP->GetBlueprintGuid().ToString(EGuidFormats::Digits));
				Test.TestTrue(TEXT("persisted Blueprint has generated and skeleton classes"), BP->GeneratedClass && BP->SkeletonGeneratedClass);
			}
			if (!Reference->GeneratedClass || !Interface->GeneratedClass || !Provider->GeneratedClass || !Caller->GeneratedClass) return false;
			Test.TestTrue(TEXT("persisted reference actor exact native parent"), Reference->ParentClass == AActor::StaticClass());
			Test.TestTrue(TEXT("persisted provider exact SaveGame parent"), Provider->ParentClass == USaveGame::StaticClass());
			Test.TestTrue(TEXT("persisted interface type"), Interface->BlueprintType == BPTYPE_Interface);
			Test.TestTrue(TEXT("persisted provider implements exact interface"), Provider->GeneratedClass->ImplementsInterface(Interface->GeneratedClass));
			CheckStruct(Test, Struct, Enum, Reference, Manifest);
			CheckSignature(Test, Interface, TEXT("Get Records"), Struct, false);
			CheckSignature(Test, Provider, TEXT("Get Records"), Struct, false);
			CheckCaller(Test, Caller, Provider, Struct, Manifest);
			FIntProperty* Count = FindFProperty<FIntProperty>(Provider->GeneratedClass.Get(), TEXT("SavedCount"));
			UObject* Default = Provider->GeneratedClass->GetDefaultObject(false);
			if (Test.TestTrue(TEXT("persisted SaveGame count and CDO exist"), Count && Default))
			{
				Test.TestTrue(TEXT("persisted SaveGame property flag"), Count->HasAnyPropertyFlags(CPF_SaveGame));
				Test.TestEqual(TEXT("fresh reader sees resaved default31 rather than initial7"), Count->GetPropertyValue_InContainer(Default), 31);
			}
			return true;
		};
		if (!VerifyLoaded(TEXT("before explicit compile"))) return false;
		int32 ExplicitCompiles = 0;
		FRemoveCompileDelegate ExplicitCleanup{ GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint*) { ++ExplicitCompiles; }) };
		for (int32 Index : { 1, 3, 4, 5 })
			if (!Compile(Test, CastChecked<UBlueprint>(Loaded[Index].Get()), FString(TEXT("fresh reader explicit ")) + ChainNames[Index])) return false;
		ExplicitCleanup.Remove();
		if (!VerifyLoaded(TEXT("after explicit compile"))) return false;
		Test.TestEqual(TEXT("fresh reader never attempts save"), Observation.Attempts.Num(), 0);
		Test.TestEqual(TEXT("fresh reader never completes save"), Observation.Saved.Num(), 0);
		const auto Reader = MakeShared<FJsonObject>();
		Reader->SetStringField(TEXT("runId"), Context.RunId);
		Reader->SetStringField(TEXT("product"), TEXT("Nwiro"));
		Reader->SetNumberField(TEXT("observedLoadTimeCompiles"), LoadTimeCompiles);
		Reader->SetNumberField(TEXT("observedExplicitCompiles"), ExplicitCompiles);
		Reader->SetNumberField(TEXT("observedSaveAttempts"), Observation.Attempts.Num());
		Reader->SetNumberField(TEXT("observedCompletedSaves"), Observation.Saved.Num());
		Reader->SetStringField(TEXT("writerManifestSha1"), OriginalManifestHash);
		for (int32 Index = 0; Index < Loaded.Num(); ++Index)
		{
			FString ExpectedHash;
			Rows[Index]->AsObject()->TryGetStringField(TEXT("sha1"), ExpectedHash);
			Test.TestEqual(TEXT("reader preserves exact writer file bytes"), DiskHash(FileName(Loaded[Index].Get())), ExpectedHash);
		}
		Test.TestEqual(TEXT("reader preserves immutable writer manifest"), DiskHash(Path), OriginalManifestHash);
		const FString ReaderPath = FPaths::Combine(FPaths::GetPath(Path), TEXT("save-proof-") + Context.RunId + TEXT("-reader.json"));
		if (!Test.TestFalse(TEXT("reader observation artifact initially absent"), IFileManager::Get().FileExists(*ReaderPath))) return false;
		return Test.TestTrue(TEXT("separate reader observations written"), FFileHelper::SaveStringToFile(Serialize(Reader), *ReaderPath));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSaveAssetExtendedOutcomesTest,
	"Nwiro.Pro.Asset.SaveAsset.Production.ExtendedOutcomes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSaveAssetExtendedOutcomesTest::RunTest(const FString&) { return NwiroSaveAssetAcceptance::RunExtendedOutcomes(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSaveAssetObstructedTest,
	"Nwiro.Pro.Asset.SaveAsset.Production.ObstructedSave", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSaveAssetObstructedTest::RunTest(const FString&) { return NwiroSaveAssetAcceptance::RunObstructedSave(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSaveAssetWriterTest,
	"Nwiro.Pro.Asset.SaveAsset.Persistence.Writer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSaveAssetWriterTest::RunTest(const FString&) { return NwiroSaveAssetAcceptance::RunWriter(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSaveAssetReaderTest,
	"Nwiro.Pro.Asset.SaveAsset.Persistence.Reader", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSaveAssetReaderTest::RunTest(const FString&) { return NwiroSaveAssetAcceptance::RunReader(*this); }
