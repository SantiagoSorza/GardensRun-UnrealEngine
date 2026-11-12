// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroMeshyRetextureSpec.inl - the own-key Meshy texture tool's retexture request and the check of
// its source model (NwiroMeshyRetexture in NwiroToolDispatch.cpp). Included at the end of that
// file, so the file-static helpers are reachable. Mirrored in NwiroIKMeshyRetextureSpec.inl and
// checked by scripts/check-plugin-parity.ts.

#include "Misc/AutomationTest.h"

// >>> NWIRO_PARITY BEGIN: meshy-retexture-spec
namespace NwiroMeshyRetextureSpecUtil
{
	static TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	static TSharedPtr<FJsonObject> Args(const FString& Prompt, const FString& ModelId, const FString& ModelUrl)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("prompt"), Prompt);
		if (!ModelId.IsEmpty()) Object->SetStringField(TEXT("model_id"), ModelId);
		if (!ModelUrl.IsEmpty()) Object->SetStringField(TEXT("model_url"), ModelUrl);
		return Object;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMeshyRetextureRequestSpec,
	"Nwiro.MeshyRetexture.Request", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroMeshyRetextureRequestSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroMeshyRetextureSpecUtil;
	const FString Url = TEXT("https://example.com/chair.glb");

	{
		NwiroMeshyRetexture::FRequest Request;
		FString Error;
		TestTrue(TEXT("task id: accepted"), NwiroMeshyRetexture::BuildRequest(Args(TEXT("  rusty metal "), TEXT("task-1"), FString()), Request, Error));
		const TSharedPtr<FJsonObject> Body = ParseObject(Request.Body);
		if (!TestTrue(TEXT("task id: body is JSON"), Body.IsValid())) return false;
		TestEqual(TEXT("task id: input_task_id"), Body->GetStringField(TEXT("input_task_id")), FString(TEXT("task-1")));
		TestTrue(TEXT("task id: keeps the UV layout"), Body->GetBoolField(TEXT("enable_original_uv")));
		TestEqual(TEXT("task id: text_style_prompt trimmed"), Body->GetStringField(TEXT("text_style_prompt")), FString(TEXT("rusty metal")));
		TestEqual(TEXT("task id: ai_model"), Body->GetStringField(TEXT("ai_model")), FString(TEXT("meshy-6")));
		TestTrue(TEXT("task id: enable_pbr"), Body->GetBoolField(TEXT("enable_pbr")));
		const TArray<TSharedPtr<FJsonValue>>& Formats = Body->GetArrayField(TEXT("target_formats"));
		TArray<FString> FormatNames;
		for (const TSharedPtr<FJsonValue>& Format : Formats) FormatNames.Add(Format->AsString());
		TestEqual(TEXT("task id: target_formats"), FString::Join(FormatNames, TEXT(",")), FString(TEXT("glb,fbx,obj")));
		TestFalse(TEXT("task id: no model_url"), Body->HasField(TEXT("model_url")));
		TestFalse(TEXT("task id: no model_id"), Body->HasField(TEXT("model_id")));
		TestFalse(TEXT("task id: no prompt"), Body->HasField(TEXT("prompt")));
		TestEqual(TEXT("task id: event prompt"), Request.Prompt, FString(TEXT("rusty metal")));
		TestEqual(TEXT("task id: source id"), Request.InputTaskId, FString(TEXT("task-1")));
	}
	{
		NwiroMeshyRetexture::FRequest Request;
		FString Error;
		TestTrue(TEXT("URL only: accepted"), NwiroMeshyRetexture::BuildRequest(Args(TEXT("wood"), FString(), Url), Request, Error));
		const TSharedPtr<FJsonObject> Body = ParseObject(Request.Body);
		if (!TestTrue(TEXT("URL only: body is JSON"), Body.IsValid())) return false;
		TestEqual(TEXT("URL only: model_url"), Body->GetStringField(TEXT("model_url")), Url);
		TestFalse(TEXT("URL only: no input_task_id"), Body->HasField(TEXT("input_task_id")));
		TestFalse(TEXT("URL only: Meshy's UV default"), Body->HasField(TEXT("enable_original_uv")));
		TestTrue(TEXT("URL only: no source id"), Request.InputTaskId.IsEmpty());
	}
	{
		NwiroMeshyRetexture::FRequest Request;
		FString Error;
		TestTrue(TEXT("both: accepted"), NwiroMeshyRetexture::BuildRequest(Args(TEXT("wood"), TEXT("task-2"), Url), Request, Error));
		const TSharedPtr<FJsonObject> Body = ParseObject(Request.Body);
		if (!TestTrue(TEXT("both: body is JSON"), Body.IsValid())) return false;
		TestEqual(TEXT("both: input_task_id"), Body->GetStringField(TEXT("input_task_id")), FString(TEXT("task-2")));
		TestEqual(TEXT("both: model_url"), Body->GetStringField(TEXT("model_url")), Url);
		TestTrue(TEXT("both: keeps the UV layout"), Body->GetBoolField(TEXT("enable_original_uv")));
	}
	{
		NwiroMeshyRetexture::FRequest Request;
		FString Error;
		TestTrue(TEXT("800 characters: accepted"), NwiroMeshyRetexture::BuildRequest(Args(FString::ChrN(800, TEXT('a')), TEXT("task-1"), FString()), Request, Error));
		TestFalse(TEXT("801 characters: refused"), NwiroMeshyRetexture::BuildRequest(Args(FString::ChrN(801, TEXT('a')), TEXT("task-1"), FString()), Request, Error));
		TestEqual(TEXT("801 characters: message"), Error, FString(TEXT("prompt must be at most 800 characters")));
		TestFalse(TEXT("empty prompt: refused"), NwiroMeshyRetexture::BuildRequest(Args(TEXT("   "), TEXT("task-1"), FString()), Request, Error));
		TestEqual(TEXT("empty prompt: message"), Error, FString(TEXT("prompt is required")));
		TestFalse(TEXT("no model: refused"), NwiroMeshyRetexture::BuildRequest(Args(TEXT("wood"), FString(), FString()), Request, Error));
		TestEqual(TEXT("no model: message"), Error, FString(TEXT("Either model_url or model_id is required")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMeshyRetextureSourceCheckSpec,
	"Nwiro.MeshyRetexture.SourceCheck", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroMeshyRetextureSourceCheckSpec::RunTest(const FString& Parameters)
{
	using NwiroMeshyRetexture::ESourceCheck;
	using NwiroMeshyRetexture::DecideSourceCheck;
	// Meshy's answers to GET /openapi/v1/retexture/<id>, which returns any of the account's tasks.
	TestTrue(TEXT("a Text to 3D preview: submit"),
		DecideSourceCheck(200, TEXT("{\"id\":\"a\",\"type\":\"text-to-3d-preview\",\"status\":\"SUCCEEDED\"}")) == ESourceCheck::Submit);
	TestTrue(TEXT("a Text to 3D refine: submit"),
		DecideSourceCheck(200, TEXT("{\"id\":\"a\",\"type\":\"text-to-3d-refine\",\"status\":\"SUCCEEDED\"}")) == ESourceCheck::Submit);
	TestTrue(TEXT("an Image to 3D model: submit"),
		DecideSourceCheck(200, TEXT("{\"id\":\"a\",\"type\":\"image-to-3d\",\"status\":\"SUCCEEDED\"}")) == ESourceCheck::Submit);
	TestTrue(TEXT("a retexture task: refuse"),
		DecideSourceCheck(200, TEXT("{\"id\":\"a\",\"type\":\"retexture\",\"status\":\"SUCCEEDED\"}")) == ESourceCheck::RefuseTexture);
	TestTrue(TEXT("unknown to Meshy: submit, Meshy decides"),
		DecideSourceCheck(404, TEXT("{\"message\":\"Task not found\"}")) == ESourceCheck::Submit);
	TestTrue(TEXT("200 without a kind: stop"), DecideSourceCheck(200, TEXT("{\"id\":\"a\"}")) == ESourceCheck::Fail);
	TestTrue(TEXT("200 that is not JSON: stop"), DecideSourceCheck(200, TEXT("<html>")) == ESourceCheck::Fail);
	for (const int32 Status : { 0, 400, 401, 403, 429, 500, 503 })
	{
		TestTrue(*FString::Printf(TEXT("%d: no reliable answer, stop"), Status), DecideSourceCheck(Status, FString()) == ESourceCheck::Fail);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroMeshyRetextureModelResultSpec,
	"Nwiro.MeshyRetexture.ModelResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroMeshyRetextureModelResultSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroMeshyRetextureSpecUtil;
	const TSharedPtr<FJsonObject> Named = ParseObject(NwiroMeshyRetexture::ModelSubmittedResult(TEXT("task-9")));
	if (!TestTrue(TEXT("with a task id: JSON"), Named.IsValid())) return false;
	TestTrue(TEXT("with a task id: success"), Named->GetBoolField(TEXT("success")));
	TestEqual(TEXT("with a task id: still submitted"), Named->GetStringField(TEXT("status")), FString(TEXT("submitted")));
	TestEqual(TEXT("with a task id: meshyTaskId"), Named->GetStringField(TEXT("meshyTaskId")), FString(TEXT("task-9")));
	TestFalse(TEXT("with a task id: no taskId field"), Named->HasField(TEXT("taskId")));
	TestTrue(TEXT("with a task id: named in the message"), Named->GetStringField(TEXT("message")).Contains(TEXT("task-9")));

	const TSharedPtr<FJsonObject> Unnamed = ParseObject(NwiroMeshyRetexture::ModelSubmittedResult(FString()));
	if (!TestTrue(TEXT("without an answer in time: JSON"), Unnamed.IsValid())) return false;
	TestEqual(TEXT("without an answer in time: submitted"), Unnamed->GetStringField(TEXT("status")), FString(TEXT("submitted")));
	TestFalse(TEXT("without an answer in time: no meshyTaskId"), Unnamed->HasField(TEXT("meshyTaskId")));
	return true;
}
// <<< NWIRO_PARITY END: meshy-retexture-spec
