#pragma once

#include "TrayWindow.g.h"
#include "pch.h"

#include "AppSettings.h"
#include "ConfirmDialogWindow.xaml.h"
#include "ContextMenuView.xaml.h"
#include "FluentView.xaml.h"
#include "LicenseDialogWindow.xaml.h"

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	/// <summary>
	/// The notification-area popup. Shows the device panel for a left click and
	/// the context menu for a right click.
	/// </summary>
	struct TrayWindow : TrayWindowT<TrayWindow>
	{
		TrayWindow();
		/// <summary>
		/// Unsubscribes everything that captured this window: the process-wide
		/// tray icon singleton and the global mouse hook all outlive it, and each of
		/// them would otherwise call into freed memory after the window is gone.
		/// </summary>
		~TrayWindow();

		void Toggle();
		void ShowContextMenu();
		void HidePopup();
		void RefreshDevices();

	private:
		/// <summary>Which content the popup currently shows.</summary>
		enum class PopupMode
		{
			Panel,
			Menu,
		};

		void ConfigurePresenter();
		void InstallShellActions();
		void ApplyTheme(::AudioPlaybackConnectorWinUI::ThemeMode mode);
		/// <summary>The XAML theme a setting maps to.</summary>
		static winrt::Microsoft::UI::Xaml::ElementTheme ToElementTheme(::AudioPlaybackConnectorWinUI::ThemeMode mode);
		/// <summary>
		/// Keeps asking for the foreground for a short while after the popup is shown:
		/// a tray callback is not user input for this process, so Windows can refuse the
		/// first request.
		/// </summary>
		void EnsureForeground();
		/// <summary>
		/// Gives the foreground to a window of this application that is already open, and
		/// reports whether there was one. A question the user still has to answer - the
		/// confirmation - outranks the surfaces the tray icon would open.
		/// </summary>
		bool FocusOpenWindow();
		void RefreshAfterEvent();
		/// <summary>
		/// Re-measures the open popup - the panel or the menu, whichever is showing -
		/// and resizes the window to the content it now holds, so content that grew
		/// while it is open is not clipped.
		/// </summary>
		void ResizePopupToContent();
		/// <summary>Runs ResizePopupToContent once the current event has finished.</summary>
		void ResizePopupAfterEvent();
		/// <summary>
		/// Re-measures the open panel and resizes the window to the content it now
		/// holds, through the same placement that put it next to the icon.
		/// </summary>
		void ResizePanelToContent();
		/// <summary>
		/// The same for the menu, which is anchored to the icon the panel is anchored
		/// to and keeps its bottom edge when its content changes.
		/// </summary>
		void ResizeMenuToContent();
		/// <summary>
		/// The menu's pixel size in the given work area: the width its widest row
		/// needs and the height it needs at that width, bounded by the work area.
		/// </summary>
		void MeasureMenuAt(RECT const& work, UINT dpi, int& width, int& height);

		void SetMode(PopupMode mode);
		void ShowPanel();
		void ShowMenu();
		/// <summary>
		/// Places the popup - the panel or the menu, whichever is showing - by the
		/// rule both share: the notification-area icon's rectangle is the anchor, the
		/// bottom edge sits kTrayGap above it, and the result is clamped to the icon
		/// monitor's work area.
		/// </summary>
		void PositionPopup();


		double SlideDistance() const;

		/// <summary>Records the offset the slide starts from.</summary>
		void PrepareSlideIn();
		/// <summary>Runs the entrance on the next loop iteration, once the window is really up.</summary>
		void BeginEntrance();
		/// <summary>Slides the window into place and fades the content in.</summary>
		void AnimateContentIn();
		/// <summary>Fades the content out and slides the window away, then hides it.</summary>
		void AnimateContentOut();
		void StartSlideOut(uint64_t token);
		void StartWindowSlide(
			winrt::Windows::Graphics::RectInt32 const& from,
			winrt::Windows::Graphics::RectInt32 const& to,
			int durationMs,
			bool hidesWindow,
			uint64_t token);
		void EnsureSlideTimer();
		void OnSlideTick();
		void CompleteHide(uint64_t token);
		/// <summary>
		/// Starts a whole-surface fade of the popup content. It is stepped by the
		/// slide timer, so the window and its content animate from one clock.
		/// </summary>
		void StartContentFade(double from, double to, int durationMs);
		void ApplyContentFade();

		/// <summary>
		/// True when this is a repeat of the activation the shell already
		/// delivered for the same physical click.
		/// </summary>
		bool IsDuplicateActivation(PopupMode mode);
		/// <summary>True when this activation is the click that just closed the same popup.</summary>
		bool WasJustDismissed(PopupMode mode) const;
		void RefreshAllViews();

		/// <summary>
		/// Low level mouse hook installed while the popup is visible. Light dismiss
		/// cannot rely on the popup losing activation: clicking the desktop or the
		/// taskbar does not necessarily move the foreground away from it.
		/// </summary>
		void InstallDismissHook();
		void RemoveDismissHook();
		bool ContainsScreenPoint(POINT const& point);
		static LRESULT CALLBACK DismissMouseHook(int code, WPARAM wParam, LPARAM lParam);
		POINT m_lastHookPoint{};

		void ShowExitDialog();
		/// <summary>
		/// Shows a confirmation and runs <paramref name="action"/> only if the user
		/// confirms. The window is created on first use and then reused.
		/// </summary>
		void Confirm(
			winrt::hstring const& title,
			winrt::hstring const& message,
			winrt::hstring const& confirmLabel,
			std::function<void()> action);
		/// <summary>
		/// Shows the licence information in its own window. The window is created
		/// on first use and then reused.
		/// </summary>
		void ShowLicenses();
		void LogPopupMetrics();

		inline static HHOOK s_dismissHook{ nullptr };
		inline static TrayWindow* s_dismissOwner{ nullptr };

		winrt::AudioPlaybackConnectorWinUI::FluentView m_panel{ nullptr };
		winrt::AudioPlaybackConnectorWinUI::ContextMenuView m_menu{ nullptr };
		winrt::com_ptr<ConfirmDialogWindow> m_confirmDialog{ nullptr };
		winrt::com_ptr<LicenseDialogWindow> m_licenseDialog{ nullptr };

		winrt::Windows::Graphics::RectInt32 m_contentRect{};
		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_slideTimer{ nullptr };
		winrt::Windows::Graphics::RectInt32 m_slideFrom{};
		winrt::Windows::Graphics::RectInt32 m_slideTo{};
		uint64_t m_slideStartTick = 0;
		int m_slideDurationMs = 0;
		bool m_slideHidesWindow = false;
		uint64_t m_slideToken = 0;

		double m_fadeFrom{ 1.0 };
		double m_fadeTo{ 1.0 };
		int m_fadeDurationMs{ 0 };
		uint64_t m_fadeStartTick{ 0 };
		bool m_fadeActive{ false };

		PopupMode m_mode{ PopupMode::Panel };
		PopupMode m_dismissedMode{ PopupMode::Panel };
		uint64_t m_dismissedTick = 0;
		PopupMode m_activatedMode{ PopupMode::Panel };
		uint64_t m_activatedTick = 0;
		/// <summary>Invalidates a pending dismissal animation when the popup is shown again.</summary>
		uint64_t m_hideToken = 0;
		bool m_pendingEntrance{ false };
		/// <summary>
		/// Set when the panel's content changed while it was animating in: the size it
		/// now needs is applied once the slide has finished and released the window.
		/// </summary>
		bool m_resizePending{ false };
		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_foregroundTimer{ nullptr };
		unsigned m_foregroundAttempts{ 0 };
		bool m_shown{ false };
		/// <summary>
		/// Set while SetMode hides the window to swap its content. That hide is a
		/// transition step, not a dismissal, and the deactivation it causes must not
		/// be read as one.
		/// </summary>
		bool m_switchingMode{ false };
		bool m_exiting{ false };
	};
}

namespace winrt::AudioPlaybackConnectorWinUI::factory_implementation
{
	struct TrayWindow : TrayWindowT<TrayWindow, implementation::TrayWindow>
	{
	};
}
