// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroBridge.h"
#include "NwiroImageDrop.h"
#include "NwiroToolDispatch.h"
#include "NwiroToolRegistry.h"
#include "NwiroPanel.h"
#include "NwiroAgentReadScope.h"

// Engine — Core
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Json.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include <atomic>
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

// Engine — Editor / Slate
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "SWebBrowser.h"

// Engine — HTTP
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// Engine — Assets
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"

// Engine — Scripting
#include "IPythonScriptPlugin.h"

// Nwiro — Zip extraction (miniz-based, no PowerShell dependency)
#include "ContentPipeline/NwiroZipExtractor.h"

UNwiroBridge* UNwiroBridge::Instance = nullptr;

// Defensive hardening (NOT the crash fix): serialize HDRI bakes so two can't
// run concurrently. The python311 bake crash itself was a per-bake Python state
// problem fixed in the emitter (pythonEmitter.ts: per-bake globals isolation +
// FAssetCompilingManager barriers). This guard just keeps overlapping bakes from
// interleaving editor/Python work. File-static rather than a UNwiroBridge member
// so the lock survives panel close/reopen (which destroys/recreates the bridge
// UObject). One HDRI bake at a time per editor session.
static bool GNwiroHDRIRunning = false;

struct FAdapterInfo { FString Id; FString BinaryName; FString DownloadUrl; FString EnvKey; TArray<FString> ExeCandidates; };
static const FAdapterInfo* FindAdapter(const FString&) { return nullptr; }

static const FAdapterInfo* FindAdapterUnused(const FString& Id)
{
	static TArray<FAdapterInfo> R;
	for (const FAdapterInfo& A : R)
	{
		if (A.Id == Id) return &A;
	}
	return nullptr;
}

static FString GetOptionalStringField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
{
	FString Value;
	return Obj.IsValid() && Obj->TryGetStringField(FieldName, Value) ? Value : TEXT("");
}

static TSharedPtr<FJsonObject> GetOptionalObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
{
	if (!Obj.IsValid()) return nullptr;
	// TryGetField avoids the FString/FSharedString key-type mismatch in UE 5.8
	// while remaining compatible with UE 5.7. Returns TSharedPtr (not pointer-to-TSharedPtr).
	TSharedPtr<FJsonValue> Field = Obj->TryGetField(FieldName);
	return Field.IsValid() ? Field->AsObject() : nullptr;
}

static bool HasTerminalContent(const TSharedPtr<FJsonObject>& Obj)
{
	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (!Obj.IsValid() || !Obj->TryGetArrayField(TEXT("content"), Content)) return false;

	for (const TSharedPtr<FJsonValue>& Item : *Content)
	{
		TSharedPtr<FJsonObject> ContentObj = Item.IsValid() ? Item->AsObject() : nullptr;
		if (!ContentObj.IsValid()) continue;
		if (GetOptionalStringField(ContentObj, TEXT("type")).Equals(TEXT("terminal"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static bool HasShellInputFields(const TSharedPtr<FJsonObject>& Obj)
{
	return Obj.IsValid()
		&& (Obj->HasField(TEXT("command"))
			|| Obj->HasField(TEXT("cwd"))
			|| Obj->HasField(TEXT("terminalId")));
}

static bool LooksLikeShellTitle(const FString& Title)
{
	return Title.Contains(TEXT("terminal/"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("PowerShell"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("cmd.exe"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("bash"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("shell"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Get-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Set-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("New-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Remove-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Start-"), ESearchCase::IgnoreCase)
		|| Title.Equals(TEXT("pwd"), ESearchCase::IgnoreCase)
		|| Title.Equals(TEXT("dir"), ESearchCase::IgnoreCase)
		|| Title.Equals(TEXT("ls"), ESearchCase::IgnoreCase);
}

static FString MakeToolCallKey(const FString& SessionId, const FString& ToolCallId)
{
	if (ToolCallId.IsEmpty()) return TEXT("");
	return SessionId.IsEmpty()
		? ToolCallId
		: FString::Printf(TEXT("%s::%s"), *SessionId, *ToolCallId);
}

static bool IsShellToolCall(const TSharedPtr<FJsonObject>& ToolCall)
{
	if (!ToolCall.IsValid()) return false;

	const FString Kind = GetOptionalStringField(ToolCall, TEXT("kind"));
	if (Kind.Equals(TEXT("execute"), ESearchCase::IgnoreCase)) return true;
	if (HasTerminalContent(ToolCall) || HasShellInputFields(ToolCall)) return true;

	TSharedPtr<FJsonObject> RawInput = GetOptionalObjectField(ToolCall, TEXT("rawInput"));
	if (HasShellInputFields(RawInput)) return true;

	return LooksLikeShellTitle(GetOptionalStringField(ToolCall, TEXT("title")));
}

void UNwiroBridge::Log(const FString& Text)
{
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Text]()
	{
		UE_LOG(LogTemp, Log, TEXT("Nwiro [JS]: %s"), *Text);
	});
}

bool UNwiroBridge::IsTCPRunning() const { return false; }
bool UNwiroBridge::IsProcessing() const
{
	const FChatSession* S = ChatSessions.Find(ActiveChatId);
	return S ? S->bProcessing : false;
}
void UNwiroBridge::SetActiveChat(const FString& ChatId)
{
	ActiveChatId = ChatId;
	if (!ChatId.IsEmpty())
	{
		FChatSession& S = ChatSessions.FindOrAdd(ChatId);
		S.ChatId = ChatId;
		if (S.AdapterId.IsEmpty())
		{
			S.AdapterId = CurrentAdapter;
		}
		else if (S.AdapterId != CurrentAdapter)
		{
			if (!S.SessionId.IsEmpty()) SessionToChatId.Remove(S.SessionId);
			S.AdapterId = CurrentAdapter;
			S.SessionId.Empty();
			S.PendingMessage.Empty();
			S.SessionRpcId = 0;
			S.PromptRpcId = 0;
			S.LastModel.Empty();
			S.SeenToolCallIds.Empty();
		}
		// If adapter is initialized and session not created yet, create it now
		FAdapterProcess* AP = AdapterProcesses.Find(S.AdapterId);
		if (AP && AP->bInitialized && S.SessionId.IsEmpty())
		{
			DoCreateSession(ChatId);
		}
	}
}
void UNwiroBridge::SetMode(const FString& Mode) { CurrentMode = Mode; }
void UNwiroBridge::SetModel(const FString& Model) { CurrentModel = Model; }
FString UNwiroBridge::GetMode() const { return CurrentMode; }
FString UNwiroBridge::GetModel() const { return CurrentModel; }
void UNwiroBridge::SetAdapter(const FString& Adapter) { CurrentAdapter = Adapter; }
FString UNwiroBridge::GetAdapter() const { return CurrentAdapter; }
void UNwiroBridge::AddImagePath(const FString& Path) { PendingImages.Add(Path); }
void UNwiroBridge::ClearImages() { PendingImages.Empty(); }

FChatSession* UNwiroBridge::GetActiveSession()
{
	return ChatSessions.Find(ActiveChatId);
}

FChatSession* UNwiroBridge::FindSessionByAcpId(const FString& AcpSessionId)
{
	FString* ChatId = SessionToChatId.Find(AcpSessionId);
	if (ChatId) return ChatSessions.Find(*ChatId);
	return nullptr;
}

FString UNwiroBridge::FindChatIdByRpcId(int32 RpcId)
{
	FString* ChatId = RpcToChatId.Find(RpcId);
	return ChatId ? *ChatId : ActiveChatId;
}

// ============================================================
// JSON helpers
// ============================================================

FString UNwiroBridge::JsonToString(const TSharedPtr<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
	return Out;
}

// ============================================================
// Response queue (polled by frontend JS)
// ============================================================

void UNwiroBridge::EnqueueResponse(const FString& Type, const FString& Data)
{
	PushEvent(Type, Data, ActiveChatId);
}

void UNwiroBridge::EnqueueResponse(const FString& Type, const FString& Data, const FString& ChatId)
{
	PushEvent(Type, Data, ChatId);
}

FString UNwiroBridge::GetChatIdForToolUse(const FString& ToolUseId) const
{
	const FString* Found = ToolUseToChatId.Find(ToolUseId);
	return Found ? *Found : ActiveChatId;
}

void UNwiroBridge::PushEvent(const FString& Type, const FString& Data, const FString& ChatId)
{
	// Build the JS string on the calling thread (often a background pipe reader)
	// so the game thread only has to hand the ready-made string to CEF.
	FString EscapedData = Data;
	EscapedData.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedData.ReplaceInline(TEXT("'"), TEXT("\\'"));
	EscapedData.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	EscapedData.ReplaceInline(TEXT("\r"), TEXT("\\r"));

	FString EscapedChatId = ChatId;
	EscapedChatId.ReplaceInline(TEXT("'"), TEXT("\\'"));

	FString JSCode = FString::Printf(
		TEXT("window.dispatchEvent(new CustomEvent('ue:response', { detail: { type: '%s', data: '%s', chatId: '%s' } }));"),
		*Type, *EscapedData, *EscapedChatId
	);

	// ExecuteJavascript must run on the game thread (CEF requirement), but
	// the escaping + string formatting above already happened off-thread.
	// If we're already on the game thread, call directly to avoid a frame
	// delay; otherwise dispatch.
	if (IsInGameThread())
	{
		if (FNwiroPanel::WebBrowserWidget.IsValid())
		{
			FNwiroPanel::WebBrowserWidget->ExecuteJavascript(JSCode);
		}
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [JSCode]()
		{
			if (FNwiroPanel::WebBrowserWidget.IsValid())
			{
				FNwiroPanel::WebBrowserWidget->ExecuteJavascript(JSCode);
			}
		});
	}

}

FString UNwiroBridge::PollResponse()
{
	FScopeLock Lock(&ResponseLock);
	if (ResponseQueue.Num() == 0) return TEXT("[]");
	FString Result = TEXT("[");
	for (int32 i = 0; i < ResponseQueue.Num(); ++i)
	{
		if (i > 0) Result += TEXT(",");
		Result += ResponseQueue[i];
	}
	Result += TEXT("]");
	ResponseQueue.Empty();
	return Result;
}

// ============================================================
// Image save (base64 → temp file)
// ============================================================

FString UNwiroBridge::SaveImageBase64(const FString& Base64Data, const FString& FileName)
{
	return NwiroImageDrop::SaveAttachment(Base64Data, FileName,
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroImages")));
}

void UNwiroBridge::OpenUrl(const FString& Url)
{
	if (!Url.IsEmpty()) FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
}

void UNwiroBridge::SetImageDropRegions(const FString& Json)
{
	TArray<FBox2D> Regions;
	if (NwiroImageDrop::ParseRegions(Json, Regions)) NwiroImageDrop::SetRegions(Regions);
}

// Legacy entry point. Stages at pick time because renderers that predate the addressed
// picker skip `source:'picker'` images at send; they rely on this having happened.
void UNwiroBridge::OpenImagePicker() { OpenImagePickerImpl(ActiveChatId, /*bAddressed*/ false); }

// The owner is captured when the button is pressed; resolving it after the modal dialog
// returns would attribute the photo to whichever chat SENT last (ActiveChatId).
void UNwiroBridge::OpenImagePickerForChat(const FString& ChatId)
{
	OpenImagePickerImpl(ChatId, /*bAddressed*/ true);
}

void UNwiroBridge::OpenImagePickerImpl(const FString& ChatId, bool bAddressed)
{
	const FString OwnerChatId = ChatId.IsEmpty() ? ActiveChatId : ChatId;

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform) return;

	TArray<FString> OutFiles;
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;

	bool bOpened = DesktopPlatform->OpenFileDialog(
		ParentWindow,
		TEXT("Select Images"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("Image Files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp"),
		EFileDialogFlags::Multiple,
		OutFiles
	);

	if (bOpened && OutFiles.Num() > 0)
	{
		for (const FString& FilePath : OutFiles)
		{
			// Read file, convert to base64, send to frontend as image_added event
			TArray<uint8> FileData;
			if (FFileHelper::LoadFileToArray(FileData, *FilePath))
			{
				FString Base64 = FBase64::Encode(FileData);
				FString FileName = FPaths::GetCleanFilename(FilePath);
				FString Ext = FPaths::GetExtension(FilePath).ToLower();
				FString MimeType = Ext == TEXT("png") ? TEXT("image/png") :
					Ext == TEXT("jpg") || Ext == TEXT("jpeg") ? TEXT("image/jpeg") :
					TEXT("image/bmp");

				// Build data URL
				FString DataUrl = FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *Base64);

				// Send to frontend
				TSharedPtr<FJsonObject> ImgInfo = MakeShareable(new FJsonObject);
				ImgInfo->SetStringField(TEXT("name"), FileName);
				ImgInfo->SetStringField(TEXT("path"), FilePath);
				ImgInfo->SetStringField(TEXT("preview"), DataUrl);
				// Whether the file is ALREADY staged natively. The addressed picker does not
				// stage: the renderer validates the image, shows it in its tray and stages
				// exactly what is on screen at send. The legacy picker keeps staging for
				// renderers that predate this flag.
				ImgInfo->SetBoolField(TEXT("nativeStaged"), !bAddressed);
				EnqueueResponse(TEXT("image_added"), JsonToString(ImgInfo), OwnerChatId);

				if (!bAddressed) AddImagePath(FilePath);
			}
		}
	}
}

FString UNwiroBridge::GetPluginInfo() const
{
	TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject);
	FString PluginVersion = TEXT("unknown");
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Nwiro"));
	if (Plugin.IsValid()) PluginVersion = Plugin->GetDescriptor().VersionName;
	Info->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Info->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Info->SetStringField(TEXT("mcpProtocol"), TEXT("2025-03-26"));
	Info->SetNumberField(TEXT("mcpPort"), 0);
	Info->SetBoolField(TEXT("mcpRunning"), false);
	Info->SetStringField(TEXT("adapter"), CurrentAdapter);
	Info->SetStringField(TEXT("acpProtocol"), TEXT("ACP v1"));
	TSharedPtr<FJsonObject> Capabilities = MakeShareable(new FJsonObject);
	Capabilities->SetBoolField(TEXT("managedTools"), true);
	Capabilities->SetBoolField(TEXT("providerModes"), true);
	Capabilities->SetBoolField(TEXT("hdriPipeline"), true);
	// The Scene Gen window's render and run-folder tools are on the panel bridge (executetool). The
	// engine (UE 5.6 or later) is checked separately; the app offers the window only when this is
	// true, so a plugin from before these tools (1.1.13 and older) is asked to update instead of
	// failing on Render.
	Capabilities->SetBoolField(TEXT("sceneGeneration"), true);
	Info->SetObjectField(TEXT("capabilities"), Capabilities);
	return JsonToString(Info);
}

// ============================================================
// ACP JSON-RPC send
// ============================================================

void UNwiroBridge::SendRaw(const FString& AdapterId, const FString& Json)
{
	FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
	if (!AP || !AP->Process.IsValid() || !AP->Process->IsRunning()) return;
	// Off by default; enable in console with: Log LogTemp Verbose
	UE_LOG(LogTemp, Verbose, TEXT("ACP send [%s]: %s"), *AdapterId, *Json);
	AP->Process->SendWhenReady(Json + TEXT("\n"));
}

int32 UNwiroBridge::SendRpc(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	int32 Id = NextRpcId++;
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Msg->SetNumberField(TEXT("id"), Id);
	Msg->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
		Msg->SetObjectField(TEXT("params"), Params);
	else
		Msg->SetObjectField(TEXT("params"), MakeShareable(new FJsonObject));
	SendRaw(AdapterId, JsonToString(Msg));
	return Id;
}

void UNwiroBridge::SendRpcNotification(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Msg->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
		Msg->SetObjectField(TEXT("params"), Params);
	SendRaw(AdapterId, JsonToString(Msg));
}

void UNwiroBridge::SendRpcResult(const FString& AdapterId, int32 Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Msg->SetNumberField(TEXT("id"), Id);
	Msg->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShareable(new FJsonObject));
	SendRaw(AdapterId, JsonToString(Msg));
}

// ─── codex-acp-string-id ─────────────────────────────────────────────────────
// Echo the original wire-type id back to the agent. Preserves Number | String |
// Null from the inbound `request` so JSON-RPC matching works on the peer side.
// ─────────────────────────────────────────────────────────────────────────────
void UNwiroBridge::SendRpcResult(const FString& AdapterId, const Acp::FRequestId& ReqId, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (ReqId.Value.IsValid())
		Msg->SetField(TEXT("id"), ReqId.Value);
	else
		Msg->SetField(TEXT("id"), MakeShareable(new FJsonValueNull()));
	Msg->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShareable(new FJsonObject));
	SendRaw(AdapterId, JsonToString(Msg));
}

void UNwiroBridge::SendRpcError(const FString& AdapterId, const Acp::FRequestId& ReqId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (ReqId.Value.IsValid())
		Msg->SetField(TEXT("id"), ReqId.Value);
	else
		Msg->SetField(TEXT("id"), MakeShareable(new FJsonValueNull()));
	TSharedPtr<FJsonObject> ErrObj = MakeShareable(new FJsonObject);
	ErrObj->SetNumberField(TEXT("code"), Code);
	ErrObj->SetStringField(TEXT("message"), Message);
	Msg->SetObjectField(TEXT("error"), ErrObj);
	SendRaw(AdapterId, JsonToString(Msg));
}

// Build the ACP `selected` outcome envelope and ship it back to the adapter.
// Payload shape preserved as `outcome.outcome = "selected"` — this is the live
// adapter contract (claude-code-acp parses response.outcome.outcome). Do not
// rename to `outcome.type` even if a future spec disagrees.
void UNwiroBridge::RespondToAcpPermission(const FString& AdapterId, const Acp::FRequestId& ReqId, const FString& OptionId)
{
	TSharedPtr<FJsonObject> Outcome = MakeShareable(new FJsonObject);
	Outcome->SetStringField(TEXT("outcome"), TEXT("selected"));
	Outcome->SetStringField(TEXT("optionId"), OptionId);

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetObjectField(TEXT("outcome"), Outcome);

	SendRpcResult(AdapterId, ReqId, Result);
}

#if WITH_EDITOR
void UNwiroBridge::DebugInjectFrame(const FString& RawJsonLine)
{
	UE_LOG(LogTemp, Warning, TEXT("Nwiro: DebugInjectFrame: %s"), *RawJsonLine);
	ProcessLine(CurrentAdapter, RawJsonLine);
}
#endif

// ============================================================
// ACP protocol steps
// ============================================================

void UNwiroBridge::DoInitialize()
{

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetNumberField(TEXT("protocolVersion"), 1);

	// Client capabilities
	TSharedPtr<FJsonObject> Caps = MakeShareable(new FJsonObject);
	TSharedPtr<FJsonObject> FsCaps = MakeShareable(new FJsonObject);
	FsCaps->SetBoolField(TEXT("readTextFile"), true);
	FsCaps->SetBoolField(TEXT("writeTextFile"), false);
	Caps->SetObjectField(TEXT("fs"), FsCaps);
	// ACP terminal support is intentionally disabled in Nwiro. Embedded UE
	// adapter sessions wedge too easily when host shell access is exposed, so
	// we remove the capability from the tool surface entirely.
	Caps->SetBoolField(TEXT("terminal"), false);
	Params->SetObjectField(TEXT("clientCapabilities"), Caps);

	// Client info
	TSharedPtr<FJsonObject> ClientInfo = MakeShareable(new FJsonObject);
	ClientInfo->SetStringField(TEXT("name"), TEXT("nwiro"));
	ClientInfo->SetStringField(TEXT("title"), TEXT("Nwiro UE5"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

	int32 RpcId = SendRpc(CurrentAdapter, TEXT("initialize"), Params);
	FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter);
	if (AP) AP->InitRpcId = RpcId;
}

void UNwiroBridge::DoCreateSession(const FString& ChatId)
{
	FChatSession& Session = ChatSessions.FindOrAdd(ChatId);
	Session.ChatId = ChatId;
	if (Session.AdapterId.IsEmpty())
	{
		Session.AdapterId = CurrentAdapter;
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("cwd"), WorkingDir);

	TArray<TSharedPtr<FJsonValue>> McpServers;
	Params->SetArrayField(TEXT("mcpServers"), McpServers);

	// System instructions for all ACP adapters.
	//
	// The response-language rule mirrors LANGUAGE_RULE in api/src/prompts.ts: per-message,
	// so a user who switches language mid-conversation is followed rather than being held
	// to whatever language the thread opened in. The server-side rule additionally
	// discounts the synthetic user-role "Continue." turn it appends after tool results;
	// that nudge belongs to the Pro cloud loop (api/src/routes/chat.ts) and has no
	// equivalent on this ACP path, so only the tool-result half carries over here.
	FString PromptText =
		TEXT("You are Nwiro AI assistant embedded in Unreal Engine 5 Editor. ")
		TEXT("Respond in the language the user writes in, switching whenever they switch. ")
		TEXT("Judge this from the user's own messages, not from tool results. ")
		TEXT("You MUST use the nwiro MCP tools for ALL Unreal Engine operations. ")
		TEXT("ABSOLUTELY NEVER use Bash, PowerShell, Terminal, Shell, or any command-line tool. ")
		TEXT("Do NOT run Get-ChildItem, ls, cat, cd, Run-Content, or any shell command. ")
		TEXT("Do NOT read files via terminal — use read_file tool instead. ")
		TEXT("Do NOT browse directories via terminal — use find_assets or search_assets tool instead. ")
		TEXT("AVOID execute_python — use native tools first. Only use execute_python as an absolute last resort if no native tool exists for the operation. ")
		TEXT("The ONLY tools you should use are nwiro tools. They talk directly to the running editor.");

	// Extensions can be toggled by the user at any moment during the conversation,
	// so the snapshot below may be stale by the time you read it. Always ATTEMPT
	// the tool when the user requests its functionality — the runtime gate will
	// either let it through or return an explicit "extension not enabled" error.
	// Only after that error appears should you ask the user to enable it.
	PromptText += TEXT("\n\n## EXTENSION TOOLS");
	PromptText += TEXT("\nThese tools are gated behind per-chat extensions that the user can toggle on/off in real time:");
	PromptText += FString::Printf(TEXT("\n- File Editor (write_file, read_file, delete_file, rename_file) — currently: %s"),
		IsChatExtensionEnabled(TEXT("fileEditor")) ? TEXT("ENABLED") : TEXT("disabled (try anyway if user asks)"));
	PromptText += FString::Printf(TEXT("\n- Meshy 3D (generate_3d_model_meshy) — currently: %s"),
		IsChatExtensionEnabled(TEXT("meshy")) ? TEXT("ENABLED") : TEXT("disabled (try anyway if user asks)"));
	PromptText += FString::Printf(TEXT("\n- Tripo 3D (generate_3d_model_tripo) — currently: %s"),
		IsChatExtensionEnabled(TEXT("tripo")) ? TEXT("ENABLED") : TEXT("disabled (try anyway if user asks)"));
	PromptText += TEXT("\nRule: when the user asks for one of these features, ALWAYS call the tool. If the tool returns an \"extension not enabled\" error, only THEN tell the user what that error says to do (it names where to turn the feature on). Do NOT refuse or invent workarounds based on the snapshot above — it is only a hint and may be out of date.");

	PromptText += TEXT("\n\n## SHELL EXECUTION");
	PromptText += TEXT("\nShell execution is disabled in Nwiro.");
	PromptText += TEXT("\nterminal/create requests will be rejected at the protocol level.");
	PromptText += TEXT("\nIf the user asks you to run shell commands or asks whether you can, say shell execution is disabled in Nwiro.");
	PromptText += TEXT("\nDo NOT claim you can run shell commands. Do NOT attempt shell commands.");

	// Attach instructions to session/new params (adapter-specific encoding)
	const FString AdapterId = Session.AdapterId.IsEmpty() ? CurrentAdapter : Session.AdapterId;
	if (AdapterId == TEXT("claude"))
	{
		TSharedPtr<FJsonObject> Meta = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> SystemPrompt = MakeShareable(new FJsonObject);
		SystemPrompt->SetStringField(TEXT("append"), PromptText);
		Meta->SetObjectField(TEXT("systemPrompt"), SystemPrompt);
		Params->SetObjectField(TEXT("_meta"), Meta);
	}
	else if (AdapterId == TEXT("codex"))
	{
		Params->SetStringField(TEXT("instructions"), PromptText);
	}

	Session.SessionRpcId = SendRpc(AdapterId, TEXT("session/new"), Params);
	RpcToChatId.Add(Session.SessionRpcId, ChatId);
}

void UNwiroBridge::DoSendPrompt(const FString& ChatId, const FString& Message)
{
	FChatSession* Session = ChatSessions.Find(ChatId);
	if (!Session || Session->SessionId.IsEmpty()) return;
	const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;

	Session->bProcessing = true;
	// Ensure session mapping exists
	if (!SessionToChatId.Contains(Session->SessionId))
		SessionToChatId.Add(Session->SessionId, ChatId);

	// Set model via config option — skip "default" (adapter uses its own default)
	if (!CurrentModel.IsEmpty() && CurrentModel != TEXT("default") && CurrentModel != Session->LastModel)
	{
		Session->LastModel = CurrentModel;
		TSharedPtr<FJsonObject> ModelParams = MakeShareable(new FJsonObject);
		ModelParams->SetStringField(TEXT("sessionId"), Session->SessionId);
		ModelParams->SetStringField(TEXT("configId"), TEXT("model"));
		ModelParams->SetStringField(TEXT("value"), CurrentModel);
		SendRpc(AdapterId, TEXT("session/set_config_option"), ModelParams);
	}

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("sessionId"), Session->SessionId);

	TArray<TSharedPtr<FJsonValue>> Prompt;

	for (const FString& ImgPath : PendingImages)
	{
		TArray<uint8> FileData;
		if (FFileHelper::LoadFileToArray(FileData, *ImgPath))
		{
			// Derive the MIME type from the file extension instead of hardcoding
			// image/png — a JPEG/BMP labelled image/png can be rejected or
			// mis-decoded by a strict vision backend once the shim forwards it.
			const FString Ext = FPaths::GetExtension(ImgPath).ToLower();
			const FString MimeType =
				Ext == TEXT("png") ? TEXT("image/png") :
				(Ext == TEXT("jpg") || Ext == TEXT("jpeg")) ? TEXT("image/jpeg") :
				Ext == TEXT("webp") ? TEXT("image/webp") :
				Ext == TEXT("gif") ? TEXT("image/gif") :
				TEXT("image/bmp");
			TSharedPtr<FJsonObject> ImgBlock = MakeShareable(new FJsonObject);
			ImgBlock->SetStringField(TEXT("type"), TEXT("image"));
			ImgBlock->SetStringField(TEXT("data"), FBase64::Encode(FileData));
			ImgBlock->SetStringField(TEXT("mimeType"), MimeType);
			Prompt.Add(MakeShareable(new FJsonValueObject(ImgBlock)));
		}
	}
	PendingImages.Empty();

	FString PromptText = Message;
	PromptText = TEXT("[SHELL DISABLED] Shell execution is disabled in Nwiro. ")
		TEXT("If the user asks whether you can run shell commands, say shell execution is disabled in Nwiro. ")
		TEXT("Do not claim you can run shell commands.")
		TEXT("\n\n---\nUser message:\n") + PromptText;

	TSharedPtr<FJsonObject> TextBlock = MakeShareable(new FJsonObject);
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), PromptText);
	Prompt.Add(MakeShareable(new FJsonValueObject(TextBlock)));
	Params->SetArrayField(TEXT("prompt"), Prompt);

	Session->PromptRpcId = SendRpc(AdapterId, TEXT("session/prompt"), Params);
	RpcToChatId.Add(Session->PromptRpcId, ChatId);
}

// ============================================================
// Process stdout — parse ACP messages
// ============================================================

void UNwiroBridge::ProcessLine(const FString& AdapterId, const FString& Line)
{
	if (Line.IsEmpty()) return;
	// Off by default; enable in console with: Log LogTemp Verbose
	UE_LOG(LogTemp, Verbose, TEXT("ACP recv: %s"), *Line);

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	// Check if frame carries an id (response, error, or agent-originated request)
	TSharedPtr<FJsonValue> IdVal = Acp::ExtractRpcId(Json);
	if (IdVal.IsValid())
	{
		const bool bIsResponse = Json->HasField(TEXT("result")) || Json->HasField(TEXT("error"));
		if (bIsResponse)
		{
			// Responses match against bridge-minted numeric ids in RpcToChatId.
			// If the peer ever sends a non-numeric response id we cannot match
			// it anyway; log and drop rather than silently coercing to 0.
			if (IdVal->Type != EJson::Number)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro: dropping ACP response with non-numeric id (type=%d) — bridge only mints numeric ids"),
					(int32)IdVal->Type);
				return;
			}
			int32 Id = (int32)IdVal->AsNumber();
			if (Json->HasField(TEXT("result")))
				HandleRpcResponse(Id, Json->GetObjectField(TEXT("result")), nullptr);
			else
				HandleRpcResponse(Id, nullptr, Json->GetObjectField(TEXT("error")));
			return;
		}

		// Agent-originated request (id + method). The id may be Number | String
		// | Null per JSON-RPC 2.0 — HandleMethod re-extracts via Acp::ExtractRpcId
		// to round-trip the original wire type on the response.
		if (Json->HasField(TEXT("method")))
		{
			FString Method = Json->GetStringField(TEXT("method"));
			HandleMethod(AdapterId, Method, Json);
			return;
		}
	}

	// Notification from agent (has "method" but no "id")
	if (Json->HasField(TEXT("method")))
	{
		FString Method = Json->GetStringField(TEXT("method"));
		HandleMethod(AdapterId, Method, Json);
	}
}

void UNwiroBridge::HandleRpcResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error)
{
	FString ChatId = FindChatIdByRpcId(Id);
	RpcToChatId.Remove(Id);

	if (Error.IsValid())
	{
		FString ErrMsg = Error->HasField(TEXT("message")) ? Error->GetStringField(TEXT("message")) : TEXT("Unknown error");
		UE_LOG(LogTemp, Error, TEXT("Nwiro: ACP error (id=%d, chat=%s): %s"), Id, *ChatId, *ErrMsg);

		// Only push error to UI for critical RPCs (session/new, prompt)
		// Non-critical RPCs (set_mode etc.) just log
		bool bIsCritical = false;
		FChatSession* Session = ChatId.IsEmpty() ? nullptr : ChatSessions.Find(ChatId);
		if (Session)
		{
			if (Session->SessionRpcId == Id || Session->PromptRpcId == Id)
				bIsCritical = true;
		}

		if (bIsCritical)
		{
			EnqueueResponse(TEXT("error"), ErrMsg, ChatId);
			if (Session && Session->PromptRpcId == Id)
			{
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro: Non-critical ACP error ignored (id=%d): %s"), Id, *ErrMsg);
		}
		return;
	}

	// Initialize response — check all adapter processes
	bool isInitResponse = false;
	FString InitializedAdapter;
	for (auto& APair : AdapterProcesses)
	{
		if (APair.Value.InitRpcId == Id)
		{
			APair.Value.bInitialized = true;
			isInitResponse = true;
			InitializedAdapter = APair.Key;
			break;
		}
	}
	if (isInitResponse)
	{

		// Create sessions for any chats that were waiting (or just opened)
		for (auto& Pair : ChatSessions)
		{
			if (Pair.Value.AdapterId == InitializedAdapter && Pair.Value.SessionId.IsEmpty())
				DoCreateSession(Pair.Key);
		}
		return;
	}

	// Session/new response — find which chat this was for
	for (auto& Pair : ChatSessions)
	{
		if (Pair.Value.SessionRpcId == Id)
		{
			FString NewSessionId = Result.IsValid() && Result->HasField(TEXT("sessionId"))
				? Result->GetStringField(TEXT("sessionId")) : TEXT("");
			Pair.Value.SessionId = NewSessionId;
			SessionToChatId.Add(NewSessionId, Pair.Key);
			const FString AdapterId = Pair.Value.AdapterId.IsEmpty() ? CurrentAdapter : Pair.Value.AdapterId;

			// Set mode to auto/full (some adapters start in read-only).
			// Claude's ACP implementation rejects session/set_mode with
			// `Internal error`, so we skip the call entirely for it
			// instead of spamming the log with non-critical errors.
			if (AdapterId != TEXT("claude"))
			{
				TSharedPtr<FJsonObject> ModeParams = MakeShareable(new FJsonObject);
				ModeParams->SetStringField(TEXT("sessionId"), NewSessionId);
				ModeParams->SetStringField(TEXT("modeId"), TEXT("auto"));
				SendRpc(AdapterId, TEXT("session/set_mode"), ModeParams);
			}


			if (!Pair.Value.PendingMessage.IsEmpty())
			{
				FString Msg = Pair.Value.PendingMessage;
				Pair.Value.PendingMessage.Empty();
				DoSendPrompt(Pair.Key, Msg);
			}
			return;
		}
	}

	// Prompt response (done) — find which chat
	for (auto& Pair : ChatSessions)
	{
		if (Pair.Value.PromptRpcId == Id)
		{
			Pair.Value.bProcessing = false;
			EnqueueResponse(TEXT("done"), TEXT(""), Pair.Key);
			return;
		}
	}
}

void UNwiroBridge::HandleMethod(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& FullMsg)
{
	TSharedPtr<FJsonObject> Params = FullMsg->HasField(TEXT("params"))
		? FullMsg->GetObjectField(TEXT("params")) : nullptr;
	// Union-aware id read (codex-acp-string-id fix). Replaces the prior
	// `(int32)GetNumberField("id")` which logged a `String used as a Number`
	// LogJson error and produced 0 whenever codex emitted a string id.
	//
	// JSON-RPC 2.0: `"id": null` is a request that REQUIRES a response with id
	// echoed as null — distinct from a missing `id` field (notification, no
	// response). HasId reflects field-present, not field-non-null; the new
	// SendRpcResult overload round-trips the null wire value verbatim.
	Acp::FRequestId ReqId(Acp::ExtractRpcId(FullMsg));
	const bool HasId = ReqId.IsValid();

	// session/update — streaming content (includes sessionId for routing)
	if (Method == TEXT("session/update") && Params.IsValid())
	{
		FString AcpSessionId = Params->HasField(TEXT("sessionId"))
			? Params->GetStringField(TEXT("sessionId")) : TEXT("");
		TSharedPtr<FJsonObject> Update = Params->HasField(TEXT("update"))
			? Params->GetObjectField(TEXT("update")) : nullptr;
		if (Update.IsValid()) HandleSessionUpdate(AcpSessionId, Update);
		return;
	}

	// session/request_permission — permission popup
	if (Method == TEXT("session/request_permission") && Params.IsValid())
	{
		// Extract options from ACP
		TArray<TSharedPtr<FJsonValue>> OptArr;
		if (Params->HasField(TEXT("options")))
			OptArr = Params->GetArrayField(TEXT("options"));

		// ── Mode-based auto-response ──

		// Helper: find first option matching a kind
		auto FindOption = [&](const FString& KindMatch) -> FString {
			for (const auto& OptVal : OptArr) {
				TSharedPtr<FJsonObject> Opt = OptVal->AsObject();
				if (!Opt.IsValid()) continue;
				FString Kind = Opt->HasField(TEXT("kind")) ? Opt->GetStringField(TEXT("kind")) : TEXT("");
				if (Kind.Contains(KindMatch, ESearchCase::IgnoreCase)) return Opt->GetStringField(TEXT("optionId"));
			}
			return TEXT("");
		};

		// Bypass Mode → auto-approve all permission prompts
		// Shell safety outranks mode-level auto-approval.
		TSharedPtr<FJsonObject> PermissionToolCall = GetOptionalObjectField(Params, TEXT("toolCall"));
		FString PermissionToolCallId = GetOptionalStringField(PermissionToolCall, TEXT("toolCallId"));
		FString PermissionSessionId = GetOptionalStringField(Params, TEXT("sessionId"));
		FString PermissionToolCallKey = MakeToolCallKey(PermissionSessionId, PermissionToolCallId);
		const bool bShellPermission =
			IsShellToolCall(PermissionToolCall)
			|| (!PermissionToolCallKey.IsEmpty() && BlockedShellToolCallIds.Contains(PermissionToolCallKey));

		if (bShellPermission)
		{
			FString Opt = FindOption(TEXT("reject"));
			if (Opt.IsEmpty()) Opt = FindOption(TEXT("deny"));
			if (Opt.IsEmpty()) Opt = TEXT("deny");

			FString Title = GetOptionalStringField(PermissionToolCall, TEXT("title"));
			if (Title.IsEmpty()) Title = PermissionToolCallId.IsEmpty() ? TEXT("(unknown)") : PermissionToolCallId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro: BLOCKED shell permission request '%s' (shell disabled)"), *Title);
			RespondToAcpPermission(AdapterId, ReqId, Opt);

			FString PermissionChatId = !PermissionSessionId.IsEmpty()
				? SessionToChatId.FindRef(PermissionSessionId) : TEXT("");
			if (PermissionChatId.IsEmpty() && !PermissionToolCallId.IsEmpty())
				PermissionChatId = ToolUseToChatId.FindRef(PermissionToolCallId);
			if (PermissionChatId.IsEmpty())
				PermissionChatId = ActiveChatId;

			if (!PermissionChatId.IsEmpty())
			{
				if (FChatSession* PermissionSession = ChatSessions.Find(PermissionChatId); PermissionSession && PermissionSession->bProcessing)
				{
					if (!PermissionSessionId.IsEmpty())
					{
						TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
						CancelParams->SetStringField(TEXT("sessionId"), PermissionSessionId);
						SendRpcNotification(AdapterId, TEXT("session/cancel"), CancelParams);
					}
					PermissionSession->bProcessing = false;
					EnqueueResponse(TEXT("done"), TEXT(""), PermissionChatId);
				}
			}
			return;
		}

		// Bypass Mode: auto-approve non-shell permission prompts.
		if (CurrentMode == TEXT("bypassPermissions"))
		{
			FString Opt = FindOption(TEXT("allow"));
			if (Opt.IsEmpty()) Opt = FindOption(TEXT("approve"));
			if (Opt.IsEmpty() && OptArr.Num() > 0) Opt = OptArr[0]->AsObject()->GetStringField(TEXT("optionId"));
			RespondToAcpPermission(AdapterId, ReqId, Opt);
			return;
		}

		// Accept Edits → auto-approve all (edits are the main use case)
		if (CurrentMode == TEXT("acceptEdits"))
		{
			FString Opt = FindOption(TEXT("allow"));
			if (Opt.IsEmpty()) Opt = FindOption(TEXT("approve"));
			if (Opt.IsEmpty() && OptArr.Num() > 0) Opt = OptArr[0]->AsObject()->GetStringField(TEXT("optionId"));
			RespondToAcpPermission(AdapterId, ReqId, Opt);
			return;
		}

		// Plan Mode → auto-deny all tool calls
		if (CurrentMode == TEXT("plan"))
		{
			FString Opt = FindOption(TEXT("reject"));
			if (Opt.IsEmpty()) Opt = FindOption(TEXT("deny"));
			if (Opt.IsEmpty()) Opt = TEXT("deny");
			RespondToAcpPermission(AdapterId, ReqId, Opt);

			// Notify user
			FString ChatId = ActiveChatId;
			if (!ChatId.IsEmpty())
				EnqueueResponse(TEXT("system"), TEXT("Plan mode: tool execution blocked."), ChatId);

			return;
		}

		// Don't Ask → auto-deny (no prompts, if not pre-allowed then deny)
		if (CurrentMode == TEXT("dontAsk"))
		{
			FString Opt = FindOption(TEXT("reject"));
			if (Opt.IsEmpty()) Opt = FindOption(TEXT("deny"));
			if (Opt.IsEmpty()) Opt = TEXT("deny");
			RespondToAcpPermission(AdapterId, ReqId, Opt);
			return;
		}

		// Default → show permission card to user.
		// Mint a local int32 id for the React contract and remember the original
		// ACP id (which may be string/null on the wire) so we can echo it back
		// verbatim when the user clicks Allow/Deny. Negative range keeps ACP
		// ids permanently disjoint from any positive-range permission system.
		if (bPanelDetached)
		{
			// No renderer can answer while the panel is closed: withdraw the request
			// instead of parking a card nobody sees.
			CancelAcpPermission(AdapterId, ReqId);
			return;
		}
		const int32 LocalPermId = NextLocalPermId--;
		LocalPermIdToAcpReqId.Add(LocalPermId, ReqId);
		LocalPermIdToAdapterId.Add(LocalPermId, AdapterId);

		TSharedPtr<FJsonObject> PermJson = MakeShareable(new FJsonObject);
		PermJson->SetNumberField(TEXT("id"), LocalPermId);

		if (Params->HasField(TEXT("toolCall")))
		{
			TSharedPtr<FJsonObject> ToolCall = Params->GetObjectField(TEXT("toolCall"));
			PermJson->SetStringField(TEXT("toolName"),
				ToolCall->HasField(TEXT("title")) ? ToolCall->GetStringField(TEXT("title")) : TEXT("Unknown"));
		}

		TArray<TSharedPtr<FJsonValue>> OutOpts;
		for (const auto& OptVal : OptArr)
		{
			TSharedPtr<FJsonObject> Opt = OptVal->AsObject();
			if (!Opt.IsValid()) continue;
			TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
			O->SetStringField(TEXT("optionId"), Opt->GetStringField(TEXT("optionId")));
			O->SetStringField(TEXT("name"), Opt->GetStringField(TEXT("name")));
			O->SetStringField(TEXT("kind"), Opt->HasField(TEXT("kind")) ? Opt->GetStringField(TEXT("kind")) : TEXT(""));
			OutOpts.Add(MakeShareable(new FJsonValueObject(O)));
		}
		PermJson->SetArrayField(TEXT("options"), OutOpts);

		EnqueueResponse(TEXT("permission_request"), JsonToString(PermJson));
		return;
	}

	// fs/read_text_file — agent wants to read a file. The bridge answers without a permission
	// card, so the path is checked before anything is read (NwiroAgentReadScope.h): the project
	// directory or the engine installation only, after following every junction and symlink, and
	// only the path the check returns is opened. This handler used to read ANY path on the
	// machine (release gate B1, 2026-09-08).
	if (Method == TEXT("fs/read_text_file") && HasId && Params.IsValid())
	{
		FString FilePath = Params->HasField(TEXT("path")) ? Params->GetStringField(TEXT("path")) : TEXT("");
		FString ResolvedPath;
		const NwiroAgentReadScope::EResult ReadDecision = NwiroAgentReadScope::Resolve(FilePath, FPaths::ProjectDir(), FPaths::EngineDir(), ResolvedPath);
		if (ReadDecision == NwiroAgentReadScope::EResult::NotFound)
		{
			SendRpcError(AdapterId, ReqId, -32002, FString::Printf(TEXT("File not found: %s"), *FilePath));
			return;
		}
		if (ReadDecision != NwiroAgentReadScope::EResult::Allowed)
		{
			SendRpcError(AdapterId, ReqId, -32000,
				TEXT("Path not allowed. Reads are limited to the project directory and the engine installation."));
			return;
		}
		auto ReadIntParam = [&](const TCHAR* Key) -> int32
		{
			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
			if (!Value.IsValid()) return 0;
			if (Value->Type == EJson::Number) return static_cast<int32>(Value->AsNumber());
			if (Value->Type == EJson::String) return FCString::Atoi(*Value->AsString());
			return 0;
		};
		int32 Line = ReadIntParam(TEXT("line"));
		int32 Limit = ReadIntParam(TEXT("limit"));

		FString Content;
		if (FFileHelper::LoadFileToString(Content, *ResolvedPath))
		{
			// Apply line/limit if specified
			if (Line > 0 || Limit > 0)
			{
				TArray<FString> Lines;
				Content.ParseIntoArrayLines(Lines);
				int32 Start = FMath::Max(0, Line - 1);
				int32 End = Limit > 0 ? FMath::Min(Start + Limit, Lines.Num()) : Lines.Num();
				Content.Empty();
				for (int32 i = Start; i < End; ++i)
				{
					Content += Lines[i];
					if (i < End - 1) Content += TEXT("\n");
				}
			}

			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			Res->SetStringField(TEXT("content"), Content);
			SendRpcResult(AdapterId, ReqId, Res);
		}
		else
		{
			SendRpcError(AdapterId, ReqId, -32002, FString::Printf(TEXT("File not found: %s"), *FilePath));
		}
		return;
	}

	// terminal/create — agent wants to run a command
	if (Method == TEXT("terminal/create") && HasId && Params.IsValid())
	{
		FString Command = Params->HasField(TEXT("command"))
			? Params->GetStringField(TEXT("command")) : TEXT("(unknown)");
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro: BLOCKED terminal/create — command='%s' (shell disabled)"), *Command);

		SendRpcError(AdapterId, ReqId, -32002,
			TEXT("Shell execution is disabled in Nwiro. Use nwiro MCP tools instead."));

		if (!ActiveChatId.IsEmpty())
		{
			EnqueueResponse(TEXT("system"),
				FString::Printf(TEXT("Blocked shell command: %s"), *Command),
				ActiveChatId);
		}
		return;
	}

	// terminal/output
	if (Method == TEXT("terminal/output") && HasId)
	{
		if (bBlockCommandExecution) { SendRpcResult(AdapterId, ReqId, MakeShareable(new FJsonObject)); return; }
		const FString TermId = Params.IsValid() && Params->HasField(TEXT("terminalId"))
			? Params->GetStringField(TEXT("terminalId")) : TEXT("");
		const FTerminalResult* TerminalResult = TerminalResults.Find(TermId);
		TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
		Res->SetStringField(TEXT("output"), TerminalResult ? TerminalResult->Output : TEXT(""));
		Res->SetBoolField(TEXT("truncated"), false);
		TSharedPtr<FJsonObject> ExitStatus = MakeShareable(new FJsonObject);
		ExitStatus->SetNumberField(TEXT("exitCode"), TerminalResult ? TerminalResult->ExitCode : 0);
		Res->SetObjectField(TEXT("exitStatus"), ExitStatus);
		SendRpcResult(AdapterId, ReqId, Res);
		return;
	}

	// terminal/wait_for_exit, terminal/kill, terminal/release
	if ((Method == TEXT("terminal/wait_for_exit") || Method == TEXT("terminal/kill") || Method == TEXT("terminal/release")) && HasId)
	{
		if (bBlockCommandExecution) { SendRpcResult(AdapterId, ReqId, MakeShareable(new FJsonObject)); return; }
		const FString TermId = Params.IsValid() && Params->HasField(TEXT("terminalId"))
			? Params->GetStringField(TEXT("terminalId")) : TEXT("");
		TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
		if (Method == TEXT("terminal/wait_for_exit"))
		{
			const FTerminalResult* TerminalResult = TerminalResults.Find(TermId);
			Res->SetNumberField(TEXT("exitCode"), TerminalResult ? TerminalResult->ExitCode : 0);
		}
		else if (Method == TEXT("terminal/release"))
		{
			TerminalResults.Remove(TermId);
		}
		SendRpcResult(AdapterId, ReqId, Res);
		return;
	}

	// mcp/connect — agent wants to connect to our in-process MCP server
	if (Method == TEXT("mcp/connect") && HasId && Params.IsValid())
	{
		FString AcpId = Params->HasField(TEXT("acpId")) ? Params->GetStringField(TEXT("acpId")) : TEXT("");

		TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
		Res->SetStringField(TEXT("connectionId"), TEXT("nwiro-mcp-conn"));
		SendRpcResult(AdapterId, ReqId, Res);
		return;
	}

	// mcp/message — agent sends MCP JSON-RPC messages to our in-process server
	if (Method == TEXT("mcp/message") && HasId && Params.IsValid())
	{
		TSharedPtr<FJsonObject> McpMsg = Params->HasField(TEXT("message"))
			? Params->GetObjectField(TEXT("message")) : nullptr;

		if (McpMsg.IsValid())
		{
			// MCP server removed — return an error response
			TSharedPtr<FJsonObject> ErrObj = MakeShareable(new FJsonObject);
			ErrObj->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			TSharedPtr<FJsonObject> ErrDetail = MakeShareable(new FJsonObject);
			ErrDetail->SetNumberField(TEXT("code"), -32603);
			ErrDetail->SetStringField(TEXT("message"), TEXT("MCP server not available"));
			ErrObj->SetObjectField(TEXT("error"), ErrDetail);

			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			Res->SetObjectField(TEXT("message"), ErrObj);
			SendRpcResult(AdapterId, ReqId, Res);
		}
		else
		{
			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			SendRpcResult(AdapterId, ReqId, Res);
		}
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("Nwiro: Unhandled ACP method: %s"), *Method);
}

void UNwiroBridge::HandleSessionUpdate(const FString& AcpSessionId, const TSharedPtr<FJsonObject>& Update)
{
	// Resolve chatId from ACP sessionId
	FString ChatId;
	FString* FoundChatId = SessionToChatId.Find(AcpSessionId);
	if (FoundChatId)
	{
		ChatId = *FoundChatId;
	}
	else
	{
		// Session not mapped yet — find by checking which chat is waiting for this session
		for (auto& Pair : ChatSessions)
		{
			if (Pair.Value.bProcessing && Pair.Value.SessionId.IsEmpty())
			{
				ChatId = Pair.Key;
				// Also set the mapping now so future events are routed correctly
				if (!AcpSessionId.IsEmpty())
				{
					Pair.Value.SessionId = AcpSessionId;
					SessionToChatId.Add(AcpSessionId, Pair.Key);
				}
				break;
			}
		}
		if (ChatId.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro: Could not resolve chatId for session %s, falling back to active"), *AcpSessionId);
			ChatId = ActiveChatId;
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("Nwiro: SessionUpdate routed to chat %s (session %s)"), *ChatId, *AcpSessionId);

	FChatSession* Session = ChatSessions.Find(ChatId);

	FString UpdateType = Update->HasField(TEXT("sessionUpdate"))
		? Update->GetStringField(TEXT("sessionUpdate")) : TEXT("");

	if (UpdateType == TEXT("user_message_chunk")) return;

	if (UpdateType == TEXT("agent_message_chunk"))
	{
		if (Update->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = Update->GetObjectField(TEXT("content"));
			FString ContentType = Content->HasField(TEXT("type")) ? Content->GetStringField(TEXT("type")) : TEXT("");
			if (ContentType == TEXT("text") && Content->HasField(TEXT("text")))
				EnqueueResponse(TEXT("stream"), Content->GetStringField(TEXT("text")), ChatId);
		}
		return;
	}

	if (UpdateType == TEXT("agent_thought_chunk"))
	{
		EnqueueResponse(TEXT("thinking"), TEXT(""), ChatId);
		return;
	}

	if (UpdateType == TEXT("tool_call"))
	{

		// Map toolCallId → chatId for MCP permission routing
		FString ToolCallIdForMap = Update->HasField(TEXT("toolCallId")) ? Update->GetStringField(TEXT("toolCallId")) : TEXT("");
		if (!ToolCallIdForMap.IsEmpty()) ToolUseToChatId.Add(ToolCallIdForMap, ChatId);

		FString ToolCallId = Update->HasField(TEXT("toolCallId")) ? Update->GetStringField(TEXT("toolCallId")) : TEXT("");
		FString ToolStatus = Update->HasField(TEXT("status")) ? Update->GetStringField(TEXT("status")) : TEXT("");

		if (IsShellToolCall(Update))
		{
			if (!ToolCallId.IsEmpty()) BlockedShellToolCallIds.Add(MakeToolCallKey(AcpSessionId, ToolCallId));
			FString Title = GetOptionalStringField(Update, TEXT("title"));
			if (Title.IsEmpty()) Title = ToolCallId.IsEmpty() ? TEXT("(unknown)") : ToolCallId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro: BLOCKED shell tool_call '%s' before UI display"), *Title);
			if (!ChatId.IsEmpty())
			{
				EnqueueResponse(TEXT("system"),
					FString::Printf(TEXT("Blocked shell command: %s"), *Title),
					ChatId);
			}
			if (Session && Session->bProcessing)
			{
				if (!AcpSessionId.IsEmpty())
				{
					const FString ChatAdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
					TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
					CancelParams->SetStringField(TEXT("sessionId"), AcpSessionId);
					SendRpcNotification(ChatAdapterId, TEXT("session/cancel"), CancelParams);
				}
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
			}
			return;
		}

		// Skip if already seen OR if it's a status update (not initial "pending")
		if (Session)
		{
			if (!ToolCallId.IsEmpty() && Session->SeenToolCallIds.Contains(ToolCallId)) return;
			if (!ToolCallId.IsEmpty()) Session->SeenToolCallIds.Add(ToolCallId);
		}
		// Skip completed/failed tool_call events (they're final status updates)
		if (ToolStatus == TEXT("completed") || ToolStatus == TEXT("failed")) return;

		FString Title = Update->HasField(TEXT("title")) ? Update->GetStringField(TEXT("title")) : TEXT("");
		// Generic: extract tool name after last "/" or "__" separator
		int32 SlashIdx = INDEX_NONE;
		Title.FindLastChar('/', SlashIdx);
		if (SlashIdx != INDEX_NONE)
			Title = Title.Mid(SlashIdx + 1);
		// Also handle "__" separator (e.g. "mcp__nwiro__create_blueprint")
		int32 DunderIdx = Title.Find(TEXT("__"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (DunderIdx != INDEX_NONE)
			Title = Title.Mid(DunderIdx + 2);
		Title.ReplaceInline(TEXT("_"), TEXT(" "));

		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
		Data->SetStringField(TEXT("name"), Title);
		Data->SetStringField(TEXT("id"), ToolCallId);
		EnqueueResponse(TEXT("tool_start"), JsonToString(Data), ChatId);

		// Extract rawInput if present
		if (Update->HasField(TEXT("rawInput")))
		{
			TSharedPtr<FJsonObject> RawInput = Update->GetObjectField(TEXT("rawInput"));
			// Use "arguments" sub-object if present, otherwise whole rawInput
			FString InputStr;
			if (RawInput.IsValid() && RawInput->HasField(TEXT("arguments")))
				InputStr = JsonToString(RawInput->GetObjectField(TEXT("arguments")));
			else if (RawInput.IsValid())
				InputStr = JsonToString(RawInput);
			if (!InputStr.IsEmpty())
			{
				TSharedPtr<FJsonObject> UpdData = MakeShareable(new FJsonObject);
				UpdData->SetStringField(TEXT("id"), ToolCallId);
				UpdData->SetStringField(TEXT("input"), InputStr);
				EnqueueResponse(TEXT("tool_update"), JsonToString(UpdData), ChatId);
			}
		}
		return;
	}

	if (UpdateType == TEXT("tool_call_update"))
	{
		FString Status = Update->HasField(TEXT("status")) ? Update->GetStringField(TEXT("status")) : TEXT("");
		FString ToolCallId = Update->HasField(TEXT("toolCallId")) ? Update->GetStringField(TEXT("toolCallId")) : TEXT("");

		FString ToolCallKey = MakeToolCallKey(AcpSessionId, ToolCallId);
		if (!ToolCallKey.IsEmpty() && BlockedShellToolCallIds.Contains(ToolCallKey))
		{
			if (Status == TEXT("completed") || Status == TEXT("failed"))
			{
				BlockedShellToolCallIds.Remove(ToolCallKey);
			}
			return;
		}

		if (IsShellToolCall(Update))
		{
			if (!ToolCallKey.IsEmpty()) BlockedShellToolCallIds.Add(ToolCallKey);
			FString Title = GetOptionalStringField(Update, TEXT("title"));
			if (Title.IsEmpty()) Title = ToolCallId.IsEmpty() ? TEXT("(unknown)") : ToolCallId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro: BLOCKED shell tool_call_update '%s' before UI display"), *Title);
			if (!ChatId.IsEmpty())
			{
				EnqueueResponse(TEXT("system"),
					FString::Printf(TEXT("Blocked shell command: %s"), *Title),
					ChatId);
			}
			if (Session && Session->bProcessing)
			{
				if (!AcpSessionId.IsEmpty())
				{
					const FString ChatAdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
					TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
					CancelParams->SetStringField(TEXT("sessionId"), AcpSessionId);
					SendRpcNotification(ChatAdapterId, TEXT("session/cancel"), CancelParams);
				}
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
			}
			return;
		}

		// Extract rawInput (comes as object, serialize to string)
		if (Update->HasField(TEXT("rawInput")))
		{
			TSharedPtr<FJsonObject> RawInput = Update->GetObjectField(TEXT("rawInput"));
			if (RawInput.IsValid() && RawInput->Values.Num() > 0)
			{
				TSharedPtr<FJsonObject> UpdData = MakeShareable(new FJsonObject);
				UpdData->SetStringField(TEXT("id"), ToolCallId);
				UpdData->SetStringField(TEXT("input"), JsonToString(RawInput));
				EnqueueResponse(TEXT("tool_update"), JsonToString(UpdData), ChatId);
			}
		}

		if (Status == TEXT("completed") || Status == TEXT("failed"))
		{
			TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
			Data->SetStringField(TEXT("id"), ToolCallId);
			Data->SetStringField(TEXT("status"), Status);

			// rawOutput: Codex 0.13.0 wraps as {"content":[{type,text}]}, older as bare [{type,text}],
			// some adapters may send a plain string.
			FString Combined;
			const TSharedPtr<FJsonValue> Raw = Update->TryGetField(TEXT("rawOutput"));

			auto ExtractRawOutputItems = [&]() -> const TArray<TSharedPtr<FJsonValue>>*
			{
				if (!Raw.IsValid()) return nullptr;
				if (Raw->Type == EJson::Object)
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
					Raw->AsObject()->TryGetArrayField(TEXT("content"), Arr);
					return Arr;
				}
				if (Raw->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
					Update->TryGetArrayField(TEXT("rawOutput"), Arr);
					return Arr;
				}
				return nullptr;
			};

			if (const TArray<TSharedPtr<FJsonValue>>* RawArr = ExtractRawOutputItems())
			{
				for (const auto& Item : *RawArr)
				{
					FString Text;
					if (!Item.IsValid() || Item->Type != EJson::Object) continue;
					TSharedPtr<FJsonObject> O = Item->AsObject();
					if (O.IsValid() && O->TryGetStringField(TEXT("text"), Text))
					{
						if (!Combined.IsEmpty()) Combined += TEXT("\n");
						Combined += Text;
					}
				}
			}
			else if (Raw.IsValid() && Raw->Type == EJson::String)
			{
				Combined = Raw->AsString();
			}

			if (!Combined.IsEmpty())
			{
				Data->SetStringField(TEXT("output"), Combined);
			}

			EnqueueResponse(TEXT("tool_end"), JsonToString(Data), ChatId);
		}
		return;
	}

	// Session info update
	if (UpdateType == TEXT("session_info_update"))
	{
		return; // Could update title
	}

	// Current mode update
	if (UpdateType == TEXT("current_mode_update"))
	{
		FString ModeId = Update->HasField(TEXT("currentModeId")) ? Update->GetStringField(TEXT("currentModeId")) : TEXT("");
		CurrentMode = ModeId;
		return;
	}
}

// ============================================================
// Permission response
// ============================================================

void UNwiroBridge::RespondToPermission(int32 RequestId, const FString& OptionId)
{
	// ─── codex-acp-string-id ─────────────────────────────────────────────────
	// ACP permission card: bridge minted RequestId as a LocalPermId, the real
	// ACP id (which may be string/null) lives in LocalPermIdToAcpReqId. Look up
	// and echo with the original wire type so the agent can match it.
	// ─────────────────────────────────────────────────────────────────────────
	if (Acp::FRequestId* AcpReqId = LocalPermIdToAcpReqId.Find(RequestId))
	{
		const FString AdapterId = LocalPermIdToAdapterId.FindRef(RequestId);
		RespondToAcpPermission(AdapterId.IsEmpty() ? CurrentAdapter : AdapterId, *AcpReqId, OptionId);
		LocalPermIdToAcpReqId.Remove(RequestId);
		LocalPermIdToAdapterId.Remove(RequestId);
		return;
	}

	// MCP id range (>= 1000) preserved for parity with NwiroIK; this plugin has
	// no in-process MCP server, so the branch is a no-op safety net.
	if (RequestId >= 1000)
	{
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("Nwiro: RespondToPermission ignored unknown id %d (no ACP map entry)"),
		RequestId);
}

// ============================================================
// Adapter binary discovery
// ============================================================

FString UNwiroBridge::FindAdapterBinary() const
{
	return TEXT("");
}

bool UNwiroBridge::IsAdapterAvailable(const FString& Adapter)
{
	return false;
}

bool UNwiroBridge::IsCliAvailable(const FString& Adapter)
{
	return false;
}

void UNwiroBridge::DeleteAdapter(const FString& Adapter)
{
}

void UNwiroBridge::DownloadAdapter(const FString& Adapter, const FString& Url)
{
}

// ============================================================
// Process management
// ============================================================

void UNwiroBridge::EnsureProcess()
{
}

void UNwiroBridge::KillProcess()
{
}

// ============================================================
// Public API
// ============================================================

void UNwiroBridge::SendMessage(const FString& Message)
{
	if (Message.TrimStartAndEnd().IsEmpty() || ActiveChatId.IsEmpty()) return;

	FChatSession* Session = ChatSessions.Find(ActiveChatId);
	if (Session && Session->bProcessing) return; // Already processing this chat

	EnsureProcess();
	FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter);
	if (!AP || !AP->Process.IsValid() || !AP->Process->IsRunning())
	{
		EnqueueResponse(TEXT("error"), TEXT("Agent process not running."), ActiveChatId);
		return;
	}

	// Get or create session for this chat
	FChatSession& S = ChatSessions.FindOrAdd(ActiveChatId);
	S.ChatId = ActiveChatId;
	if (S.AdapterId.IsEmpty())
	{
		S.AdapterId = CurrentAdapter;
	}
	else if (S.AdapterId != CurrentAdapter)
	{
		if (!S.SessionId.IsEmpty()) SessionToChatId.Remove(S.SessionId);
		S.AdapterId = CurrentAdapter;
		S.SessionId.Empty();
		S.PendingMessage.Empty();
		S.SessionRpcId = 0;
		S.PromptRpcId = 0;
		S.LastModel.Empty();
		S.SeenToolCallIds.Empty();
	}

	if (!S.SessionId.IsEmpty())
	{
		// Session exists, send prompt directly
		DoSendPrompt(ActiveChatId, Message);
	}
	else
	{
		// No session yet — queue message and create session
		S.PendingMessage = Message;
		S.bProcessing = true;
		EnqueueResponse(TEXT("thinking"), TEXT(""), ActiveChatId);

		if (AP && AP->bInitialized)
			DoCreateSession(ActiveChatId);
		// else: DoInitialize already called, session will be created after init
	}
}

void UNwiroBridge::CancelChatTurnInternal(const FString& ChatId)
{
	FChatSession* Session = ChatSessions.Find(ChatId);
	if (!Session || Session->SessionId.IsEmpty()) return;
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("sessionId"), Session->SessionId);
	const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
	SendRpcNotification(AdapterId, TEXT("session/cancel"), Params);
	Session->bProcessing = false;
	Session->PendingMessage.Empty();
}

void UNwiroBridge::CancelMessage()
{
	CancelChatTurnInternal(ActiveChatId);
	EnqueueResponse(TEXT("system"), TEXT("Cancelled."), ActiveChatId);
}

void UNwiroBridge::CancelAcpPermission(const FString& AdapterId, const Acp::FRequestId& ReqId)
{
	TSharedPtr<FJsonObject> Outcome = MakeShareable(new FJsonObject);
	Outcome->SetStringField(TEXT("outcome"), TEXT("cancelled"));
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetObjectField(TEXT("outcome"), Outcome);
	SendRpcResult(AdapterId, ReqId, Result);
}

void UNwiroBridge::OnPanelAttached()
{
	bPanelDetached = false;
}

void UNwiroBridge::OnPanelDetached()
{
	bPanelDetached = true;
	// >>> NWIRO_PARITY BEGIN: save-file-from-url-teardown
	// No renderer can receive a running save's events now: stop every one, silently.
	AbortFileSaves();
	// <<< NWIRO_PARITY END: save-file-from-url-teardown
	// Snapshot first: cancelling mutates the session entries and pushes events.
	TArray<FString> Processing;
	for (const auto& Pair : ChatSessions)
	{
		if (Pair.Value.bProcessing) Processing.Add(Pair.Key);
	}
	for (const FString& ChatId : Processing)
	{
		CancelChatTurnInternal(ChatId);
	}
	int32 Retired = 0;
	for (const auto& Pair : LocalPermIdToAcpReqId)
	{
		const FString* AdapterId = LocalPermIdToAdapterId.Find(Pair.Key);
		CancelAcpPermission(AdapterId ? *AdapterId : CurrentAdapter, Pair.Value);
		++Retired;
	}
	LocalPermIdToAcpReqId.Empty();
	LocalPermIdToAdapterId.Empty();
	UE_LOG(LogTemp, Log,
		TEXT("Nwiro: panel detached — cancelled %d in-flight turn(s), withdrew %d pending permission(s); adapter sessions kept"),
		Processing.Num(), Retired);
}

void UNwiroBridge::StopAllAdapterProcesses(const FString& Reason)
{
	for (auto& Pair : AdapterProcesses)
	{
		if (!Pair.Value.Process.IsValid()) continue;
		UE_LOG(LogTemp, Log, TEXT("Nwiro: stopping adapter=%s reason=%s"), *Pair.Key, *Reason);
		Pair.Value.Process->Cancel(true);
		Pair.Value.Process.Reset();
		Pair.Value.bInitialized = false;
	}
}

void UNwiroBridge::BeginDestroy()
{
	// >>> NWIRO_PARITY BEGIN: save-file-from-url-destroy
	// Last fallback: the panel's shutdown (OnPanelDetached) has normally emptied the list already.
	AbortFileSaves();
	// <<< NWIRO_PARITY END: save-file-from-url-destroy
	StopAllAdapterProcesses(TEXT("bridge_destroyed"));
	Super::BeginDestroy();
}

void UNwiroBridge::CloseChat(const FString& ChatId)
{
	FChatSession* Session = ChatSessions.Find(ChatId);
	if (Session && !Session->SessionId.IsEmpty())
	{
		// Cancel if processing
		if (Session->bProcessing)
		{
			TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
			Params->SetStringField(TEXT("sessionId"), Session->SessionId);
			const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
			SendRpcNotification(AdapterId, TEXT("session/cancel"), Params);
		}
		SessionToChatId.Remove(Session->SessionId);
	}
	ChatSessions.Remove(ChatId);
}

void UNwiroBridge::NewConversation()
{
	// Just clear active chat — don't kill the process (other chats may be active)
	ActiveChatId.Empty();
}

FString UNwiroBridge::ExecuteTool(const FString& ToolName, const FString& ArgsJson)
{
	// LEGACY synchronous surface. Kept verbatim for every tool that answers
	// inline (all ~224 natives and every already-fulfilled ue.* tool). A ue.*
	// tool that DEFERS cannot be answered here — CEF resolves the JS promise
	// when this returns — so it comes back with a structured
	// "nwiro.epic.async_required" error telling the caller to use
	// BeginExecuteTool. It never blocks the game thread.
	return FNwiroToolDispatch::DispatchTool(ToolName, ArgsJson);
}

FString UNwiroBridge::BeginExecuteTool(const FString& RequestId, const FString& ToolName,
	const FString& ArgsJson)
{
	// ASYNC surface. Returns an acknowledgement immediately; the result arrives
	// later as a `tool_result` event correlated by RequestId — the same
	// fire-and-await-event contract the frontend already uses for texture
	// import, HDRI bake, and landscape generation (app/src/utils/bridge-events.ts).
	//
	// Feature-detected on the JS side (`typeof bridge.beginexecutetool`), so an
	// older frontend against this plugin still works (it takes the legacy path
	// and gets the structured async_required error) and a newer frontend
	// against an older plugin falls back cleanly. No lockstep release.
	if (RequestId.IsEmpty())
	{
		return TEXT("{\"accepted\":false,\"error\":\"requestId is required\"}");
	}

	const FString CapturedRequestId = RequestId;
	const FString CapturedToolName  = ToolName;

	// Correlation is by RequestId alone, which is safe here because the caller
	// mints it with crypto.randomUUID() per call — a result arriving after a
	// CEF reload carries an id the new page never issued, so its matcher
	// rejects it. (Contrast the ACP transport, where the agent chooses the id
	// and CAN reuse it across a restart; that path additionally binds the
	// bridge instance and process generation.)
	auto Deliver = [CapturedRequestId, CapturedToolName](FString Result, bool bOk)
	{
		if (!UNwiroBridge::Instance) { return; }
		// Serialize through FJsonObject rather than hand-rolled escaping: the
		// tool result is arbitrary JSON text that must survive being carried
		// as a string field.
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetStringField(TEXT("requestId"), CapturedRequestId);
		Obj->SetStringField(TEXT("toolName"), CapturedToolName);
		Obj->SetBoolField(TEXT("ok"), bOk);
		Obj->SetStringField(TEXT("result"), Result);
		FString Data;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Data);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		UNwiroBridge::Instance->EnqueueResponse(TEXT("tool_result"), Data);
	};

	// `ok` reflects the ACTUAL outcome: a timeout/shutdown/error envelope is
	// success:false and must not be reported as ok:true just because it was
	// delivered. The frontend and the model both read this flag.
	auto DeliverEnvelope = [Deliver](FString Envelope)
	{
		const bool bOk = !Envelope.Contains(TEXT("\"success\":false"));
		Deliver(MoveTemp(Envelope), bOk);
	};

	FString Immediate;
	const ENwiroProEpicStart Start = FNwiroToolRegistry::BeginEpicTool(
		ToolName, ArgsJson,
		[DeliverEnvelope](FString Late) { DeliverEnvelope(MoveTemp(Late)); },
		Immediate);

	if (Start == ENwiroProEpicStart::Deferred)
	{
		return TEXT("{\"accepted\":true,\"deferred\":true}");
	}

	// Completed inline, or not a ue.* tool at all — run the normal dispatcher
	// for the latter. Delivered on the NEXT tick, never inline: the JS caller
	// has not finished registering its awaiter yet when this returns.
	FString Result = (Start == ENwiroProEpicStart::Completed)
		? MoveTemp(Immediate)
		: FNwiroToolDispatch::DispatchTool(ToolName, ArgsJson);

	AsyncTask(ENamedThreads::GameThread,
		[DeliverEnvelope, Result = MoveTemp(Result)]() mutable { DeliverEnvelope(MoveTemp(Result)); });

	return TEXT("{\"accepted\":true,\"deferred\":false}");
}

FString UNwiroBridge::GetToolDefinitions()
{
	return FNwiroToolDispatch::GetEnabledToolDefinitionsJson();
}

FString UNwiroBridge::GetToolDefinitionsForSettings()
{
	return FNwiroToolDispatch::GetToolDefinitionsForSettingsJson();
}

FString UNwiroBridge::GetDisabledToolNamespaces() const
{
	TArray<FString> Quoted;
	for (const FString& N : FNwiroToolRegistry::GetDisabledNamespaces())
	{
		Quoted.Add(FString::Printf(TEXT("\"%s\""), *N));
	}
	return TEXT("[") + FString::Join(Quoted, TEXT(",")) + TEXT("]");
}

void UNwiroBridge::SetDisabledToolNamespaces(const FString& JsonArray)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonArray);
	TArray<FString> Namespaces;
	if (FJsonSerializer::Deserialize(Reader, Items))
	{
		for (const TSharedPtr<FJsonValue>& V : Items)
		{
			if (V.IsValid()) { Namespaces.Add(V->AsString()); }
		}
	}
	FNwiroToolRegistry::SetDisabledNamespaces(Namespaces);
	FNwiroToolRegistry::Invalidate();
}

bool UNwiroBridge::GetToolbarButton() const
{
	return FNwiroPanel::IsToolbarButtonEnabled();
}

void UNwiroBridge::SetToolbarButton(bool bEnabled)
{
	FNwiroPanel::SetToolbarButtonEnabled(bEnabled);
}

FString UNwiroBridge::SearchAssets(const FString& Query)
{
	// Support both raw query string and JSON {"query":"..."}
	FString SearchTerm = Query;
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Query);
	if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid() && JsonObj->HasField(TEXT("query")))
	{
		SearchTerm = JsonObj->GetStringField(TEXT("query"));
	}
	bool bShowAll = SearchTerm.IsEmpty() || SearchTerm == TEXT("*");

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// `false` second arg = include in-memory assets too. Without this, BPs
	// the agent just created via create_blueprint but didn't save yet would
	// be invisible — and the agent rarely saves between turns.
	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAllAssets(AllAssets, false);

	FString QueryLower = SearchTerm.ToLower();
	TArray<TSharedPtr<FJsonValue>> Results;
	const int32 MaxResults = 50;

	for (const FAssetData& Asset : AllAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.GetObjectPathString();

		// Only project content (/Game/)
		if (!Path.StartsWith(TEXT("/Game/"))) continue;

		if (bShowAll || Name.ToLower().Contains(QueryLower))
		{
			TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("path"), Path);
			Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
			Results.Add(MakeShareable(new FJsonValueObject(Item)));
		}
	}

	// Return the same {success, assets, count} shape callers expect from
	// find_assets / the inline fallback. Previously this only emitted a bare
	// JSON array, which made `.assets` undefined on the client and looked
	// identical to "no results".
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("assets"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString ResultStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ResultStr);
	FJsonSerializer::Serialize(Root, Writer);
	return ResultStr;
}

FString UNwiroBridge::GetStaticMeshes()
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Filter for Static Meshes in /Game folder
	FARFilter Filter;
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TSharedRef<FJsonObject> RootObject = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> MeshArray;

	for (const FAssetData& Asset : AssetList)
	{
		TSharedRef<FJsonObject> MeshObj = MakeShareable(new FJsonObject());
		MeshObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		MeshObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		MeshObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

		// Derive top-level folder category from package path
		FString PackagePath = Asset.PackagePath.ToString();
		PackagePath.RemoveFromStart(TEXT("/Game/"));
		int32 SlashIndex;
		if (PackagePath.FindChar('/', SlashIndex))
		{
			PackagePath = PackagePath.Left(SlashIndex);
		}
		MeshObj->SetStringField(TEXT("category"), PackagePath);

		float SizeX = 0, SizeY = 0, SizeZ = 0;
		bool bFoundInRegistry = false;

		// Fast path: read ApproxSize tag from asset registry
		FString ApproxSizeStr;
		if (Asset.GetTagValue(FName("ApproxSize"), ApproxSizeStr))
		{
			TArray<FString> Dims;
			if (ApproxSizeStr.ParseIntoArray(Dims, TEXT("x"), true) == 3)
			{
				SizeX = FCString::Atof(*Dims[0]);
				SizeY = FCString::Atof(*Dims[1]);
				SizeZ = FCString::Atof(*Dims[2]);
				bFoundInRegistry = true;
			}
		}

		// Size unknown when ApproxSize is not populated yet. Keep the bridge
		// read-only and avoid hard-loading every mesh on the editor thread.
		if (!bFoundInRegistry)
		{
			SizeX = SizeY = SizeZ = 0.0f;
		}

		MeshObj->SetNumberField(TEXT("sizeX"), SizeX);
		MeshObj->SetNumberField(TEXT("sizeY"), SizeY);
		MeshObj->SetNumberField(TEXT("sizeZ"), SizeZ);

		MeshArray.Add(MakeShareable(new FJsonValueObject(MeshObj)));
	}

	RootObject->SetArrayField(TEXT("meshes"), MeshArray);
	RootObject->SetNumberField(TEXT("count"), MeshArray.Num());

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject, Writer);

	return OutputString;
}

FString UNwiroBridge::GetStaticMeshesByPath(const FString& FolderPath)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FString SearchPath = FolderPath;
	if (!SearchPath.StartsWith(TEXT("/Game")))
	{
		SearchPath = TEXT("/Game/") + SearchPath;
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*SearchPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TSharedRef<FJsonObject> RootObject = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> MeshArray;

	for (const FAssetData& Asset : AssetList)
	{
		TSharedRef<FJsonObject> MeshObj = MakeShareable(new FJsonObject());
		MeshObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		MeshObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		MeshObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

		float SizeX = 0, SizeY = 0, SizeZ = 0;
		bool bFoundInRegistry = false;

		FString ApproxSizeStr;
		if (Asset.GetTagValue(FName("ApproxSize"), ApproxSizeStr))
		{
			TArray<FString> Dims;
			if (ApproxSizeStr.ParseIntoArray(Dims, TEXT("x"), true) == 3)
			{
				SizeX = FCString::Atof(*Dims[0]);
				SizeY = FCString::Atof(*Dims[1]);
				SizeZ = FCString::Atof(*Dims[2]);
				bFoundInRegistry = true;
			}
		}

		if (!bFoundInRegistry)
		{
			SizeX = SizeY = SizeZ = 0.0f;
		}

		MeshObj->SetNumberField(TEXT("sizeX"), SizeX);
		MeshObj->SetNumberField(TEXT("sizeY"), SizeY);
		MeshObj->SetNumberField(TEXT("sizeZ"), SizeZ);

		MeshArray.Add(MakeShareable(new FJsonValueObject(MeshObj)));
	}

	RootObject->SetArrayField(TEXT("meshes"), MeshArray);
	RootObject->SetNumberField(TEXT("count"), MeshArray.Num());
	RootObject->SetStringField(TEXT("searchPath"), SearchPath);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject, Writer);

	return OutputString;
}

bool UNwiroBridge::ExecutePython(const FString& Code)
{
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python Script Plugin is not available."));
		return false;
	}
	if (!PythonPlugin->IsPythonAvailable())
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python is not available in this UE installation."));
		return false;
	}
	// Inline execution. Used by Biome Generator and other short-script flows.
	// HDRI's large bake script uses ExecutePythonHDRI instead — see header
	// comment for why the inline path is unsafe for big scripts.
	return PythonPlugin->ExecPythonCommand(*Code);
}

bool UNwiroBridge::ExecutePythonHDRI(const FString& Code)
{
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python Script Plugin is not available."));
		return false;
	}
	if (!PythonPlugin->IsPythonAvailable())
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python is not available in this UE installation."));
		return false;
	}

	// Re-entry guard (hardening): refuse a new bake while one is still
	// queued/running so two bakes can't interleave editor/Python work. The actual
	// python311 bake crash was fixed in the emitter (per-bake Python state
	// isolation + compilation barriers); this just prevents overlap. The UI treats
	// a false return as "not accepted".
	if (GNwiroHDRIRunning)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro: HDRI bake already in progress — ignoring re-entrant request."));
		return false;
	}

	// Write the script to a temp .py file and execute the file. The inline
	// form (`ExecPythonCommand(*Code)`) has a file-vs-inline heuristic that
	// misfires on large multi-line scripts — UE treats the entire script as
	// a filename and the load fails with "Could not load Python file ...".
	// Passing a real file path is unambiguous.
	const FString TempDir = FPaths::Combine(
		FPaths::ProjectIntermediateDir(), TEXT("Nwiro"), TEXT("PythonExec"));
	IFileManager::Get().MakeDirectory(*TempDir, /*Tree=*/true);
	const FString FileName = FString::Printf(
		TEXT("nwiro_hdri_%lld.py"), FDateTime::UtcNow().GetTicks());
	const FString TempPath = FPaths::Combine(TempDir, FileName);

	if (!FFileHelper::SaveStringToFile(Code, *TempPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error,
			TEXT("Nwiro: Failed to write HDRI Python script to temp file %s"),
			*TempPath);
		return false;
	}

	// Default: delete the temp file after exec regardless of outcome so we
	// don't leave generated scripts on disk in normal operation. Launch the
	// editor with `-NwiroKeepPythonScripts` to retain them for debugging.
	const bool bKeepTempScript =
		FParse::Param(FCommandLine::Get(), TEXT("NwiroKeepPythonScripts"));

	// Hardening: run the bake off the CEF/WebBrowser (libcef) bind-callback stack.
	// This wasn't the cause of the python311 bake crash (that was fixed in the
	// emitter), but executing heavy Python + UObject + Material work directly on a
	// browser callback is fragile, so we defer to a one-shot FTSTicker on the game
	// thread's main loop — OUTSIDE the TaskGraph context, matching the Meshy import
	// path (see the FTSTicker comment in the Meshy download handler).
	GNwiroHDRIRunning = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[TempPath, bKeepTempScript](float) -> bool
	{
		// Clear the global running flag on every exit path (defensive against
		// future edits adding early returns). The flag stays set for the whole
		// duration of ExecPythonCommand below, so a re-entrant request that
		// arrives while the script pumps the message loop is still rejected. The
		// lambda intentionally does not capture the bridge UObject — the bake
		// doesn't depend on it, and the bridge may be GC'd by the time this fires.
		ON_SCOPE_EXIT { GNwiroHDRIRunning = false; };

		IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
		const bool bSuccess = (Py != nullptr) && Py->ExecPythonCommand(*TempPath);
		if (!bSuccess)
		{
			UE_LOG(LogTemp, Error,
				TEXT("Nwiro: HDRI Python execution failed for %s. Check Output Log."),
				*TempPath);
		}

		if (!bKeepTempScript)
		{
			IFileManager::Get().Delete(*TempPath, /*bRequireExists=*/false,
				/*bEvenReadOnly=*/false, /*bQuiet=*/true);
		}

		return false; // one-shot ticker — do not repeat
	}));

	// True here means "validated, written, and queued" — not "finished". The web
	// UI awaits the actual HDRI result through a separate event flow.
	return true;
}

// Landscape placement runs inline via the generic executepython path (frontend:
// use-chat-send-pipeline.ts). There is no deferred landscape runner: create_landscape
// pre-creates the patch edit-layer, so no Slate modal pops during landscape patch/heightmap placement.

bool UNwiroBridge::SaveFile(const FString& Path, const FString& Content)
{
	FString FilePath = Path;
	FString FileContent = Content;
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [FilePath, FileContent]()
	{
		FFileHelper::SaveStringToFile(FileContent, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	});
	return true;
}

void UNwiroBridge::StartAdapter(const FString& Adapter)
{
}

// Declared in Nwiro.cpp
extern void SaveMCPConfig(int32 Port);

void UNwiroBridge::StartMCPServer(int32 Port)
{
}

void UNwiroBridge::StopMCPServer()
{
}

void UNwiroBridge::SetMCPPort(int32 Port)
{
	SaveMCPConfig(Port);
}

// Declared in Nwiro.cpp
extern FString LoadNwiroSecret(const FString& Key);
extern void SaveNwiroSecret(const FString& Key, const FString& Value);

FString UNwiroBridge::GetSecret(const FString& Key)
{
	return LoadNwiroSecret(Key);
}

void UNwiroBridge::SetSecret(const FString& Key, const FString& Value)
{
	SaveNwiroSecret(Key, Value);
}

void UNwiroBridge::SetChatExtensions(const FString& ChatId, const FString& ExtensionsJson)
{
	ActiveChatExtensions.Empty();
	bWorkspaceLayout = false;
	TSharedPtr<FJsonObject> Ext;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ExtensionsJson);
	if (FJsonSerializer::Deserialize(R, Ext) && Ext.IsValid())
	{
		for (const auto& Pair : Ext->Values)
		{
			// The app layout, not an extension: kept out of the map (TryGetBool would turn the
			// string into a false "layout" entry). Only "workspace" counts.
			if (Pair.Key == TEXT("layout"))
			{
				FString Layout;
				bWorkspaceLayout = Pair.Value->TryGetString(Layout) && Layout == TEXT("workspace");
				continue;
			}
			bool bEnabled = false;
			if (Pair.Value->TryGetBool(bEnabled))
			{
				ActiveChatExtensions.Add(FString(*Pair.Key), bEnabled);
			}
		}
	}
}

void UNwiroBridge::SetAdapterContext(const FString& ContextJson)
{
	TSharedPtr<FJsonObject> Ctx;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ContextJson);
	if (!FJsonSerializer::Deserialize(R, Ctx) || !Ctx.IsValid()) return;

	TSharedPtr<FJsonObject> Safety = Ctx->HasField(TEXT("safety"))
		? Ctx->GetObjectField(TEXT("safety")) : nullptr;
	if (Safety.IsValid())
	{
		if (Safety->HasField(TEXT("blockCommandExecution")))
		{
			const bool bRequestedBlockCommandExecution = Safety->GetBoolField(TEXT("blockCommandExecution"));
			bBlockCommandExecution = true;
			UE_LOG(LogTemp, Log,
				TEXT("Nwiro: SetAdapterContext blockCommandExecution requested=%s effective=true"),
				bRequestedBlockCommandExecution ? TEXT("true") : TEXT("false"));
		}
	}
}

bool UNwiroBridge::IsChatExtensionEnabled(const FString& ExtName) const
{
	const bool* Found = ActiveChatExtensions.Find(ExtName);
	return Found && *Found;
}

FString UNwiroBridge::ExtensionAccessHint(const FString& ExtName) const
{
	// The workspace layout runs provider generation in its windows; Chat View offers the
	// extensions in the chat's menu (tasks/chat-extensions-by-layout-plan.md, step 6).
	if (bWorkspaceLayout)
	{
		if (ExtName == TEXT("meshy") || ExtName == TEXT("tripo")) return TEXT("Use the Mesh window in the workspace rail instead.");
		if (ExtName == TEXT("elevenlabs")) return TEXT("Use the Audio window in the workspace rail instead.");
		if (ExtName == TEXT("fal")) return TEXT("Use the Material window in the workspace rail instead.");
	}
	return TEXT("Enable it from the Extensions menu in the chat input.");
}

FString UNwiroBridge::ManagedToolAdvice(const FString& ToolName) const
{
	// A *_managed tool (Nwiro Credits) only runs through the Nwiro app's workspace windows, never
	// from Claude, Codex or Local LLM. Name the window; in Chat View also the own-key route that
	// works from a chat (tasks/chat-extensions-by-layout-plan.md, step 6).
	FString Provider, Window;
	if (ToolName.Contains(TEXT("meshy"))) { Provider = TEXT("Meshy"); Window = TEXT("Mesh"); }
	else if (ToolName.Contains(TEXT("tripo"))) { Provider = TEXT("Tripo"); Window = TEXT("Mesh"); }
	else if (ToolName.Contains(TEXT("elevenlabs"))) { Provider = TEXT("ElevenLabs"); Window = TEXT("Audio"); }
	else if (ToolName.Contains(TEXT("fal"))) { Provider = TEXT("fal.ai"); Window = TEXT("Material"); }
	if (Provider.IsEmpty())
	{
		return TEXT("Nwiro Credits generation runs in the workspace windows (Audio, Mesh, Material), not from this chat.");
	}
	if (bWorkspaceLayout)
	{
		return FString::Printf(TEXT("%s with Nwiro Credits runs in the %s window in the workspace rail. Use it there."), *Provider, *Window);
	}
	return FString::Printf(TEXT("%s with Nwiro Credits runs in the %s window of the workspace layout. To use it from this chat, switch %s to Own API key in Settings > Extensions and turn it on from the Extensions menu."), *Provider, *Window, *Provider);
}

// ============================================================
// MESHY → UE5 IMPORT
// ============================================================
// Downloads an FBX (or OBJ) from a remote URL into a temp file, then runs it
// through AssetTools' import pipeline with UE-specific options tuned for the
// kind of content meshy.ai produces: single static mesh, no LODs, embedded
// textures off (we'd need to handle them separately), auto-collision on.
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetImportTask.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxFactory.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

void UNwiroBridge::ImportMesh(const FString& Url, const FString& FileName, const FString& DestFolder, bool bRevealInBrowser)
{
	if (Url.IsEmpty() || FileName.IsEmpty() || DestFolder.IsEmpty()) return;

	const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("Nwiro"), TEXT("MeshyImports"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString LocalPath = FPaths::Combine(TempDir, FileName);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(Url);
	const FString CapturedDestFolder = DestFolder;
	const bool bCapturedReveal = bRevealInBrowser;
	Req->OnProcessRequestComplete().BindLambda([LocalPath, FileName, CapturedDestFolder, bCapturedReveal](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
	{
		if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 400)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Nwiro] Model import: download failed for %s"), *FileName);
			if (UNwiroBridge::Instance)
			{
				UNwiroBridge::Instance->PushEvent(TEXT("toast"),
					FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"Model import failed: download error\"}")));
			}
			return;
		}

		if (!FFileHelper::SaveArrayToFile(Resp->GetContent(), *LocalPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Nwiro] Model import: failed to write %s"), *LocalPath);
			if (UNwiroBridge::Instance)
			{
				UNwiroBridge::Instance->PushEvent(TEXT("toast"),
					FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"Model import failed: the downloaded file could not be saved\"}")));
			}
			return;
		}

		// Use FTSTicker instead of AsyncTask to run on the game thread's main
		// loop — outside the TaskGraph context. ImportAssetTasks internally
		// creates TaskGraph tasks via Interchange, and running inside an
		// AsyncTask causes a re-entrancy assertion crash.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[LocalPath, FileName, CapturedDestFolder, bCapturedReveal](float) -> bool
		{
			const FString BaseName = FPaths::GetBaseFilename(FileName);
			const FString Ext = FPaths::GetExtension(FileName).ToLower();
			FString DestPath = CapturedDestFolder;
			while (DestPath.EndsWith(TEXT("/"))) DestPath.RemoveAt(DestPath.Len() - 1);

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = LocalPath;
			Task->DestinationPath = DestPath;
			Task->bAutomated = true;
			Task->bSave = true;
			Task->bReplaceExisting = true;
			Task->DestinationName = BaseName;

			UFbxFactory* Factory = nullptr;
			if (Ext == TEXT("fbx"))
			{
				Factory = NewObject<UFbxFactory>(GetTransientPackage(), UFbxFactory::StaticClass(), NAME_None, RF_NoFlags);
				Factory->AddToRoot();
				UFbxImportUI* Options = NewObject<UFbxImportUI>(Factory);
				if (!Options->StaticMeshImportData)
				{
					Options->StaticMeshImportData = NewObject<UFbxStaticMeshImportData>(Options);
				}
				Options->MeshTypeToImport = FBXIT_StaticMesh;
				Options->OriginalImportType = FBXIT_StaticMesh;
				Options->bImportMesh = true;
				Options->bImportMaterials = true;
				Options->bImportTextures = true;
				Options->bImportAnimations = false;
				Options->bImportAsSkeletal = false;
				Options->bCreatePhysicsAsset = false;
				Options->bAutomatedImportShouldDetectType = false;
				Options->StaticMeshImportData->NormalImportMethod = FBXNIM_ImportNormalsAndTangents;
				Options->StaticMeshImportData->bAutoGenerateCollision = true;
				Options->StaticMeshImportData->bCombineMeshes = true;
				Options->StaticMeshImportData->bGenerateLightmapUVs = true;
				Factory->SetDetectImportTypeOnImport(false);
				Factory->ImportUI = Options;
				Task->Factory = Factory;
				Task->Options = Options;
			}
			Task->AddToRoot();

			FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
			TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
			AT.Get().ImportAssetTasks(Tasks);
			Task->RemoveFromRoot();
			if (Factory) Factory->RemoveFromRoot();

			const bool bAny = Task->ImportedObjectPaths.Num() > 0;
			if (bAny && bCapturedReveal)
			{
				FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
				TArray<FAssetData> Assets;
				FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				for (const FString& ObjPath : Task->ImportedObjectPaths)
				{
					FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(ObjPath));
					if (AD.IsValid()) Assets.Add(AD);
				}
				if (Assets.Num() > 0)
				{
					CB.Get().SyncBrowserToAssets(Assets);
				}
				else
				{
					CB.Get().SyncBrowserToFolders({ DestPath });
				}
			}
			if (UNwiroBridge::Instance)
			{
				const FString Msg = bAny
					? FString::Printf(TEXT("{\"level\":\"success\",\"text\":\"Model imported: %s\"}"), *BaseName)
					: FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"Model import failed: %s\"}"), *BaseName);
				UNwiroBridge::Instance->PushEvent(TEXT("toast"), Msg);
			}
			return false; // one-shot ticker — do not repeat
		}));
	});
	Req->ProcessRequest();
}

namespace
{
	// Toast for a file written through a save dialog. The dialog returns a path relative to the
	// engine's base directory ("../../../../../../Users/..."); the message shows the full platform
	// path. A failed write reports an error instead of looking like a cancelled dialog.
	void PushSavedFileToast(const TCHAR* What, const FString& Target, bool bWritten)
	{
		if (!UNwiroBridge::Instance) return;
		FString Display = FPaths::ConvertRelativePathToFull(Target);
		FPaths::MakePlatformFilename(Display);
		const FString Escaped = Display.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		UNwiroBridge::Instance->PushEvent(TEXT("toast"), bWritten
			? FString::Printf(TEXT("{\"level\":\"success\",\"text\":\"%s saved to %s\"}"), What, *Escaped)
			: FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"Could not save %s to %s\"}"), *FString(What).ToLower(), *Escaped));
	}
}

void UNwiroBridge::SaveAudioToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext)
{
	if (Base64Bytes.IsEmpty()) return;
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0) return;

	const FString SafeExt = Ext.IsEmpty() ? TEXT("mp3") : Ext.Replace(TEXT("."), TEXT(""));
	const FString Filter = FString::Printf(TEXT("Audio (*.%s)|*.%s"), *SafeExt, *SafeExt);

	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return;
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
	TArray<FString> OutFiles;
	const bool bSaved = Desktop->SaveFileDialog(
		ParentWindow,
		TEXT("Save Audio"),
		FPaths::ProjectDir(),
		FString::Printf(TEXT("%s.%s"), *SuggestedName, *SafeExt),
		Filter,
		EFileDialogFlags::None,
		OutFiles
	);
	if (!bSaved || OutFiles.Num() == 0) return;

	FString Target = OutFiles[0];
	if (!Target.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase))
	{
		Target += TEXT(".");
		Target += SafeExt;
	}
	PushSavedFileToast(TEXT("Audio"), Target, FFileHelper::SaveArrayToFile(Bytes, *Target));
}

namespace
{
	// Fire the texture_import_done event for the new import-completion contract.
	// No-op when RequestId is empty so legacy call sites are unaffected.
	// Warning (optional): the import worked but a follow-up did not (a material map's texture
	// settings were not saved); written as "warning" only when non-empty.
	void EmitTextureImportDone(const FString& RequestId, bool bSuccess,
	                           const FString& AssetPath, const FString& Error,
	                           const FString& Warning = FString())
	{
		if (RequestId.IsEmpty() || !UNwiroBridge::Instance)
		{
			return;
		}
		auto JsonEscape = [](const FString& In)
		{
			return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		};
		FString Data = FString::Printf(
			TEXT("{\"requestId\":\"%s\",\"success\":%s,\"assetPath\":\"%s\",\"error\":\"%s\""),
			*JsonEscape(RequestId),
			bSuccess ? TEXT("true") : TEXT("false"),
			*JsonEscape(AssetPath),
			*JsonEscape(Error));
		if (!Warning.IsEmpty())
		{
			Data += FString::Printf(TEXT(",\"warning\":\"%s\""), *JsonEscape(Warning));
		}
		Data += TEXT("}");
		UNwiroBridge::Instance->PushEvent(TEXT("texture_import_done"), Data);
	}

	// Texture settings per fal material map type (tasks/fal-texture-import-settings-plan.md).
	// Unreal's importer gives every image the color defaults (sRGB on, TC_Default) and recognises
	// only normal maps, by their pixels; roughness, metalness and height would stay sRGB and read
	// too glossy. Only the app knows a map's type, so it is applied here, after the import.
	// Unknown or empty type (HDRI, the untagged texture image): the asset stays as imported.
	// bFlipGreenChannel: set explicitly on every known type so a replaced asset's value cannot
	// leak through. fal does not document PATINA's normal convention; measured 2026-10-07
	// (findings F49): the green channel follows the height map's slope the OpenGL way (Y up),
	// and Unreal expects DirectX (Y down), so normal maps are flipped.
	struct FNwiroMapTextureSettings
	{
		bool bSRGB;
		TextureCompressionSettings Compression;
		TextureGroup LODGroup;
		bool bFlipGreenChannel;
	};

	bool MapTextureSettingsFor(const FString& MapType, FNwiroMapTextureSettings& Out)
	{
		const FString Type = MapType.ToLower();
		if (Type == TEXT("basecolor")) { Out = { true,  TC_Default,   TEXTUREGROUP_World,          false }; return true; }
		if (Type == TEXT("normal"))    { Out = { false, TC_Normalmap, TEXTUREGROUP_WorldNormalMap, true  }; return true; }
		if (Type == TEXT("roughness") || Type == TEXT("metalness") || Type == TEXT("height"))
		{
			// TC_Grayscale: uncompressed single channel (G8), exact values; the compressed
			// alternative would be TC_Alpha (BC4).
			Out = { false, TC_Grayscale, TEXTUREGROUP_World, false };
			return true;
		}
		return false;
	}

	// Applies the map type's settings to the imported texture and saves it again (the import task
	// saved before the change). Returns a warning for the texture_import_done event when the
	// texture exists but the settings did not reach it or the disk; empty when applied, or when the
	// type calls for nothing.
	FString ApplyMapTextureSettings(const FString& AssetObjectPath, const FString& MapType)
	{
		FNwiroMapTextureSettings Settings;
		if (!MapTextureSettingsFor(MapType, Settings)) return FString();

		UTexture2D* Texture = Cast<UTexture2D>(UEditorAssetLibrary::LoadAsset(AssetObjectPath));
		if (!Texture)
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro [Bridge] texture settings for '%s' not applied: '%s' did not load as a Texture2D"), *MapType, *AssetObjectPath);
			return FString::Printf(TEXT("%s settings not applied: the imported asset could not be loaded"), *MapType);
		}

		Texture->PreEditChange(nullptr);
		Texture->SRGB = Settings.bSRGB;
		Texture->CompressionSettings = Settings.Compression;
		Texture->LODGroup = Settings.LODGroup;
		Texture->bFlipGreenChannel = Settings.bFlipGreenChannel;
		Texture->PostEditChange();
		Texture->MarkPackageDirty();

		if (!UEditorAssetLibrary::SaveLoadedAsset(Texture, false))
		{
			// Applied in memory; the package stays dirty, so the user's next save writes it.
			UE_LOG(LogTemp, Warning, TEXT("Nwiro [Bridge] texture settings for '%s' applied but not saved: %s"), *MapType, *AssetObjectPath);
			return FString::Printf(TEXT("%s settings applied but not saved; save the texture to keep them"), *MapType);
		}
		return FString();
	}
}

// The import itself, shared by ImportTextureFromUrl (which knows the map type) and
// ImportTextureBytes (which keeps its public signature and passes none).
static void ImportTextureBytesInternal(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId, const FString& MapType);

void UNwiroBridge::ImportTextureFromUrl(const FString& Url, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId, const FString& MapType)
{
	if (Url.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Nwiro [Bridge] ImportTextureFromUrl REJECTED: empty url/folder/name"));
		EmitTextureImportDone(RequestId, false, TEXT(""), TEXT("empty url/folder/name"));
		return;
	}
	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString CapturedFolder = DestFolder;
	const FString CapturedName = DestName;
	const bool bCapturedReveal = bRevealInBrowser;
	const FString CapturedRequestId = RequestId;
	const FString CapturedMapType = MapType;

	auto Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->OnProcessRequestComplete().BindLambda(
		[CapturedFolder, CapturedName, bCapturedReveal, SafeExt, CapturedRequestId, CapturedMapType](FHttpRequestPtr, FHttpResponsePtr R, bool ok)
		{
			if (!ok || !R.IsValid() || R->GetResponseCode() < 200 || R->GetResponseCode() >= 300)
			{
				UE_LOG(LogTemp, Error, TEXT("Nwiro [Bridge] ImportTextureFromUrl: download FAILED"));
				if (UNwiroBridge::Instance)
				{
					UNwiroBridge::Instance->PushEvent(TEXT("toast"),
						TEXT("{\"level\":\"error\",\"text\":\"fal.ai import: download failed\"}"));
				}
				const int32 Code = R.IsValid() ? R->GetResponseCode() : -1;
				EmitTextureImportDone(CapturedRequestId, false, TEXT(""),
					FString::Printf(TEXT("download failed (code %d)"), Code));
				return;
			}
			const TArray<uint8>& Bytes = R->GetContent();

			if (UNwiroBridge::Instance && Bytes.Num() > 0)
			{
				const FString B64 = FBase64::Encode(Bytes);
				// Pass RequestId through so the completion event fires from
				// the actual import callsite; the map type sets the texture's settings there.
				ImportTextureBytesInternal(B64, SafeExt, CapturedFolder, CapturedName, bCapturedReveal, CapturedRequestId, CapturedMapType);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Nwiro [Bridge] ImportTextureFromUrl: skipping ImportTextureBytes (instance=%p bytes=%d)"),
					UNwiroBridge::Instance, Bytes.Num());
				EmitTextureImportDone(CapturedRequestId, false, TEXT(""),
					TEXT("downloaded zero bytes or bridge instance gone"));
			}
		});
	Req->ProcessRequest();
}

void UNwiroBridge::SaveTextureFromUrl(const FString& Url, const FString& SuggestedName, const FString& Ext)
{
	if (Url.IsEmpty()) return;
	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString CapturedName = SuggestedName;

	auto Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->OnProcessRequestComplete().BindLambda(
		[CapturedName, SafeExt](FHttpRequestPtr, FHttpResponsePtr R, bool ok)
		{
			if (!ok || !R.IsValid() || R->GetResponseCode() < 200 || R->GetResponseCode() >= 300) return;
			const TArray<uint8>& Bytes = R->GetContent();
			const FString Filter = FString::Printf(TEXT("Image (*.%s)|*.%s"), *SafeExt, *SafeExt);
			IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
			if (!Desktop) return;
			const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
				? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
				: nullptr;
			TArray<FString> OutFiles;
			const bool bSaved = Desktop->SaveFileDialog(
				ParentWindow, TEXT("Save Texture"), FPaths::ProjectDir(),
				FString::Printf(TEXT("%s.%s"), *CapturedName, *SafeExt),
				Filter, EFileDialogFlags::None, OutFiles);
			if (!bSaved || OutFiles.Num() == 0) return;
			FString Target = OutFiles[0];
			if (!Target.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase))
			{
				Target += TEXT(".");
				Target += SafeExt;
			}
			PushSavedFileToast(TEXT("Texture"), Target, FFileHelper::SaveArrayToFile(Bytes, *Target));
		});
	Req->ProcessRequest();
}

// >>> NWIRO_PARITY BEGIN: save-file-from-url-impl
// SaveFileFromUrl (design-and-implementation-plans/wme-scg/scene-gen-save-from-link-plan.md).
// One FFileSaveOp per call, settled exactly once, on the game thread, by SettleFileSave. The HTTP
// thread touches only Writer, Received and StreamError, from the body stream delegate, and only
// until the request completes or CancelRequest returns: CancelRequest detaches the stream under
// its lock before returning, so the game thread may close the writer right after it.
struct UNwiroBridge::FFileSaveOp
{
	enum EStreamError : uint8 { StreamOk, StreamTooLarge, StreamWriteFailed };

	FGuid Id;
	FString Url;
	FString RequestId;
	FString What = TEXT("File");    // "Video" / "Image", for toasts
	FString ChosenPath;             // absolute; set once the dialog returned a path
	FString TempPath;               // set only once this save created the temporary file
	int64 Ceiling = 0;
	FArchive* Writer = nullptr;
	FHttpRequestPtr Request;
	std::atomic<int64> Received{0};
	std::atomic<uint8> StreamError{StreamOk};
	// Game thread only.
	int32 LastStatus = 0;
	double LastProgressTime = 0.0;
	bool bRequestDone = false;
	bool bPromoted = false;
	bool bSettled = false;
	bool bSuppressed = false;
};

namespace
{
	// The dialog's filter label for an extension the kind allows; nullptr when it is not allowed.
	const TCHAR* FileSaveTypeLabel(bool bVideo, const FString& Ext)
	{
		if (bVideo)
		{
			if (Ext == TEXT("mp4")) return TEXT("MP4 video");
			if (Ext == TEXT("mov")) return TEXT("QuickTime video");
			return nullptr;
		}
		if (Ext == TEXT("png")) return TEXT("PNG image");
		if (Ext == TEXT("jpg") || Ext == TEXT("jpeg")) return TEXT("JPEG image");
		if (Ext == TEXT("webp")) return TEXT("WebP image");
		return nullptr;
	}

	// The suggested name, cleaned before the dialog: the last path segment only; no control
	// characters or < > : " / \ | ? *; no spaces or dots at either end; a Windows device name (CON,
	// PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension) gets a leading '_'; at most 100
	// characters. Nothing left -> Fallback.
	FString CleanSuggestedFileName(const FString& In, const TCHAR* Fallback)
	{
		int32 LastSeparator = INDEX_NONE;
		for (int32 Index = In.Len() - 1; Index >= 0; --Index)
		{
			if (In[Index] == TEXT('/') || In[Index] == TEXT('\\')) { LastSeparator = Index; break; }
		}
		FString Name;
		for (const TCHAR C : In.Mid(LastSeparator + 1))
		{
			if (C < 32 || C == 127 || FCString::Strchr(TEXT("<>:\"/\\|?*"), C)) continue;
			Name.AppendChar(C);
		}
		auto TrimEnds = [](FString& S)
		{
			int32 Start = 0;
			int32 End = S.Len();
			while (Start < End && (S[Start] == TEXT(' ') || S[Start] == TEXT('.'))) ++Start;
			while (End > Start && (S[End - 1] == TEXT(' ') || S[End - 1] == TEXT('.'))) --End;
			S = S.Mid(Start, End - Start);
		};
		TrimEnds(Name);
		int32 FirstDot = INDEX_NONE;
		FString Stem = Name.FindChar(TEXT('.'), FirstDot) ? Name.Left(FirstDot) : Name;
		Stem.TrimEndInline();
		static const TCHAR* const DeviceNames[] = {
			TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"),
			TEXT("COM1"), TEXT("COM2"), TEXT("COM3"), TEXT("COM4"), TEXT("COM5"), TEXT("COM6"), TEXT("COM7"), TEXT("COM8"), TEXT("COM9"),
			TEXT("LPT1"), TEXT("LPT2"), TEXT("LPT3"), TEXT("LPT4"), TEXT("LPT5"), TEXT("LPT6"), TEXT("LPT7"), TEXT("LPT8"), TEXT("LPT9"),
		};
		for (const TCHAR* Device : DeviceNames)
		{
			if (Stem.Equals(Device, ESearchCase::IgnoreCase)) { Name = TEXT("_") + Name; break; }
		}
		if (Name.Len() > 100)
		{
			// Never cut a UTF-16 surrogate pair in half.
			const int32 Keep = (Name[99] >= 0xD800 && Name[99] <= 0xDBFF) ? 99 : 100;
			Name.LeftInline(Keep);
			TrimEnds(Name);
		}
		return Name.IsEmpty() ? FString(Fallback) : Name;
	}
}

void UNwiroBridge::SaveFileFromUrl(const FString& Url, const FString& SuggestedName, const FString& Kind, const FString& Ext, const FString& RequestId)
{
	const TSharedRef<FFileSaveOp> Op = MakeShared<FFileSaveOp>();
	Op->Id = FGuid::NewGuid();
	Op->Url = Url;
	Op->RequestId = RequestId;

	// 1. The request, before any dialog.
	const bool bVideo = Kind.Equals(TEXT("video"), ESearchCase::CaseSensitive);
	const bool bImage = Kind.Equals(TEXT("image"), ESearchCase::CaseSensitive);
	if (bVideo) Op->What = TEXT("Video");
	if (bImage) Op->What = TEXT("Image");
	FString SafeExt = Ext.ToLower();
	if (SafeExt.StartsWith(TEXT("."))) SafeExt.RightChopInline(1);
	if (Url.IsEmpty() || (!bVideo && !bImage))
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("invalid_request"), Url.IsEmpty() ? TEXT("No link was given.") : TEXT("Unknown file kind."));
		return;
	}
	if (!Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("not_https"), TEXT("Only https links can be saved."));
		return;
	}
	const TCHAR* TypeLabel = FileSaveTypeLabel(bVideo, SafeExt);
	if (!TypeLabel)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("type_not_allowed"), TEXT("This file type cannot be saved here."));
		return;
	}
	Op->Ceiling = bVideo ? (int64(1) << 30) : (int64(256) << 20);

	// 2. The dialog: one file type, a new name.
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("internal"), TEXT("The Save dialog is not available."));
		return;
	}
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
	TArray<FString> OutFiles;
	const bool bChosen = Desktop->SaveFileDialog(
		ParentWindow, bVideo ? TEXT("Save Video") : TEXT("Save Image"), FPaths::ProjectDir(),
		FString::Printf(TEXT("%s.%s"), *CleanSuggestedFileName(SuggestedName, bVideo ? TEXT("nwiro_video") : TEXT("nwiro_image")), *SafeExt),
		FString::Printf(TEXT("%s (*.%s)|*.%s"), TypeLabel, *SafeExt, *SafeExt),
		EFileDialogFlags::None, OutFiles);
	if (!bChosen || OutFiles.Num() == 0)
	{
		SettleFileSave(Op, TEXT("cancelled"));
		return;
	}
	// The Windows dialog can return a path relative to the engine's base directory.
	FString Chosen = FPaths::ConvertRelativePathToFull(OutFiles[0]);
	const bool bAddedExt = !Chosen.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase);
	if (bAddedExt)
	{
		Chosen += TEXT(".");
		Chosen += SafeExt;
	}
	Op->ChosenPath = Chosen;
	// An existing file may be chosen, and it is replaced only once the download has fully worked
	// (step 5). A folder of that name cannot be.
	if (FPaths::DirectoryExists(Chosen))
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("exists"), TEXT("A folder with that name already exists. Choose another name."));
		return;
	}
	// The dialog asks about replacing only the name it returns. On Windows the engine adds the file
	// type's extension after the dialog has closed (DesktopPlatformWindows: "Apply the selected
	// extension"), and the lines above may add it on any platform, so a file at the final name may
	// never have been confirmed: ask here. On Windows a name typed with its extension is asked about
	// twice; a silent replace would be worse. The Mac panel adds the extension itself and asks then.
	if ((PLATFORM_WINDOWS || bAddedExt) && FPaths::FileExists(Chosen))
	{
		const FText Question = FText::FromString(FString::Printf(
			TEXT("%s already exists. Do you want to replace it?"), *FPaths::GetCleanFilename(Chosen)));
		if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, Question) != EAppReturnType::Yes)
		{
			SettleFileSave(Op, TEXT("cancelled"));
			return;
		}
	}

	// 3. The temporary file, unique by name (the engine has no exclusive create).
	const FString Temp = FString::Printf(TEXT("%s.%s.nwiro-part"), *Chosen, *FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
	if (FPaths::FileExists(Temp) || FPaths::DirectoryExists(Temp))
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("write_failed"), TEXT("The file could not be written."));
		return;
	}
	Op->Writer = IFileManager::Get().CreateFileWriter(*Temp);
	if (!Op->Writer)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("write_failed"), TEXT("The file could not be written. Check that the folder can be written to."));
		return;
	}
	Op->TempPath = Temp;

	// 4. The download, written as it arrives. Delegates hold weak pointers only: a callback whose
	// save is gone or settled does nothing.
	const FHttpRequestRef Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	Req->SetTimeout(1800.f);
	Req->SetActivityTimeout(60.f);
	const TWeakPtr<FFileSaveOp> WeakOp = Op;
	const TWeakObjectPtr<UNwiroBridge> WeakBridge(this);

	// Body chunks, on an HTTP thread. The ceiling is checked before each chunk, also when the
	// length is unknown and for an error answer's body. A chunk is refused by changing
	// InOutLength: the request then fails; returning early would not stop it.
	const bool bStreamSet = Req->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(
		[WeakOp](void* Ptr, int64& InOutLength)
		{
			const TSharedPtr<FFileSaveOp> Pinned = WeakOp.Pin();
			if (!Pinned.IsValid() || !Pinned->Writer || Pinned->StreamError.load() != FFileSaveOp::StreamOk)
			{
				InOutLength = 0;
				return;
			}
			const int64 Length = InOutLength;
			if (Pinned->Received.load() + Length > Pinned->Ceiling)
			{
				Pinned->StreamError.store(FFileSaveOp::StreamTooLarge);
				InOutLength = 0;
				return;
			}
			Pinned->Writer->Serialize(Ptr, Length);
			if (Pinned->Writer->IsError())
			{
				Pinned->StreamError.store(FFileSaveOp::StreamWriteFailed);
				InOutLength = 0;
				return;
			}
			Pinned->Received.fetch_add(Length);
		}));
	if (!bStreamSet)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("internal"), TEXT("The download could not be started."));
		return;
	}
	// The status of the final answer (a redirect's status is not reported), so progress is sent
	// only for the file itself, not for an error answer's body.
	Req->OnStatusCodeReceived().BindLambda([WeakOp](FHttpRequestPtr, int32 StatusCode)
	{
		if (const TSharedPtr<FFileSaveOp> Pinned = WeakOp.Pin()) Pinned->LastStatus = StatusCode;
	});
	Req->OnRequestProgress64().BindLambda([WeakOp, WeakBridge](FHttpRequestPtr Request, uint64, uint64)
	{
		const TSharedPtr<FFileSaveOp> Pinned = WeakOp.Pin();
		UNwiroBridge* Bridge = WeakBridge.Get();
		if (!Pinned.IsValid() || !Bridge || Pinned->bSettled || Pinned->RequestId.IsEmpty()) return;
		if (Pinned->LastStatus < 200 || Pinned->LastStatus >= 300) return;
		const double Now = FPlatformTime::Seconds();
		if (Now - Pinned->LastProgressTime < 0.25) return; // at most about 4 a second
		Pinned->LastProgressTime = Now;
		const FHttpResponsePtr Response = Request.IsValid() ? Request->GetResponse() : nullptr;
		const uint64 Declared = Response.IsValid() ? Response->GetContentLength() : 0;
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("requestId"), Pinned->RequestId);
		Data->SetNumberField(TEXT("received"), static_cast<double>(Pinned->Received.load()));
		Data->SetNumberField(TEXT("total"), Declared > 0 ? static_cast<double>(Declared) : -1.0);
		Bridge->PushEvent(TEXT("file_save_progress"), JsonToString(Data));
	});
	Req->OnProcessRequestComplete().BindLambda([WeakOp, WeakBridge](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
	{
		const TSharedPtr<FFileSaveOp> Pinned = WeakOp.Pin();
		UNwiroBridge* Bridge = WeakBridge.Get();
		if (!Pinned.IsValid() || !Bridge || Pinned->bSettled) return;
		Pinned->bRequestDone = true;
		Bridge->FinishFileSaveDownload(Pinned.ToSharedRef(), bSucceeded && Response.IsValid(),
			Response.IsValid() ? Response->GetResponseCode() : 0,
			Request.IsValid() ? Request->GetEffectiveURL() : FString());
	});
	Op->Request = Req;
	ActiveFileSaves.Add(Op->Id, Op);
	if (!Req->ProcessRequest())
	{
		// A completion that still fires finds the save settled.
		Op->bRequestDone = true;
		SettleFileSave(Op, TEXT("failed"), TEXT("network"), TEXT("The download could not be started."));
	}
}

// 5. Completion, on the game thread. Any failure closes and deletes the temporary file.
void UNwiroBridge::FinishFileSaveDownload(const TSharedRef<FFileSaveOp>& Op, bool bSucceeded, int32 HttpStatus, const FString& EffectiveUrl)
{
	const uint8 StreamError = Op->StreamError.load();
	if (StreamError == FFileSaveOp::StreamTooLarge)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("too_large"), Op->What == TEXT("Video")
			? TEXT("The video is larger than the 1 GB limit.") : TEXT("The image is larger than the 256 MB limit."));
		return;
	}
	if (StreamError == FFileSaveOp::StreamWriteFailed)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("write_failed"), TEXT("The file could not be written. Check that the disk is not full."));
		return;
	}
	if (!bSucceeded)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("network"), TEXT("The download failed or timed out."));
		return;
	}
	if (HttpStatus < 200 || HttpStatus >= 300)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("http_error"), HttpStatus == 403
			? TEXT("The link has expired or is not allowed (status 403).")
			: FString::Printf(TEXT("The server answered with status %d."), HttpStatus), HttpStatus);
		return;
	}
	// Signed links never redirect. This refuses a changed final address; the bytes curl fetched
	// meanwhile stay in the temporary file, which is deleted.
	if (!EffectiveUrl.Equals(Op->Url, ESearchCase::CaseSensitive))
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("redirected"), TEXT("The download was redirected, so it was not saved."));
		return;
	}
	if (Op->Received.load() == 0)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("empty_response"), TEXT("The server sent an empty file."));
		return;
	}
	// The writer's last bytes are written when it closes, which can fail too (a full disk).
	FArchive* Writer = Op->Writer;
	Op->Writer = nullptr;
	const bool bClosed = Writer->Close() && !Writer->IsError();
	delete Writer;
	if (!bClosed)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("write_failed"), TEXT("The file could not be written. Check that the disk is not full."));
		return;
	}
	// Promote in one step. An existing file (the user agreed to replace it) is swapped for the new
	// one by a single replacing rename: when it fails (the file is open elsewhere, read-only), the
	// old file stays exactly as it was. Never Unreal's Move(Replace), which deletes the old file
	// first. Two saves to the same name at once: the one that finishes last stays.
	if (FPaths::DirectoryExists(Op->ChosenPath))
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("exists"), TEXT("A folder with that name already exists. Choose another name."));
		return;
	}
	const bool bReplacing = FPaths::FileExists(Op->ChosenPath);
#if PLATFORM_WINDOWS
	FString From = Op->TempPath;
	FString To = Op->ChosenPath;
	FPaths::MakePlatformFilename(From);
	FPaths::MakePlatformFilename(To);
	const bool bMoved = ::MoveFileExW(*From, *To, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	// POSIX rename replaces an existing file in one step as well.
	const bool bMoved = FPlatformFileManager::Get().GetPlatformFile().MoveFile(*Op->ChosenPath, *Op->TempPath);
#endif
	if (!bMoved)
	{
		SettleFileSave(Op, TEXT("failed"), TEXT("write_failed"), bReplacing
			? TEXT("The existing file could not be replaced. Close any program that has it open, or choose another name.")
			: TEXT("The file could not be moved into place."));
		return;
	}
	Op->bPromoted = true;
	SettleFileSave(Op, TEXT("saved"));
}

// 6. The one way a save ends: stops the request if it still runs, removes the temporary file
// unless it was promoted, leaves the active list, then a toast and at most one file_save_done
// (none without a RequestId or when suppressed by teardown).
void UNwiroBridge::SettleFileSave(const TSharedRef<FFileSaveOp>& Op, const FString& Status, const FString& ErrorCode, const FString& Error, int32 HttpStatus)
{
	if (Op->bSettled) return;
	Op->bSettled = true;
	if (Op->Request.IsValid())
	{
		// Only when settled from outside the completion (teardown). The HTTP module is gone only
		// after the panel's shutdown has already emptied the list; checked for the BeginDestroy path.
		if (!Op->bRequestDone && FModuleManager::Get().IsModuleLoaded(TEXT("HTTP")))
		{
			Op->Request->CancelRequest();
		}
		Op->Request.Reset();
	}
	if (Op->Writer)
	{
		Op->Writer->Close();
		delete Op->Writer;
		Op->Writer = nullptr;
	}
	if (!Op->bPromoted && !Op->TempPath.IsEmpty())
	{
		IFileManager::Get().Delete(*Op->TempPath, false, false, true);
	}
	ActiveFileSaves.Remove(Op->Id);
	if (Op->bSuppressed) return;

	const bool bSaved = Status == TEXT("saved");
	const bool bFailed = Status == TEXT("failed");
	if (bFailed)
	{
		UE_LOG(LogTemp, Warning, TEXT("Nwiro: SaveFileFromUrl failed: %s (%s)"), *ErrorCode, *Error);
	}
	if (bSaved)
	{
		PushSavedFileToast(*Op->What, Op->ChosenPath, true);
	}
	else if (bFailed)
	{
		TSharedPtr<FJsonObject> Toast = MakeShared<FJsonObject>();
		Toast->SetStringField(TEXT("level"), TEXT("error"));
		Toast->SetStringField(TEXT("text"), FString::Printf(TEXT("Could not save the %s: %s"), *Op->What.ToLower(), *Error));
		PushEvent(TEXT("toast"), JsonToString(Toast));
	}
	if (Op->RequestId.IsEmpty()) return;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("requestId"), Op->RequestId);
	Data->SetStringField(TEXT("status"), Status);
	if (bSaved)
	{
		FString Path = Op->ChosenPath;
		FPaths::MakePlatformFilename(Path);
		Data->SetStringField(TEXT("path"), Path);
	}
	else if (bFailed)
	{
		Data->SetStringField(TEXT("errorCode"), ErrorCode);
		Data->SetStringField(TEXT("error"), Error);
		if (HttpStatus > 0) Data->SetNumberField(TEXT("httpStatus"), HttpStatus);
	}
	PushEvent(TEXT("file_save_done"), JsonToString(Data));
}

// 7. Teardown: no renderer can receive these saves' events any more. Snapshot first, since
// settling edits the list.
void UNwiroBridge::AbortFileSaves()
{
	if (ActiveFileSaves.Num() == 0) return;
	TArray<TSharedPtr<FFileSaveOp>> Running;
	ActiveFileSaves.GenerateValueArray(Running);
	for (const TSharedPtr<FFileSaveOp>& Op : Running)
	{
		Op->bSuppressed = true;
		SettleFileSave(Op.ToSharedRef(), TEXT("cancelled"));
	}
	UE_LOG(LogTemp, Log, TEXT("Nwiro: SaveFileFromUrl stopped %d running save(s) at teardown"), Running.Num());
}
// <<< NWIRO_PARITY END: save-file-from-url-impl

void UNwiroBridge::ImportTextureBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId)
{
	// No map type: the texture keeps Unreal's import defaults (HDRI data-URL imports come here).
	ImportTextureBytesInternal(Base64Bytes, Ext, DestFolder, DestName, bRevealInBrowser, RequestId, FString());
}

static void ImportTextureBytesInternal(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId, const FString& MapType)
{
	if (Base64Bytes.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty())
	{
		EmitTextureImportDone(RequestId, false, TEXT(""), TEXT("empty bytes/folder/name"));
		return;
	}

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0)
	{
		if (UNwiroBridge::Instance)
		{
			UNwiroBridge::Instance->PushEvent(TEXT("toast"),
				TEXT("{\"level\":\"error\",\"text\":\"fal.ai import: invalid base64 payload\"}"));
		}
		EmitTextureImportDone(RequestId, false, TEXT(""), TEXT("invalid base64 payload"));
		return;
	}

	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("Nwiro"), TEXT("FalImports"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString FileName = FString::Printf(TEXT("%s_%lld.%s"), *DestName, FDateTime::UtcNow().ToUnixTimestamp(), *SafeExt);
	const FString LocalPath = FPaths::Combine(TempDir, FileName);
	if (!FFileHelper::SaveArrayToFile(Bytes, *LocalPath))
	{
		if (UNwiroBridge::Instance)
		{
			UNwiroBridge::Instance->PushEvent(TEXT("toast"),
				TEXT("{\"level\":\"error\",\"text\":\"fal.ai import: failed to write temp file\"}"));
		}
		EmitTextureImportDone(RequestId, false, TEXT(""), TEXT("failed to write temp file"));
		return;
	}

	const FString CapturedFolder = DestFolder;
	const FString CapturedName = DestName;
	const bool bCapturedReveal = bRevealInBrowser;
	const FString CapturedPath = LocalPath;
	const FString CapturedRequestId = RequestId;
	const FString CapturedMapType = MapType;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[CapturedPath, CapturedFolder, CapturedName, bCapturedReveal, CapturedRequestId, CapturedMapType](float) -> bool
	{
		FString DestPath = CapturedFolder;
		while (DestPath.EndsWith(TEXT("/"))) DestPath.RemoveAt(DestPath.Len() - 1);

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = CapturedPath;
		Task->DestinationPath = DestPath;
		Task->DestinationName = CapturedName;
		Task->bAutomated = true;
		// Explicit: the paths below are read right after ImportAssetTasks returns.
		Task->bAsync = false;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->AddToRoot();

		FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
		AT.Get().ImportAssetTasks(Tasks);

		const bool bAny = Task->ImportedObjectPaths.Num() > 0;
		FString AssetObjectPath;
		if (bAny) AssetObjectPath = Task->ImportedObjectPaths[0];

		// A material map's sRGB / compression / LOD group, by its type, before anyone sees the asset.
		const FString SettingsWarning = bAny ? ApplyMapTextureSettings(AssetObjectPath, CapturedMapType) : FString();

		if (bAny && bCapturedReveal)
		{
			FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			TArray<FAssetData> Assets;
			FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			for (const FString& ObjPath : Task->ImportedObjectPaths)
			{
				FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(ObjPath));
				if (AD.IsValid()) Assets.Add(AD);
			}
			if (Assets.Num() > 0) CB.Get().SyncBrowserToAssets(Assets);
			else                  CB.Get().SyncBrowserToFolders({ DestPath });
		}
		Task->RemoveFromRoot();

		if (UNwiroBridge::Instance)
		{
			const FString EscName  = CapturedName.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			const FString EscAsset = AssetObjectPath.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			const FString Data = FString::Printf(
				TEXT("{\"name\":\"%s\",\"assetPath\":\"%s\",\"imported\":%s}"),
				*EscName, *EscAsset, bAny ? TEXT("true") : TEXT("false"));
			UNwiroBridge::Instance->PushEvent(TEXT("fal_import"), Data);
		}

		// New import-completion contract: fire texture_import_done so callers
		// that supplied a RequestId can await real completion. The legacy
		// fal_import event above is preserved untouched for existing
		// consumers (no overlap — different event names).
		EmitTextureImportDone(
			CapturedRequestId,
			bAny,
			AssetObjectPath,
			bAny ? FString() : FString(TEXT("import task produced no assets")),
			SettingsWarning);

		return false;
	}));
}

FString UNwiroBridge::ResolveHdriOutputName(const FString& OutputPath, const FString& RequestedName)
{
	// Defensive: empty inputs just round-trip the request unchanged.
	if (OutputPath.IsEmpty() || RequestedName.IsEmpty())
	{
		return RequestedName;
	}

	// Strip any trailing slashes from OutputPath so the joined paths don't
	// accidentally become `/Game/HDRI//swiss_alps/...`.
	FString Base = OutputPath;
	while (Base.EndsWith(TEXT("/")))
	{
		Base = Base.LeftChop(1);
	}

	auto IsCandidateFree = [&Base](const FString& Name) -> bool
	{
		const FString Dir = Base + TEXT("/") + Name;
		const FString MatPath  = Dir + TEXT("/M_HDRI_Projection_") + Name;
		const FString RTPath   = Dir + TEXT("/RT_HDRI_") + Name;
		const FString CubePath = Dir + TEXT("/T_HDRI_") + Name;
		return !UEditorAssetLibrary::DoesAssetExist(MatPath)
			&& !UEditorAssetLibrary::DoesAssetExist(RTPath)
			&& !UEditorAssetLibrary::DoesAssetExist(CubePath);
	};

	if (IsCandidateFree(RequestedName))
	{
		return RequestedName;
	}
	for (int32 Suffix = 2; Suffix <= 99; ++Suffix)
	{
		const FString Candidate = FString::Printf(TEXT("%s_%d"), *RequestedName, Suffix);
		if (IsCandidateFree(Candidate))
		{
			UE_LOG(LogTemp, Log,
				TEXT("Nwiro [Bridge] ResolveHdriOutputName: bumped '%s' to '%s'"),
				*RequestedName, *Candidate);
			return Candidate;
		}
	}
	// Pathological: give up and return the original. The Python emitter has
	// its own auto-suffix pass which will report a clear ASSET_ALREADY_EXISTS
	// failure with the conflict list, rather than us silently misbehaving.
	UE_LOG(LogTemp, Warning,
		TEXT("Nwiro [Bridge] ResolveHdriOutputName: 99 suffix attempts all collided for base '%s'"),
		*RequestedName);
	return RequestedName;
}

void UNwiroBridge::SaveTextureToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext)
{
	if (Base64Bytes.IsEmpty()) return;
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0) return;

	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString Filter = FString::Printf(TEXT("Image (*.%s)|*.%s"), *SafeExt, *SafeExt);

	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return;
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
	TArray<FString> OutFiles;
	const bool bSaved = Desktop->SaveFileDialog(
		ParentWindow,
		TEXT("Save Texture"),
		FPaths::ProjectDir(),
		FString::Printf(TEXT("%s.%s"), *SuggestedName, *SafeExt),
		Filter,
		EFileDialogFlags::None,
		OutFiles
	);
	if (!bSaved || OutFiles.Num() == 0) return;

	FString Target = OutFiles[0];
	if (!Target.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase))
	{
		Target += TEXT(".");
		Target += SafeExt;
	}
	PushSavedFileToast(TEXT("Texture"), Target, FFileHelper::SaveArrayToFile(Bytes, *Target));
}

namespace
{
	// Sends the elevenlabs_import completion event: exactly once per import, either when an early
	// check fails or when the queued import has run. Payload: {name, assetPath, imported}, plus
	// "error" on failure and "requestId" when the caller supplied one. Callers that pass a request
	// id match the event by it; others can match it by asset name.
	void EmitAudioImport(const FString& Name, const FString& RequestId, bool bImported,
	                     const FString& AssetPath, const FString& Error)
	{
		if (!UNwiroBridge::Instance) return;
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), Name);
		Data->SetStringField(TEXT("assetPath"), AssetPath);
		Data->SetBoolField(TEXT("imported"), bImported);
		if (!Error.IsEmpty()) Data->SetStringField(TEXT("error"), Error);
		if (!RequestId.IsEmpty()) Data->SetStringField(TEXT("requestId"), RequestId);
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Data, Writer);
		UNwiroBridge::Instance->PushEvent(TEXT("elevenlabs_import"), Json);
	}

	// Ends an import before it reaches Unreal's importer: shows the error toast and sends the
	// completion event. Reason must be a fixed message without quotes, as it is formatted into the
	// toast's JSON directly.
	void FailAudioImport(const FString& Name, const FString& RequestId, const FString& Reason)
	{
		if (UNwiroBridge::Instance)
		{
			UNwiroBridge::Instance->PushEvent(TEXT("toast"),
				FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"ElevenLabs import: %s\"}"), *Reason));
		}
		EmitAudioImport(Name, RequestId, false, FString(), Reason);
	}
}

// Shared import path for ImportAudio (a local file, no request id) and ImportAudioBytes. Carries the
// request id into the queued import so that the completion event can report it.
static void ImportAudioInternal(const FString& LocalPath, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId);

void UNwiroBridge::ImportAudioBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId)
{
	if (Base64Bytes.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty())
	{
		// Invalid arguments: reported through the event only, without a toast.
		EmitAudioImport(DestName, RequestId, false, FString(), TEXT("empty bytes/folder/name"));
		return;
	}

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0)
	{
		FailAudioImport(DestName, RequestId, TEXT("invalid base64 payload"));
		return;
	}

	const FString SafeExt = Ext.IsEmpty() ? TEXT("mp3") : Ext.Replace(TEXT("."), TEXT(""));
	const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("Nwiro"), TEXT("ElevenLabsImports"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString FileName = FString::Printf(TEXT("%s_%lld.%s"), *DestName, FDateTime::UtcNow().ToUnixTimestamp(), *SafeExt);
	const FString LocalPath = FPaths::Combine(TempDir, FileName);

	if (!FFileHelper::SaveArrayToFile(Bytes, *LocalPath))
	{
		FailAudioImport(DestName, RequestId, TEXT("failed to write temp file"));
		return;
	}

	ImportAudioInternal(LocalPath, DestFolder, DestName, bRevealInBrowser, RequestId);
}

void UNwiroBridge::ImportAudio(const FString& LocalPath, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser)
{
	ImportAudioInternal(LocalPath, DestFolder, DestName, bRevealInBrowser, FString());
}

static void ImportAudioInternal(const FString& LocalPath, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId)
{
	if (LocalPath.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty())
	{
		EmitAudioImport(DestName, RequestId, false, FString(), TEXT("empty path/folder/name"));
		return;
	}
	if (!FPaths::FileExists(LocalPath))
	{
		FailAudioImport(DestName, RequestId, TEXT("source file missing"));
		return;
	}

	const FString CapturedPath = LocalPath;
	const FString CapturedFolder = DestFolder;
	const FString CapturedName = DestName;
	const bool bCapturedReveal = bRevealInBrowser;
	const FString CapturedRequestId = RequestId;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[CapturedPath, CapturedFolder, CapturedName, bCapturedReveal, CapturedRequestId](float) -> bool
	{
		FString DestPath = CapturedFolder;
		while (DestPath.EndsWith(TEXT("/"))) DestPath.RemoveAt(DestPath.Len() - 1);

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = CapturedPath;
		Task->DestinationPath = DestPath;
		Task->DestinationName = CapturedName;
		Task->bAutomated = true;
		// Explicit: the paths below are read right after ImportAssetTasks returns.
		Task->bAsync = false;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->AddToRoot();

		FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
		AT.Get().ImportAssetTasks(Tasks);

		const bool bAny = Task->ImportedObjectPaths.Num() > 0;
		FString AssetObjectPath;
		if (bAny) AssetObjectPath = Task->ImportedObjectPaths[0];

		if (bAny && bCapturedReveal)
		{
			FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			TArray<FAssetData> Assets;
			FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			for (const FString& ObjPath : Task->ImportedObjectPaths)
			{
				FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(ObjPath));
				if (AD.IsValid()) Assets.Add(AD);
			}
			if (Assets.Num() > 0) CB.Get().SyncBrowserToAssets(Assets);
			else                  CB.Get().SyncBrowserToFolders({ DestPath });
		}
		Task->RemoveFromRoot();

		EmitAudioImport(CapturedName, CapturedRequestId, bAny, AssetObjectPath,
			bAny ? FString() : FString(TEXT("import task produced no assets")));
		return false;
	}));
}

// ============================================================
// CONTENT PIPELINE BINDINGS
// ============================================================
#include "ContentPipeline/NwiroContentPipelineService.h"
#include "ContentPipeline/NwiroContentPipelineTypes.h"

FString UNwiroBridge::StartContentDownload(const FString& RequestJson)
{
	FNwiroContentRequest Req;
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}

	Json->TryGetStringField(TEXT("contentId"), Req.ContentId);
	Json->TryGetStringField(TEXT("hash"), Req.Hash);
	Json->TryGetStringField(TEXT("url"), Req.Url);
	Json->TryGetStringField(TEXT("sha256"), Req.ExpectedSHA256);
	Json->TryGetStringField(TEXT("md5"), Req.ExpectedMD5);
	Json->TryGetStringField(TEXT("importPath"), Req.RelativeImportPath);
	double SizeBytes = 0;
	if (Json->TryGetNumberField(TEXT("zipSize"), SizeBytes)) Req.ExpectedZipSizeBytes = (int64)SizeBytes;
	if (Json->TryGetNumberField(TEXT("unzipSize"), SizeBytes)) Req.EstimatedUnzippedSizeBytes = (int64)SizeBytes;

	FString Error;
	const bool bOk = FNwiroContentPipelineService::Get().StartDownloadAndImport(Req, Error);
	TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), bOk);
	Out->SetStringField(TEXT("hash"), Req.Hash);
	if (!bOk) Out->SetStringField(TEXT("error"), Error);
	return JsonToString(Out);
}

FString UNwiroBridge::GetContentStatus(const FString& Hash)
{
	return FNwiroContentPipelineService::Get().GetOperationStatusJson(TEXT(""), Hash);
}

bool UNwiroBridge::CancelContentDownload(const FString& Hash)
{
	FString Error;
	return FNwiroContentPipelineService::Get().CancelOperation(TEXT(""), Hash, Error);
}

FString UNwiroBridge::ListCachedContents()
{
	return FNwiroContentPipelineService::Get().GetCachedContentsJson();
}
