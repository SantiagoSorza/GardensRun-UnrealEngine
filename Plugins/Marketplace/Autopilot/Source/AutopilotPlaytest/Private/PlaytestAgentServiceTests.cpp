// Automation tests of the agent service the editor starts and of the account frames (Session Frontend > Automation:
// AutopilotPlaytest.AgentService, AutopilotPlaytest.Account). Everything is written under the project's Saved/Automation; the
// process tests (Windows) run stand-ins written here, never the real autopilot or installer.

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PlaytestAccount.h"
#include "PlaytestAgentService.h"
#include "PlaytestAura.h"
#include "PlaytestJson.h"
#include "PlaytestSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A folder of its own under Saved/Automation, removed when done. */
	struct FServiceScratch
	{
		FString Path;
		FServiceScratch()
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("AutopilotService"),
				FGuid::NewGuid().ToString()));
			IFileManager::Get().MakeDirectory(*Path, true);
		}
		~FServiceScratch()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};

	/** A search over the files given: nothing else is there. */
	FPlaytestServiceSearch SearchOver(const TSet<FString>& Files, bool bWindows, const FString& Setting = FString(), const FString& PathEnv = FString())
	{
		FPlaytestServiceSearch Search;
		Search.Setting = Setting;
		Search.PathEnv = PathEnv;
		Search.HomeDir = bWindows ? TEXT("C:/Users/me") : TEXT("/home/me");
		Search.bWindows = bWindows;
		Search.FileExists = [Files](const FString& Path) { return Files.Contains(Path); };
		return Search;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceFindTest, "AutopilotPlaytest.AgentService.Find",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceFindTest::RunTest(const FString& Parameters)
{
	// Nothing anywhere: not installed
	FPlaytestServiceCommand Found = PlaytestAgentService::Find(SearchOver({}, true, FString(), TEXT("C:/Windows;C:/tools")));
	TestFalse(TEXT("nothing: not found"), Found.IsFound());
	TestTrue(TEXT("nothing: no problem"), Found.Problem.IsEmpty());

	// Where the installer puts it
	Found = PlaytestAgentService::Find(SearchOver({ TEXT("C:/Users/me/.local/bin/autopilot.exe") }, true));
	TestEqual(TEXT("installer place"), Found.Exe, FString(TEXT("C:/Users/me/.local/bin/autopilot.exe")));
	TestEqual(TEXT("installer source"), Found.Source, FString(TEXT("installer")));
	TestTrue(TEXT("installer: no args"), Found.Args.IsEmpty());
	Found = PlaytestAgentService::Find(SearchOver({ TEXT("/home/me/.local/bin/autopilot") }, false));
	TestEqual(TEXT("installer place, elsewhere"), Found.Exe, FString(TEXT("/home/me/.local/bin/autopilot")));

	// PATH before the installer's place, in PATH's order; Windows tries .exe, .cmd, .bat
	Found = PlaytestAgentService::Find(SearchOver({ TEXT("C:/tools/autopilot.cmd"), TEXT("C:/py/autopilot.exe"), TEXT("C:/Users/me/.local/bin/autopilot.exe") }, true,
		FString(), TEXT("C:/Windows; \"C:/tools\" ;;C:/py")));
	TestEqual(TEXT("PATH first match"), Found.Exe, FString(TEXT("C:/tools/autopilot.cmd")));
	TestEqual(TEXT("PATH source"), Found.Source, FString(TEXT("path")));
	Found = PlaytestAgentService::Find(SearchOver({ TEXT("/opt/x/autopilot") }, false, FString(), TEXT("/usr/bin:/opt/x")));
	TestEqual(TEXT("PATH elsewhere (':')"), Found.Exe, FString(TEXT("/opt/x/autopilot")));

	// The setting comes first: a full command line
	const TSet<FString> Venv = { TEXT("C:\\autopilot\\.venv\\Scripts\\python.exe"), TEXT("C:/Users/me/.local/bin/autopilot.exe") };
	Found = PlaytestAgentService::Find(SearchOver(Venv, true, TEXT("  C:\\autopilot\\.venv\\Scripts\\python.exe -m autopilot ")));
	TestEqual(TEXT("setting exe"), Found.Exe, FString(TEXT("C:\\autopilot\\.venv\\Scripts\\python.exe")));
	TestEqual(TEXT("setting args"), Found.Args, FString(TEXT("-m autopilot")));
	TestEqual(TEXT("setting source"), Found.Source, FString(TEXT("setting")));
	// ... a path with spaces, quoted or not, and .exe left out
	const TSet<FString> Spaced = { TEXT("C:/Program Files/Python 3/python.exe") };
	Found = PlaytestAgentService::Find(SearchOver(Spaced, true, TEXT("\"C:/Program Files/Python 3/python.exe\" -m autopilot")));
	TestEqual(TEXT("quoted exe"), Found.Exe, FString(TEXT("C:/Program Files/Python 3/python.exe")));
	TestEqual(TEXT("quoted args"), Found.Args, FString(TEXT("-m autopilot")));
	Found = PlaytestAgentService::Find(SearchOver(Spaced, true, TEXT("C:/Program Files/Python 3/python -m autopilot")));
	TestEqual(TEXT("unquoted spaces, no .exe"), Found.Exe, FString(TEXT("C:/Program Files/Python 3/python.exe")));
	TestEqual(TEXT("unquoted args"), Found.Args, FString(TEXT("-m autopilot")));
	// ... a bare name, looked up on PATH
	Found = PlaytestAgentService::Find(SearchOver({ TEXT("C:/py/python.exe") }, true, TEXT("python -m autopilot"), TEXT("C:/py")));
	TestEqual(TEXT("bare name on PATH"), Found.Exe, FString(TEXT("C:/py/python.exe")));
	TestTrue(TEXT("bare name found"), Found.IsFound());
	// ... and one that isn't there is a problem (not a fall back to PATH)
	Found = PlaytestAgentService::Find(SearchOver(Venv, true, TEXT("D:/nowhere/python.exe -m autopilot")));
	TestFalse(TEXT("missing setting: not found"), Found.IsFound());
	TestTrue(TEXT("missing setting: says so"), Found.Problem.Contains(TEXT("D:/nowhere/python.exe")));

	FString Exe, Args;
	PlaytestAgentService::SplitCommand(TEXT("\"C:/a b/x.exe\""), Exe, Args);
	TestEqual(TEXT("quoted only"), Exe, FString(TEXT("C:/a b/x.exe")));
	TestTrue(TEXT("quoted only: no args"), Args.IsEmpty());
	PlaytestAgentService::SplitCommand(TEXT("autopilot"), Exe, Args);
	TestEqual(TEXT("one word"), Exe, FString(TEXT("autopilot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestSettingsDirTest, "AutopilotPlaytest.AgentService.SettingsDir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestSettingsDirTest::RunTest(const FString& Parameters)
{
	// The folder the agent service uses (autopilot/bridge.py brain_file), not Unreal's UserSettingsDir: on macOS that is
	// under Epic/, and the plugin then never found the service's brain.json, so started one more every time
#if PLATFORM_MAC
	const FString Settings = PlaytestAura::SettingsDir();
	TestTrue(TEXT("macOS: in Application Support"), Settings.EndsWith(TEXT("/Library/Application Support/Autopilot")));
	TestFalse(TEXT("macOS: not under Epic/"), Settings.Contains(TEXT("/Epic/")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceUpdateTest, "AutopilotPlaytest.AgentService.Update",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceUpdateTest::RunTest(const FString& Parameters)
{
	// The plugin carries one wheel, the installer installed another (or said nothing: an installer from before it did)
	const FString Shipped = TEXT("{\"sha256\": \"ABC123\", \"commit\": \"36a6fec\", \"wheel\": \"autopilot-0.1.0-py3-none-any.whl\"}");
	const FString Same = TEXT("{\"sha256\": \"abc123\", \"commit\": \"36a6fec\"}");
	const FString Older = TEXT("{\"sha256\": \"def456\", \"commit\": \"6a0bf52\"}");
	const FPlaytestServiceCommand Installer = PlaytestAgentService::Find(SearchOver({ TEXT("C:/Users/me/.local/bin/autopilot.exe") }, true));
	const FString Home = TEXT("C:/Users/me");
	TestTrue(TEXT("an older install"), PlaytestAgentService::NeedsUpdate(Shipped, Older, Installer, Home, true));
	TestTrue(TEXT("an install that didn't say"), PlaytestAgentService::NeedsUpdate(Shipped, FString(), Installer, Home, true));
	TestTrue(TEXT("an unreadable one"), PlaytestAgentService::NeedsUpdate(Shipped, TEXT("{not json"), Installer, Home, true));
	TestFalse(TEXT("the same (any case)"), PlaytestAgentService::NeedsUpdate(Shipped, Same, Installer, Home, true));
	TestFalse(TEXT("a plugin that carries none"), PlaytestAgentService::NeedsUpdate(FString(), Older, Installer, Home, true));
	TestFalse(TEXT("one naming no wheel"), PlaytestAgentService::NeedsUpdate(TEXT("{}"), Older, Installer, Home, true));

	// Only the installer's own: on PATH as uv puts it there, Windows paths in any case and either slash
	FPlaytestServiceCommand OnPath = PlaytestAgentService::Find(SearchOver({ TEXT("C:/Users/me/.local/bin/autopilot.exe") }, true,
		FString(), TEXT("C:/Users/me/.local/bin")));
	TestEqual(TEXT("found on PATH"), OnPath.Source, FString(TEXT("path")));
	TestTrue(TEXT("the installer's, on PATH"), PlaytestAgentService::NeedsUpdate(Shipped, Older, OnPath, Home, true));
	OnPath.Exe = TEXT("c:\\users\\ME\\.local\\bin\\AUTOPILOT.EXE");
	TestTrue(TEXT("Windows: any case, backslashes"), PlaytestAgentService::NeedsUpdate(Shipped, Older, OnPath, Home, true));
	const FPlaytestServiceCommand Elsewhere = PlaytestAgentService::Find(SearchOver({ TEXT("C:/py/autopilot.exe") }, true, FString(), TEXT("C:/py")));
	TestFalse(TEXT("another autopilot on PATH"), PlaytestAgentService::NeedsUpdate(Shipped, Older, Elsewhere, Home, true));
	const FPlaytestServiceCommand Setting = PlaytestAgentService::Find(SearchOver({ TEXT("C:/Users/me/.local/bin/autopilot.exe") }, true,
		TEXT("C:/Users/me/.local/bin/autopilot.exe")));
	TestEqual(TEXT("the setting's"), Setting.Source, FString(TEXT("setting")));
	TestFalse(TEXT("the setting's, even there"), PlaytestAgentService::NeedsUpdate(Shipped, Older, Setting, Home, true));
	TestFalse(TEXT("none found"), PlaytestAgentService::NeedsUpdate(Shipped, Older, FPlaytestServiceCommand(), Home, true));
	const FPlaytestServiceCommand Unix = PlaytestAgentService::Find(SearchOver({ TEXT("/home/me/.local/bin/autopilot") }, false));
	TestTrue(TEXT("elsewhere"), PlaytestAgentService::NeedsUpdate(Shipped, Older, Unix, TEXT("/home/me"), false));
	FPlaytestServiceCommand Cased = Unix;
	Cased.Exe = TEXT("/home/me/.local/bin/Autopilot");
	TestFalse(TEXT("elsewhere: case counts"), PlaytestAgentService::NeedsUpdate(Shipped, Older, Cased, TEXT("/home/me"), false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceCarriedTest, "AutopilotPlaytest.AgentService.Carried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceCarriedTest::RunTest(const FString& Parameters)
{
	// What a release download's AgentService folder says (agent-service.json), and the installed copies' stamps
	const FString Shipped = TEXT("{\"sha256\": \"ABC123\", \"commit\": \"36a6fec1d2e3\", \"wheel\": \"autopilot-0.1.0-py3-none-any.whl\"}");
	const FString Same = TEXT("{\"sha256\": \"abc123\", \"commit\": \"36a6fec1d2e3\"}");
	const FString Older = TEXT("{\"sha256\": \"def456\", \"commit\": \"6a0bf52\"}");
	const FString Home = TEXT("C:/Users/me");
	const FString Installer = TEXT("C:/Users/me/.local/bin/autopilot.exe");
	const FString OnPathExe = TEXT("C:/py/autopilot.exe");

	const FPlaytestServiceStamp Stamp = PlaytestAgentService::ParseStamp(Shipped);
	TestEqual(TEXT("stamp sha256, lower case"), Stamp.Sha256, FString(TEXT("abc123")));
	TestEqual(TEXT("stamp commit"), Stamp.Commit, FString(TEXT("36a6fec1d2e3")));
	TestTrue(TEXT("not json: nothing"), PlaytestAgentService::ParseStamp(TEXT("{x")).Commit.IsEmpty());

	// Carried: a stamp naming a commit or a wheel, and the installer
	TestTrue(TEXT("carried"), PlaytestAgentService::Carries(Shipped, true));
	TestTrue(TEXT("carried: a commit only"), PlaytestAgentService::Carries(TEXT("{\"commit\": \"36a6fec\"}"), true));
	TestTrue(TEXT("carried: a wheel only"), PlaytestAgentService::Carries(TEXT("{\"sha256\": \"abc\"}"), true));
	TestFalse(TEXT("no installer: not carried"), PlaytestAgentService::Carries(Shipped, false));
	TestFalse(TEXT("no stamp: not carried"), PlaytestAgentService::Carries(FString(), true));
	TestFalse(TEXT("a stamp naming nothing: not carried"), PlaytestAgentService::Carries(TEXT("{\"commit\": \"\", \"sha256\": \"\"}"), true));

	auto Plan = [&](const FPlaytestServiceSearch& Search, const FString& Installed, bool bHasInstaller, bool bTried)
	{
		const FPlaytestServiceCommand Command = PlaytestAgentService::Find(Search);
		return PlaytestAgentService::PlanStart(Command, Shipped, Installed, Search.bCarried, bHasInstaller, bTried, Search.HomeDir, Search.bWindows);
	};
	auto Carried = [&](const TSet<FString>& Files, const FString& Setting = FString(), const FString& PathEnv = FString())
	{
		FPlaytestServiceSearch Search = SearchOver(Files, true, Setting, PathEnv);
		Search.bCarried = true;
		return Search;
	};

	// The setting set: started as it is, never installed over, nothing replaced
	FPlaytestServiceSearch Search = Carried({ TEXT("C:/py/python.exe"), Installer }, TEXT("C:/py/python.exe -m autopilot"));
	TestEqual(TEXT("setting: its command"), PlaytestAgentService::Find(Search).Source, FString(TEXT("setting")));
	TestEqual(TEXT("setting: started"), (int32)Plan(Search, Older, true, false), (int32)EPlaytestServiceStep::Start);
	TestEqual(TEXT("setting naming nothing: missing, not installed"), (int32)Plan(Carried({}, TEXT("D:/no/python.exe")), FString(), true, false),
		(int32)EPlaytestServiceStep::Missing);

	// Carried, nothing installed, autopilot on PATH: PATH isn't looked at, the plugin's own is installed
	Search = Carried({ OnPathExe }, FString(), TEXT("C:/py"));
	const FPlaytestServiceCommand Ignored = PlaytestAgentService::Find(Search);
	TestFalse(TEXT("carried: PATH's autopilot not found"), Ignored.IsFound());
	TestTrue(TEXT("carried: not a problem"), Ignored.Problem.IsEmpty());
	TestEqual(TEXT("carried: PATH's noted"), Ignored.PathSkipped, OnPathExe);
	TestEqual(TEXT("carried, nothing installed, PATH has autopilot: install"), (int32)Plan(Search, FString(), true, false), (int32)EPlaytestServiceStep::Install);
	TestEqual(TEXT("... once a session"), (int32)Plan(Search, FString(), true, true), (int32)EPlaytestServiceStep::Missing);
	// ... nothing installed anywhere: not installed (the first-run setup's Install step does it)
	TestEqual(TEXT("carried, nothing anywhere: not installed"), (int32)Plan(Carried({}, FString(), TEXT("C:/py")), FString(), true, false),
		(int32)EPlaytestServiceStep::Missing);
	TestTrue(TEXT("not carried: nothing skipped"), PlaytestAgentService::Find(SearchOver({ OnPathExe }, true, FString(), TEXT("C:/py"))).PathSkipped.IsEmpty());
	// ... the installer's own on PATH (as uv puts it there) is its place all the same
	Search = Carried({ OnPathExe, Installer }, FString(), TEXT("C:/py;C:/Users/me/.local/bin"));
	TestEqual(TEXT("carried: the installer's place, not PATH's first"), PlaytestAgentService::Find(Search).Exe, Installer);
	TestEqual(TEXT("carried: source installer"), PlaytestAgentService::Find(Search).Source, FString(TEXT("installer")));

	// Carried, installed stamp differs (or none): installed first; the same: started
	Search = Carried({ Installer });
	TestEqual(TEXT("installed older: update"), (int32)Plan(Search, Older, true, false), (int32)EPlaytestServiceStep::Update);
	TestEqual(TEXT("installed without a stamp: update"), (int32)Plan(Search, FString(), true, false), (int32)EPlaytestServiceStep::Update);
	TestEqual(TEXT("installed the same: started"), (int32)Plan(Search, Same, true, false), (int32)EPlaytestServiceStep::Start);
	TestEqual(TEXT("older, tried this session: started"), (int32)Plan(Search, Older, true, true), (int32)EPlaytestServiceStep::Start);
	TestTrue(TEXT("a stamp with a commit only: by commit"), PlaytestAgentService::NeedsUpdate(TEXT("{\"commit\": \"36a6fec\"}"), Older,
		PlaytestAgentService::Find(Search), Home, true));
	TestFalse(TEXT("a stamp with a commit only: the same"), PlaytestAgentService::NeedsUpdate(TEXT("{\"commit\": \"36a6fec\"}"),
		TEXT("{\"commit\": \"36A6FEC\"}"), PlaytestAgentService::Find(Search), Home, true));

	// Not carried: exactly as before (PATH first, never installed over a PATH autopilot; nothing installed: the tab's Install)
	FPlaytestServiceSearch Plain = SearchOver({ OnPathExe, Installer }, true, FString(), TEXT("C:/py"));
	TestEqual(TEXT("not carried: PATH first"), PlaytestAgentService::Find(Plain).Exe, OnPathExe);
	TestEqual(TEXT("not carried: PATH's started"), (int32)Plan(Plain, Older, true, false), (int32)EPlaytestServiceStep::Start);
	TestEqual(TEXT("not carried, nothing installed: missing"), (int32)Plan(SearchOver({}, true), FString(), true, false),
		(int32)EPlaytestServiceStep::Missing);
	TestEqual(TEXT("no installer: nothing installed"), (int32)Plan(Carried({}), FString(), false, false), (int32)EPlaytestServiceStep::Missing);

	// A running one (brain.json): its commit
	FPlaytestAnnouncement Said;
	TestTrue(TEXT("brain.json with a commit"), PlaytestAgentService::ParseAnnouncement(
		TEXT("{\"url\": \"ws://127.0.0.1:50123/v1/bridge\", \"pid\": 900, \"started\": 1, \"commit\": \"36a6fec1d2e3\"}"), Said));
	TestTrue(TEXT("commit said"), Said.bHasCommit);
	TestEqual(TEXT("commit"), Said.Commit, FString(TEXT("36a6fec1d2e3")));
	FPlaytestAnnouncement Checkout;
	PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:1/v1/bridge\", \"pid\": 9, \"commit\": \"\"}"), Checkout);
	TestTrue(TEXT("a checkout's: said, empty"), Checkout.bHasCommit && Checkout.Commit.IsEmpty());
	FPlaytestAnnouncement Before;
	PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:1/v1/bridge\", \"pid\": 9}"), Before);
	TestFalse(TEXT("an older one's: not said"), Before.bHasCommit);
	FPlaytestAnnouncement Nulled;
	PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:1/v1/bridge\", \"pid\": 9, \"commit\": null}"), Nulled);
	TestFalse(TEXT("null: not said"), Nulled.bHasCommit);

	auto Running = [](const TCHAR* Commit)
	{
		FPlaytestAnnouncement A;
		A.Url = TEXT("ws://127.0.0.1:50123/v1/bridge");
		A.Pid = 900;
		A.bHasCommit = Commit != nullptr;
		A.Commit = Commit ? Commit : TEXT("");
		return A;
	};
	using EStep = EPlaytestAnnouncedStep;
	auto Announced = [&](const FPlaytestAnnouncement& A, bool bCarried, bool bSetting = false, bool bInUse = false, bool bTried = false)
	{
		return (int32)PlaytestAgentService::PlanAnnounced(A, Shipped, bCarried, bSetting, bInUse, bTried);
	};
	TestTrue(TEXT("same build"), PlaytestAgentService::IsSameBuild(Running(TEXT("36a6fec1d2e3")), Shipped));
	TestTrue(TEXT("same build, written short"), PlaytestAgentService::IsSameBuild(Running(TEXT("36A6FEC")), Shipped));
	TestFalse(TEXT("too short to tell"), PlaytestAgentService::IsSameBuild(Running(TEXT("36a6")), Shipped));
	TestEqual(TEXT("commit equal: reused"), Announced(Running(TEXT("36a6fec1d2e3")), true), (int32)EStep::Use);
	TestEqual(TEXT("commit different: replaced"), Announced(Running(TEXT("6a0bf52")), true), (int32)EStep::Replace);
	TestEqual(TEXT("commit missing: replaced"), Announced(Running(nullptr), true), (int32)EStep::Replace);
	TestEqual(TEXT("a checkout's (\"\"): replaced"), Announced(Running(TEXT("")), true), (int32)EStep::Replace);
	TestEqual(TEXT("replaced once already: used"), Announced(Running(TEXT("6a0bf52")), true, false, false, true), (int32)EStep::UseOlder);
	TestEqual(TEXT("the setting set: used"), Announced(Running(TEXT("6a0bf52")), true, true), (int32)EStep::Use);
	TestEqual(TEXT("connected to it, or started it: used"), Announced(Running(nullptr), true, false, true), (int32)EStep::Use);
	TestEqual(TEXT("not carried: used"), Announced(Running(nullptr), false), (int32)EStep::Use);
	TestEqual(TEXT("a stamp naming no commit: used"), (int32)PlaytestAgentService::PlanAnnounced(Running(nullptr), TEXT("{\"sha256\": \"abc\"}"),
		true, false, false, false), (int32)EStep::Use);

	// Only an agent service's program is stopped
	TestTrue(TEXT("autopilot.exe"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:\\Users\\me\\.local\\bin\\autopilot.exe")));
	TestTrue(TEXT("uv's python"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:\\Users\\me\\AppData\\Roaming\\uv\\tools\\autopilot\\Scripts\\python.exe")));
	TestTrue(TEXT("pythonw, any case"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:/x/PythonW.EXE")));
	TestTrue(TEXT("python3 elsewhere"), PlaytestAgentService::IsAgentServiceProgram(TEXT("/home/me/.local/share/uv/tools/autopilot/bin/python3")));
	TestTrue(TEXT("python3.12"), PlaytestAgentService::IsAgentServiceProgram(TEXT("/usr/local/bin/python3.12")));
	TestTrue(TEXT("python3.13.exe"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:/x/python3.13.exe")));
	TestFalse(TEXT("python3.x-like"), PlaytestAgentService::IsAgentServiceProgram(TEXT("/usr/bin/python3.12-config")));
	TestFalse(TEXT("python3. alone"), PlaytestAgentService::IsAgentServiceProgram(TEXT("/usr/bin/python3.")));
	TestTrue(TEXT("autopilot elsewhere"), PlaytestAgentService::IsAgentServiceProgram(TEXT("/home/me/.local/bin/autopilot")));
	TestFalse(TEXT("an editor"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe")));
	TestFalse(TEXT("a shell"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:\\Windows\\System32\\cmd.exe")));
	TestFalse(TEXT("powershell"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe")));
	TestFalse(TEXT("a look-alike"), PlaytestAgentService::IsAgentServiceProgram(TEXT("C:/x/python-evil.exe")));
	TestFalse(TEXT("unknown (none)"), PlaytestAgentService::IsAgentServiceProgram(FString()));

	// ... and only the process that wrote brain.json: created before it said it started (+5 s)
	FPlaytestAnnouncement Owner;
	TestTrue(TEXT("brain.json with started"), PlaytestAgentService::ParseAnnouncement(
		TEXT("{\"url\": \"ws://127.0.0.1:50123/v1/bridge\", \"pid\": 900, \"started\": 1790000000.5}"), Owner));
	TestTrue(TEXT("started said"), Owner.bHasStarted);
	FPlaytestAnnouncement NoStarted;
	PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:50123/v1/bridge\", \"pid\": 900}"), NoStarted);
	TestFalse(TEXT("started not said"), NoStarted.bHasStarted);
	FPlaytestAnnouncement Zero;
	PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:50123/v1/bridge\", \"pid\": 900, \"started\": 0}"), Zero);
	TestFalse(TEXT("started 0: not said"), Zero.bHasStarted);
	FPlaytestAnnouncement Garbled;
	PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:50123/v1/bridge\", \"pid\": 900, \"started\": \"soon\"}"), Garbled);
	TestFalse(TEXT("started unreadable: not said"), Garbled.bHasStarted);
	const FString Py = TEXT("C:/x/python.exe");
	FString Why;
	TestTrue(TEXT("created before it announced: may stop"), PlaytestAgentService::MayStop(Py, Owner.Started - 2.0, Owner, Why));
	TestTrue(TEXT("... no reason against"), Why.IsEmpty());
	TestTrue(TEXT("created within the slack: may stop"), PlaytestAgentService::MayStop(Py, Owner.Started + 4.9, Owner, Why));
	TestFalse(TEXT("created after (+ more than the slack): never"), PlaytestAgentService::MayStop(Py, Owner.Started + 5.5, Owner, Why));
	TestTrue(TEXT("... says another process has the id"), Why.Contains(TEXT("another process has that id")));
	TestFalse(TEXT("created long after: never"), PlaytestAgentService::MayStop(Py, Owner.Started + 3600.0, Owner, Why));
	TestFalse(TEXT("started missing: never"), PlaytestAgentService::MayStop(Py, Owner.Started - 2.0, NoStarted, Why));
	TestTrue(TEXT("... says why"), Why.Contains(TEXT("doesn't say when it started")));
	TestFalse(TEXT("started unreadable: never"), PlaytestAgentService::MayStop(Py, Owner.Started - 2.0, Garbled, Why));
	TestFalse(TEXT("start time unreadable: never"), PlaytestAgentService::MayStop(Py, 0.0, Owner, Why));
	TestTrue(TEXT("... says why"), Why.Contains(TEXT("can't be read")));
	TestFalse(TEXT("not an agent service's program: never"), PlaytestAgentService::MayStop(TEXT("C:/x/cmd.exe"), Owner.Started - 2.0, Owner, Why));

	// Process start times: this process's, read from the system; none for no process
	const double Now = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalSeconds();
	const double Mine = PlaytestAgentService::ProcessStartTime(FPlatformProcess::GetCurrentProcessId());
	TestTrue(TEXT("this process: started before now"), Mine > 1.0e9 && Mine <= Now + 1.0);
	TestEqual(TEXT("no process"), PlaytestAgentService::ProcessStartTime(0), 0.0);
	TestEqual(TEXT("etime mm:ss"), PlaytestAgentService::ParseElapsed(TEXT(" 05:07\n")), 307.0);
	TestEqual(TEXT("etime hh:mm:ss"), PlaytestAgentService::ParseElapsed(TEXT("01:00:01")), 3601.0);
	TestEqual(TEXT("etime dd-hh:mm:ss"), PlaytestAgentService::ParseElapsed(TEXT("2-00:00:10")), 172810.0);
	TestEqual(TEXT("etime nothing"), PlaytestAgentService::ParseElapsed(TEXT("")), -1.0);
	TestEqual(TEXT("etime not one"), PlaytestAgentService::ParseElapsed(TEXT("ps: no such process")), -1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceCommandLineTest, "AutopilotPlaytest.AgentService.CommandLines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceCommandLineTest::RunTest(const FString& Parameters)
{
	FPlaytestServiceCommand Autopilot;
	Autopilot.Exe = TEXT("C:/Users/me/.local/bin/autopilot.exe");
	TestEqual(TEXT("serve args"), PlaytestAgentService::ServeArgs(Autopilot, 4242), FString(TEXT("serve --port 0 --exit-with 4242")));
	FPlaytestServiceCommand Python;
	Python.Exe = TEXT("C:/autopilot/.venv/Scripts/python.exe");
	Python.Args = TEXT("-m autopilot");
	TestEqual(TEXT("serve args after the setting's"), PlaytestAgentService::ServeArgs(Python, 7), FString(TEXT("-m autopilot serve --port 0 --exit-with 7")));

	FString Exe, Params;
	PlaytestAgentService::StartCommandLine(Autopilot, 4242, TEXT("C:/Users/me/AppData/Local/Autopilot/logs/agent-service-20261013-101500.log"), true,
		TEXT("C:\\Windows\\System32\\cmd.exe"), Exe, Params);
	TestEqual(TEXT("start: the shell"), Exe, FString(TEXT("C:\\Windows\\System32\\cmd.exe")));
	TestEqual(TEXT("start: windows"), Params, FString(TEXT(
		"/d /s /c \"\"C:\\Users\\me\\.local\\bin\\autopilot.exe\" serve --port 0 --exit-with 4242 >> \"C:\\Users\\me\\AppData\\Local\\Autopilot\\logs\\agent-service-20261013-101500.log\" 2>&1\"")));
	PlaytestAgentService::StartCommandLine(Python, 9, TEXT("/home/me/.config/Autopilot/logs/a.log"), false, TEXT("/bin/sh"), Exe, Params);
	TestEqual(TEXT("start: elsewhere"), Params, FString(TEXT(
		"-c \"exec 'C:/autopilot/.venv/Scripts/python.exe' -m autopilot serve --port 0 --exit-with 9 >> '/home/me/.config/Autopilot/logs/a.log' 2>&1\"")));

	TestEqual(TEXT("installer: windows"), PlaytestAgentService::InstallerScript(TEXT("D:/P/Plugins/Autopilot/AgentService"), true),
		FString(TEXT("D:/P/Plugins/Autopilot/AgentService/install.ps1")));
	TestEqual(TEXT("installer: elsewhere"), PlaytestAgentService::InstallerScript(TEXT("/p/AgentService"), false), FString(TEXT("/p/AgentService/install.sh")));
	TestTrue(TEXT("no folder: no installer"), PlaytestAgentService::InstallerScript(FString(), true).IsEmpty());
	PlaytestAgentService::InstallCommandLine(TEXT("D:/My Game/Plugins/Autopilot/AgentService"), TEXT("C:/L/install-1.log"), true, TEXT("cmd.exe"),
		TEXT("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"), Exe, Params);
	TestEqual(TEXT("install: the shell"), Exe, FString(TEXT("cmd.exe")));
	TestEqual(TEXT("install: windows"), Params, FString(TEXT(
		"/d /s /c \"\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"D:\\My Game\\Plugins\\Autopilot\\AgentService\\install.ps1\" >> \"C:\\L\\install-1.log\" 2>&1\"")));
	PlaytestAgentService::InstallCommandLine(TEXT("/p/it's/AgentService"), TEXT("/l/i.log"), false, TEXT("/bin/sh"), FString(), Exe, Params);
	TestEqual(TEXT("install: elsewhere"), Params, FString(TEXT("-c \"sh '/p/it'\\''s/AgentService/install.sh' >> '/l/i.log' 2>&1\"")));
	return true;
}

// Removing Autopilot from this computer: `autopilot uninstall`'s command lines, and what the plugin removes itself when
// the agent service isn't installed (never memory/: shared run data is never removed; never the Aura app's file)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceUninstallTest, "AutopilotPlaytest.AgentService.Uninstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceUninstallTest::RunTest(const FString& Parameters)
{
	FPlaytestServiceCommand Autopilot;
	Autopilot.Exe = TEXT("C:/Users/me/.local/bin/autopilot.exe");
	FPlaytestServiceCommand Python;
	Python.Exe = TEXT("/home/me/autopilot/.venv/bin/python");
	Python.Args = TEXT("-m autopilot");
	TestEqual(TEXT("uninstall args"), PlaytestAgentService::UninstallArgs(Autopilot, false, TEXT("C:/T/autopilot-uninstall-1.log"), true),
		FString(TEXT("uninstall --yes --log \"C:\\T\\autopilot-uninstall-1.log\"")));
	TestEqual(TEXT("uninstall args, keeping the memory, after the setting's"), PlaytestAgentService::UninstallArgs(Python, true, TEXT("/tmp/u.log"), false),
		FString(TEXT("-m autopilot uninstall --yes --keep-memory --log '/tmp/u.log'")));
	FString Exe, Params;
	PlaytestAgentService::UninstallCommandLine(Autopilot, true, TEXT("C:/T/autopilot-uninstall-1.log"), true, TEXT("C:\\Windows\\System32\\cmd.exe"), Exe, Params);
	TestEqual(TEXT("uninstall: the shell"), Exe, FString(TEXT("C:\\Windows\\System32\\cmd.exe")));
	TestEqual(TEXT("uninstall: windows"), Params, FString(TEXT(
		"/d /s /c \"set PYTHONUNBUFFERED=1& \"C:\\Users\\me\\.local\\bin\\autopilot.exe\" uninstall --yes --keep-memory --log \"C:\\T\\autopilot-uninstall-1.log\" >> \"C:\\T\\autopilot-uninstall-1.log\" 2>&1\"")));
	PlaytestAgentService::UninstallCommandLine(Python, false, TEXT("/tmp/u.log"), false, TEXT("/bin/sh"), Exe, Params);
	TestEqual(TEXT("uninstall: elsewhere"), Params, FString(TEXT(
		"-c \"PYTHONUNBUFFERED=1 exec '/home/me/autopilot/.venv/bin/python' -m autopilot uninstall --yes --log '/tmp/u.log' >> '/tmp/u.log' 2>&1\"")));

	// The settings folder as an installed Autopilot leaves it
	FServiceScratch Dir;
	const FString Settings = FPaths::Combine(Dir.Path, TEXT("Autopilot"));
	auto Fill = [&Settings]()
	{
		for (const TCHAR* Name : { TEXT("preferences.json"), TEXT("autopilot.env"), TEXT("aura-session.json"), TEXT("install.json"),
			TEXT("aura-brain.json"), TEXT("logs/agent-service-1.log"), TEXT("memory/Orchard/notes.md") })
		{
			FFileHelper::SaveStringToFile(TEXT("x"), *FPaths::Combine(Settings, Name));
		}
	};
	auto Left = [](const FString& Folder)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*")), true, true);
		Names.Sort();
		return FString::Join(Names, TEXT(","));
	};
	Fill();
	FString Log;
	TestEqual(TEXT("not asked to keep the memory: all of it removed (0)"), PlaytestAgentService::RemoveSettingsFolder(Settings, false, Log), 0);
	TestEqual(TEXT("what is left: the Aura app's file"), Left(Settings), FString(TEXT("aura-brain.json")));
	TestTrue(TEXT("the log says what went"), Log.Contains(TEXT("Removed ")) && Log.Contains(TEXT("autopilot.env")) && Log.Contains(TEXT("memory")));
	Fill();
	Log.Empty();
	TestEqual(TEXT("asked to keep the memory: 0"), PlaytestAgentService::RemoveSettingsFolder(Settings, true, Log), 0);
	TestEqual(TEXT("kept: the Aura app's file and the memory"), Left(Settings), FString(TEXT("aura-brain.json,memory")));
	TestTrue(TEXT("the memory whole"), FPaths::FileExists(FPaths::Combine(Settings, TEXT("memory/Orchard/notes.md"))));
	TestTrue(TEXT("the log says the memory is kept"), Log.Contains(TEXT("Kept what Autopilot learned about your games in")));
	IFileManager::Get().DeleteDirectory(*FPaths::Combine(Settings, TEXT("memory")), false, true);
	IFileManager::Get().Delete(*FPaths::Combine(Settings, TEXT("aura-brain.json")), false, true, true);
	FFileHelper::SaveStringToFile(TEXT("x"), *FPaths::Combine(Settings, TEXT("preferences.json")));
	Log.Empty();
	TestEqual(TEXT("no memory: 0"), PlaytestAgentService::RemoveSettingsFolder(Settings, false, Log), 0);
	TestFalse(TEXT("nothing left: the folder gone"), IFileManager::Get().DirectoryExists(*Settings));

	// The service, none installed: the plugin removes the settings folder itself; nothing starts or installs until
	// LeaveRemoved
	Fill();
	const FString Home = Dir.Path;
	FPlaytestAgentService::FOptions Options;
	Options.SettingsDir = Settings;
	Options.Search = [Home]()
	{
		FPlaytestServiceSearch Search;
		Search.HomeDir = Home;  // (no .local/bin there: nothing found)
		return Search;
	};
	{
		FPlaytestAgentService Service(Options);
		TestTrue(TEXT("none installed: missing"), Service.IsMissing());
		TestFalse(TEXT("not removed yet"), Service.IsRemoved());
		Service.Uninstall(true);
		TestTrue(TEXT("removed"), Service.IsRemoved());
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (Service.GetUninstallState() == EPlaytestUninstallState::Removing && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Sleep(0.05f);
		}
		TestEqual(TEXT("keeping the memory: removed"), (int32)Service.GetUninstallState(), (int32)EPlaytestUninstallState::Removed);
		TestTrue(TEXT("keeps the choice"), Service.GetUninstallKeepsMemory());
		TestEqual(TEXT("what is left"), Left(Settings), FString(TEXT("aura-brain.json,memory")));
		FString Said;
		FFileHelper::LoadFileToString(Said, *Service.GetUninstallLog());
		TestTrue(TEXT("its log, in the temporary folder"), Service.GetUninstallLog().Contains(TEXT("autopilot-uninstall-"))
			&& Said.Contains(TEXT("Autopilot is removed from this computer.")));
		FString Url;
		TestEqual(TEXT("removed: nothing is started"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		TestEqual(TEXT("removed: nothing tried"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Idle);
		TestFalse(TEXT("removed: nothing is installed"), Service.Install());
		IFileManager::Get().Delete(*Service.GetUninstallLog(), false, true, true);
		Service.LeaveRemoved();
		TestFalse(TEXT("left"), Service.IsRemoved());
		TestEqual(TEXT("left: nothing asked"), (int32)Service.GetUninstallState(), (int32)EPlaytestUninstallState::None);
		TestEqual(TEXT("left: looked for again"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		TestEqual(TEXT("left: not installed"), (int32)Service.GetState(), (int32)EPlaytestServiceState::NotInstalled);
		Service.Shutdown();
	}
	// A setting naming nothing isn't missing: it says so where it is set
	Options.Search = []()
	{
		FPlaytestServiceSearch Search;
		Search.Setting = TEXT("C:/nowhere/autopilot.exe");
		Search.FileExists = [](const FString&) { return false; };
		return Search;
	};
	{
		FPlaytestAgentService Service(Options);
		TestFalse(TEXT("a setting naming nothing: not missing"), Service.IsMissing());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceAnnouncementTest, "AutopilotPlaytest.AgentService.Announcement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceAnnouncementTest::RunTest(const FString& Parameters)
{
	FPlaytestAnnouncement Said;
	TestTrue(TEXT("brain.json"), PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:50123/v1/bridge\", \"pid\": 900, \"started\": 1790000000.5}"), Said));
	TestEqual(TEXT("url"), Said.Url, FString(TEXT("ws://127.0.0.1:50123/v1/bridge")));
	TestEqual(TEXT("pid"), (int64)Said.Pid, (int64)900);
	TestEqual(TEXT("started"), Said.Started, 1790000000.5);
	TestFalse(TEXT("another machine"), PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://10.0.0.2:8765/v1/bridge\", \"pid\": 900}"), Said));
	TestFalse(TEXT("no pid"), PlaytestAgentService::ParseAnnouncement(TEXT("{\"url\": \"ws://127.0.0.1:8765/v1/bridge\"}"), Said));
	TestFalse(TEXT("not json"), PlaytestAgentService::ParseAnnouncement(TEXT("{url"), Said));

	// The process this editor started (cmd.exe 100) ran uv's autopilot.exe (200), which ran Python (300): Python announces
	const TMap<uint32, uint32> Parents = { { 300, 200 }, { 200, 100 }, { 100, 50 }, { 400, 50 } };
	const TFunction<uint32(uint32)> ParentOf = [Parents](uint32 Pid) { const uint32* P = Parents.Find(Pid); return P ? *P : 0u; };
	FPlaytestAnnouncement Python;
	Python.Pid = 300;
	TestTrue(TEXT("started by ours"), PlaytestAgentService::IsFrom(Python, 100, ParentOf));
	TestTrue(TEXT("ours itself"), PlaytestAgentService::IsFrom(Python, 300, nullptr));
	TestFalse(TEXT("no parents known: only itself"), PlaytestAgentService::IsFrom(Python, 100, nullptr));
	FPlaytestAnnouncement Other;
	Other.Pid = 400;  // (another editor's, a sibling of ours)
	TestFalse(TEXT("another's"), PlaytestAgentService::IsFrom(Other, 100, ParentOf));
	TestFalse(TEXT("no pid of ours"), PlaytestAgentService::IsFrom(Python, 0, ParentOf));
	// This process's own parent chain (Windows: read from the system)
	FPlaytestAnnouncement Self;
	Self.Pid = FPlatformProcess::GetCurrentProcessId();
	TestTrue(TEXT("this process"), PlaytestAgentService::IsFrom(Self, Self.Pid, PlaytestAgentService::ProcessParents()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceLogsTest, "AutopilotPlaytest.AgentService.Logs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceLogsTest::RunTest(const FString& Parameters)
{
	FServiceScratch Dir;
	const FDateTime When(2026, 10, 13, 9, 5, 7);
	const FString First = PlaytestAgentService::NewLogPath(Dir.Path, TEXT("agent-service-"), When);
	TestEqual(TEXT("named by its time"), FPaths::GetCleanFilename(First), FString(TEXT("agent-service-20261013-090507.log")));
	FFileHelper::SaveStringToFile(TEXT("x"), *First);
	TestEqual(TEXT("taken: -2"), FPaths::GetCleanFilename(PlaytestAgentService::NewLogPath(Dir.Path, TEXT("agent-service-"), When)),
		FString(TEXT("agent-service-20261013-090507-2.log")));
	for (int32 Day = 1; Day <= 7; ++Day)
	{
		const FString Path = PlaytestAgentService::NewLogPath(Dir.Path, TEXT("agent-service-"), FDateTime(2026, 10, Day, 12, 0, 0));
		FFileHelper::SaveStringToFile(TEXT("x"), *Path);
		IFileManager::Get().SetTimeStamp(*Path, FDateTime(2026, 10, Day, 12, 0, 0));
	}
	IFileManager::Get().SetTimeStamp(*First, When);
	FFileHelper::SaveStringToFile(TEXT("x"), *FPaths::Combine(Dir.Path, TEXT("install-20261001-120000.log")));
	PlaytestAgentService::PruneLogs(Dir.Path, TEXT("agent-service-"), 5);
	TArray<FString> Left;
	IFileManager::Get().FindFiles(Left, *FPaths::Combine(Dir.Path, TEXT("agent-service-*.log")), true, false);
	Left.Sort();
	TestEqual(TEXT("five kept"), Left.Num(), 5);
	if (Left.Num() == 5)
	{
		TestEqual(TEXT("oldest kept"), Left[0], FString(TEXT("agent-service-20261004-120000.log")));
		TestEqual(TEXT("newest kept"), Left[4], FString(TEXT("agent-service-20261013-090507.log")));
	}
	TestTrue(TEXT("other logs left alone"), FPaths::FileExists(FPaths::Combine(Dir.Path, TEXT("install-20261001-120000.log"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAccountFramesTest, "AutopilotPlaytest.Account.Frames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAccountFramesTest::RunTest(const FString& Parameters)
{
	FPlaytestAccount Account;
	TestFalse(TEXT("not an account frame"), PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"usage\"}")), Account));
	TestFalse(TEXT("nothing read"), Account.bKnown);
	TestTrue(TEXT("aura"), PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"aura\", \"email\": \"dev@studio.test\", \"signed_in\": true, \"keys\": {\"decision\": null, \"llm\": \"\\u2022\\u2022\\u2022\\u20223c2e\"}}")), Account));
	TestTrue(TEXT("known"), Account.bKnown);
	TestEqual(TEXT("mode"), Account.Mode, FString(TEXT("aura")));
	TestEqual(TEXT("email"), Account.Email, FString(TEXT("dev@studio.test")));
	TestTrue(TEXT("signed in"), Account.bSignedIn);
	TestTrue(TEXT("no decision key"), Account.DecisionKey.IsEmpty());
	TestEqual(TEXT("llm key ending"), Account.LlmKey, FString(TEXT("\u2022\u2022\u2022\u20223c2e")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": false}")), Account);
	TestFalse(TEXT("aura, session ended"), Account.bSignedIn);
	TestTrue(TEXT("no email"), Account.Email.IsEmpty());
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"something new\"}")), Account);
	TestEqual(TEXT("unknown mode: none"), Account.Mode, FString(TEXT("none")));

	// Whose LLM key it is: llm_provider (none: an older agent service, Anthropic's); one the setup doesn't offer is kept as sent
	TestEqual(TEXT("no llm_provider: anthropic"), Account.LlmProvider, FString(TEXT("anthropic")));
	TestFalse(TEXT("no llm_provider: not sent"), Account.bLlmProviderSent);
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"decision\": null, \"llm\": \"••••9d1b\"}, \"llm_provider\": \"OpenAI\"}")), Account);
	TestEqual(TEXT("llm_provider openai"), Account.LlmProvider, FString(TEXT("openai")));
	TestTrue(TEXT("llm_provider sent"), Account.bLlmProviderSent);
	TestEqual(TEXT("openai's key"), Account.LlmKey, FString(TEXT("••••9d1b")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"llm_provider\": \"anthropic\"}")), Account);
	TestEqual(TEXT("llm_provider anthropic"), Account.LlmProvider, FString(TEXT("anthropic")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"llm_provider\": \"mistral\"}")), Account);
	TestEqual(TEXT("unknown llm_provider: as sent"), Account.LlmProvider, FString(TEXT("mistral")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"llm_provider\": null}")), Account);
	TestEqual(TEXT("null llm_provider: anthropic"), Account.LlmProvider, FString(TEXT("anthropic")));
	TestFalse(TEXT("null llm_provider: not sent"), Account.bLlmProviderSent);

	// The Coding Agent: coding_agent (none, or one the setup doesn't offer: OpenCode)
	TestEqual(TEXT("no coding_agent: opencode"), Account.CodingAgent, FString(TEXT("opencode")));
	TestFalse(TEXT("no coding_agent: not sent"), Account.bCodingAgentSent);
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"Claude\", \"llm_provider\": \"anthropic\"}")), Account);
	TestEqual(TEXT("coding_agent claude"), Account.CodingAgent, FString(TEXT("claude")));
	TestTrue(TEXT("coding_agent sent"), Account.bCodingAgentSent);
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"codex\", \"llm_provider\": \"openai\"}")), Account);
	TestEqual(TEXT("coding_agent codex"), Account.CodingAgent, FString(TEXT("codex")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"aider\"}")), Account);
	TestEqual(TEXT("unknown coding_agent: opencode"), Account.CodingAgent, FString(TEXT("opencode")));
	TestTrue(TEXT("unknown coding_agent: sent"), Account.bCodingAgentSent);
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"coding_agent\": \"opencode\", \"llm_provider\": \"OpenRouter\"}")), Account);
	TestEqual(TEXT("coding_agent opencode"), Account.CodingAgent, FString(TEXT("opencode")));
	TestEqual(TEXT("llm_provider openrouter"), Account.LlmProvider, FString(TEXT("openrouter")));

	// Out of credits: Aura's words and where to add some (null, none from an older agent service, or no message: none;
	// a url that isn't a web page: not opened)
	TestTrue(TEXT("no out_of_credits: none"), Account.OutOfCredits.IsEmpty() && Account.AddCreditsUrl.IsEmpty());
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"out_of_credits\": {\"message\": \"Your team is out of Aura credits.\", \"url\": \"https://aura.test/account/pricing\"}}")), Account);
	TestEqual(TEXT("out_of_credits: Aura's words"), Account.OutOfCredits, FString(TEXT("Your team is out of Aura credits.")));
	TestEqual(TEXT("out_of_credits: where"), Account.AddCreditsUrl, FString(TEXT("https://aura.test/account/pricing")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"out_of_credits\": {\"message\": \"Out.\", \"url\": null}}")), Account);
	TestTrue(TEXT("out_of_credits, nowhere named"), Account.OutOfCredits == TEXT("Out.") && Account.AddCreditsUrl.IsEmpty());
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"out_of_credits\": {\"message\": \"Out.\", \"url\": \"file:///C:/x.exe\"}}")), Account);
	TestTrue(TEXT("out_of_credits, not a web page"), Account.OutOfCredits == TEXT("Out.") && Account.AddCreditsUrl.IsEmpty());
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"out_of_credits\": null}")), Account);
	TestTrue(TEXT("null out_of_credits: none"), Account.OutOfCredits.IsEmpty() && Account.AddCreditsUrl.IsEmpty());
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"out_of_credits\": {\"url\": \"https://aura.test/account/pricing\"}}")), Account);
	TestTrue(TEXT("out_of_credits without words: none"), Account.OutOfCredits.IsEmpty() && Account.AddCreditsUrl.IsEmpty());

	// How the tab names them
	TestTrue(TEXT("three agents, Claude Code first"), PlaytestAccount::CodingAgents() == TArray<FString>{ TEXT("claude"), TEXT("codex"), TEXT("opencode") });
	TestEqual(TEXT("name claude"), PlaytestAccount::AgentName(TEXT("claude")), FString(TEXT("Claude Code")));
	TestEqual(TEXT("name codex"), PlaytestAccount::AgentName(TEXT("codex")), FString(TEXT("Codex")));
	TestEqual(TEXT("name opencode"), PlaytestAccount::AgentName(TEXT("opencode")), FString(TEXT("OpenCode")));
	TestEqual(TEXT("name of another agent: OpenCode"), PlaytestAccount::AgentName(TEXT("aider")), FString(TEXT("OpenCode")));
	TestEqual(TEXT("claude takes anthropic"), PlaytestAccount::AgentProvider(TEXT("claude")), FString(TEXT("anthropic")));
	TestEqual(TEXT("codex takes openai"), PlaytestAccount::AgentProvider(TEXT("codex")), FString(TEXT("openai")));
	TestTrue(TEXT("opencode takes any"), PlaytestAccount::AgentProvider(TEXT("opencode")).IsEmpty());
	TestTrue(TEXT("six providers, anthropic first"), PlaytestAccount::LlmProviders()
		== TArray<FString>{ TEXT("anthropic"), TEXT("openai"), TEXT("opencode"), TEXT("openrouter"), TEXT("deepseek"), TEXT("google") });
	TestEqual(TEXT("name anthropic"), PlaytestAccount::ProviderName(TEXT("anthropic")), FString(TEXT("Anthropic")));
	TestEqual(TEXT("name openai"), PlaytestAccount::ProviderName(TEXT("openai")), FString(TEXT("OpenAI")));
	TestEqual(TEXT("name opencode"), PlaytestAccount::ProviderName(TEXT("opencode")), FString(TEXT("OpenCode Zen")));
	TestEqual(TEXT("name openrouter"), PlaytestAccount::ProviderName(TEXT("openrouter")), FString(TEXT("OpenRouter")));
	TestEqual(TEXT("name deepseek"), PlaytestAccount::ProviderName(TEXT("deepseek")), FString(TEXT("DeepSeek")));
	TestEqual(TEXT("name google"), PlaytestAccount::ProviderName(TEXT("google")), FString(TEXT("Gemini")));
	TestEqual(TEXT("name of another: as sent"), PlaytestAccount::ProviderName(TEXT("mistral")), FString(TEXT("mistral")));
	TestEqual(TEXT("name of none: Anthropic"), PlaytestAccount::ProviderName(TEXT("")), FString(TEXT("Anthropic")));

	// The setup's dropdowns: the frame's agent, else autopilot.env's, else OpenCode; OpenCode's provider: the frame's (when
	// it is OpenCode's), else autopilot.env's, else Anthropic
	auto PickAgent = [](const TCHAR* Frame, const TCHAR* Saved)
	{
		FPlaytestAccount A;
		if (Frame)
		{
			PlaytestAccount::ParseAccount(PlaytestJson::Parse(Frame), A);
		}
		return PlaytestAccount::PickAgent(A, Saved);
	};
	TestEqual(TEXT("agent: nothing known"), PickAgent(nullptr, TEXT("")), FString(TEXT("opencode")));
	TestEqual(TEXT("agent: autopilot.env"), PickAgent(nullptr, TEXT(" Codex ")), FString(TEXT("codex")));
	TestEqual(TEXT("agent: the frame's over autopilot.env's"), PickAgent(TEXT("{\"type\": \"account\", \"coding_agent\": \"claude\"}"), TEXT("codex")), FString(TEXT("claude")));
	TestEqual(TEXT("agent: an older frame, autopilot.env's"), PickAgent(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\"}"), TEXT("claude")), FString(TEXT("claude")));
	TestEqual(TEXT("agent: an unknown frame agent"), PickAgent(TEXT("{\"type\": \"account\", \"coding_agent\": \"aider\"}"), TEXT("codex")), FString(TEXT("opencode")));
	TestEqual(TEXT("agent: an unknown saved agent"), PickAgent(nullptr, TEXT("aider")), FString(TEXT("opencode")));
	auto Pick = [](const TCHAR* Frame, const TCHAR* Saved)
	{
		FPlaytestAccount A;
		if (Frame)
		{
			PlaytestAccount::ParseAccount(PlaytestJson::Parse(Frame), A);
		}
		return PlaytestAccount::PickProvider(A, Saved);
	};
	TestEqual(TEXT("pick: nothing known"), Pick(nullptr, TEXT("")), FString(TEXT("anthropic")));
	TestEqual(TEXT("pick: autopilot.env"), Pick(nullptr, TEXT(" OpenAI ")), FString(TEXT("openai")));
	TestEqual(TEXT("pick: the frame's over autopilot.env's"), Pick(TEXT("{\"type\": \"account\", \"llm_provider\": \"anthropic\"}"), TEXT("openai")), FString(TEXT("anthropic")));
	TestEqual(TEXT("pick: the frame's"), Pick(TEXT("{\"type\": \"account\", \"llm_provider\": \"openai\"}"), TEXT("")), FString(TEXT("openai")));
	TestEqual(TEXT("pick: an older frame, autopilot.env's"), Pick(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\"}"), TEXT("openai")), FString(TEXT("openai")));
	TestEqual(TEXT("pick: an unknown frame provider"), Pick(TEXT("{\"type\": \"account\", \"llm_provider\": \"mistral\"}"), TEXT("openai")), FString(TEXT("anthropic")));
	TestEqual(TEXT("pick: an unknown saved provider"), Pick(nullptr, TEXT("mistral")), FString(TEXT("anthropic")));
	TestEqual(TEXT("pick: a six-provider frame"), Pick(TEXT("{\"type\": \"account\", \"coding_agent\": \"opencode\", \"llm_provider\": \"google\"}"), TEXT("openai")), FString(TEXT("google")));
	TestEqual(TEXT("pick: autopilot.env's deepseek"), Pick(nullptr, TEXT("deepseek")), FString(TEXT("deepseek")));
	TestEqual(TEXT("pick: Claude Code's frame, OpenCode's from autopilot.env"),
		Pick(TEXT("{\"type\": \"account\", \"coding_agent\": \"claude\", \"llm_provider\": \"anthropic\"}"), TEXT("openrouter")), FString(TEXT("openrouter")));
	TestEqual(TEXT("pick: Codex's frame, nothing saved"), Pick(TEXT("{\"type\": \"account\", \"coding_agent\": \"codex\", \"llm_provider\": \"openai\"}"), TEXT("")), FString(TEXT("anthropic")));

	// The account corner's keys, by agent and provider
	auto Saved = [](const TCHAR* Frame)
	{
		FPlaytestAccount A;
		PlaytestAccount::ParseAccount(PlaytestJson::Parse(Frame), A);
		return PlaytestAccount::SavedKeysLine(A);
	};
	TestEqual(TEXT("keys: anthropic"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"decision\": \"••••a91f\", \"llm\": \"••••3c2e\"}, \"coding_agent\": \"opencode\", \"llm_provider\": \"anthropic\"}")),
		FString(TEXT("Saved on this computer: Decision provider ••••a91f · OpenCode (Anthropic) ••••3c2e")));
	TestEqual(TEXT("keys: openai"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"decision\": \"••••a91f\", \"llm\": \"••••3c2e\"}, \"llm_provider\": \"openai\"}")),
		FString(TEXT("Saved on this computer: Decision provider ••••a91f · OpenCode (OpenAI) ••••3c2e")));
	TestEqual(TEXT("keys: claude code"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"llm\": \"••••3c2e\"}, \"coding_agent\": \"claude\", \"llm_provider\": \"anthropic\"}")),
		FString(TEXT("Saved on this computer: Claude Code (Anthropic) ••••3c2e")));
	TestEqual(TEXT("keys: codex"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"llm\": \"••••3c2e\"}, \"coding_agent\": \"codex\", \"llm_provider\": \"openai\"}")),
		FString(TEXT("Saved on this computer: Codex (OpenAI) ••••3c2e")));
	TestEqual(TEXT("keys: openrouter"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"llm\": \"••••3c2e\"}, \"coding_agent\": \"opencode\", \"llm_provider\": \"openrouter\"}")),
		FString(TEXT("Saved on this computer: OpenCode (OpenRouter) ••••3c2e")));
	TestEqual(TEXT("keys: another provider"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"llm\": \"••••3c2e\"}, \"llm_provider\": \"mistral\"}")),
		FString(TEXT("Saved on this computer: OpenCode (mistral) ••••3c2e")));
	TestEqual(TEXT("keys: an older agent service"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"decision\": null, \"llm\": \"••••3c2e\"}}")),
		FString(TEXT("Saved on this computer: OpenCode (Anthropic) ••••3c2e")));
	TestEqual(TEXT("keys: decision only"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"decision\": \"••••a91f\", \"llm\": null}, \"llm_provider\": \"openai\"}")),
		FString(TEXT("Saved on this computer: Decision provider ••••a91f")));
	TestTrue(TEXT("keys: none"), Saved(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"keys\": {\"decision\": null, \"llm\": null}, \"llm_provider\": \"openai\"}")).IsEmpty());

	FPlaytestSignIn SignIn;
	TestEqual(TEXT("nothing asked: idle"), (int32)PlaytestAccount::Phase(SignIn), (int32)EPlaytestSignInPhase::Idle);
	SignIn.State = TEXT("asked");
	TestEqual(TEXT("asked"), (int32)PlaytestAccount::Phase(SignIn), (int32)EPlaytestSignInPhase::Asked);
	TestTrue(TEXT("waiting"), PlaytestAccount::ParseSignIn(PlaytestJson::Parse(TEXT(
		"{\"type\": \"aura_sign_in\", \"state\": \"waiting\", \"url\": \"https://app.tryaura.dev/auth/cli?code=1\"}")), SignIn));
	TestEqual(TEXT("waiting phase"), (int32)PlaytestAccount::Phase(SignIn), (int32)EPlaytestSignInPhase::Waiting);
	TestEqual(TEXT("waiting url"), SignIn.Url, FString(TEXT("https://app.tryaura.dev/auth/cli?code=1")));
	PlaytestAccount::ParseSignIn(PlaytestJson::Parse(TEXT("{\"type\": \"aura_sign_in\", \"state\": \"waiting\"}")), SignIn);
	TestEqual(TEXT("waiting without a url: still asked"), (int32)PlaytestAccount::Phase(SignIn), (int32)EPlaytestSignInPhase::Asked);
	PlaytestAccount::ParseSignIn(PlaytestJson::Parse(TEXT("{\"type\": \"aura_sign_in\", \"state\": \"signed_in\", \"email\": \"dev@studio.test\"}")), SignIn);
	TestEqual(TEXT("signed in phase"), (int32)PlaytestAccount::Phase(SignIn), (int32)EPlaytestSignInPhase::SignedIn);
	TestEqual(TEXT("signed in email"), SignIn.Email, FString(TEXT("dev@studio.test")));

	// Errors: one plain line each, by reason; a cancel the user made shows nothing
	struct FCase { const TCHAR* Reason; const TCHAR* Message; EPlaytestSignInPhase Phase; const TCHAR* Line; };
	const FCase Cases[] = {
		{ TEXT("denied"), TEXT("x"), EPlaytestSignInPhase::Error, TEXT("Sign-in was cancelled in the browser.") },
		{ TEXT("timeout"), TEXT("x"), EPlaytestSignInPhase::Error, TEXT("Nothing came back from the browser in 5 minutes.") },
		{ TEXT("unreachable"), TEXT("x"), EPlaytestSignInPhase::Error, TEXT("Couldn’t reach Aura. Check your connection and try again.") },
		{ TEXT("refused"), TEXT("Your Aura plan doesn't include Autopilot."), EPlaytestSignInPhase::Error, TEXT("Your Aura plan doesn't include Autopilot.") },
		{ TEXT("cancelled"), TEXT("x"), EPlaytestSignInPhase::Idle, TEXT("x") },
	};
	for (const FCase& Case : Cases)
	{
		TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("type"), TEXT("aura_sign_in"));
		Frame->SetStringField(TEXT("state"), TEXT("error"));
		Frame->SetStringField(TEXT("reason"), Case.Reason);
		Frame->SetStringField(TEXT("message"), Case.Message);
		TestTrue(Case.Reason, PlaytestAccount::ParseSignIn(Frame, SignIn));
		TestEqual(*(FString(Case.Reason) + TEXT(" phase")), (int32)PlaytestAccount::Phase(SignIn), (int32)Case.Phase);
		TestEqual(*(FString(Case.Reason) + TEXT(" line")), PlaytestAccount::ErrorLine(SignIn), FString(Case.Line));
	}
	TestFalse(TEXT("not a sign-in frame"), PlaytestAccount::ParseSignIn(PlaytestJson::Parse(TEXT("{\"type\": \"account\"}")), SignIn));

	// A sign-in going on, an account frame on a new connection: finished meanwhile when it has the session, else
	// asked again (the Aura app running alone is no session)
	auto Away = [](const TCHAR* State, const TCHAR* Frame, FPlaytestSignIn& Out)
	{
		FPlaytestSignIn Going;
		Going.State = State;
		FPlaytestAccount A;
		PlaytestAccount::ParseAccount(PlaytestJson::Parse(Frame), A);
		return PlaytestAccount::SignInFinishedAway(Going, A, Out);
	};
	const TCHAR* Kept = TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"email\": \"dev@studio.test\"}");
	FPlaytestSignIn Finished;
	TestTrue(TEXT("waiting, then a kept session: finished"), Away(TEXT("waiting"), Kept, Finished));
	TestEqual(TEXT("finished: signed in"), (int32)PlaytestAccount::Phase(Finished), (int32)EPlaytestSignInPhase::SignedIn);
	TestEqual(TEXT("finished: its email"), Finished.Email, FString(TEXT("dev@studio.test")));
	TestTrue(TEXT("asked, then a kept session: finished"), Away(TEXT("asked"), Kept, Finished));
	TestFalse(TEXT("not signed in: asked again"), Away(TEXT("waiting"), TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": false}"), Finished));
	TestFalse(TEXT("the Aura app only: asked again"), Away(TEXT("waiting"), TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true}"), Finished));
	TestFalse(TEXT("none going: nothing"), Away(TEXT(""), Kept, Finished));
	TestFalse(TEXT("answered already: nothing"), Away(TEXT("error"), Kept, Finished));
	FPlaytestSignIn Going;
	Going.State = TEXT("waiting");
	TestTrue(TEXT("waiting: going"), PlaytestAccount::IsSignInGoing(Going));
	Going.State = TEXT("signed_in");
	TestFalse(TEXT("signed in: not going"), PlaytestAccount::IsSignInGoing(Going));

	// What the Settings page's account corner shows
	auto Kind = [](const TCHAR* Frame, const TCHAR* Access, bool bApp)
	{
		FPlaytestAccount A;
		if (Frame)
		{
			PlaytestAccount::ParseAccount(PlaytestJson::Parse(Frame), A);
		}
		return (int32)PlaytestAccount::UsageKind(A, Access, bApp);
	};
	const TCHAR* AuraIn = TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"email\": \"dev@studio.test\"}");
	const TCHAR* AuraOut = TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": false}");
	const TCHAR* Keys = TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"signed_in\": false}");
	const TCHAR* NoneSignedIn = TEXT("{\"type\": \"account\", \"mode\": \"none\", \"signed_in\": true, \"email\": \"dev@studio.test\"}");
	TestEqual(TEXT("nothing known, nothing chosen"), Kind(nullptr, TEXT(""), false), (int32)EPlaytestAccountKind::NotSetUp);
	TestEqual(TEXT("nothing known, aura chosen"), Kind(nullptr, TEXT("aura"), false), (int32)EPlaytestAccountKind::Aura);
	TestEqual(TEXT("nothing known, keys chosen"), Kind(nullptr, TEXT("own_keys"), false), (int32)EPlaytestAccountKind::OwnKeys);
	TestEqual(TEXT("aura signed in"), Kind(AuraIn, TEXT("aura"), false), (int32)EPlaytestAccountKind::Aura);
	TestEqual(TEXT("aura signed out"), Kind(AuraOut, TEXT("aura"), false), (int32)EPlaytestAccountKind::SignedOut);
	TestEqual(TEXT("own keys"), Kind(Keys, TEXT(""), false), (int32)EPlaytestAccountKind::OwnKeys);
	TestEqual(TEXT("Use with Aura first"), Kind(Keys, TEXT("own_keys"), true), (int32)EPlaytestAccountKind::AuraApp);
	TestEqual(TEXT("a stale none, nothing chosen yet"), Kind(NoneSignedIn, TEXT(""), false), (int32)EPlaytestAccountKind::NotSetUp);
	TestEqual(TEXT("a stale none after Finish chose aura"), Kind(NoneSignedIn, TEXT("aura"), false), (int32)EPlaytestAccountKind::Aura);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAccountBillTest, "AutopilotPlaytest.Account.Bill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAccountBillTest::RunTest(const FString& Parameters)
{
	// The `bill` frame: what Autopilot was billed for a run through Aura
	FPlaytestRunBill Bill;
	TestFalse(TEXT("not a bill frame"), PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"usage\", \"usd\": 1}")), Bill));
	TestFalse(TEXT("nothing read"), Bill.bKnown);
	TestTrue(TEXT("billed"), PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"usd\": 0.593177, \"billed\": true}")), Bill));
	TestTrue(TEXT("billed: known"), Bill.bKnown);
	TestTrue(TEXT("billed: shows"), Bill.Shows());
	TestEqual(TEXT("billed: usd"), Bill.Usd, 0.593177, 1e-9);
	TestEqual(TEXT("billed: line"), PlaytestAccount::BilledLine(Bill.Usd), FString(TEXT("Billed $0.59")));
	TestTrue(TEXT("later"), PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"usd\": 0.41, \"billed\": false, \"reason\": \"pending\"}")), Bill));
	TestTrue(TEXT("later: known"), Bill.bKnown);
	TestFalse(TEXT("later: shows nothing"), Bill.Shows());
	TestEqual(TEXT("later: reason"), Bill.Reason, FString(TEXT("pending")));
	PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"billed\": true}")), Bill);
	TestFalse(TEXT("no sum: shows nothing"), Bill.Shows());
	PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"usd\": -2, \"billed\": true}")), Bill);
	TestFalse(TEXT("a sum below 0: shows nothing"), Bill.Shows());
	PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"usd\": null, \"billed\": true}")), Bill);
	TestFalse(TEXT("a null sum: shows nothing"), Bill.Shows());

	// Saved with the run and read back; a run saved before bills has none
	PlaytestAccount::ParseBill(PlaytestJson::Parse(TEXT("{\"type\": \"bill\", \"usd\": 12.3456, \"billed\": true}")), Bill);
	const FPlaytestRunBill Back = PlaytestAccount::BillFromJson(PlaytestJson::Parse(PlaytestJson::Write(PlaytestAccount::BillToJson(Bill))));
	TestTrue(TEXT("saved: shows"), Back.Shows());
	TestEqual(TEXT("saved: usd"), Back.Usd, 12.3456, 1e-9);
	TestFalse(TEXT("none saved"), PlaytestAccount::BillFromJson(nullptr).bKnown);

	// Money: 2 decimals, "< $0.01" below a cent
	struct FMoney { double Usd; const TCHAR* Shown; };
	const FMoney Sums[] = {
		{ 0.593177, TEXT("$0.59") }, { 0.0, TEXT("$0.00") }, { 0.004, TEXT("< $0.01") }, { 0.0099, TEXT("< $0.01") },
		{ 0.01, TEXT("$0.01") }, { 0.005, TEXT("< $0.01") }, { 0.995, TEXT("$1.00") }, { 12.34, TEXT("$12.34") }, { 7.5, TEXT("$7.50") },
		{ 999.999, TEXT("$1,000.00") }, { 1234567.891, TEXT("$1,234,567.89") }, { -3.0, TEXT("$0.00") },
	};
	for (const FMoney& Sum : Sums)
	{
		TestEqual(*FString::Printf(TEXT("money %f"), Sum.Usd), PlaytestAccount::Money(Sum.Usd), FString(Sum.Shown));
	}

	// The account frame's bill: the period (null: nothing)
	FPlaytestAccount Account;
	TestTrue(TEXT("account with a bill"), PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT(
		"{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"email\": \"dev@studio.test\","
		" \"bill\": {\"usd\": 12.34, \"runs\": 38, \"from\": \"2026-10-01\", \"to\": \"2026-10-31\"}}")), Account));
	TestTrue(TEXT("period known"), Account.Bill.bKnown);
	TestEqual(TEXT("period runs"), Account.Bill.Runs, 38);
	TestEqual(TEXT("period line"), PlaytestAccount::PeriodLine(Account.Bill), FString(TEXT("Billing period (Oct 1 – Oct 31): $12.34 USD")));
	TestEqual(TEXT("period dates"), PlaytestAccount::PeriodDates(Account.Bill), FString(TEXT("Oct 1 – Oct 31")));
	FPlaytestPeriodBill One;
	One.bKnown = true;
	One.Usd = 0.002;
	One.Runs = 1;
	One.From = TEXT("2026-12-15");
	One.To = TEXT("2027-01-14");
	TestEqual(TEXT("one run, below a cent"), PlaytestAccount::PeriodLine(One), FString(TEXT("Billing period (Dec 15, 2026 – Jan 14, 2027): < $0.01 USD")));
	TestEqual(TEXT("across years"), PlaytestAccount::PeriodDates(One), FString(TEXT("Dec 15, 2026 – Jan 14, 2027")));
	One.From = TEXT("this month");
	One.To = FString();
	TestEqual(TEXT("not dates: as sent"), PlaytestAccount::PeriodDates(One), FString(TEXT("this month")));
	One.From.Empty();
	TestEqual(TEXT("no days: the bill alone"), PlaytestAccount::PeriodLine(One), FString(TEXT("Billing period: < $0.01 USD")));
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"aura\", \"signed_in\": true, \"bill\": null}")), Account);
	TestFalse(TEXT("bill null: nothing"), Account.Bill.bKnown);
	TestTrue(TEXT("bill null: no line"), PlaytestAccount::PeriodLine(Account.Bill).IsEmpty());
	TestTrue(TEXT("bill null: no dates"), PlaytestAccount::PeriodDates(Account.Bill).IsEmpty());
	PlaytestAccount::ParseAccount(PlaytestJson::Parse(TEXT("{\"type\": \"account\", \"mode\": \"own_keys\", \"signed_in\": false}")), Account);
	TestFalse(TEXT("BYOK: no bill"), Account.Bill.bKnown);
	return true;
}


// The account frames over a real connection, against a stand-in agent service (tools: a websockets server answering
// account and aura_sign_in as autopilot/account.py does). Runs only when AUTOPILOT_STANDIN_URL names it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAccountStandInTest, "AutopilotPlaytest.Account.StandIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAccountStandInTest::RunTest(const FString& Parameters)
{
	const FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("AUTOPILOT_STANDIN_URL"));
	UPlaytestSubsystem* Bridge = UPlaytestSubsystem::Get();
	if (Url.IsEmpty() || !Bridge)
	{
		AddInfo(TEXT("AUTOPILOT_STANDIN_URL isn't set: nothing to connect to"));
		return true;
	}
	TWeakObjectPtr<UPlaytestSubsystem> Weak(Bridge);
	auto WaitUntil = [this](const FString& What, TFunction<bool()> Done, double Seconds)
	{
		TSharedRef<double> Deadline = MakeShared<double>(0.0);
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, What, Done, Deadline, Seconds]()
		{
			if (*Deadline == 0.0)
			{
				*Deadline = FPlatformTime::Seconds() + Seconds;
			}
			if (Done())
			{
				return true;
			}
			if (FPlatformTime::Seconds() > *Deadline)
			{
				AddError(FString::Printf(TEXT("Timed out waiting for: %s"), *What));
				return true;
			}
			return false;
		}));
	};
	auto Then = [](TFunction<void()> Do)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Do]() { Do(); return true; }));
	};
	auto B = [Weak]() { return Weak.Get(); };
	auto Phase = [B]() { return B() ? PlaytestAccount::Phase(B()->GetSignIn()) : EPlaytestSignInPhase::Idle; };

	Bridge->Connect(Url);
	WaitUntil(TEXT("a welcome offering the account"), [B]() { return B() && B()->HasAccountFeature(); }, 15.0);
	WaitUntil(TEXT("the account frame after the welcome"), [B]() { return B() && B()->GetAccount().bKnown; }, 5.0);
	Then([this, B]()
	{
		TestEqual(TEXT("not set up"), B()->GetAccount().Mode, FString(TEXT("none")));
		TestEqual(TEXT("llm key ending"), B()->GetAccount().LlmKey, FString(TEXT("••••3c2e")));
		TestTrue(TEXT("sign-in asked"), B()->SignInWithAura());
		TestEqual(TEXT("asked"), (int32)PlaytestAccount::Phase(B()->GetSignIn()), (int32)EPlaytestSignInPhase::Asked);
	});
	WaitUntil(TEXT("waiting for the browser"), [Phase]() { return Phase() == EPlaytestSignInPhase::Waiting; }, 5.0);
	Then([this, B]()
	{
		TestEqual(TEXT("the page"), B()->GetSignIn().Url, FString(TEXT("https://aura.invalid/auth/cli?code=one")));
		B()->CancelAuraSignIn();
		TestEqual(TEXT("cancelled: nothing shown"), (int32)PlaytestAccount::Phase(B()->GetSignIn()), (int32)EPlaytestSignInPhase::Idle);
	});
	WaitUntil(TEXT("the cancel said back"), [B]() { return B() && B()->GetSignIn().Reason == TEXT("cancelled"); }, 5.0);
	Then([this, B]()
	{
		TestEqual(TEXT("the cancel said back: still nothing shown"), (int32)PlaytestAccount::Phase(B()->GetSignIn()), (int32)EPlaytestSignInPhase::Idle);
		B()->SignInWithAura();
	});
	WaitUntil(TEXT("signed in"), [Phase]() { return Phase() == EPlaytestSignInPhase::SignedIn; }, 5.0);
	// The account frame sent at sign-in time is stale (access "aura" is written at Finish, after it)
	WaitUntil(TEXT("the account at sign-in time"), [B]() { return B() && B()->GetAccount().bSignedIn; }, 5.0);
	Then([this, B]()
	{
		TestEqual(TEXT("signed in as"), B()->GetSignIn().Email, FString(TEXT("dev@studio.test")));
		TestEqual(TEXT("stale: mode none"), B()->GetAccount().Mode, FString(TEXT("none")));
		B()->RequestAccount();  // (what Finish does once preferences.json says "aura")
	});
	WaitUntil(TEXT("the account asked for after Finish says aura"), [B]() { return B() && B()->GetAccount().Mode == TEXT("aura") && B()->GetAccount().bSignedIn; }, 5.0);
	Then([this, B]()
	{
		TestEqual(TEXT("account email"), B()->GetAccount().Email, FString(TEXT("dev@studio.test")));
		TestEqual(TEXT("the Settings page shows the account"), (int32)PlaytestAccount::UsageKind(B()->GetAccount(), TEXT("aura"), false),
			(int32)EPlaytestAccountKind::Aura);
		B()->SignOutOfAura();
	});
	WaitUntil(TEXT("signed out"), [B]() { return B() && B()->GetAccount().Mode == TEXT("aura") && !B()->GetAccount().bSignedIn; }, 5.0);
	Then([B]() { B()->SignInWithAura(true); });  // ("Use another account": sign out first, then in)
	WaitUntil(TEXT("denied"), [Phase]() { return Phase() == EPlaytestSignInPhase::Error; }, 5.0);
	Then([this, B]()
	{
		TestEqual(TEXT("denied line"), PlaytestAccount::ErrorLine(B()->GetSignIn()), FString(TEXT("Sign-in was cancelled in the browser.")));
		B()->RequestAccount();
	});
	WaitUntil(TEXT("the account asked for again"), [B]() { return B() && B()->GetAccountRevision() > 0 && B()->GetAccount().Email == TEXT("asked@studio.test"); }, 5.0);
	Then([B]() { B()->ClearSignIn(); B()->Disconnect(); });
	return true;
}

#if PLATFORM_WINDOWS

namespace
{
	FString PowerShellExe()
	{
		const FString Path = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")), TEXT("System32"), TEXT("WindowsPowerShell"),
			TEXT("v1.0"), TEXT("powershell.exe"));
		return FPaths::FileExists(Path) ? Path : FString();
	}

	/** Waits (the watchers run on threads of their own) until Done, or Seconds pass. */
	bool WaitFor(TFunctionRef<bool()> Done, double Seconds)
	{
		const double Until = FPlatformTime::Seconds() + Seconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Until)
			{
				return false;
			}
			FPlatformProcess::Sleep(0.1f);
		}
		return true;
	}

	FString ReadText(const FString& Path)
	{
		FString Text;  // (the running service's shell has it open, appending)
		FFileHelper::LoadFileToString(Text, *Path, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceProcessTest, "AutopilotPlaytest.AgentService.Process",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceProcessTest::RunTest(const FString& Parameters)
{
	const FString PowerShell = PowerShellExe();
	if (PowerShell.IsEmpty())
	{
		AddWarning(TEXT("No Windows PowerShell here: the process test is skipped"));
		return true;
	}
	FServiceScratch Dir;
	// A stand-in for autopilot serve: it says where it listens in brain.json with its own process id (a process the one
	// started runs, as uv's autopilot.exe runs Python), writes a line to its output, and waits
	const FString Fake = FPaths::Combine(Dir.Path, TEXT("fake-autopilot.ps1"));
	FFileHelper::SaveStringToFile(TEXT(
		"param([string]$Dir)\r\n"
		"[Console]::Out.WriteLine('fake agent service: ' + [Environment]::CommandLine)\r\n"
		"[Console]::Out.Flush()\r\n"
		"$json = '{\"url\": \"ws://127.0.0.1:65530/v1/bridge\", \"pid\": ' + $PID + ', \"started\": 0}'\r\n"
		"Set-Content -Path (Join-Path $Dir 'brain.json') -Value $json -Encoding ascii\r\n"
		"Start-Sleep -Seconds 40\r\n"), *Fake);
	const FString Crash = FPaths::Combine(Dir.Path, TEXT("fake-crash.ps1"));
	FFileHelper::SaveStringToFile(TEXT("[Console]::Out.WriteLine('the stand-in fails')\r\n[Console]::Out.Flush()\r\nexit 3\r\n"), *Crash);

	TSharedRef<FString> Setting = MakeShared<FString>(FString::Printf(TEXT("\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\" \"%s\""), *PowerShell, *Fake, *Dir.Path));
	FPlaytestAgentService::FOptions Options;
	Options.SettingsDir = Dir.Path;
	Options.Search = [Setting]()
	{
		FPlaytestServiceSearch Search;
		Search.Setting = *Setting;
		Search.bWindows = true;
		return Search;
	};
	Options.ExitWithPid = FPlatformProcess::GetCurrentProcessId();

	{
		FPlaytestAgentService Service(Options);
		TestFalse(TEXT("no installer folder"), Service.HasInstaller());
		FString Url;
		TestEqual(TEXT("started"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("starting"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Starting);
		TestEqual(TEXT("asked again while starting: busy, not a second one"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		const bool bRunning = WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Starting; }, 25.0);
		TestTrue(TEXT("said where it listens in time"), bRunning);
		TestEqual(TEXT("running"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Running);
		TestTrue(TEXT("ours (its pid is a process ours started)"), Service.IsOurs());
		TestEqual(TEXT("its url"), Service.GetUrl(), FString(TEXT("ws://127.0.0.1:65530/v1/bridge")));
		TestEqual(TEXT("ready"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestEqual(TEXT("ready url"), Url, FString(TEXT("ws://127.0.0.1:65530/v1/bridge")));
		const FString Log = Service.GetLogPath();
		TestTrue(TEXT("log in logs/"), Log.Contains(TEXT("/logs/agent-service-")));
		WaitFor([&Log]() { return ReadText(Log).Contains(TEXT("fake agent service")); }, 5.0);
		const FString StartLog = ReadText(Log);
		const FString Serve = FString::Printf(TEXT(" serve --port 0 --exit-with %u"), FPlatformProcess::GetCurrentProcessId());
		if (!TestTrue(TEXT("its output in the log, run as autopilot serve --port 0 --exit-with <this pid>"),
			StartLog.Contains(TEXT("fake agent service: ")) && StartLog.Contains(Serve)))
		{
			AddInfo(FString::Printf(TEXT("The log (%s): %s"), *Log, *StartLog));
		}
		// End the stand-in (its shell ends with it)
		FPlaytestAnnouncement Announced;
		if (PlaytestAgentService::ReadAnnouncement(Dir.Path, TEXT("brain.json"), Announced))
		{
			FProcHandle Handle = FPlatformProcess::OpenProcess(Announced.Pid);
			if (Handle.IsValid())
			{
				FPlatformProcess::TerminateProc(Handle, true);
				FPlatformProcess::CloseProc(Handle);
			}
		}
		WaitFor([&Dir]() { FPlaytestAnnouncement A; return !PlaytestAgentService::ReadAnnouncement(Dir.Path, TEXT("brain.json"), A); }, 10.0);
		// Gone soon after it started: not started again by itself
		WaitFor([&Service, &Url]() { return Service.Prepare(Url) == FPlaytestAgentService::EPrepared::Failed; }, 15.0);
		TestEqual(TEXT("ended soon after it started: stopped"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Stopped);
		TestTrue(TEXT("stopped: says where the log is"), Service.GetProblem().Contains(Log));
		Service.Shutdown();
	}

	// One that ends before it says where it listens
	IFileManager::Get().Delete(*FPaths::Combine(Dir.Path, TEXT("brain.json")), false, true, true);
	*Setting = FString::Printf(TEXT("\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\""), *PowerShell, *Crash);
	{
		FPlaytestAgentService Service(Options);
		FString Url;
		Service.Prepare(Url);
		WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Starting; }, 25.0);
		TestEqual(TEXT("crash: stopped"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Stopped);
		TestTrue(TEXT("crash: the agent service stopped (see <log>)"), Service.GetProblem().StartsWith(TEXT("The agent service stopped (see ")));
		const FString CrashLog = ReadText(Service.GetLogPath());
		if (!TestTrue(TEXT("crash: its output in the log"), CrashLog.Contains(TEXT("the stand-in fails"))))
		{
			AddInfo(FString::Printf(TEXT("The log (%s): %s"), *Service.GetLogPath(), *CrashLog));
		}
		TestEqual(TEXT("crash: not tried again by itself"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		Service.Retry();
		TestEqual(TEXT("crash: tried again when asked"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Idle);
		Service.Shutdown();
	}

	// A setting naming nothing
	*Setting = TEXT("D:/nowhere/at-all/python.exe -m autopilot");
	{
		FPlaytestAgentService Service(Options);
		FString Url;
		TestEqual(TEXT("bad setting: failed"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		TestEqual(TEXT("bad setting"), (int32)Service.GetState(), (int32)EPlaytestServiceState::BadCommand);
	}

	// Installing: stand-in installers that fail, then succeed (but put no autopilot anywhere)
	const FString AgentServiceDir = FPaths::Combine(Dir.Path, TEXT("AgentService"));
	IFileManager::Get().MakeDirectory(*AgentServiceDir, true);
	*Setting = FString();
	Options.AgentServiceDir = AgentServiceDir;
	Options.Search = []()
	{
		FPlaytestServiceSearch Search;  // (no PATH, no home: nothing is installed)
		Search.bWindows = true;
		return Search;
	};
	{
		FPlaytestAgentService Service(Options);
		TestFalse(TEXT("no install.ps1 yet"), Service.HasInstaller());
		TestFalse(TEXT("can't install without it"), Service.Install());
		FString Url;
		TestEqual(TEXT("not installed: failed"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		TestEqual(TEXT("not installed"), (int32)Service.GetState(), (int32)EPlaytestServiceState::NotInstalled);

		FFileHelper::SaveStringToFile(TEXT("Write-Output 'stand-in installer fails'\r\nexit 4\r\n"), *FPaths::Combine(AgentServiceDir, TEXT("install.ps1")));
		TestTrue(TEXT("installer there"), Service.HasInstaller());
		TestTrue(TEXT("install started"), Service.Install());
		TestEqual(TEXT("installing"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Installing);
		TestEqual(TEXT("busy while installing"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Installing; }, 30.0);
		TestEqual(TEXT("install failed"), (int32)Service.GetState(), (int32)EPlaytestServiceState::InstallFailed);
		TestTrue(TEXT("install failed: says so"), Service.GetProblem().StartsWith(TEXT("Couldn’t install the agent service (see ")));
		TestTrue(TEXT("install log"), Service.GetLogPath().Contains(TEXT("/logs/install-")));
		TestTrue(TEXT("installer output in the log"), ReadText(Service.GetLogPath()).Contains(TEXT("stand-in installer fails")));

		FFileHelper::SaveStringToFile(TEXT("Write-Output 'stand-in installer works'\r\nexit 0\r\n"), *FPaths::Combine(AgentServiceDir, TEXT("install.ps1")));
		TestTrue(TEXT("install again"), Service.Install());
		WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Installing; }, 30.0);
		TestEqual(TEXT("installed: ready to start"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Idle);
		TestEqual(TEXT("installed, autopilot not found: failed"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		TestEqual(TEXT("installed, autopilot not found: the installer's fault"), (int32)Service.GetState(), (int32)EPlaytestServiceState::InstallFailed);
		Service.Shutdown();
	}

	// Updating: the installer's own autopilot (a stand-in: a program that runs and ends) installed from an older wheel than
	// the one the plugin carries. The plugin's is installed first; when that fails, the installed one is started all the
	// same; when it works (the installer keeps the wheel's agent-service.json), the new one is started, and only once
	const FString Home = FPaths::Combine(Dir.Path, TEXT("home"));
	const FString Autopilot = FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("autopilot.exe"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Autopilot), true);
	TestEqual(TEXT("update: a stand-in autopilot"), (int32)IFileManager::Get().Copy(*Autopilot, *PowerShell), (int32)COPY_OK);
	const FString Stamp = FPaths::Combine(Dir.Path, PlaytestAgentService::StampFile);
	FFileHelper::SaveStringToFile(TEXT("{\"sha256\": \"new\"}"), *FPaths::Combine(AgentServiceDir, PlaytestAgentService::StampFile));
	FFileHelper::SaveStringToFile(TEXT("{\"sha256\": \"old\"}"), *Stamp);
	Options.Search = [Home]()
	{
		FPlaytestServiceSearch Search;  // (no PATH: the installer's place)
		Search.HomeDir = Home;
		Search.bWindows = true;
		return Search;
	};
	const FString InstallPs1 = FPaths::Combine(AgentServiceDir, TEXT("install.ps1"));
	FFileHelper::SaveStringToFile(TEXT("Write-Output 'stand-in update fails'\r\nexit 5\r\n"), *InstallPs1);
	{
		FPlaytestAgentService Service(Options);
		FString Url;
		TestEqual(TEXT("older: busy"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("older: the plugin's installed first"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Installing);
		WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Installing; }, 30.0);
		TestEqual(TEXT("update failed: no dead end"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Idle);
		TestTrue(TEXT("update failed: no problem shown"), Service.GetProblem().IsEmpty());
		TestEqual(TEXT("update failed: the installed one is started"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("update failed: starting it"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Starting);
		Service.Shutdown();
	}
	FFileHelper::SaveStringToFile(FString::Printf(TEXT("Copy-Item -Force (Join-Path $PSScriptRoot 'agent-service.json') '%s'\r\nexit 0\r\n"), *Stamp),
		*InstallPs1);
	{
		FPlaytestAgentService Service(Options);
		FString Url;
		TestEqual(TEXT("again: busy"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("again: installing"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Installing);
		WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Installing; }, 30.0);
		TestEqual(TEXT("updated: ready to start"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Idle);
		TestTrue(TEXT("updated: the installer kept the wheel's agent-service.json"), ReadText(Stamp).Contains(TEXT("\"new\"")));
		TestEqual(TEXT("updated: started"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("updated: starting, not installing again"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Starting);
		Service.Shutdown();
	}
	{
		FPlaytestAgentService Service(Options);  // (the next session: up to date)
		FString Url;
		Service.Prepare(Url);
		TestEqual(TEXT("up to date: started at once"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Starting);
		Service.Shutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceReplaceTest, "AutopilotPlaytest.AgentService.Replace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceReplaceTest::RunTest(const FString& Parameters)
{
	const FString PowerShell = PowerShellExe();
	if (PowerShell.IsEmpty())
	{
		AddWarning(TEXT("No Windows PowerShell here: the replace test is skipped"));
		return true;
	}
	// A plugin that carries its agent service (commit 1111111aaaa), installed and up to date; agent services already
	// running are stand-ins started here (PowerShell, under a Python's name or its own), never a real one
	FServiceScratch Dir;
	const FString AgentServiceDir = FPaths::Combine(Dir.Path, TEXT("AgentService"));
	const FString Shipped = TEXT("{\"sha256\": \"new\", \"commit\": \"1111111aaaa\"}");
	FFileHelper::SaveStringToFile(Shipped, *FPaths::Combine(AgentServiceDir, PlaytestAgentService::StampFile));
	FFileHelper::SaveStringToFile(TEXT("Write-Output 'stand-in installer: not expected'\r\nexit 6\r\n"), *FPaths::Combine(AgentServiceDir, TEXT("install.ps1")));
	FFileHelper::SaveStringToFile(Shipped, *FPaths::Combine(Dir.Path, PlaytestAgentService::StampFile));
	const FString Home = FPaths::Combine(Dir.Path, TEXT("home"));
	const FString Installed = FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("autopilot.exe"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Installed), true);
	IFileManager::Get().Copy(*Installed, *PowerShell);
	const FString Python = FPaths::Combine(Dir.Path, TEXT("py"), TEXT("python.exe"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Python), true);
	TestEqual(TEXT("a stand-in python"), (int32)IFileManager::Get().Copy(*Python, *PowerShell), (int32)COPY_OK);
	const FString Fake = FPaths::Combine(Dir.Path, TEXT("fake-running.ps1"));
	FFileHelper::SaveStringToFile(TEXT(
		"param([string]$Dir, [string]$Commit, [string]$Started = 'now')\r\n"
		"$c = if ($Commit -eq 'none') { '' } else { ', \"commit\": \"' + $Commit + '\"' }\r\n"
		"$off = if ($Started -eq 'now' -or $Started -eq 'none') { 0.0 } else { [double]::Parse($Started.Replace('m', '-'), [Globalization.CultureInfo]::InvariantCulture) }\r\n"
		"$t = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0 + $off\r\n"
		"$s = if ($Started -eq 'none') { '' } else { ', \"started\": ' + $t.ToString([Globalization.CultureInfo]::InvariantCulture) }\r\n"
		"$json = '{\"url\": \"ws://127.0.0.1:' + (60000 + ($PID % 5000)) + '/v1/bridge\", \"pid\": ' + $PID + $s + $c + '}'\r\n"
		"Set-Content -Path (Join-Path $Dir 'brain.json') -Value $json -Encoding ascii\r\n"
		"Start-Sleep -Seconds 60\r\n"), *Fake);
	const FString Brain = FPaths::Combine(Dir.Path, TEXT("brain.json"));

	TArray<FProcHandle> Started;  // (kept open: a process id ended long ago may name another process by then)
	// Runs a stand-in (Program) announcing Commit ("none": no commit) and when it started (Started: "now", seconds from
	// now with m for minus, or "none"), and waits until brain.json names it
	auto Run = [&](const FString& Program, const TCHAR* Commit, const TCHAR* StartedAt = TEXT("now")) -> uint32
	{
		IFileManager::Get().Delete(*Brain, false, true, true);
		uint32 Pid = 0;
		const FString Params = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" \"%s\" %s %s"), *Fake, *Dir.Path, Commit, StartedAt);
		FProcHandle Handle = FPlatformProcess::CreateProc(*Program, *Params, false, true, true, &Pid, 0, *Dir.Path, nullptr, nullptr);
		if (!Handle.IsValid())
		{
			return 0;
		}
		Started.Add(Handle);
		WaitFor([&Dir, Pid]() { FPlaytestAnnouncement A; return PlaytestAgentService::ReadAnnouncement(Dir.Path, TEXT("brain.json"), A) && A.Pid == Pid; }, 20.0);
		return Pid;
	};
	FPlaytestAgentService::FOptions Options;
	Options.SettingsDir = Dir.Path;
	Options.AgentServiceDir = AgentServiceDir;
	TSharedRef<FString> Setting = MakeShared<FString>();
	Options.Search = [Home, Setting]()
	{
		FPlaytestServiceSearch Search;
		Search.Setting = *Setting;
		Search.HomeDir = Home;
		Search.bWindows = true;
		return Search;
	};
	Options.ExitWithPid = FPlatformProcess::GetCurrentProcessId();

	{
		// The same build running: used
		FPlaytestAgentService Service(Options);
		const uint32 Same = Run(Python, TEXT("1111111aaaa"));
		FString Url;
		TestEqual(TEXT("same build: ready"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("same build: left running"), Same != 0 && FPlatformProcess::IsApplicationRunning(Same));
		Service.Shutdown();
	}
	{
		// An older build (a Python): stopped, brain.json removed, this plugin's started
		FPlaytestAgentService Service(Options);
		const uint32 Old = Run(Python, TEXT("2222222bbbb"));
		TestTrue(TEXT("older: running"), Old != 0 && FPlatformProcess::IsApplicationRunning(Old));
		FString Url;
		TestEqual(TEXT("older: busy (stopping it)"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("older: busy while stopping"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		WaitFor([&Service]() { return Service.GetState() == EPlaytestServiceState::Idle; }, 15.0);
		TestFalse(TEXT("older: stopped"), FPlatformProcess::IsApplicationRunning(Old));
		TestFalse(TEXT("older: its brain.json removed"), FPaths::FileExists(Brain));
		TestEqual(TEXT("then this plugin's: started"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Busy);
		TestEqual(TEXT("then this plugin's: starting"), (int32)Service.GetState(), (int32)EPlaytestServiceState::Starting);
		// One that comes back (or another older one) in the same session: used
		WaitFor([&Service]() { return Service.GetState() != EPlaytestServiceState::Starting; }, 25.0);
		const uint32 Back = Run(Python, TEXT("none"));
		TestEqual(TEXT("comes back: used"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("comes back: left running"), Back != 0 && FPlatformProcess::IsApplicationRunning(Back));
		Service.Shutdown();
	}
	{
		// An older one whose process was created well after its brain.json says it started: a process id reused since by
		// another process (here: the stand-in says it started a minute before it was created). Never stopped: used
		FPlaytestAgentService Service(Options);
		const uint32 Reused = Run(Python, TEXT("2222222bbbb"), TEXT("m60"));
		FPlaytestAnnouncement Said;
		TestTrue(TEXT("reused: brain.json says when"), PlaytestAgentService::ReadAnnouncement(Dir.Path, TEXT("brain.json"), Said) && Said.bHasStarted);
		FString Url;
		TestEqual(TEXT("reused id: used"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("reused id: left running"), Reused != 0 && FPlatformProcess::IsApplicationRunning(Reused));
		Service.Shutdown();
	}
	{
		// One whose brain.json doesn't say when it started: can't be told from another process with its id. Used
		FPlaytestAgentService Service(Options);
		const uint32 Unsaid = Run(Python, TEXT("2222222bbbb"), TEXT("none"));
		FString Url;
		TestEqual(TEXT("no started: used"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("no started: left running"), Unsaid != 0 && FPlatformProcess::IsApplicationRunning(Unsaid));
		Service.Shutdown();
	}
	{
		// An older one whose program isn't an agent service's (PowerShell itself): left running and used
		FPlaytestAgentService Service(Options);
		const uint32 Other = Run(PowerShell, TEXT("2222222bbbb"));
		FString Url;
		TestEqual(TEXT("not an agent service's program: used"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("not an agent service's program: left running"), Other != 0 && FPlatformProcess::IsApplicationRunning(Other));
		Service.Shutdown();
	}
	{
		// The setting set: what runs is used, whatever its build
		*Setting = FString::Printf(TEXT("\"%s\" -m autopilot"), *Python);
		FPlaytestAgentService Service(Options);
		const uint32 Old = Run(Python, TEXT("none"));
		FString Url;
		TestEqual(TEXT("setting: used"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("setting: left running"), Old != 0 && FPlatformProcess::IsApplicationRunning(Old));
		Service.Shutdown();
		*Setting = FString();
	}
	{
		// A plugin that carries none (no stamp): what runs is used, as always
		IFileManager::Get().Delete(*FPaths::Combine(AgentServiceDir, PlaytestAgentService::StampFile), false, true, true);
		FPlaytestAgentService Service(Options);
		const uint32 Old = Run(Python, TEXT("none"));
		FString Url;
		TestEqual(TEXT("not carried: used"), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Ready);
		TestTrue(TEXT("not carried: left running"), Old != 0 && FPlatformProcess::IsApplicationRunning(Old));
		Service.Shutdown();
	}
	// The stand-ins still running end here
	for (FProcHandle& Handle : Started)
	{
		if (FPlatformProcess::IsProcRunning(Handle))
		{
			FPlatformProcess::TerminateProc(Handle, true);
		}
		FPlatformProcess::CloseProc(Handle);
	}
	return true;
}

// Removing Autopilot with the command found: run as `<it> uninstall --yes [--keep-memory] --log <log>`, from the
// temporary folder, its output in that log; its exit code says what the tab shows (0 removed, 3 but for some, else not)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaytestAgentServiceUninstallProcessTest, "AutopilotPlaytest.AgentService.UninstallProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaytestAgentServiceUninstallProcessTest::RunTest(const FString& Parameters)
{
	const FString PowerShell = PowerShellExe();
	if (PowerShell.IsEmpty())
	{
		AddWarning(TEXT("No Windows PowerShell here: the uninstall process test is skipped"));
		return true;
	}
	FServiceScratch Dir;
	// A stand-in for autopilot uninstall: it says how it was run and where, then ends with the code it is given
	const FString Fake = FPaths::Combine(Dir.Path, TEXT("fake-uninstall.ps1"));
	FFileHelper::SaveStringToFile(TEXT(
		"param([int]$Code)\r\n"
		"[Console]::Out.WriteLine('fake uninstall: ' + [Environment]::CommandLine)\r\n"
		"[Console]::Out.WriteLine('in: ' + (Get-Location).Path)\r\n"
		"[Console]::Out.WriteLine('unbuffered: ' + $env:PYTHONUNBUFFERED)\r\n"
		"[Console]::Out.Flush()\r\n"
		"exit $Code\r\n"), *Fake);
	TSharedRef<int32> Code = MakeShared<int32>(0);
	FPlaytestAgentService::FOptions Options;
	Options.SettingsDir = FPaths::Combine(Dir.Path, TEXT("Autopilot"));
	FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Options.SettingsDir, TEXT("preferences.json")));
	const FString Script = Fake;
	Options.Search = [PowerShell, Script, Code]()
	{
		FPlaytestServiceSearch Search;
		Search.Setting = FString::Printf(TEXT("\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\" %d"), *PowerShell, *Script, *Code);
		Search.bWindows = true;
		return Search;
	};
	struct FCase { int32 Code; bool bKeep; EPlaytestUninstallState Shown; };
	const FCase Cases[] = { { 0, true, EPlaytestUninstallState::Removed }, { 3, false, EPlaytestUninstallState::RemovedButSome },
		{ 1, true, EPlaytestUninstallState::Failed } };
	for (const FCase& Case : Cases)
	{
		*Code = Case.Code;
		FPlaytestAgentService Service(Options);
		Service.Uninstall(Case.bKeep);
		TestEqual(*FString::Printf(TEXT("%d: removing"), Case.Code), (int32)Service.GetUninstallState(), (int32)EPlaytestUninstallState::Removing);
		WaitFor([&Service]() { return Service.GetUninstallState() != EPlaytestUninstallState::Removing; }, 30.0);
		TestEqual(*FString::Printf(TEXT("%d: what it says"), Case.Code), (int32)Service.GetUninstallState(), (int32)Case.Shown);
		const FString Log = Service.GetUninstallLog();
		const FString Said = ReadText(Log);
		const FString Asked = FString::Printf(TEXT(" uninstall --yes%s --log "), Case.bKeep ? TEXT(" --keep-memory") : TEXT(""));
		if (!TestTrue(*FString::Printf(TEXT("%d: run as uninstall, its output in the log"), Case.Code), Said.Contains(TEXT("fake uninstall: ")) && Said.Contains(Asked)
			&& Said.Contains(TEXT("unbuffered: 1"))))
		{
			AddInfo(FString::Printf(TEXT("The log (%s): %s"), *Log, *Said));
		}
		const FString Temp = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir());
		TestTrue(*FString::Printf(TEXT("%d: from the temporary folder"), Case.Code),
			Said.Replace(TEXT("\\"), TEXT("/")).Contains(TEXT("in: ") + Temp.LeftChop(Temp.EndsWith(TEXT("/")) ? 1 : 0), ESearchCase::IgnoreCase));
		TestTrue(*FString::Printf(TEXT("%d: the settings folder left to it"), Case.Code), FPaths::FileExists(FPaths::Combine(Options.SettingsDir, TEXT("preferences.json"))));
		FString Url;
		TestEqual(*FString::Printf(TEXT("%d: nothing started after"), Case.Code), (int32)Service.Prepare(Url), (int32)FPlaytestAgentService::EPrepared::Failed);
		IFileManager::Get().Delete(*Log, false, true, true);
		Service.Shutdown();
	}
	return true;
}

#endif  // PLATFORM_WINDOWS

#endif  // WITH_DEV_AUTOMATION_TESTS
