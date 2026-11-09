// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroArchiveSafety.h"

#include "Misc/Paths.h"

namespace NwiroArchiveSafety
{
	bool IsSafeMemberName(const FString& EntryName)
	{
		if (EntryName.IsEmpty()) return false;

		// Treat backslash as a separator, because the extractors do. Validating only the
		// forward-slash spelling would hand a helper a traversal it then performs.
		FString Normalized = EntryName;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

		// Control characters, including an embedded NUL, have no legitimate place in a
		// member name and are a classic way to confuse downstream path handling.
		for (const TCHAR C : Normalized)
		{
			if (C < 32) return false;
		}

		// Absolute in any form: POSIX root, UNC ("//server/share" also starts with "/"),
		// or a drive letter. FPaths::Combine with an absolute second argument discards the
		// destination entirely, which is the whole exposure.
		if (Normalized.StartsWith(TEXT("/"))) return false;
		if (Normalized.Len() >= 2 && Normalized[1] == TEXT(':')) return false;

		// Any ".." SEGMENT. Segment-wise so "codex-acp..backup.exe" survives while
		// "a/../b" and a bare ".." do not.
		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty*/ false);
		for (const FString& Segment : Segments)
		{
			if (Segment == TEXT("..")) return false;
		}
		return true;
	}

	bool IsContainedWithin(const FString& RootDir, const FString& CandidatePath)
	{
		if (RootDir.IsEmpty() || CandidatePath.IsEmpty()) return false;

		FString Root = FPaths::ConvertRelativePathToFull(RootDir);
		FString Candidate = FPaths::ConvertRelativePathToFull(CandidatePath);

		// Resolve any ".." that survived, so this compares destinations rather than
		// spellings. A path that cannot be collapsed is refused rather than guessed at.
		if (!FPaths::CollapseRelativeDirectories(Root)) return false;
		if (!FPaths::CollapseRelativeDirectories(Candidate)) return false;

		FPaths::NormalizeDirectoryName(Root);
		FPaths::NormalizeFilename(Candidate);

		// The trailing separator is what stops "C:/Out" from appearing to contain
		// "C:/OutSideways/x" — a prefix match without it accepts any sibling whose name
		// merely begins with the root's.
		if (!Root.EndsWith(TEXT("/"))) Root += TEXT("/");

		// Case-insensitive: this guards Windows, where two spellings are one file.
		return Candidate.StartsWith(Root, ESearchCase::IgnoreCase);
	}
}
