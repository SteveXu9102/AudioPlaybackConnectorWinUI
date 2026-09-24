#include "pch.h"
#include "TrayWindow.xaml.h"

#if __has_include("TrayWindow.g.cpp")
#include "TrayWindow.g.cpp"
#endif

#include "AppSettings.h"
#include "AppShell.h"
#include "AudioPlaybackService.h"
#include "I18n.hpp"
#include "StartupRegistration.h"
#include "TrayIcon.h"
#include "Util.hpp"
#include "ViewHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Windowing;
using namespace ::AudioPlaybackConnectorWinUI;

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	namespace
	{
		constexpr double kPanelWidth = 388;
		constexpr double kPanelFallbackHeight = 220;

		// The menu's design width. It is a floor, not a cap: the widest row decides
		// the real width, so no label is cut off whatever language it is drawn in.
		constexpr double kMenuWidth = 248;
		constexpr double kMenuFallbackHeight = 320;

		constexpr double kMinHeight = 170;
		constexpr double kMaxHeight = 620;
		constexpr double kTrayGap = 8;
		constexpr double kScreenMargin = 4;

		// Motion. The whole popup slides into place and its surface fades, both
		// driven from one frame timer. The slide is what moves the window itself:
		// the content is sized exactly to the window, so translating the content
		// would clip it.
		constexpr double kPanelSlideDip = 26;
		constexpr double kMenuSlideDip = 18;
		constexpr int kSlideFrameMs = 15;

		constexpr int kShowSlideMs = 260;
		constexpr int kHideSlideMs = 150;
		constexpr int kShowFadeMs = 200;
		constexpr int kHideFadeMs = 130;

		// Clicking the notification-area icon deactivates (and therefore hides)
		// an open popup before the shell delivers the click. An activation that
		// arrives this soon after the same popup hid is the click that closed
		// it, not a request to open it again.
		constexpr uint64_t kDismissGraceMs = 300;

		// The shell reports one physical activation with more than one callback:
		// a right click arrives as WM_RBUTTONUP followed by WM_CONTEXTMENU.
		// Without this, the second message would toggle the menu straight back
		// closed. A user cannot repeat a click this fast, so the debounce is safe.
		constexpr uint64_t kActivationDebounceMs = 250;

		// How long, and how often, the popup asks for the foreground after it is shown.
		// Windows hands it over as soon as the user's click is accounted for, but that
		// can take a moment - and until it does, no system material is painted.
		constexpr int kForegroundRetryMs = 100;
		constexpr unsigned kForegroundAttempts = 12;
	}

	TrayWindow::TrayWindow()
	{
		InitializeComponent();

		Title(L"AudioPlaybackConnectorWinUI");

		ConfigurePresenter();
		InstallShellActions();

		// The popup paints its own opaque, themed surface, so the theme has to be in
		// place before the first show.
		ApplyTheme(Settings().theme);

		// Posted by the mouse hook when the user presses a button outside the popup.
		Tray().SetDismissHandler([this] { HidePopup(); });

		// Light dismiss: the popup closes as soon as it loses activation.
		Activated([this](IInspectable const&, WindowActivatedEventArgs const& args)
			{
				LogTrace(std::wstring(L"activated shown=") + (m_shown ? L"1" : L"0")
					+ (args.WindowActivationState() == WindowActivationState::Deactivated
						? L" deactivated"
						: L" active"));

				if (!m_shown || m_exiting)
					return;

				if (args.WindowActivationState() == WindowActivationState::Deactivated)
				{
					// The popup hides its own window to swap between the panel and
					// the menu, and hiding deactivates it. Reading that as the user
					// clicking away would record a dismissal nobody made and start a
					// close animation underneath the one that is about to open.
					if (m_switchingMode)
					{
						LogTrace(L"deactivated while switching modes; not a dismissal");
						return;
					}

					HidePopup();
					return;
				}
			});

		RootHost().KeyDown([this](IInspectable const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
			{
				if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
				{
					HidePopup();
					args.Handled(true);
				}
			});

		// The popup is not the application's only window, but closing it must not
		// tear anything down.
		AppWindow().Closing([this](winrt::Microsoft::UI::Windowing::AppWindow const&, AppWindowClosingEventArgs const& args)
			{
				if (!m_exiting)
					args.Cancel(true);
			});

		AppWindow().Hide();
	}

	TrayWindow::~TrayWindow()
	{
		// A started DispatcherQueueTimer is kept alive - and keeps ticking - by the
		// dispatcher itself, so a slide that was still running when the window went
		// away would call OnSlideTick on freed members.
		if (m_slideTimer)
		{
			m_slideTimer.Stop();
			m_slideTimer = nullptr;
		}

		if (m_foregroundTimer)
		{
			m_foregroundTimer.Stop();
			m_foregroundTimer = nullptr;
		}

		// The tray icon is a process-wide singleton that outlives this window, and
		// the handler it holds captured `this`. Left in place, one click after the
		// window was destroyed would call into freed memory.
		Tray().SetDismissHandler(nullptr);

		// The low-level mouse hook is global, so s_dismissOwner would keep pointing
		// at this object and the next click anywhere on the desktop would
		// dereference it.
		RemoveDismissHook();

		// The action table is a process-wide singleton as well, and the panel and the
		// menu call through it. Every entry that captured `this` goes with the window;
		// the remaining ones are free functions and can stay.
		Shell().removeDevice = nullptr;
		Shell().showLicenses = nullptr;
		Shell().requestExit = nullptr;
		Shell().setStartWithWindows = nullptr;
		Shell().setTheme = nullptr;
		Shell().setReconnectDevice = nullptr;
		Shell().setAllowPaired = nullptr;
		Shell().setNotifications = nullptr;
		Shell().setLanguage = nullptr;
		Shell().resizePopup = nullptr;
		Shell().prepareExit = nullptr;
	}

	void TrayWindow::ConfigurePresenter()
	{
		ViewHelpers::ConfigurePopupPresenter(*this);

		// A tray flyout must not appear in the taskbar or the Alt+Tab list.
		AppWindow().IsShownInSwitchers(false);
	}

	bool TrayWindow::FocusOpenWindow()
	{
		// The confirmation is the only other window the application can have open, and a
		// question that has not been answered outranks anything the icon would open.
		if (m_confirmDialog && m_confirmDialog->AppWindow().IsVisible())
		{
			LogTrace(L"tray activation: bringing the open confirmation forward");
			m_foregroundAttempts = 0;
			ViewHelpers::ForceForeground(*m_confirmDialog);
			return true;
		}

		// The licence window is a dialog too, so it comes next: opening the popup
		// over it would leave two surfaces fighting for the foreground.
		if (m_licenseDialog && m_licenseDialog->AppWindow().IsVisible())
		{
			LogTrace(L"tray activation: bringing the open licence dialog forward");
			m_foregroundAttempts = 0;
			ViewHelpers::ForceForeground(*m_licenseDialog);
			return true;
		}

		return false;
	}

	void TrayWindow::EnsureForeground()
	{
		if (!m_shown || m_exiting)
		{
			m_foregroundAttempts = 0;
			return;
		}

		if (GetForegroundWindow() == ViewHelpers::GetWindowHandle(*this))
		{
			// Done: the popup has the keyboard, which is what the focus request was for.
			m_foregroundAttempts = 0;
			if (m_foregroundTimer)
				m_foregroundTimer.Stop();
			return;
		}

		if (m_foregroundAttempts >= kForegroundAttempts)
		{
			// Windows is not going to hand it over: another application owns the
			// foreground and the user is working there.
			LogTrace(L"could not take the foreground; the first click on the popup will");
			return;
		}

		++m_foregroundAttempts;
		ViewHelpers::ForceForeground(*this);

		if (!m_foregroundTimer)
		{
			m_foregroundTimer = RootHost().DispatcherQueue().CreateTimer();
			m_foregroundTimer.Interval(winrt::Windows::Foundation::TimeSpan{
				std::chrono::milliseconds(kForegroundRetryMs) });
			m_foregroundTimer.IsRepeating(false);
			m_foregroundTimer.Tick([this](auto&&, auto&&) { EnsureForeground(); });
		}
		m_foregroundTimer.Start();
	}

	void TrayWindow::RefreshAfterEvent()
	{
		// Deferred: the rebuild tears down the control whose event is being handled.
		RootHost().DispatcherQueue().TryEnqueue([this] { RefreshAllViews(); });
	}

	void TrayWindow::InstallShellActions()
	{
		auto& shell = Shell();

		shell.connect = [](std::wstring const& deviceId) { Playback().Connect(deviceId); };
		shell.disconnect = [](std::wstring const& deviceId) { Playback().Disconnect(deviceId); };

		shell.openBluetoothSettings = []
		{
			winrt::Windows::System::Launcher::LaunchUriAsync(
				winrt::Windows::Foundation::Uri(L"ms-settings:bluetooth"));
		};

		shell.requestExit = [this] { ShowExitDialog(); };

		shell.showLicenses = [this]
		{
			LogTrace(L"show licenses");
			HidePopup();

			ShowLicenses();
		};

		shell.removeDevice = [this](std::wstring const& deviceId, std::wstring const& name)
		{
			// Unpairing deletes the device from Windows, so it is always confirmed.
			LogTrace(L"confirm removing device");
			HidePopup();

			Confirm(
				_(L"Remove device"),
				hstring{ name } + L"\n\n"
					+ _(L"The pairing is deleted from Windows and any audio playing from it stops."),
				_(L"Remove"),
				[deviceId] { Playback().Unpair(deviceId); });
		};

		shell.prepareExit = [this] { m_exiting = true; };

		shell.setReconnectDevice = [](std::wstring const& deviceId, bool reconnect)
		{
			// Nothing is re-rendered here, unlike the other settings: the switch the
			// user just operated already shows the new state, and a rebuild would
			// collapse the per-device section it sits in.
			SetReconnectEnabled(deviceId, reconnect);
			SaveSettings();
		};

		shell.setAllowPaired = [](bool allow)
		{
			Settings().allowPaired = allow;
			SaveSettings();

			// Applied at once rather than on the next refresh: the switch means the
			// receiver is kept ready from this moment, and turning it off takes the
			// standing invitations down through the service's own teardown.
			Playback().SetAllowPaired(allow);
		};

		shell.setNotifications = [](bool enabled)
		{
			Settings().notifications = enabled;
			SaveSettings();
		};

		shell.setLanguage = [this](std::wstring const& language)
		{
			Settings().language = language;
			SaveSettings();

			// Reloaded here and now, so the rebuild below draws the new strings. The
			// view that is not on screen is rebuilt when it is shown, which is where
			// its palette resolves correctly - see RefreshAllViews.
			LoadTranslateData(LanguageIdFromSetting(language));
			Tray().SetTooltip(_(L"AudioPlaybackConnectorWinUI"));

			// Re-render so the menu and the panel show the language they just became.
			RefreshAfterEvent();
		};

		shell.setStartWithWindows = [this](bool enabled)
		{
			// The registry is the state, so the switch is rebuilt from what is
			// actually registered rather than from what was asked for.
			SetStartWithWindowsEnabled(enabled);
			RefreshAfterEvent();
		};

		shell.setTheme = [this](ThemeMode theme)
		{
			Settings().theme = theme;
			SaveSettings();

			ApplyTheme(theme);

			// Re-render so the card reflects the change.
			RefreshAfterEvent();
		};

		shell.resizePopup = [this] { ResizePopupAfterEvent(); };
	}

	ElementTheme TrayWindow::ToElementTheme(ThemeMode mode)
	{
		switch (mode)
		{
		case ThemeMode::Light: return ElementTheme::Light;
		case ThemeMode::Dark:  return ElementTheme::Dark;
		default:               return ElementTheme::Default;
		}
	}

	void TrayWindow::ApplyTheme(ThemeMode mode)
	{
		const ElementTheme theme = ToElementTheme(mode);

		// Applies to the whole popup. The surface and the hosted view are siblings
		// under RootLayer, so the theme has to go there: setting it on the content
		// host alone leaves the surface painted in the system theme.
		RootLayer().RequestedTheme(theme);
	}

	void TrayWindow::SetMode(PopupMode mode)
	{
		if (m_mode == mode && RootHost().Content() != nullptr)
		{
			RefreshAllViews();
			return;
		}

		// One window swaps its content between the panel and the menu, and the
		// compositor keeps presenting the surface it already has while the new content
		// is built and the window is re-sized to it. Switching with the window still on
		// screen would therefore show the previous view at the new mode's geometry, so
		// the window is hidden first and the switch starts from the state every show
		// starts from.
		if (AppWindow().IsVisible())
		{
			// A close that is still animating is superseded by this show just like a
			// finished one: invalidating it here rather than in the enqueued entrance
			// is what keeps its completion from hiding the window that is opening.
			++m_hideToken;
			m_switchingMode = true;
			AppWindow().Hide();
			m_switchingMode = false;

			// The switch is a new show, and this is not HidePopup: without the reset a
			// popup whose foreground attempts a previous show already spent would get
			// exactly one ForceForeground and no retry, and could open behind the
			// window the user is working in.
			m_foregroundAttempts = 0;
		}

		m_mode = mode;

		// A view reads its brushes from its themed palette probes, and a probe only
		// resolves against the inherited theme once the view is in the tree, so every
		// view is attached and laid out *before* it is refreshed. Refreshing a detached
		// view would build its rows from whatever theme that view still had cached.
		if (mode == PopupMode::Panel)
		{
			if (!m_panel)
				m_panel = winrt::make<winrt::AudioPlaybackConnectorWinUI::implementation::FluentView>();

			RootHost().Content(m_panel);
			RootHost().UpdateLayout();
			m_panel.Refresh();
		}
		else
		{
			if (!m_menu)
				m_menu = winrt::make<winrt::AudioPlaybackConnectorWinUI::implementation::ContextMenuView>();

			RootHost().Content(m_menu);
			RootHost().UpdateLayout();
			m_menu.Refresh();
		}

		// Lay the new content out once so that the height measured below is the
		// real one, not the stale value of a view that has never been laid out.
		RootHost().UpdateLayout();
	}

	void TrayWindow::RefreshAllViews()
	{
		// Only the hosted view is refreshed. The other one is rebuilt by SetMode
		// when it is shown, inside the tree, which is where its themed palette
		// resolves correctly - refreshing it while it is detached would only
		// rebuild it with a stale palette.
		if (m_mode == PopupMode::Panel && m_panel)
			m_panel.Refresh();
		else if (m_mode == PopupMode::Menu && m_menu)
			m_menu.Refresh();

		// The content can change size while the popup is open - a device connecting
		// adds its card, the panel's per-device submenu is opened, another language
		// makes the same row wider - and a window that keeps its old size would clip
		// or scroll what it now holds. The popup is re-measured against the same
		// work-area bound it was placed with.
		ResizePopupToContent();
	}

	/// <summary>
	/// Re-measures the open popup and resizes the window to the content it has now.
	/// Does nothing while the window is not shown, and waits for a running slide,
	/// which owns the window's size for its duration.
	/// </summary>
	void TrayWindow::ResizePopupToContent()
	{
		if (!m_shown || m_exiting)
			return;

		if (m_slideTimer && m_slideTimer.IsRunning())
		{
			m_resizePending = true;
			return;
		}

		m_resizePending = false;

		if (m_mode == PopupMode::Panel)
			ResizePanelToContent();
		else
			ResizeMenuToContent();
	}

	/// <summary>
	/// Re-measures the open panel and resizes the window to the content it has now.
	/// Does nothing while the window is not the open panel.
	/// </summary>
	void TrayWindow::ResizePanelToContent()
	{
		if (!m_shown || m_exiting || m_mode != PopupMode::Panel)
			return;

		// Placed by the same rule that placed it in the first place: a section that
		// makes the panel taller grows it upwards from the icon and can never run off
		// the screen - and the part that still does not fit scrolls, because the panel
		// has a ScrollViewer.
		PositionPopup();
	}

	void TrayWindow::ResizePopupAfterEvent()
	{
		// The popup is re-measured around what it holds now, after the layout pass
		// that the visibility change needs has happened.
		RootHost().DispatcherQueue().TryEnqueue([this] { ResizePopupToContent(); });
	}

	/// <summary>
	/// Re-measures the open menu and resizes the window around it, by the same rule
	/// that placed it: the icon's rectangle is the anchor, so a row that became wider
	/// or taller keeps the menu's bottom edge and grows upwards.
	/// </summary>
	void TrayWindow::ResizeMenuToContent()
	{
		if (!m_shown || m_exiting || m_mode != PopupMode::Menu)
			return;

		PositionPopup();
	}

	/// <summary>
	/// The menu's pixel size in the given work area: the width the widest row needs
	/// - never below the design width - and the height the menu needs at that width.
	/// The width is bounded by the work area, so a long string in a narrow work area
	/// wraps rather than running off the screen.
	/// </summary>
	void TrayWindow::MeasureMenuAt(RECT const& work, UINT dpi, int& width, int& height)
	{
		const int margin = ViewHelpers::ScaleToPixels(kScreenMargin, dpi);
		const int availableWidth = static_cast<int>(work.right)
			- static_cast<int>(work.left) - 2 * margin;

		const auto size = ViewHelpers::MeasureContent(
			m_menu,
			kMenuWidth,
			ViewHelpers::PixelsToDip(availableWidth, dpi),
			kMenuFallbackHeight);

		width = ViewHelpers::ScaleToPixels(size.width, dpi);
		height = ViewHelpers::ScaleToPixels(size.height, dpi);
	}

	void TrayWindow::RefreshDevices()
	{
		// Rebuilding the device list clears and recreates every card, which tears
		// down the very control whose event is still being handled - a card's own
		// Connect or Disconnect button, whose handler runs the connection change that
		// leads here. Deferring the rebuild to the next dispatcher turn keeps that
		// control alive until its handler returns.
		RefreshAfterEvent();
	}

	void TrayWindow::Toggle()
	{
		if (IsDuplicateActivation(PopupMode::Panel))
		{
			LogTrace(L"ignore duplicate panel activation");
			return;
		}

		// A window of this application that is already open - the confirmation above
		// all - outranks the panel: the icon brings it forward rather than opening a
		// second surface behind the question the user still has to answer.
		if (FocusOpenWindow())
			return;

		if (m_shown && m_mode == PopupMode::Panel)
		{
			HidePopup();
			return;
		}

		if (WasJustDismissed(PopupMode::Panel))
		{
			LogTrace(L"ignore panel activation after dismiss");
			return;
		}

		ShowPanel();
	}

	void TrayWindow::ShowContextMenu()
	{
		if (IsDuplicateActivation(PopupMode::Menu))
		{
			LogTrace(L"ignore duplicate menu activation");
			return;
		}

		if (FocusOpenWindow())
			return;

		if (m_shown && m_mode == PopupMode::Menu)
		{
			HidePopup();
			return;
		}

		if (WasJustDismissed(PopupMode::Menu))
		{
			LogTrace(L"ignore menu activation after dismiss");
			return;
		}

		ShowMenu();
	}

	bool TrayWindow::IsDuplicateActivation(PopupMode mode)
	{
		const uint64_t now = GetTickCount64();
		const bool duplicate = m_activatedMode == mode
			&& now - m_activatedTick < kActivationDebounceMs;

		m_activatedMode = mode;
		m_activatedTick = now;

		return duplicate;
	}

	void TrayWindow::HidePopup()
	{
		// A second dismiss for the same click - the mouse hook plus a deactivation -
		// would restart the slide from the resting position and make a window that is
		// already moving jump. m_shown is cleared synchronously below, before the hide
		// animation starts, so a second dismiss finds it already false.
		if (!m_shown)
			return;

		if (m_lastHookPoint.x != 0 || m_lastHookPoint.y != 0)
		{
			LogTrace(L"hide popup (click outside at "
				+ std::to_wstring(m_lastHookPoint.x) + L"," + std::to_wstring(m_lastHookPoint.y) + L")");
		}
		else
		{
			LogTrace(L"hide popup");
		}

		RemoveDismissHook();

		// Clear the state before hiding: the hide animation completes
		// asynchronously, and the deactivated event fires in the meantime.
		m_shown = false;
		m_dismissedMode = m_mode;
		m_dismissedTick = GetTickCount64();

		// The window is going away, so the focus request starts over with the next show.
		m_foregroundAttempts = 0;
		if (m_foregroundTimer)
			m_foregroundTimer.Stop();

		AnimateContentOut();
	}

	bool TrayWindow::WasJustDismissed(PopupMode mode) const
	{
		return m_dismissedTick != 0
			&& m_dismissedMode == mode
			&& GetTickCount64() - m_dismissedTick < kDismissGraceMs;
	}

	void TrayWindow::ShowPanel()
	{
		LogTrace(L"show panel");

		ApplyTheme(Settings().theme);

		SetMode(PopupMode::Panel);
		PositionPopup();
		PrepareSlideIn();

		AppWindow().Show();
		Activate();
		ViewHelpers::ForceForeground(*this);

		// m_shown first: EnsureForeground treats "not shown" as "nothing to do".
		m_shown = true;

		// A tray callback is not user input for this process, so the first request for
		// the foreground can be refused; it is repeated for a moment.
		EnsureForeground();

		RootHost().Focus(FocusState::Programmatic);
		m_lastHookPoint = POINT{};
		InstallDismissHook();
		BeginEntrance();
		LogPopupMetrics();
	}

	void TrayWindow::ShowMenu()
	{
		LogTrace(L"show menu");

		ApplyTheme(Settings().theme);

		SetMode(PopupMode::Menu);
		PositionPopup();
		PrepareSlideIn();

		AppWindow().Show();
		Activate();
		ViewHelpers::ForceForeground(*this);

		m_shown = true;

		EnsureForeground();

		m_menu.FocusFirstItem();
		m_lastHookPoint = POINT{};
		InstallDismissHook();
		BeginEntrance();
		LogPopupMetrics();
	}


	double TrayWindow::SlideDistance() const
	{
		return m_mode == PopupMode::Panel ? kPanelSlideDip : kMenuSlideDip;
	}

	/// <summary>
	/// Places the popup - the panel or the menu, whichever is showing - by the one
	/// rule both share.
	///
	/// The anchor is the notification-area icon rectangle both are opened from, not
	/// the pointer. The popup's bottom edge sits kTrayGap above the icon's top and
	/// its right edge on the icon's right, and the result is clamped to the icon
	/// monitor's work area with kScreenMargin on every side. The bottom edge is what
	/// the anchor fixes, so content that grows taller grows the popup upwards and
	/// leaves that edge where it is.
	/// </summary>
	void TrayWindow::PositionPopup()
	{
		RECT iconRect = Tray().IconRect();

		MONITORINFO monitorInfo{ sizeof(monitorInfo) };
		GetMonitorInfoW(MonitorFromRect(&iconRect, MONITOR_DEFAULTTONEAREST), &monitorInfo);

		if (IsRectEmpty(&iconRect))
		{
			// The shell has no rectangle for the icon - the notification area is
			// hidden: the work area's bottom right corner is the anchor then.
			iconRect = monitorInfo.rcWork;
			iconRect.left = iconRect.right;
			iconRect.top = iconRect.bottom;
			iconRect.bottom = iconRect.top + 1;
		}

		// A window that is still hidden is moved onto the anchor first, because
		// GetDpiForWindow describes the monitor the window is on and that is not yet
		// the monitor it is about to be placed on. While it is on screen - the
		// re-placement after its content grew - that move would flash the popup at
		// 1x1, and the window is already on the anchor's monitor.
		const bool wasVisible = AppWindow().IsVisible();
		const UINT dpi = wasVisible
			? GetDpiForWindow(ViewHelpers::GetWindowHandle(*this))
			: ViewHelpers::MoveToAndGetDpi(*this,
				static_cast<int>(iconRect.left), static_cast<int>(iconRect.top), 1, 1);

		const int margin = ViewHelpers::ScaleToPixels(kScreenMargin, dpi);
		const int gap = ViewHelpers::ScaleToPixels(kTrayGap, dpi);

		// The size is each view's own rule and is not changed here: the panel's design
		// width with its measured height, the menu's measured width and height.
		int width = 0;
		int height = 0;
		if (m_mode == PopupMode::Panel)
		{
			width = ViewHelpers::ScaleToPixels(kPanelWidth, dpi);
			height = ViewHelpers::ScaleToPixels(std::clamp(
				ViewHelpers::MeasureContentHeight(m_panel, kPanelWidth, kPanelFallbackHeight),
				kMinHeight,
				kMaxHeight), dpi);
		}
		else
		{
			MeasureMenuAt(monitorInfo.rcWork, dpi, width, height);
		}

		const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
		const int workTop = static_cast<int>(monitorInfo.rcWork.top);
		const int workRight = static_cast<int>(monitorInfo.rcWork.right);
		const int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);

		// The height half of ViewHelpers::ClampToWorkArea, and the reason the clamps
		// below bound the height too: on a short or heavily scaled display the content
		// can need more height than there is between the margins, and moving such a
		// window could only pin it to workTop and let its bottom - the last device row
		// - hang off the screen. The part that fits is what the user gets either way;
		// the difference is whether it is on the screen.
		const int available = workBottom - workTop - 2 * margin;
		if (available > 0 && height > available)
			height = available;

		// Bottom right corner on the icon, opening upwards, like the shell's own
		// notification-area flyouts.
		int x = static_cast<int>(iconRect.right) - width;
		int y = static_cast<int>(iconRect.top) - height - gap;

		x = std::clamp(x, workLeft + margin, std::max(workLeft + margin, workRight - width - margin));
		y = std::clamp(y, workTop + margin, std::max(workTop + margin, workBottom - height - margin));

		const winrt::Windows::Graphics::RectInt32 rect{ x, y, width, height };

		// A re-placement that changed nothing must not resize the window: the popup is
		// sized exactly to its content and the window is already at this rectangle.
		if (wasVisible
			&& rect.X == m_contentRect.X && rect.Y == m_contentRect.Y
			&& rect.Width == m_contentRect.Width && rect.Height == m_contentRect.Height)
		{
			return;
		}

		m_contentRect = rect;

		if (IsTraceEnabled())
		{
			LogTrace(std::wstring(m_mode == PopupMode::Panel ? L"panel" : L"menu")
				+ L" placed " + std::to_wstring(width) + L"x" + std::to_wstring(height)
				+ L" at " + std::to_wstring(x) + L"," + std::to_wstring(y)
				+ L" anchor=" + std::to_wstring(iconRect.left) + L"," + std::to_wstring(iconRect.top)
				+ L"," + std::to_wstring(iconRect.right) + L"," + std::to_wstring(iconRect.bottom)
				+ L" gap=" + std::to_wstring(gap) + L" margin=" + std::to_wstring(margin)
				+ L" work=" + std::to_wstring(workLeft) + L"," + std::to_wstring(workTop)
				+ L"," + std::to_wstring(workRight) + L"," + std::to_wstring(workBottom));
		}

		ViewHelpers::MoveContentArea(*this, x, y, width, height);
	}

	void TrayWindow::PrepareSlideIn()
	{
		if (!AreSystemAnimationsEnabled())
			return;

		const UINT dpi = GetDpiForWindow(ViewHelpers::GetWindowHandle(*this));
		const int offset = ViewHelpers::ScaleToPixels(SlideDistance(), dpi);

		// Only recorded here. The window is moved by AnimateContentIn, so a skipped
		// animation leaves the popup exactly where PositionPopup put it rather than
		// stranded at the offset.
		m_slideTo = m_contentRect;
		m_slideFrom = winrt::Windows::Graphics::RectInt32{
			m_contentRect.X, m_contentRect.Y + offset,
			m_contentRect.Width, m_contentRect.Height };
	}

	void TrayWindow::BeginEntrance()
	{
		m_pendingEntrance = true;

		// The entrance runs on the next loop iteration: the window is shown by then,
		// but the compositor has not presented its first frame yet, so an animation
		// started straight after Show() is mostly over before it can be seen.
		RootHost().DispatcherQueue().TryEnqueue([this]
			{
				if (!m_pendingEntrance)
					return;

				m_pendingEntrance = false;
				AnimateContentIn();
			});
	}

	void TrayWindow::AnimateContentIn()
	{
		// A dismissal that is still animating is superseded by this show.
		++m_hideToken;

		const bool enabled = AreSystemAnimationsEnabled();
		LogTrace(std::wstring(L"animate in enabled=") + (enabled ? L"1" : L"0"));

		if (!enabled)
		{
			// An animation that never ran must still leave the content visible. No
			// slide timer runs in this branch, so the foreground retry - which the
			// slide tick otherwise drives - is started here instead.
			RootLayer().Opacity(1.0);
			EnsureForeground();
			return;
		}

		// Jump to the offset first, then slide up into place.
		ViewHelpers::MoveContentArea(
			*this, m_slideFrom.X, m_slideFrom.Y, m_slideFrom.Width, m_slideFrom.Height);

		StartContentFade(0.0f, 1.0f, kShowFadeMs);

		StartWindowSlide(m_slideFrom, m_slideTo, kShowSlideMs, false, m_hideToken);

		if (m_mode == PopupMode::Panel && m_panel)
			m_panel.PlayEntranceAnimation();
		else if (m_menu)
			m_menu.PlayEntranceAnimation();
	}

	void TrayWindow::AnimateContentOut()
	{
		const uint64_t token = ++m_hideToken;
		m_pendingEntrance = false;

		if (!AreSystemAnimationsEnabled())
		{
			LogTrace(L"animate out skipped (animations disabled)");
			CompleteHide(token);
			return;
		}

		LogTrace(L"animate out");

		StartContentFade(1.0f, 0.0f, kHideFadeMs);
		StartSlideOut(token);
	}

	void TrayWindow::StartContentFade(double from, double to, int durationMs)
	{
		// The content fades through its own Opacity rather than a composition animation
		// on the element visual, because the window content lives in a content island
		// where animating that visual had no visible effect. The slide timer is already
		// running for long enough to step the framework property.
		m_fadeFrom = from;
		m_fadeTo = to;
		m_fadeDurationMs = durationMs;
		m_fadeStartTick = GetTickCount64();
		m_fadeActive = true;

		RootLayer().Opacity(from);

		EnsureSlideTimer();
		m_slideTimer.Start();
	}

	void TrayWindow::ApplyContentFade()
	{
		if (!m_fadeActive)
			return;

		const uint64_t elapsed = GetTickCount64() - m_fadeStartTick;
		if (m_fadeDurationMs <= 0 || elapsed >= static_cast<uint64_t>(m_fadeDurationMs))
		{
			RootLayer().Opacity(m_fadeTo);
			m_fadeActive = false;
			return;
		}

		const double progress = static_cast<double>(elapsed) / m_fadeDurationMs;
		// Decelerate into place when opening, accelerate away when closing.
		const double eased = m_fadeTo > m_fadeFrom
			? ViewHelpers::Decelerate(progress)
			: ViewHelpers::Accelerate(progress);

		RootLayer().Opacity(m_fadeFrom + (m_fadeTo - m_fadeFrom) * eased);
	}

	void TrayWindow::StartSlideOut(uint64_t token)
	{
		const UINT dpi = GetDpiForWindow(ViewHelpers::GetWindowHandle(*this));
		const int offset = ViewHelpers::ScaleToPixels(SlideDistance(), dpi);

		m_slideFrom = m_contentRect;
		m_slideTo = winrt::Windows::Graphics::RectInt32{
			m_contentRect.X, m_contentRect.Y + offset,
			m_contentRect.Width, m_contentRect.Height };

		StartWindowSlide(m_slideFrom, m_slideTo, kHideSlideMs, true, token);
	}

	void TrayWindow::StartWindowSlide(
		winrt::Windows::Graphics::RectInt32 const& from,
		winrt::Windows::Graphics::RectInt32 const& to,
		int durationMs,
		bool hidesWindow,
		uint64_t token)
	{
		m_slideFrom = from;
		m_slideTo = to;
		m_slideDurationMs = durationMs;
		m_slideHidesWindow = hidesWindow;
		m_slideToken = token;
		m_slideStartTick = GetTickCount64();

		EnsureSlideTimer();
		m_slideTimer.Start();
	}

	void TrayWindow::EnsureSlideTimer()
	{
		if (m_slideTimer)
			return;

		auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
		m_slideTimer = queue.CreateTimer();
		m_slideTimer.Interval(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(kSlideFrameMs) });
		m_slideTimer.IsRepeating(true);
		m_slideTimer.Tick([this](auto&&, auto&&) { OnSlideTick(); });
	}

	void TrayWindow::OnSlideTick()
	{
		const uint64_t elapsed = GetTickCount64() - m_slideStartTick;
		double progress = m_slideDurationMs > 0
			? static_cast<double>(elapsed) / m_slideDurationMs
			: 1.0;
		if (progress > 1.0)
			progress = 1.0;

		// Decelerate into place when opening, accelerate away when closing.
		const double eased = m_slideHidesWindow
			? ViewHelpers::Accelerate(progress)
			: ViewHelpers::Decelerate(progress);

		const int x = static_cast<int>(std::lround(
			m_slideFrom.X + (m_slideTo.X - m_slideFrom.X) * eased));
		const int y = static_cast<int>(std::lround(
			m_slideFrom.Y + (m_slideTo.Y - m_slideFrom.Y) * eased));

		AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{
			x, y, m_slideFrom.Width, m_slideFrom.Height });

		ApplyContentFade();

		if (IsTraceEnabled())
		{
			LogTrace(L"tick t=" + std::to_wstring(elapsed) + L" y=" + std::to_wstring(y)
				+ L" op=" + std::to_wstring(RootLayer().Opacity()));
		}

		// Keep the clock running until both the slide and the fade have finished.
		if (progress < 1.0 || m_fadeActive)
			return;

		m_slideTimer.Stop();

		if (m_slideHidesWindow)
		{
			CompleteHide(m_slideToken);
			return;
		}

		if (m_resizePending)
		{
			if (m_mode == PopupMode::Panel)
				ResizePanelToContent();
			else
				ResizeMenuToContent();
		}

		// The animation is over and the window has settled: one more request for the
		// foreground, which the first attempt does not always get.
		EnsureForeground();
	}

	void TrayWindow::CompleteHide(uint64_t token)
	{
		// Stale completion, or the application is going away: leave the window alone.
		if (token != m_hideToken || m_exiting)
			return;

		AppWindow().Hide();
	}

	void TrayWindow::InstallDismissHook()
	{
		if (s_dismissHook != nullptr)
			return;

		s_dismissOwner = this;
		s_dismissHook = SetWindowsHookExW(WH_MOUSE_LL, DismissMouseHook, GetModuleHandleW(nullptr), 0);

		if (s_dismissHook == nullptr)
		{
			LOG_LAST_ERROR();
			s_dismissOwner = nullptr;
		}
	}

	void TrayWindow::RemoveDismissHook()
	{
		if (s_dismissHook == nullptr)
			return;

		UnhookWindowsHookEx(s_dismissHook);
		s_dismissHook = nullptr;
		s_dismissOwner = nullptr;
	}

	bool TrayWindow::ContainsScreenPoint(POINT const& point)
	{
		RECT rect{};
		const HWND hwnd = ViewHelpers::GetWindowHandle(*this);
		if (hwnd == nullptr || !GetWindowRect(hwnd, &rect))
			return false;

		return point.x >= rect.left && point.x < rect.right
			&& point.y >= rect.top && point.y < rect.bottom;
	}

	LRESULT CALLBACK TrayWindow::DismissMouseHook(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HC_ACTION && s_dismissOwner != nullptr)
		{
			switch (wParam)
			{
			case WM_LBUTTONDOWN:
			case WM_RBUTTONDOWN:
			case WM_MBUTTONDOWN:
			case WM_NCLBUTTONDOWN:
			case WM_NCRBUTTONDOWN:
			case WM_NCMBUTTONDOWN:
			{
				auto const* info = reinterpret_cast<MSLLHOOKSTRUCT const*>(lParam);
				if (info != nullptr && !s_dismissOwner->ContainsScreenPoint(info->pt))
				{
					s_dismissOwner->m_lastHookPoint = info->pt;

					// A hook must return promptly, so ask the tray window to close
					// the popup on the next message loop iteration instead.
					PostMessageW(Tray().Hwnd(), TrayIcon::WM_DISMISSPOPUP, 0, 0);
				}
				break;
			}

			default:
				break;
			}
		}

		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	void TrayWindow::ShowExitDialog()
	{
		// The confirmation exists to warn that live connections are about to be
		// dropped, so with nothing connected Exit is immediate.
		if (!Playback().HasConnectedDevice())
		{
			LogTrace(L"exit without confirmation (nothing connected)");
			ExitApplication();
			return;
		}

		LogTrace(L"confirm exit (connections="
			+ std::to_wstring(Playback().ConnectedDeviceIds().size()) + L")");

		// The popup is sized exactly to its content, so the confirmation gets its
		// own window instead of being squeezed into that surface.
		HidePopup();

		Confirm(
			_(L"Exit AudioPlaybackConnectorWinUI"),
			_(L"All connections will be closed.\nExit anyway?"),
			_(L"Exit"),
			[] { ExitApplication(); });
	}

	void TrayWindow::Confirm(
		hstring const& title,
		hstring const& message,
		hstring const& confirmLabel,
		std::function<void()> action)
	{
		if (!m_confirmDialog)
			m_confirmDialog = winrt::make_self<ConfirmDialogWindow>();

		// The dialog is a window of its own, so it does not inherit the popup's
		// theme: it is handed the current one instead.
		m_confirmDialog->Show(
			title, message, confirmLabel, ToElementTheme(Settings().theme), std::move(action));
	}

	void TrayWindow::ShowLicenses()
	{
		if (!m_licenseDialog)
			m_licenseDialog = winrt::make_self<LicenseDialogWindow>();

		// Its own window, as the confirmation has: a licence does not fit a popup
		// that is sized exactly to its content.
		m_licenseDialog->Show(ToElementTheme(Settings().theme));
	}

	void TrayWindow::LogPopupMetrics()
	{
		if (!IsTraceEnabled())
			return;

		const HWND hwnd = ViewHelpers::GetWindowHandle(*this);
		RECT window{};
		RECT client{};
		GetWindowRect(hwnd, &window);
		GetClientRect(hwnd, &client);

		const ViewHelpers::FrameInsets insets = ViewHelpers::GetFrameInsets(hwnd);
		const bool panel = m_mode == PopupMode::Panel;

		LogTrace(std::wstring(panel ? L"metrics panel " : L"metrics menu ")
			+ L"window=" + std::to_wstring(window.right - window.left)
			+ L"x" + std::to_wstring(window.bottom - window.top)
			+ L" client=" + std::to_wstring(client.right - client.left)
			+ L"x" + std::to_wstring(client.bottom - client.top)
			+ L" insets=" + std::to_wstring(insets.left) + L"," + std::to_wstring(insets.top)
			+ L"," + std::to_wstring(insets.right) + L"," + std::to_wstring(insets.bottom)
			+ L" sized=" + std::to_wstring(m_contentRect.Width)
			+ L"x" + std::to_wstring(m_contentRect.Height)
			+ L" content=" + std::to_wstring(static_cast<int>(panel
				? ViewHelpers::MeasureContentHeight(m_panel, kPanelWidth, kPanelFallbackHeight)
				: ViewHelpers::MeasureContentHeight(m_menu, kMenuWidth, kMenuFallbackHeight)))
			+ L" desiredWidth=" + std::to_wstring(static_cast<int>(panel
				? kPanelWidth
				: ViewHelpers::MeasureContent(m_menu, kMenuWidth, 1.0e6, kMenuFallbackHeight).width))
			+ L" actualTheme=" + std::to_wstring(static_cast<int>(RootLayer().ActualTheme()))
			+ L" surface=" + ViewHelpers::BrushText(Surface().Background())
			+ L" opaque=" + std::to_wstring(Surface().Visibility() == Visibility::Visible ? 1 : 0));
	}
}

