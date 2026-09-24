#pragma once

#include "App.xaml.g.h"
#include "pch.h"

#include "TrayWindow.xaml.h"

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	/// <summary>
	/// WinUI 3 application object.
	///
	/// AudioPlaybackConnectorWinUI is a notification-area (tray) utility, so
	/// OnLaunched creates the tray icon plus the hidden popup window instead of
	/// showing a main window.
	/// </summary>
	struct App : AppT<App>
	{
		App();

		void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

	private:
		winrt::AudioPlaybackConnectorWinUI::TrayWindow m_window{ nullptr };
	};
}
