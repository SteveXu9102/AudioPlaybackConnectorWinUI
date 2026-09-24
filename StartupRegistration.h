#pragma once

// "Start with Windows" for a portable, unpackaged application.
//
// The registration is a value under HKCU\...\Run. That is the classic mechanism
// for this kind of application: per user, no elevation, no shortcut file and no
// installer, and the user can still switch it off in Task Manager. The other
// implementations were deliberately not used:
//
//   * a shortcut in the Startup folder is the same idea with a file instead of a
//     registry value, but it needs the shell link API and leaves a file behind;
//   * a scheduled task can delay the start or run with different privileges, at
//     the cost of a much heavier dependency (the Task Scheduler COM API);
//   * Windows.ApplicationModel.StartupTask only exists for packaged applications,
//     and this application is unpackaged.

namespace AudioPlaybackConnectorWinUI
{
	/// <summary>True when this file is registered to start at sign-in.</summary>
	bool IsStartWithWindowsEnabled();

	/// <summary>
	/// Registers or unregisters this file and returns the state in effect
	/// afterwards, which is what the switch in the menu displays.
	/// </summary>
	bool SetStartWithWindowsEnabled(bool enabled);

	/// <summary>
	/// Rewrites a registration that points at another copy of the file - the user
	/// may have moved it - so that signing in still starts something.
	/// </summary>
	void RepairStartWithWindows();

	/// <summary>
	/// Path of the distributed file. The launcher passes its own path down in
	/// APC_LAUNCHER_PATH, because the application itself runs from the folder the
	/// payload was unpacked into, which is not what should be registered.
	/// </summary>
	std::wstring DistributedExecutablePath();
}
