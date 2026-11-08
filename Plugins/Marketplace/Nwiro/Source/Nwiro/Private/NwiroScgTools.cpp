// Copyright 2026 Nwiro. All Rights Reserved.
//
// Implementations for the SCG local-file tools. Contracts and the sandbox rule
// are in NwiroScgTools.h.

#include "NwiroScgTools.h"
#include "NwiroPathSandbox.h"

#include "Json.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"      // LaunchFileInDefaultExternalApplication (OpenArtifact), ExploreFolder (RevealArtifact)
#include "HAL/PlatformFileManager.h"  // bounded handle read (PickFile), one-step rename (NwiroScgAtomicWrite)
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"  // MoveFileExW (NwiroScgAtomicWrite)
#endif
// PickFile's OS dialog (the engine ships Windows, Mac and Linux
// implementations) plus the parent window that makes it modal to the editor.
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroScg, Log, All);

namespace
{
	// Minimal JSON string escaper — sufficient for the small, controlled set of
	// fields written here (paths, error messages). Not a general-purpose JSON
	// encoder. The Scg prefix keeps it distinct from same-purpose helpers in
	// sibling files, which share a translation unit under unity builds.
	static FString NwiroScgJsonEscape(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 8);
		for (TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			default:
				// RFC 8259 §7: every remaining U+0000-U+001F control code must
				// be escaped or the result is not valid JSON.
				if (C < TEXT('\x20'))
				{
					Out += FString::Printf(TEXT("\\u%04x"), (uint32)C);
				}
				else
				{
					Out.AppendChar(C);
				}
				break;
			}
		}
		return Out;
	}

	// ------------------------------------------------------------------
	// Run-folder writing
	// ------------------------------------------------------------------

	/** Remove temp files left behind by an interrupted write. The `.tmp-` infix
	 *  keeps them out of every reader, so a leftover is inert. Swept on the
	 *  next write into the same directory. */
	static void NwiroScgSweepStaleTemps(const FString& DirPath)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(DirPath / TEXT("*.tmp-*")), /*Files=*/true, /*Directories=*/false);
		if (Found.Num() == 0)
		{
			return;
		}
		const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromHours(1);
		for (const FString& Name : Found)
		{
			const FString Full = DirPath / Name;
			// GetTimeStamp is UTC. Age-gated so a write still in flight — another
			// run writing right now — is never deleted out from under it.
			if (IFileManager::Get().GetTimeStamp(*Full) < Cutoff)
			{
				IFileManager::Get().Delete(*Full, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
			}
		}
	}

	/** Write via a temp file in the SAME directory, then replace atomically, so
	 *  a reader never observes a truncated file. The temp must share the
	 *  directory: across volumes the replace becomes a copy and is no longer
	 *  atomic. */
	static bool NwiroScgAtomicWrite(const FString& FullPath, const TArray<uint8>& Bytes, FString& OutError)
	{
		const FString Dir = FPaths::GetPath(FullPath);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		NwiroScgSweepStaleTemps(Dir);

		const FString TempPath = FString::Printf(TEXT("%s.tmp-%s"),
			*FullPath, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));

		// SaveArrayToFile opens, writes and closes before returning, so the
		// handle is released by the time the rename runs.
		if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
		{
			IFileManager::Get().Delete(*TempPath, false, true, true);
			OutError = TEXT("temp write failed");
			return false;
		}
		// One replacing rename, as the bridge's file save promotes its download: when it fails, an
		// existing artifact stays exactly as it was. Never Unreal's Move(Replace), which deletes the
		// destination first and loses it when the rename then fails.
#if PLATFORM_WINDOWS
		FString From = TempPath;
		FString To = FullPath;
		FPaths::MakePlatformFilename(From);
		FPaths::MakePlatformFilename(To);
		const bool bReplaced = ::MoveFileExW(*From, *To, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		// POSIX rename replaces an existing file in one step as well.
		const bool bReplaced = FPlatformFileManager::Get().GetPlatformFile().MoveFile(*FullPath, *TempPath);
#endif
		if (!bReplaced)
		{
			IFileManager::Get().Delete(*TempPath, false, true, true);
			OutError = TEXT("atomic replace failed");
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------
	// PickFile support
	// ------------------------------------------------------------------

	// Upper bound on what this build can carry across the browser binding. The
	// bytes are base64-encoded (+33%) into an FString and copied again by the
	// JSON writer and the binding itself, so peak memory is several times the
	// file size. A max_bytes above this is rejected rather than clamped.
	static constexpr int64 NwiroPickFileHardCeiling = 32 * 1024 * 1024;

	// Bounds the filter string handed to the OS dialog.
	static constexpr int32 NwiroPickFileMaxFilterChars = 1024;

	static FString NwiroPickFileJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, W);
		return Out;
	}

	/** A failure: an error code plus the relevant values, never a display
	 *  string — the caller composes the message. */
	static TSharedRef<FJsonObject> NwiroPickFileFailure(const TCHAR* ErrorCode)
	{
		TSharedRef<FJsonObject> Err = MakeShareable(new FJsonObject());
		Err->SetBoolField(TEXT("success"), false);
		Err->SetStringField(TEXT("error_code"), ErrorCode);
		return Err;
	}

	static FString NwiroPickFileBadArgs(const TCHAR* Field, const TCHAR* Reason)
	{
		TSharedRef<FJsonObject> Err = NwiroPickFileFailure(TEXT("bad_args"));
		Err->SetStringField(TEXT("field"), Field);
		Err->SetStringField(TEXT("reason"), Reason);
		return NwiroPickFileJson(Err);
	}
}

// ---------------------------------------------------------------------------
// Artifact I/O
// ---------------------------------------------------------------------------

FString FNwiroScgTools::WriteArtifact(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Args;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
	if (!(FJsonSerializer::Deserialize(R, Args) && Args.IsValid()))
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	FString RelPath, Base64;
	Args->TryGetStringField(TEXT("path"), RelPath);
	// Presence, not emptiness: "" is valid base64 for a zero-byte file.
	const bool bHasContent = Args->TryGetStringField(TEXT("content_base64"), Base64);
	if (RelPath.IsEmpty() || !bHasContent)
		return TEXT("{\"success\":false,\"error\":\"path and content_base64 are required\"}");
	const FString FullPath = NwiroPathSandbox::TryResolveScgRunPath(RelPath);
	if (FullPath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"path escapes the SCG run root\"}");
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64, Bytes))
		return TEXT("{\"success\":false,\"error\":\"content_base64 is not valid base64\"}");
	FString WriteError;
	if (!NwiroScgAtomicWrite(FullPath, Bytes, WriteError))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s: %s\"}"),
			*NwiroScgJsonEscape(WriteError), *NwiroScgJsonEscape(FullPath.Replace(TEXT("\\"), TEXT("/"))));
	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"bytes\":%d}"),
		*NwiroScgJsonEscape(FullPath.Replace(TEXT("\\"), TEXT("/"))), Bytes.Num());
}

FString FNwiroScgTools::ReadArtifact(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Args;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
	if (!(FJsonSerializer::Deserialize(R, Args) && Args.IsValid()))
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	FString RelPath;
	Args->TryGetStringField(TEXT("path"), RelPath);
	if (RelPath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"path is required\"}");
	const FString FullPath = NwiroPathSandbox::TryResolveScgRunPath(RelPath);
	if (FullPath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"path escapes the SCG run root\"}");
	TArray<uint8> Bytes;
	// Echoed paths are escaped: a legal filename may contain a quote.
	const FString EscapedPath = NwiroScgJsonEscape(FullPath.Replace(TEXT("\\"), TEXT("/")));
	if (!FFileHelper::LoadFileToArray(Bytes, *FullPath))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"file not found: %s\"}"), *EscapedPath);
	const FString Base64 = FBase64::Encode(Bytes);
	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"bytes\":%d,\"content_base64\":\"%s\"}"),
		*EscapedPath, Bytes.Num(), *Base64);
}

FString FNwiroScgTools::ListRuns(const FString& /*ArgsJson*/)
{
	const FString Root = NwiroPathSandbox::ScgRunsRoot();
	TArray<FString> Dirs;
	// Directories only. A missing root is not an error — it just means no run
	// has been written yet (the first write creates it).
	IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), /*Files=*/false, /*Directories=*/true);
	FString Json = TEXT("{\"success\":true,\"runs\":[");
	for (int32 i = 0; i < Dirs.Num(); ++i)
	{
		Json += FString::Printf(TEXT("%s\"%s\""), i == 0 ? TEXT("") : TEXT(","), *NwiroScgJsonEscape(Dirs[i]));
	}
	Json += TEXT("]}");
	return Json;
}

FString FNwiroScgTools::ReadArtifacts(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Args;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
	if (!(FJsonSerializer::Deserialize(R, Args) && Args.IsValid()))
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
	if (!Args->TryGetArrayField(TEXT("paths"), PathValues) || PathValues == nullptr)
		return TEXT("{\"success\":false,\"error\":\"paths (array of run-relative paths) is required\"}");

	// Ceiling on a single batch, so one request cannot ask for gigabytes.
	const int64 MaxTotalBytes = 64ll * 1024 * 1024;
	int64 Total = 0;

	FString Json = TEXT("{\"success\":true,\"results\":[");
	for (int32 i = 0; i < PathValues->Num(); ++i)
	{
		FString RelPath;
		(*PathValues)[i]->TryGetString(RelPath);
		const FString Escaped = NwiroScgJsonEscape(RelPath);
		const FString Sep = i == 0 ? TEXT("") : TEXT(",");

		const FString FullPath = RelPath.IsEmpty() ? FString() : NwiroPathSandbox::TryResolveScgRunPath(RelPath);
		if (FullPath.IsEmpty())
		{
			// Per-entry error: one bad path does not fail the rest of the batch.
			Json += FString::Printf(TEXT("%s{\"path\":\"%s\",\"error\":\"path escapes the SCG run root\"}"), *Sep, *Escaped);
			continue;
		}
		// Size before load, so the cap bounds the allocation instead of
		// reporting it after the fact.
		const int64 Size = IFileManager::Get().FileSize(*FullPath);
		if (Size < 0)
		{
			Json += FString::Printf(TEXT("%s{\"path\":\"%s\",\"error\":\"file not found\"}"), *Sep, *Escaped);
			continue;
		}
		if (Total + Size > MaxTotalBytes)
		{
			Json += FString::Printf(TEXT("%s{\"path\":\"%s\",\"error\":\"batch exceeds 64 MB — request fewer paths\"}"), *Sep, *Escaped);
			continue;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FullPath))
		{
			Json += FString::Printf(TEXT("%s{\"path\":\"%s\",\"error\":\"read failed\"}"), *Sep, *Escaped);
			continue;
		}
		Total += Bytes.Num();
		Json += FString::Printf(TEXT("%s{\"path\":\"%s\",\"bytes\":%d,\"content_base64\":\"%s\"}"),
			*Sep, *Escaped, Bytes.Num(), *FBase64::Encode(Bytes));
	}
	Json += TEXT("]}");
	return Json;
}

FString FNwiroScgTools::ArtifactsExist(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Args;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
	if (!(FJsonSerializer::Deserialize(R, Args) && Args.IsValid()))
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
	if (!Args->TryGetArrayField(TEXT("paths"), PathValues) || PathValues == nullptr)
		return TEXT("{\"success\":false,\"error\":\"paths (array of run-relative paths) is required\"}");

	FString Json = TEXT("{\"success\":true,\"exists\":[");
	for (int32 i = 0; i < PathValues->Num(); ++i)
	{
		FString RelPath;
		(*PathValues)[i]->TryGetString(RelPath);
		const FString FullPath = RelPath.IsEmpty() ? FString() : NwiroPathSandbox::TryResolveScgRunPath(RelPath);
		// A path outside the run root reports as absent, revealing nothing
		// about the rest of the disk.
		const bool bExists = !FullPath.IsEmpty() && IFileManager::Get().FileExists(*FullPath);
		Json += FString::Printf(TEXT("%s%s"), i == 0 ? TEXT("") : TEXT(","), bExists ? TEXT("true") : TEXT("false"));
	}
	Json += TEXT("]}");
	return Json;
}

// ---------------------------------------------------------------------------
// Handing files to the OS
// ---------------------------------------------------------------------------

// The embedded browser has no H.264 decoder, so video playback is handed to the
// operating system. Two conditions gate the launch: the path must resolve
// inside the run root, and its extension must appear in the allowlist below. A
// resolvable path alone is not sufficient.
FString FNwiroScgTools::OpenArtifact(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString RunId, RelPath;
	if (Cmd.IsValid() && Cmd->HasField(TEXT("run_id"))) RunId = Cmd->GetStringField(TEXT("run_id"));
	if (Cmd.IsValid() && Cmd->HasField(TEXT("rel_path"))) RelPath = Cmd->GetStringField(TEXT("rel_path"));
	if (RunId.IsEmpty() || RelPath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'run_id' or 'rel_path'\"}");

	// This tool LAUNCHES the file with the OS handler, so the list is a
	// deliberate decision rather than a formality. `.mov` is here because the
	// macOS render path emits QuickTime — the same class of well-known
	// container as .mp4, opened by the same players. The safety that matters is
	// the sandbox resolution below; this list is the second belt.
	static const TCHAR* AllowedExtensions[] = { TEXT(".mp4"), TEXT(".mov") };
	bool bAllowed = false;
	for (const TCHAR* Ext : AllowedExtensions)
	{
		if (RelPath.EndsWith(Ext, ESearchCase::IgnoreCase)) { bAllowed = true; break; }
	}
	if (!bAllowed)
		return TEXT("{\"success\":false,\"error\":\"Only video artifacts (.mp4, .mov) can be opened externally\"}");

	// run_id joins the path before resolution, so it faces the same checks.
	const FString Resolved = NwiroPathSandbox::TryResolveScgRunPath(RunId + TEXT("/") + RelPath);
	if (Resolved.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Path escapes the SCG run folder\"}");

	if (!FPaths::FileExists(Resolved))
	{
		// Caller-supplied values go through the JSON writer, so a quote in
		// run_id or rel_path cannot produce malformed output.
		TSharedRef<FJsonObject> Err = MakeShareable(new FJsonObject());
		Err->SetBoolField(TEXT("success"), false);
		Err->SetStringField(TEXT("error"), FString::Printf(TEXT("Artifact not found on disk: %s/%s"), *RunId, *RelPath));
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Err, W);
		return Out;
	}

	FPlatformProcess::LaunchFileInDefaultExternalApplication(*Resolved);
	UE_LOG(LogNwiroScg, Log, TEXT("SCG open_artifact: launched %s"), *Resolved);
	return TEXT("{\"success\":true}");
}

// The embedded browser suppresses HTML file inputs, so a native dialog is the
// only way to choose a file from the canvas.
//
// max_bytes and filter are required and have no defaults here: the caller owns
// the size limit and the extension list. Failures return an error code and the
// relevant values rather than a display string, so messages stay with the
// caller.
FString FNwiroScgTools::PickFile(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
	if (!(FJsonSerializer::Deserialize(Reader, Cmd) && Cmd.IsValid()))
		return NwiroPickFileBadArgs(TEXT("(root)"), TEXT("invalid"));

	// ---- argument hygiene, BEFORE anything is opened or read ----------------

	if (!Cmd->HasField(TEXT("max_bytes")))
		return NwiroPickFileBadArgs(TEXT("max_bytes"), TEXT("missing"));
	double MaxBytesD = 0.0;
	if (!Cmd->TryGetNumberField(TEXT("max_bytes"), MaxBytesD))
		return NwiroPickFileBadArgs(TEXT("max_bytes"), TEXT("invalid"));
	// Finiteness is checked explicitly: +Inf passes a plain range test and would
	// then be echoed back as a non-finite JSON number. Whole numbers only, so a
	// fractional count is invalid rather than silently truncated.
	if (!FMath::IsFinite(MaxBytesD) || !(MaxBytesD >= 1.0) || MaxBytesD != FMath::TruncToDouble(MaxBytesD))
		return NwiroPickFileBadArgs(TEXT("max_bytes"), TEXT("invalid"));
	if (MaxBytesD > static_cast<double>(NwiroPickFileHardCeiling))
	{
		// Both numbers are returned so the caller can report them.
		TSharedRef<FJsonObject> Err = NwiroPickFileFailure(TEXT("bad_args"));
		Err->SetStringField(TEXT("field"), TEXT("max_bytes"));
		Err->SetStringField(TEXT("reason"), TEXT("too_large"));
		Err->SetNumberField(TEXT("requested_bytes"), MaxBytesD);
		Err->SetNumberField(TEXT("supported_bytes"), static_cast<double>(NwiroPickFileHardCeiling));
		return NwiroPickFileJson(Err);
	}
	const int64 EffectiveLimit = static_cast<int64>(MaxBytesD);

	if (!Cmd->HasField(TEXT("filter")))
		return NwiroPickFileBadArgs(TEXT("filter"), TEXT("missing"));
	FString Filter;
	if (!Cmd->TryGetStringField(TEXT("filter"), Filter))
		return NwiroPickFileBadArgs(TEXT("filter"), TEXT("invalid"));
	if (Filter.IsEmpty())
		return NwiroPickFileBadArgs(TEXT("filter"), TEXT("missing"));
	if (Filter.Len() > NwiroPickFileMaxFilterChars)
		return NwiroPickFileBadArgs(TEXT("filter"), TEXT("too_long"));
	// Control characters, including the C1 block, are refused before the string
	// reaches the OS dialog.
	for (const TCHAR Ch : Filter)
	{
		if (FChar::IsControl(Ch))
			return NwiroPickFileBadArgs(TEXT("filter"), TEXT("invalid"));
	}

	// ---- the dialog ---------------------------------------------------------

	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
		return NwiroPickFileJson(NwiroPickFileFailure(TEXT("dialog_unavailable")));

	const void* ParentWindow = nullptr;
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWindow> Top = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (Top.IsValid() && Top->GetNativeWindow().IsValid())
			ParentWindow = Top->GetNativeWindow()->GetOSWindowHandle();
	}

	TArray<FString> OutFiles;
	const bool bPicked = Desktop->OpenFileDialog(
		ParentWindow,
		FString(), // title: empty on purpose — the OS uses its own localized default
		FString(), // default path: empty so the dialog opens where the user last was
		FString(),
		Filter,
		EFileDialogFlags::None, // single-select: one node, one source
		OutFiles);

	// Cancelling is a normal outcome, reported as success with cancelled=true.
	if (!bPicked || OutFiles.Num() == 0)
	{
		TSharedRef<FJsonObject> Res = MakeShareable(new FJsonObject());
		Res->SetBoolField(TEXT("success"), true);
		Res->SetBoolField(TEXT("cancelled"), true);
		return NwiroPickFileJson(Res);
	}

	const FString PickedPath = OutFiles[0];

	// ---- bounded read -------------------------------------------------------
	// The size comes from the open handle and exactly that many bytes are read.
	// Checking the size separately from the read would leave a window in which
	// the file grows in between, and the oversized allocation would happen
	// before the limit could reject it.
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.DirectoryExists(*PickedPath))
		return NwiroPickFileJson(NwiroPickFileFailure(TEXT("not_a_file")));

	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*PickedPath));
	if (!Handle.IsValid())
		return NwiroPickFileJson(NwiroPickFileFailure(TEXT("read_failed")));

	const int64 Size = Handle->Size();
	if (Size < 0)
		return NwiroPickFileJson(NwiroPickFileFailure(TEXT("read_failed")));
	if (Size > EffectiveLimit)
	{
		TSharedRef<FJsonObject> Err = NwiroPickFileFailure(TEXT("too_large"));
		Err->SetNumberField(TEXT("size_bytes"), static_cast<double>(Size));
		Err->SetNumberField(TEXT("limit_bytes"), static_cast<double>(EffectiveLimit));
		return NwiroPickFileJson(Err);
	}

	// ---- already a run artifact? ---------------------------------------------
	// Reported as a reference so the caller can point at the file in place
	// instead of importing a second, re-encoded copy of something this project
	// produced. Deliberately placed AFTER the size check: the transfer is what
	// the fast path skips, not the ceiling — an oversized file is refused
	// whichever side of the root it sits on, because whoever displays it later
	// reads it back through the bridge just the same.
	FString RunId, RelPath;
	if (NwiroPathSandbox::TrySplitScgRunPath(PickedPath, RunId, RelPath))
	{
		TSharedRef<FJsonObject> Ref = MakeShareable(new FJsonObject());
		Ref->SetBoolField(TEXT("success"), true);
		Ref->SetBoolField(TEXT("in_run"), true);
		Ref->SetStringField(TEXT("run_id"), RunId);
		Ref->SetStringField(TEXT("rel_path"), RelPath);
		// Carried even though no bytes move: the caller re-checks the size
		// itself rather than trusting the limit it sent us.
		Ref->SetNumberField(TEXT("size_bytes"), static_cast<double>(Size));
		UE_LOG(LogNwiroScg, Log, TEXT("SCG pick_file: in-run reference %s/%s (%lld bytes)"),
			*RunId, *RelPath, Size);
		return NwiroPickFileJson(Ref);
	}

	// Size <= EffectiveLimit <= HardCeiling, so the cast cannot overflow.
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	if (Size > 0 && !Handle->Read(Bytes.GetData(), Size))
		return NwiroPickFileJson(NwiroPickFileFailure(TEXT("read_failed")));

	// A zero-byte file is returned as an empty content_base64 rather than
	// refused; whether that is acceptable is the caller's judgement.

	TSharedRef<FJsonObject> Res = MakeShareable(new FJsonObject());
	Res->SetBoolField(TEXT("success"), true);
	// Stated rather than left to be inferred from the absence of run_id, so a
	// caller can branch on one field instead of on which fields are missing.
	Res->SetBoolField(TEXT("in_run"), false);
	Res->SetStringField(TEXT("original_name"), FPaths::GetCleanFilename(PickedPath));
	Res->SetNumberField(TEXT("size_bytes"), static_cast<double>(Size));
	Res->SetStringField(TEXT("content_base64"), FBase64::Encode(Bytes));
	UE_LOG(LogNwiroScg, Log, TEXT("SCG pick_file: %s (%lld bytes, limit %lld)"),
		*FPaths::GetCleanFilename(PickedPath), Size, EffectiveLimit);
	return NwiroPickFileJson(Res);
}

// ---------------------------------------------------------------------------
// Telling the user where a file is
// ---------------------------------------------------------------------------

// The panel is a hosted web page and cannot name a filesystem location, so the
// run root has to come from here for anything that wants to SHOW a path.
//
// No arguments, so there is no sandbox question: nothing caller-supplied is
// resolved, and a directory is named rather than its contents.
FString FNwiroScgTools::RunsRoot(const FString& /*ArgsJson*/)
{
	// Forward-slash working copies. Everything below compares and slices these;
	// only the strings that leave the function get converted to native form.
	FString RunsRootPath = NwiroPathSandbox::ScgRunsRoot();
	FPaths::NormalizeDirectoryName(RunsRootPath);
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);

	// The run root with everything above the project folder removed — for the
	// places a location is shown on every node or handed to a model, where the
	// absolute prefix is both noise and the user's account name.
	//
	// EMPTY when no project folder can be named, and the caller falls back: the
	// panel to the absolute path, the chat tool to saying nothing at all. A
	// wrong location is worse than none, since both are quoted with the same
	// confidence.
	FString Qualified;
	{
		// Empty for a drive root ("D:/") and for the POSIX root ("/") alike:
		// GetCleanFilename walks back past trailing separators before slicing,
		// so both volume roots are covered by this one test.
		const FString ProjectLeaf = FPaths::GetCleanFilename(ProjectDir);

		// A UNC share root ("//server/share") DOES have a last segment, and it
		// is the share name — infrastructure, not a project.
		bool bShareRoot = false;
		if (ProjectDir.StartsWith(TEXT("//"), ESearchCase::CaseSensitive))
		{
			TArray<FString> Segments;
			ProjectDir.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/true);
			bShareRoot = Segments.Num() <= 2;
		}

		// The chop below is by LENGTH, so the boundary is proven first. Each
		// condition earns its place: the length test keeps the index in range
		// when the two paths are equal, and the separator test stops a sibling
		// directory (".../Game" against ".../Game2/Saved/...") from passing
		// StartsWith and being sliced mid-segment.
		//
		// CaseSensitive is explicit because StartsWith defaults to IgnoreCase,
		// which on Linux and macOS would accept "/home/x/Proj" as a prefix of
		// "/home/x/proj/Saved/..." — two different directories there. Both
		// strings are engine-derived from the current project and normally share
		// casing; this fails closed if that ever stops being true.
		const bool bBoundaryOk =
			RunsRootPath.Len() > ProjectDir.Len()
			&& RunsRootPath.StartsWith(ProjectDir, ESearchCase::CaseSensitive)
			&& RunsRootPath[ProjectDir.Len()] == TEXT('/');

		if (!ProjectLeaf.IsEmpty() && !bShareRoot && bBoundaryOk)
		{
			Qualified = ProjectLeaf + RunsRootPath.RightChop(ProjectDir.Len());
			FPaths::MakePlatformFilename(Qualified);
		}
	}

	// Native form (backslashes on Windows) because these strings exist to be read
	// by a person and pasted into a file manager; FPaths hands back forward
	// slashes otherwise.
	FString Root = RunsRootPath;
	FPaths::MakePlatformFilename(Root);

	// Serialised through the writer, never Printf'd: MakePlatformFilename
	// deliberately produces "D:\Unreal Projects\...", and a bare backslash is
	// illegal inside a JSON string — hand-formatting would emit invalid JSON on
	// every Windows run.
	TSharedRef<FJsonObject> Res = MakeShareable(new FJsonObject());
	Res->SetBoolField(TEXT("success"), true);
	Res->SetStringField(TEXT("root"), Root);
	// Omitted rather than empty: absence is the contract for "no project folder
	// to name", and an empty string would have to be special-cased on the far
	// side anyway.
	if (!Qualified.IsEmpty())
	{
		Res->SetStringField(TEXT("project_qualified_root"), Qualified);
	}
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Res, W);
	return Out;
}

namespace
{
	// TryGetStringField does NOT reject a wrong-typed field: FJsonValueNumber and
	// FJsonValueBoolean implement TryGetString by CONVERTING, so {"run_id":42}
	// arrives as the string "42" and passes straight through. Measured live — the
	// hostile-input matrix returned not_found (the run "42" does not exist) where
	// bad_args was expected, which is the wrong answer to the wrong question.
	//
	// Only the value's declared type can decide this, so the field is fetched and
	// its EJson type checked. Null and object values were already refused, which
	// is why they looked fine and the number did not.
	bool NwiroScgRequireStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Name, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Name);
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return false;
		}
		Out = Value->AsString();
		return !Out.IsEmpty();
	}

	FString NwiroScgRevealFailure(const TCHAR* Code)
	{
		TSharedRef<FJsonObject> Err = MakeShareable(new FJsonObject());
		Err->SetBoolField(TEXT("success"), false);
		Err->SetStringField(TEXT("error_code"), Code);
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Err, W);
		return Out;
	}
}

// Show an artifact in the OS file browser.
//
// Two deliberate differences from OpenArtifact, which this is otherwise modelled
// on:
//
//  - No extension allowlist. OpenArtifact needs one because it hands the file to
//    the OS handler, which EXECUTES it; revealing runs nothing, so the sandbox is
//    doing all of the work and a list would only exclude the images and meshes a
//    user equally wants to find.
//  - Error codes, not sentences. A user-facing string compiled in here needs a
//    plugin rebuild to reword and never reaches the panel's translations.
//
// success means the request was DISPATCHED. FPlatformProcess::ExploreFolder is
// void on Windows, Mac and Linux alike, so whether a window actually appeared is
// not observable from here — the two failures that ARE knowable (outside the
// sandbox, file missing) are both checked before dispatch.
FString FNwiroScgTools::RevealArtifact(const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
	// A body that is malformed, an array or a bare number must read as bad_args
	// rather than reaching the sandbox as an empty path — hand-written bridge
	// calls are in scope here, so the arguments cannot be assumed clean. The
	// fields themselves are type-checked by NwiroScgRequireStringField below,
	// which is stricter than TryGetStringField for the reason recorded there.
	if (!(FJsonSerializer::Deserialize(Reader, Cmd) && Cmd.IsValid()))
		return NwiroScgRevealFailure(TEXT("bad_args"));

	FString RunId, RelPath;
	if (!NwiroScgRequireStringField(Cmd, TEXT("run_id"), RunId))
		return NwiroScgRevealFailure(TEXT("bad_args"));
	if (!NwiroScgRequireStringField(Cmd, TEXT("rel_path"), RelPath))
		return NwiroScgRevealFailure(TEXT("bad_args"));

	// run_id is joined to the path BEFORE resolution, so a hostile run id faces
	// exactly the same checks as a hostile path.
	const FString Resolved = NwiroPathSandbox::TryResolveScgRunPath(RunId + TEXT("/") + RelPath);
	if (Resolved.IsEmpty())
		return NwiroScgRevealFailure(TEXT("outside_sandbox"));

	// Checked so that a deleted artifact reports itself, rather than opening a
	// folder that does not contain the thing that was clicked on.
	if (!FPaths::FileExists(Resolved))
		return NwiroScgRevealFailure(TEXT("not_found"));

	// The CONTAINING FOLDER, never the file. Handed a file, the engine's Windows
	// implementation builds an unquoted "explorer.exe /select,<path>" command
	// line (identical in 5.5 through 5.8), so any project path containing a space
	// opens the wrong window — and this project's own path contains two. Handed a
	// directory it takes the other branch, where the path is a single API
	// parameter and spaces are harmless. Mac roots its viewer at the folder and
	// Linux only ever opened the folder anyway, so this also makes the three
	// platforms behave alike. The cost is that the file is not highlighted.
	const FString Folder = FPaths::GetPath(Resolved);
	FPlatformProcess::ExploreFolder(*Folder);
	UE_LOG(LogNwiroScg, Log, TEXT("SCG reveal_artifact: dispatched for %s"), *Folder);

	TSharedRef<FJsonObject> Res = MakeShareable(new FJsonObject());
	Res->SetBoolField(TEXT("success"), true);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Res, W);
	return Out;
}
