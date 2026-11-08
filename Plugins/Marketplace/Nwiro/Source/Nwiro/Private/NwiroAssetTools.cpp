// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroAssetTools.h"
#include "NwiroAssetGuard.h"
#include "NwiroTypeValidation.h"
#include "Editor.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Kismet2/StructureEditorUtils.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/FeedbackContext.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif
#include "Misc/PackagePath.h"
#include "Engine/Blueprint.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroAsset, Log, All);

// ============================================================
// HELPERS
// ============================================================

static void SerializePropertyValue(UObject* Obj, FProperty* Prop, TSharedPtr<FJsonObject>& OutObj, int32 Depth)
{
	if (!Prop || !Obj) return;

	FString PropName = Prop->GetName();
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

	// Skip deprecated/editor-only metadata
	if (Prop->HasMetaData(TEXT("DeprecatedProperty"))) return;

	// Bool
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		OutObj->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
		return;
	}

	// Numeric (int/float/double)
	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
	{
		if (NumProp->IsFloatingPoint())
			OutObj->SetNumberField(PropName, NumProp->GetFloatingPointPropertyValue(ValuePtr));
		else if (NumProp->IsInteger())
			OutObj->SetNumberField(PropName, (double)NumProp->GetSignedIntPropertyValue(ValuePtr));
		else
		{
			// Enum underlying
			FString Val;
			NumProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
			OutObj->SetStringField(PropName, Val);
		}
		return;
	}

	// String types
	if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		OutObj->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
		return;
	}
	if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		OutObj->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
		return;
	}
	if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		OutObj->SetStringField(PropName, TextProp->GetPropertyValue(ValuePtr).ToString());
		return;
	}

	// Enum
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		FString Val;
		EnumProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
		OutObj->SetStringField(PropName, Val);
		return;
	}
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			FString Val = ByteProp->Enum->GetNameStringByValue(ByteProp->GetPropertyValue(ValuePtr));
			OutObj->SetStringField(PropName, Val);
		}
		else
		{
			OutObj->SetNumberField(PropName, (double)ByteProp->GetPropertyValue(ValuePtr));
		}
		return;
	}

	// Object reference
	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* RefObj = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (RefObj)
		{
			if (Depth > 0)
			{
				TSharedPtr<FJsonObject> SubObj = MakeShareable(new FJsonObject());
				SubObj->SetStringField(TEXT("class"), RefObj->GetClass()->GetName());
				SubObj->SetStringField(TEXT("path"), RefObj->GetPathName());
				OutObj->SetObjectField(PropName, SubObj);
			}
			else
			{
				OutObj->SetStringField(PropName, RefObj->GetPathName());
			}
		}
		else
		{
			OutObj->SetStringField(PropName, TEXT("None"));
		}
		return;
	}

	// Struct
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (Depth > 0)
		{
			TSharedPtr<FJsonObject> StructObj = MakeShareable(new FJsonObject());
			for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
			{
				FProperty* InnerProp = *It;
				void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
				// Use export text for struct members
				FString Val;
				InnerProp->ExportTextItem_Direct(Val, InnerPtr, nullptr, nullptr, PPF_None);
				StructObj->SetStringField(InnerProp->GetName(), Val);
			}
			OutObj->SetObjectField(PropName, StructObj);
		}
		else
		{
			FString Val;
			StructProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
			OutObj->SetStringField(PropName, Val);
		}
		return;
	}

	// Array
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Arr;
		int32 Count = FMath::Min(Helper.Num(), 50); // Cap at 50 elements
		for (int32 i = 0; i < Count; i++)
		{
			FString Val;
			ArrayProp->Inner->ExportTextItem_Direct(Val, Helper.GetRawPtr(i), nullptr, Obj, PPF_None);
			Arr.Add(MakeShareable(new FJsonValueString(Val)));
		}
		OutObj->SetArrayField(PropName, Arr);
		if (Helper.Num() > 50)
		{
			OutObj->SetNumberField(PropName + TEXT("_totalCount"), Helper.Num());
		}
		return;
	}

	// Fallback: export as text
	FString Val;
	Prop->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
	if (!Val.IsEmpty())
	{
		OutObj->SetStringField(PropName, Val);
	}
}

// ============================================================
// READ ASSET
// ============================================================

FString FNwiroAssetTools::ReadAsset(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("assetPath"), TEXT("asset_path"), TEXT("name") }) {
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	}
	int32 Depth = Cmd->HasField(TEXT("depth")) ? (int32)Cmd->GetNumberField(TEXT("depth")) : 1;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"path required (accepted: path / assetPath / name)\"}");

	FString LoadError;
	UObject* Asset = NwiroSafeAssetLoad(Path, &LoadError);
	if (!Asset) return NwiroAssetLoadFailure(Path, LoadError);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("path"), Asset->GetPathName());
	Result->SetStringField(TEXT("package"), Asset->GetPackage()->GetName());

	// Serialize all properties
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject());
	for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		// Skip internal/transient properties
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
		SerializePropertyValue(Asset, Prop, Properties, Depth);
	}
	Result->SetObjectField(TEXT("properties"), Properties);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND ASSETS
// ============================================================

FString FNwiroAssetTools::FindAssets(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SearchTerm, ClassFilter, PathFilter;
	if ((Cmd->HasField(TEXT("searchTerm")) && (!Cmd->HasTypedField<EJson::String>(TEXT("searchTerm")) || !Cmd->TryGetStringField(TEXT("searchTerm"), SearchTerm))) ||
		(Cmd->HasField(TEXT("classFilter")) && (!Cmd->HasTypedField<EJson::String>(TEXT("classFilter")) || !Cmd->TryGetStringField(TEXT("classFilter"), ClassFilter))) ||
		(Cmd->HasField(TEXT("path")) && (!Cmd->HasTypedField<EJson::String>(TEXT("path")) || !Cmd->TryGetStringField(TEXT("path"), PathFilter))))
		return TEXT("{\"success\":false,\"error\":\"searchTerm, classFilter and path must be strings when supplied\"}");
	int32 MaxResults = 50;
	if (Cmd->HasField(TEXT("maxResults")))
	{
		double Requested = 0;
		if (!Cmd->HasTypedField<EJson::Number>(TEXT("maxResults")) || !Cmd->TryGetNumberField(TEXT("maxResults"), Requested) || !FMath::IsFinite(Requested) ||
			Requested < 1 || Requested > 1000 || Requested != FMath::FloorToDouble(Requested))
			return TEXT("{\"success\":false,\"error\":\"maxResults must be an integer from 1 to 1000\"}");
		MaxResults = static_cast<int32>(Requested);
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.PackagePaths.Add(PathFilter.IsEmpty() ? TEXT("/Game") : FName(*PathFilter));
	Filter.bRecursivePaths = true;

	// An unresolved requested filter is an error, never an unfiltered search.
	if (!ClassFilter.IsEmpty())
	{
		FString Error;
		UClass* FilterClass = Cast<UClass>(NwiroTypeValidation::ResolveLoadedType(ClassFilter, UClass::StaticClass(), Error));
		if (!FilterClass)
		{
			TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
			Failure->SetBoolField(TEXT("success"), false);
			Failure->SetStringField(TEXT("error"), TEXT("Invalid classFilter: ") + Error);
			FString Out; FJsonSerializer::Serialize(Failure, TJsonWriterFactory<>::Create(&Out));
			return Out;
		}
		Filter.ClassPaths.Add(FilterClass->GetClassPathName());
		Filter.bRecursiveClasses = true;
	}

	TArray<FAssetData> Assets;
	if (!ARM.Get().GetAssets(Filter, Assets))
		return TEXT("{\"success\":false,\"error\":\"Asset Registry search failed; no complete result is available\"}");

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& A : Assets)
	{
		FString Name = A.AssetName.ToString();
		if (!SearchTerm.IsEmpty() && !Name.Contains(SearchTerm, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *A.PackageName.ToString(), *Name));
		Obj->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));

		if (Results.Num() >= MaxResults) break;
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("assets"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// GET ASSET THUMBNAIL
// ============================================================

FString FNwiroAssetTools::GetAssetThumbnail(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("assetPath"), TEXT("asset_path"), TEXT("name") }) {
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	}
	int32 Width = 256, Height = 256;
	for (const TCHAR* Key : {TEXT("width"), TEXT("height")})
	{
		if (!Cmd->HasField(Key)) continue;
		double Value = 0;
		if (!Cmd->HasTypedField<EJson::Number>(Key) || !Cmd->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) ||
			Value < 1 || Value > 4096 || Value != FMath::FloorToDouble(Value))
			return TEXT("{\"success\":false,\"error\":\"Thumbnail width and height must be integers from 1 to 4096\"}");
		(FCString::Strcmp(Key, TEXT("width")) == 0 ? Width : Height) = static_cast<int32>(Value);
	}

	FString LoadError;
	UObject* Asset = NwiroSafeAssetLoad(Path, &LoadError);
	if (!Asset) return NwiroAssetLoadFailure(Path, LoadError);

	// CRITICAL crash guard: live thumbnail render instantiates a Blueprint's
	// generated-class CDO; a half-compiled class (null ClassConstructor) asserts
	// UObjectGlobals.cpp:3396 and crashes the editor. Skip live render for
	// unsafe BP classes and fall back to the cached thumbnail.
	bool bSkipLiveRender = false;
	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		UClass* GC = BP->GeneratedClass;
		bSkipLiveRender = !(GC && GC->IsValidLowLevel()
			&& GC->ClassConstructor != nullptr
			&& GC->ClassWithin != nullptr
			&& !GC->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists));
	}
	FObjectThumbnail* Thumbnail = bSkipLiveRender
		? nullptr
		: ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Asset);
	const TCHAR* ThumbnailSource = TEXT("generated");

	// The cached thumbnail points into this map; keep its owner alive through encoding.
	FThumbnailMap LoadedThumbnails;
	if (!Thumbnail || Thumbnail->GetImageWidth() == 0)
	{

		const FName FullName = *FString::Printf(TEXT("%s %s"),
			*Asset->GetClass()->GetName(), *Asset->GetPathName());
		TArray<FName> Names; Names.Add(FullName);
		ThumbnailTools::ConditionallyLoadThumbnailsForObjects(Names, LoadedThumbnails);
		if (FObjectThumbnail* Cached = LoadedThumbnails.Find(FullName))
		{
			if (Cached->GetImageWidth() > 0)
			{
				Thumbnail = Cached;
				ThumbnailSource = TEXT("package-cache");
			}
		}
	}

	if (!Thumbnail || Thumbnail->GetImageWidth() == 0)
		return TEXT("{\"success\":false,\"error\":\"Failed to generate thumbnail (asset has no cached thumbnail and live render returned empty — common for newly-created BPs that were never opened in the editor)\"}");

	const int32 SourceWidth = Thumbnail->GetImageWidth();
	const int32 SourceHeight = Thumbnail->GetImageHeight();
	const TArray<uint8>& ImgData = Thumbnail->GetUncompressedImageData();
	const int64 ExpectedBytes = static_cast<int64>(SourceWidth) * SourceHeight * sizeof(FColor);
	if (SourceWidth <= 0 || SourceHeight <= 0 || ExpectedBytes != ImgData.Num() || ImgData.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Thumbnail pixel data does not match its dimensions\"}");

	TArray<FColor> SourcePixels;
	SourcePixels.SetNumUninitialized(SourceWidth * SourceHeight);
	FMemory::Memcpy(SourcePixels.GetData(), ImgData.GetData(), ImgData.Num());
	TArray<FColor> Pixels;
	if (SourceWidth == Width && SourceHeight == Height) Pixels = MoveTemp(SourcePixels);
	else FImageUtils::ImageResize(SourceWidth, SourceHeight, SourcePixels, Width, Height, Pixels,
		/*bResizeSRGBinLinearSpace=*/true, /*bForceOpaqueOutput=*/false);
	// Live texture renders read back with alpha 0; a transparent PNG shows the agent a blank image.
	for (FColor& Pixel : Pixels) Pixel.A = 255;

	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> PngWrapper = ImageModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid() || !PngWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		return TEXT("{\"success\":false,\"error\":\"Failed to encode thumbnail\"}");
	const TArray64<uint8> PngData = PngWrapper->GetCompressed();

	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NwiroThumbnails"));
	const FString FileName = Asset->GetName() + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("_thumb.png");
	FString SavePath = Directory / FileName;
	if (PngData.IsEmpty() || !IFileManager::Get().MakeDirectory(*Directory, true) ||
		!FFileHelper::SaveArrayToFile(PngData, *SavePath) || IFileManager::Get().FileSize(*SavePath) != PngData.Num())
		return TEXT("{\"success\":false,\"error\":\"Failed to write thumbnail PNG to the project Saved directory\"}");
	FPaths::NormalizeFilename(SavePath);
	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"source\":\"%s\"}"),
		*SavePath.ReplaceCharWithEscapedChar(), Width, Height, ThumbnailSource);
}

// ============================================================
// SAVE ASSET
// ============================================================

FString FNwiroAssetTools::SaveAsset(const FString& JsonCommand)
{
	FString AssetPath;
	FString Filename;
	UPackage* Package = nullptr;
	bool bStateObserved = false;
	bool bDirtyBefore = false;
	bool bWriteAttempted = false;
	bool bSavedToDisk = false;
	const auto Response = [&](bool bSuccess, const TCHAR* Status, const FString& Error = FString())
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetStringField(TEXT("status"), Status);
		if (!AssetPath.IsEmpty()) Result->SetStringField(TEXT("assetPath"), AssetPath);
		Result->SetBoolField(TEXT("writeAttempted"), bWriteAttempted);
		Result->SetBoolField(TEXT("savedToDisk"), bSavedToDisk);
		Result->SetBoolField(TEXT("targetStateObserved"), bStateObserved);
		if (bStateObserved)
		{
			const bool bDirtyAfter = Package->IsDirty();
			Result->SetStringField(TEXT("package"), Package->GetName());
			Result->SetBoolField(TEXT("existsOnDisk"), IFileManager::Get().FileExists(*Filename));
			Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
			Result->SetBoolField(TEXT("dirtyAfter"), bDirtyAfter);
			TArray<TSharedPtr<FJsonValue>> Dirty;
			if (bDirtyAfter) Dirty.Add(MakeShared<FJsonValueString>(Package->GetName()));
			Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
		}
		else
		{
			for (const TCHAR* Key : { TEXT("package"), TEXT("existsOnDisk"), TEXT("dirtyBefore"), TEXT("dirtyAfter"), TEXT("dirtyPackages") })
				Result->SetField(Key, MakeShared<FJsonValueNull>());
		}
		if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
		FString Out;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out));
		return Out;
	};
	const auto Reject = [&](const FString& Error) { return Response(false, TEXT("rejected"), Error); };

	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		return Reject(TEXT("Invalid JSON"));
	if (!IsInGameThread()) return Reject(TEXT("save_asset must run on the editor game thread"));
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	const bool bPackageSaveInProgress = GIsSavingPackage;
#else
	const bool bPackageSaveInProgress = UE::IsSavingPackage();
#endif
	if (!GIsEditor || !GEditor || GEditor->PlayWorld || bPackageSaveInProgress || IsGarbageCollecting())
		return Reject(TEXT("save_asset requires an idle editor outside PIE, garbage collection and package saving"));
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Cmd->Values)
	{
		if (Field.Key != TEXT("assetPath")) return Reject(TEXT("save_asset accepts only assetPath"));
	}
	if (!Cmd->HasTypedField<EJson::String>(TEXT("assetPath")))
		return Reject(TEXT("assetPath must be an exact loaded /Game/Folder/Asset.Asset object path"));
	const FString RequestedPath = Cmd->GetStringField(TEXT("assetPath"));
	if (RequestedPath.IsEmpty() || RequestedPath.Len() >= NAME_SIZE)
		return Reject(TEXT("assetPath must be nonempty and shorter than NAME_SIZE characters"));
	AssetPath = RequestedPath;
	for (TCHAR Character : AssetPath)
	{
		if (FChar::IsControl(Character)) return Reject(TEXT("assetPath must not contain control characters"));
	}
	FString PackageName;
	FString ObjectName;
	FText PathError;
	if (!AssetPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !AssetPath.Split(TEXT("."), &PackageName, &ObjectName)
		|| ObjectName.IsEmpty() || ObjectName.Contains(TEXT(".")) || ObjectName.Contains(TEXT(":"))
		|| !FPackageName::IsValidObjectPath(AssetPath, &PathError)
		|| !FPackageName::IsValidLongPackageName(PackageName, false, &PathError))
		return Reject(TEXT("assetPath must be an exact /Game/Folder/Asset.Asset path without aliases, subobjects or traversal"));
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
		return Reject(TEXT("assetPath package is not mounted to a project content file"));
	Filename = FPaths::ConvertRelativePathToFull(Filename);
	FString ContentDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FPaths::NormalizeDirectoryName(ContentDirectory);
	if (!FPaths::IsUnderDirectory(Filename, ContentDirectory) || !FPaths::ValidatePath(Filename, &PathError))
		return Reject(TEXT("assetPath must map to a valid file beneath the project Content directory"));
#if PLATFORM_WINDOWS
	// Lexical containment does not detect junctions. Check the destination and its Content ancestors.
	for (FString Candidate = Filename; ; Candidate = FPaths::GetPath(Candidate))
	{
		const uint32 Attributes = ::GetFileAttributesW(*Candidate);
		if (Attributes == INVALID_FILE_ATTRIBUTES)
		{
			const uint32 AttributeError = ::GetLastError();
			// A fresh destination may be absent; every other failure leaves its physical state unknown.
			if (AttributeError != ERROR_FILE_NOT_FOUND && AttributeError != ERROR_PATH_NOT_FOUND)
				return Reject(FString::Printf(TEXT("save_asset cannot verify physical destination attributes (Windows error %u)"), AttributeError));
		}
		else if (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)
			return Reject(TEXT("save_asset does not support symbolic links or reparse points in the Content destination"));
		if (FPaths::IsSamePath(Candidate, ContentDirectory)) break;
		const FString Parent = FPaths::GetPath(Candidate);
		if (Parent.IsEmpty() || Parent == Candidate)
			return Reject(TEXT("physical destination check did not reach the project Content directory"));
	}

	// Resolve only the exact loaded object. Saving must not load a missing or damaged asset.
	TStrongObjectPtr<UObject> Asset(FindObject<UObject>(nullptr, *AssetPath));
	if (!IsValid(Asset.Get()) || !Asset->GetPathName().Equals(AssetPath, ESearchCase::CaseSensitive))
		return Reject(TEXT("assetPath is not an exact loaded object; load the intended asset separately first"));
	Package = Asset->GetOutermost();
	if (!IsValid(Package) || !Package->GetName().Equals(PackageName, ESearchCase::CaseSensitive))
		return Reject(TEXT("assetPath does not identify the object's exact owning package"));
	bDirtyBefore = Package->IsDirty();
	bStateObserved = true;

	UBlueprint* Blueprint = Cast<UBlueprint>(Asset.Get());
	UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Asset.Get());
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Asset.Get());
	if (!Blueprint && !Struct && !Enum)
		return Reject(TEXT("save_asset supports Blueprint, UserDefinedStruct and UserDefinedEnum assets only"));
	if (Asset->GetOuter() != Package || Asset->GetPackage() != Package || Asset->IsPackageExternal()
		|| Asset->HasAnyFlags(RF_Transient | RF_ClassDefaultObject | RF_ArchetypeObject | RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects)
		|| !Asset->HasAllFlags(RF_Public | RF_Standalone)
		|| Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient)
		|| Package->HasAnyPackageFlags(PKG_CompiledIn | PKG_PlayInEditor | PKG_ContainsMap | PKG_Cooked))
		return Reject(TEXT("asset must be a persistent, fully initialized primary project asset in its own package"));
	if (!Package->IsFullyLoaded())
		return Reject(TEXT("asset package is not fully loaded; complete loading separately before saving"));
	if (Package->GetExternalPackages().Num() != 0)
		return Reject(TEXT("packages owning external packages are not supported by save_asset"));

	FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	const FSoftObjectPath ExactObjectPath(Asset.Get());
	// This query includes valid in-memory asset metadata; it does not prove a prior AssetCreated broadcast.
	const FAssetData AssetData = RegistryModule.Get().GetAssetByObjectPath(ExactObjectPath);
	if (!ObjectTools::IsObjectBrowsable(Asset.Get()) || !AssetData.IsValid()
		|| AssetData.GetSoftObjectPath() != ExactObjectPath || AssetData.AssetClassPath != Asset->GetClass()->GetClassPathName())
		return Reject(TEXT("asset must be browsable with valid exact in-memory asset-registry identity before saving"));

	bool bAdditionalPrimary = false;
	bool bUnreadyObject = false;
	bool bExternalObject = false;
	ForEachObjectWithOuter(Package, [&](UObject* Object)
	{
		if (!IsValid(Object)) return;
		bUnreadyObject |= Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects);
		bExternalObject |= Object->IsPackageExternal() || Object->GetPackage() != Package;
		const bool bBlueprintCompanion = Blueprint && (Object == Blueprint->GeneratedClass || Object == Blueprint->SkeletonGeneratedClass);
		if (Object != Asset.Get() && !bBlueprintCompanion && Object->IsAsset()) bAdditionalPrimary = true;
	}, true);
	if (bUnreadyObject) return Reject(TEXT("asset package contains objects whose loading is not complete"));
	if (bExternalObject) return Reject(TEXT("asset package contains externally packaged objects"));
	if (bAdditionalPrimary) return Reject(TEXT("asset package contains another primary asset; save_asset saves one primary asset's package only"));

	if (Blueprint)
	{
		const auto ReadyClass = [Blueprint](UClass* Class)
		{
			return IsValid(Class) && Class->ClassGeneratedBy == Blueprint && Class->GetOutermost() == Blueprint->GetOutermost()
				&& Class->ClassConstructor && Class->ClassWithin
				&& !Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)
				&& IsValid(Class->GetDefaultObject(false));
		};
		// UBlueprint::PreSave may compile stale UDS references unless Status is exactly UpToDate.
		if (Blueprint->Status != BS_UpToDate || Blueprint->bBeingCompiled || Blueprint->bIsRegeneratingOnLoad
			|| !ReadyClass(Blueprint->GeneratedClass) || !ReadyClass(Blueprint->SkeletonGeneratedClass))
			return Reject(TEXT("Blueprint must be fully compiled and up to date, with valid generated and skeleton classes/CDOs; compile it separately first"));
	}
	if (Struct)
	{
		FString StructureError;
		if (Struct->Status != UDSS_UpToDate
			|| FStructureEditorUtils::IsStructureValid(Struct, nullptr, &StructureError) != FStructureEditorUtils::EStructureError::Ok)
			return Reject(TEXT("UserDefinedStruct must be valid and up to date before saving: ") + StructureError);
	}

	// Existing bytes alone do not establish that this loaded package was ever saved to that file.
	const FPackagePath& LoadedPath = Package->GetLoadedPath();
	const bool bKnownSavedDestination = !Package->HasAnyPackageFlags(PKG_NewlyCreated) && !LoadedPath.IsEmpty()
		&& FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(LoadedPath.GetLocalFullPath()), Filename);
	if (!bDirtyBefore && bKnownSavedDestination && IFileManager::Get().FileSize(*Filename) > 0)
		return Response(true, TEXT("no_op"));

	// Save exactly this package synchronously. EditorAssetLibrary also fully loads and checks out packages.
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.Error = GWarn;
	SaveArgs.bSlowTask = false;
	bWriteAttempted = true;
	const bool bSaveSucceeded = UPackage::SavePackage(Package, Asset.Get(), *Filename, SaveArgs);
	bSavedToDisk = bSaveSucceeded && IFileManager::Get().FileSize(*Filename) > 0;
	if (!bSavedToDisk)
		return Response(false, TEXT("failed"), TEXT("package save did not complete with a nonempty asset file; inspect the observed disk and dirty state before retrying"));
	if (Package->IsDirty())
		return Response(false, TEXT("failed"), TEXT("package was saved, but is dirty again after save callbacks; savedToDisk describes the completed write"));
	return Response(true, TEXT("saved"));
#else
	// The installed Apple/Unix IsSymlink implementations follow links with stat(), so do not trust them here.
	return Reject(TEXT("save_asset physical destination verification is currently supported on Windows only"));
#endif
}
