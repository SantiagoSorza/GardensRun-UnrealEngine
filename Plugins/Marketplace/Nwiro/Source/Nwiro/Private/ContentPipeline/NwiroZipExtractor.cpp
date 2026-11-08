// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroZipExtractor.h"
#include "NwiroArchiveSafety.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace
{
	FString BuildTargetPath(
		const FString& OutputDirectory,
		const FString& EntryPath,
		const bool bStripRootFolder,
		FString& InOutRootPrefix)
	{
		FString NormalizedEntryPath = EntryPath;
		NormalizedEntryPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		NormalizedEntryPath.TrimStartAndEndInline();

		if (!bStripRootFolder)
		{
			return FPaths::Combine(OutputDirectory, NormalizedEntryPath);
		}

		if (InOutRootPrefix.IsEmpty())
		{
			int32 SlashIndex = INDEX_NONE;
			if (NormalizedEntryPath.FindChar(TEXT('/'), SlashIndex))
			{
				InOutRootPrefix = NormalizedEntryPath.Left(SlashIndex + 1);
			}
		}

		if (!InOutRootPrefix.IsEmpty() && NormalizedEntryPath.StartsWith(InOutRootPrefix))
		{
			NormalizedEntryPath.RightChopInline(InOutRootPrefix.Len());
		}

		return FPaths::Combine(OutputDirectory, NormalizedEntryPath);
	}
}

bool FNwiroZipExtractor::ExtractZip(
	const FString& ZipFilePath,
	const FString& OutputDirectory,
	const bool bStripRootFolder,
	FString& OutError)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.FileExists(*ZipFilePath))
	{
		OutError = FString::Printf(TEXT("Zip file does not exist: %s"), *ZipFilePath);
		return false;
	}

	if (!PlatformFile.DirectoryExists(*OutputDirectory) && !PlatformFile.CreateDirectoryTree(*OutputDirectory))
	{
		OutError = FString::Printf(TEXT("Failed to create output directory: %s"), *OutputDirectory);
		return false;
	}

	mz_zip_archive ZipArchive = {};
	if (!mz_zip_reader_init_file(&ZipArchive, TCHAR_TO_ANSI(*ZipFilePath), 0))
	{
		OutError = FString::Printf(TEXT("Failed to open zip archive: %s"), *ZipFilePath);
		return false;
	}

	const int32 FileCount = static_cast<int32>(mz_zip_reader_get_num_files(&ZipArchive));
	FString RootPrefix;

	// PASS 1 — VALIDATE EVERY ENTRY NAME BEFORE WRITING ANY BYTES.
	//
	// BuildTargetPath below normalises separators and FPaths::Combine()s the entry straight
	// onto OutputDirectory with no validation, and Combine with an ABSOLUTE second argument
	// discards the first entirely. So an entry named "../../evil.exe" or
	// "C:/Windows/System32/evil.dll" used to be written outside the destination — with this
	// helper reachable from asset import, which unpacks archives fetched from URLs supplied
	// by the renderer.
	//
	// One hostile entry condemns the WHOLE archive. Skipping the bad entry and continuing
	// would unpack the rest of an archive already known to be hostile, and doing the check
	// inside the write loop would mean earlier entries had already landed on disk.
	FString PreRootPrefix;
	for (int32 FileIndex = 0; FileIndex < FileCount; ++FileIndex)
	{
		mz_zip_archive_file_stat PreStat = {};
		if (!mz_zip_reader_file_stat(&ZipArchive, FileIndex, &PreStat))
		{
			OutError = FString::Printf(TEXT("Failed to read zip entry stat for index %d"), FileIndex);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}
		const FString PreEntryPath = ANSI_TO_TCHAR(PreStat.m_filename);
		// Judge the name as the write pass will USE it: BuildTargetPath trims surrounding
		// spaces, so " ../x" becomes "../x". Also resolve the target exactly as the write
		// pass will (own copy of the root-prefix state) and check where it lands, so no
		// refusal can come after earlier entries are already on disk.
		const FString PreTargetPath =
			BuildTargetPath(OutputDirectory, PreEntryPath, bStripRootFolder, PreRootPrefix);
		if (!NwiroArchiveSafety::IsSafeMemberName(PreEntryPath)
			|| !NwiroArchiveSafety::IsSafeMemberName(PreEntryPath.TrimStartAndEnd())
			|| (!PreTargetPath.IsEmpty()
				&& !NwiroArchiveSafety::IsContainedWithin(OutputDirectory, PreTargetPath)))
		{
			// Deliberately does not echo the entry back: the name is attacker-controlled
			// and this string reaches logs and the renderer.
			OutError = FString::Printf(
				TEXT("Archive rejected: entry %d has an unsafe path."), FileIndex);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}
	}

	for (int32 FileIndex = 0; FileIndex < FileCount; ++FileIndex)
	{
		mz_zip_archive_file_stat Stat = {};
		if (!mz_zip_reader_file_stat(&ZipArchive, FileIndex, &Stat))
		{
			OutError = FString::Printf(TEXT("Failed to read zip entry stat for index %d"), FileIndex);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}

		const FString EntryPath = ANSI_TO_TCHAR(Stat.m_filename);
		const FString TargetPath = BuildTargetPath(OutputDirectory, EntryPath, bStripRootFolder, RootPrefix);
		if (TargetPath.IsEmpty())
		{
			continue;
		}

		// FINAL CONTAINMENT CHECK on the RESOLVED path, independent of the name check above.
		// That one decides whether a name looks safe; this one decides where the path
		// actually LANDS — so root-stripping, odd normalisation, or any future change to
		// BuildTargetPath cannot quietly reintroduce an escape. Cheap, and the last word.
		if (!NwiroArchiveSafety::IsContainedWithin(OutputDirectory, TargetPath))
		{
			OutError = FString::Printf(
				TEXT("Archive rejected: entry %d resolves outside the destination."), FileIndex);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}

		if (mz_zip_reader_is_file_a_directory(&ZipArchive, FileIndex))
		{
			if (!PlatformFile.DirectoryExists(*TargetPath) && !PlatformFile.CreateDirectoryTree(*TargetPath))
			{
				OutError = FString::Printf(TEXT("Failed to create directory: %s"), *TargetPath);
				mz_zip_reader_end(&ZipArchive);
				return false;
			}
			continue;
		}

		const FString TargetDirectory = FPaths::GetPath(TargetPath);
		if (!TargetDirectory.IsEmpty() &&
			!PlatformFile.DirectoryExists(*TargetDirectory) &&
			!PlatformFile.CreateDirectoryTree(*TargetDirectory))
		{
			OutError = FString::Printf(TEXT("Failed to create directory: %s"), *TargetDirectory);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}

		if (!mz_zip_reader_extract_to_file(&ZipArchive, FileIndex, TCHAR_TO_ANSI(*TargetPath), 0))
		{
			OutError = FString::Printf(TEXT("Failed to extract file: %s"), *TargetPath);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}
	}

	mz_zip_reader_end(&ZipArchive);
	return true;
}
