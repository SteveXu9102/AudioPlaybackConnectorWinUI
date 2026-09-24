#pragma once

#include "LicenseDialogWindow.g.h"
#include "pch.h"

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	/// <summary>
	/// The licence information, in a window of its own: the tray popup is sized to
	/// its content and a confirmation is only wide enough for a sentence, so a
	/// long text gets a fixed, scrollable window instead.
	///
	/// The window is reused: closing hides it rather than closing it, because a
	/// closed WinUI window cannot be shown a second time.
	/// </summary>
	struct LicenseDialogWindow : LicenseDialogWindowT<LicenseDialogWindow>
	{
		LicenseDialogWindow();

		/// <summary>Sizes, centres and shows the dialog, painted in the given theme.</summary>
		void Show(winrt::Microsoft::UI::Xaml::ElementTheme theme);

	private:
		/// <summary>Hides the window, keeping it available for the next time.</summary>
		void Dismiss();

		/// <summary>
		/// The text the dialog shows: the licence and the third-party notices as they
		/// were compiled into the executable.
		/// </summary>
		static std::wstring BuildText();
	};
}

namespace winrt::AudioPlaybackConnectorWinUI::factory_implementation
{
	struct LicenseDialogWindow : LicenseDialogWindowT<LicenseDialogWindow, implementation::LicenseDialogWindow>
	{
	};
}
