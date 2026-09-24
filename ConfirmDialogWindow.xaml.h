#pragma once

#include "ConfirmDialogWindow.g.h"
#include "pch.h"

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	/// <summary>
	/// A confirmation, hosted in its own small window so it is not constrained by
	/// the tray popup, which is sized exactly to its content.
	///
	/// The window is reused: cancelling hides it rather than closing it, because a
	/// closed WinUI window cannot be shown a second time.
	/// </summary>
	struct ConfirmDialogWindow : ConfirmDialogWindowT<ConfirmDialogWindow>
	{
		ConfirmDialogWindow();

		/// <summary>
		/// Fills in the confirmation, sizes and centres it, then shows it.
		///
		/// <paramref name="onConfirm"/> is only run if the user confirms, and only
		/// after the window has been hidden. It is a plain C++ callback rather than
		/// a WinRT event because the popup owns the window and calls it directly.
		/// </summary>
		void Show(
			winrt::hstring const& title,
			winrt::hstring const& message,
			winrt::hstring const& confirmLabel,
			winrt::Microsoft::UI::Xaml::ElementTheme theme,
			std::function<void()> onConfirm);

	private:
		/// <summary>Hides the window, keeping it available for the next confirmation.</summary>
		void Dismiss();

		std::function<void()> m_onConfirm;
	};
}

namespace winrt::AudioPlaybackConnectorWinUI::factory_implementation
{
	struct ConfirmDialogWindow : ConfirmDialogWindowT<ConfirmDialogWindow, implementation::ConfirmDialogWindow>
	{
	};
}
