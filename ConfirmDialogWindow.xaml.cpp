#include "pch.h"
#include "ConfirmDialogWindow.xaml.h"

#if __has_include("ConfirmDialogWindow.g.cpp")
#include "ConfirmDialogWindow.g.cpp"
#endif

#include "I18n.hpp"
#include "Util.hpp"
#include "ViewHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace ::AudioPlaybackConnectorWinUI;

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	namespace
	{
		constexpr double kDialogWidth = 420;
		constexpr double kFallbackHeight = 180;
		constexpr double kScreenMargin = 8;
	}

	ConfirmDialogWindow::ConfirmDialogWindow()
	{
		InitializeComponent();

		Title(L"AudioPlaybackConnectorWinUI");

		// A confirmation is a normal transient dialog: it takes the foreground
		// when it opens but does not float above whatever the user switches to.
		ViewHelpers::ConfigurePopupPresenter(*this, false);
		AppWindow().IsShownInSwitchers(false);
		// Alt+F4 must not close this window: it is reused, and a closed WinUI window
		// cannot be shown again - the next Show() would throw from inside the tray
		// callback that asked for it. Hiding instead keeps it usable.
		AppWindow().Closing([](winrt::Microsoft::UI::Windowing::AppWindow const& sender,
			winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
		{
			args.Cancel(true);
			sender.Hide();
		});

		// The confirm button's text is the caller's, set in Show(): this window is
		// only ever reached through Show(), which overwrites anything put here, so a
		// literal assigned at construction is dead and misleads the reader about
		// which string the user actually sees.
		CancelButton().Content(winrt::box_value(hstring(_(L"Cancel"))));

		Automation::AutomationProperties::SetAutomationId(CancelButton(), L"ConfirmDialogCancel");
		Automation::AutomationProperties::SetAutomationId(ConfirmButton(), L"ConfirmDialogConfirm");

		CancelButton().Click([this](IInspectable const&, RoutedEventArgs const&) { Dismiss(); });

		ConfirmButton().Click([this](IInspectable const&, RoutedEventArgs const&)
			{
				auto action = std::move(m_onConfirm);
				m_onConfirm = nullptr;

				Dismiss();

				if (action)
					action();
			});

		auto resources = Application::Current().Resources();
		auto key = winrt::box_value(L"AccentButtonStyle");
		if (resources && resources.HasKey(key))
			ConfirmButton().Style(resources.Lookup(key).as<Style>());

		Content().KeyDown([this](IInspectable const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
			{
				if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
				{
					Dismiss();
					args.Handled(true);
				}
			});
	}

	void ConfirmDialogWindow::Dismiss()
	{
		AppWindow().Hide();
	}

	void ConfirmDialogWindow::Show(
		hstring const& title,
		hstring const& message,
		hstring const& confirmLabel,
		ElementTheme theme,
		std::function<void()> onConfirm)
	{
		// The dialog is its own window, so it inherits nothing from the popup - not the
		// theme and not the material. Both are applied here, through the same helper
		// the popup uses, so a confirmation is painted like the window it came from.
		RootGrid().RequestedTheme(theme);

		if (IsTraceEnabled())
		{
			LogTrace(L"confirmation theme=" + std::to_wstring(static_cast<int>(RootGrid().ActualTheme()))
				+ L" message=" + ViewHelpers::BrushText(MessageText().Foreground()));
		}

		TitleText().Text(title);
		MessageText().Text(message);
		ConfirmButton().Content(winrt::box_value(confirmLabel));
		m_onConfirm = std::move(onConfirm);

		const HWND hwnd = ViewHelpers::GetWindowHandle(*this);

		// Centre on the monitor the pointer is on, which is the one the user was
		// interacting with.
		POINT cursor{};
		GetCursorPos(&cursor);
		const RECT work = ViewHelpers::WorkAreaForPoint(cursor);

		const double contentDip =
			ViewHelpers::MeasureContentHeight(Content(), kDialogWidth, kFallbackHeight);

		const UINT creationDpi = GetDpiForWindow(hwnd);
		int width = ViewHelpers::ScaleToPixels(kDialogWidth, creationDpi);
		int height = ViewHelpers::ScaleToPixels(contentDip, creationDpi);
		int margin = ViewHelpers::ScaleToPixels(kScreenMargin, creationDpi);
		int x = 0;
		int y = 0;

		ViewHelpers::CentreInWorkArea(work, margin, width, height, x, y);
		const UINT dpi = ViewHelpers::MoveToAndGetDpi(*this, x, y, width, height);

		margin = ViewHelpers::ScaleToPixels(kScreenMargin, dpi);
		width = ViewHelpers::ScaleToPixels(kDialogWidth, dpi);
		height = ViewHelpers::ScaleToPixels(contentDip, dpi);

		// Only the width gives way, and the height is measured again at that width: the
		// first measurement was taken at kDialogWidth, and a narrower dialog wraps the
		// message into more lines. Keeping the height the wider measurement produced is
		// what cuts the button row off - there is nothing here to scroll with.
		const int availableWidth =
			(static_cast<int>(work.right) - static_cast<int>(work.left)) - 2 * margin;
		if (availableWidth > 0 && width > availableWidth)
		{
			width = availableWidth;

			const double fittedDip =
				static_cast<double>(width) * 96.0 / static_cast<double>(dpi);
			height = ViewHelpers::ScaleToPixels(
				ViewHelpers::MeasureContentHeight(Content(), fittedDip, kFallbackHeight), dpi);
		}

		// Height last, because the re-measure above can only make it taller. A window
		// taller than the work area cannot be brought on screen by moving it, and
		// CentreInWorkArea would pin it to the top and let the button row - the last
		// child - hang off the bottom. The licence dialog clamps for the same reason;
		// it has a ScrollViewer behind its text, this one does not, so the part that
		// fits is what the user gets.
		ViewHelpers::ClampToWorkArea(work, margin, width, height);

		ViewHelpers::CentreInWorkArea(work, margin, width, height, x, y);
		ViewHelpers::MoveContentArea(*this, x, y, width, height);

		AppWindow().Show();
		Activate();
		ViewHelpers::ForceForeground(*this);

		if (IsTraceEnabled())
		{
			RECT rect{};
			GetWindowRect(hwnd, &rect);
			LogTrace(L"confirmation shown at " + std::to_wstring(rect.left) + L","
				+ std::to_wstring(rect.top) + L" "
				+ std::to_wstring(rect.right - rect.left) + L"x"
				+ std::to_wstring(rect.bottom - rect.top));
		}

		CancelButton().Focus(FocusState::Programmatic);
	}
}
