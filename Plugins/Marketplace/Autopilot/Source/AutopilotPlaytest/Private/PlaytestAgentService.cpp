// The agent service started by the editor: finding, starting, installing and watching it (docs/protocol.md, "Started
// by the plugin").

#include "PlaytestAgentService.h"

#include <atomic>

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlaytestInternal.h"
#include "PlaytestJson.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <TlHelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	const TCHAR* BrainFile = TEXT("brain.json");
	const TCHAR* StartLogPrefix = TEXT("agent-service-");
	const TCHAR* InstallLogPrefix = TEXT("install-");
	constexpr int32 LogsKept = 5;
	/** A service this editor started that ends sooner than this after saying where it listens isn't started again by itself. */
	constexpr double CrashSeconds = 60.0;

	double ServiceNow()
	{
		return FPlatformTime::Seconds();
	}

	bool DefaultExists(const FString& Path)
	{
		return FPaths::FileExists(Path);
	}

	/** Windows paths as cmd.exe likes them. */
	FString Native(const FString& Path, bool bWindows)
	{
		return bWindows ? Path.Replace(TEXT("/"), TEXT("\\")) : Path;
	}

	/** A word for /bin/sh in single quotes. */
	FString ShQuote(const FString& Text)
	{
		return TEXT("'") + Text.Replace(TEXT("'"), TEXT("'\\''")) + TEXT("'");
	}

	bool HasSeparator(const FString& Path)
	{
		return Path.Contains(TEXT("/")) || Path.Contains(TEXT("\\"));
	}

	/** Name in one of the PATH's folders ("" when in none). */
	FString OnPath(const FString& Name, const FPlaytestServiceSearch& Search, const TFunction<bool(const FString&)>& Exists)
	{
		TArray<FString> Dirs;
		Search.PathEnv.ParseIntoArray(Dirs, Search.bWindows ? TEXT(";") : TEXT(":"), true);
		TArray<FString> Names;
		if (Search.bWindows && FPaths::GetExtension(Name).IsEmpty())
		{
			Names = { Name + TEXT(".exe"), Name + TEXT(".cmd"), Name + TEXT(".bat") };
		}
		else
		{
			Names = { Name };
		}
		for (FString Dir : Dirs)
		{
			Dir = Dir.TrimStartAndEnd().TrimQuotes();
			if (Dir.IsEmpty())
			{
				continue;
			}
			for (const FString& Each : Names)
			{
				const FString Candidate = FPaths::Combine(Dir, Each);
				if (Exists(Candidate))
				{
					return Candidate;
				}
			}
		}
		return FString();
	}

	/** A program named by the setting: a path that is there (Windows: also with .exe), or a name on PATH. */
	FString ResolveProgram(const FString& Exe, const FPlaytestServiceSearch& Search, const TFunction<bool(const FString&)>& Exists)
	{
		if (HasSeparator(Exe))
		{
			if (Exists(Exe))
			{
				return Exe;
			}
			if (Search.bWindows && FPaths::GetExtension(Exe).IsEmpty() && Exists(Exe + TEXT(".exe")))
			{
				return Exe + TEXT(".exe");
			}
			return FString();
		}
		return OnPath(Exe, Search, Exists);
	}

	/** A file written before the process starts appending to it: when, and what was run. */
	void WriteLogHeader(const FString& LogFile, const FString& What, const FString& Exe, const FString& Params)
	{
		const FString Header = FString::Printf(TEXT("Autopilot %s, %s\n%s %s\n\n"), *What,
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")), *Exe, *Params);
		FFileHelper::SaveStringToFile(Header, *LogFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString ComSpec()
	{
		const FString Given = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
		return Given.IsEmpty() ? FString(TEXT("C:\\Windows\\System32\\cmd.exe")) : Given;
	}

	FString PowerShellPath()
	{
		const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
		const FString Path = FPaths::Combine(Root, TEXT("System32"), TEXT("WindowsPowerShell"), TEXT("v1.0"), TEXT("powershell.exe"));
		return !Root.IsEmpty() && FPaths::FileExists(Path) ? Native(Path, true) : FString(TEXT("powershell.exe"));
	}

	FString ShellPath(bool bWindows)
	{
		return bWindows ? ComSpec() : FString(TEXT("/bin/sh"));
	}
	/**
	 * Starts a hidden process: no window for it or anything it starts, no handles inherited. From 5.7 "detached" is
	 * CREATE_NO_WINDOW; before, it is DETACHED_PROCESS (no console at all: a shell's output is lost and what it starts
	 * opens console windows of its own), so the process gets a console of its own instead, hidden.
	 */
	FProcHandle StartHidden(const FString& Exe, const FString& Params, uint32* OutPid, const FString& WorkingDir)
	{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		const bool bDetached = false;
#else
		const bool bDetached = true;
#endif
		return FPlatformProcess::CreateProc(*Exe, *Params, bDetached, true, true, OutPid, 0, *WorkingDir, nullptr, nullptr);
	}
}

// ---------------------------------------------------------------------------------------------------------------------

FPlaytestServiceSearch FPlaytestServiceSearch::FromEnvironment(const FString& Setting)
{
	FPlaytestServiceSearch Search;
	Search.Setting = Setting;
	Search.PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
#if PLATFORM_WINDOWS
	Search.HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#else
	Search.HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif
	if (Search.HomeDir.IsEmpty())
	{
		Search.HomeDir = FPlatformProcess::UserHomeDir();
	}
	return Search;
}

namespace PlaytestAgentService
{
	void SplitCommand(const FString& Line, FString& OutExe, FString& OutArgs, const TFunction<bool(const FString&)>& FileExists, bool bWindows)
	{
		OutExe.Empty();
		OutArgs.Empty();
		const FString Text = Line.TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			return;
		}
		if (Text.StartsWith(TEXT("\"")))
		{
			int32 Close = INDEX_NONE;
			if (Text.Mid(1).FindChar(TEXT('"'), Close))
			{
				OutExe = Text.Mid(1, Close);
				OutArgs = Text.Mid(Close + 2).TrimStartAndEnd();
			}
			else
			{
				OutExe = Text.Mid(1);
			}
			return;
		}
		// Unquoted: the longest run of leading words that names a file ("C:\Program Files\Python\python.exe -m autopilot"),
		// else the first word
		TArray<int32> Ends;  // where each word ends
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			if (!FChar::IsWhitespace(Text[i]) && (i + 1 == Text.Len() || FChar::IsWhitespace(Text[i + 1])))
			{
				Ends.Add(i + 1);
			}
		}
		int32 End = Ends.Num() ? Ends[0] : Text.Len();
		if (FileExists)
		{
			for (int32 k = Ends.Num() - 1; k > 0; --k)
			{
				const FString Candidate = Text.Left(Ends[k]);
				if (FileExists(Candidate) || (bWindows && FileExists(Candidate + TEXT(".exe"))))
				{
					End = Ends[k];
					break;
				}
			}
		}
		OutExe = Text.Left(End);
		OutArgs = Text.Mid(End).TrimStartAndEnd();
	}

	FPlaytestServiceCommand Find(const FPlaytestServiceSearch& Search)
	{
		const TFunction<bool(const FString&)> Exists = Search.FileExists ? Search.FileExists : TFunction<bool(const FString&)>(&DefaultExists);
		FPlaytestServiceCommand Command;
		if (!Search.Setting.TrimStartAndEnd().IsEmpty())
		{
			FString Exe, Args;
			SplitCommand(Search.Setting, Exe, Args, Exists, Search.bWindows);
			Command.Source = TEXT("setting");
			Command.Args = Args;
			Command.Exe = ResolveProgram(Exe, Search, Exists);
			if (Command.Exe.IsEmpty())
			{
				Command.Exe = Exe;
				Command.Problem = FString::Printf(TEXT("The Agent service command setting names %s, which isn’t on this computer."), *Exe);
			}
			return Command;
		}
		// (a plugin that carries its own agent service starts only that: never another autopilot on PATH)
		const FString Found = OnPath(TEXT("autopilot"), Search, Exists);
		if (Search.bCarried)
		{
			Command.PathSkipped = Found;
		}
		else if (!Found.IsEmpty())
		{
			Command.Exe = Found;
			Command.Source = TEXT("path");
			return Command;
		}
		if (!Search.HomeDir.IsEmpty())
		{
			const FString Installed = FPaths::Combine(Search.HomeDir, TEXT(".local"), TEXT("bin"), Search.bWindows ? TEXT("autopilot.exe") : TEXT("autopilot"));
			if (Exists(Installed))
			{
				Command.Exe = Installed;
				Command.Source = TEXT("installer");
				return Command;
			}
		}
		return Command;
	}

	FString ServeArgs(const FPlaytestServiceCommand& Command, uint32 ExitWithPid)
	{
		const FString Serve = FString::Printf(TEXT("serve --port 0 --exit-with %u"), ExitWithPid);
		return Command.Args.IsEmpty() ? Serve : Command.Args + TEXT(" ") + Serve;
	}

	void StartCommandLine(const FPlaytestServiceCommand& Command, uint32 ExitWithPid, const FString& LogFile, bool bWindows,
		const FString& Shell, FString& OutExe, FString& OutParams)
	{
		OutExe = Shell;
		if (bWindows)
		{
			// cmd /s /c "<line>": the outer quotes go, the line runs as written (output appended to the log)
			OutParams = FString::Printf(TEXT("/d /s /c \"\"%s\" %s >> \"%s\" 2>&1\""), *Native(Command.Exe, true),
				*ServeArgs(Command, ExitWithPid), *Native(LogFile, true));
		}
		else
		{
			OutParams = FString::Printf(TEXT("-c \"exec %s %s >> %s 2>&1\""), *ShQuote(Command.Exe), *ServeArgs(Command, ExitWithPid),
				*ShQuote(LogFile));
		}
	}

	FString InstallerScript(const FString& AgentServiceDir, bool bWindows)
	{
		return AgentServiceDir.IsEmpty() ? FString() : FPaths::Combine(AgentServiceDir, bWindows ? TEXT("install.ps1") : TEXT("install.sh"));
	}

	void InstallCommandLine(const FString& AgentServiceDir, const FString& LogFile, bool bWindows, const FString& Shell,
		const FString& PowerShell, FString& OutExe, FString& OutParams)
	{
		OutExe = Shell;
		const FString Script = InstallerScript(AgentServiceDir, bWindows);
		if (bWindows)
		{
			OutParams = FString::Printf(TEXT("/d /s /c \"\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\" >> \"%s\" 2>&1\""),
				*PowerShell, *Native(Script, true), *Native(LogFile, true));
		}
		else
		{
			OutParams = FString::Printf(TEXT("-c \"sh %s >> %s 2>&1\""), *ShQuote(Script), *ShQuote(LogFile));
		}
	}

	FString UninstallArgs(const FPlaytestServiceCommand& Command, bool bKeepMemory, const FString& LogFile, bool bWindows)
	{
		FString Uninstall = TEXT("uninstall --yes");
		if (bKeepMemory)
		{
			Uninstall += TEXT(" --keep-memory");
		}
		Uninstall += TEXT(" --log ") + (bWindows ? TEXT("\"") + Native(LogFile, true) + TEXT("\"") : ShQuote(LogFile));
		return Command.Args.IsEmpty() ? Uninstall : Command.Args + TEXT(" ") + Uninstall;
	}

	void UninstallCommandLine(const FPlaytestServiceCommand& Command, bool bKeepMemory, const FString& LogFile, bool bWindows,
		const FString& Shell, FString& OutExe, FString& OutParams)
	{
		OutExe = Shell;
		const FString Args = UninstallArgs(Command, bKeepMemory, LogFile, bWindows);
		if (bWindows)
		{
			// (unbuffered: what it says reaches the log as it says it, before what the command's removal adds after it)
			OutParams = FString::Printf(TEXT("/d /s /c \"set PYTHONUNBUFFERED=1& \"%s\" %s >> \"%s\" 2>&1\""), *Native(Command.Exe, true),
				*Args, *Native(LogFile, true));
		}
		else
		{
			OutParams = FString::Printf(TEXT("-c \"PYTHONUNBUFFERED=1 exec %s %s >> %s 2>&1\""), *ShQuote(Command.Exe), *Args, *ShQuote(LogFile));
		}
	}

	int32 RemoveSettingsFolder(const FString& SettingsDir, bool bKeepMemory, FString& OutLog)
	{
		IFileManager& Files = IFileManager::Get();
		bool bSomeStay = false;
		if (!Files.DirectoryExists(*SettingsDir))
		{
			return 0;
		}
		TArray<FString> Names;
		Files.FindFiles(Names, *FPaths::Combine(SettingsDir, TEXT("*")), true, true);
		Names.Sort();
		for (const FString& Name : Names)
		{
			const FString Path = FPaths::Combine(SettingsDir, Name);
			if (Name == TEXT("aura-brain.json"))
			{
				continue;  // (the Aura app's: it writes it again whenever its agent service starts)
			}
			if (Name == TEXT("memory") && bKeepMemory)
			{
				OutLog += FString::Printf(TEXT("Kept what Autopilot learned about your games in %s\n"), *Path);
				continue;
			}
			const bool bGone = Files.DirectoryExists(*Path) ? Files.DeleteDirectory(*Path, false, true) : Files.Delete(*Path, false, true, true);
			OutLog += bGone ? FString::Printf(TEXT("Removed %s\n"), *Path) : FString::Printf(TEXT("Couldn't remove %s (in use)\n"), *Path);
			bSomeStay |= !bGone;
		}
		TArray<FString> Left;
		Files.FindFiles(Left, *FPaths::Combine(SettingsDir, TEXT("*")), true, true);
		if (Left.Num() == 0)
		{
			Files.DeleteDirectory(*SettingsDir, false, false);
		}
		return bSomeStay ? 3 : 0;
	}

	FPlaytestServiceStamp ParseStamp(const FString& Text)
	{
		FPlaytestServiceStamp Stamp;
		const TSharedPtr<FJsonObject> Said = PlaytestJson::Parse(Text);
		if (Said.IsValid())
		{
			Said->TryGetStringField(TEXT("sha256"), Stamp.Sha256);
			Said->TryGetStringField(TEXT("commit"), Stamp.Commit);
			Stamp.Sha256 = Stamp.Sha256.TrimStartAndEnd().ToLower();
			Stamp.Commit = Stamp.Commit.TrimStartAndEnd();
		}
		return Stamp;
	}

	bool NeedsUpdate(const FString& Shipped, const FString& Installed, const FPlaytestServiceCommand& Command,
		const FString& HomeDir, bool bWindows)
	{
		// (the wheel's sha256 tells them apart; a stamp naming only a commit, its commit)
		const FPlaytestServiceStamp Wanted = ParseStamp(Shipped);
		if ((Wanted.Sha256.IsEmpty() && Wanted.Commit.IsEmpty()) || !Command.IsFound() || Command.Source == TEXT("setting") || HomeDir.IsEmpty())
		{
			return false;
		}
		FString Exe = Command.Exe;
		FString Place = FPaths::Combine(HomeDir, TEXT(".local"), TEXT("bin"), bWindows ? TEXT("autopilot.exe") : TEXT("autopilot"));
		FPaths::NormalizeFilename(Exe);
		FPaths::NormalizeFilename(Place);
		if (!Exe.Equals(Place, bWindows ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FPlaytestServiceStamp Have = ParseStamp(Installed);
		return !Wanted.Sha256.IsEmpty() ? Have.Sha256 != Wanted.Sha256 : !Have.Commit.Equals(Wanted.Commit, ESearchCase::IgnoreCase);
	}

	bool Carries(const FString& Shipped, bool bHasInstaller)
	{
		const FPlaytestServiceStamp Stamp = ParseStamp(Shipped);
		return bHasInstaller && (!Stamp.Sha256.IsEmpty() || !Stamp.Commit.IsEmpty());
	}

	EPlaytestServiceStep PlanStart(const FPlaytestServiceCommand& Command, const FString& Shipped, const FString& Installed,
		bool bCarried, bool bHasInstaller, bool bInstallTried, const FString& HomeDir, bool bWindows)
	{
		if (Command.Source == TEXT("setting"))
		{
			return Command.IsFound() ? EPlaytestServiceStep::Start : EPlaytestServiceStep::Missing;  // (never installed over)
		}
		const bool bMayInstall = bHasInstaller && !bInstallTried;
		if (!Command.IsFound())
		{
			// The plugin's in place of an autopilot on PATH, when the plugin carries its own. Nothing installed at all: the
			// first-run setup's (or the tab's) Install does it
			const bool bInstall = bCarried && !Command.PathSkipped.IsEmpty();
			return bInstall && bMayInstall ? EPlaytestServiceStep::Install : EPlaytestServiceStep::Missing;
		}
		return bMayInstall && NeedsUpdate(Shipped, Installed, Command, HomeDir, bWindows) ? EPlaytestServiceStep::Update
			: EPlaytestServiceStep::Start;
	}

	bool IsSameBuild(const FPlaytestAnnouncement& Announced, const FString& Shipped)
	{
		const FString Wanted = ParseStamp(Shipped).Commit;
		const FString Have = Announced.Commit.TrimStartAndEnd();
		if (!Announced.bHasCommit || Have.IsEmpty() || Wanted.IsEmpty())
		{
			return false;
		}
		// (the same commit; one written short, the other whole, is the same too)
		const int32 Shortest = FMath::Min(Have.Len(), Wanted.Len());
		return Shortest >= 7 ? Have.Left(Shortest).Equals(Wanted.Left(Shortest), ESearchCase::IgnoreCase)
			: Have.Equals(Wanted, ESearchCase::IgnoreCase);
	}

	EPlaytestAnnouncedStep PlanAnnounced(const FPlaytestAnnouncement& Announced, const FString& Shipped, bool bCarried,
		bool bSetting, bool bInUse, bool bReplaceTried)
	{
		if (!bCarried || bSetting || bInUse || ParseStamp(Shipped).Commit.IsEmpty() || IsSameBuild(Announced, Shipped))
		{
			return EPlaytestAnnouncedStep::Use;
		}
		return bReplaceTried ? EPlaytestAnnouncedStep::UseOlder : EPlaytestAnnouncedStep::Replace;
	}

	bool IsAgentServiceProgram(const FString& Program)
	{
		FString Name = FPaths::GetCleanFilename(Program.TrimStartAndEnd().TrimQuotes().Replace(TEXT("\\"), TEXT("/"))).ToLower();
		Name.RemoveFromEnd(TEXT(".exe"));
		if (Name == TEXT("autopilot") || Name == TEXT("python") || Name == TEXT("pythonw") || Name == TEXT("python3"))
		{
			return true;
		}
		// python3.N (python3.12): a minor version of digits only
		const FString Minor = Name.StartsWith(TEXT("python3.")) ? Name.Mid(8) : FString();
		bool bDigits = !Minor.IsEmpty();
		for (const TCHAR Each : Minor)
		{
			bDigits = bDigits && FChar::IsDigit(Each);
		}
		return bDigits;
	}

	bool MayStop(const FString& Program, double ProcessStarted, const FPlaytestAnnouncement& Announced, FString& OutWhy)
	{
		OutWhy.Empty();
		if (!IsAgentServiceProgram(Program))
		{
			OutWhy = FString::Printf(TEXT("its program isn't an agent service's (%s)"), Program.IsEmpty() ? TEXT("unknown") : *Program);
		}
		else if (!Announced.bHasStarted)
		{
			OutWhy = TEXT("brain.json doesn't say when it started, so it can't be told from another process with that id");
		}
		else if (ProcessStarted <= 0)
		{
			OutWhy = TEXT("its process's start time can't be read, so it can't be told from another process with that id");
		}
		else if (ProcessStarted > Announced.Started + OwnerSlackSeconds)
		{
			OutWhy = FString::Printf(TEXT("its process started %.0f s after brain.json was written: another process has that id now"),
				ProcessStarted - Announced.Started);
		}
		return OutWhy.IsEmpty();
	}

	double ParseElapsed(const FString& Text)
	{
		// [[dd-]hh:]mm:ss
		FString Rest = Text.TrimStartAndEnd();
		double Days = 0;
		FString DaysText;
		if (Rest.Split(TEXT("-"), &DaysText, &Rest))
		{
			if (DaysText.IsEmpty() || !DaysText.IsNumeric())
			{
				return -1.0;
			}
			Days = FCString::Atod(*DaysText);
		}
		TArray<FString> Parts;
		Rest.ParseIntoArray(Parts, TEXT(":"), false);
		if (Parts.Num() < 2 || Parts.Num() > 3)
		{
			return -1.0;
		}
		double Seconds = 0;
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || !Part.IsNumeric() || Part.Contains(TEXT(".")) || Part.Contains(TEXT("-")))
			{
				return -1.0;
			}
			Seconds = Seconds * 60.0 + FCString::Atod(*Part);
		}
		return Days * 86400.0 + Seconds;
	}

	double ProcessStartTime(uint32 Pid)
	{
		if (Pid == 0)
		{
			return 0;
		}
#if PLATFORM_WINDOWS
		double Started = 0;
		HANDLE Process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, (::DWORD)Pid);
		if (Process)
		{
			::FILETIME Created, Exited, Kernel, User;
			if (::GetProcessTimes(Process, &Created, &Exited, &Kernel, &User))
			{
				ULARGE_INTEGER Ticks;  // (100 ns since 1601)
				Ticks.LowPart = Created.dwLowDateTime;
				Ticks.HighPart = Created.dwHighDateTime;
				const uint64 Epoch = 116444736000000000ull;
				Started = Ticks.QuadPart > Epoch ? (double)(Ticks.QuadPart - Epoch) / 1.0e7 : 0.0;
			}
			::CloseHandle(Process);
		}
		return Started;
#else
		int32 Code = -1;
		FString Out, Err;
		if (!FPlatformProcess::ExecProcess(TEXT("/bin/ps"), *FString::Printf(TEXT("-o etime= -p %u"), Pid), &Code, &Out, &Err) || Code != 0)
		{
			return 0;
		}
		const double Elapsed = ParseElapsed(Out);
		const double Now = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalSeconds();
		return Elapsed < 0 ? 0.0 : Now - Elapsed;
#endif
	}

	bool ParseAnnouncement(const FString& Text, FPlaytestAnnouncement& Out)
	{
		const TSharedPtr<FJsonObject> Said = PlaytestJson::Parse(Text);
		FString Url;
		double Pid = 0;
		if (!Said.IsValid() || !Said->TryGetStringField(TEXT("url"), Url) || !Said->TryGetNumberField(TEXT("pid"), Pid)
			|| !(Url.StartsWith(TEXT("ws://127.0.0.1:")) || Url.StartsWith(TEXT("ws://localhost:")))
			|| Pid <= 0 || Pid > (double)MAX_uint32)
		{
			return false;
		}
		Out = FPlaytestAnnouncement();
		Out.Url = Url;
		Out.Pid = (uint32)Pid;
		Out.bHasStarted = Said->TryGetNumberField(TEXT("started"), Out.Started) && Out.Started > 0;
		if (!Out.bHasStarted)
		{
			Out.Started = 0;
		}
		// ("commit": "" for a source checkout; agent services older than it don't say; null is as good as not said)
		Out.bHasCommit = Said->TryGetStringField(TEXT("commit"), Out.Commit);
		Out.Commit.TrimStartAndEndInline();
		return true;
	}

	bool ReadAnnouncement(const FString& Dir, const TCHAR* File, FPlaytestAnnouncement& Out)
	{
		FString Text;
		FPlaytestAnnouncement Read;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, File)) || !ParseAnnouncement(Text, Read)
			|| !FPlatformProcess::IsApplicationRunning(Read.Pid))
		{
			return false;
		}
		Out = Read;
		return true;
	}

	bool IsFrom(const FPlaytestAnnouncement& Announcement, uint32 Pid, const TFunction<uint32(uint32)>& ParentOf)
	{
		if (Pid == 0)
		{
			return false;
		}
		uint32 Each = Announcement.Pid;
		for (int32 Depth = 0; Depth < 6 && Each != 0; ++Depth)
		{
			if (Each == Pid)
			{
				return true;
			}
			if (!ParentOf)
			{
				break;
			}
			const uint32 Parent = ParentOf(Each);
			if (Parent == Each)
			{
				break;
			}
			Each = Parent;
		}
		return false;
	}

	TFunction<uint32(uint32)> ProcessParents()
	{
		TMap<uint32, uint32> Parents;
#if PLATFORM_WINDOWS
		HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (Snapshot != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W Entry;
			Entry.dwSize = sizeof(Entry);
			if (Process32FirstW(Snapshot, &Entry))
			{
				do
				{
					Parents.Add((uint32)Entry.th32ProcessID, (uint32)Entry.th32ParentProcessID);
				}
				while (Process32NextW(Snapshot, &Entry));
			}
			CloseHandle(Snapshot);
		}
#endif
		return [Parents = MoveTemp(Parents)](uint32 Pid) -> uint32
		{
			const uint32* Parent = Parents.Find(Pid);
			return Parent ? *Parent : 0;
		};
	}

	FString NewLogPath(const FString& Dir, const FString& Prefix, const FDateTime& When)
	{
		const FString Stem = FPaths::Combine(Dir, Prefix + When.ToString(TEXT("%Y%m%d-%H%M%S")));
		FString Path = Stem + TEXT(".log");
		for (int32 n = 2; FPaths::FileExists(Path) && n < 100; ++n)
		{
			Path = FString::Printf(TEXT("%s-%d.log"), *Stem, n);
		}
		return Path;
	}

	void PruneLogs(const FString& Dir, const FString& Prefix, int32 Keep)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Dir, Prefix + TEXT("*.log")), true, false);
		// (the names hold the time they were made: newest last by name, and by the file's time when two tie)
		Names.Sort([&Dir](const FString& A, const FString& B)
		{
			const FDateTime TA = IFileManager::Get().GetTimeStamp(*FPaths::Combine(Dir, A));
			const FDateTime TB = IFileManager::Get().GetTimeStamp(*FPaths::Combine(Dir, B));
			return TA != TB ? TA > TB : A > B;
		});
		for (int32 i = FMath::Max(0, Keep); i < Names.Num(); ++i)
		{
			IFileManager::Get().Delete(*FPaths::Combine(Dir, Names[i]), false, true, true);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------

/** What the threads watching the processes and the game thread share. */
struct FPlaytestAgentService::FShared
{
	mutable FCriticalSection Lock;
	EPlaytestServiceState State = EPlaytestServiceState::Idle;
	FString Url;
	FString Problem;
	FString LogPath;
	bool bOurs = false;          // the one running was started by this editor
	bool bInstalled = false;     // an install finished well (an autopilot command not found after it is the installer's fault)
	bool bUpdateTried = false;   // this session has installed the plugin's agent service over an older one (or tried)
	bool bUpdating = false;      // the install going now is that: if it fails, the installed one is started all the same
	bool bReplaceTried = false;  // this session has replaced an older agent service that was running (or decided not to)
	bool bReplacing = false;     // ... and is stopping it now
	uint32 StartedPid = 0;       // the process started last (the shell running autopilot)
	bool bStartedEnded = false;  // ... and it has ended
	double AnnouncedAt = 0;      // when it said where it listens
	double EndedAt = 0;
	double GoneSince = 0;        // when the running one's announcement went (its process still running)
	int32 Generation = 0;        // each start or install: a watcher of an older one writes nothing
	bool bRemoved = false;       // Uninstall was called (LeaveRemoved not since): nothing is started or installed
	EPlaytestUninstallState Uninstall = EPlaytestUninstallState::None;
	FString UninstallLog;
	bool bUninstallKeepsMemory = true;
	int32 UninstallGeneration = 0;  // each Uninstall (and LeaveRemoved): a watcher of an older one writes nothing
	std::atomic<bool> bCancel{false};
};

FPlaytestAgentService::FPlaytestAgentService(FOptions InOptions)
	: Options(MoveTemp(InOptions))
	, Shared(MakeShared<FShared, ESPMode::ThreadSafe>())
{
}

FPlaytestAgentService::~FPlaytestAgentService()
{
	Shutdown();
}

void FPlaytestAgentService::Shutdown()
{
	Shared->bCancel = true;
}

EPlaytestUninstallState FPlaytestAgentService::GetUninstallState() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->Uninstall;
}

FString FPlaytestAgentService::GetUninstallLog() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->UninstallLog;
}

bool FPlaytestAgentService::GetUninstallKeepsMemory() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->bUninstallKeepsMemory;
}

bool FPlaytestAgentService::IsRemoved() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->bRemoved;
}

void FPlaytestAgentService::LeaveRemoved()
{
	FScopeLock Locked(&Shared->Lock);
	Shared->bRemoved = false;
	Shared->Uninstall = EPlaytestUninstallState::None;
	++Shared->UninstallGeneration;
	Shared->State = EPlaytestServiceState::Idle;
	Shared->Problem.Empty();
	Shared->Url.Empty();
	Shared->bOurs = false;
	Shared->bInstalled = false;
	Shared->bUpdateTried = false;  // (a computer without it now: the plugin's own is installed once a session again)
	Shared->bReplaceTried = false;
}

bool FPlaytestAgentService::IsMissing() const
{
	FPlaytestAnnouncement Announced;
	if (PlaytestAgentService::ReadAnnouncement(Options.SettingsDir, BrainFile, Announced))
	{
		return false;
	}
	FPlaytestServiceSearch Search = Options.Search ? Options.Search() : FPlaytestServiceSearch::FromEnvironment(FString());
	Search.bCarried = PlaytestAgentService::Carries(ShippedStamp(), HasInstaller());
	const FPlaytestServiceCommand Command = PlaytestAgentService::Find(Search);
	// (a setting naming nothing says so where it is set; in place of a PATH one a release download skips, the plugin
	// installs its own by itself)
	return !Command.IsFound() && Command.Problem.IsEmpty() && Command.PathSkipped.IsEmpty();
}

void FPlaytestAgentService::Uninstall(bool bKeepMemory)
{
	int32 Generation = 0;
	{
		FScopeLock Locked(&Shared->Lock);
		if (Shared->Uninstall == EPlaytestUninstallState::Removing)
		{
			return;
		}
		Shared->bRemoved = true;
		Shared->bUninstallKeepsMemory = bKeepMemory;
		Shared->Uninstall = EPlaytestUninstallState::Removing;
		Generation = ++Shared->UninstallGeneration;
		++Shared->Generation;  // (a start or install still watched writes nothing from now on)
		Shared->State = EPlaytestServiceState::Idle;
		Shared->Url.Empty();
		Shared->Problem.Empty();
		Shared->bOurs = false;
		Shared->bUpdating = false;
	}
	// The command Start would run (the setting, the installer's place...)
	FPlaytestServiceSearch Search = Options.Search ? Options.Search() : FPlaytestServiceSearch::FromEnvironment(FString());
	Search.bCarried = PlaytestAgentService::Carries(ShippedStamp(), HasInstaller());
	const FPlaytestServiceCommand Command = PlaytestAgentService::Find(Search);
	const FString Temp = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir());
	const FString LogFile = FPaths::ConvertRelativePathToFull(PlaytestAgentService::NewLogPath(Temp, TEXT("autopilot-uninstall-"), FDateTime::Now()));
	{
		FScopeLock Locked(&Shared->Lock);
		Shared->UninstallLog = LogFile;
	}
	TSharedRef<FShared, ESPMode::ThreadSafe> State = Shared;
	auto Finish = [State, Generation](int32 Code)
	{
		FScopeLock Locked(&State->Lock);
		if (State->UninstallGeneration == Generation)
		{
			State->Uninstall = Code == 0 ? EPlaytestUninstallState::Removed
				: Code == 3 ? EPlaytestUninstallState::RemovedButSome : EPlaytestUninstallState::Failed;
		}
	};
	if (!Command.IsFound())
	{
		// None installed: the settings folder, but what Autopilot learned (only the agent service sends its unsent runs)
		const FString Dir = Options.SettingsDir;
		Async(EAsyncExecution::Thread, [Dir, bKeepMemory, LogFile, Finish]()
		{
			FString Log = FString::Printf(TEXT("Autopilot removes itself from this computer, %s\nThe agent service isn't installed: the plugin removes Autopilot's settings folder\n\n"),
				*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
			const int32 Code = PlaytestAgentService::RemoveSettingsFolder(Dir, bKeepMemory, Log);
			Log += Code == 0 ? TEXT("Autopilot is removed from this computer.\n") : TEXT("Autopilot is removed from this computer, but for what this names.\n");
			FFileHelper::SaveStringToFile(Log, *LogFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Removed Autopilot's settings folder (the agent service isn't installed): see %s"), *LogFile);
			Finish(Code);
		});
		return;
	}
	const bool bWindows = PLATFORM_WINDOWS != 0;
	FString Exe, Params;
	PlaytestAgentService::UninstallCommandLine(Command, bKeepMemory, LogFile, bWindows, ShellPath(bWindows), Exe, Params);
	WriteLogHeader(LogFile, TEXT("removes itself from this computer"), Exe, Params);
	// (from the temporary folder: on Windows a folder that is a process's current one can't be removed)
	FProcHandle Handle = StartHidden(Exe, Params, nullptr, Temp);
	if (!Handle.IsValid())
	{
		UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] Couldn't start %s to remove Autopilot from this computer"), *Command.Exe);
		Finish(1);
		return;
	}
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Removing Autopilot from this computer (%s), its output in %s"), *Command.Exe, *LogFile);
	Async(EAsyncExecution::Thread, [State, Handle, LogFile, Finish]() mutable
	{
		const double Deadline = ServiceNow() + PlaytestAgentService::UninstallSeconds;
		int32 Code = 1;
		while (!State->bCancel)
		{
			if (!FPlatformProcess::IsProcRunning(Handle))
			{
				FPlatformProcess::GetProcReturnCode(Handle, &Code);
				UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Removing Autopilot ended (exit code %d)"), Code);
				break;
			}
			if (ServiceNow() > Deadline)
			{
				FPlatformProcess::TerminateProc(Handle, true);
				UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] Removing Autopilot didn't finish within %.0f minutes (see %s)"),
					PlaytestAgentService::UninstallSeconds / 60.0, *LogFile);
				Code = 1;
				break;
			}
			FPlatformProcess::Sleep(0.25f);
		}
		FPlatformProcess::CloseProc(Handle);
		Finish(Code);
	});
}

EPlaytestServiceState FPlaytestAgentService::GetState() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->State;
}

FString FPlaytestAgentService::GetUrl() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->State == EPlaytestServiceState::Running ? Shared->Url : FString();
}

FString FPlaytestAgentService::GetProblem() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->Problem;
}

FString FPlaytestAgentService::GetLogPath() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->LogPath;
}

bool FPlaytestAgentService::IsOurs() const
{
	FScopeLock Locked(&Shared->Lock);
	return Shared->State == EPlaytestServiceState::Running && Shared->bOurs;
}

bool FPlaytestAgentService::HasInstaller() const
{
	const FString Script = PlaytestAgentService::InstallerScript(Options.AgentServiceDir);
	return !Script.IsEmpty() && FPaths::FileExists(Script);
}

void FPlaytestAgentService::Retry()
{
	FScopeLock Locked(&Shared->Lock);
	switch (Shared->State)
	{
	case EPlaytestServiceState::NotInstalled:
	case EPlaytestServiceState::BadCommand:
	case EPlaytestServiceState::InstallFailed:
	case EPlaytestServiceState::Stopped:
		Shared->State = EPlaytestServiceState::Idle;
		Shared->Problem.Empty();
		Shared->bInstalled = false;
		break;
	default:
		break;
	}
}

FPlaytestAgentService::EPrepared FPlaytestAgentService::Prepare(FString& OutUrl)
{
	OutUrl.Empty();
	{
		FScopeLock Locked(&Shared->Lock);
		if (Shared->bRemoved)
		{
			return EPrepared::Failed;  // (Autopilot was removed from this computer: nothing is started until Set up again)
		}
		if (Shared->bReplacing)
		{
			return EPrepared::Busy;  // (an older one is being stopped: then this plugin's starts)
		}
	}
	FPlaytestAnnouncement Announced;
	if (PlaytestAgentService::ReadAnnouncement(Options.SettingsDir, BrainFile, Announced))
	{
		if (!UseAnnounced(Announced))
		{
			return EPrepared::Busy;
		}
		FScopeLock Locked(&Shared->Lock);
		if (Shared->State != EPlaytestServiceState::Running || Shared->Url != Announced.Url)
		{
			Shared->bOurs = PlaytestAgentService::IsFrom(Announced, Shared->bStartedEnded ? 0 : Shared->StartedPid,
				PlaytestAgentService::ProcessParents());
			if (Shared->bOurs && Shared->AnnouncedAt <= 0)
			{
				Shared->AnnouncedAt = ServiceNow();
			}
		}
		Shared->State = EPlaytestServiceState::Running;
		Shared->Url = Announced.Url;
		Shared->Problem.Empty();
		Shared->GoneSince = 0;
		OutUrl = Announced.Url;
		return EPrepared::Ready;
	}
	{
		FScopeLock Locked(&Shared->Lock);
		switch (Shared->State)
		{
		case EPlaytestServiceState::Starting:
		case EPlaytestServiceState::Installing:
			return EPrepared::Busy;
		case EPlaytestServiceState::NotInstalled:
		case EPlaytestServiceState::BadCommand:
		case EPlaytestServiceState::InstallFailed:
		case EPlaytestServiceState::Stopped:
			return EPrepared::Failed;
		case EPlaytestServiceState::Running:
			// Its announcement went: it ended (the next connect starts a new one), unless it was ours and ended soon
			// after it started (it would only end again), or is still ending
			if (Shared->bOurs && !Shared->bStartedEnded)
			{
				if (Shared->GoneSince <= 0)
				{
					Shared->GoneSince = ServiceNow();
				}
				if (ServiceNow() - Shared->GoneSince < 5.0)
				{
					return EPrepared::Busy;
				}
			}
			else if (Shared->bOurs && Shared->bStartedEnded && Shared->EndedAt - Shared->AnnouncedAt < CrashSeconds)
			{
				Shared->State = EPlaytestServiceState::Stopped;
				Shared->Url.Empty();
				Shared->Problem = FString::Printf(TEXT("The agent service stopped (see %s)"), *Shared->LogPath);
				return EPrepared::Failed;
			}
			Shared->State = EPlaytestServiceState::Idle;
			Shared->Url.Empty();
			break;
		default:
			break;
		}
	}
	return Start();
}

FString FPlaytestAgentService::ShippedStamp() const
{
	FString Shipped;
	if (!Options.AgentServiceDir.IsEmpty())
	{
		FFileHelper::LoadFileToString(Shipped, *FPaths::Combine(Options.AgentServiceDir, PlaytestAgentService::StampFile));
	}
	return Shipped;
}

bool FPlaytestAgentService::UseAnnounced(const FPlaytestAnnouncement& Announced)
{
	bool bInUse = false;
	bool bTried = false;
	uint32 OurPid = 0;
	{
		FScopeLock Locked(&Shared->Lock);
		bInUse = Shared->State == EPlaytestServiceState::Running && Shared->Url == Announced.Url;
		bTried = Shared->bReplaceTried;
		OurPid = Shared->bStartedEnded ? 0 : Shared->StartedPid;
	}
	const FString Shipped = ShippedStamp();
	const bool bCarried = PlaytestAgentService::Carries(Shipped, HasInstaller());
	if (!bCarried || bInUse)
	{
		return true;  // (as always: a plugin without its own uses what runs; a connection made is left alone)
	}
	const FPlaytestServiceSearch Search = Options.Search ? Options.Search() : FPlaytestServiceSearch::FromEnvironment(FString());
	const bool bSetting = !Search.Setting.TrimStartAndEnd().IsEmpty();
	// (the one this editor started is used, whatever it says: e.g. the installed one, started when an update failed)
	bInUse = !bSetting && PlaytestAgentService::IsFrom(Announced, OurPid, PlaytestAgentService::ProcessParents());
	const EPlaytestAnnouncedStep Step = PlaytestAgentService::PlanAnnounced(Announced, Shipped, bCarried, bSetting, bInUse, bTried);
	const FString Said = Announced.bHasCommit ? FString::Printf(TEXT("commit \"%s\""), *Announced.Commit) : FString(TEXT("no commit"));
	if (Step == EPlaytestAnnouncedStep::Use)
	{
		return true;
	}
	if (Step == EPlaytestAnnouncedStep::UseOlder)
	{
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] An older agent service is running (pid %u, %s), and one was replaced this session already: using it"),
			Announced.Pid, *Said);
		return true;
	}
	{
		FScopeLock Locked(&Shared->Lock);
		Shared->bReplaceTried = true;  // (once a session: one that comes back, or won't stop, is used)
	}
	// Only the process that wrote brain.json: an agent service's program, created before it announced (a process id
	// reused by another process since is never stopped)
	const FString Program = FPlatformProcess::GetApplicationName(Announced.Pid);
	FString Why;
	if (!PlaytestAgentService::MayStop(Program, PlaytestAgentService::ProcessStartTime(Announced.Pid), Announced, Why))
	{
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] An older agent service is running (pid %u, %s), but %s: left running and used"),
			Announced.Pid, *Said, *Why);
		return true;
	}
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] An older agent service was running (pid %u): replacing it with this plugin's"), Announced.Pid);

	// Stopped off the game thread: the next Prepare (Busy until then) starts this plugin's, or uses it if it won't stop
	int32 Generation = 0;
	{
		FScopeLock Locked(&Shared->Lock);
		Generation = ++Shared->Generation;
		Shared->bReplacing = true;
		Shared->State = EPlaytestServiceState::Starting;
		Shared->Url.Empty();
		Shared->Problem.Empty();
		Shared->bOurs = false;
	}
	TSharedRef<FShared, ESPMode::ThreadSafe> State = Shared;
	const FString Brain = FPaths::Combine(Options.SettingsDir, BrainFile);
	const uint32 Pid = Announced.Pid;
	Async(EAsyncExecution::Thread, [State, Generation, Brain, Pid]()
	{
		FProcHandle Handle = FPlatformProcess::OpenProcess(Pid);
		if (Handle.IsValid())
		{
			FPlatformProcess::TerminateProc(Handle, true);  // (with what it started)
			FPlatformProcess::CloseProc(Handle);
		}
		const double Until = ServiceNow() + PlaytestAgentService::StopSeconds;
		while (FPlatformProcess::IsApplicationRunning(Pid) && ServiceNow() < Until)
		{
			FPlatformProcess::Sleep(0.1f);
		}
		if (FPlatformProcess::IsApplicationRunning(Pid))
		{
			UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] The older agent service (pid %u) didn't stop: using it"), Pid);
		}
		else
		{
			// (ended without a word: its brain.json is still there, unless one started since has written its own)
			FString Text;
			FPlaytestAnnouncement Still;
			if (FFileHelper::LoadFileToString(Text, *Brain) && PlaytestAgentService::ParseAnnouncement(Text, Still) && Still.Pid == Pid)
			{
				IFileManager::Get().Delete(*Brain, false, true, true);
			}
		}
		FScopeLock Locked(&State->Lock);
		State->bReplacing = false;
		if (State->Generation == Generation)
		{
			State->State = EPlaytestServiceState::Idle;
		}
	});
	return false;
}

FPlaytestAgentService::EPrepared FPlaytestAgentService::Start()
{
	FPlaytestServiceSearch Search = Options.Search ? Options.Search() : FPlaytestServiceSearch::FromEnvironment(FString());
	const FString Shipped = ShippedStamp();
	FString Installed;
	FFileHelper::LoadFileToString(Installed, *FPaths::Combine(Options.SettingsDir, PlaytestAgentService::StampFile));
	const bool bHasInstaller = HasInstaller();
	Search.bCarried = PlaytestAgentService::Carries(Shipped, bHasInstaller);
	const FPlaytestServiceCommand Command = PlaytestAgentService::Find(Search);
	bool bTried = false;
	{
		FScopeLock Locked(&Shared->Lock);
		bTried = Shared->bUpdateTried;
	}
	const EPlaytestServiceStep Step = PlaytestAgentService::PlanStart(Command, Shipped, Installed, Search.bCarried, bHasInstaller, bTried,
		Search.HomeDir, Search.bWindows);
	if (Step == EPlaytestServiceStep::Install)
	{
		// Another autopilot on PATH: the plugin's first (once a session; the next Prepare starts it). If that can't even
		// start, nothing is installed
		{
			FScopeLock Locked(&Shared->Lock);
			Shared->bUpdateTried = true;
		}
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Another autopilot is on PATH: installing this plugin's agent service instead"));
		if (Install())
		{
			return EPrepared::Busy;
		}
	}
	else if (Step == EPlaytestServiceStep::Update)
	{
		// An older install than the one the plugin carries (a newer download over an earlier one): this one first
		{
			FScopeLock Locked(&Shared->Lock);
			Shared->bUpdateTried = true;
			Shared->bUpdating = true;
		}
		UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] The agent service installed is older than this plugin's: installing this one"));
		if (Install())
		{
			return EPrepared::Busy;
		}
		FScopeLock Locked(&Shared->Lock);
		Shared->bUpdating = false;  // (it couldn't even start: the installed one is started)
		Shared->State = EPlaytestServiceState::Idle;
		Shared->Problem.Empty();
	}
	if (!Command.IsFound())
	{
		FScopeLock Locked(&Shared->Lock);
		if (!Command.Problem.IsEmpty())
		{
			Shared->State = EPlaytestServiceState::BadCommand;
			Shared->Problem = Command.Problem;
		}
		else if (Shared->State == EPlaytestServiceState::InstallFailed)
		{
			// (the install this Start asked for couldn't start: it says why)
		}
		else if (Shared->bInstalled)
		{
			Shared->State = EPlaytestServiceState::InstallFailed;
			Shared->Problem = FString::Printf(TEXT("The installer finished, but the agent service isn’t where it puts it (see %s)"), *Shared->LogPath);
		}
		else
		{
			Shared->State = EPlaytestServiceState::NotInstalled;
			Shared->Problem = TEXT("The agent service isn’t installed on this computer.");
		}
		return EPrepared::Failed;
	}

	const bool bWindows = PLATFORM_WINDOWS != 0;
	const FString LogsDir = FPaths::Combine(Options.SettingsDir, TEXT("logs"));
	IFileManager::Get().MakeDirectory(*LogsDir, true);
	PlaytestAgentService::PruneLogs(LogsDir, StartLogPrefix, LogsKept - 1);
	const FString LogFile = FPaths::ConvertRelativePathToFull(PlaytestAgentService::NewLogPath(LogsDir, StartLogPrefix, FDateTime::Now()));
	FString Exe, Params;
	PlaytestAgentService::StartCommandLine(Command, Options.ExitWithPid, LogFile, bWindows, ShellPath(bWindows), Exe, Params);
	WriteLogHeader(LogFile, TEXT("started the agent service"), Exe, Params);

	uint32 Pid = 0;
	FProcHandle Handle = StartHidden(Exe, Params, &Pid, Options.SettingsDir);
	int32 Generation = 0;
	{
		FScopeLock Locked(&Shared->Lock);
		Shared->LogPath = LogFile;
		Generation = ++Shared->Generation;
		if (!Handle.IsValid())
		{
			Shared->State = EPlaytestServiceState::Stopped;
			Shared->Problem = FString::Printf(TEXT("Couldn’t start the agent service (%s)"), *Command.Exe);
			return EPrepared::Failed;
		}
		Shared->State = EPlaytestServiceState::Starting;
		Shared->Problem.Empty();
		Shared->Url.Empty();
		Shared->bOurs = false;
		Shared->StartedPid = Pid;
		Shared->bStartedEnded = false;
		Shared->AnnouncedAt = 0;
		Shared->EndedAt = 0;
		Shared->GoneSince = 0;
	}
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Starting the agent service (%s), its output in %s"), *Command.Exe, *LogFile);

	// Watched off the game thread: where it says it listens (brain.json naming it or a process it started), or its end
	TSharedRef<FShared, ESPMode::ThreadSafe> State = Shared;
	const FString Dir = Options.SettingsDir;
	Async(EAsyncExecution::Thread, [State, Handle, Pid, Generation, Dir, LogFile]() mutable
	{
		const double Deadline = ServiceNow() + PlaytestAgentService::StartSeconds;
		bool bAnnounced = false;
		bool bLate = false;
		while (!State->bCancel)
		{
			if (!bAnnounced)
			{
				FPlaytestAnnouncement Announced;
				if (PlaytestAgentService::ReadAnnouncement(Dir, BrainFile, Announced)
					&& PlaytestAgentService::IsFrom(Announced, Pid, PlaytestAgentService::ProcessParents()))
				{
					bAnnounced = true;
					FScopeLock Locked(&State->Lock);
					if (State->Generation == Generation)
					{
						State->State = EPlaytestServiceState::Running;
						State->Url = Announced.Url;
						State->bOurs = true;
						State->Problem.Empty();
						State->AnnouncedAt = ServiceNow();
					}
				}
			}
			if (!FPlatformProcess::IsProcRunning(Handle))
			{
				int32 Code = 0;
				FPlatformProcess::GetProcReturnCode(Handle, &Code);
				FScopeLock Locked(&State->Lock);
				if (State->Generation == Generation)
				{
					State->bStartedEnded = true;
					State->EndedAt = ServiceNow();
					if (State->State == EPlaytestServiceState::Starting || (State->State == EPlaytestServiceState::Stopped && bLate))
					{
						State->State = EPlaytestServiceState::Stopped;
						State->Problem = FString::Printf(TEXT("The agent service stopped (see %s)"), *LogFile);
					}
				}
				UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] The agent service ended (exit code %d)"), Code);
				break;
			}
			if (!bAnnounced && !bLate && ServiceNow() > Deadline)
			{
				bLate = true;  // (still watched: once it says where it listens, Prepare finds it)
				FScopeLock Locked(&State->Lock);
				if (State->Generation == Generation && State->State == EPlaytestServiceState::Starting)
				{
					State->State = EPlaytestServiceState::Stopped;
					State->Problem = FString::Printf(TEXT("The agent service didn’t start within %.0f s (see %s)"), PlaytestAgentService::StartSeconds, *LogFile);
				}
			}
			FPlatformProcess::Sleep(bAnnounced ? 0.5f : 0.25f);
		}
		FPlatformProcess::CloseProc(Handle);
	});
	return EPrepared::Busy;
}

bool FPlaytestAgentService::Install()
{
	if (!HasInstaller())
	{
		return false;
	}
	{
		FScopeLock Locked(&Shared->Lock);
		if (Shared->bRemoved)
		{
			return false;
		}
		if (Shared->State == EPlaytestServiceState::Installing)
		{
			return true;
		}
	}
	const bool bWindows = PLATFORM_WINDOWS != 0;
	const FString LogsDir = FPaths::Combine(Options.SettingsDir, TEXT("logs"));
	IFileManager::Get().MakeDirectory(*LogsDir, true);
	PlaytestAgentService::PruneLogs(LogsDir, InstallLogPrefix, LogsKept - 1);
	const FString LogFile = FPaths::ConvertRelativePathToFull(PlaytestAgentService::NewLogPath(LogsDir, InstallLogPrefix, FDateTime::Now()));
	FString Exe, Params;
	PlaytestAgentService::InstallCommandLine(FPaths::ConvertRelativePathToFull(Options.AgentServiceDir), LogFile, bWindows,
		ShellPath(bWindows), bWindows ? PowerShellPath() : FString(), Exe, Params);
	WriteLogHeader(LogFile, TEXT("installs the agent service"), Exe, Params);

	FProcHandle Handle = StartHidden(Exe, Params, nullptr, Options.SettingsDir);
	int32 Generation = 0;
	{
		FScopeLock Locked(&Shared->Lock);
		Shared->LogPath = LogFile;
		Generation = ++Shared->Generation;
		if (!Handle.IsValid())
		{
			Shared->State = EPlaytestServiceState::InstallFailed;
			Shared->Problem = FString::Printf(TEXT("Couldn’t install the agent service (see %s)"), *LogFile);
			return false;
		}
		Shared->State = EPlaytestServiceState::Installing;
		Shared->Problem.Empty();
		Shared->bInstalled = false;
	}
	UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] Installing the agent service, its output in %s"), *LogFile);

	TSharedRef<FShared, ESPMode::ThreadSafe> State = Shared;
	Async(EAsyncExecution::Thread, [State, Handle, Generation, LogFile]() mutable
	{
		// (under the lock) An install that didn't work: a problem, unless it was an update over an installed agent
		// service, which is started all the same
		auto Failed = [&State](const FString& Problem)
		{
			if (State->bUpdating)
			{
				UE_LOG(LogPlaytest, Warning, TEXT("[Autopilot] %s: starting the agent service installed before"), *Problem);
				State->State = EPlaytestServiceState::Idle;
				State->Problem.Empty();
			}
			else
			{
				State->State = EPlaytestServiceState::InstallFailed;
				State->Problem = Problem;
			}
			State->bUpdating = false;
		};
		const double Deadline = ServiceNow() + PlaytestAgentService::InstallSeconds;
		while (!State->bCancel)
		{
			if (!FPlatformProcess::IsProcRunning(Handle))
			{
				int32 Code = -1;
				FPlatformProcess::GetProcReturnCode(Handle, &Code);
				UE_LOG(LogPlaytest, Display, TEXT("[Autopilot] The agent service's installer ended (exit code %d)"), Code);
				FScopeLock Locked(&State->Lock);
				if (State->Generation == Generation)
				{
					if (Code == 0)
					{
						State->State = EPlaytestServiceState::Idle;  // (the next Prepare finds and starts it)
						State->bInstalled = true;
						State->bUpdating = false;
					}
					else
					{
						Failed(FString::Printf(TEXT("Couldn’t install the agent service (see %s)"), *LogFile));
					}
				}
				break;
			}
			if (ServiceNow() > Deadline)
			{
				FPlatformProcess::TerminateProc(Handle, true);
				FScopeLock Locked(&State->Lock);
				if (State->Generation == Generation)
				{
					Failed(FString::Printf(TEXT("The installer didn’t finish within %.0f minutes (see %s)"),
						PlaytestAgentService::InstallSeconds / 60.0, *LogFile));
				}
				break;
			}
			FPlatformProcess::Sleep(0.25f);
		}
		FPlatformProcess::CloseProc(Handle);
	});
	return true;
}
