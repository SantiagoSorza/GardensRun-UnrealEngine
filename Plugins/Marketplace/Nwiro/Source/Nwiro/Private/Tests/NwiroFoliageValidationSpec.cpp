// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroLevelTools.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "NwiroToolDispatch.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "InstancedFoliageActor.h"
#include "Json.h"
#include "Misc/AutomationTest.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

namespace NwiroFoliageValidationProof
{
	struct FSnapshot
	{
		TArray<AActor*> Actors;
		TArray<FString> Foliage;
		TMap<UPackage*, bool> Dirty;
		explicit FSnapshot(UWorld* World)
		{
			if (!World) return;
			Dirty.Add(World->GetPackage(), World->GetPackage()->IsDirty());
			if (World->GetCurrentLevel()) Dirty.Add(World->GetCurrentLevel()->GetPackage(), World->GetCurrentLevel()->GetPackage()->IsDirty());
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				Actors.Add(*It);
				if (AInstancedFoliageActor* IFA = Cast<AInstancedFoliageActor>(*It))
				{
					Dirty.Add(IFA->GetPackage(), IFA->GetPackage()->IsDirty());
					for (const auto& Pair : IFA->GetFoliageInfos())
						Foliage.Add(IFA->GetPathName() + TEXT("|") + GetPathNameSafe(Pair.Key)
							+ TEXT("|") + FString::FromInt(Pair.Value->GetPlacedInstanceCount()));
				}
			}
			Foliage.Sort();
		}
		bool Matches(UWorld* World) const
		{
			const FSnapshot Now(World);
			if (Actors != Now.Actors || Foliage != Now.Foliage || Dirty.Num() != Now.Dirty.Num()) return false;
			for (const auto& Pair : Dirty)
			{
				const bool* Current = Now.Dirty.Find(Pair.Key);
				if (!Current || *Current != Pair.Value) return false;
			}
			return true;
		}
	};

	static bool CheckCoordinateSchemas(FAutomationTestBase& Test)
	{
		// Read the actual registry through UE's JSON DOM and serialize it again, like tools/list.
		// Case-insensitive FindField is not a valid oracle for the spelling sent to JSON Schema clients.
		TArray<TSharedPtr<FJsonValue>> Definitions;
		if (!Test.TestTrue(TEXT("foliage registry parses"), FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(FNwiroToolDispatch::GetEnabledToolDefinitionsJson()), Definitions))) return false;
		FString Serialized;
		if (!Test.TestTrue(TEXT("foliage registry serializes"), FJsonSerializer::Serialize(Definitions, TJsonWriterFactory<>::Create(&Serialized)))) return false;
		Definitions.Reset();
		if (!Test.TestTrue(TEXT("foliage registry round-trip parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Serialized), Definitions))) return false;
		bool bValid = true;
		int32 Found = 0;
		for (const TSharedPtr<FJsonValue>& Definition : Definitions)
		{
			if (!Definition.IsValid() || Definition->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> Tool = Definition->AsObject();
			FString Name;
			if (!Tool->TryGetStringField(TEXT("name"), Name) || (Name != TEXT("paint_foliage") && Name != TEXT("add_foliage_type"))) continue;
			++Found;
			const TSharedPtr<FJsonObject>* Schema = nullptr;
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (!Test.TestTrue(Name + TEXT(" has schema properties"), Tool->TryGetObjectField(TEXT("inputSchema"), Schema)
				&& (*Schema)->TryGetObjectField(TEXT("properties"), Fields))) { bValid = false; continue; }
			for (const TCHAR* Field : { TEXT("location"), TEXT("center") })
			{
				const FString Label = Name + TEXT(".") + Field;
				const TSharedPtr<FJsonObject>* Coordinate = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Branches = nullptr;
				if (!Test.TestTrue(Label + TEXT(" has anyOf branches after UE DOM round-trip"), (*Fields)->TryGetObjectField(Field, Coordinate)
					&& (*Coordinate)->TryGetArrayField(TEXT("anyOf"), Branches))) { bValid = false; continue; }
				bValid &= Test.TestEqual(Label + TEXT(" has array and eight casing alternatives"), Branches->Num(), 9);
				bool bArray = false;
				for (const TSharedPtr<FJsonValue>& Branch : *Branches)
				{
					if (!Branch.IsValid() || Branch->Type != EJson::Object) continue;
					const TSharedPtr<FJsonObject> Object = Branch->AsObject();
					FString Type;
					const TSharedPtr<FJsonObject>* Items = nullptr;
					double Min = 0, Max = 0;
					if (Object->TryGetStringField(TEXT("type"), Type) && Type == TEXT("array")
						&& Object->TryGetNumberField(TEXT("minItems"), Min) && Min == 3
						&& Object->TryGetNumberField(TEXT("maxItems"), Max) && Max == 3
						&& Object->TryGetObjectField(TEXT("items"), Items)
						&& (*Items)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("number")) bArray = true;
				}
				bValid &= Test.TestTrue(Label + TEXT(" keeps exact numeric array support"), bArray);
				for (int32 Mask = 0; Mask < 8; ++Mask)
				{
					const TArray<FString> Expected = { (Mask & 1) ? TEXT("X") : TEXT("x"), (Mask & 2) ? TEXT("Y") : TEXT("y"), (Mask & 4) ? TEXT("Z") : TEXT("z") };
					bool bCaseFound = false;
					for (const TSharedPtr<FJsonValue>& Branch : *Branches)
					{
						if (!Branch.IsValid() || Branch->Type != EJson::Object) continue;
						const TSharedPtr<FJsonObject> Object = Branch->AsObject();
						FString Type;
						const TSharedPtr<FJsonObject>* Properties = nullptr;
						bool bAdditional = true;
						double Minimum = 0;
						if (!Object->TryGetStringField(TEXT("type"), Type) || Type != TEXT("object")
							|| !Object->TryGetObjectField(TEXT("properties"), Properties) || (*Properties)->Values.Num() != 3
							|| !Object->TryGetBoolField(TEXT("additionalProperties"), bAdditional) || bAdditional
							|| !Object->TryGetNumberField(TEXT("minProperties"), Minimum) || Minimum != 1
							|| Object->HasField(TEXT("required"))) continue;
						int32 Matches = 0;
						for (const auto& Pair : (*Properties)->Values)
						{
							const FString Key(*Pair.Key);
							if (!Expected.ContainsByPredicate([&](const FString& Candidate) { return Candidate.Equals(Key, ESearchCase::CaseSensitive); })) continue;
							if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object
								&& Pair.Value->AsObject()->TryGetStringField(TEXT("type"), Type) && Type == TEXT("number")) ++Matches;
						}
						if (Matches == 3) bCaseFound = true;
					}
					bValid &= Test.TestTrue(Label + TEXT(" preserves partial coordinate casing ") + FString::Join(Expected, TEXT("/")), bCaseFound);
				}
			}
		}
		bValid &= Test.TestEqual(TEXT("both actual foliage registry entries inspected"), Found, 2);
		return bValid;
	}

	static void Reject(FAutomationTestBase& Test, const FString& Json, bool bPaint, UWorld* World, const FSnapshot& Before)
	{
		const FString Label = (bPaint ? TEXT("Paint ") : TEXT("Add ")) + Json + TEXT(": ");
		const FString Raw = bPaint ? FNwiroLevelTools::PaintFoliage(Json) : FNwiroLevelTools::AddFoliageType(Json);
		TSharedPtr<FJsonObject> Reply;
		if (!Test.TestTrue(Label + TEXT("rejection is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Reply) && Reply.IsValid())) return;
		bool Success = true, Mutated = true;
		double Painted = -1;
		FString Error;
		Test.TestTrue(Label + TEXT("invalid request reports failure"), Reply->HasTypedField<EJson::Boolean>(TEXT("success")) && Reply->TryGetBoolField(TEXT("success"), Success) && !Success);
		Test.TestTrue(Label + TEXT("invalid request reports no mutation"), Reply->HasTypedField<EJson::Boolean>(TEXT("mutationStarted")) && Reply->TryGetBoolField(TEXT("mutationStarted"), Mutated) && !Mutated);
		Test.TestTrue(Label + TEXT("invalid request places zero instances"), Reply->HasTypedField<EJson::Number>(TEXT("painted")) && Reply->TryGetNumberField(TEXT("painted"), Painted) && Painted == 0);
		Test.TestTrue(Label + TEXT("actionable error is nonempty"), Reply->HasTypedField<EJson::String>(TEXT("error")) && Reply->TryGetStringField(TEXT("error"), Error) && !Error.IsEmpty());
		Test.TestTrue(Label + TEXT("invalid request preserves actor/type/instance counts and package dirtiness"), Before.Matches(World));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroFoliageValidation,
	"Nwiro.Pro.Level.Foliage.InvalidRequestsNoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroFoliageValidation::RunTest(const FString& Parameters)
{
	using namespace NwiroFoliageValidationProof;
	if (!CheckCoordinateSchemas(*this)) return false;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FSnapshot Before(World);
	int32 Loads = 0, Saves = 0;
	const FDelegateHandle OnLoad = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject*) { ++Loads; });
	const FDelegateHandle OnSave = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject*, FObjectPreSaveContext) { ++Saves; });
	// Every request is rejected during preparation/metadata resolution; this fixture never writes the current world.
	const TCHAR* PaintCases[] = {
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":0}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":-1}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":0.5}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":5001}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":\"3\"}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":true}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":null}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"density\":\"3\"}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"radius\":true}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"radius\":null}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"brushRadius\":\"50\"}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[\"1\",2,3]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[true,2,3]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[null,2,3]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":\"1\"}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"y\":false}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"z\":null}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"x\":\"1\"}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"y\":true}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"z\":null}"),
		TEXT("{\"mesh\":true}"),
		TEXT("{\"mesh\":null}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"count\":2,\"instances\":3}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"radius\":-1}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"radius\":\"50\"}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[1,2]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[1,2,3,4]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":\"bad\"}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"typo\":1}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[0,0,0],\"center\":[1,0,0]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[0,0,0],\"x\":1}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"x\":1e30}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"meshPath\":\"/Engine/BasicShapes/Sphere.Sphere\"}"),
		TEXT("{\"mesh\":\"/Game/DefinitelyMissingNwiroFoliageProof.DefinitelyMissingNwiroFoliageProof\"}"),
		TEXT("{\"mesh\":\"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial\"}"),
		TEXT("{\"mesh\":42}"), TEXT("{}"), TEXT("[]")
	};
	for (const TCHAR* Json : PaintCases) Reject(*this, Json, true, World, Before);
	const TCHAR* AddCases[] = {
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[1,2]}"),
		// Differently cased keys collapse in UE JSON storage; use distinct aliases to test conflicts.
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":1},\"center\":{\"x\":2}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":[\"1\",2,3]}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":true}}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"z\":null}"),
		TEXT("{\"mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"x\":1e30}"),
		TEXT("{\"mesh\":\"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial\"}"),
		TEXT("{\"mesh\":\"/Game/DefinitelyMissingNwiroFoliageProof.DefinitelyMissingNwiroFoliageProof\"}"),
		TEXT("{\"mesh\":42}"), TEXT("{}")
	};
	for (const TCHAR* Json : AddCases) Reject(*this, Json, false, World, Before);
	FCoreUObjectDelegates::OnAssetLoaded.Remove(OnLoad);
	FCoreUObjectDelegates::OnObjectPreSave.Remove(OnSave);
	TestEqual(TEXT("invalid requests load no assets"), Loads, 0);
	TestEqual(TEXT("invalid requests save no assets"), Saves, 0);
	return !HasAnyErrors();
}
#endif
