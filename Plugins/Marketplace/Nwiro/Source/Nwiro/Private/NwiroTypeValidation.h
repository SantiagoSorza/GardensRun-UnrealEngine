// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace NwiroTypeValidation
{
// Resolve identity only. Callers validate Blueprint eligibility and the requested pin/container type.
// Content references must already be loaded: loading an arbitrary Blueprint can assert in PostLoad.
static UObject* ResolveLoadedType(const FString& Requested, UClass* ExpectedKind, FString& Error)
{
	Error.Reset();
	if (ExpectedKind && ExpectedKind != UClass::StaticClass() &&
		ExpectedKind != UScriptStruct::StaticClass() && ExpectedKind != UEnum::StaticClass())
	{
		Error = TEXT("Expected type kind must be class, struct, or enum.");
		return nullptr;
	}
	// Check length before any lookup can intern the caller's text as an FName.
	if (Requested.IsEmpty() || Requested.Len() >= NAME_SIZE ||
		Requested != Requested.TrimStartAndEnd() || Requested.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		Error = TEXT("Type reference must be a nonempty name or object path without surrounding whitespace and shorter than NAME_SIZE.");
		return nullptr;
	}

	for (TCHAR Character : Requested)
	{
		if (FChar::IsControl(Character))
		{
			Error = TEXT("Type reference must not contain control characters.");
			return nullptr;
		}
	}

	if (Requested.StartsWith(TEXT("/")))
	{
		FText PathError;
		if (!Requested.Contains(TEXT(".")) || Requested.Contains(TEXT(":")) ||
			!FPackageName::IsValidObjectPath(Requested, &PathError))
		{
			Error = TEXT("Type reference must be a full object path such as /Script/Engine.Actor or /Game/Types/S_Data.S_Data.");
			return nullptr;
		}
		UObject* Found = FindObjectSafe<UObject>(nullptr, *Requested);
		if (!IsValid(Found))
		{
			Error = FString::Printf(TEXT("Type '%s' is not loaded or does not exist; use a loaded type's exact object path."), *Requested);
			return nullptr;
		}
		if ((!Found->IsA<UClass>() && !Found->IsA<UScriptStruct>() && !Found->IsA<UEnum>()) ||
			(ExpectedKind && !Found->IsA(ExpectedKind)))
		{
			Error = FString::Printf(TEXT("Object '%s' is not a %s type."), *Requested,
				ExpectedKind ? *ExpectedKind->GetName() : TEXT("class, struct, or enum"));
			return nullptr;
		}
		return Found;
	}

	if (!FName::IsValidXName(Requested, INVALID_OBJECTNAME_CHARACTERS))
	{
		Error = TEXT("Type reference must be a valid short object name or a full object path.");
		return nullptr;
	}

	// Accept C++ prefixes without making them a first-match shortcut. If both names resolve
	// to different types, the caller must disambiguate with an exact object path.
	UClass* PrefixKind = nullptr;
	if (Requested.Len() > 1 && FChar::IsUpper(Requested[1]))
	{
		if (Requested[0] == TEXT('U') || Requested[0] == TEXT('A')) PrefixKind = UClass::StaticClass();
		else if (Requested[0] == TEXT('F')) PrefixKind = UScriptStruct::StaticClass();
		else if (Requested[0] == TEXT('E')) PrefixKind = UEnum::StaticClass();
	}
	const FString Unprefixed = PrefixKind ? Requested.Mid(1) : FString();
	UObject* Found = nullptr;
	bool bAmbiguous = false;
	UClass* Kinds[] = {UClass::StaticClass(), UScriptStruct::StaticClass(), UEnum::StaticClass()};
	for (UClass* Kind : Kinds)
	{
		if (ExpectedKind && ExpectedKind != Kind) continue;
		ForEachObjectOfClass(Kind, [&](UObject* Candidate)
		{
			if (!IsValid(Candidate)) return;
			const FString CandidateName = Candidate->GetName();
			// A compiled, loaded Blueprint's asset name is a class alias. Match it
			// in the same candidate set, so duplicate assets or reflected names
			// cannot silently select whichever object the iterator visits first.
			bool bBlueprintAlias = false;
			if (UClass* Class = Cast<UClass>(Candidate))
			{
				const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
				const bool bTemporaryClassName = CandidateName.StartsWith(TEXT("SKEL_"), ESearchCase::IgnoreCase) ||
					CandidateName.StartsWith(TEXT("REINST_"), ESearchCase::IgnoreCase) ||
					CandidateName.StartsWith(TEXT("TRASHCLASS_"), ESearchCase::IgnoreCase) ||
					CandidateName.StartsWith(TEXT("HOTRELOADED_"), ESearchCase::IgnoreCase);
				bBlueprintAlias = IsValid(Blueprint) && !Blueprint->HasAnyFlags(RF_Transient) &&
					!Class->HasAnyFlags(RF_Transient) && !bTemporaryClassName && Blueprint->GeneratedClass == Class &&
					Blueprint->SkeletonGeneratedClass != Class && Blueprint->IsUpToDate() &&
					!Blueprint->bBeingCompiled && !Blueprint->bIsRegeneratingOnLoad &&
					!Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated) &&
					Blueprint->GetName().Equals(Requested, ESearchCase::IgnoreCase);
			}
			if (bBlueprintAlias || CandidateName.Equals(Requested, ESearchCase::IgnoreCase) ||
				(Kind == PrefixKind && CandidateName.Equals(Unprefixed, ESearchCase::IgnoreCase)))
			{
				if (Found && Found != Candidate) bAmbiguous = true;
				else Found = Candidate;
			}
		});
	}
	if (bAmbiguous)
	{
		Error = FString::Printf(TEXT("Type name '%s' is ambiguous; use its full object path."), *Requested);
		return nullptr;
	}
	if (!Found)
	{
		Error = FString::Printf(TEXT("Type '%s' is not loaded or does not exist; use a loaded type's exact object path."), *Requested);
	}
	return Found;
}
}
