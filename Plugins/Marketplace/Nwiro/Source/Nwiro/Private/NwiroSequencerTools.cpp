// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroSequencerTools.h"
#include "NwiroTransactionHelper.h"
#include "Factories/Factory.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSpawnable.h"
#include "NwiroBlueprintPreloadGuard.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
// MovieScene3DTransformSection included via track header
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Json.h"
#include "Misc/FrameRate.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroSequencer, Log, All);

static ULevelSequence* FindSequence(const FString& PathOrName)
{
	UObject* Asset = UEditorAssetLibrary::LoadAsset(PathOrName);
	if (ULevelSequence* Seq = Cast<ULevelSequence>(Asset)) return Seq;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<ULevelSequence>(A.GetAsset());
		}
	}
	return nullptr;
}

// ============================================================
// CREATE SEQUENCE
// ============================================================

FString FNwiroSequencerTools::CreateSequence(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	int32 FPS = Cmd->HasField(TEXT("fps")) ? (int32)Cmd->GetNumberField(TEXT("fps")) : 30;

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Cinematics");

	const FString FullPath = Path / Name;
	if (ULevelSequence* Existing = LoadObject<ULevelSequence>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName());
	}

	UClass* FactoryClass = LoadObject<UClass>(nullptr, TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"));
	UFactory* Factory = FactoryClass ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateSequence", "AI: Create Sequence"));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, ULevelSequence::StaticClass(), Factory);
	ULevelSequence* Sequence = Cast<ULevelSequence>(NewAsset);
	if (!Sequence)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create LevelSequence. Ensure LevelSequenceEditor plugin is enabled.\"}");
	}
	Tx.AlsoModify(Sequence);

	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene)
	{
		Tx.AlsoModify(MovieScene);
		MovieScene->SetDisplayRate(FFrameRate(FPS, 1));
	}

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"fps\":%d}"),
		*Name, *Sequence->GetPathName(), FPS);
}

// ============================================================
// READ SEQUENCE
// ============================================================

namespace NwiroSequenceRead
{
static FString Failure(const FString& Error)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), Error);
	FString Out; FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out)); return Out;
}
static TSharedRef<FJsonObject> TrackJson(const UMovieSceneTrack* Track)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Track->GetDisplayName().ToString());
	Result->SetStringField(TEXT("class"), Track->GetClass()->GetName());
	Result->SetStringField(TEXT("path"), Track->GetPathName());
	Result->SetNumberField(TEXT("sectionCount"), Track->GetAllSections().Num());
	return Result;
}
}

FString FNwiroSequencerTools::ReadSequence(const FString& JsonCommand)
{
	using namespace NwiroSequenceRead;
	if (!IsInGameThread()) return Failure(TEXT("read_sequence requires the game thread."));
	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		return Failure(TEXT("Invalid JSON object."));
	FString Path;
	if (!Cmd->HasTypedField<EJson::String>(TEXT("path")) || !Cmd->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty() || Path.Len() >= NAME_SIZE)
		return Failure(TEXT("path must be an exact sequence asset path or unique exact asset name."));

	// The old shared mutation resolver performs substring fallback. Inspection must
	// never silently report an unrelated asset after an invalid/ambiguous request.
	ULevelSequence* Sequence = nullptr;
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (Path.Contains(TEXT("/")))
	{
		FString ObjectPath;
		if (!NwiroBlueprintPreload::NormalizeExactAssetPath(Path, ObjectPath)) return Failure(TEXT("Invalid exact sequence asset path."));
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath)) Sequence = Cast<ULevelSequence>(Existing);
		else
		{
			const FAssetData Asset = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
			if (Asset.IsValid() && Asset.AssetClassPath == ULevelSequence::StaticClass()->GetClassPathName()) Sequence = Cast<ULevelSequence>(Asset.GetAsset());
		}
	}
	else
	{
		FARFilter Filter; Filter.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());
		TArray<FAssetData> Assets, Matches; ARM.Get().GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets) if (Asset.AssetName.ToString() == Path) Matches.Add(Asset);
		if (Matches.Num() != 1) return Failure(TEXT("Sequence name must have exactly one match; use its exact asset path."));
		Sequence = Cast<ULevelSequence>(Matches[0].GetAsset());
	}
	if (!IsValid(Sequence) || Sequence->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)) return Failure(TEXT("Sequence not found or not ready: ") + Path);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!IsValid(MovieScene)) return Failure(TEXT("No MovieScene."));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Sequence->GetName());
	Result->SetStringField(TEXT("path"), Sequence->GetPathName());
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	Result->SetNumberField(TEXT("fps"), DisplayRate.AsDecimal());
	TSharedRef<FJsonObject> Rate = MakeShared<FJsonObject>();
	Rate->SetNumberField(TEXT("numerator"), DisplayRate.Numerator); Rate->SetNumberField(TEXT("denominator"), DisplayRate.Denominator);
	Result->SetObjectField(TEXT("displayRate"), Rate);
	TArray<TSharedPtr<FJsonValue>> Bindings;
	const UMovieScene* ConstMovieScene = MovieScene;
	for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
	{
		TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
		const FGuid Guid = Binding.GetObjectGuid();
		B->SetStringField(TEXT("guid"), Guid.ToString());
		if (const FMovieScenePossessable* Poss = MovieScene->FindPossessable(Guid))
		{
			B->SetStringField(TEXT("kind"), TEXT("possessable"));
			B->SetStringField(TEXT("name"), Poss->GetName());
			B->SetStringField(TEXT("class"), Poss->GetPossessedObjectClass() ? Poss->GetPossessedObjectClass()->GetName() : TEXT("None"));
		}
		else if (const FMovieSceneSpawnable* Spawn = MovieScene->FindSpawnable(Guid))
		{
			B->SetStringField(TEXT("kind"), TEXT("spawnable"));
			B->SetStringField(TEXT("name"), Spawn->GetName());
			const UObject* Template = Spawn->GetObjectTemplate();
			B->SetStringField(TEXT("class"), Template ? Template->GetClass()->GetName() : TEXT("None"));
		}
		else B->SetStringField(TEXT("kind"), TEXT("unclassified"));
		TArray<TSharedPtr<FJsonValue>> Tracks;
		for (const UMovieSceneTrack* Track : Binding.GetTracks()) if (IsValid(Track)) Tracks.Add(MakeShared<FJsonValueObject>(TrackJson(Track)));
		B->SetArrayField(TEXT("tracks"), Tracks);
		B->SetNumberField(TEXT("trackCount"), Tracks.Num());
		Bindings.Add(MakeShared<FJsonValueObject>(B));
	}
	Result->SetArrayField(TEXT("bindings"), Bindings);
	TArray<TSharedPtr<FJsonValue>> Tracks;
	for (const UMovieSceneTrack* Track : MovieScene->GetTracks()) if (IsValid(Track)) Tracks.Add(MakeShared<FJsonValueObject>(TrackJson(Track)));
	Result->SetArrayField(TEXT("masterTracks"), Tracks);
	if (const UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack()) Result->SetObjectField(TEXT("cameraCutTrack"), TrackJson(CameraCut));
	else Result->SetField(TEXT("cameraCutTrack"), MakeShared<FJsonValueNull>());
	FString Out; FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Out)); return Out;
}

// ============================================================
// ADD SEQUENCE BINDING
// ============================================================

FString FNwiroSequencerTools::AddSequenceBinding(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	FString ActorName = Cmd->GetStringField(TEXT("actor"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSequenceBinding", "AI: Add Sequence Binding"), Sequence);
	Tx.AlsoModify(MovieScene);

	// Find actor in world
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{
			Actor = *It;
			break;
		}
	}
	if (!Actor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	FGuid BindingID = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
	Sequence->BindPossessableObject(BindingID, *Actor, World);

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"bindingId\":\"%s\"}"),
		*Actor->GetActorLabel(), *BindingID.ToString());
}

// ============================================================
// ADD SEQUENCE TRACK
// ============================================================

FString FNwiroSequencerTools::AddSequenceTrack(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	FString BindingStr = Cmd->GetStringField(TEXT("binding"));
	FString TrackType = Cmd->GetStringField(TEXT("trackType"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSequenceTrack", "AI: Add Sequence Track"), Sequence);
	Tx.AlsoModify(MovieScene);

	FString TrackLower = TrackType.ToLower();
	const bool bSequenceLevel = TrackLower == TEXT("audio")
		|| TrackLower == TEXT("cameracut") || TrackLower == TEXT("camera_cut");

	FGuid BindingGuid;
	if (!bSequenceLevel)
	{
		if (!FGuid::Parse(BindingStr, BindingGuid) || !BindingGuid.IsValid())
		{
			for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
			{
				const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
				if (P.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = P.GetGuid(); break; }
			}
			if (!BindingGuid.IsValid())
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
					if (S.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = S.GetGuid(); break; }
				}
		}
		if (!BindingGuid.IsValid())
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Binding not found: %s. Pass either the GUID returned by add_sequence_binding or the actor display name.\"}"), *BindingStr);
	}

	UMovieSceneTrack* Track = nullptr;
	if (TrackLower == TEXT("transform"))
	{
		Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("float"))
	{
		Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("bool"))
	{
		Track = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("audio"))
	{
		Track = MovieScene->AddTrack<UMovieSceneAudioTrack>();
	}
	else if (TrackLower == TEXT("event"))
	{
		Track = MovieScene->AddTrack<UMovieSceneEventTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("cameracut") || TrackLower == TEXT("camera_cut"))
	{
		Track = MovieScene->AddTrack<UMovieSceneCameraCutTrack>();
	}
	else
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown track type: %s. Use: Transform, Float, Bool, Audio, Event, CameraCut\"}"), *TrackType);
	}

	if (!Track)
		return TEXT("{\"success\":false,\"error\":\"Failed to create track\"}");

	// Create initial section
	UMovieSceneSection* Section = Track->CreateNewSection();
	if (Section)
	{
		Track->AddSection(*Section);
		Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(150))); // 5 sec @ 30fps
	}

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"trackType\":\"%s\",\"trackClass\":\"%s\"}"),
		*TrackType, *Track->GetClass()->GetName());
}

// ============================================================
// ADD SEQUENCE KEYFRAME
// ============================================================

FString FNwiroSequencerTools::AddSequenceKeyframe(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	FString BindingStr = Cmd->GetStringField(TEXT("binding"));
	int32 Frame = (int32)Cmd->GetNumberField(TEXT("frame"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSequenceKeyframe", "AI: Add Sequence Keyframe"), Sequence);
	Tx.AlsoModify(MovieScene);

	FGuid BindingGuid;
	if (!FGuid::Parse(BindingStr, BindingGuid) || !BindingGuid.IsValid())
	{
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
			if (P.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = P.GetGuid(); break; }
		}
		if (!BindingGuid.IsValid())
			for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
			{
				const FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
				if (S.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = S.GetGuid(); break; }
			}
	}
	if (!BindingGuid.IsValid())
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Binding not found: %s\"}"), *BindingStr);

	// Find transform track for this binding
	const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
	if (!Binding)
		return TEXT("{\"success\":false,\"error\":\"Binding not found\"}");

	for (UMovieSceneTrack* Track : Binding->GetTracks())
	{
		UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track);
		if (!TransformTrack) continue;

		for (UMovieSceneSection* Section : TransformTrack->GetAllSections())
		{
			// Get channels - Transform has 9 channels: LocX,LocY,LocZ,RotX,RotY,RotZ,ScaleX,ScaleY,ScaleZ
			FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
			TArrayView<FMovieSceneDoubleChannel*> Channels = Proxy.GetChannels<FMovieSceneDoubleChannel>();

			if (Channels.Num() >= 9)
			{
				double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
				double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
				double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;

				FFrameNumber FrameNum(Frame);
				Channels[0]->AddCubicKey(FrameNum, X); // Location X
				Channels[1]->AddCubicKey(FrameNum, Y); // Location Y
				Channels[2]->AddCubicKey(FrameNum, Z); // Location Z

				if (Cmd->HasField(TEXT("pitch")) || Cmd->HasField(TEXT("yaw")) || Cmd->HasField(TEXT("roll")))
				{
					double Pitch = Cmd->HasField(TEXT("pitch")) ? Cmd->GetNumberField(TEXT("pitch")) : 0;
					double Yaw = Cmd->HasField(TEXT("yaw")) ? Cmd->GetNumberField(TEXT("yaw")) : 0;
					double Roll = Cmd->HasField(TEXT("roll")) ? Cmd->GetNumberField(TEXT("roll")) : 0;
					Channels[3]->AddCubicKey(FrameNum, Pitch);
					Channels[4]->AddCubicKey(FrameNum, Yaw);
					Channels[5]->AddCubicKey(FrameNum, Roll);
				}

				Sequence->MarkPackageDirty();

				return FString::Printf(TEXT("{\"success\":true,\"frame\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}"),
					Frame, X, Y, Z);
			}
		}
	}

	return TEXT("{\"success\":false,\"error\":\"No transform track found for this binding\"}");
}

// ============================================================
// SET SEQUENCE RANGE
// ============================================================

FString FNwiroSequencerTools::SetSequenceRange(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	int32 StartFrame = (int32)Cmd->GetNumberField(TEXT("startFrame"));
	int32 EndFrame = (int32)Cmd->GetNumberField(TEXT("endFrame"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetSequenceRange", "AI: Set Sequence Range"), Sequence);
	Tx.AlsoModify(MovieScene);

	if (Cmd->HasField(TEXT("fps")))
	{
		int32 FPS = (int32)Cmd->GetNumberField(TEXT("fps"));
		MovieScene->SetDisplayRate(FFrameRate(FPS, 1));
	}

	MovieScene->SetPlaybackRange(FFrameNumber(StartFrame), (EndFrame - StartFrame));

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"startFrame\":%d,\"endFrame\":%d}"), StartFrame, EndFrame);
}
