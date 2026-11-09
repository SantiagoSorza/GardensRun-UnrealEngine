// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroImageDropSpec.cpp - image files dropped onto the panel from the operating system
// (NwiroImageDrop.h). Pins which files count as images, the position the page receives as
// fractions of the panel, and the drop payload: the first image as a data URL, an oversized or
// unreadable first file reported instead, later files skipped unread, and the drop places the
// page reports, which decide where a drag is accepted. Also the attachment save behind
// SaveImageBase64: one file per image whatever its name, kept in its folder, and an empty path
// when nothing could be saved. Fixtures live under Saved/ and are
// removed afterwards. Mirrored in NwiroIKImageDropSpec.cpp and checked by
// scripts/check-plugin-parity.ts.

#include "NwiroImageDrop.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Layout/Geometry.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace NwiroImageDropSpec
{
	static TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	/** A 1x1 PNG. */
	static const TCHAR* TinyPng =
		TEXT("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroImageDropFilesSpec,
	"Nwiro.ImageDrop.ImageFiles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroImageDropFilesSpec::RunTest(const FString& Parameters)
{
	const TArray<FString> Images = NwiroImageDrop::ImageFiles({
		TEXT("C:/a/sky.PNG"), TEXT("C:/a/notes.txt"), TEXT("C:/a/photo.jpeg"), TEXT("C:/a/scan.bmp"),
		TEXT("C:/a/anim.gif"), TEXT("C:/a/noext"), TEXT("C:/a/shot.jpg"),
	});
	TestEqual(TEXT("png, jpg, jpeg and bmp are kept in order, case-insensitively"), Images,
		TArray<FString>{ TEXT("C:/a/sky.PNG"), TEXT("C:/a/photo.jpeg"), TEXT("C:/a/scan.bmp"), TEXT("C:/a/shot.jpg") });
	TestEqual(TEXT("no images in a drag of other files"), NwiroImageDrop::ImageFiles({ TEXT("C:/a/b.uasset") }).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroImageDropFractionSpec,
	"Nwiro.ImageDrop.Fraction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroImageDropFractionSpec::RunTest(const FString& Parameters)
{
	// A 200x100 panel whose top-left corner sits at (1000, 500) on screen, drawn at 1.5x.
	const FGeometry Panel = FGeometry::MakeRoot(FVector2D(200.0, 100.0), FSlateLayoutTransform(1.5f, FVector2D(1000.0, 500.0)));
	const FVector2D Inside = NwiroImageDrop::Fraction(Panel, FVector2D(1000.0 + 50.0 * 1.5, 500.0 + 75.0 * 1.5));
	TestEqual(TEXT("x is a fraction of the width, whatever the scale"), Inside.X, 0.25, 1e-6);
	TestEqual(TEXT("y is a fraction of the height, whatever the scale"), Inside.Y, 0.75, 1e-6);
	const FVector2D Outside = NwiroImageDrop::Fraction(Panel, FVector2D(900.0, 900.0));
	TestEqual(TEXT("left of the panel clamps to 0"), Outside.X, 0.0, 1e-9);
	TestEqual(TEXT("below the panel clamps to 1"), Outside.Y, 1.0, 1e-9);

	const TSharedPtr<FJsonObject> Drag = NwiroImageDropSpec::Parse(NwiroImageDrop::DragJson(Inside));
	if (!TestTrue(TEXT("the drag payload is JSON"), Drag.IsValid())) return true;
	TestEqual(TEXT("the drag payload carries x"), Drag->GetNumberField(TEXT("x")), 0.25, 1e-6);
	TestEqual(TEXT("the drag payload carries y"), Drag->GetNumberField(TEXT("y")), 0.75, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroImageDropPayloadSpec,
	"Nwiro.ImageDrop.DropPayload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroImageDropPayloadSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroImageDropSpec;
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroTests"), TEXT("ImageDrop"), FGuid::NewGuid().ToString(EGuidFormats::Short));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	TArray<uint8> Png;
	FBase64::Decode(TinyPng, Png);
	const FString Small = FPaths::Combine(Root, TEXT("sky.png"));
	const FString Large = FPaths::Combine(Root, TEXT("huge.jpg"));
	const FString Missing = FPaths::Combine(Root, TEXT("gone.bmp"));
	TArray<uint8> LargeBytes;
	LargeBytes.SetNumZeroed(2048);
	if (!TestTrue(TEXT("fixtures written"), FFileHelper::SaveArrayToFile(Png, *Small) && FFileHelper::SaveArrayToFile(LargeBytes, *Large))) return true;

	const TSharedPtr<FJsonObject> Drop = Parse(NwiroImageDrop::DropJson({ Small, Large, Missing }, FVector2D(0.5, 0.25), /*MaxBytes*/ 1024));
	if (!TestTrue(TEXT("the drop payload is JSON"), Drop.IsValid())) return true;
	TestEqual(TEXT("x"), Drop->GetNumberField(TEXT("x")), 0.5, 1e-6);
	TestEqual(TEXT("y"), Drop->GetNumberField(TEXT("y")), 0.25, 1e-6);

	const TArray<TSharedPtr<FJsonValue>>& Files = Drop->GetArrayField(TEXT("files"));
	if (TestEqual(TEXT("the first image is read"), Files.Num(), 1))
	{
		const TSharedPtr<FJsonObject> File = Files[0]->AsObject();
		TestEqual(TEXT("name"), File->GetStringField(TEXT("name")), FString(TEXT("sky.png")));
		TestEqual(TEXT("path"), File->GetStringField(TEXT("path")), Small);
		TestEqual(TEXT("preview is the file as a PNG data URL"), File->GetStringField(TEXT("preview")),
			FString(TEXT("data:image/png;base64,")) + TinyPng);
	}

	// Only the first image is read: the later ones are skipped as too_many, not measured or
	// opened (the oversized one would otherwise be too_large, the missing one unreadable).
	const TArray<TSharedPtr<FJsonValue>>& Skipped = Drop->GetArrayField(TEXT("skipped"));
	if (TestEqual(TEXT("both later files are reported"), Skipped.Num(), 2))
	{
		TestEqual(TEXT("second name"), Skipped[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("huge.jpg")));
		TestEqual(TEXT("second reason"), Skipped[0]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("too_many")));
		TestEqual(TEXT("third name"), Skipped[1]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("gone.bmp")));
		TestEqual(TEXT("third reason"), Skipped[1]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("too_many")));
	}

	// A first file that cannot be used is reported with its own reason, and still nothing after it is read.
	const TSharedPtr<FJsonObject> Oversized = Parse(NwiroImageDrop::DropJson({ Large, Small }, FVector2D(0.5, 0.25), /*MaxBytes*/ 1024));
	if (!TestTrue(TEXT("the second drop payload is JSON"), Oversized.IsValid())) return true;
	TestEqual(TEXT("no image from an oversized first file"), Oversized->GetArrayField(TEXT("files")).Num(), 0);
	const TArray<TSharedPtr<FJsonValue>>& OversizedSkipped = Oversized->GetArrayField(TEXT("skipped"));
	if (TestEqual(TEXT("both files are reported"), OversizedSkipped.Num(), 2))
	{
		TestEqual(TEXT("first reason"), OversizedSkipped[0]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("too_large")));
		TestEqual(TEXT("second reason"), OversizedSkipped[1]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("too_many")));
	}
	const TSharedPtr<FJsonObject> OnlyMissing = Parse(NwiroImageDrop::DropJson({ Missing }, FVector2D(0.5, 0.25), 1024));
	if (TestTrue(TEXT("the third drop payload is JSON"), OnlyMissing.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>& MissingSkipped = OnlyMissing->GetArrayField(TEXT("skipped"));
		if (TestEqual(TEXT("the missing file is reported"), MissingSkipped.Num(), 1))
		{
			TestEqual(TEXT("missing reason"), MissingSkipped[0]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("unreadable")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroImageDropRegionsSpec,
	"Nwiro.ImageDrop.Regions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroImageDropRegionsSpec::RunTest(const FString& Parameters)
{
	// Regions are shared state; leave none behind, as before any page reported them.
	ON_SCOPE_EXIT { NwiroImageDrop::SetRegions({}); };

	TArray<FBox2D> Regions;
	TestTrue(TEXT("a list of places parses"), NwiroImageDrop::ParseRegions(
		TEXT("[{\"x0\":0.7,\"y0\":0.1,\"x1\":0.95,\"y1\":0.3},{\"x0\":0.5,\"y0\":0.5,\"x1\":0.5,\"y1\":0.6}]"), Regions));
	TestEqual(TEXT("an empty place is dropped"), Regions.Num(), 1);
	TestTrue(TEXT("an empty list parses"), NwiroImageDrop::ParseRegions(TEXT("[]"), Regions) && Regions.Num() == 0);
	TestFalse(TEXT("not JSON"), NwiroImageDrop::ParseRegions(TEXT("tile"), Regions));
	TestFalse(TEXT("a place without y1"), NwiroImageDrop::ParseRegions(TEXT("[{\"x0\":0,\"y0\":0,\"x1\":1}]"), Regions));
	TestEqual(TEXT("a rejected list yields no places"), Regions.Num(), 0);

	TestFalse(TEXT("nothing is a drop place before the page reports any"), NwiroImageDrop::IsOverRegion(FVector2D(0.8, 0.2)));
	NwiroImageDrop::SetRegions({ FBox2D(FVector2D(0.7, 0.1), FVector2D(0.95, 0.3)) });
	TestTrue(TEXT("inside the place"), NwiroImageDrop::IsOverRegion(FVector2D(0.8, 0.2)));
	TestTrue(TEXT("on its edge"), NwiroImageDrop::IsOverRegion(FVector2D(0.7, 0.3)));
	TestFalse(TEXT("left of it"), NwiroImageDrop::IsOverRegion(FVector2D(0.5, 0.2)));
	TestFalse(TEXT("below it"), NwiroImageDrop::IsOverRegion(FVector2D(0.8, 0.5)));
	NwiroImageDrop::SetRegions({});
	TestFalse(TEXT("a page without drop places accepts nothing"), NwiroImageDrop::IsOverRegion(FVector2D(0.8, 0.2)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroImageDropSaveAttachmentSpec,
	"Nwiro.ImageDrop.SaveAttachment", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroImageDropSaveAttachmentSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroImageDropSpec;
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("NwiroTests"), TEXT("ImageDrop"), FGuid::NewGuid().ToString(EGuidFormats::Short)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString Dir = FPaths::Combine(Root, TEXT("NwiroImages"));

	const auto InDir = [&Dir](FString Path)
	{
		FString Expected = Dir;
		FPaths::NormalizeFilename(Path);
		FPaths::NormalizeFilename(Expected);
		return FPaths::GetPath(Path) == Expected;
	};
	const auto Read = [](const FString& Path)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path);
		return Bytes;
	};
	const auto Encode = [](const char* Text)
	{
		return FBase64::Encode(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	};

	// Two different images with the same name, e.g. Screenshot_1.png from two folders.
	const FString First = NwiroImageDrop::SaveAttachment(Encode("first image"), TEXT("Screenshot_1.png"), Dir);
	const FString Second = NwiroImageDrop::SaveAttachment(Encode("second image"), TEXT("Screenshot_1.png"), Dir);
	if (!TestFalse(TEXT("the first image is saved"), First.IsEmpty()) || !TestFalse(TEXT("the second image is saved"), Second.IsEmpty())) return true;
	TestNotEqual(TEXT("the same name gives two files"), First, Second);
	TestEqual(TEXT("the first file keeps its own bytes"), Read(First),
		TArray<uint8>(reinterpret_cast<const uint8*>("first image"), 11));
	TestEqual(TEXT("the second file keeps its own bytes"), Read(Second),
		TArray<uint8>(reinterpret_cast<const uint8*>("second image"), 12));
	TestEqual(TEXT("the extension is kept"), FPaths::GetExtension(First), FString(TEXT("png")));
	TestTrue(TEXT("the name starts with the original name"), FPaths::GetBaseFilename(First).StartsWith(TEXT("Screenshot_1_")));
	TestTrue(TEXT("both files are in the folder"), InDir(First) && InDir(Second));

	// A data URL is accepted as well as bare base64.
	const FString FromDataUrl = NwiroImageDrop::SaveAttachment(FString(TEXT("data:image/png;base64,")) + TinyPng, TEXT("clipboard_1.png"), Dir);
	TArray<uint8> Png;
	FBase64::Decode(TinyPng, Png);
	TestEqual(TEXT("a data URL saves the decoded image"), Read(FromDataUrl), Png);

	// Folder parts never take the file out of the folder.
	for (const TCHAR* Escaping : { TEXT("..\\..\\escape.jpg"), TEXT("../../escape.jpg"), TEXT("C:\\Windows\\escape.jpg") })
	{
		const FString Saved = NwiroImageDrop::SaveAttachment(TinyPng, Escaping, Dir);
		TestTrue(FString::Printf(TEXT("%s is saved in the folder"), Escaping), !Saved.IsEmpty() && InDir(Saved));
		TestTrue(FString::Printf(TEXT("%s keeps its file name and extension"), Escaping),
			FPaths::GetCleanFilename(Saved).StartsWith(TEXT("escape_")) && FPaths::GetExtension(Saved) == TEXT("jpg"));
	}
	const FString OnlyDots = NwiroImageDrop::SaveAttachment(TinyPng, TEXT(".."), Dir);
	TestTrue(TEXT("a name of dots only is saved in the folder"), !OnlyDots.IsEmpty() && InDir(OnlyDots));

	// Nothing to save, or nowhere to save it: an empty path, never a path to a missing file.
	TestTrue(TEXT("empty data gives an empty path"), NwiroImageDrop::SaveAttachment(TEXT(""), TEXT("a.png"), Dir).IsEmpty());
	// Malformed base64: a character outside the alphabet, and a length one past a full chunk.
	const FString MalformedDir = FPaths::Combine(Root, TEXT("Malformed"));
	TestTrue(TEXT("a character outside base64 gives an empty path"),
		NwiroImageDrop::SaveAttachment(TEXT("iVBO@@@@"), TEXT("a.png"), MalformedDir).IsEmpty());
	TestTrue(TEXT("a broken base64 length gives an empty path"),
		NwiroImageDrop::SaveAttachment(TEXT("data:image/png;base64,AAAAA"), TEXT("a.png"), MalformedDir).IsEmpty());
	TArray<FString> MalformedFiles;
	IFileManager::Get().FindFilesRecursive(MalformedFiles, *MalformedDir, TEXT("*"), /*Files*/ true, /*Directories*/ false);
	TestEqual(TEXT("malformed data writes no file"), MalformedFiles.Num(), 0);
	const FString Blocked = FPaths::Combine(Root, TEXT("blocked"));
	if (TestTrue(TEXT("blocking file written"), FFileHelper::SaveStringToFile(TEXT("not a folder"), *Blocked)))
	{
		TestTrue(TEXT("a folder that cannot be made gives an empty path"),
			NwiroImageDrop::SaveAttachment(TinyPng, TEXT("a.png"), Blocked).IsEmpty());
	}
	return true;
}

#endif
