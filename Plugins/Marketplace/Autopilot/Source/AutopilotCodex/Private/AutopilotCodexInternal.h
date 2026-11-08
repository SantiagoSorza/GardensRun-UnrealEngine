// Shared by the AutopilotCodex commandlet's files:
//
//   AutopilotCodexCommandlet.cpp   Main: finds the project's assets, writes each export file and summary.txt
//   AutopilotCodexText.cpp         text helpers: names, types and property values as text, DiffProps
//   AutopilotCodexBlueprints.cpp   blueprints/*.txt: a Blueprint and its graphs as pseudo-code
//   AutopilotCodexInputData.cpp    input/*.txt and data/*.txt: input assets, enums, structs, curves, data assets
//   AutopilotCodexMaps.cpp         maps.txt: the actors placed in each map

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class AActor;
class FProperty;
class UBlueprint;
class UEnum;
class UInputAction;
class UInputMappingContext;
class UObject;
class UUserDefinedEnum;
class UUserDefinedStruct;

namespace AutopilotCodex
{
	// ---------------------------------------------------------------- limits

	constexpr int32 MaxValueLen = 240;          // one exported value
	constexpr int32 MaxExprLen = 400;           // one rendered expression
	constexpr int32 MaxExprDepth = 8;           // nested pure nodes
	constexpr int32 MaxIndent = 24;             // nested exec branches
	constexpr int32 MaxGraphLines = 800;        // per graph
	constexpr int32 MaxDataGraphLines = 120;    // per graph without exec flow (anim graphs, state rules)
	constexpr int32 MaxBlueprintLines = 8000;   // per Blueprint file
	constexpr int32 MaxComponentProps = 12;
	constexpr int32 MaxClassDefaultProps = 40;
	constexpr int32 MaxWidgetProps = 5;
	constexpr int32 MaxActorsPerClass = 40;
	constexpr int32 MaxActorProps = 8;
	constexpr int32 GcEvery = 20;

	// --- text helpers (AutopilotCodexText.cpp) ------------------------------------------------------------------------

	/** At most Max characters, "..." added when cut. */
	FString Cap(const FString& S, int32 Max);
	/** Line breaks and tabs as spaces, trimmed. */
	FString OneLine(const FString& S);
	/** 180.000000 -> 180, 0.500000 -> 0.5 */
	FString TrimFloats(const FString& S);
	/** Shortens object paths to object names and strips user-defined struct field suffixes. */
	FString Clean(const FString& In, int32 Max = MaxValueLen);
	/** /Game/Foo/Bar -> Foo/Bar; /Plugin/Foo -> Plugin/Foo */
	FString RelPath(const FString& PackageName);

	FString TerminalStr(const FName& Cat, const FName& SubCat, const UObject* Obj);
	FString PinTypeStr(const FEdGraphPinType& T);
	FString PropTypeStr(const FProperty* P);

	FString EnumValueName(const UEnum* E, int64 V);
	FString EnumNameToDisplay(const UEnum* E, const FString& Name);
	/** How an unset pin of this type reads. */
	FString EmptyValue(const FEdGraphPinType& T);
	FString ExportValue(const FProperty* P, const void* V, int32 Max = MaxValueLen);

	/** Which properties DiffProps lists. */
	struct FDiffOptions
	{
		int32 Max = 20;
		int32 MaxLen = MaxValueLen;
		bool bRequireEdit = false;
		bool bInstanceEditableOnly = false;
		bool bBlueprintDeclaredOnly = false;  // only properties declared in a Blueprint class
		bool bSkipOwnClass = false;           // skip properties declared by the object's own class
		bool bSimpleOnly = false;
		bool bExpandStructs = true;
		const TSet<FName>* Skip = nullptr;
		const TSet<FName>* SkipNames = nullptr;
		int32 SkipLongerThan = 0;  // drop values whose full text is longer (opaque engine data), 0 = keep all
	};

	/** Lines "Name=Value" for each property of Obj that differs from Def. */
	int32 DiffProps(const UObject* Obj, const UObject* Def, TArray<FString>& Out, const FDiffOptions& Opt);
	FString JoinProps(const TArray<FString>& Props);

	// ---------------------------------------------------------------- output buffer

	struct FOut
	{
		TArray<FString> Lines;
		int32 Max = MaxBlueprintLines;
		bool bTruncated = false;

		void Add(int32 Indent, const FString& Text)
		{
			if (Lines.Num() >= Max)
			{
				bTruncated = true;
				return;
			}
			Lines.Add(FString::ChrN(Indent * 2, TEXT(' ')) + Text);
		}

		bool Full() const { return Lines.Num() >= Max; }

		FString Join() const
		{
			FString S = FString::Join(Lines, TEXT("\n"));
			if (bTruncated)
			{
				S += TEXT("\n(truncated: output limit reached)");
			}
			return S + TEXT("\n");
		}
	};

	// --- what each kind of asset reads as ---------------------------------------------------------------------------

	FString ExportBlueprint(UBlueprint* BP, const FString& DisplayPath);  // AutopilotCodexBlueprints.cpp

	// AutopilotCodexInputData.cpp
	FString ExportInputAction(UInputAction* IA, const FString& Path);
	FString ExportMappingContext(UInputMappingContext* IMC, const FString& Path);
	FString ExportUserEnum(UUserDefinedEnum* E, const FString& Path);
	FString ExportUserStruct(UUserDefinedStruct* S, const FString& Path);
	FString ExportGenericObject(UObject* O, const FString& Kind, const FString& Path);

	// AutopilotCodexMaps.cpp: a map's actors, one row each
	struct FActorRow
	{
		FString Class;
		FString Name;
		FString Text;
	};

	FString Vec(const FVector& V);
	/** Where an actor is, its tags, and its instance-editable Blueprint properties that differ from its class's. */
	FString ActorDetails(AActor* A);
}
