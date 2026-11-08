// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Image files dragged onto the panel from the operating system (Explorer, Finder).
 *
 * UE's web browser renders off-screen and never hands such drops to the page, but Slate
 * reports them to the widgets under the cursor as an FExternalDragOperation. The panel wraps
 * its browser in SNwiroImageDropTarget, which turns them into bridge events. The page
 * decides which drop place (if any) is under the reported position:
 *   image_drag       {x, y}                     while dragging, throttled
 *   image_drag_leave {}                         the drag left the panel
 *   image_dropped    {x, y, files, skipped}     files: [{name, path, preview}], skipped: [{name, reason}]
 * Only the first image of a drop is read; the others are reported as skipped (`too_many`), so a
 * drop of many large files cannot stall the editor. Every drop place takes a single image.
 * x and y are fractions of the panel's size, so the page can map them to its own
 * coordinates whatever the display scaling. Other drags are left unhandled, as before.
 *
 * The page also reports where its drop places are (the bridge's SetImageDropRegions), and
 * the widget accepts the drag only over one of them. The operating system then shows the copy
 * cursor there and the "not allowed" cursor elsewhere, and drops nowhere else.
 */
namespace NwiroImageDrop
{
	/** Largest file read into a drop event; bigger ones are reported under `skipped`. */
	constexpr int64 MaxFileBytes = 50 * 1024 * 1024;

	/** The image files among Files (png, jpg, jpeg, bmp, as the image picker), in order. */
	TArray<FString> ImageFiles(const TArray<FString>& Files);

	/** ScreenPosition inside Geometry as fractions of its size, each clamped to [0, 1]. */
	FVector2D Fraction(const FGeometry& Geometry, const FVector2D& ScreenPosition);

	/** Payload of an `image_drag` event. */
	FString DragJson(const FVector2D& Position);

	/** Payload of an `image_dropped` event: the first image as a data URL if it is readable and
	 *  at most MaxBytes, otherwise under `skipped` (`too_large`, `unreadable`). Every later image
	 *  is skipped as `too_many` without being read. */
	FString DropJson(const TArray<FString>& ImagePaths, const FVector2D& Position, int64 MaxBytes = MaxFileBytes);

	/** Reads the page's drop places, `[{"x0":..,"y0":..,"x1":..,"y1":..}, ...]` in fractions of the
	 *  panel. False, with OutRegions empty, when the JSON is not such a list. */
	bool ParseRegions(const FString& Json, TArray<FBox2D>& OutRegions);

	/** The drop places the page reported last; empty until it reports any. Game thread only. */
	void SetRegions(const TArray<FBox2D>& Regions);
	bool IsOverRegion(const FVector2D& Position);

	/** Saves an image attachment the page sends (base64, or a data URL) in Dir, for the bridge's
	 *  SaveImageBase64. The name is FileName without any folder part, made a valid file name, with
	 *  a unique part before its extension, so two attachments with the same name never overwrite
	 *  each other and none lands outside Dir. The extension is kept: the prompt's image type is
	 *  read from it. Returns the full path, or empty when the data does not decode or the file
	 *  cannot be written. */
	FString SaveAttachment(const FString& Base64Data, const FString& FileName, const FString& Dir);
}

class SNwiroImageDropTarget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SNwiroImageDropTarget) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual void OnDragLeave(const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
	/** A drag of at least one image file is over the panel. */
	bool bImageDragActive = false;
	double LastDragSentTime = 0.0;
	FVector2D LastDragSent = FVector2D(-1.0, -1.0);
};
