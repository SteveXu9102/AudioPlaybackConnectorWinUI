#include "pch.h"
#include "AppShell.h"

#include <thread>

#include "AudioPlaybackService.h"
#include "TrayIcon.h"
#include "Util.hpp"

namespace AudioPlaybackConnectorWinUI
{
	namespace
	{
		// Nothing may keep the process alive once the user has decided to quit: the
		// exit talks to the media stack and the framework's own shutdown can stall, so
		// a watchdog ends the process whatever it is waiting for.
		//
		// The budget is sized from the connections Stop() releases - each one is
		// revoked and closed, and the close tells the media stack - plus a base that
		// covers the fixed part of the exit (settings write, tray icon, framework
		// teardown).
		constexpr DWORD kExitWatchdogBaseMs = 8000;
		constexpr DWORD kExitWatchdogPerConnectionMs = 3000;
		// Ten connections at 3 s plus the base is 38 s, so the clamp is above the worst
		// case it can be asked to cover while still bounding a genuinely stuck exit.
		constexpr DWORD kExitWatchdogMaxMs = 40000;

		/// <summary>Ends the process if the shutdown below has not finished in time.</summary>
		void ArmExitWatchdog(DWORD budgetMs)
		{
			std::thread([budgetMs] {
				::Sleep(budgetMs);
				::ExitProcess(0);
			}).detach();
		}
	}

	ShellActions& Shell()
	{
		static ShellActions actions;
		return actions;
	}

	void ExitApplication()
	{
		static bool exiting = false;
		if (exiting)
			return;
		exiting = true;

		LogTrace(L"exit: begin");

		// Written *before* the connections are released: Stop() talks to the media
		// stack and can stall, and nothing about the settings should depend on it
		// having finished.
		SaveSettings();
		LogTrace(L"exit: settings saved");

		// Armed once the settings are on disk and sized from the set Stop() closes,
		// which can be larger than the connected devices: a connection that is still
		// opening is in it too.
		const DWORD connections = static_cast<DWORD>(Playback().LiveConnectionCount());
		ArmExitWatchdog(std::min(
			kExitWatchdogMaxMs,
			kExitWatchdogBaseMs + connections * kExitWatchdogPerConnectionMs));

		// Let the popup window stop cancelling its own close.
		if (Shell().prepareExit)
			Shell().prepareExit();
		LogTrace(L"exit: popup prepared");

		// The icon goes first. Everything below talks to the Bluetooth radio and can
		// take a moment; the user must not be left looking at the icon of a program
		// that has already decided to quit.
		Tray().Destroy();
		LogTrace(L"exit: tray icon removed");

		// Let go of the connections properly: releasing them is what deactivates the
		// A2DP transport in the Bluetooth audio service. Skipping that release left
		// the profile behind in a state that refused every later connection with
		// 0x8007001F (ERROR_GEN_FAILURE), so it is not optional.
		Playback().Stop();
		LogTrace(L"exit: connections stopped");

		// Leave without unwinding XAML: that unwinding is the other half of the
		// shutdown that has been seen to sit for seconds. Everything the application
		// owns is saved, hidden, released or gone by now.
		LogTrace(L"exit: leaving the process");
		::ExitProcess(0);
	}
}
