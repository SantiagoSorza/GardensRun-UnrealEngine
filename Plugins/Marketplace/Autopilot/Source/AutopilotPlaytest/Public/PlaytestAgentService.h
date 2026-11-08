#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Templates/Function.h"

/**
 * The agent service on this computer, started by the editor itself (docs/protocol.md, "Started by the plugin"):
 *
 *   - Finding it: the "Agent service command" setting (a full command line), else `autopilot` on PATH, else where the
 *     installer puts it (%USERPROFILE%\.local\bin\autopilot.exe, ~/.local/bin/autopilot). A plugin that carries an
 *     agent service (Carries: a release download) never takes one from PATH: the installer's place only.
 *   - Starting it: `<autopilot> serve --port 0 --exit-with <the editor's pid>`, hidden, its output appended to
 *     Autopilot/logs/agent-service-<time>.log (the 5 newest kept); it says where it listens in Autopilot/brain.json,
 *     with its process id and commit, and ends with the editor. One already announced and alive is used instead: never
 *     a second. But a plugin that carries an agent service uses only one built from the same commit (PlanAnnounced):
 *     another is stopped, once a session, and this plugin's started instead (unless the setting names a command).
 *   - Installing it: the installer the plugin carries (AgentService/install.ps1, install.sh), hidden, its output in
 *     Autopilot/logs/install-<time>.log. Also before a start, once a session (PlanStart), when the installer's own
 *     install is older than the one the plugin carries (NeedsUpdate; if that fails, the installed one is started all
 *     the same) or, for a plugin that carries one, in place of an `autopilot` on PATH (nothing installed at all: the
 *     first-run setup's Install).
 *
 * Processes are watched on threads of their own: nothing here waits on the game thread.
 */

/** The `autopilot` command found: a program, and what comes before `serve` on its command line. */
struct AUTOPILOTPLAYTEST_API FPlaytestServiceCommand
{
	/** The program: a full path when it was found. */
	FString Exe;
	/** What comes before "serve" (e.g. "-m autopilot" for a Python), "" for autopilot itself. */
	FString Args;
	/** Where it was found: "setting", "path" or "installer"; "" when none was. */
	FString Source;
	/** The setting names a program that isn't on this computer (in words for the user). */
	FString Problem;
	/** An `autopilot` on PATH that wasn't taken because the plugin carries its own (Search.bCarried); "" when none. */
	FString PathSkipped;

	bool IsFound() const { return !Exe.IsEmpty() && Problem.IsEmpty(); }
};

/** Where to look for the `autopilot` command (all of it given, so tests can say what is there). */
struct AUTOPILOTPLAYTEST_API FPlaytestServiceSearch
{
	/** The "Agent service command" setting ("" : look for it). */
	FString Setting;
	/** The PATH environment variable. */
	FString PathEnv;
	/** The user's home folder (%USERPROFILE%, $HOME). */
	FString HomeDir;
	bool bWindows = PLATFORM_WINDOWS != 0;
	/** The plugin carries an agent service (PlaytestAgentService::Carries): PATH isn't looked at, only the installer's place. */
	bool bCarried = false;
	/** Whether a file is there (default: FPaths::FileExists). */
	TFunction<bool(const FString&)> FileExists;

	/** This computer's PATH and home folder, with Setting. */
	static FPlaytestServiceSearch FromEnvironment(const FString& Setting);
};

/** What a running agent service wrote to Autopilot/brain.json. */
struct AUTOPILOTPLAYTEST_API FPlaytestAnnouncement
{
	FString Url;
	uint32 Pid = 0;
	/** When it announced itself (Python's time.time() then), in Unix seconds; bHasStarted false when not said. */
	double Started = 0;
	bool bHasStarted = false;
	/** The commit it was built from ("" for a source checkout); bHasCommit false when it doesn't say (older than commits). */
	FString Commit;
	bool bHasCommit = false;
};

/** What an agent-service.json says: the wheel's sha256 (lower case) and the commit it was built from ("" when not said). */
struct AUTOPILOTPLAYTEST_API FPlaytestServiceStamp
{
	FString Sha256;
	FString Commit;
};

/** What Start does with the command found (PlaytestAgentService::PlanStart). */
enum class EPlaytestServiceStep : uint8
{
	Start,    // start it
	Install,  // install the plugin's own first (in place of a PATH autopilot); then start it
	Update,   // install the plugin's own over the one found first; if that fails, the one found is started all the same
	Missing,  // nothing to start (not installed, or the setting names nothing: Command.Problem)
};

/** What Prepare does with the agent service announced in brain.json (PlaytestAgentService::PlanAnnounced). */
enum class EPlaytestAnnouncedStep : uint8
{
	Use,       // connect to it
	UseOlder,  // connect to it, though it isn't the plugin's (one was replaced this session already)
	Replace,   // stop it (if its program is an agent service's), then start the plugin's
};

/** Removing Autopilot from this computer (FPlaytestAgentService::Uninstall). */
enum class EPlaytestUninstallState : uint8
{
	None,            // not asked
	Removing,        // `autopilot uninstall` running (or the settings folder being removed)
	Removed,         // all of it removed (exit code 0)
	RemovedButSome,  // removed but for what the log names (exit code 3: files in use, memory kept for its unsent runs)
	Failed,          // nothing removed, or it couldn't start, or it didn't finish in time
};

enum class EPlaytestServiceState : uint8
{
	Idle,           // nothing tried yet (or a problem was cleared to try again)
	NotInstalled,   // no autopilot command found
	BadCommand,     // the "Agent service command" setting names a program that isn't there
	Installing,
	InstallFailed,
	Starting,       // started, not yet said where it listens
	Running,        // one is announced and alive (this editor's or another's)
	Stopped,        // the one started ended before it said where it listens, or didn't say in time
};

namespace PlaytestAgentService
{
	/** The program and the rest of a command line: a quoted program, or the longest leading words that name a file. */
	AUTOPILOTPLAYTEST_API void SplitCommand(const FString& Line, FString& OutExe, FString& OutArgs,
		const TFunction<bool(const FString&)>& FileExists = nullptr, bool bWindows = PLATFORM_WINDOWS != 0);

	/** Finds the autopilot command (the setting, PATH, the installer's place, in that order; PATH skipped when bCarried). */
	AUTOPILOTPLAYTEST_API FPlaytestServiceCommand Find(const FPlaytestServiceSearch& Search);

	/** "<args> serve --port 0 --exit-with <pid>". */
	AUTOPILOTPLAYTEST_API FString ServeArgs(const FPlaytestServiceCommand& Command, uint32 ExitWithPid);

	/**
	 * The process to start for the agent service: a shell (Windows: cmd.exe, Shell; elsewhere /bin/sh) running autopilot
	 * serve with its output appended to LogFile.
	 */
	AUTOPILOTPLAYTEST_API void StartCommandLine(const FPlaytestServiceCommand& Command, uint32 ExitWithPid, const FString& LogFile,
		bool bWindows, const FString& Shell, FString& OutExe, FString& OutParams);

	/** The installer script in the plugin's AgentService folder (install.ps1 on Windows, install.sh elsewhere). */
	AUTOPILOTPLAYTEST_API FString InstallerScript(const FString& AgentServiceDir, bool bWindows = PLATFORM_WINDOWS != 0);

	/**
	 * The process to start to install the agent service: Windows `powershell -NoProfile -ExecutionPolicy Bypass -File
	 * install.ps1` (PowerShell: its path), elsewhere `sh install.sh`, run by the shell with their output in LogFile.
	 */
	AUTOPILOTPLAYTEST_API void InstallCommandLine(const FString& AgentServiceDir, const FString& LogFile, bool bWindows,
		const FString& Shell, const FString& PowerShell, FString& OutExe, FString& OutParams);

	/** agent-service.json: which agent service the AgentService folder carries, and (its copy in Autopilot's folder) which
	 *  one the installer installed last. */
	constexpr const TCHAR* StampFile = TEXT("agent-service.json");

	/**
	 * Whether the agent service found is an older install than the one the plugin carries (a newer download over an
	 * earlier one): Command is the installer's own (its place: not the setting, nor another autopilot on PATH), the plugin's
	 * agent-service.json (Shipped) names a wheel, and the installed one's (Installed, "" when there is none) another.
	 */
	AUTOPILOTPLAYTEST_API bool NeedsUpdate(const FString& Shipped, const FString& Installed, const FPlaytestServiceCommand& Command,
		const FString& HomeDir, bool bWindows = PLATFORM_WINDOWS != 0);

	/** An agent-service.json's text read (nothing when it isn't one). */
	AUTOPILOTPLAYTEST_API FPlaytestServiceStamp ParseStamp(const FString& Text);

	/**
	 * Whether the plugin carries an agent service of its own: its agent-service.json (Shipped) names a commit or a wheel,
	 * and the installer is there. A plugin that doesn't (a source checkout) finds and reuses agent services as it always did.
	 */
	AUTOPILOTPLAYTEST_API bool Carries(const FString& Shipped, bool bHasInstaller);

	/**
	 * What to do with the command Find found: start it; install the plugin's own first (the plugin carries one and an
	 * `autopilot` on PATH would have run: Command.PathSkipped); update it first (NeedsUpdate); or nothing to start (nothing installed at all: the first-run setup's Install does it). Installs and
	 * updates happen once a session (bInstallTried: one was tried already) and only with the installer (bHasInstaller).
	 */
	AUTOPILOTPLAYTEST_API EPlaytestServiceStep PlanStart(const FPlaytestServiceCommand& Command, const FString& Shipped,
		const FString& Installed, bool bCarried, bool bHasInstaller, bool bInstallTried, const FString& HomeDir,
		bool bWindows = PLATFORM_WINDOWS != 0);

	/** Whether the agent service announced was built from the commit the plugin's agent-service.json (Shipped) names. */
	AUTOPILOTPLAYTEST_API bool IsSameBuild(const FPlaytestAnnouncement& Announced, const FString& Shipped);

	/**
	 * What to do with the agent service announced: used as it always was when the plugin carries none, the "Agent service
	 * command" setting is set (bSetting), it is the one this plugin uses or started (bInUse), it is the same build, or the
	 * shipped agent-service.json names no commit to compare; else replaced, once a session (bReplaceTried: used).
	 */
	AUTOPILOTPLAYTEST_API EPlaytestAnnouncedStep PlanAnnounced(const FPlaytestAnnouncement& Announced, const FString& Shipped,
		bool bCarried, bool bSetting, bool bInUse, bool bReplaceTried);

	/**
	 * Whether a process's program (its full path) is an agent service's, the only kind the plugin stops: autopilot, or a
	 * Python (python, pythonw, python3, python3.N: uv's tool runs Autopilot under its own), with or without .exe, any case.
	 */
	AUTOPILOTPLAYTEST_API bool IsAgentServiceProgram(const FString& Program);

	/** How much later than the announcement's "started" its process may have been created and still be its writer. */
	constexpr double OwnerSlackSeconds = 5.0;

	/**
	 * Whether the process the announcement names may be stopped: its program is an agent service's (IsAgentServiceProgram)
	 * and it is the process that wrote the announcement: created (ProcessStarted, Unix seconds; 0 when it can't be read)
	 * no later than the announcement's "started" + OwnerSlackSeconds. A process id reused by a process created after the
	 * old one ended is never stopped; nor is any process when "started" is missing or the start time can't be read.
	 * OutWhy: why not, in words for the log.
	 */
	AUTOPILOTPLAYTEST_API bool MayStop(const FString& Program, double ProcessStarted, const FPlaytestAnnouncement& Announced, FString& OutWhy);

	/** When a process was created, in Unix seconds (0 when it can't be read). */
	AUTOPILOTPLAYTEST_API double ProcessStartTime(uint32 Pid);

	/** `ps -o etime=` ("[[dd-]hh:]mm:ss"), in seconds; -1 when it isn't one. */
	AUTOPILOTPLAYTEST_API double ParseElapsed(const FString& Text);

	/** Reads brain.json's text. False when it isn't one, has no process id, or names anything but this machine. */
	AUTOPILOTPLAYTEST_API bool ParseAnnouncement(const FString& Text, FPlaytestAnnouncement& Out);

	/** Dir/File read and parsed, and its process alive. */
	AUTOPILOTPLAYTEST_API bool ReadAnnouncement(const FString& Dir, const TCHAR* File, FPlaytestAnnouncement& Out);

	/**
	 * Whether the announcing process is Pid or was started by it (a launcher in between: cmd.exe, uv's autopilot.exe and a
	 * venv's python.exe each start the next process): ParentOf gives a process's parent (0: unknown).
	 */
	AUTOPILOTPLAYTEST_API bool IsFrom(const FPlaytestAnnouncement& Announcement, uint32 Pid, const TFunction<uint32(uint32)>& ParentOf);

	/** This computer's processes' parents, as they are now (Windows; elsewhere every parent is unknown). */
	AUTOPILOTPLAYTEST_API TFunction<uint32(uint32)> ProcessParents();

	/** Dir/<Prefix><yyyymmdd-hhmmss>.log (with -2, -3... when taken). */
	AUTOPILOTPLAYTEST_API FString NewLogPath(const FString& Dir, const FString& Prefix, const FDateTime& When);

	/** Deletes Dir/<Prefix>*.log but the Keep newest. */
	AUTOPILOTPLAYTEST_API void PruneLogs(const FString& Dir, const FString& Prefix, int32 Keep);

	/** "<args> uninstall --yes [--keep-memory] --log <LogFile>": what `autopilot uninstall` is run with. */
	AUTOPILOTPLAYTEST_API FString UninstallArgs(const FPlaytestServiceCommand& Command, bool bKeepMemory, const FString& LogFile,
		bool bWindows = PLATFORM_WINDOWS != 0);

	/**
	 * The process to start to remove Autopilot from this computer: a shell (Windows: cmd.exe, Shell; elsewhere /bin/sh)
	 * running `autopilot uninstall` unbuffered, with its output appended to LogFile.
	 */
	AUTOPILOTPLAYTEST_API void UninstallCommandLine(const FPlaytestServiceCommand& Command, bool bKeepMemory, const FString& LogFile,
		bool bWindows, const FString& Shell, FString& OutExe, FString& OutParams);

	/**
	 * What the plugin removes itself when the agent service isn't installed (its `uninstall` can't run): what is in
	 * Autopilot's settings folder (SettingsDir), but memory/ (the shared runs in it not sent
	 * yet are sent only by the agent service: shared run data is never removed) and aura-brain.json (the Aura app's).
	 * Returns the exit code `autopilot uninstall` would: 0, or 3 when something stays that wasn't asked to (memory/ with
	 * bKeepMemory false, files in use). OutLog: what was removed and kept, a line each.
	 */
	AUTOPILOTPLAYTEST_API int32 RemoveSettingsFolder(const FString& SettingsDir, bool bKeepMemory, FString& OutLog);

	/** How long removing Autopilot may take (it sends the shared runs not sent yet first). */
	constexpr double UninstallSeconds = 10.0 * 60.0;

	/** How long a start may take to say where it listens. */
	constexpr double StartSeconds = 30.0;
	/** How long an install may take. */
	constexpr double InstallSeconds = 15.0 * 60.0;
	/** How long an older agent service may take to end once stopped. */
	constexpr double StopSeconds = 5.0;
}

/** The agent service this editor starts, installs and watches. Game-thread calls; the processes are watched off it. */
class AUTOPILOTPLAYTEST_API FPlaytestAgentService
{
public:
	struct FOptions
	{
		/** Autopilot's folder (brain.json, logs/): PlaytestAura::SettingsDir() but in tests. */
		FString SettingsDir;
		/** The plugin's AgentService folder (the installer), "" when the plugin has none. */
		FString AgentServiceDir;
		/** Where to look for the autopilot command, asked at each start. */
		TFunction<FPlaytestServiceSearch()> Search;
		/** The process the agent service ends with (--exit-with): this editor. */
		uint32 ExitWithPid = 0;
	};

	enum class EPrepared : uint8
	{
		Ready,   // one is running: connect to OutUrl
		Busy,    // installing or starting: ask again soon
		Failed,  // see GetState and GetProblem; nothing is tried again until Retry
	};

	explicit FPlaytestAgentService(FOptions InOptions);
	~FPlaytestAgentService();

	/**
	 * Before each connection attempt: one announced and alive (OutUrl), else one is started (Busy), or why not. An older
	 * one announced is stopped first (Busy) when the plugin carries its own (PlanAnnounced).
	 */
	EPrepared Prepare(FString& OutUrl);
	/** Clears a problem, so the next Prepare tries again. */
	void Retry();
	/** Runs the installer the plugin carries (the next Prepare starts what it installed). False when it can't. */
	bool Install();
	/** Whether the plugin carries the installer (a release download does; a repository checkout doesn't). */
	bool HasInstaller() const;
	/** Stops watching (the editor is closing). The agent service ends by itself once the editor has (--exit-with). */
	void Shutdown();

	/**
	 * Removes Autopilot from this computer: `<autopilot> uninstall --yes [--keep-memory]` (the command Start would run),
	 * hidden, from the temporary folder (a folder that is a process's current one can't be removed on Windows), its
	 * output in <temp>/autopilot-uninstall-<time>.log; without one installed, RemoveSettingsFolder. From then on this
	 * editor session starts, installs and connects to no agent service (Prepare fails, Install doesn't) until
	 * LeaveRemoved.
	 */
	void Uninstall(bool bKeepMemory);
	EPlaytestUninstallState GetUninstallState() const;
	/** The log of the last Uninstall ("" before one). */
	FString GetUninstallLog() const;
	/** The Keep what Autopilot learned choice of the last Uninstall. */
	bool GetUninstallKeepsMemory() const;
	/** Uninstall was called, and LeaveRemoved not since. */
	bool IsRemoved() const;
	/** Back to as it was: the agent service is found, installed and started again as usual (Set up again). */
	void LeaveRemoved();
	/**
	 * Whether no agent service is on this computer: none announced in brain.json, and Find finds no command (nor a
	 * setting naming one that isn't there, nor one Start installs over by itself).
	 */
	bool IsMissing() const;

	EPlaytestServiceState GetState() const;
	/** Where the one running listens ("" when none is). */
	FString GetUrl() const;
	/** What went wrong, in words for the user (with the log's path). */
	FString GetProblem() const;
	/** The log of the last start or install ("" before one). */
	FString GetLogPath() const;
	/** Whether the one running was started by this editor. */
	bool IsOurs() const;
	const FOptions& GetOptions() const { return Options; }

	struct FShared;

private:
	EPrepared Start();
	/** Whether the one announced is used (else it is being stopped: Busy). */
	bool UseAnnounced(const FPlaytestAnnouncement& Announced);
	/** The plugin's agent-service.json's text ("" when it has none). */
	FString ShippedStamp() const;

	FOptions Options;
	TSharedRef<FShared, ESPMode::ThreadSafe> Shared;
};
