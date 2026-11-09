// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroGASTools.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "NwiroBlueprintPreloadGuard.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Factories/Factory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Json.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroGAS, Log, All);

// ============================================================
// HELPER: Create GAS Blueprint by finding class dynamically
// ============================================================

static FString CreateGASBlueprint(const FString& JsonCommand, const FString& ClassName, const FString& DefaultPrefix, const FString& DefaultPath)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = DefaultPath;
	if (!Name.StartsWith(DefaultPrefix)) Name = DefaultPrefix + Name;

	// Find the GAS class dynamically (no compile dependency on GameplayAbilities)
	UClass* ParentClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass, ELogVerbosity::Warning, TEXT("NwiroGAS"));
	if (!ParentClass)
	{
		// Try with full path
		ParentClass = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/GameplayAbilities.%s"), *ClassName));
	}
	if (!ParentClass)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Class '%s' not found. Is GameplayAbilities plugin enabled?\"}"), *ClassName);

	FString FullPath = Path / Name;
	// Idempotent guard — CreateBlueprint asserts if the BP already exists in the package.
	if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName(), *ClassName);
	}
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package) return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	if (UBlueprint* Existing = FindObject<UBlueprint>(Package, *Name))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName(), *ClassName);
	}

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to create %s blueprint\"}"), *ClassName);

	FKismetEditorUtilities::CompileBlueprint(BP);
	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\"}"),
		*Name, *BP->GetPathName(), *ClassName);
}

static FString ListGASAssets(const FString& JsonCommand, const FString& ClassName)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));

	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& A : Assets)
	{
		// Never force a disk load here. A.GetAsset() == FastGetAsset(bLoad=true), and loading a
		// compile-failed GAS Blueprint (whose parent UClass is unresolved when the GameplayAbilities
		// plugin is not fully loaded) derefs a null generated-class/CDO (read 0x8) INSIDE the load,
		// before the BP null-guard runs -> EXCEPTION_ACCESS_VIOLATION that kills the editor. Read the
		// parent from the asset-registry NativeParentClass tag instead (no load), mirroring the
		// existing smart-fallback guard in NwiroBlueprintTools.cpp.
		FString NativeParent;
		if (!A.GetTagValue(TEXT("NativeParentClass"), NativeParent) || NativeParent.IsEmpty()) continue;
		if (!NativeParent.Contains(ClassName, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *A.PackageName.ToString(), *A.AssetName.ToString()));
		// Normalize the tag (e.g. "/Script/GameplayAbilities.GameplayEffect" or
		// "Class'/Script/...GameplayEffect'") back to the short class name to preserve the
		// pre-fix output contract.
		FString ParentShort = NativeParent;
		int32 DotIdx = INDEX_NONE;
		if (ParentShort.FindLastChar(TEXT('.'), DotIdx)) ParentShort = ParentShort.Mid(DotIdx + 1);
		ParentShort.RemoveFromEnd(TEXT("'"));
		Obj->SetStringField(TEXT("parentClass"), ParentShort);
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("assets"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// GAS TOOLS
// ============================================================

FString FNwiroGASTools::CreateGameplayAbility(const FString& JsonCommand)
{
	return CreateGASBlueprint(JsonCommand, TEXT("GameplayAbility"), TEXT("GA_"), TEXT("/Game/GAS"));
}

FString FNwiroGASTools::CreateGameplayEffect(const FString& JsonCommand)
{
	return CreateGASBlueprint(JsonCommand, TEXT("GameplayEffect"), TEXT("GE_"), TEXT("/Game/GAS"));
}

FString FNwiroGASTools::CreateAttributeSet(const FString& JsonCommand)
{
	return CreateGASBlueprint(JsonCommand, TEXT("AttributeSet"), TEXT("AS_"), TEXT("/Game/GAS"));
}

FString FNwiroGASTools::ListGameplayAbilities(const FString& JsonCommand)
{
	return ListGASAssets(JsonCommand, TEXT("GameplayAbility"));
}

FString FNwiroGASTools::ListGameplayEffects(const FString& JsonCommand)
{
	return ListGASAssets(JsonCommand, TEXT("GameplayEffect"));
}

FString FNwiroGASTools::ListAttributeSets(const FString& JsonCommand)
{
	return ListGASAssets(JsonCommand, TEXT("AttributeSet"));
}

namespace NwiroGASRead
{
static FString Serialize(const TSharedRef<FJsonObject>& Result)
{
	FString Out; FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out)); return Out;
}
static FString Failure(const FString& Error)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), Error); return Serialize(Result);
}
// GAS is optional: invoke only these source-verified public native getters. Never
// inspect private ASC fields or assume layout for reflected parameter structs.
struct FGetter
{
	UObject* Target = nullptr;
	UFunction* Function = nullptr;
	TUniquePtr<FStructOnScope> Params;
	FGetter(UObject* InTarget, UClass* Owner, const TCHAR* Name, int32 ParameterCount) : Target(InTarget)
	{
		UFunction* Candidate = Owner ? Owner->FindFunctionByName(Name) : nullptr;
		if (IsValid(Target) && Owner && Target->IsA(Owner) && Candidate && Candidate->GetOuterUClass() == Owner
			&& Candidate->HasAllFunctionFlags(FUNC_Native | FUNC_Public) && Candidate->NumParms == ParameterCount)
		{ Function = Candidate; Params = MakeUnique<FStructOnScope>(Function); }
	}
	template<typename T> T* Param(const TCHAR* Name) const
	{
		T* Property = Function ? FindFProperty<T>(Function, Name) : nullptr;
		return Property && Property->HasAnyPropertyFlags(CPF_Parm) ? Property : nullptr;
	}
	uint8* Data() const { return Params ? Params->GetStructMemory() : nullptr; }
	void Call() const { Target->ProcessEvent(Function, Data()); }
};
static UObject* LibraryDefault(const TCHAR* Path)
{
	UClass* Class = FindObject<UClass>(nullptr, Path);
	return Class ? Class->GetDefaultObject(false) : nullptr;
}
static TSharedRef<FJsonObject> ObjectJson(const UObject* Object)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Object->GetPathName()); Result->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
	return Result;
}
static bool ReadAttributes(UObject* Component, UClass* ASCClass, TArray<TSharedPtr<FJsonValue>>& Out)
{
	UClass* AttributeBase = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.AttributeSet"));
	FGetter Getter(Component, ASCClass, TEXT("GetAttributeSet"), 2);
	FClassProperty* Input = Getter.Param<FClassProperty>(TEXT("AttributeSetClass"));
	FObjectPropertyBase* Return = Getter.Param<FObjectPropertyBase>(TEXT("ReturnValue"));
	if (!AttributeBase || !Input || Input->MetaClass != AttributeBase || !Return || Return->PropertyClass != AttributeBase) return false;
	TSet<UObject*> Seen;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!IsValid(Class) || !Class->IsChildOf(AttributeBase) || Class->HasAnyClassFlags(CLASS_NewerVersionExists) || Class->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)) continue;
		Input->SetObjectPropertyValue_InContainer(Getter.Data(), Class);
		Getter.Call();
		UObject* Set = Return->GetObjectPropertyValue_InContainer(Getter.Data());
		if (IsValid(Set) && !Seen.Contains(Set)) { Seen.Add(Set); Out.Add(MakeShared<FJsonValueObject>(ObjectJson(Set))); }
	}
	return true;
}
static bool ReadAbilities(UObject* Component, UClass* ASCClass, TArray<TSharedPtr<FJsonValue>>& Out)
{
	FGetter Handles(Component, ASCClass, TEXT("GetAllAbilities"), 1);
	FArrayProperty* Array = Handles.Param<FArrayProperty>(TEXT("OutAbilityHandles"));
	FStructProperty* HandleType = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
	UObject* Library = LibraryDefault(TEXT("/Script/GameplayAbilities.AbilitySystemBlueprintLibrary"));
	FGetter Lookup(Library, Library ? Library->GetClass() : nullptr, TEXT("GetGameplayAbilityFromSpecHandle"), 4);
	FObjectPropertyBase* ASC = Lookup.Param<FObjectPropertyBase>(TEXT("AbilitySystem"));
	FStructProperty* Handle = Lookup.Param<FStructProperty>(TEXT("AbilitySpecHandle"));
	FBoolProperty* Instance = Lookup.Param<FBoolProperty>(TEXT("bIsInstance"));
	FObjectPropertyBase* Return = Lookup.Param<FObjectPropertyBase>(TEXT("ReturnValue"));
	if (!HandleType || HandleType->Struct->GetPathName() != TEXT("/Script/GameplayAbilities.GameplayAbilitySpecHandle") || !ASC || ASC->PropertyClass != ASCClass
		|| !Handle || Handle->Struct != HandleType->Struct || !Instance || !Return || Return->PropertyClass->GetPathName() != TEXT("/Script/GameplayAbilities.GameplayAbility")) return false;
	Handles.Call();
	FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Handles.Data()));
	ASC->SetObjectPropertyValue_InContainer(Lookup.Data(), Component);
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		Handle->CopyCompleteValue(Handle->ContainerPtrToValuePtr<void>(Lookup.Data()), Values.GetRawPtr(Index));
		Lookup.Call();
		UObject* Ability = Return->GetObjectPropertyValue_InContainer(Lookup.Data());
		if (!IsValid(Ability)) return false;
		TSharedRef<FJsonObject> Item = ObjectJson(Ability);
		Item->SetBoolField(TEXT("isInstance"), Instance->GetPropertyValue_InContainer(Lookup.Data()));
		Out.Add(MakeShared<FJsonValueObject>(Item));
	}
	return true;
}
static bool ReadTags(UObject* Component, TArray<TSharedPtr<FJsonValue>>& Out)
{
	UObject* Library = LibraryDefault(TEXT("/Script/GameplayTags.BlueprintGameplayTagLibrary"));
	UClass* Owner = Library ? Library->GetClass() : nullptr;
	FGetter Getter(Library, Owner, TEXT("GetOwnedGameplayTags"), 2);
	FInterfaceProperty* Input = Getter.Param<FInterfaceProperty>(TEXT("TagContainerInterface"));
	FStructProperty* Container = Getter.Param<FStructProperty>(TEXT("ReturnValue"));
	FGetter Break(Library, Owner, TEXT("BreakGameplayTagContainer"), 2);
	FStructProperty* BreakInput = Break.Param<FStructProperty>(TEXT("GameplayTagContainer"));
	FArrayProperty* Tags = Break.Param<FArrayProperty>(TEXT("GameplayTags"));
	FStructProperty* TagType = Tags ? CastField<FStructProperty>(Tags->Inner) : nullptr;
	FGetter Name(Library, Owner, TEXT("GetTagName"), 2);
	FStructProperty* NameInput = Name.Param<FStructProperty>(TEXT("GameplayTag"));
	FNameProperty* NameReturn = Name.Param<FNameProperty>(TEXT("ReturnValue"));
	if (!Input || Input->InterfaceClass->GetPathName() != TEXT("/Script/GameplayTags.GameplayTagAssetInterface") || !Component->GetClass()->ImplementsInterface(Input->InterfaceClass)
		|| !Container || Container->Struct->GetPathName() != TEXT("/Script/GameplayTags.GameplayTagContainer") || !BreakInput || BreakInput->Struct != Container->Struct
		|| !TagType || TagType->Struct->GetPathName() != TEXT("/Script/GameplayTags.GameplayTag") || !NameInput || NameInput->Struct != TagType->Struct || !NameReturn) return false;
	FScriptInterface* Interface = Input->ContainerPtrToValuePtr<FScriptInterface>(Getter.Data());
	Interface->SetObject(Component); Interface->SetInterface(Component->GetInterfaceAddress(Input->InterfaceClass));
	if (!Interface->GetInterface()) return false;
	Getter.Call();
	BreakInput->CopyCompleteValue(BreakInput->ContainerPtrToValuePtr<void>(Break.Data()), Container->ContainerPtrToValuePtr<void>(Getter.Data()));
	Break.Call();
	FScriptArrayHelper Values(Tags, Tags->ContainerPtrToValuePtr<void>(Break.Data()));
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		NameInput->CopyCompleteValue(NameInput->ContainerPtrToValuePtr<void>(Name.Data()), Values.GetRawPtr(Index));
		Name.Call(); Out.Add(MakeShared<FJsonValueString>(NameReturn->GetPropertyValue_InContainer(Name.Data()).ToString()));
	}
	return true;
}
static TSharedRef<FJsonObject> ReadComponent(UObject* Component, UClass* ASCClass, const TCHAR* Scope)
{
	TSharedRef<FJsonObject> Result = ObjectJson(Component);
	Result->SetStringField(TEXT("scope"), Scope);
	TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Attributes, Abilities, Tags;
	const bool HasAttributes = ReadAttributes(Component, ASCClass, Attributes);
	const bool HasAbilities = ReadAbilities(Component, ASCClass, Abilities);
	const bool HasTags = ReadTags(Component, Tags);
	Capabilities->SetBoolField(TEXT("attributeSets"), HasAttributes); Capabilities->SetBoolField(TEXT("abilities"), HasAbilities); Capabilities->SetBoolField(TEXT("ownedTags"), HasTags);
	Result->SetObjectField(TEXT("capabilities"), Capabilities);
	if (HasAttributes) Result->SetArrayField(TEXT("attributeSets"), Attributes);
	if (HasAbilities) Result->SetArrayField(TEXT("abilities"), Abilities);
	if (HasTags) Result->SetArrayField(TEXT("ownedTags"), Tags);
	// GetAttributeSet returns the first matching instance for a class. Distinct
	// same-class instances can exist, so these observations are not an inventory.
	Result->SetBoolField(TEXT("attributeSetInventoryComplete"), false);
	Result->SetStringField(TEXT("attributeSetLookupScope"), HasAttributes ? TEXT("first_matching_instance_per_loaded_class") : TEXT("unavailable"));
	Result->SetBoolField(TEXT("complete"), false);
	Result->SetStringField(TEXT("incompleteReason"), TEXT("Attribute sets are observed through first-match per-class lookup; additional instances of the same class may be omitted."));
	if (!(HasAttributes && HasAbilities && HasTags)) Result->SetStringField(TEXT("unavailableReason"), TEXT("A public native getter or its loaded library/signature is unavailable; omitted fields are unknown, not empty."));
	return Result;
}
}

FString FNwiroGASTools::GetGASInfo(const FString& JsonCommand)
{
	using namespace NwiroGASRead;
	if (!IsInGameThread() || !GEditor) return Failure(TEXT("get_gas_info requires the editor game thread."));
	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid()) return Failure(TEXT("Invalid JSON object."));
	FString ActorName, Path;
	if (Cmd->HasField(TEXT("actor")) && (!Cmd->HasTypedField<EJson::String>(TEXT("actor")) || !Cmd->TryGetStringField(TEXT("actor"), ActorName))) return Failure(TEXT("actor must be a string."));
	if (Cmd->HasField(TEXT("path")) && (!Cmd->HasTypedField<EJson::String>(TEXT("path")) || !Cmd->TryGetStringField(TEXT("path"), Path))) return Failure(TEXT("path must be a string."));
	if (!ActorName.IsEmpty() && !Path.IsEmpty() && ActorName != Path) return Failure(TEXT("Supply actor or path, not conflicting targets."));
	const FString Target = ActorName.IsEmpty() ? Path : ActorName;
	if (Target.IsEmpty() || Target.Len() >= NAME_SIZE) return Failure(TEXT("An exact actor path/name/unique label or Blueprint asset path is required."));
	UWorld* World = GEditor->PlayWorld ? GEditor->PlayWorld.Get() : GEditor->GetEditorWorldContext().World();
	TArray<AActor*> Matches;
	if (World) for (TActorIterator<AActor> It(World); It; ++It)
		if (IsValid(*It) && ((*It)->GetPathName() == Target || (*It)->GetName() == Target || (*It)->GetActorLabel() == Target)) Matches.Add(*It);
	if (Matches.Num() > 1) return Failure(TEXT("Actor identifier is ambiguous; use its exact object path."));
	AActor* Actor = Matches.Num() == 1 ? Matches[0] : nullptr;
	UBlueprint* Blueprint = nullptr;
	UBlueprintGeneratedClass* Generated = nullptr;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Actor)
	{
		FString ObjectPath;
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		const auto Inspection = NwiroBlueprintPreload::InspectExactAssetPath(Target, ARM.Get(), ObjectPath);
		if (Inspection.Disposition == NwiroBlueprintPreload::EDisposition::Rejected) return Failure(FString(TEXT("Blueprint inspection rejected: ")) + Inspection.Code);
		UObject* Object = Inspection.ExistingObject;
		if (!Object)
		{
			const FAssetData Asset = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
			if (Asset.IsValid() && Asset.AssetClassPath == UBlueprint::StaticClass()->GetClassPathName()) Object = Asset.GetAsset();
		}
		Blueprint = Cast<UBlueprint>(Object);
		if (!IsValid(Blueprint)) return Failure(TEXT("No exact actor or Blueprint asset found: ") + Target);
		Generated = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		Result->SetStringField(TEXT("scope"), TEXT("blueprint_defaults_and_declarations"));
		Result->SetStringField(TEXT("name"), Blueprint->GetName()); Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("None"));
		// Never compile or manufacture a CDO to answer a read. Dirty Blueprints can
		// have stale generated defaults; their declarations are reported separately.
		Actor = Blueprint->IsUpToDate() && Generated ? Cast<AActor>(Generated->GetDefaultObject(false)) : nullptr;
		Result->SetBoolField(TEXT("defaultObjectAvailable"), Actor != nullptr);
		Result->SetBoolField(TEXT("blueprintUpToDate"), Blueprint->IsUpToDate());
	}
	else
	{
		Result->SetStringField(TEXT("scope"), GEditor->PlayWorld ? TEXT("pie_actor") : TEXT("editor_actor"));
		Result->SetStringField(TEXT("name"), Actor->GetName()); Result->SetStringField(TEXT("path"), Actor->GetPathName());
		Result->SetStringField(TEXT("world"), World->GetPathName());
	}
	Result->SetBoolField(TEXT("success"), true);
	UClass* ASCClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.AbilitySystemComponent"));
	Result->SetBoolField(TEXT("gasAvailable"), ASCClass != nullptr);
	if (!ASCClass)
	{
		Result->SetStringField(TEXT("gasState"), TEXT("GameplayAbilities native classes are not loaded; component details are unavailable."));
		return Serialize(Result);
	}
	TArray<TSharedPtr<FJsonValue>> Components;
	TSet<UObject*> Seen;
	bool AssociationKnown = false;
	if (Actor)
	{
		for (UActorComponent* Component : Actor->GetComponents())
			if (IsValid(Component) && Component->IsA(ASCClass))
			{ Seen.Add(Component); Components.Add(MakeShared<FJsonValueObject>(ReadComponent(Component, ASCClass, Blueprint ? TEXT("class_default_component") : TEXT("actor_component")))); }
		// The public GAS resolver also supports an actor interface exposing an ASC
		// owned elsewhere (for example, a pawn using its PlayerState's component).
		UObject* Library = LibraryDefault(TEXT("/Script/GameplayAbilities.AbilitySystemBlueprintLibrary"));
		FGetter Getter(Library, Library ? Library->GetClass() : nullptr, TEXT("GetAbilitySystemComponent"), 2);
		FObjectPropertyBase* Input = Getter.Param<FObjectPropertyBase>(TEXT("Actor"));
		FObjectPropertyBase* Return = Getter.Param<FObjectPropertyBase>(TEXT("ReturnValue"));
		const bool CanResolve = Input && Input->PropertyClass == AActor::StaticClass() && Return && Return->PropertyClass == ASCClass;
		AssociationKnown = CanResolve;
		Result->SetBoolField(TEXT("associationAvailable"), CanResolve);
		if (CanResolve)
		{
			Input->SetObjectPropertyValue_InContainer(Getter.Data(), Actor); Getter.Call();
			UObject* Associated = Return->GetObjectPropertyValue_InContainer(Getter.Data());
			if (IsValid(Associated))
			{
				Result->SetStringField(TEXT("associatedAbilitySystem"), Associated->GetPathName());
				if (!Seen.Contains(Associated)) Components.Add(MakeShared<FJsonValueObject>(ReadComponent(Associated, ASCClass, Blueprint ? TEXT("class_default_association") : TEXT("actor_association"))));
			}
			else Result->SetField(TEXT("associatedAbilitySystem"), MakeShared<FJsonValueNull>());
		}
	}
	Result->SetArrayField(TEXT("abilitySystemComponents"), Components);
	if (!Components.IsEmpty() || (Actor && AssociationKnown)) Result->SetBoolField(TEXT("hasAbilitySystem"), !Components.IsEmpty());
	else Result->SetField(TEXT("hasAbilitySystem"), MakeShared<FJsonValueNull>());
	if (Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Declared;
		TSet<UObject*> SeenTemplates;
		TArray<USimpleConstructionScript*> Scripts;
		if (Blueprint->SimpleConstructionScript) Scripts.Add(Blueprint->SimpleConstructionScript);
		for (UClass* Class = Blueprint->ParentClass; Class; Class = Class->GetSuperClass())
		{
			const UBlueprint* ParentBlueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
			if (ParentBlueprint && ParentBlueprint->SimpleConstructionScript) Scripts.AddUnique(ParentBlueprint->SimpleConstructionScript);
		}
		for (const USimpleConstructionScript* Script : Scripts)
		{
			for (const USCS_Node* Node : Script->GetAllNodes())
			{
				UActorComponent* Template = Node ? (Generated ? Node->GetActualComponentTemplate(Generated) : Node->ComponentTemplate.Get()) : nullptr;
				if (IsValid(Template) && Template->IsA(ASCClass) && !SeenTemplates.Contains(Template))
				{
					SeenTemplates.Add(Template); TSharedRef<FJsonObject> Item = ObjectJson(Template);
					Item->SetStringField(TEXT("scope"), TEXT("blueprint_component_template")); Item->SetBoolField(TEXT("instantiated"), false);
					Declared.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
		}
		Result->SetArrayField(TEXT("declaredAbilitySystemComponents"), Declared);
		Result->SetBoolField(TEXT("hasDeclaredAbilitySystem"), !Declared.IsEmpty());
	}
	return Serialize(Result);
}

// ============================================================
// PCG TOOLS
// ============================================================

FString FNwiroGASTools::CreatePCGGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/PCG");

	// Find PCGGraph class dynamically
	UClass* PCGGraphClass = FindFirstObject<UClass>(TEXT("PCGGraph"), EFindFirstObjectOptions::None, ELogVerbosity::Warning, TEXT("NwiroGAS"));
	if (!PCGGraphClass)
		PCGGraphClass = LoadObject<UClass>(nullptr, TEXT("/Script/PCG.PCGGraph"));

	if (!PCGGraphClass)
		return TEXT("{\"success\":false,\"error\":\"PCGGraph class not found. Is PCG plugin enabled?\"}");

	const FString FullPath = Path / Name;
	if (UObject* Existing = LoadObject<UObject>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		if (Existing->IsA(PCGGraphClass))
			return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"existed\":true}"),
				*Name, *Existing->GetPathName());
	}

	UClass* FactoryClass = LoadObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGGraphFactory"));
	UFactory* Factory = FactoryClass ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, PCGGraphClass, Factory);

	if (!NewAsset)
		return TEXT("{\"success\":false,\"error\":\"Failed to create PCG Graph\"}");

	NewAsset->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *NewAsset->GetPathName());
}

FString FNwiroGASTools::GetPCGInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{
			AActor* Actor = *It;
			TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			Result->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

			// Check for PCG component via reflection
			for (UActorComponent* Comp : Actor->GetComponents())
			{
				if (Comp->GetClass()->GetName().Contains(TEXT("PCG")))
				{
					Result->SetStringField(TEXT("pcgComponent"), Comp->GetName());
					Result->SetStringField(TEXT("pcgComponentClass"), Comp->GetClass()->GetName());
					break;
				}
			}

			FString Out;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Result, W);
			return Out;
		}
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);
}

FString FNwiroGASTools::ExecutePCG(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{
			// Trigger PCG generation via the component
			AActor* Actor = *It;
			for (UActorComponent* Comp : Actor->GetComponents())
			{
				if (Comp->GetClass()->GetName().Contains(TEXT("PCGComponent")))
				{
					// Use reflection to call Generate()
					UFunction* GenFunc = Comp->GetClass()->FindFunctionByName(TEXT("Generate"));
					if (GenFunc)
					{
						Comp->ProcessEvent(GenFunc, nullptr);
						return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"message\":\"PCG generation triggered\"}"), *Actor->GetActorLabel());
					}
					return TEXT("{\"success\":false,\"error\":\"PCGComponent found but Generate() function not available\"}");
				}
			}
			return TEXT("{\"success\":false,\"error\":\"No PCGComponent found on actor\"}");
		}
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);
}
