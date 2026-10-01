#pragma once

// "Start with Windows" for a portable, unpackaged application, registered as a
// value under HKCU\...\Run: per user, no elevation, no shortcut file and no
// installer, and the user can still switch it off in Task Manager.

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
