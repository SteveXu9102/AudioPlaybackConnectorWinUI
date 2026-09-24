#pragma once

#include "AppSettings.h"

// Actions a view or dialog can request from the window that hosts it.
//
// Views are XAML runtime classes, so they cannot receive std::function callbacks
// through their WinRT interface. Instead the hosting TrayWindow publishes its
// handlers here and the views call through this table.

namespace AudioPlaybackConnectorWinUI
{
	struct ShellActions
	{
		std::function<void(std::wstring const& deviceId)> connect;
		std::function<void(std::wstring const& deviceId)> disconnect;
		/// <summary>
		/// Asks to remove a device: the popup confirms it, then unpairs it from
		/// Windows. Needs the display name for the confirmation text.
		/// </summary>
		std::function<void(std::wstring const& deviceId, std::wstring const& name)> removeDevice;
		std::function<void()> openBluetoothSettings;
		/// <summary>
		/// Shows the licence information of the application and of everything it
		/// redistributes.
		/// </summary>
		std::function<void()> showLicenses;
		std::function<void()> requestExit;
		/// <summary>Registers or unregisters the application for sign-in startup.</summary>
		std::function<void(bool enabled)> setStartWithWindows;
		std::function<void(ThemeMode theme)> setTheme;
		/// <summary>
		/// Records whether the device is to be connected again on the next launch,
		/// which is a per-device choice.
		/// </summary>
		std::function<void(std::wstring const& deviceId, bool reconnect)> setReconnectDevice;
		/// <summary>Keeps the receiver advertised for every paired sink device.</summary>
		std::function<void(bool allow)> setAllowPaired;
		/// <summary>Announces readiness and connection changes through the tray icon.</summary>
		std::function<void(bool enabled)> setNotifications;
		/// <summary>Switches the interface language, and reloads the translations.</summary>
		std::function<void(std::wstring const& language)> setLanguage;
		/// <summary>
		/// Lets the popup know its content changed size - a device's own submenu was
		/// opened or closed, or the panel grew a card - so the window is re-measured
		/// around what it now holds. The panel is a content-sized popup: without this
		/// the section the user just opened would be clipped by the old window.
		/// </summary>
		std::function<void()> resizePopup;
		/// <summary>Lets the popup window know the process is going away.</summary>
		std::function<void()> prepareExit;
	};

	ShellActions& Shell();

	/// <summary>
	/// Writes the settings, closes the audio connections, removes the
	/// notification-area icon and exits the application.
	/// </summary>
	void ExitApplication();
}
