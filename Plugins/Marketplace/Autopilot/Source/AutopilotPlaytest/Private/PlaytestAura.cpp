#include "PlaytestAura.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlaytestAgentService.h"
#include "PlaytestPreferences.h"

namespace PlaytestAura
{
	const TCHAR* DownloadUrl = TEXT("https://www.tryaura.dev/");

	FString SignUpUrl(const FString& KeysDir, const FString& AgentServiceDir)
	{
		FString Server = PlaytestPreferences::ReadKey(TEXT("AUTOPILOT_AURA_SERVER"), KeysDir).TrimStartAndEnd();
		if (Server.IsEmpty())
		{
			FString Dir = AgentServiceDir;
			if (Dir.IsEmpty())
			{
				if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Autopilot")))
				{
					Dir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("AgentService"));
				}
			}
			FString Settings;
			if (!Dir.IsEmpty() && FFileHelper::LoadFileToString(Settings, *FPaths::Combine(Dir, TEXT("settings.env"))))
			{
				Server = PlaytestPreferences::KeyIn(Settings, TEXT("AUTOPILOT_AURA_SERVER")).TrimStartAndEnd();
			}
		}
		while (Server.EndsWith(TEXT("/")))
		{
			Server.LeftChopInline(1);
		}
		return (Server.IsEmpty() ? FString(TEXT("https://app.tryaura.dev")) : Server) + TEXT("/auth/signup");
	}

	namespace
	{
		/** The Aura app, where its installer puts it ("" when it isn't there). */
		FString AppPath()
		{
#if PLATFORM_WINDOWS
			const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
			const FString Exe = FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("aura-client"), TEXT("Aura.exe"));
			return !LocalAppData.IsEmpty() && FPaths::FileExists(Exe) ? Exe : FString();
#elif PLATFORM_MAC
			for (const FString& App : { FString(TEXT("/Applications/Aura.app")),
				FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT("Applications"), TEXT("Aura.app")) })
			{
				if (FPaths::DirectoryExists(App))
				{
					return App;
				}
			}
			return FString();
#else
			return FString();
#endif
		}
	}

	bool IsInstalled()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Aura"));
		return !AppPath().IsEmpty() || (Plugin.IsValid() && Plugin->IsEnabled());
	}

	bool Launch()
	{
		const FString App = AppPath();
		if (App.IsEmpty())
		{
			return false;
		}
#if PLATFORM_MAC
		// (an .app is opened, not run: `open` starts it, or brings the running one forward)
		return FPlatformProcess::CreateProc(TEXT("/usr/bin/open"), *FString::Printf(TEXT("\"%s\""), *App), true, false, false,
			nullptr, 0, nullptr, nullptr).IsValid();
#else
		return FPlatformProcess::CreateProc(*App, TEXT(""), true, false, false, nullptr, 0, nullptr, nullptr).IsValid();
#endif
	}

	FString AnnouncedUrl(const TCHAR* File)
	{
		FPlaytestAnnouncement Announced;
		return PlaytestAgentService::ReadAnnouncement(SettingsDir(), File, Announced) ? Announced.Url : FString();
	}

	FString ServiceUrl()
	{
		return AnnouncedUrl(TEXT("aura-brain.json"));
	}

	FString SettingsDir()
	{
		// (the folder autopilot/bridge.py names: Unreal's own UserSettingsDir is ~/Library/Application Support/Epic/ on
		// macOS, where the agent service would never find the plugin's files, nor the plugin the service's brain.json)
#if PLATFORM_WINDOWS
		const FString Base = FPlatformProcess::UserSettingsDir();
#elif PLATFORM_MAC
		const FString Base = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT("Library"), TEXT("Application Support"));
#else
		const FString Xdg = FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_CONFIG_HOME"));
		const FString Base = Xdg.IsEmpty() ? FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".config")) : Xdg;
#endif
		return FPaths::Combine(Base, TEXT("Autopilot"));
	}
}
