#include "pch.h"
#include "TrayIcon.h"
#include "Direct2DSvg.hpp"
#include "Util.hpp"
#include "resource.h"

#include <thread>

namespace AudioPlaybackConnectorWinUI
{
	namespace
	{
		bool IsColorSchemeChange(LPARAM lParam)
		{
			return lParam
				&& CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
		}
	}

	TrayIcon& Tray()
	{
		static TrayIcon trayIcon;
		return trayIcon;
	}

	TrayIcon::~TrayIcon()
	{
		Destroy();
	}

	void TrayIcon::Create(HINSTANCE instance, std::wstring_view tooltip, ActivateHandler onActivate)
	{
		FAIL_FAST_IF(instance == nullptr);
		m_instance = instance;
		m_onActivate = std::move(onActivate);

		WNDCLASSEXW wcex = {
			.cbSize = sizeof(wcex),
			.lpfnWndProc = WindowProcThunk,
			.hInstance = instance,
			.hCursor = LoadCursorW(nullptr, IDC_ARROW),
			.lpszClassName = TrayIcon::ClassName,
		};
		if (RegisterClassExW(&wcex) == 0)
		{
			FAIL_FAST_IF(GetLastError() != ERROR_CLASS_ALREADY_EXISTS);
		}

		// Regular rather than message-only: broadcast messages such as TaskbarCreated
		// are not delivered to message-only windows.
		m_hwnd = CreateWindowExW(0, TrayIcon::ClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
		FAIL_FAST_LAST_ERROR_IF_NULL(m_hwnd);

		m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
		LOG_LAST_ERROR_IF(m_taskbarCreatedMessage == 0);

		LoadIcons();

		m_nid.cbSize = sizeof(m_nid);
		m_nid.hWnd = m_hwnd;
		m_nid.uID = ICON_ID;
		m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
		m_nid.uCallbackMessage = WM_TRAYICON;
		m_nid.uVersion = NOTIFYICON_VERSION_4;

		// Copied through a string of our own and truncated rather than handed to
		// wcscpy_s whole: szTip holds 127 characters plus the terminator, and
		// wcscpy_s on the array overload calls the invalid-parameter handler - which
		// ends the process - the moment a longer tooltip is passed in.
		const std::wstring tooltipText(tooltip);
		wcsncpy_s(m_nid.szTip, tooltipText.c_str(), _TRUNCATE);

		m_niid.cbSize = sizeof(m_niid);
		m_niid.hWnd = m_hwnd;
		m_niid.uID = ICON_ID;

		AddOrModifyIcon();
	}

	void TrayIcon::Destroy()
	{
		if (m_iconAdded)
		{
			Shell_NotifyIconW(NIM_DELETE, &m_nid);
			m_iconAdded = false;
		}

		if (m_hwnd)
		{
			DestroyWindow(m_hwnd);
			m_hwnd = nullptr;
		}

		if (m_iconLight)
		{
			DestroyIcon(m_iconLight);
			m_iconLight = nullptr;
		}
		if (m_iconDark)
		{
			DestroyIcon(m_iconDark);
			m_iconDark = nullptr;
		}
	}

	void TrayIcon::LoadIcons()
	{
		if (m_iconLight && m_iconDark)
			return;

		auto svg = LoadEmbeddedSvg(m_instance, IDR_SVG);
		const int width = GetSystemMetrics(SM_CXSMICON);
		const int height = GetSystemMetrics(SM_CYSMICON);

		if (!m_iconLight)
			m_iconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
		if (!m_iconDark)
			m_iconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
	}

	void TrayIcon::ApplyThemeIcon()
	{
		LoadIcons();
		m_nid.hIcon = IsSystemUsingLightTheme() ? m_iconLight : m_iconDark;
	}

	void TrayIcon::AddOrModifyIcon()
	{
		ApplyThemeIcon();

		if (m_iconAdded && Shell_NotifyIconW(NIM_MODIFY, &m_nid))
			return;

		if (Shell_NotifyIconW(NIM_ADD, &m_nid))
		{
			m_iconAdded = true;
			if (!Shell_NotifyIconW(NIM_SETVERSION, &m_nid))
			{
				LOG_LAST_ERROR();
			}
		}
		else
		{
			m_iconAdded = false;
			LOG_LAST_ERROR();
		}
	}

	void TrayIcon::Refresh()
	{
		if (!m_hwnd)
			return;

		// Reached from the window procedure, where nothing may escape.
		try
		{
			if (!m_iconAdded)
			{
				AddOrModifyIcon();
				return;
			}

			ApplyThemeIcon();
			if (!Shell_NotifyIconW(NIM_MODIFY, &m_nid))
			{
				m_iconAdded = false;
				AddOrModifyIcon();
			}
		}
		catch (winrt::hresult_error const& ex)
		{
			LogFailure(L"Tray icon", std::wstring(ex.message()));
		}
		catch (...)
		{
			LogFailure(L"Tray icon", L"refreshing the icon failed");
		}
	}

	void TrayIcon::ShowNotification(std::wstring_view title, std::wstring_view message)
	{
		if (m_hwnd == nullptr || !m_iconAdded)
			return;

		// The shell call is made on a thread of its own: it is reached from the
		// state changes of the audio service and from startup, both of which run on
		// the UI thread, and Shell_NotifyIcon talks to Explorer. The strings are
		// copied rather than referenced, and both fields are truncated rather than
		// copied whole - wcscpy_s on an array overload ends the process the moment a
		// longer one is passed in.
		const HWND hwnd = m_hwnd;
		const std::wstring titleText(title);
		const std::wstring bodyText(message);

		std::thread([hwnd, titleText, bodyText]
			{
				NOTIFYICONDATAW nid{};
				nid.cbSize = sizeof(nid);
				nid.hWnd = hwnd;
				nid.uID = ICON_ID;
				nid.uFlags = NIF_INFO;
				nid.dwInfoFlags = NIIF_NONE;
				wcsncpy_s(nid.szInfoTitle, titleText.c_str(), _TRUNCATE);
				wcsncpy_s(nid.szInfo, bodyText.c_str(), _TRUNCATE);

				Shell_NotifyIconW(NIM_MODIFY, &nid);
			}).detach();
	}

	void TrayIcon::SetTooltip(std::wstring_view tooltip)
	{
		if (m_hwnd == nullptr)
			return;

		// szTip holds 127 characters plus the terminator, so this is truncated into
		// the registration's own copy rather than handed over whole.
		const std::wstring text(tooltip);
		wcsncpy_s(m_nid.szTip, text.c_str(), _TRUNCATE);

		Refresh();
	}

	RECT TrayIcon::IconRect() const
	{
		RECT rect{};
		auto hr = Shell_NotifyIconGetRect(&m_niid, &rect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			SetRectEmpty(&rect);
		}
		return rect;
	}

	LRESULT CALLBACK TrayIcon::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		TrayIcon* self = nullptr;

		if (message == WM_NCCREATE)
		{
			auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
			self = static_cast<TrayIcon*>(cs->lpCreateParams);
			self->m_hwnd = hwnd;
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		}
		else
		{
			self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		}

		if (self)
			return self->WindowProc(message, wParam, lParam);

		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	LRESULT TrayIcon::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (m_taskbarCreatedMessage != 0 && message == m_taskbarCreatedMessage)
		{
			m_iconAdded = false;
			Refresh();
			return 0;
		}

		switch (message)
		{
		case WM_DISMISSPOPUP:
			if (m_onDismiss)
				m_onDismiss();
			return 0;

		case WM_SHOWPANEL:
			// A second launch of the executable was turned away by the
			// single-instance guard and asked this instance to show its panel.
			if (m_onActivate)
				m_onActivate(false);
			return 0;

		case WM_TRAYICON:
			// Traced including the notification code: the shell reports one click as
			// several different codes, and which one arrives is what an activation
			// that does nothing has to be read against.
			LogTrace(L"tray activation code " + std::to_wstring(LOWORD(lParam))
				+ (m_onActivate ? L" (handled)" : L" (no handler)"));
			switch (LOWORD(lParam))
			{
			case NIN_SELECT:
			case NIN_KEYSELECT:
				if (m_onActivate)
					m_onActivate(false);
				return 0;

			case WM_RBUTTONUP:
			case WM_CONTEXTMENU:
				if (m_onActivate)
					m_onActivate(true);
				return 0;

			default:
				return 0;
			}

		case WM_SETTINGCHANGE:
			if (IsColorSchemeChange(lParam))
				Refresh();
			break;

		case WM_CLOSE:
			return 0;

		default:
			break;
		}

		return DefWindowProcW(m_hwnd, message, wParam, lParam);
	}
}

