// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroAgentReadScopeSpec.cpp - accept/refuse matrix for NwiroAgentReadScope, the check in front
// of the silent ACP fs/read_text_file handler (release gate B1). The string cases pass their own
// roots, so they do not depend on where the test project lives. The link cases build a small tree
// under Saved/ with real directory links - junctions on Windows, which need no elevation, symlinks
// elsewhere - that only ever point inside that tree. Each refusal group maps to one rule, so
// removing the rule turns its cases red. Mirrored in NwiroIKAgentReadScopeSpec.cpp and checked by
// scripts/check-plugin-parity.ts.

#include "NwiroAgentReadScope.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#if !PLATFORM_WINDOWS
#include <unistd.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroAgentReadScopeSpec,
	"Nwiro.AgentReadScope.Resolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroAgentReadScopeSpec::RunTest(const FString& Parameters)
{
	using NwiroAgentReadScope::EResult;

	// Stage 1: the string alone, against roots that need not exist.
#if PLATFORM_WINDOWS
	const FString Root = TEXT("C:/NwiroReadScope/");
	const FString FileSystemRoot = TEXT("C:/");
#else
	const FString Root = TEXT("/NwiroReadScope/");
	const FString FileSystemRoot = TEXT("/");
#endif
	const FString Game = Root + TEXT("Game/");
	const FString Engine = Root + TEXT("Engine/");

	auto Allowed = [this, &Game, &Engine](const TCHAR* What, const FString& In, const FString& Want)
	{
		FString Got;
		TestTrue(*FString::Printf(TEXT("allowed: %s"), What), NwiroAgentReadScope::ResolveLexical(In, Game, Engine, Got));
		TestEqual(*FString::Printf(TEXT("resolved: %s"), What), Got, Want);
	};
	auto Refused = [this, &Game, &Engine](const TCHAR* What, const FString& In)
	{
		FString Got = TEXT("unset");
		TestFalse(*FString::Printf(TEXT("refused: %s"), What), NwiroAgentReadScope::ResolveLexical(In, Game, Engine, Got));
		TestTrue(*FString::Printf(TEXT("no path returned: %s"), What), Got.IsEmpty());
	};

	// Reads agents need.
	Allowed(TEXT("relative project file"), TEXT("Config/DefaultEngine.ini"), Game + TEXT("Config/DefaultEngine.ini"));
	Allowed(TEXT("absolute project file"), Game + TEXT("Content/Maps/Level.umap"), Game + TEXT("Content/Maps/Level.umap"));
	Allowed(TEXT("dot-dot that stays inside"), TEXT("Content/../Config/DefaultGame.ini"), Game + TEXT("Config/DefaultGame.ini"));
	Allowed(TEXT("engine header"), Engine + TEXT("Source/Runtime/Core/Public/CoreMinimal.h"), Engine + TEXT("Source/Runtime/Core/Public/CoreMinimal.h"));
	Allowed(TEXT("case differs from the root, settled in stage 2"), Root + TEXT("game/Config/DefaultEngine.ini"), Root + TEXT("game/Config/DefaultEngine.ini"));

	Refused(TEXT("empty path"), TEXT(""));

	// Containment in the project or the engine tree.
	Refused(TEXT("absolute path outside both roots"), Root + TEXT("Outside/secret.txt"));
	Refused(TEXT("absolute path that climbs out of the project"), Game + TEXT("../Outside/secret.txt"));
	Refused(TEXT("relative path that climbs out of the project"), TEXT("../../../../../../../../../../Outside/secret.txt"));
	Refused(TEXT("sibling directory sharing the project name as a prefix"), Root + TEXT("GameEvil/secret.txt"));

	// The OS stops reading a name at a NUL, so the string checked would not be the file opened.
	FString WithNul = TEXT("Config/DefaultEngine.ini");
	WithNul.GetCharArray().Insert(TEXT('\0'), 6);
	Refused(TEXT("embedded NUL"), WithNul);

	// Final paths come from the file system with their on-disk case, so stage 2 compares exactly.
	TestTrue(TEXT("allowed: final path inside the root"), NwiroAgentReadScope::IsUnder(Game + TEXT("Config/DefaultEngine.ini"), Game, ESearchCase::CaseSensitive));
	TestTrue(TEXT("allowed: final path under a file-system root"), NwiroAgentReadScope::IsUnder(Game + TEXT("Config/DefaultEngine.ini"), FileSystemRoot, ESearchCase::CaseSensitive));
	TestFalse(TEXT("refused: final path of a case-only sibling"), NwiroAgentReadScope::IsUnder(Root + TEXT("game/secret.txt"), Game, ESearchCase::CaseSensitive));
	TestFalse(TEXT("refused: final path of a sibling sharing the root name as a prefix"), NwiroAgentReadScope::IsUnder(Root + TEXT("GameEvil/secret.txt"), Game, ESearchCase::CaseSensitive));

#if PLATFORM_WINDOWS
	Allowed(TEXT("backslash project file"), TEXT("C:\\NwiroReadScope\\Game\\Content\\Maps\\Level.umap"), Game + TEXT("Content/Maps/Level.umap"));
	Refused(TEXT("UNC path"), TEXT("//server/share/secret.txt"));
	Refused(TEXT("UNC path with backslashes"), TEXT("\\\\server\\share\\secret.txt"));

	// A colon only as a leading drive root.
	Refused(TEXT("alternate data stream of a project file"), Game + TEXT("Config/DefaultEngine.ini:hidden"));
	Refused(TEXT("drive-relative path"), TEXT("C:Windows/win.ini"));
	Refused(TEXT("device namespace path"), TEXT("\\\\?\\C:\\NwiroReadScope\\Game\\Config\\DefaultEngine.ini"));

	// Reserved device names, even inside the project.
	FString SuperscriptCom = TEXT("Saved/COM");
	SuperscriptCom.AppendChar(static_cast<TCHAR>(0x00B9));
	FString SuperscriptLpt = TEXT("Saved/lpt");
	SuperscriptLpt.AppendChar(static_cast<TCHAR>(0x00B3));
	SuperscriptLpt += TEXT(".log");
	Refused(TEXT("NUL"), TEXT("NUL"));
	Refused(TEXT("CON with an extension"), TEXT("Content/CON.txt"));
	Refused(TEXT("lower-case aux folder"), TEXT("Content/aux/readme.md"));
	Refused(TEXT("COM1"), TEXT("Saved/COM1"));
	Refused(TEXT("LPT9 with an extension"), TEXT("Saved/lpt9.log"));
	Refused(TEXT("CONOUT$"), TEXT("CONOUT$"));
	Refused(TEXT("COM with a superscript one"), SuperscriptCom);
	Refused(TEXT("LPT with a superscript three and an extension"), SuperscriptLpt);

	// A project on a network share.
	{
		const FString Share = TEXT("//server/share/Game/");
		FString Got;
		TestTrue(TEXT("allowed: relative read in a project on a network share"),
			NwiroAgentReadScope::ResolveLexical(TEXT("Config/DefaultEngine.ini"), Share, Engine, Got));
		TestTrue(TEXT("resolved: relative read in a project on a network share"), Got.EndsWith(TEXT("/server/share/Game/Config/DefaultEngine.ini")));
		TestFalse(TEXT("refused: sibling of a network-share project"),
			NwiroAgentReadScope::ResolveLexical(TEXT("//server/share/GameEvil/secret.txt"), Share, Engine, Got));
		TestFalse(TEXT("refused: another share on the same server"),
			NwiroAgentReadScope::ResolveLexical(TEXT("//server/other/Game/Config/DefaultEngine.ini"), Share, Engine, Got));
	}
#else
	// Windows file-name rules do not apply: these are ordinary names on macOS.
	Allowed(TEXT("aux folder"), TEXT("Content/aux/readme.md"), Game + TEXT("Content/aux/readme.md"));
	Allowed(TEXT("CON with an extension"), TEXT("Content/CON.txt"), Game + TEXT("Content/CON.txt"));
	Allowed(TEXT("colon in a file name"), TEXT("Config/notes:2026.txt"), Game + TEXT("Config/notes:2026.txt"));
#endif

	// Stage 2: links are followed before the decision. The tree's links only ever point inside
	// it, so even a delete that followed one would stay inside the tree.
	IFileManager& Files = IFileManager::Get();
	const FString Tree = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) / TEXT("NwiroAgentReadScopeSpec");
	const TCHAR* const Links[] = { TEXT("Game/LinkOut"), TEXT("Game/LinkIn"), TEXT("Game/Dangling"), TEXT("GameLink") };
	auto RemoveTree = [&Files, &Tree, &Links]()
	{
		for (const TCHAR* Link : Links)
		{
			// The link itself, never what it points at.
#if PLATFORM_WINDOWS
			Files.DeleteDirectory(*(Tree / Link), false, false);
#else
			unlink(TCHAR_TO_UTF8(*(Tree / Link)));
#endif
		}
		Files.DeleteDirectory(*Tree, false, true);
	};
	auto MakeLink = [&Tree](const TCHAR* Link, const TCHAR* Target)
	{
#if PLATFORM_WINDOWS
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""),
			*(Tree / Link).Replace(TEXT("/"), TEXT("\\")), *(Tree / Target).Replace(TEXT("/"), TEXT("\\"))), &ReturnCode, &StdOut, &StdErr);
		return ReturnCode == 0;
#else
		return symlink(TCHAR_TO_UTF8(*(Tree / Target)), TCHAR_TO_UTF8(*(Tree / Link))) == 0;
#endif
	};
	auto Put = [this, &Files, &Tree](const TCHAR* Rel, const TCHAR* Text)
	{
		const FString Path = Tree / Rel;
		TestTrue(*FString::Printf(TEXT("fixture: %s"), Rel), Files.MakeDirectory(*FPaths::GetPath(Path), true) && FFileHelper::SaveStringToFile(Text, *Path));
	};

	RemoveTree();
	Put(TEXT("Game/inside.txt"), TEXT("inside"));
	Put(TEXT("Game/Sub/deep.txt"), TEXT("deep"));
	Put(TEXT("Outside/secret.txt"), TEXT("secret"));
	TestTrue(TEXT("fixture: Engine"), Files.MakeDirectory(*(Tree / TEXT("Engine")), true));
	TestTrue(TEXT("fixture: Gone"), Files.MakeDirectory(*(Tree / TEXT("Gone")), true));
	const FString TreeGame = Tree / TEXT("Game/");
	const FString TreeEngine = Tree / TEXT("Engine/");
	const bool bLinks = MakeLink(TEXT("Game/LinkOut"), TEXT("Outside"))
		&& MakeLink(TEXT("Game/LinkIn"), TEXT("Game/Sub"))
		&& MakeLink(TEXT("Game/Dangling"), TEXT("Gone"))
		&& MakeLink(TEXT("GameLink"), TEXT("Game"))
		&& Files.DeleteDirectory(*(Tree / TEXT("Gone")), true, false);
	if (TestTrue(TEXT("link tree created"), bLinks))
	{
		FString Got;
		TestTrue(TEXT("allowed: plain file"), NwiroAgentReadScope::Resolve(TEXT("inside.txt"), TreeGame, TreeEngine, Got) == EResult::Allowed);
		TestTrue(TEXT("resolved: plain file"), Got.EndsWith(TEXT("/Game/inside.txt")));
		TestTrue(TEXT("allowed: link that stays inside the project"), NwiroAgentReadScope::Resolve(TEXT("LinkIn/deep.txt"), TreeGame, TreeEngine, Got) == EResult::Allowed);
		TestTrue(TEXT("resolved: the link's target, not the link"), Got.EndsWith(TEXT("/Game/Sub/deep.txt")));
		TestTrue(TEXT("refused: link that leads out of the project"), NwiroAgentReadScope::Resolve(TEXT("LinkOut/secret.txt"), TreeGame, TreeEngine, Got) == EResult::Refused);
		TestTrue(TEXT("no path returned: link that leads out of the project"), Got.IsEmpty());
		TestTrue(TEXT("allowed: project opened through a link"), NwiroAgentReadScope::Resolve(TEXT("inside.txt"), Tree / TEXT("GameLink/"), TreeEngine, Got) == EResult::Allowed);
		TestTrue(TEXT("resolved: project opened through a link"), Got.EndsWith(TEXT("/Game/inside.txt")));

		// A missing file is "not found" only inside the roots; elsewhere it must not tell the agent
		// whether an outside path exists.
		TestTrue(TEXT("not found: missing file"), NwiroAgentReadScope::Resolve(TEXT("missing.txt"), TreeGame, TreeEngine, Got) == EResult::NotFound);
		TestTrue(TEXT("not found: missing file behind a link that stays inside"), NwiroAgentReadScope::Resolve(TEXT("LinkIn/missing.txt"), TreeGame, TreeEngine, Got) == EResult::NotFound);
		TestTrue(TEXT("refused: missing file behind a link that leads out"), NwiroAgentReadScope::Resolve(TEXT("LinkOut/missing.txt"), TreeGame, TreeEngine, Got) == EResult::Refused);
		TestTrue(TEXT("refused: dangling link"), NwiroAgentReadScope::Resolve(TEXT("Dangling"), TreeGame, TreeEngine, Got) == EResult::Refused);
		TestTrue(TEXT("refused: file behind a dangling link"), NwiroAgentReadScope::Resolve(TEXT("Dangling/secret.txt"), TreeGame, TreeEngine, Got) == EResult::Refused);
#if PLATFORM_WINDOWS
		TestTrue(TEXT("allowed: case-variant spelling of a project file"), NwiroAgentReadScope::Resolve(Tree / TEXT("game/inside.txt"), TreeGame, TreeEngine, Got) == EResult::Allowed);
		TestTrue(TEXT("resolved: the on-disk spelling"), Got.EndsWith(TEXT("/Game/inside.txt"), ESearchCase::CaseSensitive));
#endif
	}

#if PLATFORM_WINDOWS
	// A directory can be case-sensitive on Windows (WSL, fsutil). "Game" and "game" are then two
	// folders, and only the one that is the project may be read. Setting the flag needs full
	// control of the directory, which the user's temp folder gives and a project drive may not.
	const FString Cased = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir()) / TEXT("NwiroAgentReadScopeSpecCased");
	Files.DeleteDirectory(*Cased, false, true);
	int32 FsutilCode = -1;
	FString FsutilOut;
	FString FsutilErr;
	if (Files.MakeDirectory(*Cased, true))
	{
		FPlatformProcess::ExecProcess(TEXT("fsutil.exe"), *FString::Printf(TEXT("file setCaseSensitiveInfo \"%s\" enable"),
			*Cased.Replace(TEXT("/"), TEXT("\\"))), &FsutilCode, &FsutilOut, &FsutilErr);
	}
	if (FsutilCode == 0)
	{
		for (const TCHAR* Rel : { TEXT("Game/inside.txt"), TEXT("game/secret.txt") })
		{
			const FString Path = Cased / Rel;
			TestTrue(*FString::Printf(TEXT("fixture: cased %s"), Rel),
				Files.MakeDirectory(*FPaths::GetPath(Path), true) && FFileHelper::SaveStringToFile(Rel, *Path));
		}
		FString Got;
		TestTrue(TEXT("refused: case-only sibling in a case-sensitive directory"),
			NwiroAgentReadScope::Resolve(Cased / TEXT("game/secret.txt"), Cased / TEXT("Game/"), TreeEngine, Got) == EResult::Refused);
		TestTrue(TEXT("allowed: project in a case-sensitive directory"),
			NwiroAgentReadScope::Resolve(TEXT("inside.txt"), Cased / TEXT("Game/"), TreeEngine, Got) == EResult::Allowed);
	}
	else
	{
		AddInfo(FString::Printf(TEXT("per-directory case sensitivity is not available here (fsutil exit %d), so the case-only sibling case did not run"), FsutilCode));
	}
	Files.DeleteDirectory(*Cased, false, true);
#endif

	RemoveTree();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
