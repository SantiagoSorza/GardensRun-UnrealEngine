// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroEditorTools.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Editor.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
#include "Engine/GameViewportClient.h"
#include "RenderingThread.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroEditor, Log, All);

namespace NwiroScreenshotPrivate
{
	static FString SerializeJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	static FString MakeError(const FString& Error)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Error);
		return SerializeJson(Result);
	}

	static FString MakeSuccess(const FString& FullPath, int32 Width, int32 Height)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("path"), FullPath);
		Result->SetNumberField(TEXT("width"), Width);
		Result->SetNumberField(TEXT("height"), Height);
		return SerializeJson(Result);
	}

	static bool ResolveFilename(const TSharedPtr<FJsonObject>& Command, FString& OutFilename, FString& OutError)
	{
		OutFilename.Reset();
		OutError.Reset();

		if (Command->HasField(TEXT("filename")) && !Command->TryGetStringField(TEXT("filename"), OutFilename))
		{
			OutError = TEXT("filename must be a string");
			return false;
		}

		OutFilename.TrimStartAndEndInline();
		if (OutFilename.IsEmpty())
		{
			const FDateTime Now = FDateTime::Now();
			const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
			OutFilename = FString::Printf(TEXT("Screenshot_%s_%s.png"),
				*Now.ToString(TEXT("%Y%m%d_%H%M%S")), *UniqueSuffix);
		}

		if (OutFilename.Contains(TEXT("/")) || OutFilename.Contains(TEXT("\\"))
			|| OutFilename == TEXT(".") || OutFilename == TEXT(".."))
		{
			OutError = TEXT("filename must be a file name, not a path");
			return false;
		}

		FText InvalidPathReason;
		if (!FPaths::ValidatePath(OutFilename, &InvalidPathReason))
		{
			OutError = FString::Printf(TEXT("Invalid filename: %s"), *InvalidPathReason.ToString());
			return false;
		}

		const FString Extension = FPaths::GetExtension(OutFilename, false);
		if (Extension.IsEmpty())
		{
			OutFilename += TEXT(".png");
		}
		else if (!Extension.Equals(TEXT("png"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("filename must use the .png extension");
			return false;
		}

		return true;
	}

	static bool EncodeAndSaveOpaquePng(
		TArray<FColor>& Bitmap,
		int32 Width,
		int32 Height,
		const FString& FullPath,
		FString& OutError)
	{
		OutError.Reset();
		if (Width <= 0 || Height <= 0)
		{
			OutError = TEXT("Viewport has zero size");
			return false;
		}

		const int64 ExpectedPixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
		if (static_cast<int64>(Bitmap.Num()) != ExpectedPixelCount)
		{
			OutError = FString::Printf(TEXT("Viewport pixel count mismatch: expected %lld, got %d"),
				ExpectedPixelCount, Bitmap.Num());
			return false;
		}

		for (FColor& Pixel : Bitmap)
		{
			Pixel.A = 255;
		}

		const FImageView Image(Bitmap.GetData(), Width, Height, EGammaSpace::sRGB);
		TArray64<uint8> PngData;
		if (!FImageUtils::CompressImage(PngData, TEXT("png"), Image) || PngData.IsEmpty())
		{
			OutError = TEXT("Failed to encode viewport pixels as PNG");
			return false;
		}

		IFileManager& FileManager = IFileManager::Get();
		const FString SaveDir = FPaths::GetPath(FullPath);
		if (!FileManager.DirectoryExists(*SaveDir) && !FileManager.MakeDirectory(*SaveDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create screenshot directory: %s"), *SaveDir);
			return false;
		}

		if (!FFileHelper::SaveArrayToFile(PngData, *FullPath))
		{
			OutError = FString::Printf(TEXT("Failed to write screenshot PNG: %s"), *FullPath);
			return false;
		}

		const int64 PersistedSize = FileManager.FileSize(*FullPath);
		if (PersistedSize != PngData.Num())
		{
			OutError = FString::Printf(TEXT("Screenshot PNG validation failed after write: %s"), *FullPath);
			return false;
		}

		return true;
	}
}

// ============================================================
// LOG CAPTURE SINGLETON
// ============================================================

FNwiroLogCapture& FNwiroLogCapture::Get()
{
	static FNwiroLogCapture Instance;
	return Instance;
}

void FNwiroLogCapture::Register()
{
	GLog->AddOutputDevice(this);
}

void FNwiroLogCapture::Unregister()
{
	GLog->RemoveOutputDevice(this);
}

void FNwiroLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);

	FString Severity;
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal:   Severity = TEXT("Fatal"); break;
	case ELogVerbosity::Error:   Severity = TEXT("Error"); break;
	case ELogVerbosity::Warning: Severity = TEXT("Warning"); break;
	case ELogVerbosity::Display: Severity = TEXT("Display"); break;
	default:                     Severity = TEXT("Log"); break;
	}

	FCapturedLine Captured;
	Captured.Text = FString::Printf(TEXT("[%s][%s] %s"), *Severity, *Category.ToString(), V);
	Captured.Category = Category;
	Lines.Add(Captured);
	CategoryCounts.FindOrAdd(Category)++;

	// Evict from the LOUDEST category, not simply the oldest line. A single global FIFO lets
	// one chatty producer starve every other, and the filtering in GetRecentLines happens at
	// READ time — by then the evicted lines are gone, so a category filter cannot recover
	// them. Measured on a freshly opened editor: LogDesktopPlatform, LogContentValidation and
	// LogModuleManager held 68% of the buffer between them, and 31 of 1000 lines mentioned
	// Nwiro at all. Raising MaxLines only moves that threshold; this changes who pays for it.
	//
	// Self-balancing on purpose: no per-category budget to tune, and a category is only
	// trimmed while it is the largest, so rare lines — tool calls, errors — survive.
	while (Lines.Num() > MaxLines)
	{
		FName Loudest = NAME_None;
		int32 LoudestCount = -1;
		for (const TPair<FName, int32>& Pair : CategoryCounts)
		{
			if (Pair.Value > LoudestCount)
			{
				LoudestCount = Pair.Value;
				Loudest = Pair.Key;
			}
		}

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (Lines[i].Category == Loudest)
			{
				Lines.RemoveAt(i);
				if (int32* Count = CategoryCounts.Find(Loudest))
				{
					if (--(*Count) <= 0)
					{
						CategoryCounts.Remove(Loudest);
					}
				}
				break;
			}
		}
	}
}

TArray<FString> FNwiroLogCapture::GetRecentLines(int32 Count, const FString& SeverityFilter, const FString& CategoryFilter) const
{
	FScopeLock ScopeLock(&Lock);

	TArray<FString> Result;

	for (int32 i = Lines.Num() - 1; i >= 0 && Result.Num() < Count; --i)
	{
		const FString& Line = Lines[i].Text;

		if (!SeverityFilter.IsEmpty() && !Line.Contains(FString::Printf(TEXT("[%s]"), *SeverityFilter), ESearchCase::IgnoreCase))
			continue;
		if (!CategoryFilter.IsEmpty() && !Line.Contains(CategoryFilter, ESearchCase::IgnoreCase))
			continue;

		Result.Insert(Line, 0);
	}

	return Result;
}

// ============================================================
// TAKE SCREENSHOT
// ============================================================

FString FNwiroEditorTools::TakeScreenshot(const FString& JsonCommand)
{
	if (!IsInGameThread())
	{
		const TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise =
			MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroEditorTools::TakeScreenshot(JsonCommand));
		});
		return Future.Get();
	}

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return NwiroScreenshotPrivate::MakeError(TEXT("Invalid JSON"));
	}

	FString Filename;
	FString Error;
	if (!NwiroScreenshotPrivate::ResolveFilename(Cmd, Filename, Error))
	{
		return NwiroScreenshotPrivate::MakeError(Error);
	}

	const FString SaveDir = FPaths::ProjectSavedDir() / TEXT("NwiroScreenshots");
	FString FullPath = FPaths::ConvertRelativePathToFull(SaveDir / Filename);
	FPaths::NormalizeFilename(FullPath);

	// Get the active level editor viewport
	FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();

	if (!ActiveViewport.IsValid())
		return NwiroScreenshotPrivate::MakeError(TEXT("No active viewport"));

	FViewport* Viewport = ActiveViewport->GetActiveViewport();
	if (!Viewport)
		return NwiroScreenshotPrivate::MakeError(TEXT("No viewport found"));

	// Read pixels from viewport
	TArray<FColor> Bitmap;
	int32 Width = Viewport->GetSizeXY().X;
	int32 Height = Viewport->GetSizeXY().Y;

	if (Width <= 0 || Height <= 0)
		return NwiroScreenshotPrivate::MakeError(TEXT("Viewport has zero size"));

	// Force a synchronous redraw before sampling the buffer.
	//
	// The editor throttles viewport rendering whenever its window is not in the
	// foreground (bThrottleCPUWhenNotForeground defaults to true), and an agent
	// driving the chat panel means the editor is backgrounded by definition. In
	// that state the back buffer is never refreshed, so ReadPixels returns
	// whatever was last drawn and every capture in a session comes back
	// byte-identical to the first one.
	//
	// Invalidate() alone is NOT sufficient: it only marks the viewport dirty and
	// defers the redraw to the next editor tick, which never arrives while
	// throttled. Draw() renders synchronously on this thread (the caller above
	// has already marshalled us onto the game thread), and the flush then
	// guarantees the render thread has finished writing before we read.
	Viewport->Invalidate();
	Viewport->Draw(/*bShouldPresent=*/false);
	FlushRenderingCommands();

	if (!Viewport->ReadPixels(Bitmap))
		return NwiroScreenshotPrivate::MakeError(TEXT("Failed to read viewport pixels"));

	if (!NwiroScreenshotPrivate::EncodeAndSaveOpaquePng(Bitmap, Width, Height, FullPath, Error))
	{
		UE_LOG(LogNwiroEditor, Error, TEXT("Screenshot failed: %s"), *Error);
		return NwiroScreenshotPrivate::MakeError(Error);
	}

	UE_LOG(LogNwiroEditor, Log, TEXT("Screenshot saved: %s (%dx%d)"), *FullPath, Width, Height);

	return NwiroScreenshotPrivate::MakeSuccess(FullPath, Width, Height);
}

// ============================================================
// READ LOG
// ============================================================

FString FNwiroEditorTools::ReadLog(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	int32 LineCount = 50;
	FString Severity, Category;

	if (Cmd.IsValid())
	{
		if (Cmd->HasField(TEXT("lines"))) LineCount = (int32)Cmd->GetNumberField(TEXT("lines"));
		Severity = Cmd->GetStringField(TEXT("severity"));
		Category = Cmd->GetStringField(TEXT("category"));
	}

	TArray<FString> LogLines = FNwiroLogCapture::Get().GetRecentLines(LineCount, Severity, Category);

	TArray<TSharedPtr<FJsonValue>> LinesArr;
	for (const FString& L : LogLines)
	{
		LinesArr.Add(MakeShareable(new FJsonValueString(L)));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("lines"), LinesArr);
	Result->SetNumberField(TEXT("count"), LinesArr.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// PLAY IN EDITOR
// ============================================================

FString FNwiroEditorTools::PlayInEditor(const FString& JsonCommand)
{
	if (!GEditor) return TEXT("{\"success\":false,\"error\":\"No editor\"}");

	if (GEditor->IsPlaySessionInProgress())
		return TEXT("{\"success\":false,\"error\":\"PIE already running\"}");

	FRequestPlaySessionParams Params;
	// Default to PIE
	GEditor->RequestPlaySession(Params);

	return TEXT("{\"success\":true,\"message\":\"Play in Editor started\"}");
}

// ============================================================
// STOP PIE
// ============================================================

FString FNwiroEditorTools::StopPIE(const FString& JsonCommand)
{
	if (!GEditor) return TEXT("{\"success\":false,\"error\":\"No editor\"}");

	if (GEditor->IsPlaySessionRequestQueued())
	{
		GEditor->CancelRequestPlaySession();
		return TEXT("{\"success\":true,\"message\":\"Queued Play in Editor request cancelled\"}");
	}

	if (!GEditor->IsPlayingSessionInEditor() && !GEditor->PlayWorld)
		return TEXT("{\"success\":false,\"error\":\"PIE not running\"}");

	GEditor->RequestEndPlayMap();
	return TEXT("{\"success\":true,\"message\":\"Play in Editor stopped\"}");
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroScreenshotSpec.inl"
#endif
