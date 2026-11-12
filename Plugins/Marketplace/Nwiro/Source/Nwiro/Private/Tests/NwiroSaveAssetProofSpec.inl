// Copyright 2026 Nwiro. All Rights Reserved.
// Explicit save proofs use owned GUID packages and real built-in dispatch.
// Included by ToolDispatch.cpp; Integration Kit uses its existing local route.
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorSettings.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Curves/CurveFloat.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SaveGame.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Interface.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace NwiroSaveAssetProof
{
	static FString Serialize(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		return Text;
	}

	struct FContext
	{
		FString RunId;
		FString Root;
		bool bValid = false;
		explicit FContext(FAutomationTestBase& Test)
		{
			FParse::Value(FCommandLine::Get(), TEXT("NwiroSaveProofRunId="), RunId);
			FGuid Parsed;
			bValid = RunId.Len() == 32 && FGuid::ParseExact(RunId, EGuidFormats::Digits, Parsed);
			if (!Test.TestTrue(TEXT("save fixture requires runner-owned GUID"), bValid)) return;
			Root = TEXT("/Game/NwiroSaveAcceptance_") + RunId + TEXT("/");
		}
	};

	struct FNeverSaveOnCompile
	{
		UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
		TEnumAsByte<ESaveOnCompile> Previous = Settings->SaveOnCompile;
		FNeverSaveOnCompile() { Settings->SaveOnCompile = SoC_Never; }
		~FNeverSaveOnCompile() { Settings->SaveOnCompile = Previous; }
	};

	enum class EKind { Enum, Struct, Actor, SaveGame, Interface, Widget, Unsupported };

	struct FFixture
	{
		TStrongObjectPtr<UObject> Asset;
		UPackage* Package = nullptr;
		FString FileName;
		bool bReady = false;
		FFixture(FAutomationTestBase& Test, const FContext& Context, const FString& Name, EKind Kind, bool bRegister = true)
		{
			const FString Path = Context.Root + Name;
			FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
			if (!Test.TestNull(TEXT("owned package absent before fixture"), FindPackage(nullptr, *Path))
				|| !Test.TestFalse(TEXT("owned file absent before fixture"), IFileManager::Get().FileExists(*FileName))) return;
			Package = CreatePackage(*Path);
			const EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;
			if (Kind == EKind::Enum)
			{
				auto* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*Name), Flags));
				if (Enum) FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
				Asset.Reset(Enum);
			}
			else if (Kind == EKind::Struct)
				Asset.Reset(FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), Flags));
			else if (Kind == EKind::Unsupported)
			{
				if (!Test.TestFalse(TEXT("unsupported fixture class is concrete"), UCurveFloat::StaticClass()->HasAnyClassFlags(CLASS_Abstract))) return;
				Asset.Reset(NewObject<UCurveFloat>(Package, FName(*Name), Flags));
			}
			else
			{
				UClass* Parent = Kind == EKind::SaveGame ? USaveGame::StaticClass()
					: Kind == EKind::Interface ? UInterface::StaticClass()
					: Kind == EKind::Widget ? UUserWidget::StaticClass() : AActor::StaticClass();
				auto* BP = FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(*Name),
					Kind == EKind::Interface ? BPTYPE_Interface : BPTYPE_Normal,
					Kind == EKind::Widget ? UWidgetBlueprint::StaticClass() : UBlueprint::StaticClass(),
					Kind == EKind::Widget ? UWidgetBlueprintGeneratedClass::StaticClass() : UBlueprintGeneratedClass::StaticClass(),
					TEXT("NwiroSaveAcceptance"));
				Asset.Reset(BP);
				if (BP) FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave);
			}
			if (!Test.TestNotNull(TEXT("owned save fixture created"), Asset.Get())) return;
			if (bRegister) FAssetRegistryModule::AssetCreated(Asset.Get());
			Package->MarkAsFullyLoaded();
			if (const auto* BP = Cast<UBlueprint>(Asset.Get()))
				if (!Test.TestEqual(TEXT("owned save Blueprint ready"), BP->Status.GetValue(), BS_UpToDate)) return;
			if (const auto* Struct = Cast<UUserDefinedStruct>(Asset.Get()))
				if (!Test.TestEqual(TEXT("owned save struct ready"), Struct->Status.GetValue(), UDSS_UpToDate)) return;
			bReady = true;
		}
		~FFixture()
		{
			if (!Package) return;
			ForEachObjectWithPackage(Package, [](UObject* Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				return true;
			}, true);
			Package->SetDirtyFlag(false);
			// Release the object graph normally; forced child garbage can invalidate
			// reflected and raw Blueprint frame references at different times.
			Package->MarkAsGarbage();
		}
	};

	struct FState
	{
		UPackage* Package;
		bool bDirty;
		uint32 PackageFlags;
		TMap<FString, TArray<uint8>> Objects;
		explicit FState(UPackage* InPackage) : Package(InPackage), bDirty(InPackage->IsDirty()), PackageFlags(InPackage->GetPackageFlags())
		{
			ForEachObjectWithPackage(Package, [this](UObject* Object)
			{
				TArray<uint8> Bytes;
				FMemoryWriter Header(Bytes);
				uint32 Flags = static_cast<uint32>(Object->GetFlags());
				Header << Flags;
				TArray<uint8> Body;
				FObjectWriter Writer(Object, Body);
				Bytes.Append(Body);
				Objects.Add(Object->GetPathName(), MoveTemp(Bytes));
				return true;
			}, true);
		}
		bool Matches() const
		{
			const FState After(Package);
			if (bDirty != After.bDirty || PackageFlags != After.PackageFlags || Objects.Num() != After.Objects.Num()) return false;
			for (const auto& Object : Objects)
			{
				const auto* Bytes = After.Objects.Find(Object.Key);
				if (!Bytes || Object.Value != *Bytes) return false;
			}
			return true;
		}
	};

	struct FObservedCall
	{
		TArray<FString> Attempts;
		TArray<FString> Saved;
		int32 Loads = 0;
		int32 Compiles = 0;
		FDelegateHandle PreHandle, PostHandle, LoadHandle;
		TArray<TPair<TWeakObjectPtr<UBlueprint>, FDelegateHandle>> CompileHandles;
		FObservedCall()
		{
			PreHandle = UPackage::PreSavePackageWithContextEvent.AddLambda([this](UPackage* Package, FObjectPreSaveContext)
			{ Attempts.Add(Package->GetName()); });
			PostHandle = UPackage::PackageSavedWithContextEvent.AddLambda([this](const FString&, UPackage* Package, FObjectPostSaveContext Context)
			{ if (Context.SaveSucceeded()) Saved.Add(Package->GetName()); });
			LoadHandle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([this](UObject*) { ++Loads; });
			for (TObjectIterator<UBlueprint> It; It; ++It)
				CompileHandles.Emplace(*It, It->OnCompiled().AddLambda([this](UBlueprint*) { ++Compiles; }));
		}
		~FObservedCall()
		{
			UPackage::PreSavePackageWithContextEvent.Remove(PreHandle);
			UPackage::PackageSavedWithContextEvent.Remove(PostHandle);
			FCoreUObjectDelegates::OnAssetLoaded.Remove(LoadHandle);
			for (const auto& Handle : CompileHandles)
				if (Handle.Key.IsValid()) Handle.Key->OnCompiled().Remove(Handle.Value);
		}
	};

	static TSharedPtr<FJsonObject> Call(FAutomationTestBase& Test, const FString& Json)
	{
		const FString Raw = FNwiroToolDispatch::DispatchTool(TEXT("save_asset"), Json);
		Test.AddInfo(TEXT("save_asset response ") + Raw);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result);
		Test.TestTrue(TEXT("save_asset response is JSON"), Result.IsValid());
		return Result;
	}

	static FString Request(UObject* Asset)
	{
		const auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("assetPath"), Asset->GetPathName());
		return Serialize(Json);
	}

	static bool Succeeded(const TSharedPtr<FJsonObject>& Result)
	{
		bool Value = false;
		return Result && Result->TryGetBoolField(TEXT("success"), Value) && Value;
	}

	static void Bool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, bool Expected)
	{
		bool Value = false;
		if (Test.TestTrue(FString(TEXT("save result has bool ")) + Field, Result && Result->TryGetBoolField(Field, Value)))
			Test.TestEqual(FString(TEXT("save result bool ")) + Field, Value, Expected);
	}

	static void Outcome(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result, const TCHAR* Status,
		bool bSuccess, bool bAttempted, bool bSaved, UObject* Observed, bool bBefore, bool bAfter, bool bExists)
	{
		FString Actual;
		Test.TestTrue(TEXT("save result has status"), Result && Result->TryGetStringField(TEXT("status"), Actual));
		Test.TestEqual(TEXT("save result status"), Actual, FString(Status));
		Bool(Test, Result, TEXT("success"), bSuccess);
		Bool(Test, Result, TEXT("writeAttempted"), bAttempted);
		Bool(Test, Result, TEXT("savedToDisk"), bSaved);
		Bool(Test, Result, TEXT("targetStateObserved"), Observed != nullptr);
		if (!Observed)
		{
			for (const TCHAR* Field : { TEXT("package"), TEXT("existsOnDisk"), TEXT("dirtyBefore"), TEXT("dirtyAfter"), TEXT("dirtyPackages") })
			{
				const auto Value = Result ? Result->TryGetField(Field) : nullptr;
				Test.TestTrue(FString(TEXT("unknown save state is null ")) + Field, Value.IsValid() && Value->Type == EJson::Null);
			}
			return;
		}
		Test.TestTrue(TEXT("save result has package"), Result && Result->TryGetStringField(TEXT("package"), Actual));
		Test.TestEqual(TEXT("save result exact package"), Actual, Observed->GetOutermost()->GetName());
		Bool(Test, Result, TEXT("existsOnDisk"), bExists);
		Bool(Test, Result, TEXT("dirtyBefore"), bBefore);
		Bool(Test, Result, TEXT("dirtyAfter"), bAfter);
		const TArray<TSharedPtr<FJsonValue>>* Dirty = nullptr;
		if (Test.TestTrue(TEXT("save result has observed dirty packages"), Result && Result->TryGetArrayField(TEXT("dirtyPackages"), Dirty)))
		{
			Test.TestEqual(TEXT("save dirty packages only target scope"), Dirty->Num(), bAfter ? 1 : 0);
			if (Dirty->Num() == 1) Test.TestEqual(TEXT("save dirty package identity"), (*Dirty)[0]->AsString(), Observed->GetOutermost()->GetName());
		}
	}

	static void NoSideEffects(FAutomationTestBase& Test, const FObservedCall& Observation)
	{
		Test.TestEqual(TEXT("save rejection has no save attempt"), Observation.Attempts.Num(), 0);
		Test.TestEqual(TEXT("save rejection has no completed save"), Observation.Saved.Num(), 0);
		Test.TestEqual(TEXT("save call does not load assets"), Observation.Loads, 0);
		Test.TestEqual(TEXT("save call does not compile"), Observation.Compiles, 0);
	}

	static bool RunFailedNoTouch(FAutomationTestBase& Test)
	{
		FContext Context(Test);
		if (!Context.bValid) return false;
		FNeverSaveOnCompile NeverSave;
		TArray<FString> Invalid = { TEXT("{"), TEXT("[]"), TEXT("null"), TEXT("{}"), TEXT("{\"assetPath\":null}"),
			TEXT("{\"assetPath\":42}"), TEXT("{\"assetPath\":\"\"}"), TEXT("{\"path\":\"/Game/Bad.Bad\"}") };
		for (const FString& Path : { FString(TEXT(" ")), FString(TEXT("BareName")), FString(TEXT("/Engine/Bad.Bad")),
			Context.Root + TEXT("Missing"), Context.Root + TEXT("Missing.Missing:Inner"),
			Context.Root + TEXT("../Escape.Escape"), Context.Root + TEXT("Missing.Missing"), FString::ChrN(2048, TEXT('x')) })
		{
			const auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("assetPath"), Path);
			Invalid.Add(Serialize(Json));
		}
		for (const FString& Json : Invalid)
		{
			FObservedCall Observation;
			Outcome(Test, Call(Test, Json), TEXT("rejected"), false, false, false, nullptr, false, false, false);
			NoSideEffects(Test, Observation);
		}
		for (int32 Shape = 0; Shape < 7; ++Shape) for (bool bDirty : { false, true })
		{
			const EKind Kind = Shape == 1 ? EKind::Actor : Shape == 2 ? EKind::Struct : Shape == 3 ? EKind::Unsupported : EKind::Enum;
			FFixture Fixture(Test, Context, FString::Printf(TEXT("Reject_%d_%s"), Shape, bDirty ? TEXT("Dirty") : TEXT("Clean")), Kind);
			if (!Fixture.bReady) return false;
			TStrongObjectPtr<UObject> Second;
			if (Shape == 0) Fixture.Asset->SetFlags(RF_Transient);
			if (Shape == 1) CastChecked<UBlueprint>(Fixture.Asset.Get())->Status = BS_Dirty;
			if (Shape == 2) CastChecked<UUserDefinedStruct>(Fixture.Asset.Get())->Status = UDSS_Error;
			if (Shape == 4) Fixture.Asset->SetFlags(RF_NeedPostLoad);
			if (Shape == 6) Fixture.Asset->ClearFlags(RF_Public);
			if (Shape == 5)
			{
				Second.Reset(FEnumEditorUtils::CreateUserDefinedEnum(Fixture.Package, TEXT("SecondPrimary"), RF_Public | RF_Standalone | RF_Transactional));
				if (!Test.TestNotNull(TEXT("owned second primary asset"), Second.Get())) return false;
				FAssetRegistryModule::AssetCreated(Second.Get());
			}
			Fixture.Package->SetDirtyFlag(bDirty);
			const FState Before(Fixture.Package);
			FObservedCall Observation;
			Outcome(Test, Call(Test, Request(Fixture.Asset.Get())), TEXT("rejected"), false, false, false, Fixture.Asset.Get(), bDirty, bDirty, false);
			NoSideEffects(Test, Observation);
			Test.TestTrue(TEXT("save rejection preserves complete package state"), Before.Matches());
			Test.TestFalse(TEXT("save rejection creates no file"), IFileManager::Get().FileExists(*Fixture.FileName));
			Fixture.Asset->ClearFlags(RF_NeedPostLoad | RF_Transient);
		}
		return true;
	}

	static bool RunOutcomes(FAutomationTestBase& Test)
	{
		FContext Context(Test);
		if (!Context.bValid) return false;
		FNeverSaveOnCompile NeverSave;
		const EKind Kinds[] = { EKind::Enum, EKind::Struct };
		const TCHAR* Names[] = { TEXT("Enum"), TEXT("Struct") };
		for (int32 Kind = 0; Kind < UE_ARRAY_COUNT(Kinds); ++Kind) for (bool bDirty : { false, true })
		{
			FFixture Fixture(Test, Context, FString::Printf(TEXT("Outcome_%s_%s"), Names[Kind], bDirty ? TEXT("Dirty") : TEXT("Clean")), Kinds[Kind]);
			if (!Fixture.bReady) return false;
			Fixture.Package->SetDirtyFlag(bDirty);
			TSharedPtr<FJsonObject> Result;
			{
				FObservedCall Observation;
				Result = Call(Test, Request(Fixture.Asset.Get()));
				Outcome(Test, Result, TEXT("saved"), true, true, true, Fixture.Asset.Get(), bDirty, false, true);
				Test.TestEqual(TEXT("explicit save completed once"), Observation.Saved.Num(), 1);
				if (Observation.Saved.Num() == 1) Test.TestEqual(TEXT("explicit save only target package"), Observation.Saved[0], Fixture.Package->GetName());
				Test.TestEqual(TEXT("explicit save does not load assets"), Observation.Loads, 0);
				Test.TestEqual(TEXT("explicit save does not compile"), Observation.Compiles, 0);
			}
			Test.TestFalse(TEXT("successful save leaves target clean"), Fixture.Package->IsDirty());
			Test.TestTrue(TEXT("successful save writes nonempty file"), IFileManager::Get().FileSize(*Fixture.FileName) > 0);
			if (!Succeeded(Result)) continue;
			TArray<uint8> BeforeBytes, AfterBytes;
			if (!Test.TestTrue(TEXT("saved bytes readable"), FFileHelper::LoadFileToArray(BeforeBytes, *Fixture.FileName))) return false;
			const FState Before(Fixture.Package);
			FObservedCall Observation;
			Outcome(Test, Call(Test, Request(Fixture.Asset.Get())), TEXT("no_op"), true, false, false, Fixture.Asset.Get(), false, false, true);
			NoSideEffects(Test, Observation);
			Test.TestTrue(TEXT("clean save no-op preserves package state"), Before.Matches());
			Test.TestTrue(TEXT("no-op saved bytes readable"), FFileHelper::LoadFileToArray(AfterBytes, *Fixture.FileName));
			Test.TestTrue(TEXT("clean save no-op preserves exact disk bytes"), BeforeBytes == AfterBytes);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSaveAssetFailedNoTouchTest,
	"Nwiro.Pro.Asset.SaveAsset.Production.FailedNoTouch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSaveAssetFailedNoTouchTest::RunTest(const FString&) { return NwiroSaveAssetProof::RunFailedNoTouch(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroSaveAssetOutcomesTest,
	"Nwiro.Pro.Asset.SaveAsset.Production.Outcomes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroSaveAssetOutcomesTest::RunTest(const FString&) { return NwiroSaveAssetProof::RunOutcomes(*this); }
