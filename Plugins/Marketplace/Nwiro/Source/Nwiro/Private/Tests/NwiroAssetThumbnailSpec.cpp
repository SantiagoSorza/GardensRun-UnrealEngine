// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroAssetTools.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Json.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace NwiroAssetThumbnailProof
{
using FTool = FString (*)(const FString&);
static TSharedPtr<FJsonObject> Call(FAutomationTestBase& Test, FTool Tool, const TSharedRef<FJsonObject>& Request)
{
 FString Input; FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Input));
 const FString Output = Tool(Input);
 TSharedPtr<FJsonObject> Result;
 Test.TestTrue(TEXT("tool returned JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Output), Result) && Result.IsValid());
 return Result;
}
static bool Success(const TSharedPtr<FJsonObject>& Result)
{
 bool Value = false; return Result.IsValid() && Result->TryGetBoolField(TEXT("success"), Value) && Value;
}
static TSharedRef<FJsonObject> Request(const FString& Path)
{
 TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("path"), Path); return Result;
}
static TArray<FString> Outputs(const FString& Directory, const FString& Name)
{
 TArray<FString> Files;
 IFileManager::Get().FindFiles(Files, *(Directory / (Name + TEXT("_*_thumb.png"))), true, false);
 Files.Sort(); return Files;
}
static bool VerifyPNG(FAutomationTestBase& Test, const FString& Path, int32 Width, int32 Height)
{
 TArray<uint8> Encoded;
 if (!Test.TestTrue(TEXT("PNG file readable"), FFileHelper::LoadFileToArray(Encoded, *Path))) return false;
 auto& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
 const auto Decoder = Module.CreateImageWrapper(EImageFormat::PNG);
 if (!Test.TestTrue(TEXT("actual file decodes as PNG"), Decoder.IsValid() && Decoder->SetCompressed(Encoded.GetData(), Encoded.Num()))) return false;
 Test.TestEqual(TEXT("decoded width"), Decoder->GetWidth(), static_cast<int64>(Width));
 Test.TestEqual(TEXT("decoded height"), Decoder->GetHeight(), static_cast<int64>(Height));
 if (!Test.TestTrue(TEXT("PNG contains 8-bit color"), Decoder->GetBitDepth() == 8 && (Decoder->GetFormat() == ERGBFormat::RGBA || Decoder->GetFormat() == ERGBFormat::BGRA))) return false;
 TArray64<uint8> Pixels;
 if (!Test.TestTrue(TEXT("actual PNG pixels decoded"), Decoder->GetRaw(Pixels))) return false;
 if (!Test.TestEqual(TEXT("decoded byte count"), Pixels.Num(), static_cast<int64>(Width) * Height * 4)) return false;
 bool bOpaque = true;
 for (int64 Offset = 3; Offset < Pixels.Num(); Offset += 4) bOpaque &= Pixels[Offset] == 255;
 Test.TestTrue(TEXT("actual PNG is opaque"), bOpaque);
 bool bNonuniform = false;
 for (int64 Offset = 4; Offset < Pixels.Num(); Offset += 4)
  if (FMemory::Memcmp(Pixels.GetData(), Pixels.GetData() + Offset, 4) != 0) { bNonuniform = true; break; }
 return Test.TestTrue(TEXT("actual PNG preserves nonuniform content"), bNonuniform);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroAssetThumbnailContract, "Nwiro.Pro.Assets.Thumbnail.CachedDimensionsAndFilters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroAssetThumbnailContract::RunTest(const FString& Parameters)
{
 using namespace NwiroAssetThumbnailProof;
 const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString Folder = TEXT("/Game/__NwiroThumbnailProof_") + Id;
 const FString Name = TEXT("BP_Macro_") + Id;
 const FString PackageName = Folder / Name;
 const FString Filename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
 if (!TestTrue(TEXT("fixture inside Content"), FPaths::IsUnderDirectory(Filename, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()))) ||
     !TestFalse(TEXT("never overwrite existing package"), IFileManager::Get().FileExists(*Filename))) return false;
 TStrongObjectPtr<UPackage> Package(CreatePackage(*PackageName));
 TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), Package.Get(), FName(*Name), BPTYPE_MacroLibrary,
  UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("NwiroThumbnailProof")));
 if (!TestNotNull(TEXT("normal Macro Library created"), BP.Get())) return false;
 FKismetEditorUtilities::CompileBlueprint(BP.Get(), EBlueprintCompileOptions::SkipSave);
 FAssetRegistryModule::AssetCreated(BP.Get());
 if (!TestNotNull(TEXT("fixture generated class"), BP->GeneratedClass.Get()) ||
     !TestTrue(TEXT("legitimate compiler-created abstract Macro Library"), BP->BlueprintType == BPTYPE_MacroLibrary && BP->GeneratedClass->HasAnyClassFlags(CLASS_Abstract)) ||
     !TestTrue(TEXT("class initialized"), BP->GeneratedClass->ClassConstructor != nullptr && BP->GeneratedClass->ClassWithin != nullptr)) return false;
 // The compiler makes Macro Library classes abstract. Never alter class/CDO flags to force this branch.
 TArray<FColor> Colors;
 for (int32 Y = 0; Y < 4; ++Y)
  for (int32 X = 0; X < 4; ++X)
   Colors.Add(Y < 2 ? (X < 2 ? FColor::Red : FColor::Green) : (X < 2 ? FColor::Blue : FColor::Yellow));
 // Alpha 0 with correct RGB, as a live texture render reads back; the tool must still write an opaque PNG.
 for (FColor& Color : Colors) Color.A = 0;
 FObjectThumbnail Pattern;
 Pattern.SetImageSize(4, 4);
 Pattern.AccessImageData().SetNumUninitialized(Colors.Num() * sizeof(FColor));
 FMemory::Memcpy(Pattern.AccessImageData().GetData(), Colors.GetData(), Pattern.AccessImageData().Num());
 Pattern.SetCreatedAfterCustomThumbsEnabled();
 const FString FullName = BP->GetClass()->GetName() + TEXT(" ") + BP->GetPathName();
 if (!TestNotNull(TEXT("owned custom thumbnail cached"), ThumbnailTools::CacheThumbnail(FullName, &Pattern, Package.Get()))) return false;
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
 FSavePackageArgs SaveArgs;
 SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
 SaveArgs.Error = GWarn; SaveArgs.bSlowTask = false;
 if (!TestTrue(TEXT("fixture explicitly saved"), UPackage::SavePackage(Package.Get(), BP.Get(), *Filename, SaveArgs))) return false;
 TestFalse(TEXT("explicitly saved fixture clean"), Package->IsDirty());
 FThumbnailMap DiskThumbnails;
 TSet<FName> Names; Names.Add(FName(*FullName));
 if (!TestTrue(TEXT("independent disk thumbnail read"), ThumbnailTools::LoadThumbnailsFromPackage(Filename, Names, DiskThumbnails))) return false;
 FObjectThumbnail* DiskThumbnail = DiskThumbnails.Find(FName(*FullName));
 if (!TestNotNull(TEXT("saved custom thumbnail exists"), DiskThumbnail)) return false;
 TestEqual(TEXT("saved width"), DiskThumbnail->GetImageWidth(), 4);
 TestEqual(TEXT("saved height"), DiskThumbnail->GetImageHeight(), 4);
 if (!TestTrue(TEXT("saved exact deterministic pixels"), DiskThumbnail->GetUncompressedImageData() == Pattern.GetUncompressedImageData())) return false;
 if (!TestTrue(TEXT("owned package has thumbnail map"), Package->HasThumbnailMap())) return false;
 Package->AccessThumbnailMap().Empty();
 TestEqual(TEXT("clear only owned cache to force disk path"), Package->GetThumbnailMap().Num(), 0);
 TArray<uint8> PackageBefore;
 if (!TestTrue(TEXT("saved package bytes captured"), FFileHelper::LoadFileToArray(PackageBefore, *Filename))) return false;
 UClass* ClassBefore = BP->GeneratedClass;
 UObject* CDOBefore = ClassBefore->GetDefaultObject(false);
 const EBlueprintStatus StatusBefore = BP->Status;
 const bool bDirtyBefore = Package->IsDirty();
 const FString OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NwiroThumbnails"));
 TArray<FString> ProducedPaths;
 TArray<TArray<uint8>> ProducedBytes;
 for (const FIntPoint Size : {FIntPoint(8, 4), FIntPoint(4, 8), FIntPoint(8, 4), FIntPoint(4, 4)})
 {
  const auto Args = Request(BP->GetPathName()); Args->SetNumberField(TEXT("width"), Size.X); Args->SetNumberField(TEXT("height"), Size.Y);
  const auto Result = Call(*this, &FNwiroAssetTools::GetAssetThumbnail, Args);
  if (!TestTrue(TEXT("production thumbnail succeeds"), Success(Result))) return false;
  TestEqual(TEXT("actual source package-cache"), Result->GetStringField(TEXT("source")), FString(TEXT("package-cache")));
  TestEqual(TEXT("reported width"), Result->GetIntegerField(TEXT("width")), Size.X);
  TestEqual(TEXT("reported height"), Result->GetIntegerField(TEXT("height")), Size.Y);
  const FString Path = Result->GetStringField(TEXT("path"));
  if (!TestTrue(TEXT("output inside thumbnail directory"), FPaths::IsUnderDirectory(Path, OutputDirectory))) return false;
  TestFalse(TEXT("unique output names across repeated calls"), ProducedPaths.Contains(Path));
  VerifyPNG(*this, Path, Size.X, Size.Y);
  for (int32 Index = 0; Index < ProducedPaths.Num(); ++Index)
  {
   TArray<uint8> Previous; TestTrue(TEXT("previous PNG readable"), FFileHelper::LoadFileToArray(Previous, *ProducedPaths[Index]));
   TestTrue(TEXT("previous PNG bytes never overwritten"), Previous == ProducedBytes[Index]);
  }
  TArray<uint8> Current;
  if (!TestTrue(TEXT("current PNG captured"), FFileHelper::LoadFileToArray(Current, *Path))) return false;
  ProducedPaths.Add(Path); ProducedBytes.Add(MoveTemp(Current)); AddInfo(TEXT("Thumbnail evidence: ") + Path);
 }
 const TArray<FString> OutputsBefore = Outputs(OutputDirectory, Name);
 for (const TCHAR* Dimension : {TEXT("width"), TEXT("height")})
 {
  for (double Invalid : {0.0, -1.0, 1.5, 4097.0})
  {
   const auto Args = Request(BP->GetPathName()); Args->SetNumberField(Dimension, Invalid);
   const auto Result = Call(*this, &FNwiroAssetTools::GetAssetThumbnail, Args);
   TestFalse(TEXT("invalid numeric dimension rejects"), Success(Result));
   TestTrue(TEXT("invalid dimension has no output path"), Result.IsValid() && !Result->HasField(TEXT("path")));
  }
  const auto Args = Request(BP->GetPathName()); Args->SetStringField(Dimension, TEXT("256"));
  const auto Result = Call(*this, &FNwiroAssetTools::GetAssetThumbnail, Args);
  TestFalse(TEXT("string dimension rejects"), Success(Result));
  TestTrue(TEXT("string dimension has no output path"), Result.IsValid() && !Result->HasField(TEXT("path")));
 }
 TestTrue(TEXT("invalid requests create no owned PNGs"), Outputs(OutputDirectory, Name) == OutputsBefore);
 const auto Search = Request(Folder); Search->SetStringField(TEXT("searchTerm"), Name);
 Search->SetStringField(TEXT("classFilter"), TEXT("/Script/Engine.Blueprint")); Search->SetNumberField(TEXT("maxResults"), 1);
 const auto Found = Call(*this, &FNwiroAssetTools::FindAssets, Search);
 if (!TestTrue(TEXT("valid loaded class finds owned asset"), Success(Found))) return false;
 const auto& Assets = Found->GetArrayField(TEXT("assets"));
 if (!TestEqual(TEXT("owned asset present for invalid-filter controls"), Assets.Num(), 1)) return false;
 TestEqual(TEXT("valid filter returns exact fixture"), Assets[0]->AsObject()->GetStringField(TEXT("path")), BP->GetPathName());
 for (const FString& Filter : {FString(TEXT("/Script/CoreUObject.Vector")), TEXT("MissingClass_") + Id})
 {
  Search->SetStringField(TEXT("classFilter"), Filter);
  const auto Result = Call(*this, &FNwiroAssetTools::FindAssets, Search);
  TestFalse(TEXT("invalid requested class does not broaden search"), Success(Result));
  TestTrue(TEXT("invalid class returns no assets"), Result.IsValid() && !Result->HasField(TEXT("assets")));
 }
 for (const TCHAR* Field : {TEXT("searchTerm"),TEXT("classFilter"),TEXT("path")})
 {
  const auto Args = Request(Folder); Args->SetStringField(TEXT("classFilter"),TEXT("/Script/Engine.Blueprint"));
  Args->SetBoolField(Field,true);
  const auto Result = Call(*this, &FNwiroAssetTools::FindAssets, Args);
  TestFalse(TEXT("malformed search fields cannot broaden the query"),Success(Result));
  TestTrue(TEXT("malformed search returns no assets"),Result.IsValid() && !Result->HasField(TEXT("assets")));
 }
 TArray<uint8> PackageAfter; TestTrue(TEXT("package remains readable"), FFileHelper::LoadFileToArray(PackageAfter, *Filename));
 TestTrue(TEXT("tools preserve saved package bytes"), PackageAfter == PackageBefore);
 TestEqual(TEXT("tools preserve dirty state"), Package->IsDirty(), bDirtyBefore);
 TestTrue(TEXT("tools preserve class and CDO identity"), BP->GeneratedClass == ClassBefore && ClassBefore->GetDefaultObject(false) == CDOBefore);
 TestEqual(TEXT("tools preserve Blueprint status"), static_cast<int32>(BP->Status), static_cast<int32>(StatusBefore));
 AddInfo(TEXT("Retained owned Macro Library: ") + Filename);
 return true;
}
#endif
