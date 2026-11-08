// Copyright 2026 Nwiro. All Rights Reserved.
//
// File-tool path sandbox for the Nwiro main plugin.
//
// Closes the absolute-path bypass present in the original file-tool
// resolution code (write_file, read_file, delete_file, rename_file).
//
// The bypass: the previous code branched on `FPaths::IsRelative(InputPath)`.
// For inputs like "/etc/passwd" or "C:\\Windows\\..." this returns false, so
// ProjectDir was NOT prepended and the absolute path was used as-is.
// Result: tool calls could touch any file on disk an LLM could name.
//
// Fix: unconditionally combine with ProjectDir, then verify the resolved
// path lives under ProjectDir via FPaths::IsUnderDirectory. Absolute paths
// inside the project still work; absolute paths outside fail.
//
// Audit confirmation (run before IK-PR-3 merge): all file-tool handlers in
// both plugins consume the resolved path via FFileHelper / IFileManager
// (pure filesystem). No tool routes file_path through the content registry
// or asset loader. Safe to apply the sandbox uniformly.

#pragma once

#include "CoreMinimal.h"

namespace NwiroPathSandbox
{
	/**
	 * Resolve InputPath to an absolute filesystem path confined to ProjectDir.
	 * Returns the safe absolute path on success, or an empty FString if the
	 * resolved path escapes ProjectDir.
	 *
	 * Callers must treat empty return as "deny this operation".
	 */
	FString TryResolveSandboxed(const FString& InputPath);

	/**
	 * The SCG run-folder root: <project>/Saved/Nwiro/SceneGeneration/runs (absolute).
	 * Decided 2026-07-22. Kept in ONE place so a future relocation is one edit.
	 */
	FString ScgRunsRoot();

	/**
	 * Resolve a run-relative path (e.g. "run_x/outputs/generated.png") to an
	 * absolute path confined to ScgRunsRoot(). TIGHTER than TryResolveSandboxed:
	 * the SCG artifact tools must never touch anything outside the run root,
	 * even elsewhere in the project. Rejects absolute inputs and "../" escapes.
	 * Empty return = deny.
	 */
	FString TryResolveScgRunPath(const FString& RelPath);

	/**
	 * The INVERSE of TryResolveScgRunPath: an absolute filesystem path to the run
	 * it belongs to. Returns false — with both outputs cleared — when the path is
	 * relative, lies outside ScgRunsRoot(), or sits directly under it with no run
	 * folder to name.
	 *
	 * Every rule about run-folder paths (separators, per-platform case, the root's
	 * trailing separator, sibling directories whose names start with the root's)
	 * lives HERE rather than in the callers, for the same reason ScgRunsRoot() is
	 * one line: a rule in one place can be fixed in one place.
	 *
	 * The result is ENFORCED to round-trip: the answer is fed back through
	 * TryResolveScgRunPath and must land on the path that was passed in, so a
	 * split this function would otherwise accept but the resolver would reject
	 * (a ':' in a filename, legal on Linux and macOS) returns false instead of
	 * producing a reference nothing can read. That keeps the pair in step
	 * without restating any of the resolver's rules here.
	 */
	bool TrySplitScgRunPath(const FString& FullPath, FString& OutRunId, FString& OutRelPath);
}
