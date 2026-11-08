// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Misc/InteractiveProcess.h"
#include "Dom/JsonValue.h"
#include "NwiroBridge.generated.h"

// Per-chat session state
struct FChatSession
{
	FString ChatId;
	FString AdapterId;
	FString SessionId;
	bool bProcessing = false;
	FString PendingMessage;
	int32 SessionRpcId = 0;
	int32 PromptRpcId = 0;
	FString LastModel;
	TSet<FString> SeenToolCallIds;
};

struct FTerminalResult
{
	FString Output;
	int32 ExitCode = 0;
};

// ─── PORTING BANNER (codex-acp-string-id) ────────────────────────────────────
// Staging diff applied in nwiro-agents repo's plugin/ copy. Source-of-truth
// repo is leartes/nwiro/plugin — port this block + matching .cpp changes there
// before next `bun extract`, otherwise the fix is lost on round-trip.
// ─────────────────────────────────────────────────────────────────────────────
namespace Acp
{
	// JSON-RPC 2.0 permits `id` to be Number | String | Null (§4 Request object).
	// ACP's RequestId enum mirrors this: { Null, Number(i64), Str(String) }.
	// Bridge-originated ids stay int32 (NextRpcId++ controls them); this type
	// is only used on the inbound-request → outbound-response round-trip path
	// where the agent picks the wire shape and we must echo it verbatim.
	struct FRequestId
	{
		TSharedPtr<FJsonValue> Value;

		FRequestId() = default;
		explicit FRequestId(TSharedPtr<FJsonValue> InValue) : Value(MoveTemp(InValue)) {}

		bool IsValid() const { return Value.IsValid(); }
		bool IsNull() const { return !Value.IsValid() || Value->Type == EJson::Null; }

		FString ToLogString() const
		{
			if (!Value.IsValid()) return TEXT("<missing>");
			switch (Value->Type)
			{
				case EJson::Number: return FString::Printf(TEXT("%g"), Value->AsNumber());
				case EJson::String: return FString::Printf(TEXT("\"%s\""), *Value->AsString());
				case EJson::Null:   return TEXT("null");
				default:            return TEXT("<unknown>");
			}
		}
	};

	// Read the raw `id` field (Number | String | Null) from an inbound message.
	// Returns nullptr if the field is missing — callers treat that as a notification.
	inline TSharedPtr<FJsonValue> ExtractRpcId(const TSharedPtr<FJsonObject>& Msg)
	{
		if (!Msg.IsValid()) return nullptr;
		const TSharedPtr<FJsonValue>* Field = Msg->Values.Find(TEXT("id"));
		return Field ? *Field : nullptr;
	}
}

UCLASS()
class UNwiroBridge : public UObject
{
	GENERATED_BODY()

public:
	static UNwiroBridge* Instance;

	// JS bridge API
	UFUNCTION() void SetActiveChat(const FString& ChatId);
	UFUNCTION() void SendMessage(const FString& Message);
	UFUNCTION() void RespondToPermission(int32 RequestId, const FString& OptionId);
	UFUNCTION() void CancelMessage();
	UFUNCTION() void CloseChat(const FString& ChatId);
	UFUNCTION() void NewConversation();
	UFUNCTION() bool IsProcessing() const;
	UFUNCTION() FString PollResponse(); // Legacy — kept for compat but push is preferred
	UFUNCTION() bool IsTCPRunning() const;
	UFUNCTION() void Log(const FString& Text);
	UFUNCTION() FString GetPluginInfo() const;
	UFUNCTION() FString GetDisabledToolNamespaces() const;
	UFUNCTION() void SetDisabledToolNamespaces(const FString& JsonArray);
	UFUNCTION() bool GetToolbarButton() const;
	UFUNCTION() void SetToolbarButton(bool bEnabled);
	UFUNCTION() void SetMode(const FString& Mode);
	UFUNCTION() void SetModel(const FString& Model);
	UFUNCTION() FString GetMode() const;
	UFUNCTION() FString GetModel() const;
	UFUNCTION() void SetAdapter(const FString& Adapter);
	UFUNCTION() FString GetAdapter() const;
	UFUNCTION() void AddImagePath(const FString& Path);
	UFUNCTION() void OpenUrl(const FString& Url);
	UFUNCTION() void OpenImagePicker();
	// Open the picker on behalf of a NAMED owner (a chat, or the HDRI window's reference image
	// slot). OpenFileDialog is MODAL, so the owner has to be decided when the button is pressed,
	// not when the dialog returns. Unlike OpenImagePicker it stages nothing: the renderer sends
	// what is visible in its tray, like any pasted image.
	UFUNCTION() void OpenImagePickerForChat(const FString& ChatId);
	/** Where the page's image drop places are, `[{x0, y0, x1, y1}, ...]` as fractions of the
	 *  panel, so a file dragged in from the operating system is accepted only over them
	 *  (NwiroImageDrop.h). Malformed JSON keeps the previous places. */
	UFUNCTION() void SetImageDropRegions(const FString& Json);
	UFUNCTION() FString SaveImageBase64(const FString& Base64Data, const FString& FileName);
	UFUNCTION() void ClearImages();
	UFUNCTION() void DownloadAdapter(const FString& Adapter, const FString& Url);
	UFUNCTION() void DeleteAdapter(const FString& Adapter);
	UFUNCTION() bool IsAdapterAvailable(const FString& Adapter);
	UFUNCTION() bool IsCliAvailable(const FString& Adapter);
	UFUNCTION() void StartAdapter(const FString& Adapter);
	UFUNCTION() FString ExecuteTool(const FString& ToolName, const FString& ArgsJson);
	/** Async tool surface — returns an ack immediately; the result arrives as a
	 *  `tool_result` event correlated by RequestId. Required for ue.* tools that
	 *  defer their result, which cannot be answered by the synchronous
	 *  ExecuteTool above (CEF resolves the JS promise when it returns). */
	UFUNCTION() FString BeginExecuteTool(const FString& RequestId, const FString& ToolName, const FString& ArgsJson);
	/** Enabled tools only — feeds the chat/LLM pipeline. */
	UFUNCTION() FString GetToolDefinitions();
	/** Full inventory incl. disabled tools (flagged) — Tool Sources UI only. */
	UFUNCTION() FString GetToolDefinitionsForSettings();
	UFUNCTION() FString SearchAssets(const FString& Query);
	UFUNCTION() FString GetStaticMeshes();
	UFUNCTION() FString GetStaticMeshesByPath(const FString& FolderPath);
	UFUNCTION() bool ExecutePython(const FString& Code);
	// HDRI-only Python entry. Writes the script to a temp .py file and executes
	// the file instead of passing the source inline. The inline path
	// (`ExecPythonCommand(*Code)`) has a file-vs-inline heuristic that misfires
	// on large multi-line scripts (the HDRI bake recipe is ~700 lines), causing
	// "Could not load Python file '<cwd>/...'" errors. ExecutePython remains
	// untouched for Biome Generator and other small-script flows.
	//
	// Temp files default to delete-after-exec (success or failure). Launch with
	// `-NwiroKeepPythonScripts` to retain them at <Project>/Intermediate/Nwiro/
	// PythonExec/ for debugging.
	UFUNCTION() bool ExecutePythonHDRI(const FString& Code);
	UFUNCTION() bool SaveFile(const FString& Path, const FString& Content);
	UFUNCTION() void StartMCPServer(int32 Port);
	UFUNCTION() void StopMCPServer();
	UFUNCTION() void SetMCPPort(int32 Port);
	UFUNCTION() FString GetSecret(const FString& Key);
	UFUNCTION() void SetSecret(const FString& Key, const FString& Value);
	UFUNCTION() void SetChatExtensions(const FString& ChatId, const FString& ExtensionsJson);
	UFUNCTION() void SetAdapterContext(const FString& ContextJson);
	UFUNCTION() void ImportMesh(const FString& Url, const FString& FileName, const FString& DestFolder, bool bRevealInBrowser);
	UFUNCTION() void ImportAudio(const FString& LocalPath, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser);
	// RequestId (trailing, optional) — returned in the `elevenlabs_import` event, which is sent
	// exactly once per call: on any early failure and when the import has run.
	UFUNCTION() void ImportAudioBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId = TEXT(""));
	UFUNCTION() void SaveAudioToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext);
	// RequestId (trailing, optional) — if non-empty, a `texture_import_done`
	// event is fired with { requestId, success, assetPath, error } when the
	// import completes (or fails at any point). Existing call sites pass no
	// extra arg and get the legacy behavior unchanged.
	UFUNCTION() void ImportTextureBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId = TEXT(""));
	UFUNCTION() void SaveTextureToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext);
	// MapType (trailing, optional) — one of fal's PBR map types (basecolor, normal,
	// roughness, metalness, height): the imported texture then gets that map's sRGB,
	// compression and LOD group (tasks/fal-texture-import-settings-plan.md). Empty or
	// unknown keeps Unreal's import defaults, as before.
	UFUNCTION() void ImportTextureFromUrl(const FString& Url, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser, const FString& RequestId = TEXT(""), const FString& MapType = TEXT(""));
	UFUNCTION() void SaveTextureFromUrl(const FString& Url, const FString& SuggestedName, const FString& Ext);
	// >>> NWIRO_PARITY BEGIN: save-file-from-url-decl
	// Saves the file at an https link (a signed R2 link) to the file the user names in Unreal's
	// Save dialog. The dialog comes first; the plugin then downloads the file itself, writing it as
	// it arrives into a temporary file next to the chosen one, promoted only when everything worked.
	// An existing file the user chose (the dialog asks first) is replaced in one step at the end;
	// when that fails, the old file stays as it was. A redirected download is refused.
	// Kind: "video" (Ext mp4, mov) or "image" (png, jpg, jpeg, webp). With a non-empty RequestId the
	// panel gets file_save_progress { requestId, received, total } and exactly one file_save_done
	// { requestId, status: saved | cancelled | failed, path | errorCode, error, httpStatus }.
	// The name, arguments, events and codes are frozen with the 2026-10-27 plugin release
	// (design-and-implementation-plans/wme-scg/scene-gen-save-from-link-plan.md).
	UFUNCTION() void SaveFileFromUrl(const FString& Url, const FString& SuggestedName, const FString& Kind, const FString& Ext, const FString& RequestId);
private:
	struct FFileSaveOp;
	// Saves whose download is running, by operation id. Each ends through SettleFileSave, once,
	// on the game thread; AbortFileSaves ends them all silently when the panel goes away.
	TMap<FGuid, TSharedPtr<FFileSaveOp>> ActiveFileSaves;
	void FinishFileSaveDownload(const TSharedRef<FFileSaveOp>& Op, bool bSucceeded, int32 HttpStatus, const FString& EffectiveUrl);
	void SettleFileSave(const TSharedRef<FFileSaveOp>& Op, const FString& Status, const FString& ErrorCode = FString(), const FString& Error = FString(), int32 HttpStatus = 0);
	void AbortFileSaves();
public:
	// <<< NWIRO_PARITY END: save-file-from-url-decl

	/**
	 * Walk the HDRI output-name suffix chain (RequestedName, RequestedName_2,
	 * RequestedName_3, ...) and return the first one where none of the bake
	 * outputs (material, render target, texture cube) already exist under
	 * OutputPath. The frontend calls this BEFORE importing the source so the
	 * import lands in the resolved subfolder and doesn't overwrite a previous
	 * bake's source texture.
	 *
	 * Returns the resolved name on success; falls back to RequestedName if 99
	 * suffix attempts all collide. The Python emitter does its own auto-suffix
	 * pass too (race-safety), so this can be slightly stale without causing
	 * incorrect bakes — just a small chance of one extra suffix bump.
	 */
	UFUNCTION() FString ResolveHdriOutputName(const FString& OutputPath, const FString& RequestedName);

#if WITH_EDITOR
	// Editor-only test entry point. Feeds a synthetic JSON-RPC line into ProcessLine
	// so the union-aware id round-trip can be verified without a live ACP adapter.
	// See tasks/codex-acp-string-id.md "Verification plan".
	UFUNCTION(BlueprintCallable, CallInEditor, Category="Nwiro|Debug")
	void DebugInjectFrame(const FString& RawJsonLine);
#endif

	// Content Pipeline — download remote asset packs (PCG demo / Megascans-style bundles)
	// All return JSON strings; the JS side polls GetContentStatus until status is completed/failed.
	UFUNCTION() FString StartContentDownload(const FString& RequestJson);
	UFUNCTION() FString GetContentStatus(const FString& Hash);
	UFUNCTION() bool CancelContentDownload(const FString& Hash);
	UFUNCTION() FString ListCachedContents();


	bool IsChatExtensionEnabled(const FString& ExtName) const;
	/** What an "extension not enabled" refusal tells the user to do for a provider extension
	 *  (meshy, tripo, elevenlabs, fal): its workspace window in the workspace layout, the chat's
	 *  Extensions menu in Chat View. Built at call time, so it follows the last layout the app sent. */
	FString ExtensionAccessHint(const FString& ExtName) const;
	/** What the refusal of a *_managed (Nwiro Credits) tool tells the user: those tools run only in
	 *  the Nwiro app's workspace windows, never from Claude, Codex or Local LLM. Follows the layout. */
	FString ManagedToolAdvice(const FString& ToolName) const;

	void EnqueueResponse(const FString& Type, const FString& Data);
	void EnqueueResponse(const FString& Type, const FString& Data, const FString& ChatId);
	void PushEvent(const FString& Type, const FString& Data, const FString& ChatId = TEXT(""));
	FString GetChatIdForToolUse(const FString& ToolUseId) const;

	// Panel lifecycle. The bridge outlives the chat panel's browser widget: the
	// panel roots it for the editor's lifetime so adapter sessions and processes
	// survive a tab close (a closed tab used to garbage-collect the bridge and
	// lose every session). Detach cancels every in-flight turn and withdraws
	// pending permissions, because no renderer can answer them until the next
	// attach; events pushed while detached are dropped, not queued.
	void OnPanelAttached();
	void OnPanelDetached();
	// Stop every adapter process. Idempotent. Called from the panel's engine
	// pre-exit shutdown and from BeginDestroy.
	void StopAllAdapterProcesses(const FString& Reason);
	virtual void BeginDestroy() override;

private:
	void OpenImagePickerImpl(const FString& ChatId, bool bAddressed);
	// True between OnPanelDetached and OnPanelAttached: permission requests that
	// arrive in that window are withdrawn immediately instead of parked.
	bool bPanelDetached = false;
	// The one cancel implementation: session/cancel for the chat's session, turn
	// state cleared; CancelMessage adds the "Cancelled." notice for the active chat.
	void CancelChatTurnInternal(const FString& ChatId);
	// ACP `cancelled` outcome for a permission request that can no longer be answered.
	void CancelAcpPermission(const FString& AdapterId, const Acp::FRequestId& ReqId);
	// Per-adapter processes
	struct FAdapterProcess {
		TSharedPtr<FInteractiveProcess> Process;
		bool bInitialized = false;
		int32 InitRpcId = 0;
	};
	TMap<FString, FAdapterProcess> AdapterProcesses;
	int32 NextRpcId = 1;

	// Multi-session: one session per chat
	TMap<FString, FChatSession> ChatSessions;
	FString ActiveChatId;

	// Reverse lookup: RPC id -> chatId
	TMap<int32, FString> RpcToChatId;
	// Reverse lookup: sessionId -> chatId
	TMap<FString, FString> SessionToChatId;

	// Map toolUseId → chatId for MCP permission routing
	TMap<FString, FString> ToolUseToChatId;
	TSet<FString> BlockedShellToolCallIds;
	TMap<FString, FTerminalResult> TerminalResults;

	// ─── codex-acp-string-id: ACP permission round-trip ─────────────────────
	// React's permission card needs a stable int32 id (UFUNCTION contract).
	// ACP request ids may be Number | String | Null on the wire, so we mint a
	// local int32 LocalPermId for each card surfaced to React and remember the
	// original ACP id here.
	//
	// LocalPermId mints in the NEGATIVE int range (start -1, decrement) to keep
	// the ACP id space permanently disjoint from any positive-range permission
	// system (parity with NwiroIK's MCP-backed bridge).
	int32 NextLocalPermId = -1;
	TMap<int32, Acp::FRequestId> LocalPermIdToAcpReqId;
	TMap<int32, FString> LocalPermIdToAdapterId;

	// Settings
	FString CurrentAdapter = TEXT("nwiro");
	FString CurrentMode;
	FString CurrentModel;
	TArray<FString> PendingImages;
	TMap<FString, bool> ActiveChatExtensions; // extension name → enabled for active chat
	// The app layout sent with the same push as ActiveChatExtensions ("layout": "workspace").
	// Any push without it (Chat View, or an older app) resets it to false.
	bool bWorkspaceLayout = false;
	bool bBlockCommandExecution = true; // safety default: block shell commands from adapters

	// Response queue
	TArray<FString> ResponseQueue;
	FCriticalSection ResponseLock;

	// ACP protocol
	void EnsureProcess();
	void KillProcess();
	int32 SendRpc(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params = nullptr);
	void SendRpcNotification(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params);
	// Outbound bridge-originated responses (to bridge-minted numeric ids).
	void SendRpcResult(const FString& AdapterId, int32 Id, const TSharedPtr<FJsonObject>& Result);
	// Outbound responses to inbound agent-originated requests — preserves the
	// agent's original wire-type for the id (Number | String | Null).
	void SendRpcResult(const FString& AdapterId, const Acp::FRequestId& ReqId, const TSharedPtr<FJsonObject>& Result);
	void SendRpcError(const FString& AdapterId, const Acp::FRequestId& ReqId, int32 Code, const FString& Message);
	void RespondToAcpPermission(const FString& AdapterId, const Acp::FRequestId& ReqId, const FString& OptionId);
	void SendRaw(const FString& AdapterId, const FString& Json);
	void ProcessLine(const FString& AdapterId, const FString& Line);
	void HandleRpcResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error);
	void HandleMethod(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& FullMsg);
	void HandleSessionUpdate(const FString& AcpSessionId, const TSharedPtr<FJsonObject>& Update);

	void DoInitialize();
	void DoCreateSession(const FString& ChatId);
	void DoSendPrompt(const FString& ChatId, const FString& Message);

	FString FindAdapterBinary() const;
	FChatSession* GetActiveSession();
	FChatSession* FindSessionByAcpId(const FString& AcpSessionId);
	FString FindChatIdByRpcId(int32 RpcId);

	static FString JsonToString(const TSharedPtr<FJsonObject>& Obj);
};
