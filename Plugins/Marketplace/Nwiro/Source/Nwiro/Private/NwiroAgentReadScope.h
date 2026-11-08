// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroAgentReadScope.h - which files an ACP agent may read through fs/read_text_file.
//
// The bridge answers fs/read_text_file without a permission card, so this check is the only
// thing between an agent (or an instruction injected into something it read) and any file the
// editor process can open. Release gate B1 (2026-09-08): the Pro handler passed the agent's path
// straight to FFileHelper::LoadFileToString.
//
// Scope: the PROJECT directory or the ENGINE installation (agents legitimately read engine
// headers and config). Relative paths resolve under the project, the agent's working directory.
// The decision has two stages, and the caller may open only the path the second one returns.
//
// Stage 1, ResolveLexical - the string alone. Nothing touches the file system, so a UNC path
// outside both roots is refused without a network round trip. Refused everywhere: an empty path,
// an embedded NUL (the OS stops reading the name there, so the string checked would not be the
// file opened), and anything that is not under a root once "." and ".." are collapsed. Case is
// ignored here; stage 2 settles it. Refused on Windows only, because lexical containment lets
// them through there:
//   - a ':' anywhere except a leading drive root ("C:/"): NTFS alternate data streams
//     ("DefaultEngine.ini:hidden"), drive-relative paths ("C:Windows/..."), and the \\?\ device
//     namespace, whose drive colon is no longer at the start;
//   - a reserved DOS device name in any component (CON, NUL, AUX, COM1, "Content/lpt9.log", and
//     COM or LPT followed by a superscript 1-3): opening one reads a device, which can block the
//     game thread this handler runs on.
// Those are ordinary file names on macOS, so there they stay readable.
//
// Stage 2, ResolvePhysical - the file system. The OS reports the final path of the file with
// every junction and symlink followed (GetFinalPathNameByHandleW on Windows, realpath elsewhere),
// and it must lie under the project or engine directory resolved the same way. Both final paths
// carry the on-disk case, so they are compared exactly: a case-sensitive directory (WSL, fsutil)
// can hold "Game" and "game" side by side. A link inside the project that points elsewhere -
// planted by a tool, or shipped in a cloned repository or an unpacked archive - is refused; a
// project opened through a link still works. A missing file is "not found" only when the deepest
// name that exists is inside a root and is not a link; behind a link that leaves the roots, or at
// a dangling link, it is refused like an existing file, so the answer reveals nothing about paths
// outside. Nothing is opened for either answer. Win32 trims trailing dots and spaces only in ways
// that keep a name in its directory, and stage 2 opens names untrimmed (\\?\) anyway.
//
// Not covered:
//   - a hardlink inside a root to a file outside it, which has no separate final path. Making one
//     needs access to that file beforehand; git and ordinary archives do not create them;
//   - swapping a directory for a link between the check and the read, which needs something
//     writing to the project at that moment;
//   - following a link reaches its target before the decision: a Windows symbolic link to a
//     network share makes the OS contact (and possibly authenticate to) that host even though the
//     read is then refused, and macOS may automount while resolving a link. Creating a Windows
//     symbolic link needs Developer Mode or elevation, and a junction cannot point at a share.
//
// Mirrored in NwiroIKAgentReadScope.h and checked by scripts/check-plugin-parity.ts. The namespace
// names differ on purpose, so the identical inline functions of the two editor modules are never
// merged into one definition when both products load in one editor.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#endif

namespace NwiroAgentReadScope
{
	/** Only Allowed may be opened, and only at the path Resolve returned. */
	enum class EResult : uint8
	{
		Allowed,
		Refused,
		NotFound,
	};

	/** True for CON, PRN, AUX, NUL, CONIN$, CONOUT$, COM1-9 and LPT1-9 (superscript 1-3 too), with or without an extension. */
	inline bool IsReservedDeviceName(const FString& Component)
	{
		FString Base = Component;
		int32 Dot = INDEX_NONE;
		if (Base.FindChar(TEXT('.'), Dot))
		{
			Base.LeftInline(Dot);
		}
		Base.TrimEndInline();
		static const TCHAR* const Names[] = { TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"), TEXT("CONIN$"), TEXT("CONOUT$") };
		for (const TCHAR* Name : Names)
		{
			if (Base.Equals(Name, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		if (Base.Len() != 4 || !(Base.StartsWith(TEXT("COM"), ESearchCase::IgnoreCase) || Base.StartsWith(TEXT("LPT"), ESearchCase::IgnoreCase)))
		{
			return false;
		}
		// Windows also reads the Latin-1 superscripts U+00B9, U+00B2 and U+00B3 as the port digit.
		const TCHAR Digit = Base[3];
		return (Digit >= TEXT('1') && Digit <= TEXT('9')) || Digit == 0x00B9 || Digit == 0x00B2 || Digit == 0x00B3;
	}

	/**
	 * True when Path is Root or lies below it, on a component boundary. Stage 1 passes IgnoreCase,
	 * since it only narrows what stage 2 looks at; stage 2 passes CaseSensitive.
	 */
	inline bool IsUnder(const FString& Path, FString Root, ESearchCase::Type Case)
	{
		if (Root.Len() > 1 && Root.EndsWith(TEXT("/")) && !Root.EndsWith(TEXT(":/")))
		{
			Root.LeftChopInline(1);
		}
		if (Root.IsEmpty() || !Path.StartsWith(Root, Case))
		{
			return false;
		}
		return Path.Len() == Root.Len() || Root.EndsWith(TEXT("/")) || Path[Root.Len()] == TEXT('/');
	}

	/**
	 * Stage 1, the string alone: true when Requested is under a root, with OutFullPath the
	 * normalized absolute path. On false OutFullPath is empty. Tests pass their own roots.
	 */
	inline bool ResolveLexical(const FString& Requested, const FString& ProjectDir, const FString& EngineDir, FString& OutFullPath)
	{
		OutFullPath.Reset();
		if (Requested.IsEmpty() || FCString::Strlen(*Requested) != Requested.Len())
		{
			return false;
		}

		FString Path = Requested;
#if PLATFORM_WINDOWS
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		const bool bDriveRoot = Path.Len() > 2 && Path[1] == TEXT(':') && Path[2] == TEXT('/')
			&& ((Path[0] >= TEXT('A') && Path[0] <= TEXT('Z')) || (Path[0] >= TEXT('a') && Path[0] <= TEXT('z')));
		int32 Colon = INDEX_NONE;
		if (Path.FindLastChar(TEXT(':'), Colon) && !(Colon == 1 && bDriveRoot))
		{
			return false;
		}

		TArray<FString> Components;
		Path.ParseIntoArray(Components, TEXT("/"), true);
		for (const FString& Component : Components)
		{
			if (IsReservedDeviceName(Component))
			{
				return false;
			}
		}
#endif

		const FString Full = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::Combine(ProjectDir, Path) : Path);
		if (!IsUnder(Full, FPaths::ConvertRelativePathToFull(ProjectDir), ESearchCase::IgnoreCase)
			&& !IsUnder(Full, FPaths::ConvertRelativePathToFull(EngineDir), ESearchCase::IgnoreCase))
		{
			return false;
		}
		OutFullPath = Full;
		return true;
	}

#if PLATFORM_WINDOWS
	/**
	 * FullPath in the \\?\ form, which the OS opens exactly as written: no trimming of trailing
	 * dots or spaces, no device names, no MAX_PATH limit. It also takes separators literally.
	 */
	inline FString ToExtendedPath(const FString& FullPath)
	{
		FString Native = FullPath.Replace(TEXT("/"), TEXT("\\"));
		const bool bUnc = Native.StartsWith(TEXT("\\\\"));
		if (bUnc)
		{
			Native.RightChopInline(2);
		}
		while (Native.ReplaceInline(TEXT("\\\\"), TEXT("\\")) > 0)
		{
		}
		if (Native.EndsWith(TEXT(":")))
		{
			Native += TEXT("\\");
		}
		else if (Native.EndsWith(TEXT("\\")) && !Native.EndsWith(TEXT(":\\")))
		{
			Native.LeftChopInline(1);
		}
		return (bUnc ? TEXT("\\\\?\\UNC\\") : TEXT("\\\\?\\")) + Native;
	}
#endif

	/**
	 * The final path of an existing file or directory, every junction and symlink followed, in
	 * FPaths form. False when there is none; bOutMissing then says whether nothing exists there.
	 */
	inline bool FinalPath(const FString& FullPath, FString& OutFinal, bool& bOutMissing)
	{
		OutFinal.Reset();
		bOutMissing = false;
#if PLATFORM_WINDOWS
		const HANDLE Handle = ::CreateFileW(*ToExtendedPath(FullPath), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
		{
			const uint32 Error = ::GetLastError();
			bOutMissing = Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND;
			return false;
		}
		TArray<TCHAR> Buffer;
		Buffer.SetNumZeroed(MAX_PATH);
		uint32 Length = ::GetFinalPathNameByHandleW(Handle, Buffer.GetData(), static_cast<uint32>(Buffer.Num()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (Length >= static_cast<uint32>(Buffer.Num()))
		{
			Buffer.SetNumZeroed(static_cast<int32>(Length) + 1);
			Length = ::GetFinalPathNameByHandleW(Handle, Buffer.GetData(), static_cast<uint32>(Buffer.Num()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		}
		::CloseHandle(Handle);
		if (Length == 0 || Length >= static_cast<uint32>(Buffer.Num()))
		{
			return false;
		}

		FString Final = Buffer.GetData();
		if (Final.StartsWith(TEXT("\\\\?\\UNC\\")))
		{
			Final = TEXT("\\\\") + Final.RightChop(8);
		}
		else if (Final.StartsWith(TEXT("\\\\?\\")))
		{
			Final.RightChopInline(4);
		}
		OutFinal = Final.Replace(TEXT("\\"), TEXT("/"));
		return true;
#else
		ANSICHAR Resolved[PATH_MAX];
		if (::realpath(TCHAR_TO_UTF8(*FullPath), Resolved) == nullptr)
		{
			bOutMissing = errno == ENOENT || errno == ENOTDIR;
			return false;
		}
		OutFinal = UTF8_TO_TCHAR(Resolved);
		return true;
#endif
	}

	/** True when something exists at FullPath; a link there counts as itself and is not followed. */
	inline bool NameExists(const FString& FullPath)
	{
#if PLATFORM_WINDOWS
		const HANDLE Handle = ::CreateFileW(*ToExtendedPath(FullPath), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		::CloseHandle(Handle);
		return true;
#else
		struct stat Info;
		return ::lstat(TCHAR_TO_UTF8(*FullPath), &Info) == 0;
#endif
	}

	/**
	 * Stage 2, the file system: FullPath (stage 1's result) must still be under the project or the
	 * engine directory once every junction and symlink is followed, on both sides. On Allowed,
	 * OutPath is that final path, the only one the caller may open; otherwise it is empty.
	 */
	inline EResult ResolvePhysical(const FString& FullPath, const FString& ProjectDir, const FString& EngineDir, FString& OutPath)
	{
		OutPath.Reset();
		auto Inside = [&ProjectDir, &EngineDir](const FString& Final)
		{
			for (const FString* Root : { &ProjectDir, &EngineDir })
			{
				FString FinalRoot;
				bool bRootMissing = false;
				if (FinalPath(FPaths::ConvertRelativePathToFull(*Root), FinalRoot, bRootMissing) && IsUnder(Final, FinalRoot, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		};

		FString Final;
		bool bMissing = false;
		if (FinalPath(FullPath, Final, bMissing))
		{
			if (!Inside(Final))
			{
				return EResult::Refused;
			}
			OutPath = Final;
			return EResult::Allowed;
		}
		if (!bMissing)
		{
			return EResult::Refused;
		}

		// Missing: find the deepest name that exists. If that is FullPath itself, it is a dangling
		// link; otherwise it must resolve inside a root for the answer to be "not found".
		FString Probe = FullPath;
		while (!NameExists(Probe))
		{
			const FString Parent = FPaths::GetPath(Probe);
			if (Parent.IsEmpty() || Parent.Len() >= Probe.Len())
			{
				return EResult::Refused;
			}
			Probe = Parent;
		}
		FString FinalProbe;
		bool bProbeMissing = false;
		const bool bInsideDirectory = Probe.Len() < FullPath.Len() && FinalPath(Probe, FinalProbe, bProbeMissing) && Inside(FinalProbe);
		return bInsideDirectory ? EResult::NotFound : EResult::Refused;
	}

	/**
	 * What the fs/read_text_file handler calls: stage 1, then stage 2. ProjectDir and EngineDir are
	 * FPaths::ProjectDir() and FPaths::EngineDir() at the call site.
	 */
	inline EResult Resolve(const FString& Requested, const FString& ProjectDir, const FString& EngineDir, FString& OutPath)
	{
		FString FullPath;
		if (!ResolveLexical(Requested, ProjectDir, EngineDir, FullPath))
		{
			OutPath.Reset();
			return EResult::Refused;
		}
		return ResolvePhysical(FullPath, ProjectDir, EngineDir, OutPath);
	}
}
