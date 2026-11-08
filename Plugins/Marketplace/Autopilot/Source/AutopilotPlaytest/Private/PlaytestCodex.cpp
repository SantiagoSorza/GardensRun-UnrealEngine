// The game's code for a brain that can't read this machine's disk (docs/protocol.md: `codex_manifest`, `codex_read`).
// Read-only, and only the project's own code: Source/ (C++ and C#), Config/*.ini, the .uproject, and the Blueprint
// export the AutopilotCodex commandlet writes to Saved/AutopilotCodex/ (run as a child process, never in this one: it loads
// every asset). The walks, hashes and reads run off the game thread; the answers go from the game thread.

#include "PlaytestInternal.h"

#include "Algo/Sort.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PlaytestSubsystem.h"

namespace PlaytestCodex
{
namespace
{
	using FPending = UPlaytestSubsystem::FPending;

	// What one manifest serves in all (the brain fetches it over the plugin's socket and keeps a copy): past it the
	// manifest says truncated. Every file counts, however many there are
	constexpr int64 MaxTotalBytes = 200ll * 1024 * 1024;
	constexpr int64 DefaultReadBytes = 1000000;
	constexpr int64 MaxReadBytes = 4ll * 1024 * 1024;
	constexpr int32 SyncReadFiles = 3;  // reads of more files than this go off the game thread
	// The brain waits up to 15 minutes for a manifest: an export still going after this is answered without waiting.
	constexpr double ExportWaitSeconds = 14.0 * 60.0;
	const TCHAR* const ExportPrefix = TEXT("Saved/AutopilotCodex");

	/** Where the served files are: absolute, forward slashes, no trailing slash. Worked out on the game thread. */
	struct FRoots
	{
		FString Project;      // the project's directory
		FString Export;       // <ProjectSavedDir>/AutopilotCodex
		FString UProjectName; // e.g. "MyGame.uproject"
		FString Content;      // the project's Content/ (Blueprint edits: see ExportIsStale)
		FString Plugins;      // the project's Plugins/
	};

	FString Clean(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Path);  // forward slashes, no trailing slash
		return Path;
	}

	FRoots GetRoots()
	{
		FRoots Roots;
		Roots.Project = Clean(FPaths::ProjectDir());
		Roots.Export = Clean(FPaths::ProjectSavedDir() / TEXT("AutopilotCodex"));
		Roots.UProjectName = FPaths::GetCleanFilename(FPaths::GetProjectFilePath());
		Roots.Content = Clean(FPaths::ProjectContentDir());
		Roots.Plugins = Clean(FPaths::ProjectPluginsDir());
		return Roots;
	}

	bool IsSkippedDir(const FString& Name)
	{
		return Name.Equals(TEXT("Intermediate"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Binaries"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("ThirdParty"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("node_modules"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT(".git"), ESearchCase::IgnoreCase);
	}

	bool IsSourceFile(const FString& Name)
	{
		const FString Ext = FPaths::GetExtension(Name);
		return Ext.Equals(TEXT("h"), ESearchCase::IgnoreCase) || Ext.Equals(TEXT("hpp"), ESearchCase::IgnoreCase)
			|| Ext.Equals(TEXT("cpp"), ESearchCase::IgnoreCase) || Ext.Equals(TEXT("inl"), ESearchCase::IgnoreCase)
			|| Ext.Equals(TEXT("cs"), ESearchCase::IgnoreCase);
	}

	bool IsIni(const FString& Name)
	{
		return FPaths::GetExtension(Name).Equals(TEXT("ini"), ESearchCase::IgnoreCase);
	}

	/**
	 * The file on disk a path the brain names stands for, "" when it isn't one the manifest could list: relative to the
	 * project, forward or back slashes, no "." or ".." or empty parts, no drive or leading slash, no skipped directory.
	 */
	FString Resolve(const FRoots& Roots, const FString& Asked)
	{
		FString Path = Asked.Replace(TEXT("\\"), TEXT("/"));
		if (Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Path.Contains(TEXT(":")))
		{
			return FString();
		}
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"), false);
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || IsSkippedDir(Part))
			{
				return FString();
			}
		}
		const FString Name = Parts.Last();
		FString Root;
		int32 First = 0;
		if (Parts.Num() == 1)
		{
			if (!Name.Equals(Roots.UProjectName, ESearchCase::IgnoreCase))
			{
				return FString();
			}
			Root = Roots.Project;
		}
		else if (Parts[0].Equals(TEXT("Config"), ESearchCase::IgnoreCase))
		{
			if (Parts.Num() != 2 || !IsIni(Name))
			{
				return FString();
			}
			Root = Roots.Project;
		}
		else if (Parts[0].Equals(TEXT("Source"), ESearchCase::IgnoreCase))
		{
			if (!IsSourceFile(Name))
			{
				return FString();
			}
			Root = Roots.Project;
		}
		else if (Parts.Num() >= 3 && Parts[0].Equals(TEXT("Saved"), ESearchCase::IgnoreCase)
			&& Parts[1].Equals(TEXT("AutopilotCodex"), ESearchCase::IgnoreCase))
		{
			Root = Roots.Export;
			First = 2;
		}
		else
		{
			return FString();
		}
		FString Full = Root;
		for (int32 i = First; i < Parts.Num(); ++i)
		{
			Full /= Parts[i];
		}
		Full = Clean(Full);
		// (belt and braces: the parts above can't leave the root)
		if (!Full.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))
		{
			return FString();
		}
		return Full;
	}

	struct FListed
	{
		FString Path;  // as the manifest names it
		FString Full;  // on disk
		int64 Size = 0;
		FDateTime Modified;
	};

	/**
	 * The hashes worked out so far, by file and its size and time: a manifest asked for again and again while the export
	 * runs (codex_manifest with wait false) hashes only the files written since. Any thread, under HashesLock.
	 */
	struct FKnownHash
	{
		int64 Size = 0;
		FDateTime Modified;
		FString Hash;
	};
	TMap<FString, FKnownHash> KnownHashes;
	FCriticalSection HashesLock;

	/** Files under Dir (and its subdirectories when bRecurse, skipped ones left out), Keep deciding by file name. */
	void Walk(const FString& Dir, const FString& Prefix, bool bRecurse, TFunctionRef<bool(const FString&)> Keep, TArray<FListed>& Out)
	{
		TArray<TPair<FString, FString>> SubDirs;
		IFileManager::Get().IterateDirectoryStat(*Dir, [&](const TCHAR* Found, const FFileStatData& Stat)
		{
			const FString Name = FPaths::GetCleanFilename(Found);
			if (Stat.bIsDirectory)
			{
				if (bRecurse && !IsSkippedDir(Name))
				{
					SubDirs.Emplace(Dir / Name, Prefix / Name);
				}
			}
			else if (Keep(Name))
			{
				Out.Add({Prefix / Name, Dir / Name, Stat.FileSize, Stat.ModificationTime});
			}
			return true;
		});
		for (const TPair<FString, FString>& Sub : SubDirs)
		{
			Walk(Sub.Key, Sub.Value, bRecurse, Keep, Out);
		}
	}

	/** The manifest's files, hashed (off the game thread). */
	TSharedPtr<FJsonObject> BuildManifest(const FRoots& Roots)
	{
		TArray<FListed> Found;
		const FString UProject = Roots.Project / Roots.UProjectName;
		const FFileStatData UProjectStat = IFileManager::Get().GetStatData(*UProject);
		if (UProjectStat.bIsValid && !UProjectStat.bIsDirectory)
		{
			Found.Add({Roots.UProjectName, UProject, UProjectStat.FileSize, UProjectStat.ModificationTime});
		}
		auto Sorted = [&Found](int32 From)
		{
			Algo::Sort(MakeArrayView(Found.GetData() + From, Found.Num() - From),
				[](const FListed& A, const FListed& B) { return A.Path < B.Path; });
		};
		int32 From = Found.Num();
		Walk(Roots.Project / TEXT("Config"), TEXT("Config"), false, [](const FString& Name) { return IsIni(Name); }, Found);
		Sorted(From);
		From = Found.Num();
		Walk(Roots.Project / TEXT("Source"), TEXT("Source"), true, [](const FString& Name) { return IsSourceFile(Name); }, Found);
		Sorted(From);
		From = Found.Num();
		Walk(Roots.Export, ExportPrefix, true, [](const FString&) { return true; }, Found);
		Sorted(From);

		TArray<TSharedPtr<FJsonValue>> Files;
		bool bTruncated = false;
		int64 Total = 0;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(256 * 1024);  // HashFile reads in chunks of its size (an empty one would never end)
		for (const FListed& File : Found)
		{
			if (Total + File.Size > MaxTotalBytes)
			{
				bTruncated = true;
				continue;  // (a smaller one may still fit)
			}
			FString Hash;
			{
				FScopeLock Lock(&HashesLock);
				const FKnownHash* Known = KnownHashes.Find(File.Full);
				if (Known && Known->Size == File.Size && Known->Modified == File.Modified)
				{
					Hash = Known->Hash;
				}
			}
			if (Hash.IsEmpty())
			{
				const FMD5Hash Md5 = FMD5Hash::HashFile(*File.Full, &Buffer);
				if (!Md5.IsValid())
				{
					continue;  // gone, or can't be read
				}
				Hash = LexToString(Md5);
				FScopeLock Lock(&HashesLock);
				KnownHashes.Add(File.Full, {File.Size, File.Modified, Hash});
			}
			Total += File.Size;
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), File.Path);
			Entry->SetNumberField(TEXT("size"), (double)File.Size);
			Entry->SetStringField(TEXT("hash"), Hash);
			Files.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("files"), Files);
		Result->SetBoolField(TEXT("truncated"), bTruncated);
		return Result;
	}

	/** The files asked for, as text, within MaxBytes (any thread). */
	TSharedPtr<FJsonObject> ReadFiles(const FRoots& Roots, const TArray<FString>& Paths, int64 MaxBytes)
	{
		TSharedRef<FJsonObject> Texts = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Missing;
		TArray<TSharedPtr<FJsonValue>> TooBig;
		TSet<FString> Seen;
		int64 Total = 0;
		for (const FString& Asked : Paths)
		{
			bool bAlready = false;
			Seen.Add(Asked, &bAlready);
			if (bAlready)
			{
				continue;
			}
			const FString Full = Resolve(Roots, Asked);
			const int64 Size = Full.IsEmpty() ? -1 : IFileManager::Get().FileSize(*Full);  // (-1 for a directory too)
			if (Size < 0)
			{
				Missing.Add(MakeShared<FJsonValueString>(Asked));
				continue;
			}
			if (Size > MaxBytes)
			{
				TooBig.Add(MakeShared<FJsonValueString>(Asked));
				continue;
			}
			if (Total + Size > MaxBytes)
			{
				break;  // the rest go unanswered: the brain asks again
			}
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Full))
			{
				Missing.Add(MakeShared<FJsonValueString>(Asked));
				continue;
			}
			Total += Size;
			Texts->SetStringField(Asked, Text);
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("files"), Texts);
		Result->SetArrayField(TEXT("missing"), Missing);
		Result->SetArrayField(TEXT("too_big"), TooBig);
		return Result;
	}

	/** Whether a .uasset or .umap under Dir (skipped directories left out) was written after Since. */
	bool HasNewerAsset(const FString& Dir, const FDateTime& Since)
	{
		bool bNewer = false;
		TArray<FString> SubDirs;
		IFileManager::Get().IterateDirectoryStat(*Dir, [&](const TCHAR* Found, const FFileStatData& Stat)
		{
			const FString Name = FPaths::GetCleanFilename(Found);
			if (Stat.bIsDirectory)
			{
				if (!IsSkippedDir(Name))
				{
					SubDirs.Add(Dir / Name);
				}
			}
			else if (Stat.ModificationTime > Since)
			{
				const FString Ext = FPaths::GetExtension(Name);
				bNewer = Ext.Equals(TEXT("uasset"), ESearchCase::IgnoreCase) || Ext.Equals(TEXT("umap"), ESearchCase::IgnoreCase);
			}
			return !bNewer;
		});
		for (int32 i = 0; i < SubDirs.Num() && !bNewer; ++i)
		{
			bNewer = HasNewerAsset(SubDirs[i], Since);
		}
		return bNewer;
	}

	/** The Content/ of each plugin under Dir (a directory holding a .uplugin), our own (Autopilot.uplugin) left out. */
	void FindPluginContent(const FString& Dir, TArray<FString>& Out)
	{
		TArray<FString> SubDirs;
		bool bPlugin = false;
		bool bOurs = false;
		IFileManager::Get().IterateDirectoryStat(*Dir, [&](const TCHAR* Found, const FFileStatData& Stat)
		{
			const FString Name = FPaths::GetCleanFilename(Found);
			if (Stat.bIsDirectory)
			{
				if (!IsSkippedDir(Name))
				{
					SubDirs.Add(Dir / Name);
				}
			}
			else if (FPaths::GetExtension(Name).Equals(TEXT("uplugin"), ESearchCase::IgnoreCase))
			{
				bPlugin = true;
				bOurs |= Name.Equals(TEXT("Autopilot.uplugin"), ESearchCase::IgnoreCase);
			}
			return true;
		});
		if (bPlugin)
		{
			if (!bOurs)
			{
				Out.Add(Dir / TEXT("Content"));
			}
			return;  // (no plugin inside a plugin)
		}
		for (const FString& Sub : SubDirs)
		{
			FindPluginContent(Sub, Out);
		}
	}

	/**
	 * Whether the export is older than the game's assets: a .uasset or .umap under the project's Content/ or a project
	 * plugin's Content/ (not AutopilotPlaytest's) written after the export's summary.txt. File times only (off the game thread).
	 */
	bool ExportIsStale(const FRoots& Roots)
	{
		// (the stat the walk reads too: the same clock and precision)
		const FFileStatData Summary = IFileManager::Get().GetStatData(*(Roots.Export / TEXT("summary.txt")));
		if (!Summary.bIsValid)
		{
			return true;
		}
		const FDateTime Exported = Summary.ModificationTime;
		TArray<FString> Dirs = {Roots.Content};
		FindPluginContent(Roots.Plugins, Dirs);
		for (const FString& Dir : Dirs)
		{
			if (HasNewerAsset(Dir, Exported))
			{
				return true;
			}
		}
		return false;
	}

	// ------------------------------------------------------------------ the export (game thread only)

	struct FExportJob
	{
		FProcHandle Proc;
		double Start = 0;
		bool bFinished = false;
		double Seconds = 0;
		int32 ExitCode = 0;
		FString Error;
		FString Reason;  // why it runs: "missing" | "stale" | "refresh"
		bool bStopped = false;  // ended by Stop (StopExport)
		FString ProgressFile;  // the commandlet's -progress file: "<done> <total>"
		int32 Done = 0;
		int32 Total = 0;
		double ProgressReadAt = 0;
	};

	/** The export running now, shared by every manifest request waiting for it. */
	TSharedPtr<FExportJob> Running;

	FString LogRelative()
	{
		return TEXT("Saved/Logs/AutopilotCodexExport.log");
	}

	FString ProgressRelative()
	{
		return TEXT("Saved/Logs/AutopilotCodexExport.progress");
	}

	/** The engine's command-line editor next to the running executable (or in the engine's binaries). */
	FString FindEditorCmd()
	{
		const FString Exe = FPlatformProcess::ExecutablePath();
		const FString Dir = FPaths::GetPath(Exe);
		const FString Ext = FPaths::GetExtension(Exe, true);
		const FString Base = FPaths::GetBaseFilename(Exe);
		TArray<FString> Candidates;
		if (Base.StartsWith(TEXT("UnrealEditor")))
		{
			// UnrealEditor.exe -> UnrealEditor-Cmd.exe; UnrealEditor-Win64-DebugGame.exe -> ...-DebugGame-Cmd.exe
			Candidates.Add(Dir / (Base.EndsWith(TEXT("-Cmd")) ? Base : Base + TEXT("-Cmd")) + Ext);
		}
		const FString Plain = FString(TEXT("UnrealEditor-Cmd")) + Ext;
		Candidates.Add(Dir / Plain);
		const FString EngineBinaries = FPaths::ConvertRelativePathToFull(
			FPaths::EngineDir() / TEXT("Binaries") / FPlatformProcess::GetBinariesSubdirectory());
		Candidates.Add(EngineBinaries / Plain);
#if !PLATFORM_WINDOWS
		Candidates.Add(Dir / TEXT("UnrealEditor"));
		Candidates.Add(EngineBinaries / TEXT("UnrealEditor"));
#endif
		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	/** Start the export: the job, or nullptr with Error set. FirstMap: the level the run plays, exported (with what it
	 * uses) first; "" for none. */
	TSharedPtr<FExportJob> StartExport(const FRoots& Roots, const FString& Reason, const FString& FirstMap, FString& Error)
	{
		const FString Cmd = FindEditorCmd();
		if (Cmd.IsEmpty())
		{
			Error = FString::Printf(TEXT("UnrealEditor-Cmd was not found next to %s or in the engine's binaries: the Blueprint export needs the editor installed"),
				FPlatformProcess::ExecutablePath());
			return nullptr;
		}
		const FString UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		const FString Log = Roots.Project / LogRelative();
		const FString ProgressFile = Roots.Project / ProgressRelative();
		IFileManager::Get().Delete(*ProgressFile, false, true, true);  // (an earlier export's)
		FString Params = FString::Printf(
			TEXT("\"%s\" -run=AutopilotCodex -out=\"%s\" -unattended -nullrhi -nosplash -nop4 -abslog=\"%s\" -progress=\"%s\""),
			*UProject, *Roots.Export, *Log, *ProgressFile);
		if (!FirstMap.IsEmpty() && !FirstMap.Contains(TEXT("\"")))
		{
			Params += FString::Printf(TEXT(" -first=\"%s\""), *FirstMap);
		}
		uint32 ProcessId = 0;
		// Hidden, below normal priority: the game keeps its frames
		FProcHandle Proc = FPlatformProcess::CreateProc(*Cmd, *Params, false, true, true, &ProcessId, -1, nullptr, nullptr);
		if (!Proc.IsValid())
		{
			Error = FString::Printf(TEXT("Couldn't start %s"), *Cmd);
			return nullptr;
		}
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] project export: exporting Blueprints (process %u): %s %s"), ProcessId, *Cmd, *Params);
		TSharedPtr<FExportJob> Job = MakeShared<FExportJob>();
		Job->Proc = Proc;
		Job->Start = FPlatformTime::Seconds();
		Job->Reason = Reason;
		Job->ProgressFile = ProgressFile;
		return Job;
	}

	/** Whether the job has ended (noting how, the first time it is seen to). */
	bool Poll(const TSharedPtr<FExportJob>& Job, const FRoots& Roots)
	{
		if (Job->bFinished)
		{
			return true;
		}
		if (FPlatformProcess::IsProcRunning(Job->Proc))
		{
			return false;
		}
		int32 Code = -1;
		if (!FPlatformProcess::GetProcReturnCode(Job->Proc, &Code))
		{
			Code = -1;
		}
		FPlatformProcess::CloseProc(Job->Proc);
		Job->bFinished = true;
		Job->ExitCode = Code;
		Job->Seconds = FPlatformTime::Seconds() - Job->Start;
		IFileManager::Get().Delete(*Job->ProgressFile, false, true, true);
		if (Job->bStopped)
		{
			Job->Error = TEXT("The Blueprint export was stopped (the run was stopped)");
		}
		else if (Code != 0)
		{
			Job->Error = FString::Printf(TEXT("The Blueprint export exited with code %d (its log: %s)"), Code, *LogRelative());
		}
		else if (!FPaths::FileExists(Roots.Export / TEXT("summary.txt")))
		{
			Job->Error = FString::Printf(TEXT("The Blueprint export wrote no summary.txt (its log: %s)"), *LogRelative());
		}
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] project export: ended after %.1f s, exit code %d"), Job->Seconds, Code);
		if (Running == Job)
		{
			Running.Reset();
		}
		return true;
	}

	TSharedRef<FJsonObject> ExportInfo(bool bRan, const FString& Reason, double Seconds, const int32* ExitCode, const FString& Error)
	{
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetBoolField(TEXT("ran"), bRan);
		if (!Reason.IsEmpty())
		{
			Info->SetStringField(TEXT("reason"), Reason);
		}
		Info->SetNumberField(TEXT("seconds"), FMath::RoundToDouble(Seconds * 10) / 10);
		if (ExitCode)
		{
			Info->SetNumberField(TEXT("exit_code"), *ExitCode);
		}
		else
		{
			Info->SetField(TEXT("exit_code"), MakeShared<FJsonValueNull>());
		}
		if (!Error.IsEmpty())
		{
			Info->SetStringField(TEXT("error"), Error);
		}
		return Info;
	}

	/** Read the job's -progress file into Done and Total (at most 4 times a second; what was read last is kept when it
	 * can't be read). */
	void ReadProgress(FExportJob& Job)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - Job.ProgressReadAt < 0.25)
		{
			return;
		}
		Job.ProgressReadAt = Now;
		FString Text;
		TArray<FString> Parts;
		if (FFileHelper::LoadFileToString(Text, *Job.ProgressFile, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite)
			&& Text.ParseIntoArrayWS(Parts) == 2 && Parts[0].IsNumeric() && Parts[1].IsNumeric())
		{
			Job.Done = FCString::Atoi(*Parts[0]);
			Job.Total = FCString::Atoi(*Parts[1]);
		}
	}

	/** The export's part of an answer that doesn't wait for it to end: running, with how far it is. */
	TSharedRef<FJsonObject> RunningInfo(FExportJob& Job)
	{
		ReadProgress(Job);
		TSharedRef<FJsonObject> Info = ExportInfo(true, Job.Reason, FPlatformTime::Seconds() - Job.Start, nullptr, FString());
		Info->SetBoolField(TEXT("running"), true);
		Info->SetNumberField(TEXT("done"), Job.Done);
		Info->SetNumberField(TEXT("total"), Job.Total);
		return Info;
	}

	/** One manifest request: waiting for the export, then for the hashes. */
	struct FManifestRequest
	{
		FRoots Roots;
		TSharedPtr<FExportJob> Job;  // the export it waits for
		TSharedPtr<FJsonObject> Export;  // the export's part of the answer, once known
		TSharedPtr<TFuture<bool>> Checking;  // "auto" with an export there: whether it is stale
		TSharedPtr<TFuture<TSharedPtr<FJsonObject>>> Hashing;
	};
}

void Manifest(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
{
	FString Mode = TEXT("auto");
	// wait false: answered with the files there now while the export runs (its progress in the answer), so the brain
	// fetches them as they come and asks again; first_map: the level the run plays, which a new export does first
	bool bWait = true;
	FString FirstMap;
	if (Args.IsValid())
	{
		Args->TryGetStringField(TEXT("export"), Mode);
		Args->TryGetBoolField(TEXT("wait"), bWait);
		Args->TryGetStringField(TEXT("first_map"), FirstMap);
	}
	if (Mode != TEXT("auto") && Mode != TEXT("refresh") && Mode != TEXT("skip"))
	{
		Bridge.RespondError(Id, FString::Printf(TEXT("export must be \"auto\", \"refresh\" or \"skip\", not \"%s\""), *Mode));
		return;
	}
	TSharedPtr<FManifestRequest> Request = MakeShared<FManifestRequest>();
	Request->Roots = GetRoots();
	// Run the export, or wait for the one running (one at a time)
	auto Export = [Request, FirstMap](const FString& Reason)
	{
		if (Running.IsValid() && Running->bStopped)
		{
			// (a stopped export's process, still going away: the new one starts once it is gone)
			FPlatformProcess::WaitForProc(Running->Proc);
			Poll(Running, Request->Roots);
		}
		if (Running.IsValid())
		{
			Request->Job = Running;
			return;
		}
		FString Error;
		Running = StartExport(Request->Roots, Reason, FirstMap, Error);
		Request->Job = Running;
		if (!Running.IsValid())
		{
			UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] project export: %s"), *Error);
			Request->Export = ExportInfo(false, Reason, 0, nullptr, Error);
		}
	};
	if (Mode == TEXT("skip"))
	{
		Request->Export = ExportInfo(false, FString(), 0, nullptr, FString());
	}
	else if (Running.IsValid() || Mode == TEXT("refresh"))
	{
		Export(TEXT("refresh"));
	}
	else if (!FPaths::FileExists(Request->Roots.Export / TEXT("summary.txt")))
	{
		Export(TEXT("missing"));
	}
	else
	{
		// There already: run it again when the game's assets were saved since (file times, walked off the game thread)
		const FRoots Roots = Request->Roots;
		Request->Checking = MakeShared<TFuture<bool>>(Async(EAsyncExecution::ThreadPool, [Roots]() { return ExportIsStale(Roots); }));
	}

	FPending Pending;
	Pending.Id = Id;
	Pending.Op = TEXT("codex_manifest");
	Pending.StartReal = FPlatformTime::Seconds();
	Pending.Tick = [&Bridge, Request, Export, bWait](FPending& P, double Now)
	{
		if (Request->Checking.IsValid())
		{
			if (!Request->Checking->IsReady())
			{
				return false;
			}
			const bool bStale = Request->Checking->Get();
			Request->Checking.Reset();
			if (bStale)
			{
				Export(TEXT("stale"));
			}
			else
			{
				Request->Export = ExportInfo(false, FString(), 0, nullptr, FString());  // up to date
			}
		}
		if (!Request->Export.IsValid())
		{
			const TSharedPtr<FExportJob>& Job = Request->Job;
			if (Poll(Job, Request->Roots))
			{
				Request->Export = ExportInfo(true, Job->Reason, Job->Seconds, &Job->ExitCode, Job->Error);
			}
			else if (!bWait)
			{
				Request->Export = RunningInfo(*Job);
			}
			else if (FPlatformTime::Seconds() - P.StartReal > ExportWaitSeconds)
			{
				const double Seconds = FPlatformTime::Seconds() - Job->Start;
				Request->Export = ExportInfo(true, Job->Reason, Seconds, nullptr, FString::Printf(
					TEXT("The Blueprint export is still running after %.0f s: these are the files there now; ask again for its files once it ends"), Seconds));
			}
			else
			{
				return false;
			}
		}
		if (!Request->Hashing.IsValid())
		{
			const FRoots Roots = Request->Roots;
			Request->Hashing = MakeShared<TFuture<TSharedPtr<FJsonObject>>>(
				Async(EAsyncExecution::ThreadPool, [Roots]() { return BuildManifest(Roots); }));
			return false;
		}
		if (!Request->Hashing->IsReady())
		{
			return false;
		}
		TSharedPtr<FJsonObject> Result = Request->Hashing->Get();
		Result->SetStringField(TEXT("project"), FApp::GetProjectName());
		Result->SetObjectField(TEXT("export"), Request->Export);
		Bridge.Respond(P.Id, MakeShared<FJsonValueObject>(Result));
		return true;
	};
	Bridge.AddPending(MoveTemp(Pending));
}

void Read(UPlaytestSubsystem& Bridge, int64 Id, const TSharedPtr<FJsonObject>& Args)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("paths"), Values) || !Values)
	{
		Bridge.RespondError(Id, TEXT("codex_read needs paths"));
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Path;
		if (Value.IsValid() && Value->TryGetString(Path))
		{
			Paths.Add(Path);
		}
	}
	double Asked = (double)DefaultReadBytes;
	Args->TryGetNumberField(TEXT("max_bytes"), Asked);
	const int64 MaxBytes = FMath::Clamp((int64)Asked, (int64)1, MaxReadBytes);
	const FRoots Roots = GetRoots();
	if (Paths.Num() <= SyncReadFiles)
	{
		Bridge.Respond(Id, MakeShared<FJsonValueObject>(ReadFiles(Roots, Paths, MaxBytes)));
		return;
	}
	TSharedPtr<TFuture<TSharedPtr<FJsonObject>>> Reading = MakeShared<TFuture<TSharedPtr<FJsonObject>>>(
		Async(EAsyncExecution::ThreadPool, [Roots, Paths, MaxBytes]() { return ReadFiles(Roots, Paths, MaxBytes); }));
	FPending Pending;
	Pending.Id = Id;
	Pending.Op = TEXT("codex_read");
	Pending.StartReal = FPlatformTime::Seconds();
	Pending.Tick = [&Bridge, Reading](FPending& P, double Now)
	{
		if (!Reading->IsReady())
		{
			return false;
		}
		Bridge.Respond(P.Id, MakeShared<FJsonValueObject>(Reading->Get()));
		return true;
	};
	Bridge.AddPending(MoveTemp(Pending));
}

bool ExportProgress(int32& OutDone, int32& OutTotal)
{
	// (an export that ended is over for the tab, though no manifest request has seen it end yet: they are only polled
	// while connected)
	if (!Running.IsValid() || Running->bFinished || Running->bStopped || !FPlatformProcess::IsProcRunning(Running->Proc))
	{
		return false;
	}
	ReadProgress(*Running);  // (the tab asks every frame)
	OutDone = Running->Done;
	OutTotal = Running->Total;
	return true;
}

void StopExport()
{
	if (Running.IsValid() && !Running->bFinished && !Running->bStopped)
	{
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] project export: stopped"));
		Running->bStopped = true;
		// (the manifest request waiting for it sees it end and answers with what is there: the run goes on to its stop)
		FPlatformProcess::TerminateProc(Running->Proc, true);
	}
}

void Shutdown()
{
	if (Running.IsValid())
	{
		if (!Running->bFinished && FPlatformProcess::IsProcRunning(Running->Proc))
		{
			FPlatformProcess::TerminateProc(Running->Proc, true);
		}
		FPlatformProcess::CloseProc(Running->Proc);
		Running.Reset();
	}
}
}
