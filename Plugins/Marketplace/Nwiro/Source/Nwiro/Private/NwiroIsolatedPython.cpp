// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIsolatedPython.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace NwiroIsolatedPython
{
namespace
{
	constexpr int32 MaxOutputCharacters = 64 * 1024;
	// Two bounded 64K Python strings can each escape to 12 bytes per Unicode scalar.
	constexpr int64 MaxWorkerResponseBytes = 2 * 1024 * 1024;
	constexpr int32 MaxRetainedJobs = 32;

	FString Json(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
		return Result;
	}

	FString Error(const FString& Message)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("execution_mode"), TEXT("isolated"));
		Result->SetStringField(TEXT("error"), Message);
		return Json(Result);
	}

	// Python writes a bounded response atomically. A missing response on exit is never
	// interpreted as successful execution. This is process isolation, not a file sandbox.
	const TCHAR* Wrapper = TEXT(R"PY(import contextlib
import json
import pathlib
import traceback

base = pathlib.Path(__file__).resolve().parent
class BoundedOutput:
    def __init__(self):
        self.parts = []
        self.length = 0
        self.truncated = False
    def write(self, value):
        value = str(value)
        room = max(0, 65536 - self.length)
        self.parts.append(value[:room]) if room else None
        self.length += min(len(value), room)
        self.truncated = self.truncated or len(value) > room
        return len(value)
    def flush(self):
        pass
capture = BoundedOutput()
response = {"success": False}
try:
    source = (base / "code.py").read_text(encoding="utf-8")
    with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
        exec(compile(source, str(base / "code.py"), "exec"), {"__name__": "__main__", "__file__": str(base / "code.py")})
    response["success"] = True
except BaseException:
    response["error"] = traceback.format_exc()[-65536:]
response["output"] = "".join(capture.parts)
response["output_truncated"] = capture.truncated
temporary = base / "result.tmp"
temporary.write_text(json.dumps(response, ensure_ascii=True), encoding="utf-8")
temporary.replace(base / "result.json")
)PY");

	class FJob final : public FRunnable
	{
	public:
		FString Id;
		FString Directory;
		FString Project;
		FString Executable;
		double TimeoutSeconds = 120;
		double CreatedAt = FPlatformTime::Seconds();
		FRunnableThread* Thread = nullptr;

		~FJob()
		{
			Stop();
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
			}
		}

		virtual void Stop() override
		{
			FScopeLock Lock(&Mutex);
			bCancelRequested = true;
		}

		bool IsDone()
		{
			FScopeLock Lock(&Mutex);
			return bDone;
		}

		TSharedRef<FJsonObject> Snapshot()
		{
			FScopeLock Lock(&Mutex);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), !bDone || Status == TEXT("completed"));
			Result->SetStringField(TEXT("execution_mode"), TEXT("isolated"));
			Result->SetStringField(TEXT("job_id"), Id);
			Result->SetStringField(TEXT("status"), Status);
			Result->SetBoolField(TEXT("completed"), bDone);
			Result->SetStringField(TEXT("state_scope"), TEXT("saved_project"));
			Result->SetStringField(TEXT("side_effects"), TEXT("not_rolled_back"));
			Result->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
			Result->SetNumberField(TEXT("process_id"), ProcessId);
			if (bHasExitCode) Result->SetNumberField(TEXT("exit_code"), ExitCode);
			if (!Failure.IsEmpty()) Result->SetStringField(TEXT("error"), Failure);
			if (bDone)
			{
				Result->SetStringField(TEXT("output"), Output);
				Result->SetBoolField(TEXT("output_truncated"), bOutputTruncated);
			}
			return Result;
		}

		virtual uint32 Run() override
		{
			if (Cancelled())
			{
				Finish(TEXT("cancelled"), TEXT("Cancelled before the worker started."));
				return 0;
			}
#if PLATFORM_WINDOWS
			// Start suspended and assign a kill-on-close Job before any child code runs.
			// Descendants cannot escape via a fast spawn or outlive a normally exiting root.
			HANDLE JobObject = ::CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = {};
			Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!JobObject || !::SetInformationJobObject(JobObject, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
			{
				if (JobObject) ::CloseHandle(JobObject);
				Finish(TEXT("failed"), TEXT("Could not establish worker process containment."));
				return 0;
			}
			const FString Args = FString::Printf(
				TEXT("\"%s\" \"%s\" -run=pythonscript -script=\"%s\" -NwiroIsolatedWorker -unattended -nullrhi -nosound -nosplash -NoLiveCoding -nop4 -UTF8Output -abslog=\"%s\" -EditorPerProjectUserSettingsINI=\"%s\" -EditorLayoutINI=\"%s\""),
				*Executable, *Project, *(Directory / TEXT("worker.py")), *(Directory / TEXT("editor.log")),
				*(Directory / TEXT("EditorPerProjectUserSettings.ini")), *(Directory / (TEXT("NwiroPython_") + Id + TEXT(".ini"))));
			TArray<TCHAR> CommandLine = Args.GetCharArray();
			STARTUPINFOW Startup = {};
			Startup.cb = sizeof(Startup);
			Startup.dwFlags = STARTF_USESHOWWINDOW;
			Startup.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION ProcessInfo = {};
			if (!::CreateProcessW(*Executable, CommandLine.GetData(), nullptr, nullptr, false,
				CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, *FPaths::GetPath(Project), &Startup, &ProcessInfo))
			{
				::CloseHandle(JobObject);
				Finish(TEXT("failed"), TEXT("Could not launch the isolated editor commandlet."));
				return 0;
			}
			if (!::AssignProcessToJobObject(JobObject, ProcessInfo.hProcess))
			{
				::TerminateProcess(ProcessInfo.hProcess, 1);
				::CloseHandle(ProcessInfo.hThread);
				::CloseHandle(ProcessInfo.hProcess);
				::CloseHandle(JobObject);
				Finish(TEXT("failed"), TEXT("Worker containment failed; suspended process was terminated before execution."));
				return 0;
			}
			FProcHandle Process(ProcessInfo.hProcess);
			{
				FScopeLock Lock(&Mutex);
				ProcessId = ProcessInfo.dwProcessId;
				Status = TEXT("running");
			}
			const bool bResumed = ::ResumeThread(ProcessInfo.hThread) != static_cast<DWORD>(-1);
			::CloseHandle(ProcessInfo.hThread);
			FString EndStatus;
			FString EndError;
			if (!bResumed)
			{
				EndStatus = TEXT("failed");
				EndError = TEXT("Could not resume the contained worker.");
			}
			while (bResumed && FPlatformProcess::IsProcRunning(Process))
			{
				if (Cancelled())
				{
					EndStatus = TEXT("cancelled");
					EndError = TEXT("Worker cancelled; prior file or asset writes may remain.");
					break;
				}
				if (FPlatformTime::Seconds() - CreatedAt >= TimeoutSeconds)
				{
					EndStatus = TEXT("timed_out");
					EndError = TEXT("Worker deadline exceeded; prior file or asset writes may remain.");
					break;
				}
				FPlatformProcess::Sleep(0.05f);
			}
			// Termination is asynchronous. Confirm root AND descendant exit before
			// publishing a terminal result, without an infinite wait during unload.
			const bool bTerminationRequested = ::TerminateJobObject(JobObject, 1) != 0;
			const double CleanupDeadline = FPlatformTime::Seconds() + 5.0;
			int32 Code = 0;
			bool bGotCode = false;
			bool bCleanupConfirmed = false;
			while (FPlatformTime::Seconds() < CleanupDeadline)
			{
				if (Process.IsValid() && !FPlatformProcess::IsProcRunning(Process))
				{
					bGotCode = FPlatformProcess::GetProcReturnCode(Process, &Code);
					FPlatformProcess::CloseProc(Process);
				}
				JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting = {};
				if (::QueryInformationJobObject(JobObject, JobObjectBasicAccountingInformation,
					&Accounting, sizeof(Accounting), nullptr) && Accounting.ActiveProcesses == 0)
				{
					bCleanupConfirmed = true;
					break;
				}
				FPlatformProcess::Sleep(0.01f);
			}
			if (Process.IsValid())
			{
				bGotCode = FPlatformProcess::GetProcReturnCode(Process, &Code);
				FPlatformProcess::CloseProc(Process);
			}
			::CloseHandle(JobObject); // kill-on-close remains the final fallback
			if (!bCleanupConfirmed)
			{
				Finish(TEXT("cleanup_failed"), bTerminationRequested
					? TEXT("Worker termination was requested but process-tree exit could not be confirmed; side effects remain unknown.")
					: TEXT("Worker termination failed and process-tree exit could not be confirmed; side effects remain unknown."));
				return 0;
			}
			{
				FScopeLock Lock(&Mutex);
				bHasExitCode = bGotCode;
				ExitCode = Code;
			}
			if (!EndStatus.IsEmpty())
			{
				Finish(EndStatus, EndError);
				return 0;
			}

			const FString ResultPath = Directory / TEXT("result.json");
			const int64 ResultSize = IFileManager::Get().FileSize(*ResultPath);
			FString ResultText;
			TSharedPtr<FJsonObject> Response;
			if (ResultSize < 0 || ResultSize > MaxWorkerResponseBytes
				|| !FFileHelper::LoadFileToString(ResultText, *ResultPath)
				|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultText), Response) || !Response)
			{
				Finish(TEXT("crashed"), TEXT("Worker exited without a valid result; completion and earlier side effects are unknown."));
				return 0;
			}
			bool bSucceeded = false;
			bool bTruncated = false;
			FString ResponseOutput;
			FString ResponseError;
			if (!Response->TryGetBoolField(TEXT("success"), bSucceeded)
				|| !Response->TryGetStringField(TEXT("output"), ResponseOutput)
				|| !Response->TryGetBoolField(TEXT("output_truncated"), bTruncated))
			{
				Finish(TEXT("failed"), TEXT("Worker returned an invalid result contract."));
				return 0;
			}
			Response->TryGetStringField(TEXT("error"), ResponseError);
			{
				FScopeLock Lock(&Mutex);
				Output = ResponseOutput.Left(MaxOutputCharacters);
				bOutputTruncated = bTruncated || ResponseOutput.Len() > MaxOutputCharacters;
			}
			if (!bGotCode || Code != 0)
			{
				Finish(TEXT("crashed"), TEXT("Worker returned a result but did not exit cleanly; completion is unknown."));
			}
			else
			{
				Finish(bSucceeded ? TEXT("completed") : TEXT("failed"), ResponseError.Left(MaxOutputCharacters));
			}
#else
			Finish(TEXT("failed"), TEXT("Isolated Python process containment is currently supported on Windows only."));
#endif
			return 0;
		}

		void Finish(const FString& InStatus, const FString& InError)
		{
			FScopeLock Lock(&Mutex);
			Status = InStatus;
			Failure = InError;
			bDone = true;
		}

	private:
		bool Cancelled()
		{
			FScopeLock Lock(&Mutex);
			return bCancelRequested;
		}

		FCriticalSection Mutex;
		FString Status = TEXT("queued");
		FString Failure;
		FString Output;
		uint32 ProcessId = 0;
		int32 ExitCode = 0;
		bool bHasExitCode = false;
		bool bDone = false;
		bool bCancelRequested = false;
		bool bOutputTruncated = false;
	};

	FCriticalSection JobsMutex;
	TMap<FString, TSharedPtr<FJob, ESPMode::ThreadSafe>> Jobs;
	bool bShuttingDown = false;
}

bool IsWorker()
{
	return FParse::Param(FCommandLine::Get(), TEXT("NwiroIsolatedWorker"));
}

FString Execute(const TSharedPtr<FJsonObject>& Command)
{
	// JSON conversion helpers are permissive; envelope types remain strict.
	FString Action = TEXT("start");
	if (Command->HasField(TEXT("action"))
		&& (!Command->HasTypedField<EJson::String>(TEXT("action")) || !Command->TryGetStringField(TEXT("action"), Action)))
		return Error(TEXT("action must be start, status, or cancel."));
	if (Action != TEXT("start") && Action != TEXT("status") && Action != TEXT("cancel"))
		return Error(TEXT("action must be start, status, or cancel."));
	if (IsWorker()) return Error(TEXT("An isolated worker cannot start or control nested workers."));
	FScopeLock Lock(&JobsMutex);
	if (bShuttingDown) return Error(TEXT("The module is shutting down."));
	if (Action != TEXT("start"))
	{
		FString JobId;
		if (!Command->HasTypedField<EJson::String>(TEXT("job_id")) || !Command->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty()
			|| Command->HasField(TEXT("code")) || Command->HasField(TEXT("timeout_seconds")))
			return Error(TEXT("status/cancel requires job_id and does not accept code or timeout_seconds."));
		const auto* Found = Jobs.Find(JobId);
		if (!Found) return Error(TEXT("Unknown or expired job_id in this editor session."));
		if (Action == TEXT("cancel")) (*Found)->Stop();
		return Json((*Found)->Snapshot());
	}
	FString Code;
	if (Command->HasField(TEXT("job_id")) || !Command->HasTypedField<EJson::String>(TEXT("code")) || !Command->TryGetStringField(TEXT("code"), Code)
		|| Code.TrimStartAndEnd().IsEmpty() || Code.Len() > MaxCodeCharacters)
		return Error(TEXT("start requires nonempty code (at most 1048576 characters), without job_id."));
	double Timeout = 120;
	if (Command->HasField(TEXT("timeout_seconds"))
		&& (!Command->HasTypedField<EJson::Number>(TEXT("timeout_seconds")) || !Command->TryGetNumberField(TEXT("timeout_seconds"), Timeout)
			|| !FMath::IsFinite(Timeout) || Timeout < 1 || Timeout > 3600))
		return Error(TEXT("timeout_seconds must be a finite number from 1 to 3600, including editor startup."));
#if !PLATFORM_WINDOWS
	return Error(TEXT("Isolated Python process containment is currently supported on Windows only."));
#else
	FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	FString Executable = FPaths::ConvertRelativePathToFull(FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), EBuildConfiguration::Development));
	if (!FPaths::FileExists(Project) || !FPaths::FileExists(Executable))
		return Error(TEXT("Isolated execution requires a saved .uproject and the matching UnrealEditor-Cmd executable."));
	int32 Active = 0;
	FString Oldest;
	double OldestTime = TNumericLimits<double>::Max();
	for (const auto& Pair : Jobs)
	{
		if (!Pair.Value->IsDone()) ++Active;
		else if (Pair.Value->CreatedAt < OldestTime)
		{
			OldestTime = Pair.Value->CreatedAt;
			Oldest = Pair.Key;
		}
	}
	if (Active >= 2) return Error(TEXT("Two isolated workers are already active; poll or cancel an existing job."));
	if (Jobs.Num() >= MaxRetainedJobs && !Oldest.IsEmpty()) Jobs.Remove(Oldest);
	auto Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Job->Project = Project;
	Job->Executable = Executable;
	Job->TimeoutSeconds = Timeout;
	Job->Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Nwiro/IsolatedPython") / Job->Id);
	if (!IFileManager::Get().MakeDirectory(*Job->Directory, true)
		|| !FFileHelper::SaveStringToFile(Code, *(Job->Directory / TEXT("code.py")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !FFileHelper::SaveStringToFile(Wrapper, *(Job->Directory / TEXT("worker.py")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		return Error(TEXT("Could not prepare the isolated worker files."));
	// Commandlets may write module/config metadata even for read-only user code.
	// Give normal startup its own profile so it cannot rewrite the live user's INI.
	FString UserSettings;
	if (FPaths::FileExists(GEditorPerProjectIni) && !FFileHelper::LoadFileToString(UserSettings, *GEditorPerProjectIni))
		return Error(TEXT("Could not capture the isolated worker settings profile."));
	if (!FFileHelper::SaveStringToFile(UserSettings, *(Job->Directory / TEXT("EditorPerProjectUserSettings.ini")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		return Error(TEXT("Could not prepare the isolated worker settings profile."));
	Jobs.Add(Job->Id, Job);
	Job->Thread = FRunnableThread::Create(&Job.Get(), *FString::Printf(TEXT("NwiroPython_%s"), *Job->Id));
	if (!Job->Thread) Job->Finish(TEXT("failed"), TEXT("Could not start the worker supervisor."));
	return Json(Job->Snapshot());
#endif
}

void Shutdown()
{
	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> Pending;
	{
		FScopeLock Lock(&JobsMutex);
		bShuttingDown = true;
		Jobs.GenerateValueArray(Pending);
		for (const auto& Job : Pending) Job->Stop();
		Jobs.Empty();
	}
	// Join outside the manager lock; no supervisor work calls the game thread.
	Pending.Empty();
}
}
