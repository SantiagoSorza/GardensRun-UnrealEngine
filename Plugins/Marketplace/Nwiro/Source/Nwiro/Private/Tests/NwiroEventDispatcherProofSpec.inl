// Copyright 2026 Nwiro. All Rights Reserved.
// Production-path contracts for Blueprint Event Dispatcher creation/removal.

#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MakeArray.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "NwiroToolDispatch.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace NwiroEventDispatcherProofPrivate
{
	static UBlueprint* MakeBlueprint(const TCHAR* Prefix)
	{
		const FName Name(*FString::Printf(
			TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), Name, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
			TEXT("NwiroEventDispatcherContract"));
	}

	static TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Json);
		return Json;
	}

	static FString Serialize(const TSharedRef<FJsonObject>& Json)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Json, Writer);
		return Text;
	}

	static bool Succeeded(const FString& Text)
	{
		const TSharedPtr<FJsonObject> Json = ParseJson(Text);
		return Json.IsValid()
			&& Json->HasTypedField<EJson::Boolean>(TEXT("success"))
			&& Json->GetBoolField(TEXT("success"));
	}

	static TSharedPtr<FJsonValue> StringParam(const TCHAR* Name, const TCHAR* Type)
	{
		const TSharedRef<FJsonObject> Param = MakeShared<FJsonObject>();
		Param->SetStringField(TEXT("name"), Name);
		Param->SetStringField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Param);
	}

	static TSharedPtr<FJsonValue> StructuredParam(
		const TCHAR* Name, const TCHAR* Base, const TCHAR* Subtype, const TCHAR* Container)
	{
		const TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
		Type->SetStringField(TEXT("base"), Base);
		if (Subtype && *Subtype) Type->SetStringField(TEXT("subtype"), Subtype);
		if (Container && *Container) Type->SetStringField(TEXT("container"), Container);
		const TSharedRef<FJsonObject> Param = MakeShared<FJsonObject>();
		Param->SetStringField(TEXT("name"), Name);
		Param->SetObjectField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Param);
	}

	static FString StandaloneCommand(
		UBlueprint* BP, const FString& Name,
		const TArray<TSharedPtr<FJsonValue>>& Params)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Root->SetStringField(TEXT("name"), Name);
		Root->SetArrayField(TEXT("params"), Params);
		return Serialize(Root);
	}

	static FString EditAddCommandWithParams(
		UBlueprint* BP, const FName Name,
		const TArray<TSharedPtr<FJsonValue>>& Params,
		const bool bCompile)
	{
		const TSharedRef<FJsonObject> Dispatcher = MakeShared<FJsonObject>();
		Dispatcher->SetStringField(TEXT("name"), Name.ToString());
		Dispatcher->SetArrayField(TEXT("params"), Params);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Root->SetStringField(TEXT("graph"), TEXT("EventGraph"));
		Root->SetArrayField(
			TEXT("add_event_dispatchers"),
			{MakeShared<FJsonValueObject>(Dispatcher)});
		Root->SetBoolField(TEXT("compile"), bCompile);
		return Serialize(Root);
	}

	static FString EditAddCommand(
		UBlueprint* BP, const FName Name, const bool bCompile)
	{
		return EditAddCommandWithParams(
			BP, Name, {StringParam(TEXT("Amount"), TEXT("float"))}, bCompile);
	}

	static FString RemoveCommand(UBlueprint* BP, const FName Name)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Root->SetStringField(TEXT("graph"), TEXT("EventGraph"));
		Root->SetArrayField(
			TEXT("remove_variables"),
			{MakeShared<FJsonValueString>(Name.ToString())});
		Root->SetBoolField(TEXT("compile"), true);
		return Serialize(Root);
	}

	static bool HasMember(const UBlueprint* BP, const FName Name)
	{
		return BP && BP->NewVariables.ContainsByPredicate(
			[Name](const FBPVariableDescription& Var) { return Var.VarName == Name; });
	}

	static UEdGraph* FindSignatureGraph(const UBlueprint* BP, const FName Name)
	{
		if (!BP) return nullptr;
		for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
		{
			if (Graph && Graph->GetFName() == Name) return Graph;
		}
		return nullptr;
	}

	static UK2Node_FunctionEntry* FindEntry(UEdGraph* Graph)
	{
		if (!Graph) return nullptr;
		TArray<UK2Node_FunctionEntry*> Entries;
		Graph->GetNodesOfClass(Entries);
		return Entries.Num() == 1 ? Entries[0] : nullptr;
	}

	static UEdGraphPin* FindPin(
		const UEdGraphNode* Node, const FName Name, const EEdGraphPinDirection Direction)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == Name && Pin->Direction == Direction) return Pin;
		}
		return nullptr;
	}

	static FEdGraphPinType FloatType()
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Real;
		Type.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return Type;
	}

	static FEdGraphPinType ActorType(
		const EPinContainerType Container = EPinContainerType::None)
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Object;
		Type.PinSubCategoryObject = AActor::StaticClass();
		Type.ContainerType = Container;
		return Type;
	}

	static FEdGraphPinType NameArrayType()
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Name;
		Type.ContainerType = EPinContainerType::Array;
		return Type;
	}

	static bool PinMatches(
		const UEdGraphPin* Pin, const FEdGraphPinType& ExpectedType)
	{
		return Pin && Pin->PinType == ExpectedType;
	}

	static UK2Node_CallDelegate* AddCall(UBlueprint* BP, const FName Name)
	{
		if (!BP || BP->UbergraphPages.IsEmpty() || !BP->UbergraphPages[0]) return nullptr;
		UEdGraph* EventGraph = BP->UbergraphPages[0];
		FGraphNodeCreator<UK2Node_CallDelegate> Creator(*EventGraph);
		UK2Node_CallDelegate* Call = Creator.CreateNode();
		Call->DelegateReference.SetSelfMember(Name);
		Creator.Finalize();

		FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*EventGraph);
		UK2Node_CustomEvent* Event = EventCreator.CreateNode();
		Event->CustomFunctionName =
			FName(*(FString(TEXT("Trigger_")) + Name.ToString()));
		EventCreator.Finalize();

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (!Event->GetThenPin()
			|| !Call->GetExecPin()
			|| !Schema->TryCreateConnection(
				Event->GetThenPin(), Call->GetExecPin()))
		{
			Event->DestroyNode();
			Call->DestroyNode();
			return nullptr;
		}
		return Call;
	}

	static FMulticastDelegateProperty* FindProperty(UClass* Class, const FName Name)
	{
		FMulticastDelegateProperty* Property =
			Class ? FindFProperty<FMulticastDelegateProperty>(Class, Name) : nullptr;
		return Property && Property->GetOwner<UClass>() == Class ? Property : nullptr;
	}

	static bool PayloadTypeMatches(
		const FEdGraphPinType& ActualType,
		const FEdGraphPinType& ExpectedType)
	{
		// Callable/reflected array and set pins may carry ABI-only const/reference
		// flags. The Blueprint-visible payload contract is the semantic shape.
		return ActualType.PinCategory == ExpectedType.PinCategory
			&& ActualType.PinSubCategory == ExpectedType.PinSubCategory
			&& ActualType.PinSubCategoryObject == ExpectedType.PinSubCategoryObject
			&& ActualType.ContainerType == ExpectedType.ContainerType;
	}

	static bool SignatureParamMatches(
		UFunction* Signature, const FName Name,
		const FEdGraphPinType& ExpectedType)
	{
		FProperty* Parameter =
			Signature ? FindFProperty<FProperty>(Signature, Name) : nullptr;
		if (!Parameter || Parameter->GetOwner<UFunction>() != Signature
			|| !Parameter->HasAnyPropertyFlags(CPF_Parm))
		{
			return false;
		}
		FEdGraphPinType ReflectedType;
		return GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(
			Parameter, ReflectedType)
			&& PayloadTypeMatches(ReflectedType, ExpectedType);
	}

	static TSharedPtr<FJsonObject> FindDispatcherJson(
		const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, const FName Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(Field, Values)) return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			if (Item.IsValid()
				&& Item->GetStringField(TEXT("name")).Equals(
					Name.ToString(), ESearchCase::IgnoreCase))
			{
				return Item;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> SchemaObjectField(
		const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Result = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Result)
			? *Result : nullptr;
	}

	static bool SchemaHasType(
		const TSharedPtr<FJsonObject>& Schema, const TCHAR* Expected)
	{
		FString Type;
		return Schema.IsValid()
			&& Schema->TryGetStringField(TEXT("type"), Type)
			&& Type == Expected;
	}

	static bool SchemaRequires(
		const TSharedPtr<FJsonObject>& Schema, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (!Schema.IsValid()
			|| !Schema->TryGetArrayField(TEXT("required"), Required))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Required)
		{
			if (Value.IsValid() && Value->Type == EJson::String
				&& Value->AsString() == Field)
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> FindSchemaBranchByType(
		const TSharedPtr<FJsonObject>& Schema,
		const TCHAR* BranchArrayField,
		const TCHAR* ExpectedType,
		int32* OutBranchCount = nullptr)
	{
		const TArray<TSharedPtr<FJsonValue>>* Branches = nullptr;
		if (!Schema.IsValid()
			|| !Schema->TryGetArrayField(BranchArrayField, Branches))
		{
			if (OutBranchCount) *OutBranchCount = 0;
			return nullptr;
		}
		if (OutBranchCount) *OutBranchCount = Branches->Num();
		for (const TSharedPtr<FJsonValue>& Value : *Branches)
		{
			const TSharedPtr<FJsonObject> Branch =
				Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject() : nullptr;
			if (SchemaHasType(Branch, ExpectedType))
			{
				return Branch;
			}
		}
		return nullptr;
	}

	static bool ValidateParamArraySchema(
		const TSharedPtr<FJsonObject>& ParamsSchema,
		const FString& Context,
		FString& OutError)
	{
		auto Fail = [&](const FString& Detail)
		{
			OutError = Context + TEXT(": ") + Detail;
			return false;
		};
		if (!SchemaHasType(ParamsSchema, TEXT("array")))
			return Fail(TEXT("params must advertise an array"));
		const TSharedPtr<FJsonObject> ParamItem =
			SchemaObjectField(ParamsSchema, TEXT("items"));
		if (!SchemaHasType(ParamItem, TEXT("object")))
			return Fail(TEXT("params.items must advertise an object"));
		if (!SchemaRequires(ParamItem, TEXT("name"))
			|| !SchemaRequires(ParamItem, TEXT("type")))
		{
			return Fail(TEXT("each parameter must require name and type"));
		}

		const TSharedPtr<FJsonObject> ParamProperties =
			SchemaObjectField(ParamItem, TEXT("properties"));
		if (!ParamProperties.IsValid()
			|| ParamProperties->HasField(TEXT("subtype"))
			|| ParamProperties->HasField(TEXT("container")))
		{
			return Fail(TEXT("subtype/container must not be advertised beside parameter type"));
		}
		if (!SchemaHasType(
			SchemaObjectField(ParamProperties, TEXT("name")), TEXT("string")))
		{
			return Fail(TEXT("parameter name must advertise a string"));
		}

		const TSharedPtr<FJsonObject> TypeSchema =
			SchemaObjectField(ParamProperties, TEXT("type"));
		int32 TypeBranchCount = 0;
		const TSharedPtr<FJsonObject> StringType =
			FindSchemaBranchByType(
				TypeSchema, TEXT("oneOf"), TEXT("string"), &TypeBranchCount);
		const TSharedPtr<FJsonObject> StructuredType =
			FindSchemaBranchByType(
				TypeSchema, TEXT("oneOf"), TEXT("object"));
		if (TypeBranchCount != 2 || !StringType.IsValid()
			|| !StructuredType.IsValid())
		{
			return Fail(TEXT("type must expose exactly string and structured-object forms"));
		}
		if (!SchemaRequires(StructuredType, TEXT("base")))
			return Fail(TEXT("structured type must require base"));

		const TSharedPtr<FJsonObject> StructuredProperties =
			SchemaObjectField(StructuredType, TEXT("properties"));
		if (!StructuredProperties.IsValid()
			|| StructuredProperties->Values.Num() != 3
			|| !SchemaHasType(
				SchemaObjectField(StructuredProperties, TEXT("base")), TEXT("string"))
			|| !SchemaHasType(
				SchemaObjectField(StructuredProperties, TEXT("subtype")), TEXT("string"))
			|| !SchemaHasType(
				SchemaObjectField(StructuredProperties, TEXT("container")), TEXT("string")))
		{
			return Fail(TEXT("structured type must expose only string base/subtype/container"));
		}

		const TSharedPtr<FJsonObject> ContainerSchema =
			SchemaObjectField(StructuredProperties, TEXT("container"));
		const TArray<TSharedPtr<FJsonValue>>* Containers = nullptr;
		if (!ContainerSchema.IsValid()
			|| !ContainerSchema->TryGetArrayField(TEXT("enum"), Containers)
			|| Containers->Num() != 3)
		{
			return Fail(TEXT("container enum must contain exactly three values"));
		}
		TSet<FString> ContainerNames;
		for (const TSharedPtr<FJsonValue>& Value : *Containers)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
				return Fail(TEXT("container enum values must be strings"));
			ContainerNames.Add(Value->AsString());
		}
		if (!ContainerNames.Contains(TEXT("none"))
			|| !ContainerNames.Contains(TEXT("array"))
			|| !ContainerNames.Contains(TEXT("set"))
			|| ContainerNames.Contains(TEXT("map"))
			|| ContainerNames.Contains(TEXT("single"))
			|| ContainerNames.Contains(TEXT("scalar")))
		{
			return Fail(TEXT("container enum must be exactly none/array/set"));
		}
		return true;
	}

	static bool ValidateDispatcherArraySchema(
		const TSharedPtr<FJsonObject>& DispatcherArray,
		const bool bRequireNonEmpty,
		const FString& Context,
		FString& OutError)
	{
		auto Fail = [&](const FString& Detail)
		{
			OutError = Context + TEXT(": ") + Detail;
			return false;
		};
		if (!SchemaHasType(DispatcherArray, TEXT("array")))
			return Fail(TEXT("dispatcher batch must advertise an array"));
		if (bRequireNonEmpty)
		{
			double MinItems = 0.0;
			if (!DispatcherArray->TryGetNumberField(TEXT("minItems"), MinItems)
				|| MinItems < 1.0)
			{
				return Fail(TEXT("standalone dispatcher batch must require at least one item"));
			}
		}
		const TSharedPtr<FJsonObject> DispatcherItem =
			SchemaObjectField(DispatcherArray, TEXT("items"));
		if (!SchemaHasType(DispatcherItem, TEXT("object"))
			|| !SchemaRequires(DispatcherItem, TEXT("name")))
		{
			return Fail(TEXT("dispatcher item must be an object requiring name"));
		}
		const TSharedPtr<FJsonObject> DispatcherProperties =
			SchemaObjectField(DispatcherItem, TEXT("properties"));
		if (!SchemaHasType(
			SchemaObjectField(DispatcherProperties, TEXT("name")), TEXT("string")))
		{
			return Fail(TEXT("dispatcher name must advertise a string"));
		}
		return ValidateParamArraySchema(
			SchemaObjectField(DispatcherProperties, TEXT("params")),
			Context + TEXT(".params"), OutError);
	}

	static TSharedPtr<FJsonObject> FindToolDefinition(
		const TArray<TSharedPtr<FJsonValue>>& Definitions,
		const TCHAR* Name)
	{
		for (const TSharedPtr<FJsonValue>& Value : Definitions)
		{
			const TSharedPtr<FJsonObject> Tool =
				Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject() : nullptr;
			FString ToolName;
			if (Tool.IsValid()
				&& Tool->TryGetStringField(TEXT("name"), ToolName)
				&& ToolName == Name)
			{
				return Tool;
			}
		}
		return nullptr;
	}

	static bool ResolveLocalSchemaRef(
		const TSharedPtr<FJsonObject>& SchemaRoot,
		const FString& Ref)
	{
		if (!SchemaRoot.IsValid() || !Ref.StartsWith(TEXT("#/")))
			return false;
		TSharedPtr<FJsonValue> Current =
			MakeShared<FJsonValueObject>(SchemaRoot.ToSharedRef());
		TArray<FString> Tokens;
		Ref.Mid(2).ParseIntoArray(Tokens, TEXT("/"), false);
		for (FString Token : Tokens)
		{
			Token.ReplaceInline(TEXT("~1"), TEXT("/"));
			Token.ReplaceInline(TEXT("~0"), TEXT("~"));
			if (!Current.IsValid()) return false;
			if (Current->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Current->AsObject();
				const TSharedPtr<FJsonValue> Next =
					Object.IsValid() ? Object->TryGetField(Token) : nullptr;
				if (!Next.IsValid()) return false;
				Current = Next;
			}
			else if (Current->Type == EJson::Array)
			{
				int32 Index = INDEX_NONE;
				if (!LexTryParseString(Index, *Token))
					return false;
				const TArray<TSharedPtr<FJsonValue>>& Values = Current->AsArray();
				if (!Values.IsValidIndex(Index) || !Values[Index].IsValid())
					return false;
				Current = Values[Index];
			}
			else
			{
				return false;
			}
		}
		return Current.IsValid();
	}

	static void ValidateLocalSchemaRefsRecursive(
		const TSharedPtr<FJsonValue>& Value,
		const TSharedPtr<FJsonObject>& SchemaRoot,
		const FString& Path,
		int32& OutRefCount,
		TArray<FString>& OutErrors)
	{
		if (!Value.IsValid()) return;
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			FString Ref;
			if (Object.IsValid()
				&& Object->TryGetStringField(TEXT("$ref"), Ref)
				&& Ref.StartsWith(TEXT("#/")))
			{
				++OutRefCount;
				if (!ResolveLocalSchemaRef(SchemaRoot, Ref))
				{
					OutErrors.Add(FString::Printf(
						TEXT("%s has unresolved local ref '%s'"), *Path, *Ref));
				}
			}
			if (!Object.IsValid()) return;
			for (const auto& Pair : Object->Values)
			{
				ValidateLocalSchemaRefsRecursive(
					Pair.Value, SchemaRoot, Path + TEXT("/") + Pair.Key,
					OutRefCount, OutErrors);
			}
		}
		else if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				ValidateLocalSchemaRefsRecursive(
					Values[Index], SchemaRoot,
					FString::Printf(TEXT("%s/%d"), *Path, Index),
					OutRefCount, OutErrors);
			}
		}
	}

	static const FString& PersistencePackagePath()
	{
		static const FString Path(
			TEXT("/Game/NwiroAutomation/BP_NwiroProEventDispatcherPersistenceProof_A71E4C92"));
		return Path;
	}

	static const FString& PersistenceAssetName()
	{
		static const FString Name(
			TEXT("BP_NwiroProEventDispatcherPersistenceProof_A71E4C92"));
		return Name;
	}

	static const FString& PersistenceOwnershipMarker()
	{
		static const FString Marker(
			TEXT("NWIRO_PRO_EVENT_DISPATCHER_PERSISTENCE_PROOF_A71E4C92_V1"));
		return Marker;
	}

	static FString PersistenceObjectPath()
	{
		return PersistencePackagePath() + TEXT(".") + PersistenceAssetName();
	}

	static UBlueprint* LoadPersistenceBlueprint()
	{
		return LoadObject<UBlueprint>(nullptr, *PersistenceObjectPath());
	}

	static bool IsOwnedPersistenceAsset(const UBlueprint* BP)
	{
		return BP && BP->GetPathName() == PersistenceObjectPath()
			&& BP->BlueprintDescription == PersistenceOwnershipMarker();
	}

	static bool DeleteOwnedPersistenceAsset(FString& OutError)
	{
		if (!UEditorAssetLibrary::DoesAssetExist(PersistencePackagePath()))
			return true;
		UBlueprint* Existing = LoadPersistenceBlueprint();
		if (!IsOwnedPersistenceAsset(Existing))
		{
			OutError = FString::Printf(
				TEXT("refusing to delete unowned asset at reserved proof path '%s'"),
				*PersistencePackagePath());
			return false;
		}
		if (!UEditorAssetLibrary::DeleteAsset(PersistencePackagePath()))
		{
			OutError = FString::Printf(
				TEXT("failed to delete owned proof asset '%s'"),
				*PersistencePackagePath());
			return false;
		}
		if (UEditorAssetLibrary::DoesAssetExist(PersistencePackagePath()))
		{
			OutError = TEXT("owned proof asset remains registered after deletion");
			return false;
		}
		return true;
	}

	static UBlueprint* CreatePersistenceBlueprint()
	{
		UPackage* Package = CreatePackage(*PersistencePackagePath());
		if (!Package) return nullptr;
		Package->FullyLoad();
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*PersistenceAssetName()),
			BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			TEXT("NwiroProDispatcherPersistence"));
		if (BP)
		{
			BP->BlueprintDescription = PersistenceOwnershipMarker();
			FAssetRegistryModule::AssetCreated(BP);
			BP->MarkPackageDirty();
		}
		return BP;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherAdvertisedSchemaContractSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Schema.RuntimeAdvertisedContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherAdvertisedSchemaContractSpec::RunTest(
	const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	const FString DefinitionsJson =
		FNwiroToolDispatch::GetEnabledToolDefinitionsJson();
	TArray<TSharedPtr<FJsonValue>> Definitions;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(DefinitionsJson);
	const bool bParsed =
		FJsonSerializer::Deserialize(Reader, Definitions)
		&& !Definitions.IsEmpty();
	TestTrue(TEXT("Complete advertised tool definitions parse as a nonempty JSON array"),
		bParsed);
	if (!bParsed) return false;

	const TSharedPtr<FJsonObject> StandaloneTool =
		FindToolDefinition(Definitions, TEXT("create_event_dispatcher"));
	const TSharedPtr<FJsonObject> EditTool =
		FindToolDefinition(Definitions, TEXT("edit_blueprint"));
	TestTrue(TEXT("Advertised tools include create_event_dispatcher"),
		StandaloneTool.IsValid());
	TestTrue(TEXT("Advertised tools include edit_blueprint"), EditTool.IsValid());
	if (!StandaloneTool.IsValid() || !EditTool.IsValid()) return false;

	const TSharedPtr<FJsonObject> StandaloneSchema =
		SchemaObjectField(StandaloneTool, TEXT("inputSchema"));
	const TSharedPtr<FJsonObject> StandaloneProperties =
		SchemaObjectField(StandaloneSchema, TEXT("properties"));
	TestTrue(TEXT("Standalone schema is an object requiring blueprint"),
		SchemaHasType(StandaloneSchema, TEXT("object"))
		&& SchemaRequires(StandaloneSchema, TEXT("blueprint")));
	TestTrue(TEXT("Standalone dispatcher name advertises a string"),
		SchemaHasType(
			SchemaObjectField(StandaloneProperties, TEXT("name")), TEXT("string")));

	const TArray<TSharedPtr<FJsonValue>>* StandaloneAnyOf = nullptr;
	bool bStandaloneNameAlternative = false;
	bool bStandaloneBatchAlternative = false;
	if (StandaloneSchema.IsValid()
		&& StandaloneSchema->TryGetArrayField(TEXT("anyOf"), StandaloneAnyOf))
	{
		for (const TSharedPtr<FJsonValue>& Value : *StandaloneAnyOf)
		{
			const TSharedPtr<FJsonObject> Alternative =
				Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject() : nullptr;
			bStandaloneNameAlternative |=
				SchemaRequires(Alternative, TEXT("name"));
			bStandaloneBatchAlternative |=
				SchemaRequires(Alternative, TEXT("dispatchers"));
		}
	}
	TestTrue(TEXT("Standalone schema requires either name or dispatchers"),
		bStandaloneNameAlternative && bStandaloneBatchAlternative);

	FString GrammarError;
	const bool bStandaloneParamsValid = ValidateParamArraySchema(
		SchemaObjectField(StandaloneProperties, TEXT("params")),
		TEXT("create_event_dispatcher.params"), GrammarError);
	TestTrue(FString::Printf(
		TEXT("Standalone params expose the strict grammar%s%s"),
		GrammarError.IsEmpty() ? TEXT("") : TEXT(": "),
		*GrammarError), bStandaloneParamsValid);

	GrammarError.Reset();
	const bool bStandaloneBatchValid = ValidateDispatcherArraySchema(
		SchemaObjectField(StandaloneProperties, TEXT("dispatchers")),
		true, TEXT("create_event_dispatcher.dispatchers"), GrammarError);
	TestTrue(FString::Printf(
		TEXT("Standalone batch exposes the strict dispatcher grammar%s%s"),
		GrammarError.IsEmpty() ? TEXT("") : TEXT(": "),
		*GrammarError), bStandaloneBatchValid);

	const TSharedPtr<FJsonObject> EditSchema =
		SchemaObjectField(EditTool, TEXT("inputSchema"));
	const TSharedPtr<FJsonObject> EditProperties =
		SchemaObjectField(EditSchema, TEXT("properties"));
	TestTrue(TEXT("edit_blueprint schema is an object requiring blueprint"),
		SchemaHasType(EditSchema, TEXT("object"))
		&& SchemaRequires(EditSchema, TEXT("blueprint")));

	const TSharedPtr<FJsonObject> EditDispatcherSchema =
		SchemaObjectField(EditProperties, TEXT("add_event_dispatchers"));
	int32 EditBranchCount = 0;
	const TSharedPtr<FJsonObject> EditArrayBranch =
		FindSchemaBranchByType(
			EditDispatcherSchema, TEXT("oneOf"), TEXT("array"), &EditBranchCount);
	const TSharedPtr<FJsonObject> EditStringBranch =
		FindSchemaBranchByType(
			EditDispatcherSchema, TEXT("oneOf"), TEXT("string"));
	TestTrue(TEXT("edit_blueprint add_event_dispatchers exposes array plus string compatibility forms"),
		EditBranchCount == 2
		&& EditArrayBranch.IsValid() && EditStringBranch.IsValid());

	GrammarError.Reset();
	const bool bEditDispatcherValid = ValidateDispatcherArraySchema(
		EditArrayBranch, false,
		TEXT("edit_blueprint.add_event_dispatchers"), GrammarError);
	TestTrue(FString::Printf(
		TEXT("edit_blueprint dispatcher batch exposes the strict grammar%s%s"),
		GrammarError.IsEmpty() ? TEXT("") : TEXT(": "),
		*GrammarError), bEditDispatcherValid);

	int32 LocalRefCount = 0;
	TArray<FString> RefErrors;
	for (const TSharedPtr<FJsonValue>& DefinitionValue : Definitions)
	{
		const TSharedPtr<FJsonObject> Tool =
			DefinitionValue.IsValid() && DefinitionValue->Type == EJson::Object
				? DefinitionValue->AsObject() : nullptr;
		const TSharedPtr<FJsonObject> InputSchema =
			SchemaObjectField(Tool, TEXT("inputSchema"));
		if (!InputSchema.IsValid()) continue;
		FString ToolName(TEXT("<unnamed>"));
		Tool->TryGetStringField(TEXT("name"), ToolName);
		ValidateLocalSchemaRefsRecursive(
			MakeShared<FJsonValueObject>(InputSchema.ToSharedRef()),
			InputSchema, ToolName + TEXT("/inputSchema"),
			LocalRefCount, RefErrors);
	}
	TestTrue(FString::Printf(
		TEXT("Every one of %d advertised local schema refs resolves%s%s"),
		LocalRefCount,
		RefErrors.IsEmpty() ? TEXT("") : TEXT(": "),
		*FString::Join(RefErrors, TEXT("; "))),
		RefErrors.IsEmpty());
	// Fix B (2026-09), mirrored from the Integration Kit spec: local refs are
	// inlined in the registry because Bionic / LM Studio's llama.cpp engine
	// rejects a tool schema that carries `definitions` (HTTP 400 on every chat
	// turn). The walk above still reports a ref's path if one sneaks back in.
	TestEqual(
		TEXT("Advertised schemas carry no local refs (inlined for llama.cpp backends)"),
		LocalRefCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherUnrelatedCompileErrorContractSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Production.UnrelatedCompileErrorRetainsSkeleton",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherUnrelatedCompileErrorContractSpec::RunTest(
	const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	UBlueprint* BP = MakeBlueprint(TEXT("NwiroDispatcherUnrelatedCompileError"));
	TestNotNull(TEXT("Transient Blueprint is created"), BP);
	if (!BP) return false;

	const FName MissingName(TEXT("MissingUnrelatedDispatcher"));
	const FString ExpectedCompilerError(
		TEXT("Could not find an event-dispatcher named \"MissingUnrelatedDispatcher\""));
	AddExpectedError(
		ExpectedCompilerError,
		EAutomationExpectedErrorFlags::Contains, 0);
	UK2Node_CallDelegate* BrokenCall = AddCall(BP, MissingName);
	TestNotNull(TEXT("Unrelated malformed Call Delegate node is placed"), BrokenCall);
	if (!BrokenCall) return false;

	const FName NewName(TEXT("RetainedTypedDispatcher"));
	const TSharedPtr<FJsonObject> Response = ParseJson(
		FNwiroBlueprintTools::CreateEventDispatcher(
			StandaloneCommand(BP, NewName.ToString(), {
				StringParam(TEXT("Amount"), TEXT("float"))})));
	TestTrue(TEXT("Dispatcher-local creation succeeds despite unrelated compile error"),
		Response.IsValid() && Response->GetBoolField(TEXT("success")));
	if (!Response.IsValid()) return false;
	TestTrue(TEXT("Production path attempted the whole Blueprint compile"),
		Response->GetBoolField(TEXT("compiled")));
	TestEqual(TEXT("Whole Blueprint compile error is reported separately"),
		Response->GetStringField(TEXT("compileStatus")), FString(TEXT("error")));
	TestEqual(TEXT("Validation remains skeleton-only after aborted generated compile"),
		Response->GetStringField(TEXT("validationLevel")), FString(TEXT("skeleton")));
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	const bool bHasDiagnostics =
		Response->TryGetArrayField(TEXT("compileDiagnostics"), Diagnostics)
		&& Diagnostics && !Diagnostics->IsEmpty();
	TestTrue(TEXT("Compiler diagnostics preserve the unrelated failure"),
		bHasDiagnostics);
	bool bHasExpectedDiagnostic = false;
	if (Diagnostics)
	{
		for (const TSharedPtr<FJsonValue>& Diagnostic : *Diagnostics)
		{
			bHasExpectedDiagnostic |= Diagnostic.IsValid()
				&& Diagnostic->Type == EJson::String
				&& Diagnostic->AsString().Contains(ExpectedCompilerError);
		}
	}
	TestTrue(TEXT("Diagnostics contain the UE 5.7 missing-dispatcher error"),
		bHasExpectedDiagnostic);

	const TSharedPtr<FJsonObject> Dispatcher =
		FindDispatcherJson(Response, TEXT("dispatchers"), NewName);
	TestTrue(TEXT("Response retains the newly created dispatcher"),
		Dispatcher.IsValid()
		&& Dispatcher->GetBoolField(TEXT("created"))
		&& !Dispatcher->GetBoolField(TEXT("rolledBack")));
	TestTrue(TEXT("Response reports its local skeleton invariant"),
		Dispatcher.IsValid()
		&& Dispatcher->GetBoolField(TEXT("signatureGraphPresent"))
		&& Dispatcher->GetBoolField(TEXT("skeletonSignatureValid")));
	TestTrue(TEXT("Generated validity remains explicitly unvalidated"),
		Dispatcher.IsValid()
		&& Dispatcher->TryGetField(TEXT("generatedSignatureValid"))
		&& Dispatcher->TryGetField(TEXT("generatedSignatureValid"))->Type == EJson::Null);

	TestTrue(TEXT("New multicast member remains in the Blueprint"),
		HasMember(BP, NewName));
	UEdGraph* SignatureGraph = FindSignatureGraph(BP, NewName);
	TestNotNull(TEXT("New signature graph remains registered"), SignatureGraph);
	UK2Node_FunctionEntry* Entry = FindEntry(SignatureGraph);
	TestNotNull(TEXT("New signature graph retains its editable entry"), Entry);
	TestTrue(TEXT("New skeleton graph retains the typed Amount pin"),
		FindPin(Entry, TEXT("Amount"), EGPD_Output)
		&& FindPin(Entry, TEXT("Amount"), EGPD_Output)->PinType == FloatType());

	FMulticastDelegateProperty* SkeletonProperty =
		FindProperty(BP->SkeletonGeneratedClass, NewName);
	TestNotNull(TEXT("Skeleton multicast delegate property remains"),
		SkeletonProperty);
	UFunction* SkeletonSignature =
		SkeletonProperty ? SkeletonProperty->SignatureFunction.Get() : nullptr;
	TestNotNull(TEXT("Skeleton signature function remains"), SkeletonSignature);
	FProperty* ReflectedAmount =
		SkeletonSignature
			? FindFProperty<FProperty>(SkeletonSignature, TEXT("Amount")) : nullptr;
	TestNotNull(TEXT("Skeleton signature retains reflected Amount parameter"),
		ReflectedAmount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherProductionContractSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Production.TypedSignatureAndCall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherProductionContractSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	UBlueprint* BP = MakeBlueprint(TEXT("NwiroDispatcherProduction"));
	TestNotNull(TEXT("Transient Blueprint is created"), BP);
	if (!BP) return false;

	const FName Name(TEXT("TypedDispatcher"));
	const FString ResponseText = FNwiroBlueprintTools::CreateEventDispatcher(
		StandaloneCommand(BP, Name.ToString(), {
			StringParam(TEXT("Amount"), TEXT("float")),
			StringParam(TEXT("Context"), TEXT("Actor")),
			StructuredParam(TEXT("Tags"), TEXT("name"), TEXT(""), TEXT("array"))}));
	const TSharedPtr<FJsonObject> Response = ParseJson(ResponseText);
	AddInfo(FString::Printf(
		TEXT("create_event_dispatcher response: %s"), *ResponseText));
	const bool bResponseSucceeded =
		Response.IsValid() && Response->GetBoolField(TEXT("success"));
	TestTrue(TEXT("Production path reports success"), bResponseSucceeded);
	if (!bResponseSucceeded) return false;
	TestEqual(TEXT("Production path reports generated validation"),
		Response->GetStringField(TEXT("validationLevel")), FString(TEXT("generated")));
	TestEqual(TEXT("Production path reports clean compile"),
		Response->GetStringField(TEXT("compileStatus")), FString(TEXT("success")));

	UEdGraph* Graph = FindSignatureGraph(BP, Name);
	TestNotNull(TEXT("Signature graph is registered"), Graph);
	UK2Node_FunctionEntry* Entry = FindEntry(Graph);
	TestNotNull(TEXT("Signature graph has one entry"), Entry);
	TestEqual(TEXT("Signature entry preserves ordered parameter count"),
		Entry ? Entry->UserDefinedPins.Num() : -1, 3);
	if (Entry && Entry->UserDefinedPins.Num() == 3)
	{
		TestEqual(TEXT("First parameter remains Amount"),
			Entry->UserDefinedPins[0]->PinName, FName(TEXT("Amount")));
		TestEqual(TEXT("Second parameter remains Context"),
			Entry->UserDefinedPins[1]->PinName, FName(TEXT("Context")));
		TestEqual(TEXT("Third parameter remains Tags"),
			Entry->UserDefinedPins[2]->PinName, FName(TEXT("Tags")));
		TestEqual(TEXT("Tags remains an array container"),
			Entry->UserDefinedPins[2]->PinType.ContainerType,
			EPinContainerType::Array);
	}
	TestTrue(TEXT("Amount entry pin preserves float type"),
		FindPin(Entry, TEXT("Amount"), EGPD_Output)
		&& FindPin(Entry, TEXT("Amount"), EGPD_Output)->PinType == FloatType());
	TestTrue(TEXT("Context entry pin preserves Actor type"),
		FindPin(Entry, TEXT("Context"), EGPD_Output)
		&& FindPin(Entry, TEXT("Context"), EGPD_Output)->PinType == ActorType());
	TestTrue(TEXT("Tags entry pin preserves name-array type"),
		FindPin(Entry, TEXT("Tags"), EGPD_Output)
		&& FindPin(Entry, TEXT("Tags"), EGPD_Output)->PinType == NameArrayType());

	UK2Node_CallDelegate* Call = AddCall(BP, Name);
	TestNotNull(TEXT("Call Delegate node is placeable"), Call);
	if (!Call) return false;
	TestTrue(TEXT("Call exposes Amount payload"),
		FindPin(Call, TEXT("Amount"), EGPD_Input)
		&& FindPin(Call, TEXT("Amount"), EGPD_Input)->PinType == FloatType());
	TestTrue(TEXT("Call exposes Context payload"),
		FindPin(Call, TEXT("Context"), EGPD_Input)
		&& FindPin(Call, TEXT("Context"), EGPD_Input)->PinType == ActorType());
	UEdGraphPin* TagsPin = FindPin(Call, TEXT("Tags"), EGPD_Input);
	TestTrue(TEXT("Call exposes Tags array payload"),
		TagsPin && PayloadTypeMatches(TagsPin->PinType, NameArrayType()));
	if (!TagsPin) return false;

	UEdGraph* EventGraph = Call->GetGraph();
	TestNotNull(TEXT("Call belongs to the executable event graph"), EventGraph);
	if (!EventGraph) return false;
	FGraphNodeCreator<UK2Node_MakeArray> ArrayCreator(*EventGraph);
	UK2Node_MakeArray* TagsArray = ArrayCreator.CreateNode();
	ArrayCreator.Finalize();
	TestNotNull(TEXT("A Make Array node supplies the Tags input"), TagsArray);
	if (!TagsArray) return false;
	UEdGraphPin* ArrayOutput = TagsArray->GetOutputPin();
	TestNotNull(TEXT("Make Array exposes its output"), ArrayOutput);
	if (!ArrayOutput) return false;
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const bool bTagsConnected =
		Schema->TryCreateConnection(ArrayOutput, TagsPin);
	TestTrue(TEXT("Make Array output connects to Tags"), bTagsConnected);
	TestTrue(TEXT("Tags connection types the Make Array output"),
		bTagsConnected && PayloadTypeMatches(ArrayOutput->PinType, NameArrayType()));
	if (!bTagsConnected) return false;
	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(
		BP, EBlueprintCompileOptions::None, &Results);
	TestEqual(TEXT("Blueprint compiles after Call Delegate placement"), Results.NumErrors, 0);

	FMulticastDelegateProperty* Skeleton =
		FindProperty(BP->SkeletonGeneratedClass, Name);
	FMulticastDelegateProperty* Generated = FindProperty(BP->GeneratedClass, Name);
	TestNotNull(TEXT("Skeleton delegate property exists"), Skeleton);
	TestNotNull(TEXT("Generated delegate property exists"), Generated);
	TestNotNull(TEXT("Skeleton signature resolves"),
		Skeleton ? Skeleton->SignatureFunction.Get() : nullptr);
	TestNotNull(TEXT("Generated signature resolves"),
		Generated ? Generated->SignatureFunction.Get() : nullptr);

	const FString ReadText = FNwiroBlueprintTools::ReadBlueprint(
		FString::Printf(TEXT("{\"assetPath\":\"%s\"}"), *BP->GetPathName()));
	const TSharedPtr<FJsonObject> Read = ParseJson(ReadText);
	const TSharedPtr<FJsonObject> ReadDispatcher =
		FindDispatcherJson(Read, TEXT("eventDispatchers"), Name);
	TestTrue(TEXT("Readback reports valid signature"),
		ReadDispatcher.IsValid()
		&& ReadDispatcher->GetBoolField(TEXT("signatureValid")));
	TestFalse(TEXT("Valid readback has no malformed reason"),
		ReadDispatcher.IsValid() && ReadDispatcher->HasField(TEXT("malformedReason")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherValidationRemovalContractSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Production.ValidationAndRemoval",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherValidationRemovalContractSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	UBlueprint* BP = MakeBlueprint(TEXT("NwiroDispatcherValidation"));
	TestNotNull(TEXT("Transient Blueprint is created"), BP);
	if (!BP) return false;

	const FName ValidName(TEXT("WouldBeValid"));
	const TSharedRef<FJsonObject> Valid = MakeShared<FJsonObject>();
	Valid->SetStringField(TEXT("name"), ValidName.ToString());
	Valid->SetArrayField(TEXT("params"), {StringParam(TEXT("Amount"), TEXT("float"))});
	const TSharedRef<FJsonObject> Invalid = MakeShared<FJsonObject>();
	Invalid->SetStringField(TEXT("name"), TEXT("InvalidMap"));
	Invalid->SetArrayField(
		TEXT("params"), {StructuredParam(TEXT("Lookup"), TEXT("name"), TEXT(""), TEXT("map"))});
	const TSharedRef<FJsonObject> Batch = MakeShared<FJsonObject>();
	Batch->SetStringField(TEXT("blueprint"), BP->GetPathName());
	Batch->SetArrayField(TEXT("dispatchers"), {
		MakeShared<FJsonValueObject>(Valid),
		MakeShared<FJsonValueObject>(Invalid)});
	const TSharedPtr<FJsonObject> BatchResponse = ParseJson(
		FNwiroBlueprintTools::CreateEventDispatcher(Serialize(Batch)));
	TestFalse(TEXT("Invalid second item rejects whole batch"),
		BatchResponse.IsValid() && BatchResponse->GetBoolField(TEXT("success")));
	TestFalse(TEXT("Prevalidation leaves no first member"), HasMember(BP, ValidName));
	TestNull(TEXT("Prevalidation leaves no first graph"), FindSignatureGraph(BP, ValidName));

	TestFalse(TEXT("Dispatcher names with spaces are rejected"),
		Succeeded(FNwiroBlueprintTools::CreateEventDispatcher(
			StandaloneCommand(BP, TEXT("Has Space"), {}))));
	TestFalse(TEXT("Parameter names with Blueprint graph separators are rejected"),
		Succeeded(FNwiroBlueprintTools::CreateEventDispatcher(
			StandaloneCommand(BP, TEXT("InvalidParameterName"),
				{StringParam(TEXT("Payload.Value"), TEXT("float"))}))));
	TestFalse(TEXT("Type strings with surrounding whitespace are rejected"),
		Succeeded(FNwiroBlueprintTools::CreateEventDispatcher(
			StandaloneCommand(BP, TEXT("InvalidWhitespaceType"),
				{StringParam(TEXT("Value"), TEXT(" float"))}))));

	const int32 TooLongLength = FKismetNameValidator::GetMaximumNameLength() + 1;
	const FString TooLong = FString::ChrN(TooLongLength, TCHAR('A'));
	const TSharedPtr<FJsonObject> LongDispatcher = ParseJson(
		FNwiroBlueprintTools::CreateEventDispatcher(
			StandaloneCommand(BP, TooLong, {})));
	TestFalse(TEXT("Overlong dispatcher name is rejected safely"),
		LongDispatcher.IsValid() && LongDispatcher->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject> LongParameter = ParseJson(
		FNwiroBlueprintTools::CreateEventDispatcher(
			StandaloneCommand(BP, TEXT("LongParamDispatcher"),
				{StringParam(*TooLong, TEXT("float"))})));
	TestFalse(TEXT("Overlong parameter name is rejected safely"),
		LongParameter.IsValid() && LongParameter->GetBoolField(TEXT("success")));

	const FName DeferredName(TEXT("SkeletonOnlyDispatcher"));
	const TSharedPtr<FJsonObject> EditResponse = ParseJson(
		FNwiroBlueprintTools::EditBlueprint(
			EditAddCommand(BP, DeferredName, false)));
	TestTrue(TEXT("compile:false creation succeeds at skeleton level"),
		EditResponse.IsValid() && EditResponse->GetBoolField(TEXT("success")));
	if (!EditResponse.IsValid()) return false;
	TestFalse(TEXT("compile:false reports compiled false"),
		EditResponse->GetBoolField(TEXT("compiled")));
	TestEqual(TEXT("compile:false reports skeleton validation"),
		EditResponse->GetStringField(TEXT("validationLevel")), FString(TEXT("skeleton")));
	const TSharedPtr<FJsonObject> EditDispatcher =
		FindDispatcherJson(EditResponse, TEXT("dispatchers"), DeferredName);
	TestTrue(TEXT("compile:false reports valid skeleton"),
		EditDispatcher.IsValid()
		&& EditDispatcher->GetBoolField(TEXT("skeletonSignatureValid")));
	TestTrue(TEXT("compile:false generated validity is null"),
		EditDispatcher.IsValid()
		&& EditDispatcher->TryGetField(TEXT("generatedSignatureValid"))
		&& EditDispatcher->TryGetField(TEXT("generatedSignatureValid"))->Type == EJson::Null);

	const TSharedPtr<FJsonObject> Read = ParseJson(
		FNwiroBlueprintTools::ReadBlueprint(
			FString::Printf(TEXT("{\"assetPath\":\"%s\"}"), *BP->GetPathName())));
	const TSharedPtr<FJsonObject> ReadDispatcher =
		FindDispatcherJson(Read, TEXT("eventDispatchers"), DeferredName);
	TestTrue(TEXT("dirty readback keeps generated validity null"),
		ReadDispatcher.IsValid()
		&& ReadDispatcher->TryGetField(TEXT("generatedSignatureValid"))
		&& ReadDispatcher->TryGetField(TEXT("generatedSignatureValid"))->Type == EJson::Null);
	TestFalse(TEXT("dirty valid dispatcher has no malformed reason"),
		ReadDispatcher.IsValid() && ReadDispatcher->HasField(TEXT("malformedReason")));

	AddExpectedError(
		TEXT("SaveLoadedAsset failed: Asset is not registered"),
		EAutomationExpectedErrorFlags::Contains, 1);
	const TSharedPtr<FJsonObject> RemoveResponse = ParseJson(
		FNwiroBlueprintTools::EditBlueprint(RemoveCommand(BP, DeferredName)));
	TestTrue(TEXT("Public removal path succeeds"),
		RemoveResponse.IsValid() && RemoveResponse->GetBoolField(TEXT("success")));
	TestFalse(TEXT("Removal deletes delegate member"), HasMember(BP, DeferredName));
	TestNull(TEXT("Removal unregisters signature graph"), FindSignatureGraph(BP, DeferredName));
	TestNull(TEXT("Removal clears generated delegate property"),
		FindProperty(BP->GeneratedClass, DeferredName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherInvalidEditDiskNoOpSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Production.InvalidEditIsDiskNoOp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherInvalidEditDiskNoOpSpec::RunTest(
	const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	const FString PackagePath(
		TEXT("/Game/NwiroAutomation/BP_NwiroProDispatcherNoOpProof_4BC07A19"));
	const FString AssetName(TEXT("BP_NwiroProDispatcherNoOpProof_4BC07A19"));
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	const FString OwnershipMarker(
		TEXT("NWIRO_PRO_EVENT_DISPATCHER_NOOP_PROOF_4BC07A19_V1"));
	auto DeleteOwnedFixture = [&]() -> bool
	{
		if (!UEditorAssetLibrary::DoesAssetExist(PackagePath)) return true;
		UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (!Existing || Existing->BlueprintDescription != OwnershipMarker)
		{
			AddError(FString::Printf(
				TEXT("Refusing to delete unowned no-op proof asset '%s'"),
				*PackagePath));
			return false;
		}
		return UEditorAssetLibrary::DeleteAsset(PackagePath)
			&& !UEditorAssetLibrary::DoesAssetExist(PackagePath);
	};

	TestTrue(TEXT("No-op proof safely removes an owned stale fixture"),
		DeleteOwnedFixture());
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath)) return false;

	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* BP = Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
			TEXT("NwiroProDispatcherNoOp"))
		: nullptr;
	TestNotNull(TEXT("No-op proof creates a package-backed Blueprint"), BP);
	if (!BP) return false;
	BP->BlueprintDescription = OwnershipMarker;
	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(BP);
	const bool bBaselineSaved = UEditorAssetLibrary::SaveLoadedAsset(BP, false);
	TestTrue(TEXT("No-op proof saves a clean baseline"), bBaselineSaved);
	if (!bBaselineSaved)
	{
		DeleteOwnedFixture();
		return false;
	}
	TestFalse(TEXT("Baseline package is clean"), Package->IsDirty());

	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackagePath, FPackageName::GetAssetPackageExtension());
	TArray<uint8> BeforeBytes;
	TestTrue(TEXT("Baseline .uasset is readable"),
		FFileHelper::LoadFileToArray(BeforeBytes, *Filename));
	const FDateTime SentinelTimestamp(2001, 1, 2, 3, 4, 5);
	IFileManager::Get().SetTimeStamp(*Filename, SentinelTimestamp);
	TestTrue(TEXT("No-op proof installs a deterministic timestamp sentinel"),
		IFileManager::Get().GetTimeStamp(*Filename) == SentinelTimestamp);
	const int32 VariableCountBefore = BP->NewVariables.Num();
	const int32 SignatureGraphCountBefore = BP->DelegateSignatureGraphs.Num();
	const EBlueprintStatus BlueprintStatusBefore = BP->Status;
	const bool bDependenciesUpToDateBefore = BP->bCachedDependenciesUpToDate;

	int32 CompileCount = 0;
	int32 SaveCount = 0;
	const FDelegateHandle CompileHandle = BP->OnCompiled().AddLambda(
		[&CompileCount](UBlueprint*) { ++CompileCount; });
	const FDelegateHandle SaveHandle =
		UPackage::PackageSavedWithContextEvent.AddLambda(
			[&SaveCount, Package](
				const FString&, UPackage* SavedPackage, FObjectPostSaveContext)
			{
				if (SavedPackage == Package) ++SaveCount;
			});

	const FName RejectedName(TEXT("RejectedDuplicateParams"));
	const TSharedPtr<FJsonObject> RejectedResponse = ParseJson(
		FNwiroBlueprintTools::EditBlueprint(EditAddCommandWithParams(
			BP, RejectedName,
			{StringParam(TEXT("Value"), TEXT("int")),
			 StringParam(TEXT("value"), TEXT("int"))},
			true)));
	BP->OnCompiled().Remove(CompileHandle);
	UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);

	TestTrue(TEXT("Invalid dispatcher response parses"), RejectedResponse.IsValid());
	TestFalse(TEXT("Invalid dispatcher edit reports failure"),
		RejectedResponse.IsValid()
		&& RejectedResponse->GetBoolField(TEXT("success")));
	TestFalse(TEXT("Invalid dispatcher edit reports no compile"),
		RejectedResponse.IsValid()
		&& RejectedResponse->GetBoolField(TEXT("compiled")));
	TestFalse(TEXT("Invalid dispatcher edit reports no disk save"),
		RejectedResponse.IsValid()
		&& RejectedResponse->GetBoolField(TEXT("savedToDisk")));
	TestEqual(TEXT("Invalid dispatcher keeps compile status not_requested"),
		RejectedResponse.IsValid()
			? RejectedResponse->GetStringField(TEXT("compileStatus"))
			: FString(),
		FString(TEXT("not_requested")));
	TestEqual(TEXT("Invalid dispatcher triggers no real compile"), CompileCount, 0);
	TestEqual(TEXT("Invalid dispatcher triggers no real package save"), SaveCount, 0);
	TestFalse(TEXT("Invalid dispatcher leaves a clean package"), Package->IsDirty());
	TestTrue(TEXT("Invalid dispatcher preserves Blueprint compile status"),
		BP->Status == BlueprintStatusBefore);
	TestEqual(TEXT("Invalid dispatcher preserves dependency-cache state"),
		BP->bCachedDependenciesUpToDate, bDependenciesUpToDateBefore);
	TestEqual(TEXT("Invalid dispatcher preserves variable count"),
		BP->NewVariables.Num(), VariableCountBefore);
	TestEqual(TEXT("Invalid dispatcher preserves signature graph count"),
		BP->DelegateSignatureGraphs.Num(), SignatureGraphCountBefore);
	TestFalse(TEXT("Invalid dispatcher leaves no delegate member"),
		HasMember(BP, RejectedName));
	TestNull(TEXT("Invalid dispatcher leaves no signature graph"),
		FindSignatureGraph(BP, RejectedName));

	TArray<uint8> AfterBytes;
	TestTrue(TEXT("No-op .uasset remains readable"),
		FFileHelper::LoadFileToArray(AfterBytes, *Filename));
	TestTrue(TEXT("Invalid dispatcher leaves .uasset bytes unchanged"),
		BeforeBytes == AfterBytes);
	TestTrue(TEXT("Invalid dispatcher leaves timestamp sentinel unchanged"),
		IFileManager::Get().GetTimeStamp(*Filename) == SentinelTimestamp);

	const FName PendingName(TEXT("PendingTypedDispatcher"));
	const TSharedPtr<FJsonObject> PendingResponse = ParseJson(
		FNwiroBlueprintTools::EditBlueprint(EditAddCommand(
			BP, PendingName, false)));
	TestTrue(TEXT("compile:false creates pending work"),
		PendingResponse.IsValid()
		&& PendingResponse->GetBoolField(TEXT("success")));
	TestTrue(TEXT("compile:false leaves the package dirty"), Package->IsDirty());

	CompileCount = 0;
	SaveCount = 0;
	const FDelegateHandle DirtyCompileHandle = BP->OnCompiled().AddLambda(
		[&CompileCount](UBlueprint*) { ++CompileCount; });
	const FDelegateHandle DirtySaveHandle =
		UPackage::PackageSavedWithContextEvent.AddLambda(
			[&SaveCount, Package](
				const FString&, UPackage* SavedPackage, FObjectPostSaveContext)
			{
				if (SavedPackage == Package) ++SaveCount;
			});
	const TSharedPtr<FJsonObject> DirtyResponse = ParseJson(
		FNwiroBlueprintTools::EditBlueprint(EditAddCommandWithParams(
			BP, FName(TEXT("RejectedWhileDirty")),
			{StringParam(TEXT("Value"), TEXT("int")),
			 StringParam(TEXT("value"), TEXT("int"))},
			true)));
	BP->OnCompiled().Remove(DirtyCompileHandle);
	UPackage::PackageSavedWithContextEvent.Remove(DirtySaveHandle);

	TestFalse(TEXT("Invalid edit still reports failure while flushing pending work"),
		DirtyResponse.IsValid()
		&& DirtyResponse->GetBoolField(TEXT("success")));
	TestTrue(TEXT("Dirty baseline is compiled despite the invalid current op"),
		DirtyResponse.IsValid()
		&& DirtyResponse->GetBoolField(TEXT("compiled")));
	TestTrue(TEXT("Dirty baseline is saved despite the invalid current op"),
		DirtyResponse.IsValid()
		&& DirtyResponse->GetBoolField(TEXT("savedToDisk")));
	TestTrue(TEXT("Dirty baseline emits a real compile"), CompileCount > 0);
	TestTrue(TEXT("Dirty baseline emits a real package save"), SaveCount > 0);
	TestFalse(TEXT("Pending-work flush leaves the package clean"), Package->IsDirty());
	TestNotNull(TEXT("Pending dispatcher survives the flush"),
		FindSignatureGraph(BP, PendingName));
	TestNotNull(TEXT("Pending dispatcher reaches generated reflection"),
		FindProperty(BP->GeneratedClass, PendingName));
	TestNull(TEXT("Invalid dirty dispatcher leaves no signature graph"),
		FindSignatureGraph(BP, FName(TEXT("RejectedWhileDirty"))));
	TArray<uint8> FlushedBytes;
	TestTrue(TEXT("Flushed .uasset is readable"),
		FFileHelper::LoadFileToArray(FlushedBytes, *Filename));
	TestFalse(TEXT("Pending-work flush changes the persisted asset"),
		BeforeBytes == FlushedBytes);
	TestTrue(TEXT("Pending-work flush replaces the timestamp sentinel"),
		IFileManager::Get().GetTimeStamp(*Filename) != SentinelTimestamp);

	TestTrue(TEXT("No-op proof deletes only its owned fixture"),
		DeleteOwnedFixture());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherPersistenceCreateSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Persistence.01CreateCompileSave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherPersistenceCreateSpec::RunTest(
	const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	FString CleanupError;
	const bool bStaleCleanupOk = DeleteOwnedPersistenceAsset(CleanupError);
	TestTrue(*FString::Printf(
		TEXT("Phase 1 safely cleans only an owned stale proof asset: %s"),
		*CleanupError), bStaleCleanupOk);
	if (!bStaleCleanupOk) return false;

	UBlueprint* BP = CreatePersistenceBlueprint();
	TestNotNull(TEXT("Phase 1 creates a registered package-backed Blueprint"), BP);
	if (!BP) return false;
	TestTrue(TEXT("Phase 1 applies the persistent proof ownership marker"),
		IsOwnedPersistenceAsset(BP));
	TestTrue(TEXT("Phase 1 registers the proof asset with the asset registry"),
		UEditorAssetLibrary::DoesAssetExist(PersistencePackagePath()));

	const FName DispatcherName(TEXT("PersistentTypedDispatcher"));
	const FString ResponseText = FNwiroBlueprintTools::CreateEventDispatcher(
		StandaloneCommand(BP, DispatcherName.ToString(), {
			StringParam(TEXT("Amount"), TEXT("float")),
			StringParam(TEXT("Context"), TEXT("Actor")),
			StructuredParam(
				TEXT("Actors"), TEXT("object"), TEXT("Actor"), TEXT("array"))}));
	const TSharedPtr<FJsonObject> Response = ParseJson(ResponseText);
	TestTrue(TEXT("Phase 1 production dispatcher creation succeeds"),
		Response.IsValid() && Response->GetBoolField(TEXT("success")));
	if (!Response.IsValid()) return false;
	TestTrue(TEXT("Phase 1 production path compiles"),
		Response->GetBoolField(TEXT("compiled")));
	TestEqual(TEXT("Phase 1 compile is clean"),
		Response->GetStringField(TEXT("compileStatus")), FString(TEXT("success")));
	TestEqual(TEXT("Phase 1 reaches generated validation"),
		Response->GetStringField(TEXT("validationLevel")), FString(TEXT("generated")));
	const TSharedPtr<FJsonObject> Diagnostic =
		FindDispatcherJson(Response, TEXT("dispatchers"), DispatcherName);
	TestTrue(TEXT("Phase 1 validates the generated signature"),
		Diagnostic.IsValid()
		&& Diagnostic->GetBoolField(TEXT("signatureValid"))
		&& Diagnostic->GetBoolField(TEXT("skeletonSignatureValid"))
		&& Diagnostic->GetBoolField(TEXT("generatedSignatureValid")));

	UEdGraph* Graph = FindSignatureGraph(BP, DispatcherName);
	UK2Node_FunctionEntry* Entry = FindEntry(Graph);
	TestNotNull(TEXT("Phase 1 creates the same-name signature graph"), Graph);
	TestNotNull(TEXT("Phase 1 creates the editable signature entry"), Entry);
	TestTrue(TEXT("Phase 1 creates the scalar pin"),
		PinMatches(FindPin(Entry, TEXT("Amount"), EGPD_Output), FloatType()));
	TestTrue(TEXT("Phase 1 creates the object pin"),
		PinMatches(FindPin(Entry, TEXT("Context"), EGPD_Output), ActorType()));
	TestTrue(TEXT("Phase 1 creates the object-array pin"),
		PinMatches(FindPin(Entry, TEXT("Actors"), EGPD_Output),
			ActorType(EPinContainerType::Array)));

	FMulticastDelegateProperty* Skeleton =
		FindProperty(BP->SkeletonGeneratedClass, DispatcherName);
	FMulticastDelegateProperty* Generated =
		FindProperty(BP->GeneratedClass, DispatcherName);
	TestNotNull(TEXT("Phase 1 creates skeleton reflection"), Skeleton);
	TestNotNull(TEXT("Phase 1 creates generated reflection"), Generated);
	UFunction* SkeletonSignature = Skeleton ? Skeleton->SignatureFunction.Get() : nullptr;
	UFunction* GeneratedSignature = Generated ? Generated->SignatureFunction.Get() : nullptr;
	TestTrue(TEXT("Phase 1 skeleton scalar reflection is exact"),
		SignatureParamMatches(SkeletonSignature, TEXT("Amount"), FloatType()));
	TestTrue(TEXT("Phase 1 skeleton object reflection is exact"),
		SignatureParamMatches(SkeletonSignature, TEXT("Context"), ActorType()));
	TestTrue(TEXT("Phase 1 skeleton array reflection is exact"),
		SignatureParamMatches(SkeletonSignature, TEXT("Actors"),
			ActorType(EPinContainerType::Array)));
	TestTrue(TEXT("Phase 1 generated scalar reflection is exact"),
		SignatureParamMatches(GeneratedSignature, TEXT("Amount"), FloatType()));
	TestTrue(TEXT("Phase 1 generated object reflection is exact"),
		SignatureParamMatches(GeneratedSignature, TEXT("Context"), ActorType()));
	TestTrue(TEXT("Phase 1 generated array reflection is exact"),
		SignatureParamMatches(GeneratedSignature, TEXT("Actors"),
			ActorType(EPinContainerType::Array)));

	const TSharedPtr<FJsonObject> Read = ParseJson(
		FNwiroBlueprintTools::ReadBlueprint(FString::Printf(
			TEXT("{\"assetPath\":\"%s\"}"), *PersistenceObjectPath())));
	const TSharedPtr<FJsonObject> ReadDispatcher =
		FindDispatcherJson(Read, TEXT("eventDispatchers"), DispatcherName);
	TestTrue(TEXT("Phase 1 production readback sees the valid dispatcher"),
		ReadDispatcher.IsValid()
		&& ReadDispatcher->GetBoolField(TEXT("signatureGraphPresent"))
		&& ReadDispatcher->GetBoolField(TEXT("signatureValid"))
		&& ReadDispatcher->GetBoolField(TEXT("generatedSignatureValid")));

	TestTrue(TEXT("Phase 1 saves the registered proof asset"),
		UEditorAssetLibrary::SaveLoadedAsset(BP, false));
	TestFalse(TEXT("Phase 1 leaves the saved package clean"),
		BP->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherPersistenceReopenRemoveSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Persistence.02ReopenVerifyRemoveSave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherPersistenceReopenRemoveSpec::RunTest(
	const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	TestTrue(TEXT("Phase 2 finds the phase-1 asset on disk"),
		UEditorAssetLibrary::DoesAssetExist(PersistencePackagePath()));
	UBlueprint* BP = LoadPersistenceBlueprint();
	TestNotNull(TEXT("Phase 2 reopens the package-backed Blueprint"), BP);
	if (!BP) return false;
	TestTrue(TEXT("Phase 2 reopens only the marked proof asset"),
		IsOwnedPersistenceAsset(BP));

	const FName DispatcherName(TEXT("PersistentTypedDispatcher"));
	TestTrue(TEXT("Phase 2 reload preserves the delegate member"),
		HasMember(BP, DispatcherName));
	UEdGraph* Graph = FindSignatureGraph(BP, DispatcherName);
	UK2Node_FunctionEntry* Entry = FindEntry(Graph);
	TestNotNull(TEXT("Phase 2 reload preserves the signature graph"), Graph);
	TestNotNull(TEXT("Phase 2 reload preserves the editable entry"), Entry);
	if (Entry)
	{
		TestEqual(TEXT("Phase 2 reload preserves ordered parameter count"),
			Entry->UserDefinedPins.Num(), 3);
		if (Entry->UserDefinedPins.Num() == 3)
		{
			TestTrue(TEXT("Phase 2 ordered scalar parameter is exact"),
				Entry->UserDefinedPins[0].IsValid()
				&& Entry->UserDefinedPins[0]->PinName == FName(TEXT("Amount"))
				&& Entry->UserDefinedPins[0]->DesiredPinDirection == EGPD_Output
				&& Entry->UserDefinedPins[0]->PinType == FloatType());
			TestTrue(TEXT("Phase 2 ordered object parameter is exact"),
				Entry->UserDefinedPins[1].IsValid()
				&& Entry->UserDefinedPins[1]->PinName == FName(TEXT("Context"))
				&& Entry->UserDefinedPins[1]->DesiredPinDirection == EGPD_Output
				&& Entry->UserDefinedPins[1]->PinType == ActorType());
			TestTrue(TEXT("Phase 2 ordered array parameter is exact"),
				Entry->UserDefinedPins[2].IsValid()
				&& Entry->UserDefinedPins[2]->PinName == FName(TEXT("Actors"))
				&& Entry->UserDefinedPins[2]->DesiredPinDirection == EGPD_Output
				&& Entry->UserDefinedPins[2]->PinType
					== ActorType(EPinContainerType::Array));
		}
	}

	FMulticastDelegateProperty* Skeleton =
		FindProperty(BP->SkeletonGeneratedClass, DispatcherName);
	FMulticastDelegateProperty* Generated =
		FindProperty(BP->GeneratedClass, DispatcherName);
	TestNotNull(TEXT("Phase 2 reload preserves skeleton reflection"), Skeleton);
	TestNotNull(TEXT("Phase 2 reload preserves generated reflection"), Generated);
	UFunction* SkeletonSignature = Skeleton ? Skeleton->SignatureFunction.Get() : nullptr;
	UFunction* GeneratedSignature = Generated ? Generated->SignatureFunction.Get() : nullptr;
	TestTrue(TEXT("Phase 2 skeleton scalar parameter is exact"),
		SignatureParamMatches(SkeletonSignature, TEXT("Amount"), FloatType()));
	TestTrue(TEXT("Phase 2 skeleton object parameter is exact"),
		SignatureParamMatches(SkeletonSignature, TEXT("Context"), ActorType()));
	TestTrue(TEXT("Phase 2 skeleton array parameter is exact"),
		SignatureParamMatches(SkeletonSignature, TEXT("Actors"),
			ActorType(EPinContainerType::Array)));
	TestTrue(TEXT("Phase 2 generated scalar parameter is exact"),
		SignatureParamMatches(GeneratedSignature, TEXT("Amount"), FloatType()));
	TestTrue(TEXT("Phase 2 generated object parameter is exact"),
		SignatureParamMatches(GeneratedSignature, TEXT("Context"), ActorType()));
	TestTrue(TEXT("Phase 2 generated array parameter is exact"),
		SignatureParamMatches(GeneratedSignature, TEXT("Actors"),
			ActorType(EPinContainerType::Array)));

	const TSharedPtr<FJsonObject> ReadBeforeRemove = ParseJson(
		FNwiroBlueprintTools::ReadBlueprint(FString::Printf(
			TEXT("{\"assetPath\":\"%s\"}"), *PersistenceObjectPath())));
	const TSharedPtr<FJsonObject> ReadDispatcher = FindDispatcherJson(
		ReadBeforeRemove, TEXT("eventDispatchers"), DispatcherName);
	TestTrue(TEXT("Phase 2 production readback preserves the valid signature"),
		ReadDispatcher.IsValid()
		&& ReadDispatcher->GetBoolField(TEXT("signatureGraphPresent"))
		&& ReadDispatcher->GetBoolField(TEXT("signatureValid"))
		&& ReadDispatcher->GetBoolField(TEXT("generatedSignatureValid")));

	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(
		BP, EBlueprintCompileOptions::None, &CompileResults);
	TestEqual(TEXT("Phase 2 reopened dispatcher compiles cleanly"),
		CompileResults.NumErrors, 0);
	TestTrue(TEXT("Phase 2 reopened Blueprint has a clean status"),
		BP->Status != EBlueprintStatus::BS_Error);

	const TSharedPtr<FJsonObject> RemoveResponse = ParseJson(
		FNwiroBlueprintTools::EditBlueprint(RemoveCommand(BP, DispatcherName)));
	TestTrue(TEXT("Phase 2 production removal succeeds"),
		RemoveResponse.IsValid() && RemoveResponse->GetBoolField(TEXT("success")));
	if (!RemoveResponse.IsValid()) return false;
	TestTrue(TEXT("Phase 2 removal compiles"),
		RemoveResponse->GetBoolField(TEXT("compiled")));
	TestEqual(TEXT("Phase 2 removal compile is clean"),
		RemoveResponse->GetStringField(TEXT("compileStatus")),
		FString(TEXT("success")));
	TestTrue(TEXT("Phase 2 production removal saves to disk"),
		RemoveResponse->GetBoolField(TEXT("savedToDisk")));
	TestFalse(TEXT("Phase 2 removes the member in memory"),
		HasMember(BP, DispatcherName));
	TestNull(TEXT("Phase 2 removes the graph in memory"),
		FindSignatureGraph(BP, DispatcherName));
	TestNull(TEXT("Phase 2 removes skeleton reflection in memory"),
		FindProperty(BP->SkeletonGeneratedClass, DispatcherName));
	TestNull(TEXT("Phase 2 removes generated reflection in memory"),
		FindProperty(BP->GeneratedClass, DispatcherName));

	const TSharedPtr<FJsonObject> ReadAfterRemove = ParseJson(
		FNwiroBlueprintTools::ReadBlueprint(FString::Printf(
			TEXT("{\"assetPath\":\"%s\"}"), *PersistenceObjectPath())));
	const TSharedPtr<FJsonObject> RemovedReadDispatcher = FindDispatcherJson(
		ReadAfterRemove, TEXT("eventDispatchers"), DispatcherName);
	TestNull(TEXT("Phase 2 production readback sees no dispatcher residue"),
		RemovedReadDispatcher.Get());
	TestFalse(TEXT("Phase 2 leaves the saved package clean"),
		BP->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroEventDispatcherPersistenceAbsentCleanupSpec,
	"Nwiro.Pro.Blueprint.EventDispatcher.Persistence.03ReopenVerifyAbsentCleanup",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroEventDispatcherPersistenceAbsentCleanupSpec::RunTest(
	const FString& Parameters)
{
	using namespace NwiroEventDispatcherProofPrivate;
	TestTrue(TEXT("Phase 3 finds the phase-2 asset on disk"),
		UEditorAssetLibrary::DoesAssetExist(PersistencePackagePath()));
	UBlueprint* BP = LoadPersistenceBlueprint();
	TestNotNull(TEXT("Phase 3 reopens the phase-2 Blueprint"), BP);
	if (!BP) return false;
	TestTrue(TEXT("Phase 3 reopens only the marked proof asset"),
		IsOwnedPersistenceAsset(BP));

	const FName DispatcherName(TEXT("PersistentTypedDispatcher"));
	TestFalse(TEXT("Phase 3 persisted state has no delegate member"),
		HasMember(BP, DispatcherName));
	TestNull(TEXT("Phase 3 persisted state has no signature graph"),
		FindSignatureGraph(BP, DispatcherName));
	TestNull(TEXT("Phase 3 persisted state has no skeleton delegate property"),
		FindProperty(BP->SkeletonGeneratedClass, DispatcherName));
	TestNull(TEXT("Phase 3 persisted state has no generated delegate property"),
		FindProperty(BP->GeneratedClass, DispatcherName));

	const TSharedPtr<FJsonObject> Read = ParseJson(
		FNwiroBlueprintTools::ReadBlueprint(FString::Printf(
			TEXT("{\"assetPath\":\"%s\"}"), *PersistenceObjectPath())));
	TestTrue(TEXT("Phase 3 production readback parses as an object"),
		Read.IsValid());
	const TSharedPtr<FJsonObject> ReadDispatcher =
		FindDispatcherJson(Read, TEXT("eventDispatchers"), DispatcherName);
	TestNull(TEXT("Phase 3 production readback has no dispatcher residue"),
		ReadDispatcher.Get());

	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(
		BP, EBlueprintCompileOptions::None, &CompileResults);
	TestEqual(TEXT("Phase 3 dispatcher-free Blueprint compiles cleanly"),
		CompileResults.NumErrors, 0);
	TestTrue(TEXT("Phase 3 dispatcher-free Blueprint has a clean status"),
		BP->Status != EBlueprintStatus::BS_Error);

	FString CleanupError;
	const bool bCleanupOk = DeleteOwnedPersistenceAsset(CleanupError);
	TestTrue(*FString::Printf(
		TEXT("Phase 3 deletes only the marked proof asset: %s"),
		*CleanupError), bCleanupOk);
	TestFalse(TEXT("Phase 3 cleanup removes the proof asset from the registry"),
		UEditorAssetLibrary::DoesAssetExist(PersistencePackagePath()));
	return true;
}
