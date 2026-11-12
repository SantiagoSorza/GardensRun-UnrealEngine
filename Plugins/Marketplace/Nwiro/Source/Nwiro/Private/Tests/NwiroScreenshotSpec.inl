// Copyright 2026 Nwiro. All Rights Reserved.
//
// Included at file scope from NwiroEditorTools.cpp under WITH_DEV_AUTOMATION_TESTS.

#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroScreenshotOpaquePngSpec,
	"Nwiro.Pro.Editor.ScreenshotOpaquePng",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroScreenshotOpaquePngSpec::RunTest(const FString& Parameters)
{
	constexpr int32 Width = 2;
	constexpr int32 Height = 2;
	TArray<FColor> Pixels{
		FColor(10, 20, 30, 0),
		FColor(40, 50, 60, 1),
		FColor(70, 80, 90, 128),
		FColor(100, 110, 120, 254)
	};
	const TArray<FColor> ExpectedPixels = Pixels;

	const FString TestDir = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation/NwiroScreenshot"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString OutputPath = FPaths::Combine(TestDir, TEXT("opaque.png"));

	FString Error;
	const bool bSaved = NwiroScreenshotPrivate::EncodeAndSaveOpaquePng(
		Pixels, Width, Height, OutputPath, Error);
	TestTrue(TEXT("Opaque PNG save succeeds"), bSaved);
	if (!bSaved)
	{
		AddError(FString::Printf(TEXT("Save failed: %s"), *Error));
	}

	IFileManager& FileManager = IFileManager::Get();
	TestTrue(TEXT("Missing output directory is created"), FileManager.DirectoryExists(*TestDir));
	TestTrue(TEXT("PNG exists at the requested path"), FileManager.FileExists(*OutputPath));

	TArray64<uint8> Encoded;
	const bool bLoaded = FFileHelper::LoadFileToArray(Encoded, *OutputPath);
	TestTrue(TEXT("Written PNG can be reopened"), bLoaded && Encoded.Num() > 0);

	if (bLoaded && Encoded.Num() > 0)
	{
		IImageWrapperModule& ImageModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Decoder = ImageModule.CreateImageWrapper(EImageFormat::PNG);
		const bool bHeaderDecoded =
			Decoder.IsValid() && Decoder->SetCompressed(Encoded.GetData(), Encoded.Num());
		TestTrue(TEXT("Written file has a decodable PNG header"), bHeaderDecoded);

		if (bHeaderDecoded)
		{
			TestEqual(TEXT("Decoded width matches"), Decoder->GetWidth(), static_cast<int64>(Width));
			TestEqual(TEXT("Decoded height matches"), Decoder->GetHeight(), static_cast<int64>(Height));
			TestEqual(TEXT("Decoded PNG uses 8-bit channels"), Decoder->GetBitDepth(), 8);

			const ERGBFormat Format = Decoder->GetFormat();
			const bool bFourChannel = Format == ERGBFormat::BGRA || Format == ERGBFormat::RGBA;
			TestTrue(TEXT("Decoded PNG has four color channels"), bFourChannel);

			TArray64<uint8> RawPixels;
			const bool bPixelsDecoded = bFourChannel && Decoder->GetRaw(RawPixels);
			TestTrue(TEXT("Written PNG pixels decode"), bPixelsDecoded);
			TestEqual(TEXT("Decoded byte count matches"), RawPixels.Num(), static_cast<int64>(Width * Height * 4));

			if (bPixelsDecoded && RawPixels.Num() == Width * Height * 4)
			{
				for (int32 Index = 0; Index < Width * Height; ++Index)
				{
					const int64 Base = static_cast<int64>(Index) * 4;
					const uint8 Red = Format == ERGBFormat::RGBA ? RawPixels[Base] : RawPixels[Base + 2];
					const uint8 Green = RawPixels[Base + 1];
					const uint8 Blue = Format == ERGBFormat::RGBA ? RawPixels[Base + 2] : RawPixels[Base];

					TestEqual(*FString::Printf(TEXT("Pixel %d red is preserved"), Index), Red, ExpectedPixels[Index].R);
					TestEqual(*FString::Printf(TEXT("Pixel %d green is preserved"), Index), Green, ExpectedPixels[Index].G);
					TestEqual(*FString::Printf(TEXT("Pixel %d blue is preserved"), Index), Blue, ExpectedPixels[Index].B);
					TestEqual(*FString::Printf(TEXT("Pixel %d alpha is opaque"), Index), RawPixels[Base + 3], uint8(255));
				}
			}
		}
	}

	TArray<FColor> IncompletePixels;
	IncompletePixels.SetNum(Width * Height - 1);
	const FString IncompletePath = FPaths::Combine(TestDir, TEXT("incomplete.png"));
	Error.Reset();
	TestFalse(
		TEXT("Incomplete viewport buffers are rejected"),
		NwiroScreenshotPrivate::EncodeAndSaveOpaquePng(
			IncompletePixels, Width, Height, IncompletePath, Error));
	TestTrue(
		TEXT("Incomplete-buffer error identifies the pixel-count mismatch"),
		Error.Contains(TEXT("pixel count"), ESearchCase::IgnoreCase));
	TestFalse(
		TEXT("An incomplete viewport buffer writes no PNG"),
		FileManager.FileExists(*IncompletePath));

	const TSharedPtr<FJsonObject> UnsafeCommand = MakeShared<FJsonObject>();
	UnsafeCommand->SetStringField(TEXT("filename"), TEXT("../escape.png"));
	FString ResolvedFilename;
	Error.Reset();
	TestFalse(
		TEXT("Screenshot filenames cannot escape the screenshot directory"),
		NwiroScreenshotPrivate::ResolveFilename(UnsafeCommand, ResolvedFilename, Error));

	FileManager.DeleteDirectory(*TestDir, false, true);
	return true;
}
