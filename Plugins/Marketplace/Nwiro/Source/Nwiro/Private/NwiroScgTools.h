// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Scene Generation local-file tools.
 *
 * Every path argument is relative to the Scene Generation run root,
 * Saved/Nwiro/SceneGeneration/runs, and resolves through
 * NwiroPathSandbox::TryResolveScgRunPath. Absolute paths, drive letters, ".."
 * segments and anything else landing outside that root are refused. No tool
 * here can read or write elsewhere on disk.
 *
 * These tools are dispatchable but are not advertised to the chat model, so it
 * is never given a schema for them.
 *
 * Each method takes the tool's raw JSON arguments and returns a JSON string.
 * Failure is always {"success":false, ...}, never an exception.
 */
class FNwiroScgTools
{
public:
	// {path, content_base64} both required. content_base64 may be an empty
	// string, which writes a zero-byte file. Missing parent directories are
	// created. The write is atomic: a reader sees either the previous contents
	// or the new ones. -> {success, path, bytes}
	static FString WriteArtifact(const FString& ArgsJson);

	// -> {success, path, bytes, content_base64}, or a "file not found: ..."
	// error when the path resolves but nothing is there.
	static FString ReadArtifact(const FString& ArgsJson);

	// Run folder names, directories only, unordered. A run root that does not
	// exist yet returns an empty list rather than an error.
	// -> {success, runs}
	static FString ListRuns(const FString& ArgsJson);

	// Batch read, results in input order. A path that cannot be read produces
	// an error entry rather than failing the batch. Total payload capped at
	// 64 MB. -> {success, results}
	static FString ReadArtifacts(const FString& ArgsJson);

	// Batch existence check, one bool per input path, in order.
	// -> {success, exists}
	static FString ArtifactsExist(const FString& ArgsJson);

	// Hand a run-folder video to the operating system's default player.
	// {run_id, rel_path} resolves through the run sandbox, and only allowlisted
	// media extensions launch. -> {success} or {success:false, error}
	static FString OpenArtifact(const FString& ArgsJson);

	// Show a run-folder artifact in the operating system's file browser.
	// {run_id, rel_path} resolves through the run sandbox exactly as
	// OpenArtifact does, but nothing is executed, so no extension allowlist
	// applies. The CONTAINING FOLDER is what gets opened, not the file: the
	// engine's file-selecting path builds an unquoted command line on Windows
	// and misbehaves on any project path containing a space.
	// success means the request was DISPATCHED — FPlatformProcess::ExploreFolder
	// returns void on every platform, so "it actually opened" is unobservable.
	// -> {success} | {success:false, error_code}
	static FString RevealArtifact(const FString& ArgsJson);

	// The Scene Generation run root, in native form (backslashes on Windows)
	// because its only purpose is to be shown to a human and pasted into a file
	// manager. Takes no arguments and names a directory, not its contents.
	//
	// `project_qualified_root` is the same path with everything above the
	// project folder removed — for the places a location is shown to a model or
	// repeated on every node, where the absolute prefix is both noise and the
	// user's account name. It is OMITTED when no project folder can be named
	// (a drive root, the POSIX root, a UNC share root); callers treat absence as
	// "fall back", not as an error.
	// -> {success, root, project_qualified_root?} | {success:false, error_code}
	static FString RunsRoot(const FString& ArgsJson);

	// Open the operating system's file dialog and return the chosen file's
	// bytes as base64. {max_bytes, filter} are both required: the caller
	// supplies the size limit and the extension list. Failures carry an error
	// code and the relevant values, never a display string.
	//
	// A pick that already lives inside the run root is reported as a REFERENCE
	// instead of being transferred — the caller points at it in place rather
	// than importing a second copy. The size limit still applies to it: only
	// the byte transfer is skipped, never the ceiling.
	// -> {success, in_run:false, original_name, size_bytes, content_base64}
	//  | {success, in_run:true, run_id, rel_path, size_bytes}
	//  | {success, cancelled}
	//  | {success:false, error_code, ...}
	static FString PickFile(const FString& ArgsJson);
};
