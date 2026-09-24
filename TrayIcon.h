#pragma once

// Notification-area icon host.
//
// WinUI 3 cannot create a notification-area icon, so this is the only piece of
// classic Win32 UI left in the application: a hidden popup window that receives
// the shell's callback messages for the icon.

namespace AudioPlaybackConnectorWinUI
{
	class TrayIcon
	{
	public:
		/// <summary>Window class of the hidden tray message window.</summary>
		static constexpr wchar_t ClassName[] = L"AudioPlaybackConnectorWinUI.TrayIcon";

		static constexpr UINT WM_TRAYICON = WM_APP + 1;
		/// <summary>Internal: asks the owner to close the popup (posted by the mouse hook).</summary>
		static constexpr UINT WM_DISMISSPOPUP = WM_APP + 3;
		/// <summary>
		/// Posted by a second launch of the executable, which the single-instance
		/// guard turns away: it wakes this instance instead of starting its own.
		/// </summary>
		static constexpr UINT WM_SHOWPANEL = WM_APP + 4;
		static constexpr UINT ICON_ID = 1;

		/// <summary>
		/// Invoked when the user activates the icon. <paramref name="contextMenu"/>
		/// is true for a right-click / keyboard context-menu activation.
		/// </summary>
		using ActivateHandler = std::function<void(bool contextMenu)>;

		using DismissHandler = std::function<void()>;

		TrayIcon() = default;
		~TrayIcon();

		TrayIcon(TrayIcon const&) = delete;
		TrayIcon& operator=(TrayIcon const&) = delete;

		void Create(HINSTANCE instance, std::wstring_view tooltip, ActivateHandler onActivate);
		void Destroy();

		void SetDismissHandler(DismissHandler onDismiss) { m_onDismiss = std::move(onDismiss); }

		/// <summary>
		/// Re-applies the theme-appropriate icon and re-registers it with the
		/// shell. Called for theme changes and after an Explorer restart.
		/// </summary>
		void Refresh();

		/// <summary>
		/// Shows a notification through the icon itself, the way a tray utility
		/// reports something without a window. Returns immediately: the shell call
		/// is handed to a thread of its own, because the caller is the UI thread.
		/// </summary>
		void ShowNotification(std::wstring_view title, std::wstring_view message);

		void SetTooltip(std::wstring_view tooltip);

		RECT IconRect() const;

		HWND Hwnd() const { return m_hwnd; }

	private:
		static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
		LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);

		void LoadIcons();
		void ApplyThemeIcon();
		void AddOrModifyIcon();

		HINSTANCE m_instance = nullptr;
		HWND m_hwnd = nullptr;
		HICON m_iconLight = nullptr;
		HICON m_iconDark = nullptr;
		NOTIFYICONDATAW m_nid{};
		NOTIFYICONIDENTIFIER m_niid{};
		UINT m_taskbarCreatedMessage = 0;
		bool m_iconAdded = false;
		ActivateHandler m_onActivate;
		DismissHandler m_onDismiss;
	};

	/// <summary>Process-wide notification-area icon.</summary>
	TrayIcon& Tray();
}
