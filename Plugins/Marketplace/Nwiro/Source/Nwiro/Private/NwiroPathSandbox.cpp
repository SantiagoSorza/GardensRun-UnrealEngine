// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroPathSandbox.h"
#include "Misc/Paths.h"

FString NwiroPathSandbox::TryResolveSandboxed(const FString& InputPath)
{
	if (InputPath.IsEmpty())
	{
		return FString();
	}

	// Always combine with ProjectDir — do NOT branch on IsRelative.
	// Absolute inputs that already live under ProjectDir survive the
	// IsUnderDirectory check below; absolute inputs outside ProjectDir
	// (e.g. /etc/passwd, C:\Windows\...) fold onto a path that resolves
	// outside ProjectDir and get rejected.
	const FString Combined = FPaths::Combine(FPaths::ProjectDir(), InputPath);
	const FString Resolved = FPaths::ConvertRelativePathToFull(Combined);
	const FString ProjectDirFull = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	if (!FPaths::IsUnderDirectory(Resolved, ProjectDirFull))
	{
		return FString();
	}

	return Resolved;
}

FString NwiroPathSandbox::ScgRunsRoot()
{
	// ONE definition of the run root. If the run folder ever relocates, change
	// only this line.
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved/Nwiro/SceneGeneration/runs")));
}

FString NwiroPathSandbox::TryResolveScgRunPath(const FString& RelPath)
{
	if (RelPath.IsEmpty())
	{
		return FString();
	}

	// Defense in depth: reject absolute inputs and drive/stream specifiers
	// OUTRIGHT rather than trusting FPaths::Combine to fold them outside the
	// root (its handling of an absolute second component is platform-
	// dependent). A ':' catches Windows drive letters (C:\) and NTFS alternate
	// data streams (file.png:evil). "../" traversal is still caught below by
	// the resolve + IsUnderDirectory backstop.
	if (!FPaths::IsRelative(RelPath) || RelPath.Contains(TEXT(":")))
	{
		return FString();
	}

	const FString Root = ScgRunsRoot();
	const FString Resolved = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, RelPath));

	// Confine to the run root. A "../" escape or an absolute input resolves
	// outside Root and is rejected here (same mechanism as TryResolveSandboxed,
	// tighter boundary).
	if (!FPaths::IsUnderDirectory(Resolved, Root))
	{
		return FString();
	}

	return Resolved;
}

bool NwiroPathSandbox::TrySplitScgRunPath(const FString& FullPath, FString& OutRunId, FString& OutRelPath)
{
	// Cleared before every failure return: a caller that ignores the bool must
	// not read a run id left behind by an earlier call.
	OutRunId.Reset();
	OutRelPath.Reset();

	// The parameter is named FullPath and the contract says absolute. Resolving a
	// relative input would silently answer about whatever directory the editor
	// happened to be launched from.
	if (FullPath.IsEmpty() || FPaths::IsRelative(FullPath))
	{
		return false;
	}

	// ConvertRelativePathToFull runs NormalizeFilename, which folds '\' to '/'.
	// That is what lets a native path straight out of the OS file dialog be
	// compared against the root without any separator handling here.
	const FString Full = FPaths::ConvertRelativePathToFull(FullPath);
	FString Root = FPaths::ConvertRelativePathToFull(ScgRunsRoot());
	if (Root.EndsWith(TEXT("/")))
	{
		Root.RemoveAt(Root.Len() - 1);
	}

	// IsUnderDirectory carries the per-platform case rule (insensitive on
	// Microsoft, sensitive elsewhere) and requires a separator at the boundary,
	// so ".../runs2/x.png" cannot pass as living under ".../runs".
	if (!FPaths::IsUnderDirectory(Full, Root))
	{
		return false;
	}

	// Chop by LENGTH rather than by matching the prefix: on Windows the two may
	// differ in case, and the remainder must carry the file's own spelling.
	FString Remainder = Full.RightChop(Root.Len());
	while (Remainder.StartsWith(TEXT("/")))
	{
		Remainder.RemoveAt(0);
	}

	TArray<FString> Segments;
	Remainder.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/true);

	// One segment means a file sitting directly in the runs folder: it is under
	// the root, but there is no run to name it against, so it is not an artifact.
	if (Segments.Num() < 2)
	{
		return false;
	}

	const FString RunId = Segments[0];
	Segments.RemoveAt(0);
	const FString RelPath = FString::Join(Segments, TEXT("/"));

	// The claim "this is the inverse of TryResolveScgRunPath" is ENFORCED here
	// rather than asserted in a comment: feed the answer back through the
	// resolver and require it to land on the path we were given.
	//
	// This is what keeps the two functions from drifting apart, and it does so
	// without restating a single one of the resolver's rules. A concrete case it
	// catches today: on Linux and macOS a ':' is a legal filename character, so
	// the split above happily produces one — but the resolver rejects every ':'
	// (drive letters, NTFS streams). Without this check the caller would be
	// handed a reference that can never be read back.
	//
	// IsSamePath, not '==': on Windows the root's casing may differ from the
	// picked path's, which is exactly the case the length-based chop above
	// exists to support, and a string compare would reject it.
	// The empty case is rejected on its own rather than handed to IsSamePath: an
	// empty string is a RELATIVE path, so IsSamePath would resolve it against the
	// process base directory and compare that instead of failing. Nothing here can
	// coincide with the base directory today, but the check must not depend on it.
	const FString Resolved = TryResolveScgRunPath(RunId + TEXT("/") + RelPath);
	if (Resolved.IsEmpty() || !FPaths::IsSamePath(Resolved, Full))
	{
		return false;
	}

	OutRunId = RunId;
	OutRelPath = RelPath;
	return true;
}
