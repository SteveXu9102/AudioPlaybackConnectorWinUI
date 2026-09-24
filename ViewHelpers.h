#pragma once

#include "AudioPlaybackService.h"
#include "I18n.hpp"
#include "Util.hpp"

// Small helpers shared by the design views.
//
// Both views build their device rows in code (rather than through a
// DataTemplate) because a row's content depends on live connection state and
// the views are rebuilt wholesale whenever that state changes. Themed brushes
// are looked up from each view's own XAML resources so that Fluent colour
// tokens stay declared in markup.

namespace AudioPlaybackConnectorWinUI::ViewHelpers
{
	namespace Mux = winrt::Microsoft::UI::Xaml;
	namespace Muxc = winrt::Microsoft::UI::Xaml::Controls;
	namespace Muxm = winrt::Microsoft::UI::Xaml::Media;

	inline Muxm::SolidColorBrush Transparent()
	{
		return Muxm::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent());
	}

	/// <summary>
	/// Opaque white, for a glyph drawn on an accent fill. The accent fill is chosen
	/// by the theme system to carry white text; a card background token is not, and
	/// disappears under it in the dark theme.
	/// </summary>
	inline Muxm::SolidColorBrush White()
	{
		return Muxm::SolidColorBrush(winrt::Microsoft::UI::Colors::White());
	}

	inline winrt::Windows::UI::Color ColorOf(Muxm::Brush const& brush)
	{
		auto solid = brush ? brush.try_as<Muxm::SolidColorBrush>() : nullptr;
		return solid ? solid.Color() : winrt::Microsoft::UI::Colors::Transparent();
	}

	/// <summary>
	/// The ARGB text of a brush, for diagnostics. A themed brush only reports the
	/// colour of the theme it was resolved against, which is what makes a palette
	/// read while a view is detached from the tree visible in the trace.
	/// </summary>
	inline std::wstring BrushText(Muxm::Brush const& brush)
	{
		auto solid = brush ? brush.try_as<Muxm::SolidColorBrush>() : nullptr;
		if (!solid)
			return L"-";

		const auto colour = solid.Color();
		const uint32_t argb = (static_cast<uint32_t>(colour.A) << 24)
			| (static_cast<uint32_t>(colour.R) << 16)
			| (static_cast<uint32_t>(colour.G) << 8)
			| static_cast<uint32_t>(colour.B);

		constexpr wchar_t kDigits[] = L"0123456789ABCDEF";
		std::wstring text = L"#";
		for (int shift = 28; shift >= 0; shift -= 4)
			text += kDigits[(argb >> shift) & 0xF];

		return text;
	}

	inline Muxc::FontIcon MakeGlyph(wchar_t const* glyph, double size)
	{
		Muxc::FontIcon icon;
		icon.Glyph(glyph);
		icon.FontSize(size);
		return icon;
	}

	inline Muxc::FontIcon MakeGlyph(wchar_t const* glyph, double size, Muxm::Brush const& brush)
	{
		auto icon = MakeGlyph(glyph, size);
		icon.Foreground(brush);
		return icon;
	}

	inline Muxc::TextBlock MakeText(winrt::hstring const& text, double size)
	{
		Muxc::TextBlock block;
		block.Text(text);
		block.FontSize(size);
		block.TextTrimming(Mux::TextTrimming::CharacterEllipsis);
		block.VerticalAlignment(Mux::VerticalAlignment::Center);
		return block;
	}

	inline Muxc::ProgressRing MakeSpinner(double size)
	{
		Muxc::ProgressRing ring;
		ring.IsActive(true);
		ring.Width(size);
		ring.Height(size);
		return ring;
	}

	inline Mux::CornerRadius CornerRadius(double radius)
	{
		return Mux::CornerRadius{ radius, radius, radius, radius };
	}

	inline HWND GetWindowHandle(Mux::Window const& window)
	{
		HWND hwnd{ nullptr };
		winrt::check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
		return hwnd;
	}

	inline int ScaleToPixels(double dip, UINT dpi)
	{
		return static_cast<int>(std::lround(dip * dpi / 96.0));
	}

	inline double PixelsToDip(int pixels, UINT dpi)
	{
		return pixels * 96.0 / dpi;
	}

	struct FrameInsets
	{
		int left = 0;
		int top = 0;
		int right = 0;
		int bottom = 0;
	};

	/// <summary>
	/// A WinUI borderless window still keeps a few pixels of non-client frame, so
	/// its client area is smaller than its window rectangle. Everything here is
	/// expressed in terms of the visible content, so these insets are added back
	/// when the window is moved and resized.
	/// </summary>
	inline FrameInsets GetFrameInsets(HWND hwnd)
	{
		RECT window{};
		RECT client{};
		POINT origin{ 0, 0 };

		if (hwnd == nullptr
			|| !GetWindowRect(hwnd, &window)
			|| !GetClientRect(hwnd, &client)
			|| !ClientToScreen(hwnd, &origin))
		{
			return {};
		}

		FrameInsets insets;
		insets.left = origin.x - window.left;
		insets.top = origin.y - window.top;
		insets.right = window.right - (origin.x + client.right);
		insets.bottom = window.bottom - (origin.y + client.bottom);
		return insets;
	}

	/// <summary>
	/// Strips the non-client frame. That frame is outside the client area, so the
	/// content cannot paint it: it shows up as a gap between the window border and
	/// the panel, and it makes the client area smaller than the window.
	/// </summary>
	inline void RemoveNonClientFrame(Mux::Window const& window)
	{
		const HWND hwnd = GetWindowHandle(window);
		if (hwnd == nullptr)
			return;

		LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
		style |= WS_POPUP;
		SetWindowLongPtrW(hwnd, GWL_STYLE, style);

		LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		exStyle &= ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	inline void MoveContentArea(Mux::Window const& window, int x, int y, int width, int height)
	{
		const FrameInsets insets = GetFrameInsets(GetWindowHandle(window));

		window.AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{
			x - insets.left,
			y - insets.top,
			width + insets.left + insets.right,
			height + insets.top + insets.bottom });
	}

	/// <summary>Work area of the monitor that contains a screen point.</summary>
	inline RECT WorkAreaForPoint(POINT screenPoint)
	{
		MONITORINFO monitorInfo{ sizeof(monitorInfo) };
		if (GetMonitorInfoW(MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST), &monitorInfo))
			return monitorInfo.rcWork;

		monitorInfo.rcWork = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		return monitorInfo.rcWork;
	}

	/// <summary>
	/// Shrinks a pixel size until it fits the work area with a margin on every side.
	/// Clamping the position alone is not enough, because a window taller than the
	/// work area cannot be brought fully on screen by moving it.
	/// </summary>
	inline void ClampToWorkArea(RECT const& work, int margin, int& width, int& height)
	{
		// RECT holds LONGs; the arithmetic below is in int, so the four values are
		// converted once rather than letting std::clamp deduce two different types.
		const int availableWidth = (static_cast<int>(work.right) - static_cast<int>(work.left)) - 2 * margin;
		const int availableHeight = (static_cast<int>(work.bottom) - static_cast<int>(work.top)) - 2 * margin;

		if (availableWidth > 0 && width > availableWidth)
			width = availableWidth;

		if (availableHeight > 0 && height > availableHeight)
			height = availableHeight;
	}

	inline void CentreInWorkArea(RECT const& work, int margin, int width, int height, int& x, int& y)
	{
		const int left = static_cast<int>(work.left);
		const int top = static_cast<int>(work.top);
		const int right = static_cast<int>(work.right);
		const int bottom = static_cast<int>(work.bottom);

		x = left + ((right - left) - width) / 2;
		y = top + ((bottom - top) - height) / 2;

		x = std::clamp(x, left + margin, std::max(left + margin, right - width - margin));
		y = std::clamp(y, top + margin, std::max(top + margin, bottom - height - margin));
	}

	/// <summary>
	/// Moves a window to a pixel rectangle and reports the DPI of the monitor it
	/// landed on. GetDpiForWindow describes the monitor a window is currently on,
	/// which for a window that has just been created is not the monitor it is about
	/// to be moved to; both dialogs call this while still hidden, so the intermediate
	/// placement is never visible.
	/// </summary>
	inline UINT MoveToAndGetDpi(Mux::Window const& window, int x, int y, int width, int height)
	{
		MoveContentArea(window, x, y, width, height);
		return GetDpiForWindow(GetWindowHandle(window));
	}

	/// <summary>
	/// Makes a window the real foreground window, and reports whether it is one.
	///
	/// A tray icon callback is a message posted by the shell, so this process did not
	/// receive the last input event - exactly the condition under which Windows
	/// refuses SetForegroundWindow. The foreground lock timeout is cleared for the
	/// duration of the call and restored afterwards, the foreground thread's input
	/// queue is attached, and the result is verified rather than assumed because
	/// Windows ignores the first request often enough to be worth asking twice.
	/// </summary>
	inline bool ForceForeground(Mux::Window const& window)
	{
		const HWND hwnd = GetWindowHandle(window);
		if (hwnd == nullptr)
			return false;

		if (GetForegroundWindow() == hwnd)
			return true;

		const HWND foreground = GetForegroundWindow();
		const DWORD foregroundThread = foreground
			? GetWindowThreadProcessId(foreground, nullptr)
			: 0;
		const DWORD thisThread = GetCurrentThreadId();

		DWORD previousTimeout = 0;
		const bool haveTimeout = SystemParametersInfoW(
			SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &previousTimeout, 0) != FALSE;
		if (haveTimeout)
			SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nullptr, 0);

		// Attaching to the current foreground thread's input queue lifts the
		// foreground lock for the duration of the call as well.
		const bool attached = foregroundThread != 0
			&& foregroundThread != thisThread
			&& AttachThreadInput(foregroundThread, thisThread, TRUE);

		BringWindowToTop(hwnd);
		SetForegroundWindow(hwnd);
		SetActiveWindow(hwnd);
		SetFocus(hwnd);

		if (GetForegroundWindow() != hwnd)
			SetForegroundWindow(hwnd);

		const bool succeeded = GetForegroundWindow() == hwnd;

		if (attached)
			AttachThreadInput(foregroundThread, thisThread, FALSE);

		if (haveTimeout)
		{
			SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
				reinterpret_cast<void*>(static_cast<uintptr_t>(previousTimeout)), 0);
		}

		if (IsTraceEnabled())
		{
			LogTrace(L"foreground hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(hwnd))
				+ L" was=" + std::to_wstring(reinterpret_cast<uintptr_t>(foreground))
				+ L" attached=" + std::to_wstring(attached ? 1 : 0)
				+ L" lockTimeout=" + std::to_wstring(previousTimeout)
				+ L" ok=" + std::to_wstring(succeeded ? 1 : 0));
		}

		return succeeded;
	}

	/// <summary>
	/// Applies the standard borderless, always on top presenter. The window's own
	/// presenter is reconfigured rather than replaced, because replacing it resets the
	/// window frame; the cast is checked first, because calling through a failed cast
	/// crashed the process inside SetBorderAndTitleBar.
	/// </summary>
	inline void ConfigurePopupPresenter(Mux::Window const& window, bool alwaysOnTop = true)
	{
		try
		{
			const auto appWindow = window.AppWindow();
			if (!appWindow)
			{
				LogFailure(L"presenter", L"the window has no AppWindow");
			}
			else if (auto presenter = appWindow.Presenter().try_as<
				winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
			{
				presenter.SetBorderAndTitleBar(false, false);
				presenter.IsResizable(false);
				presenter.IsMaximizable(false);
				presenter.IsMinimizable(false);
				presenter.IsAlwaysOnTop(alwaysOnTop);
			}
			else
			{
				LogFailure(L"presenter", L"the window's presenter is not an OverlappedPresenter");
			}
		}
		catch (winrt::hresult_error const& ex)
		{
			LogFailure(L"presenter",
				std::wstring(L"could not configure the window presenter: ") + ex.message().c_str());
		}
		catch (...)
		{
			LogFailure(L"presenter", L"could not configure the window presenter");
		}

		RemoveNonClientFrame(window);
	}

	inline double Decelerate(double progress)
	{
		return 1.0 - std::pow(1.0 - progress, 3.0);
	}

	inline double Accelerate(double progress)
	{
		return progress * progress;
	}

	/// <summary>
	/// Height the given view wants at the given content width. The hosting window
	/// is sized before it is shown, so views are measured explicitly instead of
	/// relying on hand maintained height constants.
	/// </summary>
	inline double MeasureContentHeight(Mux::FrameworkElement const& view, double widthDip, double fallbackDip)
	{
		if (view == nullptr)
			return fallbackDip;

		// A view that has never been through a layout pass can report a stale
		// desired size, which would size the window a few pixels short and clip
		// the last row. Force one pass first.
		view.UpdateLayout();
		view.Measure(winrt::Windows::Foundation::Size{ static_cast<float>(widthDip), 1.0e6f });

		const double desired = view.DesiredSize().Height;
		return desired > 0.0 ? std::ceil(desired) : fallbackDip;
	}

	struct ContentSize
	{
		double width = 0.0;
		double height = 0.0;
	};

	/// <summary>
	/// The size the given view needs when nothing but the monitor constrains it.
	///
	/// The hosting window is sized exactly to the popup content, so every string in
	/// it - in any language - has to be able to ask for the room it needs; a fixed
	/// width silently cuts the ones that are wider than it. The width is what the
	/// content wants, never below the design width the popup is built around and
	/// never above what the work area leaves; the height is then measured at that
	/// width, because a label that wraps onto a second line needs the window to grow
	/// downwards as well.
	/// </summary>
	inline ContentSize MeasureContent(
		Mux::FrameworkElement const& view, double minWidthDip, double maxWidthDip, double fallbackHeightDip)
	{
		ContentSize size{ minWidthDip, fallbackHeightDip };
		if (view == nullptr)
			return size;

		const double upper = std::max(minWidthDip, maxWidthDip);
		const auto clampWidth = [minWidthDip, upper](double width)
		{
			return std::ceil(std::clamp(width, minWidthDip, upper));
		};

		// Laid out once and then invalidated. The layout pass is what gives a
		// control its template - a ToggleSwitch that has never been laid out reports
		// no width at all - and the invalidation is what makes the unconstrained
		// measure below really happen: the framework skips a measure that is still
		// valid for a larger available size, and the answer would then be the width
		// the window happened to have rather than the width the content needs, which
		// is exactly how the popup used to keep cutting off its longest label.
		view.UpdateLayout();
		view.InvalidateMeasure();

		// Unconstrained in width: this is the widest row of the popup, converted by
		// that row's own measure into what the window has to be.
		view.Measure(winrt::Windows::Foundation::Size{ std::numeric_limits<float>::infinity(), 1.0e6f });

		double width = view.DesiredSize().Width;
		if (!(width > 0.0))
			width = minWidthDip;

		size.width = clampWidth(width);

		// Measured at the width that will really be used. A minimum width the content
		// imposes on itself can be wider than what the text alone asked for, and the
		// height has to be taken at the width the window ends up with - so the size
		// the view reports here is allowed to widen it once more.
		for (int pass = 0; pass < 2; ++pass)
		{
			view.Measure(winrt::Windows::Foundation::Size{
				static_cast<float>(size.width), 1.0e6f });

			const double wanted = view.DesiredSize().Width;
			const double widened = clampWidth(wanted);
			if (widened <= size.width)
				break;

			size.width = widened;
		}

		const double desiredHeight = view.DesiredSize().Height;
		size.height = desiredHeight > 0.0 ? std::ceil(desiredHeight) : fallbackHeightDip;

		return size;
	}

	/// <summary>
	/// A staggered opacity fade for a list of elements, stepped by a frame timer.
	///
	/// It drives <see cref="Mux::UIElement::Opacity"/> rather than a composition
	/// animation on the element visual, because the popup content is hosted inside a
	/// content island where animating that visual's Opacity had no visible effect. The
	/// elements are deliberately not translated: the popup window is sized exactly to
	/// its content, so moving one would clip the outermost rows. The fade always runs
	/// to its target value, so an interrupted or skipped animation cannot leave an
	/// element stuck at a partial opacity.
	/// </summary>
	class StaggeredFade
	{
	public:
		/// <summary>
		/// Stops the frame timer. A started DispatcherQueueTimer is kept alive by the
		/// dispatcher, so without this its Tick handler would outlive the object it
		/// captured and run Advance() against freed members.
		/// </summary>
		~StaggeredFade()
		{
			Stop();
		}

		/// <summary>
		/// Fades <paramref name="elements"/> from <paramref name="from"/> to
		/// <paramref name="to"/>, each one <paramref name="staggerMs"/> after the
		/// previous.
		/// </summary>
		void Start(
			Mux::UIElement const& owner,
			winrt::Windows::Foundation::Collections::IIterable<Mux::UIElement> const& elements,
			int staggerMs,
			int durationMs,
			double from,
			double to)
		{
			// Abandon anything still running: the elements of a rebuilt view are
			// gone, and the new run sets its own start values below.
			Stop();

			m_from = from;
			m_to = to;
			m_durationMs = durationMs > 0 ? durationMs : 1;

			int index = 0;
			for (auto const& element : elements)
			{
				element.Opacity(from);
				m_items.push_back(Item{ element, staggerMs * index });
				++index;
			}

			if (m_items.empty())
				return;

			if (IsTraceEnabled())
			{
				LogTrace(L"entrance start items=" + std::to_wstring(m_items.size())
					+ L" stagger=" + std::to_wstring(staggerMs)
					+ L" duration=" + std::to_wstring(durationMs));
			}

			m_startTick = GetTickCount64();

			if (!m_timer)
			{
				m_timer = owner.DispatcherQueue().CreateTimer();
				m_timer.Interval(winrt::Windows::Foundation::TimeSpan{
					std::chrono::milliseconds(kFrameMs) });
				m_timer.IsRepeating(true);
				m_timer.Tick([this](auto&&, auto&&) { Advance(); });
			}

			m_timer.Start();
		}

		/// <summary>
		/// Jumps every element to the target value and stops. Used both when a run
		/// finishes and when a new run supersedes it.
		/// </summary>
		void Stop()
		{
			if (m_timer)
				m_timer.Stop();

			if (!m_items.empty() && IsTraceEnabled())
			{
				LogTrace(L"entrance done items=" + std::to_wstring(m_items.size())
					+ L" elapsed=" + std::to_wstring(GetTickCount64() - m_startTick));
			}

			for (auto const& item : m_items)
				item.element.Opacity(m_to);

			m_items.clear();
		}

	private:
		struct Item
		{
			Mux::UIElement element{ nullptr };
			int delayMs{ 0 };
		};

		static constexpr int kFrameMs = 15;

		void Advance()
		{
			const uint64_t elapsed = GetTickCount64() - m_startTick;
			bool running = false;

			for (auto const& item : m_items)
			{
				const uint64_t begin = static_cast<uint64_t>(item.delayMs);
				if (elapsed <= begin)
				{
					running = true;
					continue;
				}

				const double progress =
					static_cast<double>(elapsed - begin) / m_durationMs;
				if (progress >= 1.0)
					continue;

				item.element.Opacity(m_from + (m_to - m_from) * Decelerate(progress));
				running = true;
			}

			if (!running)
				Stop();
		}

		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
		std::vector<Item> m_items{};
		double m_from{ 1.0 };
		double m_to{ 1.0 };
		int m_durationMs{ 1 };
		uint64_t m_startTick{ 0 };
	};

	inline winrt::hstring StateText(DeviceState state)
	{
		switch (state)
		{
		case DeviceState::Connected:    return _(L"Connected");
		case DeviceState::Connecting:   return _(L"Connecting");
		case DeviceState::Failed:       return _(L"Failed");
		case DeviceState::Disconnected: return _(L"Not connected");
		}
		return L"";
	}
}
