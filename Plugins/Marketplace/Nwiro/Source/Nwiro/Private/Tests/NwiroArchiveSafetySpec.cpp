// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroArchiveSafetySpec.cpp - zip-slip guard (IK-142) for the content-pipeline zip extractor.
// The predicate cases pin each refusal rule of NwiroArchiveSafety. The extract cases build REAL
// archives with miniz's writer, including hostile ones, and run the real extractor, because a
// predicate that works proves nothing about whether the extractor consults it. Every fixture
// lives under Saved/, and the escape targets are siblings of the output folder inside that same
// fixture root, so a regression writes into the fixture and never outside it. Mirrored in
// NwiroIKArchiveSafetySpec.cpp and checked by scripts/check-plugin-parity.ts.

#include "NwiroArchiveSafety.h"
#include "ContentPipeline/NwiroZipExtractor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace NwiroArchiveSafetySpec
{
	/** Builds a real zip from (entry name, contents) pairs; returns the entry count the archive
	 *  actually holds, so a fixture that silently failed to build cannot pass as a refusal. */
	static int32 MakeZip(const FString& ZipPath, const TArray<TPair<FString, FString>>& Entries)
	{
		IFileManager::Get().Delete(*ZipPath, false, true, true);

		mz_zip_archive Writer = {};
		if (!mz_zip_writer_init_file(&Writer, TCHAR_TO_ANSI(*ZipPath), 0)) return -1;
		for (const TPair<FString, FString>& Entry : Entries)
		{
			const FTCHARToUTF8 Payload(*Entry.Value);
			mz_zip_writer_add_mem(
				&Writer, TCHAR_TO_ANSI(*Entry.Key), Payload.Get(), Payload.Length(), MZ_NO_COMPRESSION);
		}
		mz_zip_writer_finalize_archive(&Writer);
		mz_zip_writer_end(&Writer);

		mz_zip_archive Reader = {};
		if (!mz_zip_reader_init_file(&Reader, TCHAR_TO_ANSI(*ZipPath), 0)) return -1;
		const int32 Count = static_cast<int32>(mz_zip_reader_get_num_files(&Reader));
		mz_zip_reader_end(&Reader);
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroArchiveSafetyPredicateSpec,
	"Nwiro.ArchiveSafety.Predicates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroArchiveSafetyPredicateSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroArchiveSafety;

	// Accepted: ordinary relative names, nested names, and names that merely contain dots.
	TestTrue(TEXT("plain file"), IsSafeMemberName(TEXT("a.txt")));
	TestTrue(TEXT("nested file"), IsSafeMemberName(TEXT("dir/sub/a.txt")));
	TestTrue(TEXT("backslash nested file"), IsSafeMemberName(TEXT("dir\\a.txt")));
	TestTrue(TEXT("directory entry"), IsSafeMemberName(TEXT("dir/")));
	TestTrue(TEXT("double dot inside a name"), IsSafeMemberName(TEXT("archive..backup.zip")));
	TestTrue(TEXT("dot-dot prefix of a name"), IsSafeMemberName(TEXT("..hidden")));

	// Rule: empty.
	TestFalse(TEXT("empty"), IsSafeMemberName(TEXT("")));
	// Rule: a ".." segment, in either separator form.
	TestFalse(TEXT("bare parent"), IsSafeMemberName(TEXT("..")));
	TestFalse(TEXT("leading parent"), IsSafeMemberName(TEXT("../evil.exe")));
	TestFalse(TEXT("inner parent"), IsSafeMemberName(TEXT("a/../../evil.exe")));
	TestFalse(TEXT("trailing parent"), IsSafeMemberName(TEXT("a/..")));
	TestFalse(TEXT("backslash parent"), IsSafeMemberName(TEXT("a\\..\\..\\evil.exe")));
	// Rule: absolute (POSIX root, UNC, drive letter).
	TestFalse(TEXT("posix root"), IsSafeMemberName(TEXT("/etc/passwd")));
	TestFalse(TEXT("unc forward"), IsSafeMemberName(TEXT("//server/share/x")));
	TestFalse(TEXT("unc backslash"), IsSafeMemberName(TEXT("\\\\server\\share\\x")));
	TestFalse(TEXT("drive letter"), IsSafeMemberName(TEXT("C:/Windows/evil.dll")));
	TestFalse(TEXT("drive relative"), IsSafeMemberName(TEXT("c:evil.dll")));
	// Rule: control characters.
	FString WithControl = TEXT("a");
	WithControl.AppendChar(TCHAR(1));
	WithControl += TEXT("b.txt");
	TestFalse(TEXT("control character"), IsSafeMemberName(WithControl));

	// Containment is decided on where the path lands, not on how it is spelled.
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroArchiveSafetySpec"), TEXT("Out"));
	TestTrue(TEXT("child"), IsContainedWithin(Root, Root / TEXT("a.txt")));
	TestTrue(TEXT("child after collapse"), IsContainedWithin(Root, Root / TEXT("sub/../a.txt")));
	TestTrue(TEXT("root with trailing slash"), IsContainedWithin(Root, Root + TEXT("/")));
	TestFalse(TEXT("parent escape"), IsContainedWithin(Root, Root / TEXT("../escape.txt")));
	TestFalse(TEXT("sibling sharing the prefix"), IsContainedWithin(Root, Root + TEXT("Sideways/x.txt")));
	TestFalse(TEXT("empty candidate"), IsContainedWithin(Root, TEXT("")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroArchiveSafetyExtractSpec,
	"Nwiro.ArchiveSafety.Extract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroArchiveSafetyExtractSpec::RunTest(const FString& Parameters)
{
	using NwiroArchiveSafetySpec::MakeZip;

	IFileManager& FileManager = IFileManager::Get();
	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroArchiveSafetySpec")));
	FileManager.DeleteDirectory(*Root, false, true);
	FileManager.MakeDirectory(*Root, true);
	ON_SCOPE_EXIT { FileManager.DeleteDirectory(*Root, false, true); };

	// Hostile archive, benign entry FIRST: the whole archive must be refused before any byte is
	// written, so ok.txt must be absent too, not just the escape.
	{
		const FString Out = Root / TEXT("OutHostile");
		const FString Zip = Root / TEXT("hostile.zip");
		const int32 Count = MakeZip(Zip, {
			{ TEXT("ok.txt"), TEXT("benign") },
			{ TEXT("../escape.txt"), TEXT("escaped") },
		});
		TestEqual(TEXT("hostile fixture holds both entries"), Count, 2);

		FString Error;
		TestFalse(TEXT("hostile archive refused"), FNwiroZipExtractor::ExtractZip(Zip, Out, false, Error));
		TestFalse(TEXT("escape target not written"), FileManager.FileExists(*(Root / TEXT("escape.txt"))));
		TestFalse(TEXT("nothing written before the refusal"), FileManager.FileExists(*(Out / TEXT("ok.txt"))));
		TestFalse(TEXT("error does not echo the entry name"), Error.Contains(TEXT("escape")));
	}

	// A leading space hides ".." from a raw-name check, and the write pass trims it: " ../x"
	// lands one level up. It must be refused in the validation pass, with the benign entry
	// first, so nothing at all is written.
	{
		const FString Out = Root / TEXT("OutPadded");
		const FString Zip = Root / TEXT("padded.zip");
		const int32 Count = MakeZip(Zip, {
			{ TEXT("ok.txt"), TEXT("benign") },
			{ TEXT(" ../escape-padded.txt"), TEXT("escaped") },
		});
		TestEqual(TEXT("padded fixture holds both entries"), Count, 2);

		FString Error;
		TestFalse(TEXT("padded archive refused"), FNwiroZipExtractor::ExtractZip(Zip, Out, false, Error));
		TestFalse(TEXT("padded escape target not written"), FileManager.FileExists(*(Root / TEXT("escape-padded.txt"))));
		TestFalse(TEXT("nothing written before the padded refusal"), FileManager.FileExists(*(Out / TEXT("ok.txt"))));
	}

	// Root stripping must not open a second route: "pkg/../x" strips to "../x", one level above
	// the output folder, which is still inside the fixture root.
	{
		const FString Out = Root / TEXT("OutStripHostile");
		const FString Zip = Root / TEXT("strip-hostile.zip");
		const int32 Count = MakeZip(Zip, {
			{ TEXT("pkg/a.txt"), TEXT("benign") },
			{ TEXT("pkg/../escape-strip.txt"), TEXT("escaped") },
		});
		TestEqual(TEXT("strip-hostile fixture holds both entries"), Count, 2);

		FString Error;
		TestFalse(TEXT("strip-hostile archive refused"), FNwiroZipExtractor::ExtractZip(Zip, Out, true, Error));
		TestFalse(TEXT("strip escape target not written"), FileManager.FileExists(*(Root / TEXT("escape-strip.txt"))));
	}

	// Benign archive with an explicit root directory entry, extracted with root stripping, the
	// way asset packs arrive. The stripped root entry resolves to the output folder itself, and
	// the guard must not refuse that.
	{
		const FString Out = Root / TEXT("OutBenign");
		const FString Zip = Root / TEXT("benign.zip");
		const int32 Count = MakeZip(Zip, {
			{ TEXT("pkg/"), TEXT("") },
			{ TEXT("pkg/sub/"), TEXT("") },
			{ TEXT("pkg/sub/a.txt"), TEXT("alpha") },
			{ TEXT("pkg/b..c.txt"), TEXT("bravo") },
		});
		TestEqual(TEXT("benign fixture holds all entries"), Count, 4);

		FString Error;
		TestTrue(TEXT("benign archive extracted"), FNwiroZipExtractor::ExtractZip(Zip, Out, true, Error));
		TestEqual(TEXT("benign archive reported no error"), Error, FString());

		FString Alpha, Bravo;
		TestTrue(TEXT("nested file written"), FFileHelper::LoadFileToString(Alpha, *(Out / TEXT("sub/a.txt"))));
		TestEqual(TEXT("nested file content"), Alpha, FString(TEXT("alpha")));
		TestTrue(TEXT("dotted file written"), FFileHelper::LoadFileToString(Bravo, *(Out / TEXT("b..c.txt"))));
		TestEqual(TEXT("dotted file content"), Bravo, FString(TEXT("bravo")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
