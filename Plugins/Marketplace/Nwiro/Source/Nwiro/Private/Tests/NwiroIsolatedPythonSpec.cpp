// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroSettingsTools.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace NwiroIsolatedPythonTests
{
	TSharedPtr<FJsonObject> Call(const TSharedRef<FJsonObject>& Request)
	{
		FString Input;
		FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Input));
		const FString Output = FNwiroSettingsTools::ExecutePython(Input);
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Output), Result);
		return Result;
	}

	FString Literal(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("'"), TEXT("\\'"));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		return TEXT("'") + Value + TEXT("'");
	}

	class FWaitForJob final : public IAutomationLatentCommand
	{
	public:
		FWaitForJob(FAutomationTestBase* InTest, FString InId, FString InCase, FString InMarker, FString InChildPid)
			: Test(InTest), Id(MoveTemp(InId)), Case(MoveTemp(InCase)), Marker(MoveTemp(InMarker)), ChildPid(MoveTemp(InChildPid))
		{
		}

		virtual bool Update() override
		{
			TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetStringField(TEXT("execution_mode"), TEXT("isolated"));
			Request->SetStringField(TEXT("job_id"), Id);
			const bool bCancel = !bCancelled && ((Case == TEXT("Cancel") && FPaths::FileExists(ChildPid))
				|| FPlatformTime::Seconds() - Started > 150);
			Request->SetStringField(TEXT("action"), bCancel ? TEXT("cancel") : TEXT("status"));
			if (bCancel) bCancelled = true;
			auto Result = Call(Request);
			if (!Test->TestTrue(TEXT("Polling returns a JSON object"), Result.IsValid())) return true;
			if (!Result->GetBoolField(TEXT("completed"))) return false;

			const bool bSuccessful = Case == TEXT("Success") || Case == TEXT("ChildAfterReturn");
			const FString Expected = bSuccessful ? TEXT("completed")
				: (Case == TEXT("Exception") || Case == TEXT("UnicodeException")) ? TEXT("failed")
				: Case == TEXT("Crash") ? TEXT("crashed")
				: Case == TEXT("Timeout") ? TEXT("timed_out") : TEXT("cancelled");
			Test->TestEqual(TEXT("Terminal state matches the observed execution"), Result->GetStringField(TEXT("status")), Expected);
			Test->TestEqual(TEXT("Only clean completion is successful"), Result->GetBoolField(TEXT("success")), bSuccessful);
			Test->TestTrue(TEXT("User code really started before the terminal outcome"), FPaths::FileExists(Marker));
			if (Case == TEXT("Crash"))
			{
				double ExitCode = 0;
				Test->TestTrue(TEXT("Abrupt termination exposes the worker exit code"), Result->TryGetNumberField(TEXT("exit_code"), ExitCode));
				Test->TestEqual(TEXT("Abrupt termination is the requested exit, not an unrelated crash"), ExitCode, 47.0);
			}
			Test->TestEqual(TEXT("No result promises rollback"), Result->GetStringField(TEXT("side_effects")), FString(TEXT("not_rolled_back")));
			if (Case == TEXT("Success")) Test->TestEqual(TEXT("Captured Python output"), Result->GetStringField(TEXT("output")), FString(TEXT("isolated-proof\n")));
			if (Case == TEXT("Exception")) Test->TestTrue(TEXT("Python exception is reported"), Result->GetStringField(TEXT("error")).Contains(TEXT("proof-error")));
			if (Case == TEXT("UnicodeException"))
			{
				Test->TestTrue(TEXT("Large Unicode output is bounded"), Result->GetBoolField(TEXT("output_truncated")));
				Test->TestTrue(TEXT("Handled Unicode exception stays readable"), Result->GetStringField(TEXT("error")).Contains(TEXT("\U0001F680")));
			}
			if (Case == TEXT("Timeout") || Case == TEXT("Cancel") || Case == TEXT("ChildAfterReturn"))
			{
				FString PidText;
				Test->TestTrue(TEXT("External child really started"), FFileHelper::LoadFileToString(PidText, *ChildPid));
				const uint32 Pid = FCString::Atoi(*PidText);
				Test->TestTrue(TEXT("Child identity was captured"), Pid > 0);
				Test->TestFalse(TEXT("Owned external child is terminated"), Pid > 0 && FPlatformProcess::IsApplicationRunning(Pid));
			}
			auto Live = MakeShared<FJsonObject>();
			Live->SetStringField(TEXT("code"), TEXT("print('live-editor-still-responsive')"));
			auto LiveResult = Call(Live);
			Test->TestTrue(TEXT("Parent live Python remains responsive"), LiveResult && LiveResult->GetBoolField(TEXT("success"))
				&& LiveResult->GetStringField(TEXT("output")).Contains(TEXT("live-editor-still-responsive")));
			Request->SetStringField(TEXT("action"), TEXT("status"));
			auto Again = Call(Request);
			Test->TestTrue(TEXT("Terminal result remains queryable"), Again && Again->GetStringField(TEXT("status")) == Expected);
			return true;
		}
	private:
		FAutomationTestBase* Test;
		FString Id;
		FString Case;
		FString Marker;
		FString ChildPid;
		double Started = FPlatformTime::Seconds();
		bool bCancelled = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroIsolatedPythonArguments,
	"Nwiro.Pro.Python.Isolated.Arguments", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNwiroIsolatedPythonArguments::RunTest(const FString& Parameters)
{
	struct FInvalidCase { const TCHAR* Name; const TCHAR* Input; const TCHAR* Error; };
	const TArray<FInvalidCase> Invalid = {
		{TEXT("execution_mode/number"), TEXT(R"JSON({"execution_mode":42,"code":"pass"})JSON"), TEXT("execution_mode must be live or isolated.")},
		{TEXT("default_live/code/number"), TEXT(R"JSON({"code":42})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("explicit_live/code/number"), TEXT(R"JSON({"execution_mode":"live","code":42})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("isolated_start/code/number"), TEXT(R"JSON({"execution_mode":"isolated","code":42})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("isolated/action/number"), TEXT(R"JSON({"execution_mode":"isolated","action":42,"code":"pass"})JSON"), TEXT("action must be start, status, or cancel.")},
		{TEXT("status/job_id/number"), TEXT(R"JSON({"execution_mode":"isolated","action":"status","job_id":42})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("cancel/job_id/number"), TEXT(R"JSON({"execution_mode":"isolated","action":"cancel","job_id":42})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("execution_mode/boolean"), TEXT(R"JSON({"execution_mode":true,"code":"pass"})JSON"), TEXT("execution_mode must be live or isolated.")},
		{TEXT("default_live/code/boolean"), TEXT(R"JSON({"code":true})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("explicit_live/code/boolean"), TEXT(R"JSON({"execution_mode":"live","code":true})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("isolated_start/code/boolean"), TEXT(R"JSON({"execution_mode":"isolated","code":true})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("isolated/action/boolean"), TEXT(R"JSON({"execution_mode":"isolated","action":true,"code":"pass"})JSON"), TEXT("action must be start, status, or cancel.")},
		{TEXT("status/job_id/boolean"), TEXT(R"JSON({"execution_mode":"isolated","action":"status","job_id":true})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("cancel/job_id/boolean"), TEXT(R"JSON({"execution_mode":"isolated","action":"cancel","job_id":true})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("execution_mode/null"), TEXT(R"JSON({"execution_mode":null,"code":"pass"})JSON"), TEXT("execution_mode must be live or isolated.")},
		{TEXT("default_live/code/null"), TEXT(R"JSON({"code":null})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("explicit_live/code/null"), TEXT(R"JSON({"execution_mode":"live","code":null})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("isolated_start/code/null"), TEXT(R"JSON({"execution_mode":"isolated","code":null})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("isolated/action/null"), TEXT(R"JSON({"execution_mode":"isolated","action":null,"code":"pass"})JSON"), TEXT("action must be start, status, or cancel.")},
		{TEXT("status/job_id/null"), TEXT(R"JSON({"execution_mode":"isolated","action":"status","job_id":null})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("cancel/job_id/null"), TEXT(R"JSON({"execution_mode":"isolated","action":"cancel","job_id":null})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("execution_mode/array"), TEXT(R"JSON({"execution_mode":[],"code":"pass"})JSON"), TEXT("execution_mode must be live or isolated.")},
		{TEXT("default_live/code/array"), TEXT(R"JSON({"code":[]})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("explicit_live/code/array"), TEXT(R"JSON({"execution_mode":"live","code":[]})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("isolated_start/code/array"), TEXT(R"JSON({"execution_mode":"isolated","code":[]})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("isolated/action/array"), TEXT(R"JSON({"execution_mode":"isolated","action":[],"code":"pass"})JSON"), TEXT("action must be start, status, or cancel.")},
		{TEXT("status/job_id/array"), TEXT(R"JSON({"execution_mode":"isolated","action":"status","job_id":[]})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("cancel/job_id/array"), TEXT(R"JSON({"execution_mode":"isolated","action":"cancel","job_id":[]})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("execution_mode/object"), TEXT(R"JSON({"execution_mode":{},"code":"pass"})JSON"), TEXT("execution_mode must be live or isolated.")},
		{TEXT("default_live/code/object"), TEXT(R"JSON({"code":{}})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("explicit_live/code/object"), TEXT(R"JSON({"execution_mode":"live","code":{}})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("isolated_start/code/object"), TEXT(R"JSON({"execution_mode":"isolated","code":{}})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("isolated/action/object"), TEXT(R"JSON({"execution_mode":"isolated","action":{},"code":"pass"})JSON"), TEXT("action must be start, status, or cancel.")},
		{TEXT("status/job_id/object"), TEXT(R"JSON({"execution_mode":"isolated","action":"status","job_id":{}})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("cancel/job_id/object"), TEXT(R"JSON({"execution_mode":"isolated","action":"cancel","job_id":{}})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("isolated/timeout/numeric_string"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":"120"})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("isolated/timeout/boolean_true"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":true})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("isolated/timeout/boolean_false"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":false})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("isolated/timeout/null"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":null})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("isolated/timeout/array"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":[]})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("isolated/timeout/object"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":{}})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("execution_mode/unknown"), TEXT(R"JSON({"execution_mode":"wrong","code":"pass"})JSON"), TEXT("execution_mode must be live or isolated.")},
		{TEXT("isolated/action/unknown"), TEXT(R"JSON({"execution_mode":"isolated","action":"other","code":"pass"})JSON"), TEXT("action must be start, status, or cancel.")},
		{TEXT("isolated/code/whitespace"), TEXT(R"JSON({"execution_mode":"isolated","code":"  "})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("isolated/code/missing"), TEXT(R"JSON({"execution_mode":"isolated"})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
		{TEXT("default_live/code/missing"), TEXT(R"JSON({})JSON"), TEXT("code must be a nonempty string.")},
		{TEXT("default_live/code/empty"), TEXT(R"JSON({"code":""})JSON"), TEXT("code is required")},
		{TEXT("isolated/timeout/range_0"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":0})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("isolated/timeout/range_3601"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","timeout_seconds":3601})JSON"), TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup.")},
		{TEXT("status/unknown_job"), TEXT(R"JSON({"execution_mode":"isolated","action":"status","job_id":"missing"})JSON"), TEXT("Unknown or expired job_id in this editor session.")},
		{TEXT("cancel/forbidden_code"), TEXT(R"JSON({"execution_mode":"isolated","action":"cancel","job_id":"missing","code":"pass"})JSON"), TEXT("status/cancel requires job_id and does not accept code or timeout_seconds.")},
		{TEXT("live/forbidden_timeout_seconds"), TEXT(R"JSON({"code":"pass","timeout_seconds":1})JSON"), TEXT("action, job_id and timeout_seconds require execution_mode=isolated.")},
		{TEXT("live/forbidden_action"), TEXT(R"JSON({"code":"pass","action":"start"})JSON"), TEXT("action, job_id and timeout_seconds require execution_mode=isolated.")},
		{TEXT("live/forbidden_job_id"), TEXT(R"JSON({"code":"pass","job_id":"unexpected"})JSON"), TEXT("action, job_id and timeout_seconds require execution_mode=isolated.")},
		{TEXT("start/forbidden_job_id"), TEXT(R"JSON({"execution_mode":"isolated","code":"pass","job_id":"unexpected"})JSON"), TEXT("start requires nonempty code (at most 1048576 characters), without job_id.")},
	};
	const FString JobsRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Nwiro/IsolatedPython"));
	const bool bRootExisted = IFileManager::Get().DirectoryExists(*JobsRoot);
	TArray<FString> BeforeJobs;
	IFileManager::Get().FindFiles(BeforeJobs, *(JobsRoot / TEXT("*")), false, true);
	BeforeJobs.Sort();
	for (const FInvalidCase& Case : Invalid)
	{
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroSettingsTools::ExecutePython(Case.Input)), Result);
		const FString Label = FString(TEXT("argument case ")) + Case.Name;
		TestTrue(Label + TEXT(" fails without allocating a job"), Result && Result->HasTypedField<EJson::Boolean>(TEXT("success"))
			&& !Result->GetBoolField(TEXT("success")) && !Result->HasField(TEXT("job_id")));
		if (Result) TestEqual(Label + TEXT(" reports its validation boundary"), Result->GetStringField(TEXT("error")), FString(Case.Error));
		TArray<FString> AfterJobs;
		IFileManager::Get().FindFiles(AfterJobs, *(JobsRoot / TEXT("*")), false, true);
		AfterJobs.Sort();
		TestTrue(Label + TEXT(" creates no worker directory"), AfterJobs == BeforeJobs
			&& IFileManager::Get().DirectoryExists(*JobsRoot) == bRootExisted);
		// If a future regression allocates a job, cancel that exact unexpected
		// job immediately. The failing assertions above remain the test outcome.
		FString UnexpectedJob;
		if (Result && Result->TryGetStringField(TEXT("job_id"), UnexpectedJob) && !UnexpectedJob.IsEmpty())
		{
			auto Cancel = MakeShared<FJsonObject>();
			Cancel->SetStringField(TEXT("execution_mode"), TEXT("isolated"));
			Cancel->SetStringField(TEXT("action"), TEXT("cancel"));
			Cancel->SetStringField(TEXT("job_id"), UnexpectedJob);
			NwiroIsolatedPythonTests::Call(Cancel);
		}
	}

	// The harmless comment is also caught by the existing crash-pattern guard.
	// It proves the inclusive length boundary without executing a megabyte script.
	const FString CodePrefix = TEXT("# compile_blueprint: pre-execution length proof\n#");
	for (bool bExplicitLive : { false, true })
	{
		for (int32 CodeLength : { 1048575, 1048576, 1048577 })
		{
			const FString Label = FString::Printf(TEXT("live code length %d (%s)"), CodeLength,
				bExplicitLive ? TEXT("explicit") : TEXT("default"));
			auto Request = MakeShared<FJsonObject>();
			if (bExplicitLive) Request->SetStringField(TEXT("execution_mode"), TEXT("live"));
			Request->SetStringField(TEXT("code"), CodePrefix + FString::ChrN(CodeLength - CodePrefix.Len(), TEXT('x')));
			auto Result = NwiroIsolatedPythonTests::Call(Request);
			TestTrue(Label + TEXT(" rejects before Python execution or job allocation"), Result
				&& Result->HasTypedField<EJson::Boolean>(TEXT("success")) && !Result->GetBoolField(TEXT("success"))
				&& !Result->HasField(TEXT("job_id")) && !Result->HasField(TEXT("output")));
			if (Result)
			{
				const FString Error = Result->GetStringField(TEXT("error"));
				if (CodeLength > 1048576)
					TestEqual(Label + TEXT(" reports the size boundary"), Error, FString(TEXT("code exceeds the 1048576-character limit.")));
				else
					TestTrue(Label + TEXT(" reaches the existing pre-execution guard"), Error.StartsWith(TEXT("execute_python rejected: BlueprintEditorLibrary.compile_blueprint")));
			}
			TArray<FString> AfterJobs;
			IFileManager::Get().FindFiles(AfterJobs, *(JobsRoot / TEXT("*")), false, true);
			AfterJobs.Sort();
			TestTrue(Label + TEXT(" creates no worker directory"), AfterJobs == BeforeJobs
				&& IFileManager::Get().DirectoryExists(*JobsRoot) == bRootExisted);
		}
	}
	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FNwiroIsolatedPythonLifecycle,
	"Nwiro.Pro.Python.Isolated.Lifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FNwiroIsolatedPythonLifecycle::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const FString Case : { TEXT("Success"), TEXT("Exception"), TEXT("Crash"), TEXT("Timeout"), TEXT("Cancel"), TEXT("ChildAfterReturn"), TEXT("UnicodeException") })
	{
		Names.Add(Case);
		Commands.Add(Case);
	}
}
bool FNwiroIsolatedPythonLifecycle::RunTest(const FString& Case)
{
#if !PLATFORM_WINDOWS
	AddWarning(TEXT("Isolated process containment requires Windows."));
	return true;
#else
	using namespace NwiroIsolatedPythonTests;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Nwiro/IsolatedPythonProof") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!TestTrue(TEXT("Fresh proof directory"), !FPaths::DirectoryExists(Dir) && IFileManager::Get().MakeDirectory(*Dir, true))) return false;
	const FString Marker = Dir / TEXT("started.txt");
	const FString ChildPid = Dir / TEXT("child-pid.txt");
	FString Code = TEXT("import pathlib\npathlib.Path(") + Literal(Marker) + TEXT(").write_text('started', encoding='utf-8')\n");
	if (Case == TEXT("Success")) Code += TEXT("print('isolated-proof')\n");
	else if (Case == TEXT("Exception")) Code += TEXT("raise RuntimeError('proof-error')\n");
	else if (Case == TEXT("UnicodeException")) Code += TEXT("print(chr(0x1f680)*65536)\nraise RuntimeError(chr(0x1f680)*65536)\n");
	else if (Case == TEXT("Crash")) Code += TEXT("import ctypes\nk=ctypes.WinDLL('kernel32')\nk.GetCurrentProcess.restype=ctypes.c_void_p\nk.TerminateProcess.argtypes=[ctypes.c_void_p,ctypes.c_uint]\nk.TerminateProcess(k.GetCurrentProcess(),47)\n");
	else
	{
		const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
		if (!TestTrue(TEXT("Independent Python executable exists"), FPaths::FileExists(Python))) return false;
		const FString ChildCode = TEXT("import os,pathlib,time; pathlib.Path(") + Literal(ChildPid) + TEXT(").write_text(str(os.getpid()),encoding='utf-8'); time.sleep(600)");
		Code += TEXT("import subprocess,time\nsubprocess.Popen([") + Literal(Python) + TEXT(", '-c', ") + Literal(ChildCode)
			+ TEXT("], creationflags=subprocess.CREATE_NO_WINDOW)\n");
		if (Case == TEXT("ChildAfterReturn"))
			Code += TEXT("while not pathlib.Path(") + Literal(ChildPid) + TEXT(").exists():\n    time.sleep(0.01)\n");
		else
			Code += TEXT("time.sleep(600)\n");
	}
	auto Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("execution_mode"), TEXT("isolated"));
	Request->SetStringField(TEXT("code"), Code);
	Request->SetNumberField(TEXT("timeout_seconds"), Case == TEXT("Timeout") ? 90 : 120);
	auto Result = Call(Request);
	if (!TestTrue(TEXT("Worker request accepted"), Result && Result->GetBoolField(TEXT("success")) && Result->HasField(TEXT("job_id")))) return false;
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForJob(this, Result->GetStringField(TEXT("job_id")), Case, Marker, ChildPid));
	return true;
#endif
}
#endif
