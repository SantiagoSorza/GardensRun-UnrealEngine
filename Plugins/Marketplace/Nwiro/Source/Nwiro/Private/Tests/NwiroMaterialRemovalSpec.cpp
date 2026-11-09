// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroMaterialTools.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Json.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/AutomationTest.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace NwiroMaterialRemovalProof
{
	static TSharedPtr<FJsonObject> Call(const TSharedRef<FJsonObject>& Request, bool bCompound = false)
	{
		FString Input;
		FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Input));
		const FString Output = bCompound ? FNwiroMaterialTools::EditMaterial(Input) : FNwiroMaterialTools::DeleteExpression(Input);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Output), Result);
		return Result;
	}
	static bool Success(const TSharedPtr<FJsonObject>& Result)
	{
		bool Value = false;
		return Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Value) && Value;
	}
	struct FFixture
	{
		TStrongObjectPtr<UMaterial> Material;
		FFixture()
		{
			const FString Name = TEXT("M_Removal_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Material.Reset(NewObject<UMaterial>(CreatePackage(*(TEXT("/Game/NwiroRemovalProof/") + Name)), *Name, RF_Public | RF_Standalone | RF_Transactional));
		}
		~FFixture()
		{
			FNwiroMaterialTools::ClearExpressionRefs();
			Material->GetPackage()->SetDirtyFlag(false);
			Material->ClearFlags(RF_Public | RF_Standalone);
			Material->MarkAsGarbage();
		}
		UMaterialExpressionScalarParameter* Add(const TCHAR* Parameter)
		{
			auto* Expr = CastChecked<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionScalarParameter::StaticClass()));
			Expr->ParameterName = Parameter;
			return Expr;
		}
		TSharedRef<FJsonObject> Request(const TArray<FString>& Refs, const TCHAR* Key = TEXT("refs")) const
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("material"), Material->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Ref : Refs) Values.Add(MakeShared<FJsonValueString>(Ref));
			Result->SetArrayField(Key, Values);
			return Result;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMaterialRemovalRejected,
	"Nwiro.Pro.Material.Removal.FailedBatchNoMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroMaterialRemovalRejected::RunTest(const FString& Parameters)
{
	using namespace NwiroMaterialRemovalProof;
	FFixture Fixture, Foreign;
	auto* Keep = Fixture.Add(TEXT("Keep"));
	auto* SameA = Fixture.Add(TEXT("Ambiguous"));
	auto* SameB = Fixture.Add(TEXT("Ambiguous"));
	auto* Other = Foreign.Add(TEXT("Foreign"));
	SameB->GetMaterialExpressionId() = SameA->GetMaterialExpressionId(); // corrupted/duplicated GUID must be ambiguous, never first-match
	Fixture.Material->GetExpressionInputForProperty(MP_BaseColor)->Expression = Keep;
	const FGuid MaterialState = Fixture.Material->StateId;
	int32 Saves = 0, Modifications = 0;
	const FDelegateHandle ModifyHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object)
	{
		if (Object->GetPackage() == Fixture.Material->GetPackage() || Object->GetPackage() == Foreign.Material->GetPackage()) ++Modifications;
	});
	const FDelegateHandle SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject*, FObjectPreSaveContext) { ++Saves; });
	for (bool bDirty : {false, true})
	{
		TArray<TSharedRef<FJsonObject>> Requests = {
			Fixture.Request({TEXT("Keep"), TEXT("Missing_0908")}),
			Fixture.Request({TEXT("Keep"), Other->GetPathName()}),
			Fixture.Request({TEXT("Ambiguous")}),
			Fixture.Request({SameA->GetMaterialExpressionId().ToString()}),
			Fixture.Request({TEXT("Keep"), Keep->GetPathName()}),
			Fixture.Request({}), Fixture.Request({TEXT("")}), Fixture.Request({TEXT(" Keep ")})
		};
		auto WrongType = Fixture.Request({TEXT("Keep")});
		WrongType->SetArrayField(TEXT("refs"), {MakeShared<FJsonValueString>(TEXT("Keep")), MakeShared<FJsonValueNumber>(42)});
		Requests.Add(WrongType);
		for (const auto& Request : Requests)
		{
			Fixture.Material->GetPackage()->SetDirtyFlag(bDirty);
			auto Reply = Call(Request);
			TestFalse(TEXT("invalid batch rejected"), Success(Reply));
			TestTrue(TEXT("structured no-mutation response"), Reply.IsValid() && Reply->HasTypedField<EJson::Boolean>(TEXT("mutationStarted")) && !Reply->GetBoolField(TEXT("mutationStarted")));
			TestEqual(TEXT("all target expressions retained"), Fixture.Material->GetExpressionCollection().Expressions.Num(), 3);
			TestTrue(TEXT("connection retained"), Fixture.Material->GetExpressionInputForProperty(MP_BaseColor)->Expression == Keep);
			TestTrue(TEXT("foreign expression retained"), Foreign.Material->GetExpressionCollection().Expressions.Contains(Other));
			TestEqual(TEXT("dirty state retained"), Fixture.Material->GetPackage()->IsDirty(), bDirty);
			TestTrue(TEXT("no material recompile"), Fixture.Material->StateId == MaterialState);
		}
		Fixture.Material->GetPackage()->SetDirtyFlag(bDirty);
		auto Compound = Fixture.Request({TEXT("Keep"), TEXT("Missing_0908")}, TEXT("remove_expressions"));
		auto Properties = MakeShared<FJsonObject>(); Properties->SetBoolField(TEXT("two_sided"), true);
		Compound->SetObjectField(TEXT("set_properties"), Properties);
		auto NewExpression = MakeShared<FJsonObject>(); NewExpression->SetStringField(TEXT("type"), TEXT("Constant")); NewExpression->SetStringField(TEXT("ref"), TEXT("MustNotCreate"));
		Compound->SetArrayField(TEXT("add_expressions"), {MakeShared<FJsonValueObject>(NewExpression)});
		TestFalse(TEXT("invalid removal rejects compound edit before properties/add/recompile"), Success(Call(Compound, true)));
		TestFalse(TEXT("material property unchanged"), Fixture.Material->TwoSided != 0);
		TestEqual(TEXT("no expression added or removed"), Fixture.Material->GetExpressionCollection().Expressions.Num(), 3);
		TestEqual(TEXT("compound rejection preserves dirtiness"), Fixture.Material->GetPackage()->IsDirty(), bDirty);
		TestTrue(TEXT("compound rejection does not recompile"), Fixture.Material->StateId == MaterialState);
	}
	TestEqual(TEXT("no implicit saves"), Saves, 0);
	TestEqual(TEXT("rejections do not call Modify"), Modifications, 0);
	FCoreUObjectDelegates::OnObjectModified.Remove(ModifyHandle);
	FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMaterialRemovalExact,
	"Nwiro.Pro.Material.Removal.OwnedReferencesWithoutCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroMaterialRemovalExact::RunTest(const FString& Parameters)
{
	using namespace NwiroMaterialRemovalProof;
	for (int32 Route = 0; Route < 5; ++Route)
	{
		FFixture Fixture;
		auto* Remove = Fixture.Add(TEXT("ParameterToRemove"));
		auto* Keep = Fixture.Add(TEXT("Keep"));
		FString Ref;
		switch (Route)
		{
		case 0: Ref = Remove->GetName(); break;
		case 1: Ref = Remove->GetName().Mid(18); break;
		case 2: Ref = Remove->GetPathName(); break;
		case 3: Ref = Remove->GetParameterName().ToString(); break;
		default: Ref = Remove->GetMaterialExpressionId().ToString(); break;
		}
		FNwiroMaterialTools::ClearExpressionRefs();
		Fixture.Material->GetPackage()->SetDirtyFlag(false);
		TestTrue(TEXT("exact owned reference resolves without inspection/cache"), Success(Call(Fixture.Request({Ref}))));
		TestEqual(TEXT("exactly one expression removed"), Fixture.Material->GetExpressionCollection().Expressions.Num(), 1);
		TestTrue(TEXT("unselected expression retained"), Fixture.Material->GetExpressionCollection().Expressions.Contains(Keep));
		TestTrue(TEXT("valid deletion dirties material"), Fixture.Material->GetPackage()->IsDirty());
		TestFalse(TEXT("already removed expression fails, not Removed0 success"), Success(Call(Fixture.Request({Ref}))));
	}

	// The compound tool creates a non-parameter alias; ownership must hold even when another material is inspected.
	FFixture Owned, Foreign;
	auto AddAlias = [&](FFixture& Fixture, const FString& Alias)
	{
		auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("material"), Fixture.Material->GetPathName());
		auto Expression = MakeShared<FJsonObject>(); Expression->SetStringField(TEXT("type"), TEXT("Constant")); Expression->SetStringField(TEXT("ref"), Alias);
		Request->SetArrayField(TEXT("add_expressions"), {MakeShared<FJsonValueObject>(Expression)});
		Request->SetBoolField(TEXT("recompile"), false);
		return Success(Call(Request, true));
	};
	TestTrue(TEXT("create owned non-parameter cache alias"), AddAlias(Owned, TEXT("OwnedAlias")));
	TestTrue(TEXT("valid cached alias deletes its owned expression"), Success(Call(Owned.Request({TEXT("OwnedAlias")}))));
	TestEqual(TEXT("cached alias removed exactly its expression"), Owned.Material->GetExpressionCollection().Expressions.Num(), 0);
	TestTrue(TEXT("create foreign non-parameter cache alias"), AddAlias(Foreign, TEXT("ForeignAlias")));
	Owned.Material->GetPackage()->SetDirtyFlag(false);
	TestFalse(TEXT("foreign cached alias cannot delete another material's expression"), Success(Call(Owned.Request({TEXT("ForeignAlias")}))));
	TestEqual(TEXT("foreign cached expression survives"), Foreign.Material->GetExpressionCollection().Expressions.Num(), 1);
	TestFalse(TEXT("foreign alias rejection does not dirty target"), Owned.Material->GetPackage()->IsDirty());
	return true;
}
#endif
