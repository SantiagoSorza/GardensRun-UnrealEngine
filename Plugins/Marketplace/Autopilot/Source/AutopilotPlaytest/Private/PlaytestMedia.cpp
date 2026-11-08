// `record` and `capture` (docs/protocol.md, "Recording, stills and usage"): the game's picture as the player sees it,
// UI included, as JPEGs: `frame` messages a few times a second while recording, or one still.
//
// The picture is the game viewport's part of its window's back buffer, once Slate has drawn the window: the scene, the
// HUD and every UMG or Slate widget over it, the same in Play In Editor (the PIE viewport's part of the editor's window,
// never the rest of the editor) and in a game (-game: its window). The scene viewport's own render target would miss the
// UMG widgets, which Slate draws on the window, not on the viewport.
//
// Nothing here waits for the GPU on the game thread:
//   game thread   picks the window and the viewport's rectangle in it (from the widget's paint geometry: window pixels),
//                 queues a job, and later sends what came back (frames whose time comes while one is still in flight
//                 are skipped: a gap in `n`)
//   render thread when Slate presents that window (OnBackBufferReadyToPresent), copies the rectangle into a
//                 FRHIGPUTextureReadback; on later presents, once the copy is ready, maps it and copies the pixels out
//   pool thread   converts, scales (the longer side at most max_dim, never up), encodes the JPEG (IImageWrapper) and
//                 writes the whole message as JSON text, base64 included
// Without a renderer (-nullrhi), with no game running, or with the game's window not drawn, both ops answer 409.

#include "PlaytestInternal.h"

#include <atomic>

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Math/Float16.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "PixelFormat.h"
#include "PlaytestJson.h"
#include "PlaytestSubsystem.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIResources.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
#include "Slate/SlateViewportProvider.h"
#endif

namespace
{
	constexpr double WaitForDrawSeconds = 2.0;   // a job whose window isn't presented by then: no picture
	constexpr double WaitForGpuSeconds = 5.0;    // a copy the GPU hasn't finished by then: given up
	constexpr int32 MaxSpareReadbacks = 4;

	enum class EJobState : int32
	{
		Waiting,   // for its window's next present (render thread)
		Copying,   // the GPU copies it out (render thread)
		Encoding,  // on a pool thread
		Done,      // Text is ready (game thread)
		Failed,    // Error says why (game thread)
	};

	/** One picture asked for: a recording's frame, or a still answering a `capture`. */
	struct FMediaJob
	{
		std::atomic<int32> State{(int32)EJobState::Waiting};
		bool bStill = false;
		int64 RequestId = 0;   // a still: the request it answers
		int64 N = 0;           // a frame: its number in the recording
		int32 Generation = 0;  // a frame: the recording it belongs to
		int32 Serial = 0;      // the connection it was asked on
		double T = 0.0;        // the game time it shows
		double Wall = 0.0;     // Unix seconds, when it was asked for
		int32 MaxDim = 960;
		int32 Quality = 75;
		const SWindow* Window = nullptr;  // its window (compared on the render thread, never used)
		FIntRect Rect;                    // the viewport in the window's back buffer
		double Started = 0.0;             // FPlatformTime

		// Written off the game thread before State becomes Done or Failed
		FString Text;  // a frame: the whole message; a still: the response without its closing "t"
		FString Error;

		bool Move(EJobState From, EJobState To)
		{
			int32 Expected = (int32)From;
			return State.compare_exchange_strong(Expected, (int32)To);
		}

		void Fail(const FString& Why)
		{
			Error = Why;
			State = (int32)EJobState::Failed;
		}
	};

	using FMediaJobPtr = TSharedPtr<FMediaJob, ESPMode::ThreadSafe>;

	double UnixNow()
	{
		return (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalSeconds();
	}

	/** Whether the game draws at all (-nullrhi: never). */
	bool Renders()
	{
		static const bool bRenders = FApp::CanEverRender() && !FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
		return bRenders;
	}

	bool IsSupported(EPixelFormat Format)
	{
		return Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8 || Format == PF_A2B10G10R10 || Format == PF_FloatRGBA;
	}

	/** The read-back pixels (rows packed) as 8-bit BGRA, opaque. */
	bool ToColors(const TArray<uint8>& In, int32 Width, int32 Height, EPixelFormat Format, TArray<FColor>& Out)
	{
		const int32 Count = Width * Height;
		Out.SetNumUninitialized(Count);
		switch (Format)
		{
		case PF_B8G8R8A8:
			FMemory::Memcpy(Out.GetData(), In.GetData(), (SIZE_T)Count * 4);
			for (FColor& C : Out)
			{
				C.A = 255;
			}
			return true;
		case PF_R8G8B8A8:
			for (int32 i = 0; i < Count; ++i)
			{
				const uint8* P = In.GetData() + (SIZE_T)i * 4;
				Out[i] = FColor(P[0], P[1], P[2], 255);
			}
			return true;
		case PF_A2B10G10R10:  // (R in the low 10 bits)
			for (int32 i = 0; i < Count; ++i)
			{
				uint32 V;
				FMemory::Memcpy(&V, In.GetData() + (SIZE_T)i * 4, 4);
				Out[i] = FColor((uint8)((V & 0x3ff) >> 2), (uint8)(((V >> 10) & 0x3ff) >> 2), (uint8)(((V >> 20) & 0x3ff) >> 2), 255);
			}
			return true;
		case PF_FloatRGBA:  // (linear half floats: to sRGB)
			for (int32 i = 0; i < Count; ++i)
			{
				const FFloat16* P = reinterpret_cast<const FFloat16*>(In.GetData() + (SIZE_T)i * 8);
				const FLinearColor Linear(FMath::Clamp(P[0].GetFloat(), 0.f, 1.f), FMath::Clamp(P[1].GetFloat(), 0.f, 1.f),
					FMath::Clamp(P[2].GetFloat(), 0.f, 1.f), 1.f);
				Out[i] = Linear.ToFColor(true);
			}
			return true;
		default:
			return false;
		}
	}

	/** Scaled down to Width x Height by averaging the source pixels each covers (a box filter). */
	TArray<FColor> Shrink(const TArray<FColor>& In, int32 InWidth, int32 InHeight, int32 Width, int32 Height)
	{
		if (Width == InWidth && Height == InHeight)
		{
			return In;
		}
		TArray<FColor> Out;
		Out.SetNumUninitialized(Width * Height);
		TArray<int32> X0, X1;
		X0.SetNumUninitialized(Width);
		X1.SetNumUninitialized(Width);
		for (int32 X = 0; X < Width; ++X)
		{
			X0[X] = (int32)((int64)X * InWidth / Width);
			X1[X] = FMath::Max(X0[X] + 1, (int32)((int64)(X + 1) * InWidth / Width));
		}
		for (int32 Y = 0; Y < Height; ++Y)
		{
			const int32 Y0 = (int32)((int64)Y * InHeight / Height);
			const int32 Y1 = FMath::Max(Y0 + 1, (int32)((int64)(Y + 1) * InHeight / Height));
			for (int32 X = 0; X < Width; ++X)
			{
				uint32 R = 0, G = 0, B = 0, N = 0;
				for (int32 SY = Y0; SY < Y1; ++SY)
				{
					const FColor* Row = In.GetData() + (SIZE_T)SY * InWidth;
					for (int32 SX = X0[X]; SX < X1[X]; ++SX)
					{
						R += Row[SX].R;
						G += Row[SX].G;
						B += Row[SX].B;
						++N;
					}
				}
				Out[Y * Width + X] = FColor((uint8)(R / N), (uint8)(G / N), (uint8)(B / N), 255);
			}
		}
		return Out;
	}

	/** The size a picture is sent at: its longer side at most MaxDim, never scaled up. */
	FIntPoint SentSize(const FIntPoint& Size, int32 MaxDim)
	{
		const int32 Longer = FMath::Max(Size.X, Size.Y);
		if (Longer <= MaxDim || Longer <= 0)
		{
			return Size;
		}
		const double Scale = (double)MaxDim / Longer;
		return FIntPoint(FMath::Max(1, FMath::RoundToInt(Size.X * Scale)), FMath::Max(1, FMath::RoundToInt(Size.Y * Scale)));
	}

	FString Seconds(double Value)
	{
		return FString::Printf(TEXT("%.3f"), Value);
	}

	/** On a pool thread: the pixels to JPEG, and the message (a frame) or the response but its "t" (a still). */
	void Encode(const FMediaJobPtr& Job, IImageWrapperModule* Wrappers, TArray<uint8> Pixels, FIntPoint Size, EPixelFormat Format)
	{
		TArray<FColor> Colors;
		if (!ToColors(Pixels, Size.X, Size.Y, Format, Colors))
		{
			Job->Fail(TEXT("no picture: the window's pixel format isn't supported"));
			return;
		}
		Pixels.Empty();
		const FIntPoint Sent = SentSize(Size, Job->MaxDim);
		Colors = Shrink(Colors, Size.X, Size.Y, Sent.X, Sent.Y);
		TSharedPtr<IImageWrapper> Jpeg = Wrappers ? Wrappers->CreateImageWrapper(EImageFormat::JPEG) : nullptr;
		if (!Jpeg.IsValid() || !Jpeg->SetRaw(Colors.GetData(), (int64)Colors.Num() * 4, Sent.X, Sent.Y, ERGBFormat::BGRA, 8))
		{
			Job->Fail(TEXT("no picture: the JPEG encoder isn't available"));
			return;
		}
		const TArray64<uint8> Bytes = Jpeg->GetCompressed(FMath::Clamp(Job->Quality, 1, 100));
		if (Bytes.Num() == 0)
		{
			Job->Fail(TEXT("no picture: the JPEG couldn't be encoded"));
			return;
		}
		const FString Base64 = FBase64::Encode(Bytes.GetData(), (uint32)Bytes.Num());
		FString Text;
		Text.Reserve(Base64.Len() + 256);
		if (Job->bStill)
		{
			Text += FString::Printf(TEXT("{\"type\":\"response\",\"id\":%lld,\"ok\":true,\"result\":{\"width\":%d,\"height\":%d,\"t\":%s,\"jpeg\":\""),
				(long long)Job->RequestId, Sent.X, Sent.Y, *Seconds(Job->T));
			Text += Base64;
			Text += TEXT("\"}");
		}
		else
		{
			Text += FString::Printf(TEXT("{\"type\":\"frame\",\"n\":%lld,\"t\":%s,\"wall\":%s,\"width\":%d,\"height\":%d,\"jpeg\":\""),
				(long long)Job->N, *Seconds(Job->T), *Seconds(Job->Wall), Sent.X, Sent.Y);
			Text += Base64;
			Text += TEXT("\"}");
		}
		Job->Text = MoveTemp(Text);
		Job->State = (int32)EJobState::Done;
	}
}

/** The render thread's side: copies a waiting job's rectangle when its window is presented, and maps finished copies. */
class FPlaytestMediaRender : public TSharedFromThis<FPlaytestMediaRender, ESPMode::ThreadSafe>
{
public:
	IImageWrapperModule* Wrappers = nullptr;  // loaded on the game thread

	void Add(const FMediaJobPtr& Job)
	{
		FScopeLock Locked(&Lock);
		Waiting.Add(Job);
	}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
	void OnBackBuffer(SWindow& Window, const FTextureRHIRef& BackBuffer)
	{
		Present(Window, BackBuffer.GetReference());
	}
#else
	void OnBackBuffer(SWindow& Window, ISlateViewportProvider& Provider)
	{
		Present(Window, Provider.GetBackBufferResource());
	}
#endif

	/** Frees the copies' textures (render thread). */
	void Release()
	{
		Reading.Reset();
		Spares.Reset();
	}

private:
	struct FRead
	{
		FMediaJobPtr Job;
		TSharedPtr<FRHIGPUTextureReadback> Readback;
		FIntPoint Size = FIntPoint::ZeroValue;
		EPixelFormat Format = PF_Unknown;
	};

	void Present(SWindow& Window, FRHITexture* BackBuffer)
	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		Collect();
		TArray<FMediaJobPtr> Mine;
		{
			FScopeLock Locked(&Lock);
			for (int32 i = Waiting.Num() - 1; i >= 0; --i)
			{
				const FMediaJobPtr& Job = Waiting[i];
				if (Job->State != (int32)EJobState::Waiting)
				{
					Waiting.RemoveAt(i);  // (given up on by the game thread)
				}
				else if (Job->Window == &Window)
				{
					Mine.Add(Job);
					Waiting.RemoveAt(i);
				}
			}
		}
		for (const FMediaJobPtr& Job : Mine)
		{
			if (!Job->Move(EJobState::Waiting, EJobState::Copying))
			{
				continue;
			}
			if (!BackBuffer)
			{
				Job->Fail(TEXT("no picture: the game's window has no picture"));
				continue;
			}
			const FIntPoint Extent = BackBuffer->GetDesc().Extent;
			FIntRect Rect = Job->Rect;
			Rect.Clip(FIntRect(FIntPoint::ZeroValue, Extent));
			if (Rect.Width() <= 0 || Rect.Height() <= 0)
			{
				Job->Fail(TEXT("no picture: the game's viewport is outside its window"));
				continue;
			}
			const EPixelFormat Format = BackBuffer->GetDesc().Format;
			if (!IsSupported(Format))
			{
				Job->Fail(FString::Printf(TEXT("no picture: the window's pixel format (%s) isn't supported"), GPixelFormats[Format].Name));
				continue;
			}
			FRead Read;
			Read.Job = Job;
			Read.Size = Rect.Size();
			Read.Format = Format;
			Read.Readback = TakeSpare(Read.Size, Format);
			// The back buffer's state while Slate presents it (SlateRHIRenderer.cpp of each version): shader read (5.2-5.4:
			// Slate's own transition before the broadcast), RDG's default final state for an external texture (5.5, 5.6:
			// SRVMask), present (5.7+: SetTextureAccessFinal). Put back as it was once copied.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
			const ERHIAccess Before = ERHIAccess::SRVGraphics;
			const ERHIAccess After = ERHIAccess::SRVGraphics;
#elif UE_VERSION_OLDER_THAN(5, 7, 0)
			const ERHIAccess Before = ERHIAccess::SRVMask;
			const ERHIAccess After = ERHIAccess::SRVMask;
#else
			const ERHIAccess Before = ERHIAccess::Present;
			const ERHIAccess After = ERHIAccess::Present;
#endif
			RHICmdList.Transition(FRHITransitionInfo(BackBuffer, Before, ERHIAccess::CopySrc));
			Read.Readback->EnqueueCopy(RHICmdList, BackBuffer, FResolveRect(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y));
			RHICmdList.Transition(FRHITransitionInfo(BackBuffer, ERHIAccess::CopySrc, After));
			Reading.Add(MoveTemp(Read));
		}
	}

	/** The copies the GPU has finished: their pixels go to a pool thread. */
	void Collect()
	{
		for (int32 i = Reading.Num() - 1; i >= 0; --i)
		{
			FRead& Read = Reading[i];
			const bool bGivenUp = Read.Job->State != (int32)EJobState::Copying;
			if (!bGivenUp && !Read.Readback->IsReady())
			{
				continue;
			}
			if (!bGivenUp)
			{
				TArray<uint8> Pixels;
				const int32 Bytes = GPixelFormats[Read.Format].BlockBytes;
				int32 RowPitch = 0;
				int32 Rows = 0;
				const uint8* Data = static_cast<const uint8*>(Read.Readback->Lock(RowPitch, &Rows));
				if (Data && RowPitch >= Read.Size.X)
				{
					const SIZE_T RowBytes = (SIZE_T)Read.Size.X * Bytes;
					Pixels.SetNumUninitialized(RowBytes * Read.Size.Y);
					for (int32 Y = 0; Y < Read.Size.Y; ++Y)
					{
						FMemory::Memcpy(Pixels.GetData() + RowBytes * Y, Data + (SIZE_T)RowPitch * Bytes * Y, RowBytes);
					}
				}
				Read.Readback->Unlock();
				if (Read.Job->Move(EJobState::Copying, EJobState::Encoding))  // (else the game thread gave up on it)
				{
					if (Pixels.Num())
					{
						FMediaJobPtr Job = Read.Job;
						IImageWrapperModule* ImageWrappers = Wrappers;
						const FIntPoint Size = Read.Size;
						const EPixelFormat Format = Read.Format;
						Async(EAsyncExecution::ThreadPool, [Job, ImageWrappers, Pixels = MoveTemp(Pixels), Size, Format]() mutable
						{
							Encode(Job, ImageWrappers, MoveTemp(Pixels), Size, Format);
						});
					}
					else
					{
						Read.Job->Fail(TEXT("no picture: the GPU copy couldn't be read"));
					}
				}
			}
			if (Spares.Num() < MaxSpareReadbacks)
			{
				Read.Job.Reset();
				Spares.Add(MoveTemp(Read));
			}
			Reading.RemoveAt(i);
		}
	}

	/** A readback for a copy of this size and format: one used before (its staging texture is kept), else a new one. */
	TSharedPtr<FRHIGPUTextureReadback> TakeSpare(const FIntPoint& Size, EPixelFormat Format)
	{
		for (int32 i = 0; i < Spares.Num(); ++i)
		{
			if (Spares[i].Size == Size && Spares[i].Format == Format)
			{
				TSharedPtr<FRHIGPUTextureReadback> Readback = Spares[i].Readback;
				Spares.RemoveAt(i);
				return Readback;
			}
		}
		if (Spares.Num() >= MaxSpareReadbacks)
		{
			Spares.RemoveAt(0);  // (the oldest size: the viewport was resized)
		}
		return MakeShared<FRHIGPUTextureReadback>(TEXT("AutopilotCapture"));
	}

	FCriticalSection Lock;
	TArray<FMediaJobPtr> Waiting;  // the game thread adds, the render thread takes
	TArray<FRead> Reading;         // render thread only
	TArray<FRead> Spares;          // render thread only: readbacks free for another copy of their size
};

struct FPlaytestMedia::FJobs
{
	TArray<FMediaJobPtr> InFlight;
	FMediaJobPtr Frame;  // the recording's frame in flight
};

FPlaytestMedia::FPlaytestMedia(UPlaytestSubsystem& InBridge)
	: Bridge(InBridge)
	, Jobs(MakeUnique<FJobs>())
{
}

FPlaytestMedia::~FPlaytestMedia()
{
	Shutdown();
}

bool FPlaytestMedia::CanCapture()
{
	return Renders() && !IsRunningDedicatedServer() && !IsRunningCommandlet();
}

void FPlaytestMedia::Shutdown()
{
	if (Render.IsValid())
	{
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer() && BackBufferHandle.IsValid())
		{
			FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
		}
		BackBufferHandle.Reset();
		TSharedPtr<FPlaytestMediaRender, ESPMode::ThreadSafe> Releasing = Render;
		ENQUEUE_RENDER_COMMAND(AutopilotReleaseCapture)([Releasing](FRHICommandListImmediate&)
		{
			Releasing->Release();
		});
		Render.Reset();
	}
	if (Jobs.IsValid())
	{
		Jobs->InFlight.Reset();
		Jobs->Frame.Reset();
	}
	bRecording = false;
}

bool FPlaytestMedia::Hook()
{
	if (Render.IsValid())
	{
		return true;
	}
	if (!CanCapture() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
	{
		return false;
	}
	Render = MakeShared<FPlaytestMediaRender, ESPMode::ThreadSafe>();
	Render->Wrappers = FModuleManager::LoadModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
	BackBufferHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddSP(Render.ToSharedRef(), &FPlaytestMediaRender::OnBackBuffer);
	return true;
}

FString FPlaytestMedia::FindPicture(FIntRect& OutRect, const SWindow*& OutWindow)
{
	if (!CanCapture())
	{
		return TEXT("no picture: the game runs without rendering (-nullrhi)");
	}
	UWorld* World = Bridge.GetGameWorld();
	if (!World)
	{
		return TEXT("no picture: no game is running");
	}
	UGameViewportClient* Client = World->GetGameViewport();
	TSharedPtr<SViewport> Widget = Client ? Client->GetGameViewportWidget() : nullptr;
	if (!Widget.IsValid())
	{
		return TEXT("no picture: the game has no viewport");
	}
	TSharedPtr<SWindow> Window = Widget == ViewportWidget.Pin() ? ViewportWindow.Pin() : nullptr;
	if (!Window.IsValid())
	{
		Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
		ViewportWidget = Widget;
		ViewportWindow = Window;
	}
	if (!Window.IsValid())
	{
		return TEXT("no picture: the game's viewport isn't in a window");
	}
	if (Window->IsWindowMinimized() || !Window->IsVisible())
	{
		return TEXT("no picture: the game's window is minimized or hidden");
	}
	// The viewport as it was last painted, in its window's pixels (the back buffer's)
	const FGeometry& Geometry = Widget->GetPaintSpaceGeometry();
	const FVector2D Position(Geometry.GetAbsolutePosition());
	const FVector2D Size(Geometry.GetAbsoluteSize());
	OutRect = FIntRect(FMath::RoundToInt(Position.X), FMath::RoundToInt(Position.Y),
		FMath::RoundToInt(Position.X + Size.X), FMath::RoundToInt(Position.Y + Size.Y));
	if (OutRect.Width() < 2 || OutRect.Height() < 2)
	{
		return TEXT("no picture: the game's viewport isn't drawn");
	}
	if (!Hook())
	{
		return TEXT("no picture: the game's window isn't drawn by Slate");
	}
	OutWindow = Window.Get();
	return FString();
}

bool FPlaytestMedia::Start(bool bStill, int64 Number, int32 MaxDim, int32 Quality, FString& OutError)
{
	FIntRect Rect;
	const SWindow* Window = nullptr;
	OutError = FindPicture(Rect, Window);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	FMediaJobPtr Job = MakeShared<FMediaJob, ESPMode::ThreadSafe>();
	Job->bStill = bStill;
	(bStill ? Job->RequestId : Job->N) = Number;
	Job->Generation = Generation;
	Job->Serial = Bridge.GetConnectionSerial();
	Job->T = FMath::Max(0.0, Bridge.GameTime());
	Job->Wall = UnixNow();
	Job->MaxDim = MaxDim;
	Job->Quality = Quality;
	Job->Window = Window;
	Job->Rect = Rect;
	Job->Started = FPlatformTime::Seconds();
	Jobs->InFlight.Add(Job);
	if (!bStill)
	{
		Jobs->Frame = Job;
	}
	Render->Add(Job);
	return true;
}

void FPlaytestMedia::Record(int64 Id, const TSharedPtr<FJsonObject>& Args)
{
	FString Action;
	Args->TryGetStringField(TEXT("action"), Action);
	if (Action == TEXT("stop"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("recording"), false);
		Result->SetNumberField(TEXT("frames"), (double)FramesSent);
		StopRecording();
		Bridge.Respond(Id, MakeShared<FJsonValueObject>(Result));
		return;
	}
	if (Action != TEXT("start"))
	{
		Bridge.RespondError(Id, FString::Printf(TEXT("record needs action \"start\" or \"stop\", not \"%s\""), *Action));
		return;
	}
	double Fps = 5.0, MaxDimArg = 960.0, QualityArg = 75.0;
	Args->TryGetNumberField(TEXT("fps"), Fps);
	Args->TryGetNumberField(TEXT("max_dim"), MaxDimArg);
	Args->TryGetNumberField(TEXT("quality"), QualityArg);
	FIntRect Rect;
	const SWindow* Window = nullptr;
	const FString Problem = FindPicture(Rect, Window);
	if (!Problem.IsEmpty())
	{
		Bridge.RespondError(Id, Problem, 409);
		return;
	}
	StopRecording();  // (a start while recording starts again: frame numbers from 0)
	bRecording = true;
	Interval = 1.0 / FMath::Clamp(Fps, 0.1, 30.0);
	RecordMaxDim = FMath::Clamp((int32)MaxDimArg, 16, 8192);
	RecordQuality = FMath::Clamp((int32)QualityArg, 1, 100);
	NextFrame = 0.0;
	NextN = 0;
	FramesSent = 0;
	RecordSerial = Bridge.GetConnectionSerial();
	const FIntPoint Sent = SentSize(Rect.Size(), RecordMaxDim);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("recording"), true);
	Result->SetNumberField(TEXT("width"), Sent.X);
	Result->SetNumberField(TEXT("height"), Sent.Y);
	Bridge.Respond(Id, MakeShared<FJsonValueObject>(Result));
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Recording the game: %.1f frames a second, %dx%d"), 1.0 / Interval, Sent.X, Sent.Y);
}

void FPlaytestMedia::StopRecording()
{
	if (bRecording)
	{
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Recording stopped: %lld frames sent"), (long long)FramesSent);
	}
	bRecording = false;
	++Generation;  // (frames still in flight are dropped)
	Jobs->Frame.Reset();
}

void FPlaytestMedia::Capture(int64 Id, const TSharedPtr<FJsonObject>& Args)
{
	double MaxDimArg = 1920.0, QualityArg = 90.0;
	Args->TryGetNumberField(TEXT("max_dim"), MaxDimArg);
	Args->TryGetNumberField(TEXT("quality"), QualityArg);
	FString Problem;
	if (!Start(true, Id, FMath::Clamp((int32)MaxDimArg, 16, 8192), FMath::Clamp((int32)QualityArg, 1, 100), Problem))
	{
		Bridge.RespondError(Id, Problem, 409);
	}
	// (else answered once encoded: Tick)
}

void FPlaytestMedia::Tick()
{
	const bool bConnected = Bridge.IsConnected();
	if (bRecording && (!bConnected || Bridge.GetConnectionSerial() != RecordSerial))
	{
		StopRecording();  // (a recording never outlives its connection)
	}
	const double Now = FPlatformTime::Seconds();

	// Jobs that are done, failed, or waited for too long
	TArray<FMediaJobPtr>& InFlight = Jobs->InFlight;
	for (int32 i = 0; i < InFlight.Num(); ++i)
	{
		const FMediaJobPtr Job = InFlight[i];
		if (Now - Job->Started > WaitForDrawSeconds && Job->Move(EJobState::Waiting, EJobState::Failed))
		{
			Job->Error = TEXT("no picture: the game's window wasn't drawn (is it minimized or hidden?)");
		}
		else if (Now - Job->Started > WaitForDrawSeconds + WaitForGpuSeconds && Job->Move(EJobState::Copying, EJobState::Failed))
		{
			Job->Error = TEXT("no picture: the GPU didn't return the picture in time");
		}
		const int32 State = Job->State;
		if (State != (int32)EJobState::Done && State != (int32)EJobState::Failed)
		{
			continue;
		}
		InFlight.RemoveAt(i--);
		const bool bSameConnection = bConnected && Job->Serial == Bridge.GetConnectionSerial();
		if (Job->bStill)
		{
			if (!bSameConnection)
			{
				continue;
			}
			if (State == (int32)EJobState::Done)
			{
				const double GameNow = Bridge.GameTime();
				Bridge.SendRaw(Job->Text + TEXT(",\"t\":") + (GameNow < 0 ? FString(TEXT("null")) : PlaytestJson::Write(PlaytestJson::Number(GameNow))) + TEXT("}"));
			}
			else
			{
				Bridge.RespondError(Job->RequestId, Job->Error, 409);
			}
			continue;
		}
		if (Jobs->Frame == Job)
		{
			Jobs->Frame.Reset();
		}
		if (State == (int32)EJobState::Done && bRecording && bSameConnection && Job->Generation == Generation)
		{
			Bridge.SendRaw(Job->Text);
			++FramesSent;
		}
		else if (State == (int32)EJobState::Failed)
		{
			UE_LOG(LogPlaytest, Verbose, TEXT("[Autopilot] Frame %lld not recorded: %s"), (long long)Job->N, *Job->Error);
		}
	}

	// The recording's next frame: only while the game runs and isn't paused (a paused game's picture doesn't change)
	if (!bRecording)
	{
		return;
	}
	UWorld* World = Bridge.GetGameWorld();
	if (!World || World->IsPaused())
	{
		NextFrame = 0.0;  // (on again: a frame at once, without a gap in the numbers)
		return;
	}
	if (NextFrame <= 0.0)
	{
		NextFrame = Now;
	}
	if (Now < NextFrame)
	{
		return;
	}
	NextFrame += Interval;
	if (NextFrame <= Now)
	{
		NextFrame = Now + Interval;  // (fell behind: no burst to catch up)
	}
	const int64 N = NextN++;
	if (Jobs->Frame.IsValid())
	{
		return;  // the one before is still being read or encoded: this one is skipped (a gap in n)
	}
	FString Problem;
	Start(false, N, RecordMaxDim, RecordQuality, Problem);  // (no picture now: skipped)
}
