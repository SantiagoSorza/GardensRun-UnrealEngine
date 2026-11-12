// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroImageDrop.h"
#include "NwiroBridge.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Input/DragAndDrop.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Drag positions are sent at most this often, and only when the cursor moved.
	constexpr double DragSendInterval = 1.0 / 30.0;
	constexpr double DragSendMinMove = 0.002;

	FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	/** The image files an external drag carries; empty for any other drag. */
	TArray<FString> DraggedImages(const FDragDropEvent& DragDropEvent)
	{
		const TSharedPtr<FExternalDragOperation> Operation = DragDropEvent.GetOperationAs<FExternalDragOperation>();
		if (!Operation.IsValid() || !Operation->HasFiles()) return {};
		return NwiroImageDrop::ImageFiles(Operation->GetFiles());
	}

	void Push(const TCHAR* Type, const FString& Json)
	{
		if (UNwiroBridge::Instance) UNwiroBridge::Instance->PushEvent(Type, Json);
	}

	TArray<FBox2D> GDropRegions;
}

TArray<FString> NwiroImageDrop::ImageFiles(const TArray<FString>& Files)
{
	TArray<FString> Images;
	for (const FString& File : Files)
	{
		const FString Ext = FPaths::GetExtension(File).ToLower();
		if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("bmp")) Images.Add(File);
	}
	return Images;
}

FVector2D NwiroImageDrop::Fraction(const FGeometry& Geometry, const FVector2D& ScreenPosition)
{
	const FVector2D Size = FVector2D(Geometry.GetLocalSize());
	if (Size.X <= 0.0 || Size.Y <= 0.0) return FVector2D::ZeroVector;
	const FVector2D Local = FVector2D(Geometry.AbsoluteToLocal(ScreenPosition));
	return FVector2D(FMath::Clamp(Local.X / Size.X, 0.0, 1.0), FMath::Clamp(Local.Y / Size.Y, 0.0, 1.0));
}

FString NwiroImageDrop::DragJson(const FVector2D& Position)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("x"), Position.X);
	Payload->SetNumberField(TEXT("y"), Position.Y);
	return ToJson(Payload);
}

FString NwiroImageDrop::DropJson(const TArray<FString>& ImagePaths, const FVector2D& Position, int64 MaxBytes)
{
	TArray<TSharedPtr<FJsonValue>> Files;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	const auto Skip = [&Skipped](const FString& Name, const TCHAR* Reason)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("reason"), Reason);
		Skipped.Add(MakeShared<FJsonValueObject>(Entry));
	};
	for (int32 Index = 0; Index < ImagePaths.Num(); ++Index)
	{
		const FString& Path = ImagePaths[Index];
		const FString Name = FPaths::GetCleanFilename(Path);
		if (Index > 0) { Skip(Name, TEXT("too_many")); continue; }
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size > MaxBytes) { Skip(Name, TEXT("too_large")); continue; }
		TArray<uint8> Bytes;
		if (Size <= 0 || !FFileHelper::LoadFileToArray(Bytes, *Path)) { Skip(Name, TEXT("unreadable")); continue; }
		// Same data URL as the bridge's image picker.
		const FString Ext = FPaths::GetExtension(Path).ToLower();
		const TCHAR* Mime = Ext == TEXT("png") ? TEXT("image/png")
			: (Ext == TEXT("jpg") || Ext == TEXT("jpeg")) ? TEXT("image/jpeg")
			: TEXT("image/bmp");
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("preview"), FString::Printf(TEXT("data:%s;base64,%s"), Mime, *FBase64::Encode(Bytes)));
		Files.Add(MakeShared<FJsonValueObject>(Entry));
	}
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("x"), Position.X);
	Payload->SetNumberField(TEXT("y"), Position.Y);
	Payload->SetArrayField(TEXT("files"), Files);
	Payload->SetArrayField(TEXT("skipped"), Skipped);
	return ToJson(Payload);
}

bool NwiroImageDrop::ParseRegions(const FString& Json, TArray<FBox2D>& OutRegions)
{
	OutRegions.Reset();
	TArray<TSharedPtr<FJsonValue>> Values;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Values)) return false;
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		double X0 = 0.0, Y0 = 0.0, X1 = 0.0, Y1 = 0.0;
		if (!Value.IsValid() || !Value->TryGetObject(Object)
			|| !(*Object)->TryGetNumberField(TEXT("x0"), X0) || !(*Object)->TryGetNumberField(TEXT("y0"), Y0)
			|| !(*Object)->TryGetNumberField(TEXT("x1"), X1) || !(*Object)->TryGetNumberField(TEXT("y1"), Y1))
		{
			OutRegions.Reset();
			return false;
		}
		if (X1 > X0 && Y1 > Y0) OutRegions.Add(FBox2D(FVector2D(X0, Y0), FVector2D(X1, Y1)));
	}
	return true;
}

void NwiroImageDrop::SetRegions(const TArray<FBox2D>& Regions)
{
	GDropRegions = Regions;
}

bool NwiroImageDrop::IsOverRegion(const FVector2D& Position)
{
	for (const FBox2D& Region : GDropRegions)
	{
		if (Region.IsInsideOrOn(Position)) return true;
	}
	return false;
}

FString NwiroImageDrop::SaveAttachment(const FString& Base64Data, const FString& FileName, const FString& Dir)
{
	FString Pure = Base64Data;
	int32 CommaIdx;
	if (Pure.FindChar(',', CommaIdx)) Pure = Pure.Mid(CommaIdx + 1);
	// Decode sizes the array before decoding, so malformed input leaves it filled: check the result.
	TArray<uint8> Decoded;
	if (!FBase64::Decode(Pure, Decoded) || Decoded.Num() == 0) return FString();

	// Folders go first, so a name like "..\x.png" stays in Dir; then characters no file name may hold.
	const FString Clean = FPaths::MakeValidFileName(FPaths::GetCleanFilename(FileName), TEXT('_'));
	const FString Ext = FPaths::GetExtension(Clean);
	FString Base = FPaths::GetBaseFilename(Clean);
	Base.TrimCharInline(TEXT('.'), nullptr);
	Base.LeftInline(64); // keeps a very long name within the path length limit
	if (Base.IsEmpty()) Base = TEXT("image");
	const FString Unique = Base + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FilePath = FPaths::Combine(Dir, Ext.IsEmpty() ? Unique : Unique + TEXT(".") + Ext);

	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	if (!FFileHelper::SaveArrayToFile(Decoded, *FilePath)) return FString();
	FString FullPath = FPaths::ConvertRelativePathToFull(FilePath);
	FPaths::MakePlatformFilename(FullPath);
	return FullPath;
}

void SNwiroImageDropTarget::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		InArgs._Content.Widget
	];
}

void SNwiroImageDropTarget::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	bImageDragActive = DraggedImages(DragDropEvent).Num() > 0;
	LastDragSentTime = 0.0;
	LastDragSent = FVector2D(-1.0, -1.0);
}

void SNwiroImageDropTarget::OnDragLeave(const FDragDropEvent& DragDropEvent)
{
	if (!bImageDragActive) return;
	bImageDragActive = false;
	Push(TEXT("image_drag_leave"), TEXT("{}"));
}

FReply SNwiroImageDropTarget::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (!bImageDragActive) return FReply::Unhandled();
	const FVector2D Position = NwiroImageDrop::Fraction(MyGeometry, FVector2D(DragDropEvent.GetScreenSpacePosition()));
	const double Now = FPlatformTime::Seconds();
	if (Now - LastDragSentTime >= DragSendInterval && FVector2D::Distance(Position, LastDragSent) >= DragSendMinMove)
	{
		LastDragSentTime = Now;
		LastDragSent = Position;
		Push(TEXT("image_drag"), NwiroImageDrop::DragJson(Position));
	}
	// The position is reported either way, so the page can light up its drop places; the
	// drag is accepted only over one of them.
	return NwiroImageDrop::IsOverRegion(Position) ? FReply::Handled() : FReply::Unhandled();
}

FReply SNwiroImageDropTarget::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const TArray<FString> Images = DraggedImages(DragDropEvent);
	bImageDragActive = false;
	if (Images.Num() == 0) return FReply::Unhandled();
	const FVector2D Position = NwiroImageDrop::Fraction(MyGeometry, FVector2D(DragDropEvent.GetScreenSpacePosition()));
	if (!NwiroImageDrop::IsOverRegion(Position))
	{
		Push(TEXT("image_drag_leave"), TEXT("{}"));
		return FReply::Unhandled();
	}
	Push(TEXT("image_dropped"), NwiroImageDrop::DropJson(Images, Position));
	return FReply::Handled();
}
