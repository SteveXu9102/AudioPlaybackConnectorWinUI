#include "pch.h"
#include "App.xaml.h"

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include "AppSettings.h"
#include "AudioPlaybackService.h"
#include "I18n.hpp"
#include "StartupRegistration.h"
#include "TrayIcon.h"
#include "TrayWindow.xaml.h"
#include "Util.hpp"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace ::AudioPlaybackConnectorWinUI;

// Module handle used by the translation catalog to locate the embedded
// translation resources. Declared extern in I18n.hpp.
HINSTANCE g_hInst = nullptr;

namespace
{
	constexpr wchar_t kSingleInstanceMutex[] = L"Local\\AudioPlaybackConnectorWinUI.SingleInstance";

	/// <summary>
	/// Reports a failure that happened before the application could put its tray icon
	/// up, then ends the process.
	/// </summary>
	[[noreturn]] void ReportStartupFailure(std::wstring const& message)
	{
		LogFailure(L"OnLaunched", message);

		const std::wstring text = _(L"AudioPlaybackConnectorWinUI could not start.")
			+ std::wstring(L"\n\n") + message;

		MessageBoxW(nullptr, text.c_str(), _(L"AudioPlaybackConnectorWinUI"), MB_OK | MB_ICONERROR);
		::ExitProcess(1);
	}
}

// Entry point.
// The XAML compiler's generated main is disabled (DISABLE_XAML_GENERATED_MAIN in
// the project file) so that the single-instance guard runs before any WinUI or
// window state is created. A second launch must not add a second tray icon: it
// asks the running instance to show its panel and then exits.
//
// Command-line arguments are ignored, and there is nothing for them to select.
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	SetLastError(ERROR_SUCCESS);
	wil::unique_handle mutex(CreateMutexW(nullptr, TRUE, kSingleInstanceMutex));

	if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (HWND tray = FindWindowW(TrayIcon::ClassName, nullptr))
			PostMessageW(tray, TrayIcon::WM_SHOWPANEL, 0, 0);

		return 0;
	}

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
		{
			winrt::make<winrt::AudioPlaybackConnectorWinUI::implementation::App>();
		});

	return 0;
}

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	App::App()
	{
		InitializeComponent();

		UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& args)
			{
				LogFailure(L"UnhandledException", args.Message().c_str());
			});
	}

	void App::OnLaunched(LaunchActivatedEventArgs const&)
	{
		try
		{
			g_hInst = GetModuleHandleW(nullptr);

			// The settings come first: the language they hold decides which
			// translation catalog is loaded.
			LoadSettings();
			LoadTranslateData(LanguageIdFromSetting(Settings().language));

			RepairStartWithWindows();

			if (!AudioPlaybackService::IsSupported())
			{
				MessageBoxW(
					nullptr,
					_(L"AudioPlaybackConnectorWinUI is not supported on this operating system version."),
					_(L"Unsupported Operating System"),
					MB_OK | MB_ICONERROR);
				Application::Current().Exit();
				return;
			}

			m_window = winrt::make<implementation::TrayWindow>();

			const auto window = m_window;
			Tray().Create(
				g_hInst,
				_(L"AudioPlaybackConnectorWinUI"),
				[window](bool contextMenu)
				{
					if (contextMenu)
						window.ShowContextMenu();
					else
						window.Toggle();
				});

			Playback().SetChangeHandler([window] { window.RefreshDevices(); });

			// The two events worth announcing through the icon. It is installed
			// before anything can connect, so a device that comes up during startup
			// is announced like any other.
			Playback().SetConnectionEventHandler([](std::wstring const& deviceName, bool connected)
				{
					// Traced as well as shown, and before the switch is consulted: which
					// device a connection event names is the one part of this that the
					// panel cannot show, and naming the wrong device is exactly what a
					// mixed-up device identity used to do.
					LogTrace(L"connection event: " + deviceName
						+ (connected ? L" connected" : L" disconnected"));

					if (!Settings().notifications)
						return;

					Tray().ShowNotification(
						connected ? _(L"Audio device connected") : _(L"Audio device disconnected"),
						deviceName);
				});

			Playback().Start();

			Playback().SetAllowPaired(Settings().allowPaired);

			if (!Settings().reconnectDevices.empty())
				Playback().ReconnectSaved();

			if (Settings().notifications)
				Tray().ShowNotification(
					_(L"AudioPlaybackConnectorWinUI is ready"),
					_(L"Waiting for a Bluetooth audio device."));
		}
		catch (winrt::hresult_error const& ex)
		{
			ReportStartupFailure(std::wstring(ex.message()));
		}
		catch (std::exception const& ex)
		{
			ReportStartupFailure(Utf8ToUtf16(ex.what()));
		}
		catch (...)
		{
			ReportStartupFailure(L"unknown failure");
		}
	}
}
