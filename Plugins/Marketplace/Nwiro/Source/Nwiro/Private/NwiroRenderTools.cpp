// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroRenderTools.h"
#include "NwiroPathSandbox.h"
#include "EditorAssetLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Camera/CameraActor.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
// MRQ. UMoviePipelineQueueSubsystem (the EDITOR queue subsystem) lives in
// MovieRenderPipelineEditor; the queue itself lives in MovieRenderPipelineCore.
#include "UObject/SoftObjectPath.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelinePIEExecutor.h"
// Movie Render Graph (MRG): the primary config path. The still render is a
// pure run over a provisioned /Game graph asset rather than one built at
// runtime, so only Imath-safe headers are needed here.
#include "Graph/MovieGraphConfig.h"
#include "Graph/Nodes/MovieGraphGlobalOutputSettingNode.h"
#include "Graph/Nodes/MovieGraphFileOutputNode.h"
// The sequence (video) render needs MRG APIs that first appear in UE 5.6
// (AdaptResolution, the struct-typed custom playback bounds) — and the MP4
// encoder module itself does not exist in 5.5. On older engines the tool
// compiles to a clear refusal instead of breaking the build.
#define NWIRO_HAS_SEQUENCE_RENDER (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6))
#if NWIRO_HAS_SEQUENCE_RENDER
#include "Graph/MovieGraphBlueprintLibrary.h" // GetEffectiveFrameRate (render_meta.json)
#include "MovieSceneTimeHelpers.h" // DiscreteInclusiveLower/ExclusiveUpper (camera-cut coverage)
#endif
// UMovieGraphDeferredRenderPassNode (note: the class name does not match the
// header name) and, via MoviePipelineDeferredPasses.h, FMoviePipelinePostProcessPass.
#include "Graph/Nodes/MovieGraphDeferredPassNode.h"
#include "MoviePipelineDeferredPasses.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Json.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
// Header-only dimension probe of the finalized guides (camera.json stamps
// MEASURED output pixels, never a requested or configured size).
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "SceneView.h" // GNearClippingPlane (camera.json)
#include "Math/RotationMatrix.h" // view-matrix basis for camera.json
#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogNwiroRender, Log, All);

// ============================================================
// TODO (render tool family)
// ============================================================
// - multi-camera / batch stills
// - per-job console variable overrides (r.MotionBlurQuality etc.)

// ============================================================
// JOB REGISTRY (static, process-lifetime)
// ============================================================

namespace
{
	enum class ENwiroRenderJobState : uint8
	{
		Running,
		Succeeded,
		Failed
	};

	// Pose + intrinsics captured at SUBMIT time, written out at finalize.
	// Captured early on purpose: for camera="viewport" the tool spawns a
	// transient CineCamera which is destroyed when the render finishes, so by
	// finalize time there is nothing left to read.
	// EXCEPTION: Width/Height are MEASURED from the finalized guide files at
	// finalize time (the movie graph owns the output resolution; there is no
	// width/height request anymore), so they stay 0 until then.
	struct FNwiroCameraMeta
	{
		int32 Width = 0;
		int32 Height = 0;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		double FovDeg = 0.0;
		double NearClip = 0.0;
		bool bValid = false;
	};

	// ── Conditioning-pass inspection (depth / normals) ─────────────────
	// ONE inspector, two callers: ensure_provisioned gates on it, and the render
	// path records its verdict into camera.json. It REPORTS ONLY — every policy
	// decision (what blocks, what merely warns) lives with the caller, so neither
	// caller can silently discard a secondary problem.
	//
	// Binding a pass to your own material is an EXPERT OVERRIDE and is allowed:
	// Nwiro verifies that a guide is structurally emitted, never that its encoding,
	// polarity or range make sense. That is the material author's responsibility —
	// which is why the origin is RECORDED per run rather than enforced here.
	struct FNwiroPassMatch
	{
		bool bEnabled = false;
		// The containing deferred node's material-list override. With it off the
		// engine may ignore the list entirely — warned about, never blocked, until
		// that behaviour is verified live on 5.5-5.8.
		bool bContainerOverrideEnabled = true;
		// 32-bit float output for this pass. Harmless for the MP4 encoder, FATAL
		// for ProRes: that node hardcodes a 16-bit target and the quantizer has
		// no 32-bit source case, so it takes an engine check() — an editor
		// CRASH, not a failed render. Carried here so validation can refuse
		// before submitting.
		bool bHighPrecisionOutput = false;
		// FMoviePipelinePostProcessPass::Material is a TSoftObjectPtr, and the PATH
		// is all identification needs — comparing and printing paths avoids forcing
		// a load of a material we only want to name.
		FSoftObjectPath MaterialPath;
		bool HasMaterial() const { return MaterialPath.IsValid(); }
	};

	// Every pass carrying the searched name, across every deferred node. More than
	// one is an ambiguity, not a preference: passes are identified in the output
	// filename by {renderer_sub_name}, so same-named passes overwrite each other.
	struct FNwiroPassInspection
	{
		TArray<FNwiroPassMatch> Matches;
		bool IsMissing() const { return Matches.Num() == 0; }
		bool IsAmbiguous() const { return Matches.Num() > 1; }
		const FNwiroPassMatch* Single() const { return Matches.Num() == 1 ? &Matches[0] : nullptr; }
	};

	// Where a pass's material came from. Unknown means the comparison could not be
	// made (reference asset unreadable) — never reported as "default" on a guess.
	enum class ENwiroMaterialOrigin : uint8 { Default, Custom, Unknown };

	static const TCHAR* NwiroMaterialOriginToString(ENwiroMaterialOrigin Origin)
	{
		switch (Origin)
		{
		case ENwiroMaterialOrigin::Default: return TEXT("default");
		case ENwiroMaterialOrigin::Custom:  return TEXT("custom");
		default:                            return TEXT("unknown");
		}
	}

	// Per-run provenance for the conditioning guides, written into camera.json.
	// The defaults describe the SHIPPED materials; a custom or unclassifiable
	// material downgrades the claim instead of repeating it falsely. A run folder
	// has to explain itself without knowing which plugin build produced it.
	struct FNwiroGuideProvenance
	{
		FString DepthEncoding = TEXT("reciprocal_normalized");
		FString DepthPolarity = TEXT("near_white");
		FString NormalsSpace  = TEXT("world");
		FString DepthMaterial;
		FString NormalsMaterial;
		FString DepthOrigin   = TEXT("unknown");
		FString NormalsOrigin = TEXT("unknown");
	};

#if NWIRO_HAS_SEQUENCE_RENDER
	// Everything render_pass_sequence changes on the persistent /Game movie graph,
	// captured before the first mutation and written back on every terminal path.
	// The graph is the USER'S editable asset: without this, a per-run output dir
	// would stay stuck on it for the rest of the editor session. The snapshot
	// covers ONLY the fields the tool writes (output directory + filename
	// format) — resolution is deliberately absent, because the tool no longer
	// touches it: the graph owns its resolution, always.
	struct FNwiroMovieGraphSnapshot
	{
		bool bValid = false;
		TWeakObjectPtr<UMovieGraphGlobalOutputSettingNode> OutNode;
		TWeakObjectPtr<UMovieGraphFileOutputNode> FileNode;
		uint8 bDirOverride = 0;
		FString Dir;
		uint8 bFmtOverride = 0;
		FString Fmt;
	};

	// Idempotent: bValid drops after the first restore, so the finalizer and the
	// pre-registration scope guard can both call it without double-writing.
	static void RestoreNwiroMovieGraphSnapshot(FNwiroMovieGraphSnapshot& S)
	{
		if (!S.bValid)
		{
			return;
		}
		S.bValid = false;
		if (UMovieGraphGlobalOutputSettingNode* Out = S.OutNode.Get())
		{
			Out->bOverride_OutputDirectory = S.bDirOverride;
			Out->OutputDirectory.Path = S.Dir;
		}
		if (UMovieGraphFileOutputNode* File = S.FileNode.Get())
		{
			File->bOverride_FileNameFormat = S.bFmtOverride;
			File->FileNameFormat = S.Fmt;
		}
		UE_LOG(LogNwiroRender, Log, TEXT("Sequence render: movie graph settings restored"));
	}

	// Restores on destruction unless disarmed — covers every early return between
	// the first graph mutation and job registration. Registration transfers
	// restoration ownership to the job record (the finalizer restores from there).
	struct FNwiroScopedGraphRestore
	{
		FNwiroMovieGraphSnapshot& Snapshot;
		bool bArmed = true;
		explicit FNwiroScopedGraphRestore(FNwiroMovieGraphSnapshot& InSnapshot) : Snapshot(InSnapshot) {}
		~FNwiroScopedGraphRestore() { if (bArmed) { RestoreNwiroMovieGraphSnapshot(Snapshot); } }
	};
#endif // NWIRO_HAS_SEQUENCE_RENDER

	// Sidecar facts for render_meta.json, captured at submit. Field names are
	// deliberate: graph_* values are CONFIGURATION read off the graph node (not a
	// measured output), sequence_* come from the sequence asset — nothing here
	// claims the encoded mp4 was measured. Rates are rational pairs so
	// 24000/1001 survives. (requested_output_resolution / resolution_was_forced
	// left this struct when the tool stopped accepting a size; the WRITER still
	// emits them as deprecated constants for one release — see
	// WriteNwiroRenderMetaJson.)
	struct FNwiroSequenceMeta
	{
		FString SequencePath;
		FString MapPath;
		int32 DisplayRateNum = 24;
		int32 DisplayRateDen = 1;
		int32 PlaybackFrameCount = 0;
		int32 EffRateNum = 24;
		int32 EffRateDen = 1;
		FIntPoint GraphResolution = FIntPoint::ZeroValue;
		bool bGraphResOverrideEnabled = false;
		FString GraphAdapt;
		bool bGraphAdaptOverrideEnabled = false;
		// Class name of the output node validation accepted. Recorded so the
		// sidecar names the encoder that actually ran rather than the one this
		// platform is expected to use — the graph is a user-editable asset, so
		// the platform only implies an encoder, it does not prove one.
		FString EncoderNodeClass;
		// The encoder's codec/quality tier when it exposes one (ProRes: Proxy,
		// LT, 422, HQ, 4444, 4444 XQ). RECORDED, never enforced: the tier drives
		// the byte rate by roughly an order of magnitude across that range, and
		// without it a /Game copy retuned from Proxy to 4444 XQ would blow the
		// upload budget with nothing in the run to explain why. Empty when the
		// node has no such setting (the MP4 encoder).
		FString EncoderCodec;
	};

	struct FNwiroRenderJobRecord
	{
		ENwiroRenderJobState State = ENwiroRenderJobState::Running;
		FString OutputDir;
		FString Error;
		// Written to camera.json by the finalizer.
		FNwiroCameraMeta CameraMeta;
		// Which conditioning materials this run actually used. Captured at SUBMIT
		// (where the graph is loaded) and written at finalize, same as CameraMeta.
		FNwiroGuideProvenance GuideProvenance;
		// Keeps the transient LevelSequence alive against GC for the duration
		// of the render. Released when the executor finishes.
		TStrongObjectPtr<ULevelSequence> Sequence;
		// Keeps the provisioned MRG graph asset alive against GC while the job
		// runs: the job only holds it through a TSoftObjectPtr. The asset itself
		// persists under /Game, so this is a reference, not ownership.
		TStrongObjectPtr<UMovieGraphConfig> GraphConfig;
		TWeakObjectPtr<UMoviePipelineExecutorBase> Executor;
		TWeakObjectPtr<UMoviePipelineExecutorJob> Job;
		// Set only when we spawned a transient camera for camera="viewport";
		// destroyed on finish.
		TWeakObjectPtr<ACineCameraActor> TransientCamera;
		// The executor-finished delegate can broadcast more than once (see the
		// engine-quirk note in the OnExecutorFinished handler) — cleanup must
		// only ever run once per job.
		bool bCleanupDone = false;
		// Set by cancel_render_job — lets terminal bookkeeping distinguish a
		// user-canceled run from an organic failure.
		bool bCancelRequested = false;
		// Submit timestamp (FPlatformTime::Seconds) — grace-gates the
		// alive-but-not-rendering orphan check against any startup window.
		double SubmittedAt = 0.0;
		// ── Sequence (video) jobs only ──────────────────────────────────
		// True for render_pass_sequence jobs: the finalizer then expects the
		// depth-video bundle + render_meta.json instead of the still JPEGs +
		// camera.json, and success is only committed once both exist.
		bool bIsSequence = false;
		// Restoration ownership: registered here at submit, written back by the
		// finalizer on EVERY terminal path (success, failure, cancel, orphan).
#if NWIRO_HAS_SEQUENCE_RENDER
		FNwiroMovieGraphSnapshot GraphSnapshot;
#endif
		FNwiroSequenceMeta SequenceMeta;
	};

	static TMap<FString, FNwiroRenderJobRecord> GNwiroRenderJobs;

	static const TCHAR* JobStateToString(ENwiroRenderJobState State)
	{
		switch (State)
		{
		case ENwiroRenderJobState::Succeeded: return TEXT("succeeded");
		case ENwiroRenderJobState::Failed:    return TEXT("failed");
		default:                              return TEXT("running");
		}
	}

	static ACameraActor* FindCameraByName(UWorld* World, const FString& NameOrLabel)
	{
		for (TActorIterator<ACameraActor> It(World); It; ++It)
		{
			if ((*It)->GetName() == NameOrLabel || (*It)->GetActorLabel() == NameOrLabel)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// ── SCG project copies: names and versions ──────────────────────────
	// ensure_provisioned copies each shipped template into NwiroScgGameFolder,
	// and the render uses exactly that copy. Version 1 keeps the template's own
	// name; later versions are named <template name>_v<N>.
	//
	// Nwiro never replaces or re-saves a copy once it exists (a render only sets
	// its output folder and file names in memory). To ship a changed template,
	// raise that asset's version below: every project then gets the new copy
	// next to the old one on its next render and renders with it. The old copy
	// stays until the user deletes it; nobody has to delete anything to update.
	//
	// A graph copy points at one depth material copy and is never edited, so a
	// new depth material version needs a new version of every graph as well.
	//
	// Each product ships its own templates and makes its own copies, so a project
	// with both plugins never renders one product from the other's copy (as
	// Landscape does). These three lines are the only ones that differ between
	// the two plugins' copies of this file (scripts/check-plugin-parity.ts).
	static const TCHAR* NwiroScgPluginName = TEXT("Nwiro");
	static const TCHAR* NwiroScgTemplateRoot = TEXT("/Nwiro/Nwiro");
	static const TCHAR* NwiroScgCopySuffix = TEXT("");
	static const TCHAR* NwiroScgGameFolder = TEXT("/Game/NwiroAssets/SceneGeneration");
	static constexpr int32 NwiroScgDepthMIVersion = 1;
	static constexpr int32 NwiroScgStillGraphVersion = 1;
	static constexpr int32 NwiroScgMovieGraphVersion = 1;
	static constexpr int32 NwiroScgProResGraphVersion = 1;
	// The depth material version each graph version points at. Raising
	// NwiroScgDepthMIVersion alone fails these checks on purpose: raise each
	// graph's version too, then set its value here to the new material version.
	static constexpr int32 NwiroScgStillGraphDepthMI = 1;
	static constexpr int32 NwiroScgMovieGraphDepthMI = 1;
	static constexpr int32 NwiroScgProResGraphDepthMI = 1;
	static_assert(NwiroScgStillGraphDepthMI == NwiroScgDepthMIVersion, "MRG_StillScene's copy points at an older depth material copy: raise NwiroScgStillGraphVersion, then set NwiroScgStillGraphDepthMI to NwiroScgDepthMIVersion");
	static_assert(NwiroScgMovieGraphDepthMI == NwiroScgDepthMIVersion, "MRG_MovieScene's copy points at an older depth material copy: raise NwiroScgMovieGraphVersion, then set NwiroScgMovieGraphDepthMI to NwiroScgDepthMIVersion");
	static_assert(NwiroScgProResGraphDepthMI == NwiroScgDepthMIVersion, "MRG_MovieScene_ProRes's copy points at an older depth material copy: raise NwiroScgProResGraphVersion, then set NwiroScgProResGraphDepthMI to NwiroScgDepthMIVersion");

	// A copy's asset name: the template's own name and the product's suffix for
	// version 1, then <template name><suffix>_v2, _v3, ... (there is no _v1).
	static FString NwiroScgCopyName(const TCHAR* TemplateName, int32 Version)
	{
		return Version <= 1
			? FString::Printf(TEXT("%s%s"), TemplateName, NwiroScgCopySuffix)
			: FString::Printf(TEXT("%s%s_v%d"), TemplateName, NwiroScgCopySuffix, Version);
	}
	// A shipped template's package path under this plugin's content.
	static FString NwiroScgTemplate(const TCHAR* RelPath)
	{
		return FString::Printf(TEXT("%s/%s"), NwiroScgTemplateRoot, RelPath);
	}
	// A copy's package path, as UEditorAssetLibrary takes it.
	static FString NwiroScgCopyPackage(const TCHAR* TemplateName, int32 Version)
	{
		return FString::Printf(TEXT("%s/%s"), NwiroScgGameFolder, *NwiroScgCopyName(TemplateName, Version));
	}
	// A copy's object path, as LoadObject takes it: a package path alone
	// resolves to the package, not the asset inside it.
	static FString NwiroScgCopyObject(const TCHAR* TemplateName, int32 Version)
	{
		const FString Name = NwiroScgCopyName(TemplateName, Version);
		return FString::Printf(TEXT("%s/%s.%s"), NwiroScgGameFolder, *Name, *Name);
	}

	// Reference paths shared by the render path and ensure_provisioned.
	static FString NwiroGameDepthMIPath() { return NwiroScgCopyPackage(TEXT("MI_SceneGeneration_SceneDepth_Inst"), NwiroScgDepthMIVersion); }
	static FString NwiroGameStillGraphPackage() { return NwiroScgCopyPackage(TEXT("MRG_StillScene"), NwiroScgStillGraphVersion); }
	static FString NwiroTemplateStillGraphPath() { return NwiroScgTemplate(TEXT("MRGs/MRG_StillScene.MRG_StillScene")); }
	// The movie graph is platform-selected: the engine's MP4 encoder exists only
	// on Windows, and macOS renders through the Apple ProRes node instead. Two
	// template assets rather than one switchable graph, because a failed encoder
	// validation shuts the whole pipeline down — one encoder node per graph.
#if PLATFORM_MAC
	static FString NwiroGameMovieGraphName() { return NwiroScgCopyName(TEXT("MRG_MovieScene_ProRes"), NwiroScgProResGraphVersion); }
	static FString NwiroGameMovieGraphPath() { return NwiroScgCopyObject(TEXT("MRG_MovieScene_ProRes"), NwiroScgProResGraphVersion); }
	// Class-name token the output node must carry on this platform. Kept
	// platform-KEYED rather than "either is fine": the check exists to catch a
	// graph that drifted onto an encoder we never validated here.
	static const TCHAR* NwiroExpectedEncoderToken = TEXT("AppleProRes");
#else
	static FString NwiroGameMovieGraphName() { return NwiroScgCopyName(TEXT("MRG_MovieScene"), NwiroScgMovieGraphVersion); }
	static FString NwiroGameMovieGraphPath() { return NwiroScgCopyObject(TEXT("MRG_MovieScene"), NwiroScgMovieGraphVersion); }
	static const TCHAR* NwiroExpectedEncoderToken = TEXT("MP4");
#endif

	static FNwiroPassInspection InspectNwiroPostProcessPass(UMovieGraphConfig* Graph, const TCHAR* PassName)
	{
		FNwiroPassInspection Out;
		if (!Graph)
		{
			return Out;
		}
		for (UMovieGraphNode* Node : Graph->GetNodes())
		{
			UMovieGraphDeferredRenderPassNode* Deferred = Cast<UMovieGraphDeferredRenderPassNode>(Node);
			if (!Deferred)
			{
				continue;
			}
			for (const FMoviePipelinePostProcessPass& Pass : Deferred->AdditionalPostProcessMaterials)
			{
				if (!Pass.Name.Equals(PassName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				// Whole-graph scan, not first-match-wins: a second match is the
				// thing we most need to report, so it must not be skipped past.
				FNwiroPassMatch& M = Out.Matches.AddDefaulted_GetRef();
				M.bEnabled = Pass.bEnabled;
				M.bContainerOverrideEnabled = Deferred->bOverride_AdditionalPostProcessMaterials != 0;
				M.bHighPrecisionOutput = Pass.bHighPrecisionOutput;
				M.MaterialPath = Pass.Material.ToSoftObjectPath();
			}
		}
		return Out;
	}

	// What the SHIPPED TEMPLATE binds to a pass. Derived, never hardcoded: the
	// normals pass uses an engine material whose path is not guaranteed stable
	// across UE 5.5-5.8, so "what did we ship?" is asked of the template itself.
	// Invalid when the template cannot be read — that yields origin Unknown, which
	// is never on its own a reason to block an otherwise usable graph.
	static FSoftObjectPath NwiroTemplatePassMaterial(const TCHAR* PassName)
	{
		UMovieGraphConfig* Template = LoadObject<UMovieGraphConfig>(nullptr, *NwiroTemplateStillGraphPath());
		if (!Template)
		{
			return FSoftObjectPath();
		}
		const FNwiroPassInspection Ins = InspectNwiroPostProcessPass(Template, PassName);
		const FNwiroPassMatch* M = Ins.Single();
		return M ? M->MaterialPath : FSoftObjectPath();
	}

	// Depth's reference is the PROVISIONED /Game instance — provisioning repoints
	// the pass there so the user's K edits take effect, so that copy, not the
	// template, is what "our material" means. Every other pass is compared against
	// what the template ships.
	static ENwiroMaterialOrigin NwiroClassifyPassMaterial(const TCHAR* PassName, const FSoftObjectPath& MaterialPath)
	{
		if (!MaterialPath.IsValid())
		{
			return ENwiroMaterialOrigin::Unknown;
		}
		FSoftObjectPath Expected;
		if (FCString::Stricmp(PassName, TEXT("depth")) == 0)
		{
			// Built from the loaded object, not from the package string: a soft path
			// carries the "/Package/Path.ObjectName" form, which the bare asset path
			// constant does not.
			if (UObject* GameMI = UEditorAssetLibrary::LoadAsset(NwiroGameDepthMIPath()))
			{
				Expected = FSoftObjectPath(GameMI);
			}
		}
		else
		{
			Expected = NwiroTemplatePassMaterial(PassName);
		}
		if (!Expected.IsValid())
		{
			return ENwiroMaterialOrigin::Unknown;
		}
		return MaterialPath == Expected ? ENwiroMaterialOrigin::Default : ENwiroMaterialOrigin::Custom;
	}

	// Provenance for one run, read off the graph that is about to render it.
	// A pass that is missing or ambiguous cannot be described, so its claim drops
	// to custom/unknown rather than inheriting the shipped material's.
	static FNwiroGuideProvenance BuildNwiroGuideProvenance(UMovieGraphConfig* Graph)
	{
		FNwiroGuideProvenance Prov;

		const FNwiroPassInspection Depth = InspectNwiroPostProcessPass(Graph, TEXT("depth"));
		if (const FNwiroPassMatch* M = Depth.Single())
		{
			const ENwiroMaterialOrigin Origin = NwiroClassifyPassMaterial(TEXT("depth"), M->MaterialPath);
			Prov.DepthMaterial = M->MaterialPath.ToString();
			Prov.DepthOrigin = NwiroMaterialOriginToString(Origin);
			if (Origin != ENwiroMaterialOrigin::Default)
			{
				// The reciprocal claim belongs to OUR material alone.
				Prov.DepthEncoding = TEXT("custom");
				Prov.DepthPolarity = TEXT("unknown");
			}
		}
		else
		{
			Prov.DepthEncoding = TEXT("custom");
			Prov.DepthPolarity = TEXT("unknown");
		}

		const FNwiroPassInspection Normals = InspectNwiroPostProcessPass(Graph, TEXT("normals"));
		if (const FNwiroPassMatch* M = Normals.Single())
		{
			const ENwiroMaterialOrigin Origin = NwiroClassifyPassMaterial(TEXT("normals"), M->MaterialPath);
			Prov.NormalsMaterial = M->MaterialPath.ToString();
			Prov.NormalsOrigin = NwiroMaterialOriginToString(Origin);
			if (Origin != ENwiroMaterialOrigin::Default)
			{
				Prov.NormalsSpace = TEXT("unknown");
			}
		}
		else
		{
			Prov.NormalsSpace = TEXT("unknown");
		}

		UE_LOG(LogNwiroRender, Log, TEXT("SCG guide provenance: depth=%s (%s), normals=%s (%s)"),
			Prov.DepthMaterial.IsEmpty() ? TEXT("<none>") : *Prov.DepthMaterial, *Prov.DepthOrigin,
			Prov.NormalsMaterial.IsEmpty() ? TEXT("<none>") : *Prov.NormalsMaterial, *Prov.NormalsOrigin);
		return Prov;
	}

	// ── SCG still render graph acquisition (pure run) ──────────────────
	// Loads the provisioned still graph copy (NwiroGameStillGraphPackage, made by
	// ensure_provisioned from the plugin template), points its Global
	// Output node at this run's folder, and pins the file-name format so a user
	// edit to their copy cannot break pass identification. K and resolution are
	// left untouched: they live on the user's editable /Game copy. Fails loud if
	// the asset is missing, which means provisioning has not run yet.
	static UMovieGraphConfig* AcquireNwiroStillGraph(const FString& OutputDir, FNwiroGuideProvenance& OutProvenance, FString& OutError)
	{
		UMovieGraphConfig* Graph = LoadObject<UMovieGraphConfig>(nullptr, *NwiroScgCopyObject(TEXT("MRG_StillScene"), NwiroScgStillGraphVersion));
		if (!Graph)
		{
			OutError = FString::Printf(TEXT("the render graph %s is not installed, and setup did not create it. Reinstall the %s plugin, or check that its content is mounted, then try again."), *NwiroGameStillGraphPackage(), NwiroScgPluginName);
			return nullptr;
		}
		for (UMovieGraphNode* Node : Graph->GetNodes())
		{
			if (UMovieGraphGlobalOutputSettingNode* AsOut = Cast<UMovieGraphGlobalOutputSettingNode>(Node))
			{
				// Only the output directory is stomped per-render; resolution is
				// left to the graph (the user's copy owns it).
				AsOut->bOverride_OutputDirectory = 1;
				AsOut->OutputDirectory.Path = OutputDir;
			}
			if (UMovieGraphFileOutputNode* AsFileOut = Cast<UMovieGraphFileOutputNode>(Node))
			{
				// Enforce the pass-identifying name format the finalizer parses by
				// token — a user edit to their copy must not break it.
				AsFileOut->bOverride_FileNameFormat = 1;
				AsFileOut->FileNameFormat = TEXT("{sequence_name}.{layer_name}.{renderer_sub_name}.{frame_number}");
			}
		}
		// Read the conditioning materials off the graph we are ABOUT TO RUN, so the
		// sidecar describes this render rather than what provisioning last saw.
		OutProvenance = BuildNwiroGuideProvenance(Graph);
		UE_LOG(LogNwiroRender, Log, TEXT("SCG still render: using provisioned graph %s -> %s"), *Graph->GetPathName(), *OutputDir);
		return Graph;
	}

#if NWIRO_HAS_SEQUENCE_RENDER
	// ── SCG sequence (video) graph: load + validate WITHOUT mutating ────
	// Everything that can refuse the render happens here, before the first write
	// to the user's asset: the mutation step below then cannot need unwinding for
	// a validation failure. Topology is deliberately strict — exactly one Global
	// Output Setting node and one MP4 output node. A duplicate may even be
	// disconnected and harmless, but without graph traversal the tool cannot tell
	// WHICH node the render will honor, so ambiguity is refused rather than
	// guessed at.
	static UMovieGraphConfig* LoadAndValidateNwiroMovieGraph(
		UMovieGraphGlobalOutputSettingNode*& OutGlobalNode,
		UMovieGraphFileOutputNode*& OutFileNode,
		FString& OutError)
	{
		OutGlobalNode = nullptr;
		OutFileNode = nullptr;
		const FString MovieGraphName = NwiroGameMovieGraphName();
		UMovieGraphConfig* Graph = LoadObject<UMovieGraphConfig>(nullptr, *NwiroGameMovieGraphPath());
		if (!Graph)
		{
			// Names the graph THIS platform looks for — a Mac user sent to
			// provision the Windows asset would install the wrong one.
			OutError = FString::Printf(TEXT("the video render graph %s/%s is not installed, and setup did not create it. Reinstall the %s plugin, or check that its content is mounted, then try again."), NwiroScgGameFolder, *MovieGraphName, NwiroScgPluginName);
			return nullptr;
		}

		int32 GlobalCount = 0;
		int32 FileCount = 0;
		for (UMovieGraphNode* Node : Graph->GetNodes())
		{
			if (UMovieGraphGlobalOutputSettingNode* AsOut = Cast<UMovieGraphGlobalOutputSettingNode>(Node))
			{
				++GlobalCount;
				OutGlobalNode = AsOut;
			}
			if (UMovieGraphFileOutputNode* AsFileOut = Cast<UMovieGraphFileOutputNode>(Node))
			{
				++FileCount;
				OutFileNode = AsFileOut;
			}
		}
		if (GlobalCount != 1 || FileCount != 1)
		{
			OutError = FString::Printf(TEXT("%s has %d output-settings node(s) and %d file-output node(s). The video render needs exactly one of each — with more than one, Nwiro cannot tell which one the render would use. Remove the extras, or delete the /Game copy so setup can recreate it."), *MovieGraphName, GlobalCount, FileCount);
			return nullptr;
		}
		// The encoder nodes live in their own engine modules; identifying them by
		// class NAME (mutating via the UMovieGraphFileOutputNode base) avoids
		// linking MovieRenderPipelineMP4Encoder or AppleProResMedia just for a
		// Cast. The expected token is platform-keyed — accepting "either" would
		// throw away what this check is for.
		if (!OutFileNode->GetClass()->GetName().Contains(NwiroExpectedEncoderToken))
		{
			OutError = FString::Printf(TEXT("%s's file-output node is %s, but the video render needs the %s encoder on this platform. Swap the node back, or delete the /Game copy so setup can recreate it."), *MovieGraphName, *OutFileNode->GetClass()->GetName(), NwiroExpectedEncoderToken);
			return nullptr;
		}

		// The depth pass is what the whole render exists for: EnsureProvisioned
		// only WARNS about this graph (it is optional for still-only setups), so
		// the invocation-time check is the real gate. Same inspector + policy as
		// the still path.
		const FNwiroPassInspection Depth = InspectNwiroPostProcessPass(Graph, TEXT("depth"));
		if (Depth.IsMissing())
		{
			OutError = FString::Printf(TEXT("%s has no 'depth' post-process pass — there is nothing to render. Delete the /Game copy so setup can recreate it."), *MovieGraphName);
			return nullptr;
		}
		if (Depth.IsAmbiguous())
		{
			OutError = FString::Printf(TEXT("%s has more than one 'depth' post-process pass — passes are identified by name in the output, so duplicates overwrite each other. Remove the extra pass."), *MovieGraphName);
			return nullptr;
		}
		// Split rather than merged: the two faults are fixed in different places, so
		// a message that names which one is what the reader acts on. Same policy as
		// the provisioning-time checks, which have always reported them separately.
		if (!Depth.Single()->bEnabled)
		{
			OutError = FString::Printf(TEXT("%s's 'depth' pass is switched off — the render would produce no depth video."), *MovieGraphName);
			return nullptr;
		}
		if (!Depth.Single()->HasMaterial())
		{
			OutError = FString::Printf(TEXT("%s's 'depth' pass has no material assigned — the render would produce no depth video."), *MovieGraphName);
			return nullptr;
		}
		// 32-bit float depth + ProRes = an engine check() inside the quantizer
		// (the ProRes node hardcodes a 16-bit target and has no 32-bit source
		// case), which CRASHES the editor mid-render rather than failing the
		// job. Refusing here is the difference between a message and losing the
		// session. Keyed off the ENCODER that will actually run, not the
		// platform: the graph is user-editable, and it is the node that asserts.
		// Harmless on the MP4 path, so it is not checked there.
		if (OutFileNode->GetClass()->GetName().Contains(TEXT("AppleProRes")) && Depth.Single()->bHighPrecisionOutput)
		{
			OutError = FString::Printf(TEXT("%s's 'depth' pass has 32-bit output (High Precision Output) enabled, which the Apple ProRes encoder cannot accept — it would crash the editor during the render. Turn High Precision Output off on the depth pass."), *MovieGraphName);
			return nullptr;
		}
		return Graph;
	}

	// The output node's codec/quality tier, as its enum value's NAME, or empty
	// when the node exposes no such setting.
	//
	// Read by REFLECTION on purpose: the encoder nodes live in engine modules
	// this file deliberately does not link (same reasoning as identifying the
	// node by class name), so a named property lookup is what keeps that true.
	// A rename upstream degrades to "not recorded", never to a wrong value or a
	// build break — which is why nothing is gated on the result.
	static FString ReadNwiroEncoderCodecName(const UMovieGraphFileOutputNode* Node)
	{
		if (!Node) { return FString(); }
		const FProperty* Prop = Node->GetClass()->FindPropertyByName(TEXT("Quality"));
		if (!Prop) { return FString(); }
		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Prop))
		{
			const void* Value = AsEnum->ContainerPtrToValuePtr<void>(Node);
			const int64 Raw = AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
			const UEnum* Enum = AsEnum->GetEnum();
			return Enum ? Enum->GetNameStringByValue(Raw) : LexToString(Raw);
		}
		if (const FByteProperty* AsByte = CastField<FByteProperty>(Prop))
		{
			const uint8 Raw = AsByte->GetPropertyValue_InContainer(Node);
			return AsByte->Enum ? AsByte->Enum->GetNameStringByValue(Raw) : LexToString(static_cast<int32>(Raw));
		}
		return FString();
	}

	// ── SCG sequence (video) graph: per-run mutation ────────────────────
	// FIRST WRITE to the user's asset — everything refusable happened in
	// LoadAndValidateNwiroMovieGraph. Snapshot precedes every write, and the
	// caller must hold an armed FNwiroScopedGraphRestore from this call until
	// the job record owns the snapshot.
	static void MutateNwiroMovieGraphForRun(
		UMovieGraphGlobalOutputSettingNode* GlobalNode,
		UMovieGraphFileOutputNode* FileNode,
		const FString& OutputDir,
		FNwiroMovieGraphSnapshot& OutSnapshot,
		FNwiroSequenceMeta& InOutMeta)
	{
		OutSnapshot.OutNode = GlobalNode;
		OutSnapshot.FileNode = FileNode;
		OutSnapshot.bDirOverride = GlobalNode->bOverride_OutputDirectory;
		OutSnapshot.Dir = GlobalNode->OutputDirectory.Path;
		OutSnapshot.bFmtOverride = FileNode->bOverride_FileNameFormat;
		OutSnapshot.Fmt = FileNode->FileNameFormat;
		OutSnapshot.bValid = true;

		GlobalNode->bOverride_OutputDirectory = 1;
		GlobalNode->OutputDirectory.Path = OutputDir;
		// No {frame_number}: these video encoders emit ONE file per pass, and the
		// finalizer identifies passes by dot-separated token ({renderer_sub_name}).
		FileNode->bOverride_FileNameFormat = 1;
		FileNode->FileNameFormat = TEXT("{sequence_name}.{layer_name}.{renderer_sub_name}");

		// Resolution is NEVER touched: the user's graph owns it (there is no
		// width/height request anymore). Read the configuration back off the
		// node for the sidecar — these are the graph_* fields, CONFIGURATION,
		// never a claim about the encoded file.
		InOutMeta.GraphResolution = GlobalNode->OutputResolution.Resolution;
		InOutMeta.bGraphResOverrideEnabled = GlobalNode->bOverride_OutputResolution != 0;
		InOutMeta.GraphAdapt = StaticEnum<EMovieGraphAspectRatioAdaptBehavior>()->GetNameStringByValue(static_cast<int64>(GlobalNode->AdaptResolution));
		InOutMeta.bGraphAdaptOverrideEnabled = GlobalNode->bOverride_AdaptResolution != 0;
	}
#endif // NWIRO_HAS_SEQUENCE_RENDER

	// Pose + intrinsics sidecar.
	//
	// The SHAPE HERE IS LOAD-BEARING — it must match the camera.json contract in
	// shared/scg/artifacts.ts (CAMERA_JSON_REQUIRED_KEYS + CAMERA_CONVENTIONS).
	// The SCG executor reads this file and exposes its contents as the capture
	// node's `camera` output pin, and that pin is validated: location must be
	// {x,y,z} and rotation {pitch,yaw,roll} as OBJECTS. Arrays are rejected and
	// the node fails. Matrices are included for the same reason.
	//
	// `conventions` describes THIS run, not the shipped defaults: a custom depth or
	// normals material downgrades the claim to custom/unknown, and the material
	// paths are recorded alongside so the run explains itself.
	// Returns whether camera.json is on disk — the caller fails the job on
	// false (camera.json is part of the bundle contract; a "succeeded" job
	// without it surfaces downstream as a consumer error).
	static bool WriteNwiroCameraJson(const FString& OutputDir, const FNwiroCameraMeta& Meta, const FNwiroGuideProvenance& Prov)
	{
		if (!Meta.bValid)
		{
			return false;
		}
		// Matrices computed UE-style (left-handed, X forward, Z up,
		// reversed-Z projection).
		constexpr double FarClip = 1.0e8; // camera far plane, for the projection matrix
		const double Aspect = Meta.Height > 0 ? static_cast<double>(Meta.Width) / Meta.Height : 1.0;
		const double TanHalfFov = FMath::Tan(FMath::DegreesToRadians(Meta.FovDeg) * 0.5);
		const double InvTan = TanHalfFov != 0.0 ? 1.0 / TanHalfFov : 0.0;

		const FRotationMatrix RotMatrix(Meta.Rotation);
		const FVector Fwd = RotMatrix.GetScaledAxis(EAxis::X);
		const FVector Right = RotMatrix.GetScaledAxis(EAxis::Y);
		const FVector Up = RotMatrix.GetScaledAxis(EAxis::Z);
		const FVector& L = Meta.Location;

		const FString Json = FString::Printf(
			TEXT("{\n")
			TEXT("  \"width\": %d,\n")
			TEXT("  \"height\": %d,\n")
			TEXT("  \"location\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f},\n")
			TEXT("  \"rotation\": {\"pitch\": %.6f, \"yaw\": %.6f, \"roll\": %.6f},\n")
			TEXT("  \"fov_deg\": %.6f,\n")
			TEXT("  \"near_clip\": %.6f,\n")
			TEXT("  \"far_clip\": %.1f,\n")
			TEXT("  \"view_matrix\": [[%.6f, %.6f, %.6f, 0.0], [%.6f, %.6f, %.6f, 0.0], [%.6f, %.6f, %.6f, 0.0], [%.6f, %.6f, %.6f, 1.0]],\n")
			TEXT("  \"projection_matrix\": [[%.6f, 0.0, 0.0, 0.0], [0.0, %.6f, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0], [0.0, 0.0, %.6f, 0.0]],\n")
			TEXT("  \"conventions\": {\"depth_encoding\": \"%s\", \"depth_polarity\": \"%s\", \"normals_space\": \"%s\"},\n")
			TEXT("  \"depth_material\": \"%s\",\n")
			TEXT("  \"depth_material_origin\": \"%s\",\n")
			TEXT("  \"normals_material\": \"%s\",\n")
			TEXT("  \"normals_material_origin\": \"%s\"\n")
			TEXT("}\n"),
			Meta.Width, Meta.Height,
			L.X, L.Y, L.Z,
			Meta.Rotation.Pitch, Meta.Rotation.Yaw, Meta.Rotation.Roll,
			Meta.FovDeg, Meta.NearClip, FarClip,
			Right.X, Up.X, Fwd.X,
			Right.Y, Up.Y, Fwd.Y,
			Right.Z, Up.Z, Fwd.Z,
			-(L.X * Right.X + L.Y * Right.Y + L.Z * Right.Z),
			-(L.X * Up.X + L.Y * Up.Y + L.Z * Up.Z),
			-(L.X * Fwd.X + L.Y * Fwd.Y + L.Z * Fwd.Z),
			InvTan, Aspect * InvTan, Meta.NearClip,
			*Prov.DepthEncoding, *Prov.DepthPolarity, *Prov.NormalsSpace,
			*Prov.DepthMaterial, *Prov.DepthOrigin,
			*Prov.NormalsMaterial, *Prov.NormalsOrigin);
		const FString Path = OutputDir / TEXT("camera.json");
		// Write-to-temp + move, because SaveStringToFile can die MID-write
		// (disk full): a truncated camera.json surviving in a folder whose job
		// reports FAILED would defeat the transactional contract — "failed
		// means NO camera.json". The move is atomic on the same volume; on any
		// failure the temp is swept and no camera.json exists.
		const FString TmpPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Json, *TmpPath))
		{
			IFileManager::Get().Delete(*TmpPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
			UE_LOG(LogNwiroRender, Warning, TEXT("Finalizer: failed writing %s"), *TmpPath);
			return false;
		}
		if (!IFileManager::Get().Move(*Path, *TmpPath))
		{
			IFileManager::Get().Delete(*TmpPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
			UE_LOG(LogNwiroRender, Warning, TEXT("Finalizer: failed moving %s into place"), *TmpPath);
			return false;
		}
		UE_LOG(LogNwiroRender, Log, TEXT("Finalizer: camera.json written (fov %.2f deg, measured %dx%d)"), Meta.FovDeg, Meta.Width, Meta.Height);
		return true;
	}

	// Header-only dimension probe of a finalized guide. The format is detected
	// from the BYTES, never trusted from the extension — a mislabeled file still
	// measures correctly and a garbage file fails here, on the free side of the
	// bundle contract, instead of downstream.
	static bool MeasureNwiroGuide(const FString& Path, FIntPoint& OutDims, FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutError = TEXT("the file could not be read");
			return false;
		}
		IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		const EImageFormat Format = WrapperModule.DetectImageFormat(Bytes.GetData(), Bytes.Num());
		if (Format == EImageFormat::Invalid)
		{
			OutError = TEXT("the file is not a decodable image");
			return false;
		}
		const TSharedPtr<IImageWrapper> Wrapper = WrapperModule.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
		{
			OutError = TEXT("the image header could not be parsed");
			return false;
		}
		const int64 W = static_cast<int64>(Wrapper->GetWidth());
		const int64 H = static_cast<int64>(Wrapper->GetHeight());
		if (W <= 0 || H <= 0 || W > MAX_int32 || H > MAX_int32)
		{
			OutError = FString::Printf(TEXT("the image header reports invalid dimensions (%lldx%lld)"), W, H);
			return false;
		}
		OutDims = FIntPoint(static_cast<int32>(W), static_cast<int32>(H));
		return true;
	}

	// Post-success output normalization: MRG names files by its own tokens, so
	// map the recognized pass names onto the tool's stable bundle contract
	// (inputs/depth.jpeg, inputs/normals.jpeg, inputs/preview.jpeg, with the
	// camera.json sidecar written alongside by WriteNwiroCameraJson). The
	// reciprocal depth material emits display-ready guides, so there is no
	// float EXR to convert here. Only recognized files are renamed: unknown
	// files are logged and left untouched, and nothing is ever deleted.
	//
	// RETURNS success, same policy as the sequence normalizer below: success
	// MEANS the bundle exists and can vouch for itself. All three canonical
	// guides must be present, decode, and agree on pixel dimensions — the
	// agreed measurement comes back in OutMeasured and is what camera.json
	// stamps as width/height (which also drive its projection-matrix aspect).
	// A hole or an unmeasurable guide fails HERE, where the culprit can be
	// named, instead of downstream where it reads as a consumer bug.
	static bool NormalizeNwiroRenderOutputs(const FString& OutputDir, FIntPoint& OutMeasured, FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *OutputDir, TEXT("*"), true, false);
		for (const FString& F : Files)
		{
			// Raw-emission log: what the pipeline actually wrote, before any
			// renaming.
			UE_LOG(LogNwiroRender, Log, TEXT("Render output (raw): %s"), *F);
		}
		for (const FString& File : Files)
		{
			const FString Base = FPaths::GetBaseFilename(File);
			const FString Ext = FPaths::GetExtension(File).ToLower();
			// No EXR anymore — the reciprocal depth material writes a display-ready
			// JPEG, so only viewable image files are part of the bundle.
			const bool bIsViewable = Ext == TEXT("png") || Ext == TEXT("jpeg") || Ext == TEXT("jpg");
			if (!bIsViewable)
			{
				continue;
			}

			// ANCHORED pass matching: the filename format is dot-separated
			// ({renderer_sub_name}.{frame_number}), so compare whole tokens
			// against the known pass names instead of substring-searching the
			// filename. Substring matching mis-renamed passes whenever a level,
			// sequence or layer name happened to contain "depth"/"normal".
			TArray<FString> Tokens;
			Base.ParseIntoArray(Tokens, TEXT("."), true);
			auto HasToken = [&Tokens](const TCHAR* Name)
			{
				return Tokens.ContainsByPredicate([Name](const FString& T) { return T.Equals(Name, ESearchCase::IgnoreCase); });
			};

			FString Target;
			if (HasToken(TEXT("depth")))
			{
				// Reciprocal depth guide (near=white), display-ready from the
				// material — the model-ready depth.jpeg directly, no EXR.
				Target = FString::Printf(TEXT("depth.%s"), *Ext);
			}
			else if (HasToken(TEXT("normals")) || HasToken(TEXT("normal")))
			{
				// PLURAL viewable name (`normals.jpeg`, RUN_FILES.renderNormalsJpeg)
				// — matches the SCG contract. (No EXR to reconcile anymore.)
				Target = FString::Printf(TEXT("normals.%s"), *Ext);
			}
			else if (HasToken(TEXT("beauty")) || HasToken(TEXT("FinalImage")))
			{
				// The viewable beauty is the capture's one human-facing face,
				// so it keeps the canonical "preview" name it has always had.
				Target = FString::Printf(TEXT("preview.%s"), *Ext);
			}
			if (Target.IsEmpty())
			{
				UE_LOG(LogNwiroRender, Log, TEXT("Render output not recognized as a pass — left untouched: %s"), *File);
				continue;
			}

			const FString Dest = OutputDir / Target;
			if (FPaths::IsSamePath(File, Dest))
			{
				continue;
			}
			if (IFileManager::Get().FileExists(*Dest))
			{
				// The submit-time stale-folder refusal guarantees the folder
				// started clean, so a collision here means TWO passes of THIS
				// render mapped onto one canonical name — measuring either
				// would silently pick a winner. Refuse instead.
				OutError = FString::Printf(TEXT("Two render passes both produced %s (same-named passes overwrite each other) — rename the duplicate pass in the movie graph."), *Target);
				return false;
			}
			if (IFileManager::Get().Move(*Dest, *File))
			{
				UE_LOG(LogNwiroRender, Log, TEXT("Render output normalized: %s -> %s"), *FPaths::GetCleanFilename(File), *Target);
			}
			else
			{
				UE_LOG(LogNwiroRender, Warning, TEXT("Failed to rename render output: %s -> %s"), *File, *Target);
			}
		}

		// Bundle contract + measurement. A missing file here means a pass didn't
		// render or its emitted name wasn't recognized above — see the
		// raw-emission log.
		//
		// The reciprocal depth material writes a display-ready JPEG, so depth.jpeg
		// IS expected (no EXR, no downstream normalization). All three guides
		// are JPEG — no alpha channel, so they display correctly in any viewer.
		static const TCHAR* ExpectedBundle[] = { TEXT("depth.jpeg"), TEXT("normals.jpeg"), TEXT("preview.jpeg") };
		FIntPoint Dims = FIntPoint::ZeroValue;
		const TCHAR* DimsSource = nullptr;
		for (const TCHAR* Expected : ExpectedBundle)
		{
			const FString Path = OutputDir / Expected;
			if (!IFileManager::Get().FileExists(*Path))
			{
				OutError = FString::Printf(TEXT("the render finished but %s was not produced — that pass emitted nothing, or emitted a file under a name Nwiro does not recognise. The Output Log lists every file the render wrote."), Expected);
				return false;
			}
			FIntPoint GuideDims = FIntPoint::ZeroValue;
			FString MeasureError;
			if (!MeasureNwiroGuide(Path, GuideDims, MeasureError))
			{
				OutError = FString::Printf(TEXT("the render finished but %s could not be measured — %s. Nwiro cannot record the image size, so this render cannot be used."), Expected, *MeasureError);
				return false;
			}
			if (DimsSource == nullptr)
			{
				Dims = GuideDims;
				DimsSource = Expected;
			}
			else if (Dims != GuideDims)
			{
				OutError = FString::Printf(TEXT("the render produced images of different sizes (%s is %dx%d, %s is %dx%d) — one render must produce one size, so this result cannot be used."), DimsSource, Dims.X, Dims.Y, Expected, GuideDims.X, GuideDims.Y);
				return false;
			}
		}
		OutMeasured = Dims;
		return true;
	}

	// Sequence (video) counterpart of NormalizeNwiroRenderOutputs, deliberately
	// separate: it handles video containers (which the still normalizer's
	// viewable-image gate skips on purpose), and it RETURNS what it produced —
	// a video job with no depth clip must report FAILED, because a "succeeded"
	// job with a missing bundle file
	// surfaces downstream as a confusing consumer error instead of naming the
	// render as the culprit. Same policy on strangers: extra outputs from
	// user-added passes are logged and left in place, never deleted — the
	// bundle contract is a required set, not an exact set.
	// The containers a depth video can be emitted in. ONE definition, read by
	// the raw-emission matcher, the stale preflight and the unrecognized-pass
	// log alike: if these drifted apart, the stale-file protection would cover
	// a different set than normalization claims to handle, and the
	// renamed-in-this-pass proof would quietly weaken.
	static const TCHAR* const NwiroDepthContainers[] = { TEXT("mp4"), TEXT("mov") };

	static bool IsNwiroDepthContainer(const FString& Extension)
	{
		for (const TCHAR* const Container : NwiroDepthContainers)
		{
			if (Extension.Equals(Container, ESearchCase::IgnoreCase)) return true;
		}
		return false;
	}

	// Shared matcher: which video files in the folder are RAW depth emissions
	// (the enforced format {sequence_name}.{layer_name}.{renderer_sub_name}
	// means multiple dot-separated tokens; a canonical single-token depth.<ext>
	// never matches). ONE definition on purpose — the submit-time preflight and
	// the finalizer must agree on what counts, or the stale-file protection
	// drifts.
	static TArray<FString> FindNwiroRawDepthVideos(const FString& OutputDir)
	{
		TArray<FString> Found;
		TArray<FString> Files;
		for (const TCHAR* const Container : NwiroDepthContainers)
		{
			TArray<FString> OfContainer;
			IFileManager::Get().FindFilesRecursive(OfContainer, *OutputDir, *FString::Printf(TEXT("*.%s"), Container), true, false);
			Files.Append(OfContainer);
		}
		for (const FString& File : Files)
		{
			// ANCHORED pass matching, same rule as the still path: dot-separated
			// whole tokens, never substrings.
			const FString Base = FPaths::GetBaseFilename(File);
			TArray<FString> Tokens;
			Base.ParseIntoArray(Tokens, TEXT("."), true);
			if (Tokens.Num() > 1 && Tokens.ContainsByPredicate(
				[](const FString& T) { return T.Equals(TEXT("depth"), ESearchCase::IgnoreCase); }))
			{
				Found.Add(File);
			}
		}
		return Found;
	}

	// What normalization actually produced. A bool return would force the
	// sidecar to DERIVE the container from the platform, turning a measurement
	// into a guess — and the graph is user-editable, so the platform only says
	// which encoder is EXPECTED.
	struct FNwiroNormalizedSequenceOutput
	{
		bool bNormalized = false;
		// Extension of the file actually renamed: "mp4" | "mov". Empty unless
		// bNormalized.
		FString Container;
	};

	// OutError carries the specific failure reason (e.g. ambiguity) so job
	// status reports what actually went wrong; empty = use the caller's generic
	// missing-file message.
	static FNwiroNormalizedSequenceOutput NormalizeNwiroSequenceOutputs(const FString& OutputDir, FString& OutError)
	{
		FNwiroNormalizedSequenceOutput Result;
		OutError.Empty();
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *OutputDir, TEXT("*"), true, false);
		for (const FString& F : Files)
		{
			UE_LOG(LogNwiroRender, Log, TEXT("Sequence output (raw): %s"), *F);
		}
		// Success = a raw depth video was renamed into place IN THIS PASS —
		// never the mere existence of depth.<ext>. This is only proof of a
		// current render because the submit-time preflight refused to start
		// while any raw depth video (e.g. from a FAILED earlier attempt, which
		// skips normalization) was already in the folder: whatever the shared
		// matcher finds now must have appeared after this job was submitted.
		const TArray<FString> RawDepth = FindNwiroRawDepthVideos(OutputDir);
		if (RawDepth.Num() > 1)
		{
			// Same ambiguity policy as duplicate depth passes / output nodes:
			// refuse rather than let filesystem enumeration order pick which
			// file becomes the depth video. Unreachable with the validated
			// template (one depth pass, one output node) — this guards custom
			// graphs. It also covers an mp4 AND a mov sitting together, which
			// means the graph changed under a reused run folder: picking either
			// would hand on a clip from the wrong encoder.
			OutError = FString::Printf(TEXT("Render emitted %d depth videos — ambiguous, refusing to pick one. The movie graph should produce exactly one depth output."), RawDepth.Num());
			UE_LOG(LogNwiroRender, Warning, TEXT("Sequence render emitted %d raw depth video files — ambiguous, refusing to pick one"), RawDepth.Num());
			return Result;
		}
		for (const FString& File : RawDepth)
		{
			// The target keeps the SOURCE's own extension: renaming a QuickTime
			// clip to depth.mp4 would make the file lie about its format, and
			// everything downstream (OS handler, upload type, extension sniff)
			// reads that name.
			const FString Container = FPaths::GetExtension(File).ToLower();
			const FString TargetName = FString::Printf(TEXT("depth.%s"), *Container);
			const FString Target = OutputDir / TargetName;
			if (IFileManager::Get().Move(*Target, *File, /*bReplace*/ true))
			{
				Result.bNormalized = true;
				Result.Container = Container;
				UE_LOG(LogNwiroRender, Log, TEXT("Sequence output normalized: %s -> %s"), *FPaths::GetCleanFilename(File), *TargetName);
			}
			else
			{
				// The render DID produce the video — the failure is filesystem-
				// level (locked target, permissions). Saying so beats the generic
				// "nothing was produced", which would send someone debugging the
				// render instead of the file.
				OutError = FString::Printf(TEXT("Depth video was produced (%s) but could not be renamed to %s — is the file locked or the folder write-protected?"), *FPaths::GetCleanFilename(File), *TargetName);
				UE_LOG(LogNwiroRender, Warning, TEXT("Failed to rename sequence output: %s"), *File);
			}
		}
		for (const FString& File : Files)
		{
			const FString Extension = FPaths::GetExtension(File).ToLower();
			if (IsNwiroDepthContainer(Extension) && !RawDepth.Contains(File)
				&& FPaths::GetCleanFilename(File) != FString::Printf(TEXT("depth.%s"), *Extension))
			{
				UE_LOG(LogNwiroRender, Log, TEXT("Sequence output left as-is (unrecognized pass): %s"), *FPaths::GetCleanFilename(File));
			}
		}
		return Result;
	}

	// Provenance + configuration sidecar for a sequence render. NOT camera.json:
	// a moving camera has no single pose, and CAMERA_JSON_REQUIRED_KEYS
	// (shared/scg/artifacts.ts) mandates pose fields a sequence cannot honestly
	// supply. Field-name policy is documented on FNwiroSequenceMeta.
	static bool WriteNwiroRenderMetaJson(const FString& OutputDir, const FNwiroSequenceMeta& Meta, const FNwiroGuideProvenance& Prov, const FString& DepthContainer)
	{
		TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());

		// The depth clip's container, MEASURED from the file normalization
		// actually renamed — the consumer resolves the artifact's name from
		// this rather than assuming one, so a wrong value here is worse than a
		// missing one. `encoder_node` is the output-node class validation
		// accepted, recorded beside it so a run can be traced to the encoder
		// that produced it.
		Root->SetStringField(TEXT("depth_container"), DepthContainer);
		Root->SetStringField(TEXT("encoder_node"), Meta.EncoderNodeClass);
		// NULL rather than "" when the encoder has no codec setting (the MP4
		// node) or the property could not be read — an empty string would read
		// as a codec named nothing.
		if (Meta.EncoderCodec.IsEmpty())
		{
			Root->SetField(TEXT("encoder_codec"), MakeShareable(new FJsonValueNull()));
		}
		else
		{
			Root->SetStringField(TEXT("encoder_codec"), Meta.EncoderCodec);
		}

		TSharedRef<FJsonObject> Conventions = MakeShareable(new FJsonObject());
		Conventions->SetStringField(TEXT("depth_encoding"), Prov.DepthEncoding);
		Conventions->SetStringField(TEXT("depth_polarity"), Prov.DepthPolarity);
		Root->SetObjectField(TEXT("conventions"), Conventions);
		Root->SetStringField(TEXT("depth_material"), Prov.DepthMaterial);
		Root->SetStringField(TEXT("depth_material_origin"), Prov.DepthOrigin);

		Root->SetStringField(TEXT("sequence"), Meta.SequencePath);
		Root->SetStringField(TEXT("map"), Meta.MapPath);

		auto MakeRate = [](int32 Num, int32 Den)
		{
			TSharedRef<FJsonObject> Rate = MakeShareable(new FJsonObject());
			Rate->SetNumberField(TEXT("numerator"), Num);
			Rate->SetNumberField(TEXT("denominator"), Den);
			return Rate;
		};
		Root->SetObjectField(TEXT("sequence_display_rate"), MakeRate(Meta.DisplayRateNum, Meta.DisplayRateDen));
		Root->SetNumberField(TEXT("sequence_playback_frame_count"), Meta.PlaybackFrameCount);
		Root->SetObjectField(TEXT("effective_output_rate"), MakeRate(Meta.EffRateNum, Meta.EffRateDen));

		// DEPRECATED CONSTANTS, one-release compatibility window: the tool no
		// longer accepts a size, so nothing is ever requested (null) and nothing
		// is ever forced (false) — both literally true. Kept so an older app
		// build that still REQUIRES these keys validates a new plugin's sidecar
		// (and rollback works in both directions). Remove in the release after
		// the app stops requiring them.
		Root->SetField(TEXT("requested_output_resolution"), MakeShareable(new FJsonValueNull()));
		TSharedRef<FJsonObject> GraphRes = MakeShareable(new FJsonObject());
		GraphRes->SetNumberField(TEXT("width"), Meta.GraphResolution.X);
		GraphRes->SetNumberField(TEXT("height"), Meta.GraphResolution.Y);
		Root->SetObjectField(TEXT("graph_output_resolution"), GraphRes);
		Root->SetBoolField(TEXT("graph_output_resolution_override_enabled"), Meta.bGraphResOverrideEnabled);
		Root->SetStringField(TEXT("graph_adapt_resolution"), Meta.GraphAdapt);
		Root->SetBoolField(TEXT("graph_adapt_resolution_override_enabled"), Meta.bGraphAdaptOverrideEnabled);
		Root->SetBoolField(TEXT("resolution_was_forced"), false); // deprecated constant, see above

		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, W);
		const FString Path = OutputDir / TEXT("render_meta.json");
		if (!FFileHelper::SaveStringToFile(Out, *Path))
		{
			UE_LOG(LogNwiroRender, Warning, TEXT("Finalizer: failed writing %s"), *Path);
			return false;
		}
		UE_LOG(LogNwiroRender, Log, TEXT("Finalizer: render_meta.json written (%d frames @ %d/%d)"), Meta.PlaybackFrameCount, Meta.DisplayRateNum, Meta.DisplayRateDen);
		return true;
	}

	// Terminal bookkeeping for a render job — the only place a record is
	// allowed to leave Running. Idempotent (bCleanupDone): callable from the
	// executor-finished broadcast, the status-poll self-heal, the submit-time
	// orphan sweep, and cancel's dead-executor branch, whichever gets there
	// first.
	static void FinalizeNwiroRenderJob(const FString& JobId, FNwiroRenderJobRecord& Rec, ENwiroRenderJobState FinalState, const TCHAR* DefaultError)
	{
		if (Rec.bCleanupDone)
		{
			return;
		}
		Rec.bCleanupDone = true;

		if (Rec.State == ENwiroRenderJobState::Running)
		{
			Rec.State = FinalState;
		}
		// Preserve a more specific reason recorded earlier (e.g. "Canceled via
		// cancel_render_job") — only fall back to DefaultError on a blank slate.
		if (Rec.State == ENwiroRenderJobState::Failed && Rec.Error.IsEmpty() && DefaultError)
		{
			Rec.Error = DefaultError;
		}

		// Clean up the transient viewport camera.
		if (ACineCameraActor* Cam = Rec.TransientCamera.Get())
		{
			if (UWorld* CamWorld = Cam->GetWorld())
			{
				CamWorld->DestroyActor(Cam);
			}
		}
		Rec.TransientCamera = nullptr;

		// Remove our job from the shared queue so a later render doesn't re-run
		// it. Deleting strictly by the job pointer stored in OUR record is what
		// makes this safe — jobs the user queued in the MRQ UI are never touched.
		if (GEditor)
		{
			if (UMoviePipelineQueueSubsystem* Sub = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>())
			{
				if (UMoviePipelineExecutorJob* DoneJob = Rec.Job.Get())
				{
					Sub->GetQueue()->DeleteJob(DoneJob);
				}
			}
		}

		// Put the user's movie graph back EXACTLY as the submit found it — on
		// every terminal path (success, failure, cancel, orphan sweep), before
		// the graph reference is released. No-op for still jobs and for
		// already-restored snapshots (bValid gates).
#if NWIRO_HAS_SEQUENCE_RENDER
		RestoreNwiroMovieGraphSnapshot(Rec.GraphSnapshot);
#endif

		// Release the transient sequence for GC, and drop our strong reference to
		// the provisioned graph asset (the asset itself persists under /Game).
		Rec.Sequence.Reset();
		Rec.GraphConfig.Reset();

		// Map the raw pipeline output names onto the tool's stable bundle
		// contract (only on success — a canceled/failed job has no bundle).
		if (Rec.State == ENwiroRenderJobState::Succeeded)
		{
			if (Rec.bIsSequence)
			{
				// Success MEANS the bundle exists: a sequence job whose depth
				// video or render_meta.json is missing reports FAILED with a
				// specific reason. A job that says "succeeded" while its bundle
				// has a hole pushes the failure downstream — after minutes of
				// rendering — where it reads as a consumer bug rather than a
				// render one.
				FString NormalizeError;
				const FNwiroNormalizedSequenceOutput Normalized = NormalizeNwiroSequenceOutputs(Rec.OutputDir, NormalizeError);
				if (!Normalized.bNormalized)
				{
					Rec.State = ENwiroRenderJobState::Failed;
					Rec.Error = !NormalizeError.IsEmpty() ? NormalizeError
						: TEXT("the render finished but no depth video was produced — the movie graph's depth pass emitted nothing. The Output Log lists every file the render wrote.");
				}
				else if (!WriteNwiroRenderMetaJson(Rec.OutputDir, Rec.SequenceMeta, Rec.GuideProvenance, Normalized.Container))
				{
					Rec.State = ENwiroRenderJobState::Failed;
					Rec.Error = FString::Printf(TEXT("the render produced depth.%s but Nwiro could not write render_meta.json into the run folder — is the disk full, or the folder write-protected?"), *Normalized.Container);
				}
			}
			else
			{
				// Same policy as the sequence branch above: success MEANS the
				// bundle exists AND camera.json tells the truth. The guides are
				// measured, all three must agree, and the agreed dimensions are
				// stamped into camera.json (they drive its projection-matrix
				// aspect) — so this is transactional: on any failure the job
				// reports FAILED with the culprit named and NO camera.json is
				// written; a sidecar claiming dimensions nothing vouches for
				// must not exist.
				FString NormalizeError;
				FIntPoint Measured = FIntPoint::ZeroValue;
				if (!NormalizeNwiroRenderOutputs(Rec.OutputDir, Measured, NormalizeError))
				{
					Rec.State = ENwiroRenderJobState::Failed;
					Rec.Error = NormalizeError;
				}
				else
				{
					Rec.CameraMeta.Width = Measured.X;
					Rec.CameraMeta.Height = Measured.Y;
					if (!WriteNwiroCameraJson(Rec.OutputDir, Rec.CameraMeta, Rec.GuideProvenance))
					{
						Rec.State = ENwiroRenderJobState::Failed;
						Rec.Error = TEXT("the render produced the images but Nwiro could not write camera.json into the run folder — is the disk full, or the folder write-protected?");
					}
				}
			}
		}

		UE_LOG(LogNwiroRender, Log, TEXT("Render job %s finished: %s%s"), *JobId, JobStateToString(Rec.State), Rec.bCancelRequested ? TEXT(" (after cancel request)") : TEXT(""));
	}

	// A Running record that can no longer receive its executor-finished
	// broadcast. Canceling during MRQ's warm-up phase ends the executor without
	// any broadcast that passes the IsRendering() guard: the engine releases the
	// executor, it is GC'd, the record wedges in Running, and the next submit
	// silently re-renders the job left in the shared queue.
	// Two signals, either is terminal:
	//   - executor weak-ptr stale: the engine released it and GC collected it
	//     (do NOT pin the executor with a strong ref — this staleness is the
	//     signal, and the subsystem owns the executor while it is active);
	//   - executor alive but !IsRendering(): the true finished broadcast
	//     passes the guard exactly when IsRendering() is false, so a record
	//     still Running here means that broadcast was missed. Grace-gated on
	//     submit age so no startup window can false-positive.
	static bool IsNwiroRenderJobOrphaned(const FNwiroRenderJobRecord& Rec)
	{
		if (Rec.State != ENwiroRenderJobState::Running || Rec.bCleanupDone)
		{
			return false;
		}
		UMoviePipelineExecutorBase* Exec = Rec.Executor.Get();
		if (!Exec)
		{
			return true;
		}
		return !Exec->IsRendering() && (FPlatformTime::Seconds() - Rec.SubmittedAt) > 10.0;
	}

	// One shape for "the editor is not in a state where it can render". These
	// cases differ only in WHICH internal object could not be obtained, which is
	// diagnosis rather than anything the reader can act on — so the detail rides
	// inside a sentence that ends with the one action available. Reached only
	// when the editor is already failing (out of memory, subsystem torn down),
	// so they are guards, not advice.
	static FString NwiroEditorUnavailable(const TCHAR* Detail)
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"the editor could not start a render (%s). Restart the editor and try again; if it keeps happening, please report this.\"}"), Detail);
	}
}

// ============================================================
// RENDER STILL PASSES
// ============================================================

FString FNwiroRenderTools::RenderStillPasses(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString CameraParam = TEXT("viewport");
	FString OutputDir;
	FString RunId;
	FString RelDir;
	if (Cmd.IsValid())
	{
		// No width/height: the movie graph owns the output resolution, and
		// camera.json's dimensions are MEASURED from the finalized guides.
		if (Cmd->HasField(TEXT("camera"))) CameraParam = Cmd->GetStringField(TEXT("camera"));
		if (Cmd->HasField(TEXT("output_dir"))) OutputDir = Cmd->GetStringField(TEXT("output_dir"));
		if (Cmd->HasField(TEXT("run_id"))) RunId = Cmd->GetStringField(TEXT("run_id"));
		if (Cmd->HasField(TEXT("rel_dir"))) RelDir = Cmd->GetStringField(TEXT("rel_dir"));
	}
	if (CameraParam.IsEmpty()) CameraParam = TEXT("viewport");

	// Output-location precedence (Option 3): the client-side panel runner sends
	// { run_id, rel_dir } and NEVER learns a filesystem path — resolve it against
	// the SCG runs sandbox root here (rejects "../"/absolute escapes). The server
	// executor and the CLI still send an absolute output_dir. Neither present →
	// timestamped fallback under the plugin's own Saved/Nwiro/ home.
	if (!RunId.IsEmpty())
	{
		const FString Rel = RelDir.IsEmpty() ? RunId : (RunId / RelDir);
		OutputDir = NwiroPathSandbox::TryResolveScgRunPath(Rel);
		if (OutputDir.IsEmpty())
		{
			return TEXT("{\"success\":false,\"error\":\"run_id/rel_dir did not resolve inside the SCG runs sandbox (rejected as an escape or invalid name)\"}");
		}
	}
	else if (OutputDir.IsEmpty())
	{
		OutputDir = FPaths::ProjectSavedDir() / TEXT("Nwiro") / TEXT("Renders") / FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	}
	OutputDir = FPaths::ConvertRelativePathToFull(OutputDir);

	// A reused output folder is REFUSED up front (the sequence tool's stale-file
	// policy): the finalizer measures whatever guides sit in the folder and
	// success vouches for them, so a leftover image from an earlier render —
	// canonical (depth.jpeg would block the fresh rename) OR raw (a stale raw
	// still matches the pass tokens and could be renamed as if this render
	// produced it) — could be validated as this render's output. Only viewable
	// images and camera.json are checked; foreign files are left alone.
	{
		TArray<FString> Existing;
		IFileManager::Get().FindFilesRecursive(Existing, *OutputDir, TEXT("*"), true, false);
		for (const FString& F : Existing)
		{
			const FString Ext = FPaths::GetExtension(F).ToLower();
			const bool bViewable = Ext == TEXT("png") || Ext == TEXT("jpeg") || Ext == TEXT("jpg");
			if (bViewable || FPaths::GetCleanFilename(F) == TEXT("camera.json"))
			{
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"the output folder already contains %s from an earlier render — start a new run rather than re-running this one\"}"),
					*FPaths::GetCleanFilename(F));
			}
		}
	}

	if (!GEditor) return NwiroEditorUnavailable(TEXT("no editor available"));

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return NwiroEditorUnavailable(TEXT("no editor world"));

	// MRQ resolves the map by soft object path — an unsaved/untitled level has
	// no on-disk package and the render would fail at map load.
	if (World->GetPackage()->GetName().StartsWith(TEXT("/Temp/")))
		return TEXT("{\"success\":false,\"error\":\"Current level is unsaved. Save the level first — Movie Render Queue loads the map by asset path.\"}");

	UMoviePipelineQueueSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
	if (!Subsystem)
		return TEXT("{\"success\":false,\"error\":\"the Movie Render Queue plugin is not enabled — turn it on in Edit > Plugins and restart the editor\"}");
	if (Subsystem->IsRendering())
		return TEXT("{\"success\":false,\"error\":\"Movie Render Queue is already rendering. Wait for the current job or cancel it first.\"}");

	// Finalize any orphaned records from earlier runs BEFORE enqueuing: an
	// orphan's job is still in the shared queue and the new executor would
	// silently re-render it first (observed live: "starting job [2/2]" and a
	// stray beauty.exr in the old job's folder). Swept strictly from
	// GNwiroRenderJobs — only jobs this tool created are considered; other
	// entries in the shared MRQ queue are never enumerated or touched.
	for (auto& OrphanPair : GNwiroRenderJobs)
	{
		if (IsNwiroRenderJobOrphaned(OrphanPair.Value))
		{
			FinalizeNwiroRenderJob(OrphanPair.Key, OrphanPair.Value, ENwiroRenderJobState::Failed, TEXT("the render stopped without reporting a result — try it again"));
		}
	}

	const FString JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

	// ─── Resolve the camera ─────────────────────────────────────────
	ACameraActor* Camera = nullptr;
	ACineCameraActor* SpawnedCamera = nullptr;
	if (CameraParam.Equals(TEXT("viewport"), ESearchCase::IgnoreCase))
	{
		// Read the active level editor viewport and spawn a TRANSIENT
		// CineCamera there (same viewport access pattern as TakeScreenshot /
		// GetViewportInfo).
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
		if (!ActiveViewport.IsValid())
			return TEXT("{\"success\":false,\"error\":\"no active level viewport to read the camera from — click in a viewport first, or name a camera actor instead of 'viewport'\"}");

		FEditorViewportClient& Client = ActiveViewport->GetAssetViewportClient();
		const FVector ViewLoc = Client.GetViewLocation();
		const FRotator ViewRot = Client.GetViewRotation();
		const float ViewFOV = Client.ViewFOV;

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		SpawnedCamera = World->SpawnActor<ACineCameraActor>(ViewLoc, ViewRot, SpawnParams);
		if (!SpawnedCamera)
			return NwiroEditorUnavailable(TEXT("could not create the temporary camera"));
		SpawnedCamera->SetActorLabel(TEXT("NwiroRender_Camera"));

		// CineCamera derives FOV from focal length + filmback, so translate the
		// viewport's horizontal FOV into a focal length.
		if (UCineCameraComponent* CineComp = SpawnedCamera->GetCineCameraComponent())
		{
			const float SensorWidth = CineComp->Filmback.SensorWidth;
			const float FocalLength = (SensorWidth * 0.5f) / FMath::Tan(FMath::DegreesToRadians(ViewFOV * 0.5f));
			CineComp->SetCurrentFocalLength(FocalLength);
		}
		Camera = SpawnedCamera;
	}
	else
	{
		Camera = FindCameraByName(World, CameraParam);
		if (!Camera)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"no camera actor named '%s' in the current level — check the name in the World Outliner, or use 'viewport' to capture from your current view\"}"), *CameraParam);
	}

	// ─── Build a transient 1-frame LevelSequence with a camera cut ──
	// The sequence lives in the transient package, so the job's soft object
	// path is /Engine/Transient.<name>. MRQ accepts that, so no asset needs to
	// be created on disk for a render.
	const FString SeqName = FString::Printf(TEXT("NwiroRenderSeq_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ULevelSequence* Sequence = NewObject<ULevelSequence>(GetTransientPackage(), FName(*SeqName), RF_Transient);
	if (!Sequence)
	{
		if (SpawnedCamera) World->DestroyActor(SpawnedCamera);
		return NwiroEditorUnavailable(TEXT("could not create the temporary sequence"));
	}
	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		if (SpawnedCamera) World->DestroyActor(SpawnedCamera);
		return NwiroEditorUnavailable(TEXT("the temporary sequence has no data"));
	}

	MovieScene->SetDisplayRate(FFrameRate(24, 1));
	// Playback range is stored in tick resolution, not display frames.
	const FFrameNumber OneFrameTicks = FFrameRate::TransformTime(
		FFrameTime(FFrameNumber(1)), MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).CeilToFrame();
	MovieScene->SetPlaybackRange(FFrameNumber(0), OneFrameTicks.Value);

	// Possessable binding to the resolved camera (same pattern as
	// AddSequenceBinding in NwiroSequencerTools.cpp).
	const FGuid CameraBindingGuid = MovieScene->AddPossessable(Camera->GetActorLabel(), Camera->GetClass());
	Sequence->BindPossessableObject(CameraBindingGuid, *Camera, World);

	UMovieSceneCameraCutTrack* CutTrack = MovieScene->AddTrack<UMovieSceneCameraCutTrack>();
	if (CutTrack)
	{
		UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(
			UE::MovieScene::FRelativeObjectBindingID(CameraBindingGuid), FFrameNumber(0));
		if (CutSection)
		{
			CutSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), OneFrameTicks));
		}
	}

	// ─── Build the MRQ job ──────────────────────────────────────────
	UMoviePipelineQueue* Queue = Subsystem->GetQueue();
	UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	if (!Job)
	{
		if (SpawnedCamera) World->DestroyActor(SpawnedCamera);
		return NwiroEditorUnavailable(TEXT("could not queue the render job"));
	}
	Job->JobName = FString::Printf(TEXT("NwiroRender_%s"), *JobId);
	Job->Map = FSoftObjectPath(World);
	// SetSequence (not raw member write) so the job's shot list is populated
	// from the sequence's camera cuts.
	Job->SetSequence(FSoftObjectPath(Sequence));

	// Load the provisioned still graph and run it as-is.
	FString GraphError;
	FNwiroGuideProvenance GuideProvenance;
	UMovieGraphConfig* GraphPresetForJob = AcquireNwiroStillGraph(OutputDir, GuideProvenance, GraphError);
	if (!GraphPresetForJob)
	{
		Queue->DeleteJob(Job);
		if (SpawnedCamera) World->DestroyActor(SpawnedCamera);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"MRG graph setup failed: %s\"}"), *GraphError);
	}
	Job->SetGraphPreset(GraphPresetForJob);

	// ─── Register the job BEFORE kicking the executor ───────────────
	// A synchronous validation failure fires OnExecutorFinished immediately,
	// so the record must already be findable.
	FNwiroRenderJobRecord& Record = GNwiroRenderJobs.Add(JobId);
	Record.State = ENwiroRenderJobState::Running;
	Record.OutputDir = OutputDir;
	Record.GuideProvenance = GuideProvenance;
	Record.Sequence = TStrongObjectPtr<ULevelSequence>(Sequence);
	if (GraphPresetForJob)
	{
		Record.GraphConfig = TStrongObjectPtr<UMovieGraphConfig>(GraphPresetForJob);
	}
	Record.Job = Job;
	Record.TransientCamera = SpawnedCamera;
	Record.SubmittedAt = FPlatformTime::Seconds();

	// ─── Capture camera pose + intrinsics for camera.json ───────────
	// Read now, not at finalize: the camera="viewport" path spawns a transient
	// CineCamera that is destroyed when the render finishes.
	if (Camera)
	{
		// Width/Height stay 0 here — measured from the finalized guides at
		// finalize time (see NormalizeNwiroRenderOutputs).
		Record.CameraMeta.Location = Camera->GetActorLocation();
		Record.CameraMeta.Rotation = Camera->GetActorRotation();
		Record.CameraMeta.NearClip = static_cast<double>(GNearClippingPlane);
		// A CineCamera derives its FOV from focal length + filmback rather
		// than storing one, so ask the component for the effective value;
		// plain camera actors expose FieldOfView directly.
		if (const ACineCameraActor* CineActor = Cast<ACineCameraActor>(Camera))
		{
			if (const UCineCameraComponent* CineComp = CineActor->GetCineCameraComponent())
			{
				Record.CameraMeta.FovDeg = static_cast<double>(CineComp->GetHorizontalFieldOfView());
			}
		}
		else if (const UCameraComponent* CamComp = Camera->GetCameraComponent())
		{
			Record.CameraMeta.FovDeg = static_cast<double>(CamComp->FieldOfView);
		}
		Record.CameraMeta.bValid = true;
	}

	// UMoviePipelinePIEExecutor, not UMoviePipelineInProcessExecutor: the
	// latter drives map travel on the running game world and is meant for
	// -game / standalone runs. PIEExecutor is the supported local executor for
	// editor-launched renders — what the MRQ UI's "Render (Local)" button uses.
	UMoviePipelinePIEExecutor* Executor = NewObject<UMoviePipelinePIEExecutor>(Subsystem);
	Record.Executor = Executor;

	Executor->OnExecutorErrored().AddLambda(
		[JobId](UMoviePipelineExecutorBase* /*InExecutor*/, UMoviePipeline* /*InPipeline*/, bool bIsFatal, FText ErrorText)
		{
			if (FNwiroRenderJobRecord* Rec = GNwiroRenderJobs.Find(JobId))
			{
				Rec->Error = ErrorText.ToString();
				if (bIsFatal)
				{
					Rec->State = ENwiroRenderJobState::Failed;
				}
				UE_LOG(LogNwiroRender, Warning, TEXT("Render job %s errored (fatal=%d): %s"), *JobId, bIsFatal ? 1 : 0, *Rec->Error);
			}
		});

	Executor->OnExecutorFinished().AddLambda(
		[JobId](UMoviePipelineExecutorBase* InExecutor, bool bSuccess)
		{
			FNwiroRenderJobRecord* Rec = GNwiroRenderJobs.Find(JobId);
			if (!Rec) return;

			// ENGINE QUIRK: UMoviePipelineExecutorBase::
			// OnExecutorErroredImpl broadcasts the *finished* delegate, so
			// CancelAllJobs() fires this while the pipeline is still alive — and
			// again when the executor really ends. Cleaning up on the premature
			// broadcast (in particular DeleteJob on the shared queue) crashed the
			// editor: UMoviePipelinePIEExecutor::OnPIEMoviePipelineFinished then
			// indexed Queue->GetJobs()[CurrentPipelineIndex] on the emptied array.
			// Only treat the broadcast as final once the executor reports it is
			// no longer rendering (bIsRendering clears right before the true
			// end-of-executor broadcast; it is still set during cancel). If the
			// true broadcast never arrives at all (observed live on cancel
			// during MRQ warm-up: the executor ends and is GC'd broadcast-less),
			// the orphan checks in GetRenderJobStatus and the next
			// RenderStillPasses finalize the record instead.
			if (InExecutor && InExecutor->IsRendering())
			{
				UE_LOG(LogNwiroRender, Verbose, TEXT("Render job %s: ignoring premature executor-finished broadcast (executor still rendering)"), *JobId);
				return;
			}

			FinalizeNwiroRenderJob(JobId, *Rec,
				bSuccess ? ENwiroRenderJobState::Succeeded : ENwiroRenderJobState::Failed,
				TEXT("Executor reported failure (no error text — check Output Log / read_log for MovieRenderPipeline entries)"));
		});

	// Kick off the render. This returns after submission — the render itself
	// runs asynchronously (PIE session driven by the executor). Never block here.
	Subsystem->RenderQueueWithExecutorInstance(Executor);

	// No dimensions in this log on purpose: nothing is known yet — the
	// finalizer logs what it MEASURED.
	UE_LOG(LogNwiroRender, Log, TEXT("Render job %s submitted (camera=%s) -> %s"),
		*JobId, *CameraParam, *OutputDir);

	return FString::Printf(TEXT("{\"success\":true,\"job_id\":\"%s\",\"output_dir\":\"%s\"}"),
		*JobId, *OutputDir.Replace(TEXT("\\"), TEXT("/")));
}

// ============================================================
// RENDER PASS SEQUENCE (video: a depth clip from a LevelSequence)
// ============================================================
// Renders an EXISTING LevelSequence through the provisioned /Game movie graph
// for this platform into a run folder: depth.<container> + render_meta.json,
// where the container follows the encoder that ran and is recorded in the
// sidecar rather than assumed. The
// sequence's camera cut owns the viewpoint (no camera param); the graph owns
// the resolution (no width/height params), and every graph setting this tool
// touches is restored on every terminal path. Everything refusable is checked
// BEFORE the first mutation and before minutes of rendering.

FString FNwiroRenderTools::RenderPassSequence(const FString& JsonCommand)
{
#if !NWIRO_HAS_SEQUENCE_RENDER
	// UE 5.5 lacks the MRG APIs (and the MP4 encoder module) this tool needs —
	// the tool exists there as this refusal so older-engine builds stay green.
	return TEXT("{\"success\":false,\"error\":\"Video render requires Unreal 5.6 or newer.\"}");
#elif !PLATFORM_WINDOWS && !PLATFORM_MAC
	// Windows encodes through the engine's MP4 node (Media Foundation) and macOS
	// through the Apple ProRes node. Everywhere else the MP4 module still LOADS
	// but is a dummy that fails at encode time, AFTER minutes of rendering, with
	// no file and no error — refusing up front is the honest version of that.
	return TEXT("{\"success\":false,\"error\":\"Video render is available on Windows and macOS only — this platform has no supported video encoder.\"}");
#else
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString SequencePath;
	FString RunId;
	FString RelDir;
	if (Cmd.IsValid())
	{
		// No width/height: the movie graph owns the output resolution.
		if (Cmd->HasField(TEXT("sequence"))) SequencePath = Cmd->GetStringField(TEXT("sequence"));
		if (Cmd->HasField(TEXT("run_id"))) RunId = Cmd->GetStringField(TEXT("run_id"));
		if (Cmd->HasField(TEXT("rel_dir"))) RelDir = Cmd->GetStringField(TEXT("rel_dir"));
	}
	if (SequencePath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'sequence' — pass the LevelSequence asset path to render\"}");

	// Sandbox-resolved output ONLY — the caller addresses runs by
	// {run_id, rel_dir} and never learns a filesystem path.
	if (RunId.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'run_id' — the sequence render writes into the SCG runs sandbox\"}");
	const FString Rel = RelDir.IsEmpty() ? RunId : (RunId / RelDir);
	FString OutputDir = NwiroPathSandbox::TryResolveScgRunPath(Rel);
	if (OutputDir.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"run_id/rel_dir did not resolve inside the SCG runs sandbox (rejected as an escape or invalid name)\"}");
	OutputDir = FPaths::ConvertRelativePathToFull(OutputDir);

	// Preflight against a reused run folder: a FAILED earlier attempt skips
	// normalization and can leave its raw depth video behind, and a later render
	// into the same folder that emits NOTHING would rename that stale file and
	// report success. Refusing here is what makes the finalizer's
	// renamed-this-pass check a real proof of a current render. Covers EVERY
	// container, not just the one this platform emits — a leftover that arrived
	// some other way is exactly as stale.
	if (FindNwiroRawDepthVideos(OutputDir).Num() > 0)
		return TEXT("{\"success\":false,\"error\":\"the run folder already contains a depth video from an earlier attempt — start a new run, or delete the leftover *.depth.* file, so this render's output cannot be confused with it\"}");

	if (!GEditor) return NwiroEditorUnavailable(TEXT("no editor available"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return NwiroEditorUnavailable(TEXT("no editor world"));

	// MRQ resolves the map by soft object path — an unsaved/untitled level has
	// no on-disk package and the render would fail at map load, minutes late.
	if (World->GetPackage()->GetName().StartsWith(TEXT("/Temp/")))
		return TEXT("{\"success\":false,\"error\":\"Current level is unsaved. Save the level first — Movie Render Queue loads the map by asset path.\"}");

	UMoviePipelineQueueSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
	if (!Subsystem)
		return TEXT("{\"success\":false,\"error\":\"the Movie Render Queue plugin is not enabled — turn it on in Edit > Plugins and restart the editor\"}");
	if (Subsystem->IsRendering())
		return TEXT("{\"success\":false,\"error\":\"Movie Render Queue is already rendering. Wait for the current job or cancel it first.\"}");

	// Same pre-enqueue orphan sweep as the still path (an orphan's job is still
	// in the shared queue and a new executor would silently re-render it).
	for (auto& OrphanPair : GNwiroRenderJobs)
	{
		if (IsNwiroRenderJobOrphaned(OrphanPair.Value))
		{
			FinalizeNwiroRenderJob(OrphanPair.Key, OrphanPair.Value, ENwiroRenderJobState::Failed, TEXT("the render stopped without reporting a result — try it again"));
		}
	}

	// ─── Load the sequence (asset path, not editor state) ───────────
	// UEditorAssetLibrary::LoadAsset accepts the package path callers naturally
	// send; raw LoadObject would demand the full /Path/Name.Name object path.
	ULevelSequence* Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(SequencePath));
	if (!Sequence)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"no Level Sequence found at %s — check the asset path\"}"), *SequencePath);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
		return TEXT("{\"success\":false,\"error\":\"that Level Sequence asset appears to be damaged — it has no sequence data inside\"}");

	const FFrameRate TickRes = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
	if (!Playback.GetLowerBound().IsClosed() || !Playback.GetUpperBound().IsClosed())
		return TEXT("{\"success\":false,\"error\":\"the sequence's playback range is open-ended — give it a start and end frame in Sequencer\"}");

	UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
	if (!CutTrack || CutTrack->GetAllSections().Num() == 0)
		return TEXT("{\"success\":false,\"error\":\"The sequence has no camera cut — add a Camera Cuts track bound to a camera, otherwise the render has no viewpoint\"}");

	// ─── Load + validate the movie graph (no mutation yet) ──────────
	UMovieGraphGlobalOutputSettingNode* GlobalNode = nullptr;
	UMovieGraphFileOutputNode* FileNode = nullptr;
	FString GraphError;
	UMovieGraphConfig* Graph = LoadAndValidateNwiroMovieGraph(GlobalNode, FileNode, GraphError);
	if (!Graph)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *GraphError);

	// ─── Effective render range (ticks) ─────────────────────────────
	// The graph can override playback start/end and add handle frames; camera
	// coverage is validated against the range that will actually render, so no
	// camera hole survives preflight. Conversions mirror the engine: bound
	// values are frames at the ORIGINAL sequence display rate (applied before
	// any frame-rate override), start floors, end ceils, upper bound exclusive.
	// Discrete helpers, not raw bound values: an exclusive lower or inclusive
	// upper bound differs from its raw value by one tick, and coverage math
	// must not inherit that off-by-one.
	FFrameNumber EffStart = UE::MovieScene::DiscreteInclusiveLower(Playback);
	FFrameNumber EffEnd = UE::MovieScene::DiscreteExclusiveUpper(Playback);
	if (GlobalNode->bOverride_CustomPlaybackRangeStart && GlobalNode->CustomPlaybackRangeStart.Type == EMovieGraphSequenceRangeType::Custom)
	{
		const FFrameNumber Custom = FFrameRate::TransformTime(FFrameTime(FFrameNumber(GlobalNode->CustomPlaybackRangeStart.Value)), DisplayRate, TickRes).FloorToFrame();
		if (Custom < EffStart || Custom >= EffEnd)
			return TEXT("{\"success\":false,\"error\":\"the movie graph's custom playback START is outside the sequence's range — move it inside, or widen the sequence\"}");
		EffStart = Custom;
	}
	if (GlobalNode->bOverride_CustomPlaybackRangeEnd && GlobalNode->CustomPlaybackRangeEnd.Type == EMovieGraphSequenceRangeType::Custom)
	{
		const FFrameNumber Custom = FFrameRate::TransformTime(FFrameTime(FFrameNumber(GlobalNode->CustomPlaybackRangeEnd.Value)), DisplayRate, TickRes).CeilToFrame();
		if (Custom <= EffStart || Custom > UE::MovieScene::DiscreteExclusiveUpper(Playback))
			return TEXT("{\"success\":false,\"error\":\"the movie graph's custom playback END is outside the sequence's range (or before its start) — move it inside, or widen the sequence\"}");
		EffEnd = Custom;
	}
	if (GlobalNode->bOverride_HandleFrameCount && GlobalNode->HandleFrameCount > 0)
	{
		const FFrameNumber Handle = FFrameRate::TransformTime(FFrameTime(FFrameNumber(GlobalNode->HandleFrameCount)), DisplayRate, TickRes).CeilToFrame();
		EffStart = EffStart - Handle;
		EffEnd = EffEnd + Handle;
	}

	// ─── Camera-cut coverage + binding validation ───────────────────
	// Coverage runs in the tick domain: display-rate comparison can hide a
	// one-frame gap between seemingly adjacent sections after rate conversion.
	TArray<TRange<FFrameNumber>> CutRanges;
	for (UMovieSceneSection* Section : CutTrack->GetAllSections())
	{
		UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
		if (!Cut || !Cut->IsActive())
		{
			continue;
		}
		CutRanges.Add(Cut->GetRange());

		const FGuid BindingGuid = Cut->GetCameraBindingID().GetGuid();
		if (!BindingGuid.IsValid())
			return TEXT("{\"success\":false,\"error\":\"a camera cut section is not bound to any camera — open the sequence and bind it to a camera actor\"}");
		if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid))
		{
			// Class pre-check (catches a cut bound to a non-camera), then a real
			// resolve in the CURRENT world: iterate its cameras and ask the
			// sequence which guid each one maps to. FindPossessableObjectId is
			// the non-deprecated direction of that lookup.
			const UClass* BoundClass = Possessable->GetPossessedObjectClass();
			if (BoundClass && !BoundClass->IsChildOf(ACameraActor::StaticClass()))
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"the camera cut is bound to '%s', which is not a camera actor — bind it to a camera instead\"}"), *Possessable->GetName());
			bool bResolved = false;
			for (TActorIterator<ACameraActor> It(World); It; ++It)
			{
				if (Sequence->FindPossessableObjectId(**It, World) == BindingGuid)
				{
					bResolved = true;
					break;
				}
			}
			if (!bResolved)
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"Camera cut binding '%s' does not resolve to a camera in the current level — is the right map open?\"}"), *Possessable->GetName());
		}
		else if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
		{
			// A spawnable has no live actor before rendering — validate its
			// object template instead. Valid spawnable cameras must pass.
			if (!Cast<ACameraActor>(Spawnable->GetObjectTemplate()))
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"the camera cut is bound to '%s', which is not a camera actor — bind it to a camera instead\"}"), *Spawnable->GetName());
		}
		else
		{
			return TEXT("{\"success\":false,\"error\":\"A camera cut's binding was not found in the root sequence — camera cuts inside subsequences are not supported\"}");
		}
	}
	if (CutRanges.Num() == 0)
		return TEXT("{\"success\":false,\"error\":\"every camera cut section in the sequence is switched off — re-enable at least one, or the render has no viewpoint\"}");
	// Discrete conversion before sorting/merging: raw bound values ignore
	// inclusive/exclusive semantics and can hide or invent a one-tick gap.
	CutRanges.Sort([](const TRange<FFrameNumber>& A, const TRange<FFrameNumber>& B)
	{
		const FFrameNumber ALo = A.GetLowerBound().IsClosed() ? UE::MovieScene::DiscreteInclusiveLower(A) : FFrameNumber(TNumericLimits<int32>::Min());
		const FFrameNumber BLo = B.GetLowerBound().IsClosed() ? UE::MovieScene::DiscreteInclusiveLower(B) : FFrameNumber(TNumericLimits<int32>::Min());
		return ALo < BLo;
	});
	FFrameNumber Covered = EffStart;
	for (const TRange<FFrameNumber>& R : CutRanges)
	{
		const FFrameNumber Lo = R.GetLowerBound().IsClosed() ? UE::MovieScene::DiscreteInclusiveLower(R) : FFrameNumber(TNumericLimits<int32>::Min());
		const FFrameNumber Hi = R.GetUpperBound().IsClosed() ? UE::MovieScene::DiscreteExclusiveUpper(R) : FFrameNumber(TNumericLimits<int32>::Max());
		if (Lo > Covered)
		{
			break; // gap before this section — reported below
		}
		Covered = FMath::Max(Covered, Hi);
		if (Covered >= EffEnd)
		{
			break;
		}
	}
	if (Covered < EffEnd)
	{
		const int32 GapFrame = FFrameRate::TransformTime(FFrameTime(Covered), TickRes, DisplayRate).FloorToFrame().Value;
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"The camera cuts do not cover the whole render range — no camera is active from frame %d. Extend the camera cut section(s), including any handle frames the movie graph adds.\"}"), GapFrame);
	}

	const FString JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

	// ─── Mutate the graph for this run (snapshot first, guard armed) ─
	FNwiroMovieGraphSnapshot Snapshot;
	FNwiroSequenceMeta Meta;
	MutateNwiroMovieGraphForRun(GlobalNode, FileNode, OutputDir, Snapshot, Meta);
	FNwiroScopedGraphRestore Guard(Snapshot);

	Meta.SequencePath = SequencePath;
	Meta.MapPath = World->GetPackage()->GetName();
	// The node validation just accepted — captured here so the sidecar reports
	// the encoder that ran, not the one the platform implies.
	Meta.EncoderNodeClass = FileNode->GetClass()->GetName();
	Meta.EncoderCodec = ReadNwiroEncoderCodecName(FileNode);
	Meta.DisplayRateNum = DisplayRate.Numerator;
	Meta.DisplayRateDen = DisplayRate.Denominator;
	// Count the display frames the render actually samples: convert each BOUND
	// to the display domain (ceiling the exclusive end), never floor the tick
	// LENGTH of the range — sequencer tooling can store the upper bound a tick
	// short of a frame boundary (e.g. 47999 ticks for a 48-frame range), and
	// flooring the length then undercounts by one while the render still
	// samples the full frame.
	Meta.PlaybackFrameCount =
		(FFrameRate::TransformTime(FFrameTime(UE::MovieScene::DiscreteExclusiveUpper(Playback)), TickRes, DisplayRate).CeilToFrame()
		- FFrameRate::TransformTime(FFrameTime(UE::MovieScene::DiscreteInclusiveLower(Playback)), TickRes, DisplayRate).FloorToFrame()).Value;
	const FFrameRate EffRate = UMovieGraphBlueprintLibrary::GetEffectiveFrameRate(GlobalNode, DisplayRate);
	Meta.EffRateNum = EffRate.Numerator;
	Meta.EffRateDen = EffRate.Denominator;

	const FNwiroGuideProvenance GuideProvenance = BuildNwiroGuideProvenance(Graph);

	// ─── Build + register the MRQ job, then hand restore ownership over ─
	UMoviePipelineQueue* Queue = Subsystem->GetQueue();
	UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	if (!Job)
	{
		return NwiroEditorUnavailable(TEXT("could not queue the render job")); // Guard restores the graph
	}
	Job->JobName = FString::Printf(TEXT("NwiroRender_%s"), *JobId);
	Job->Map = FSoftObjectPath(World);
	// SetSequence (not raw member write) so the job's shot list is populated
	// from the sequence's camera cuts.
	Job->SetSequence(FSoftObjectPath(Sequence));
	Job->SetGraphPreset(Graph);

	// Register BEFORE kicking the executor (a synchronous validation failure
	// fires OnExecutorFinished immediately, so the record must be findable) —
	// and only then disarm the guard: from here the record owns restoration,
	// and the finalizer writes the snapshot back on every terminal path.
	FNwiroRenderJobRecord& Record = GNwiroRenderJobs.Add(JobId);
	Record.State = ENwiroRenderJobState::Running;
	Record.OutputDir = OutputDir;
	Record.GuideProvenance = GuideProvenance;
	Record.Sequence = TStrongObjectPtr<ULevelSequence>(Sequence);
	Record.GraphConfig = TStrongObjectPtr<UMovieGraphConfig>(Graph);
	Record.Job = Job;
	Record.SubmittedAt = FPlatformTime::Seconds();
	Record.bIsSequence = true;
	Record.GraphSnapshot = Snapshot;
	Record.SequenceMeta = Meta;
	Guard.bArmed = false;

	// Same executor + delegate pattern as the still path (see the engine-quirk
	// note there for why the finished broadcast is gated on IsRendering()).
	UMoviePipelinePIEExecutor* Executor = NewObject<UMoviePipelinePIEExecutor>(Subsystem);
	Record.Executor = Executor;

	Executor->OnExecutorErrored().AddLambda(
		[JobId](UMoviePipelineExecutorBase* /*InExecutor*/, UMoviePipeline* /*InPipeline*/, bool bIsFatal, FText ErrorText)
		{
			if (FNwiroRenderJobRecord* Rec = GNwiroRenderJobs.Find(JobId))
			{
				Rec->Error = ErrorText.ToString();
				if (bIsFatal)
				{
					Rec->State = ENwiroRenderJobState::Failed;
				}
				UE_LOG(LogNwiroRender, Warning, TEXT("Sequence render job %s errored (fatal=%d): %s"), *JobId, bIsFatal ? 1 : 0, *Rec->Error);
			}
		});

	Executor->OnExecutorFinished().AddLambda(
		[JobId](UMoviePipelineExecutorBase* InExecutor, bool bSuccess)
		{
			FNwiroRenderJobRecord* Rec = GNwiroRenderJobs.Find(JobId);
			if (!Rec) return;
			if (InExecutor && InExecutor->IsRendering())
			{
				UE_LOG(LogNwiroRender, Verbose, TEXT("Sequence render job %s: ignoring premature executor-finished broadcast (executor still rendering)"), *JobId);
				return;
			}
			FinalizeNwiroRenderJob(JobId, *Rec,
				bSuccess ? ENwiroRenderJobState::Succeeded : ENwiroRenderJobState::Failed,
				TEXT("Executor reported failure (no error text — check Output Log / read_log for MovieRenderPipeline entries)"));
		});

	Subsystem->RenderQueueWithExecutorInstance(Executor);

	UE_LOG(LogNwiroRender, Log, TEXT("Sequence render job %s submitted (sequence=%s, %d frames @ %d/%d) -> %s"),
		*JobId, *SequencePath, Meta.PlaybackFrameCount, Meta.DisplayRateNum, Meta.DisplayRateDen,
		*OutputDir);

	return FString::Printf(TEXT("{\"success\":true,\"job_id\":\"%s\",\"output_dir\":\"%s\"}"),
		*JobId, *OutputDir.Replace(TEXT("\\"), TEXT("/")));
#endif
}

// ============================================================
// GET RENDER JOB STATUS
// ============================================================

FString FNwiroRenderTools::GetRenderJobStatus(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString JobId;
	if (Cmd.IsValid() && Cmd->HasField(TEXT("job_id"))) JobId = Cmd->GetStringField(TEXT("job_id"));
	if (JobId.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Nwiro made an invalid request to the plugin (missing job_id) — please report this\"}");

	FNwiroRenderJobRecord* Record = GNwiroRenderJobs.Find(JobId);
	if (!Record)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"that render (%s) is no longer known to the editor — it was probably restarted. Start the render again.\"}"), *JobId);

	// Self-heal: a record whose executor died broadcast-less would otherwise
	// report "running" forever (and keep its job in the shared queue).
	// Polling converges to failed instead — a cancel reason recorded earlier
	// is preserved by FinalizeNwiroRenderJob.
	if (IsNwiroRenderJobOrphaned(*Record))
	{
		FinalizeNwiroRenderJob(JobId, *Record, ENwiroRenderJobState::Failed, TEXT("the render stopped without reporting a result — try it again"));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("job_id"), JobId);
	Result->SetStringField(TEXT("state"), JobStateToString(Record->State));
	Result->SetStringField(TEXT("output_dir"), Record->OutputDir.Replace(TEXT("\\"), TEXT("/")));
	if (!Record->Error.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), Record->Error);
	}

	TArray<TSharedPtr<FJsonValue>> FilesArr;
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *Record->OutputDir, TEXT("*"), true, false);
	for (const FString& F : FoundFiles)
	{
		FilesArr.Add(MakeShareable(new FJsonValueString(F.Replace(TEXT("\\"), TEXT("/")))));
	}
	Result->SetArrayField(TEXT("files"), FilesArr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CANCEL RENDER JOB
// ============================================================

FString FNwiroRenderTools::CancelRenderJob(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString JobId;
	if (Cmd.IsValid() && Cmd->HasField(TEXT("job_id"))) JobId = Cmd->GetStringField(TEXT("job_id"));
	if (JobId.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Nwiro made an invalid request to the plugin (missing job_id) — please report this\"}");

	FNwiroRenderJobRecord* Record = GNwiroRenderJobs.Find(JobId);
	if (!Record)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"that render (%s) is no longer known to the editor — it was probably restarted. Start the render again.\"}"), *JobId);

	// Self-heal first (same as GetRenderJobStatus): a record whose executor
	// died broadcast-less can never be canceled — without this, an executor
	// that is alive but no longer rendering would hit the spin-up guard below
	// and answer "still starting up — retry" forever. Finalize instead, so the
	// state check answers truthfully.
	if (IsNwiroRenderJobOrphaned(*Record))
	{
		FinalizeNwiroRenderJob(JobId, *Record, ENwiroRenderJobState::Failed, TEXT("the render stopped without reporting a result — try it again"));
	}

	if (Record->State != ENwiroRenderJobState::Running)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"that render already finished (state: %s) — there is nothing to cancel\"}"), JobStateToString(Record->State));

	UMoviePipelineExecutorBase* Executor = Record->Executor.Get();
	if (!Executor)
	{
		// Executor died without a final broadcast — finalize through the shared
		// path (frees the queue slot + transient camera; preserves a cancel
		// reason recorded by an earlier cancel attempt).
		FinalizeNwiroRenderJob(JobId, *Record, ENwiroRenderJobState::Failed, TEXT("the render stopped without reporting a result — try it again"));
		return TEXT("{\"success\":false,\"error\":\"the render stopped without reporting a result — try it again\"}");
	}

	// While PIE is still spinning up the executor has no pipeline yet:
	// CancelAllJobs() in that phase cannot stop the session, and its premature
	// executor-finished broadcast (see the engine-quirk note on the finished
	// handler in RenderStillPasses) would tear the job record down under a
	// render that is still starting. Refuse and let the caller retry.
	if (!Executor->IsRendering())
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Render session for job %s is still starting up and cannot be canceled yet — retry in a moment\"}"), *JobId);
	}

	// Record the cancel intent + reason up front — terminal bookkeeping
	// preserves a non-empty Error whichever path finalizes the record.
	Record->bCancelRequested = true;
	if (Record->Error.IsEmpty())
	{
		Record->Error = TEXT("Canceled via cancel_render_job");
	}

	// Safe here: the pipeline exists, and the finished handler ignores the
	// premature broadcast this triggers (executor still rendering), deferring
	// cleanup to the real end-of-executor broadcast. The stall-stop inside
	// (UMoviePipeline::Shutdown(true)) runs synchronously on the game thread;
	// repeat cancels are no-ops (Shutdown early-returns once Finished).
	Executor->CancelAllJobs();

	UE_LOG(LogNwiroRender, Log, TEXT("Cancel requested for render job %s"), *JobId);

	return FString::Printf(TEXT("{\"success\":true,\"job_id\":\"%s\",\"message\":\"Cancel requested — job will report failed once the pipeline finishes shutting down\"}"), *JobId);
}

// ============================================================
// ENSURE PROVISIONED (SCG render assets)
// ============================================================
//
// Copies the shipped SCG render assets into NwiroScgGameFolder under their
// versioned names (see "SCG project copies: names and versions"), then points
// each FRESHLY COPIED graph's depth pass at the current depth material copy.
// Copy-if-absent: a present copy is NEVER overwritten and never deleted — this
// function has no destructive path. A raised version is a new name, so an
// update arrives as a new copy beside the old one.
//
// What a present copy is validated for is USABILITY, not identity. Pointing the
// depth or normals pass at your own material is an expert override and passes:
// the render is a pure run that never reads the material back, so the invariants
// that matter are structural (exactly one pass under that name, enabled, bound).
// The material actually used is recorded per run in camera.json instead.

FString FNwiroRenderTools::EnsureProvisioned(const FString& /*JsonCommand*/)
{
	// Each fresh copy is stamped with its row's Version (the metadata tag below).
	// Updates do not depend on the stamp: the version is part of the copy's name
	// (see "SCG project copies: names and versions"), so a copy Nwiro made always
	// carries its row's current version and the stamp checks below never fire for
	// it. They only cover an asset at that name that Nwiro did not make (no stamp
	// = version 0): an older stamp WARNS, and blocks only when MinCompatible is
	// raised above it. MinCompatible is 0 for every row, and that blocking path
	// has never run, so test it before relying on it.
	static const TCHAR* VersionTag = TEXT("NwiroScgAssetsVersion");
	const FString GameMIPath = NwiroGameDepthMIPath();

	// bRequired = the STILL render path needs it, so a structural failure is a HARD
	// FAILURE (success=false) and the executor never renders against a broken graph.
	// The movie graph is only used by the video workflow → warnings.
	// bStillBundle = validate the full still bundle (depth AND normals), which only
	// the still graph owes; the movie graph is not asked for a normals guide.
	// MissingHint: what to DO when the template is absent from plugin content.
	// The default answer ("is the plugin content mounted?") is right for assets
	// that only depend on us, and wrong for one that also needs an ENGINE plugin
	// loaded before its node class can resolve — so that row carries its own.
	struct FAssetCopy
	{
		FString Src;
		FString Dest;
		bool bIsGraph;
		bool bRequired;
		bool bStillBundle;
		int32 Version;
		int32 MinCompatible;
		const TCHAR* MissingHint;
	};
	// BOTH movie graphs are provisioned on Windows and Mac, not just the one this
	// platform renders with, so a project moved between the two is already set up
	// and the asset stays version-stamped in one place. Linux gets neither the
	// ProRes row nor the engine plugin (see the plugin descriptor's PlatformAllowList) —
	// the main plugin still supports Linux, only this video path does not.
	// ⚠️ Provisioning it on Windows does NOT make the ProRes path exercisable
	// THROUGH THIS TOOL there: graph selection is compile-time and Windows
	// validates for an MP4 node, so a Windows ProRes render has to be driven by
	// hand through Movie Render Queue. Real end-to-end coverage needs a Mac.
	const FAssetCopy Copies[] = {
		{ NwiroScgTemplate(TEXT("Materials/MI_SceneGeneration_SceneDepth_Inst")), GameMIPath,                                                                 false, true,  false, NwiroScgDepthMIVersion,     0, nullptr },
		{ NwiroScgTemplate(TEXT("MRGs/MRG_StillScene")),                          NwiroGameStillGraphPackage(),                                               true,  true,  true,  NwiroScgStillGraphVersion,  0, nullptr },
		{ NwiroScgTemplate(TEXT("MRGs/MRG_MovieScene")),                          NwiroScgCopyPackage(TEXT("MRG_MovieScene"), NwiroScgMovieGraphVersion),     true,  false, false, NwiroScgMovieGraphVersion,  0, nullptr },
#if PLATFORM_WINDOWS || PLATFORM_MAC
		{ NwiroScgTemplate(TEXT("MRGs/MRG_MovieScene_ProRes")),                   NwiroScgCopyPackage(TEXT("MRG_MovieScene_ProRes"), NwiroScgProResGraphVersion), true,  false, false, NwiroScgProResGraphVersion, 0,
			TEXT("the Apple ProRes Media plugin may be disabled — enable it in Edit > Plugins, RESTART the editor, then try the render again") },
#endif
	};

	TArray<FString> Copied, Skipped, Warnings, Errors;

	static const TCHAR* const ValidatedPasses[] = { TEXT("depth"), TEXT("normals") };

	// The asset's short name rather than its full /Game path. These are message
	// PREFIXES, repeated on every line of the panel's notice box, and the panel
	// relays them verbatim (as does the AI, by instruction). The short name is
	// what the content browser shows and is enough to tell the graphs apart.
	auto AssetLabel = [](const FAssetCopy& C) { return FPaths::GetCleanFilename(C.Dest); };

	// Structural validation of one conditioning pass, against the inspector's
	// report. Returns false only for conditions under which the render would emit
	// no usable guide — a custom material is not one of them.
	auto ValidatePass = [&](UMovieGraphConfig* Graph, const TCHAR* PassName, const FAssetCopy& C) -> bool
	{
		auto Report = [&](const FString& Msg)
		{
			if (C.bRequired) { Errors.Add(Msg); } else { Warnings.Add(Msg); }
		};
		const FNwiroPassInspection Ins = InspectNwiroPostProcessPass(Graph, PassName);
		if (Ins.IsMissing())
		{
			Report(FString::Printf(TEXT("%s: there is no post-process pass named '%s' — the render would emit no %s guide"), *AssetLabel(C), PassName, PassName));
			return false;
		}
		if (Ins.IsAmbiguous())
		{
			// WARNING, not an error, and no further checks on an ambiguous pass.
			//
			// This scan reads every node in the asset, but MRG only evaluates nodes
			// reachable from the output — so a disconnected leftover node carrying a
			// second 'depth' entry lands here while being completely inert. Blocking
			// on that would stop a user whose graph is fine, which is the failure
			// this file has already been burned by once. We also cannot tell WHICH
			// match is the live one, so judging either of them (disabled? unbound?)
			// could block on the dead one — hence the early return.
			//
			// Two genuinely live same-named passes still overwrite each other's
			// output file; the warning says so and the amber box carries it.
			// Branch-aware traversal would separate the two cases properly.
			TArray<FString> Paths;
			for (const FNwiroPassMatch& M : Ins.Matches)
			{
				Paths.Add(M.HasMaterial() ? M.MaterialPath.ToString() : FString(TEXT("<none>")));
			}
			Warnings.Add(FString::Printf(TEXT("%s: %d passes are named '%s' (%s). Passes are identified by that name in the output filename, so if two of them are actually wired to the output they overwrite each other — if the %s guide looks wrong, this is the first thing to check."), *AssetLabel(C), Ins.Matches.Num(), PassName, *FString::Join(Paths, TEXT(", ")), PassName));
			return true;
		}
		const FNwiroPassMatch& M = Ins.Matches[0];
		if (!M.bEnabled)
		{
			// Caught here rather than three minutes into a render, where it surfaces
			// as a missing bundle file with no hint of the cause.
			Report(FString::Printf(TEXT("%s: the '%s' post-process pass is switched off — the render would emit no %s guide"), *AssetLabel(C), PassName, PassName));
			return false;
		}
		if (!M.HasMaterial())
		{
			Report(FString::Printf(TEXT("%s: the '%s' post-process pass has no material assigned — the render would emit no %s guide"), *AssetLabel(C), PassName, PassName));
			return false;
		}
		if (!M.bContainerOverrideEnabled)
		{
			// Warning, never an error: the engine MAY ignore an un-overridden
			// material list, but that is unverified on 5.5-5.8, and this file already
			// carries the scar of a structural check strict enough to call a valid
			// graph broken. Promote it only once observed failing for real.
			Warnings.Add(FString::Printf(TEXT("%s: the node holding the '%s' pass has its additional-post-process-materials override switched off — if that guide comes out missing, check this first"), *AssetLabel(C), PassName));
		}
		if (NwiroClassifyPassMaterial(PassName, M.MaterialPath) == ENwiroMaterialOrigin::Custom)
		{
			Warnings.Add(FString::Printf(TEXT("%s: the '%s' pass uses a custom material (%s). Nwiro checks only that a guide is emitted — its encoding, polarity and range are yours to get right, and camera.json records the choice."), *AssetLabel(C), PassName, *M.MaterialPath.ToString()));
		}
		return true;
	};

	// Re-provisioning means deleting the copy, which throws away any custom
	// binding on it. Name them in the message so they can be re-assigned after.
	auto CustomBindingCaveat = [&](const FAssetCopy& C, UObject* Existing) -> FString
	{
		if (!C.bIsGraph) { return FString(); }
		UMovieGraphConfig* Graph = Cast<UMovieGraphConfig>(Existing);
		TArray<FString> Customs;
		for (const TCHAR* PassName : ValidatedPasses)
		{
			const FNwiroPassInspection Ins = InspectNwiroPostProcessPass(Graph, PassName);
			if (const FNwiroPassMatch* M = Ins.Single())
			{
				if (NwiroClassifyPassMaterial(PassName, M->MaterialPath) == ENwiroMaterialOrigin::Custom)
				{
					Customs.Add(FString::Printf(TEXT("%s = %s"), PassName, *M->MaterialPath.ToString()));
				}
			}
		}
		if (Customs.Num() == 0) { return FString(); }
		return FString::Printf(TEXT(" — note your custom bindings first (%s); deleting the copy loses them"), *FString::Join(Customs, TEXT(", ")));
	};

	// A fresh duplicate still references the PLUGIN material instance, so K edits
	// on the user's /Game MI would be ignored — rewire the "depth" pass to /Game.
	// Returns true iff a "depth" pass was found and bound to the /Game MI.
	auto RepointDepthPass = [&](UMovieGraphConfig* Graph) -> bool
	{
		UMaterialInterface* GameMI = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(GameMIPath));
		if (!Graph || !GameMI) { return false; }
		bool bFound = false;
		for (UMovieGraphNode* Node : Graph->GetNodes())
		{
			if (UMovieGraphDeferredRenderPassNode* Deferred = Cast<UMovieGraphDeferredRenderPassNode>(Node))
			{
				for (FMoviePipelinePostProcessPass& Pass : Deferred->AdditionalPostProcessMaterials)
				{
					if (Pass.Name.Equals(TEXT("depth"), ESearchCase::IgnoreCase))
					{
						Pass.Material = GameMI;
						bFound = true;
					}
				}
			}
		}
		return bFound;
	};

	for (const FAssetCopy& C : Copies)
	{
		if (!UEditorAssetLibrary::DoesAssetExist(C.Src))
		{
			// Provisioning only DETECTS absence here — it cannot make an unloaded
			// engine plugin usable, because the node class has to resolve before
			// the graph asset can load at all. So the row's own hint carries the
			// remediation instead of a generic "content not mounted".
			const FString Hint = C.MissingHint
				? FString(C.MissingHint)
				: FString::Printf(TEXT("reinstall the %s plugin, or check that its content is mounted"), NwiroScgPluginName);
			const FString Msg = FString::Printf(TEXT("the %s plugin's own %s asset is missing: %s"),
				NwiroScgPluginName, *AssetLabel(C), *Hint);
			if (C.bRequired) { return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *Msg); }
			Warnings.Add(Msg);
			continue;
		}
		if (UEditorAssetLibrary::DoesAssetExist(C.Dest))
		{
			// Present — never overwritten (preserve user K/resolution/material
			// edits). Validated on two independent axes: is it compatible with this
			// build (version), and does it still emit what the render needs
			// (structure). Only the second can fail a run today.
			UObject* Existing = UEditorAssetLibrary::LoadAsset(C.Dest);
			const FString Ver = Existing ? UEditorAssetLibrary::GetMetadataTag(Existing, VersionTag) : FString();
			// No stamp = version 0: an asset we did not create (or one from before
			// stamping). Not a fault in itself — structure decides.
			const int32 Stamped = Ver.IsEmpty() ? 0 : FCString::Atoi(*Ver);
			if (Stamped < C.MinCompatible)
			{
				const FString Msg = FString::Printf(TEXT("%s is too old for this Nwiro version (found %s, needs v%d or newer) — delete the /Game copy to re-provision%s"),
					*AssetLabel(C), Ver.IsEmpty() ? TEXT("no version stamp") : *FString::Printf(TEXT("v%s"), *Ver), C.MinCompatible, *CustomBindingCaveat(C, Existing));
				if (C.bRequired) { Errors.Add(Msg); } else { Warnings.Add(Msg); }
			}
			else if (Stamped < C.Version)
			{
				// Informational only. Your copy keeps working; taking the update is
				// your call, because it costs you your edits on that asset.
				Warnings.Add(FString::Printf(TEXT("a newer version of %s ships with this Nwiro (yours v%d, current v%d) — your copy still works; delete it to re-provision if you want the update%s"),
					*AssetLabel(C), Stamped, C.Version, *CustomBindingCaveat(C, Existing)));
			}
			if (C.bIsGraph)
			{
				UMovieGraphConfig* Graph = Cast<UMovieGraphConfig>(Existing);
				ValidatePass(Graph, TEXT("depth"), C);
				if (C.bStillBundle)
				{
					ValidatePass(Graph, TEXT("normals"), C);
				}
			}
			Skipped.Add(C.Dest);
			continue;
		}
		UObject* Dup = UEditorAssetLibrary::DuplicateAsset(C.Src, C.Dest);
		if (!Dup)
		{
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Nwiro could not create %s in your project (%s) — is that folder read-only, or the asset checked out in source control?\"}"), *AssetLabel(C), *C.Dest);
		}
		if (C.bIsGraph)
		{
			UMovieGraphConfig* Graph = Cast<UMovieGraphConfig>(Dup);
			if (!Graph || !RepointDepthPass(Graph))
			{
				// A freshly copied graph with no bindable "depth" pass is a malformed
				// template — validate fresh copies the same as present ones.
				const FString Msg = FString::Printf(TEXT("%s was recreated, but its 'depth' pass could not be bound to the depth material — this Nwiro build shipped a broken template. Please report this."), *AssetLabel(C));
				if (C.bRequired) { Errors.Add(Msg); } else { Warnings.Add(Msg); }
			}
			// Same structural bar as a present copy: a template that ships a broken
			// pass must not slip through just because this is the first run.
			ValidatePass(Graph, TEXT("depth"), C);
			if (C.bStillBundle)
			{
				ValidatePass(Graph, TEXT("normals"), C);
			}
		}
		UEditorAssetLibrary::SetMetadataTag(Dup, VersionTag, FString::FromInt(C.Version));
		UEditorAssetLibrary::SaveAsset(C.Dest, /*bOnlyIfIsDirty*/ false);
		Copied.Add(C.Dest);
	}

	// The render tool loads MRG_StillScene and fails loud without it — make sure
	// provisioning actually left it (and its depth material) in place.
	if (!UEditorAssetLibrary::DoesAssetExist(NwiroGameStillGraphPackage())
		|| !UEditorAssetLibrary::DoesAssetExist(GameMIPath))
	{
		Errors.Add(TEXT("setup finished but the still-render graph and its depth material are still missing — the render cannot start. Please report this."));
	}

	// Any hard error on a REQUIRED asset fails the whole call — the executor gates
	// the render on success, so a stale/broken still graph never renders.
	const bool bSuccess = Errors.Num() == 0;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), bSuccess);
	if (!bSuccess) { Result->SetStringField(TEXT("error"), FString::Join(Errors, TEXT("; "))); }
	auto ToArr = [](const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& I : Items) { Arr.Add(MakeShareable(new FJsonValueString(I))); }
		return Arr;
	};
	Result->SetArrayField(TEXT("errors"), ToArr(Errors));
	Result->SetArrayField(TEXT("copied"), ToArr(Copied));
	Result->SetArrayField(TEXT("skipped"), ToArr(Skipped));
	Result->SetArrayField(TEXT("warnings"), ToArr(Warnings));

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	UE_LOG(LogNwiroRender, Log, TEXT("SCG ensure_provisioned: success=%d copied=%d skipped=%d warnings=%d errors=%d"), bSuccess ? 1 : 0, Copied.Num(), Skipped.Num(), Warnings.Num(), Errors.Num());
	return Out;
}
