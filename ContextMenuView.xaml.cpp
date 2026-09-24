#include "pch.h"
#include "ContextMenuView.xaml.h"

#if __has_include("ContextMenuView.g.cpp")
#include "ContextMenuView.g.cpp"
#endif

#include "AppSettings.h"
#include "AppShell.h"
#include "I18n.hpp"
#include "StartupRegistration.h"
#include "Util.hpp"
#include "ViewHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace ::AudioPlaybackConnectorWinUI;

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	namespace
	{
		constexpr wchar_t kGlyphSettings[] = L"\uE713";
		constexpr wchar_t kGlyphInfo[] = L"\uE946";
		constexpr wchar_t kGlyphExit[] = L"\uE8BB";

		constexpr double kRowHeight = 40;
		constexpr double kGutterWidth = 28;

		/// <summary>
		/// The width a settings switch occupies in this theme.
		///
		/// A card's width decides the popup's width, and the popup is sized to its
		/// content, so the card's own measurement has to include the switch. A control
		/// gets its width from its template and a ToggleSwitch that has never been
		/// laid out reports no width at all, which would size the window exactly one
		/// switch too narrow on the first build and wrap the longest label. This is
		/// the width the switch's template gives it, set as a minimum so that the
		/// measurement can never be short of it - and so it still grows if a future
		/// template is wider.
		/// </summary>
		constexpr double kSwitchWidth = 52;

		/// <summary>
		/// The width a freshly built row needs, measured while it has never been
		/// through a layout pass. That is what the popup is widened to fit: a
		/// TextBlock that has already been laid out answers with the width it was
		/// last wrapped at rather than with the width its text needs, and a menu
		/// measured that way can never grow to fit its longest label and cuts it off
		/// instead.
		/// </summary>
		double NaturalWidth(FrameworkElement const& element)
		{
			if (element == nullptr)
				return 0.0;

			element.Measure(winrt::Windows::Foundation::Size{
				std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() });

			const double width = element.DesiredSize().Width;
			return width > 0.0 ? width : 0.0;
		}
	}

	ContextMenuView::ContextMenuView()
	{
		InitializeComponent();
	}

	void ContextMenuView::FocusFirstItem()
	{
		if (m_firstItem)
			m_firstItem.Focus(FocusState::Programmatic);
	}

	void ContextMenuView::PlayEntranceAnimation()
	{
		if (IsTraceEnabled())
		{
			// Layout geometry, for checking the spacing rhythm: the offset of each
			// child inside the panel, relative to the panel itself.
			int index = 0;
			for (auto const& child : ItemPanel().Children())
			{
				if (auto element = child.try_as<FrameworkElement>())
				{
					const auto point = element.TransformToVisual(ItemPanel()).TransformPoint({ 0.0f, 0.0f });
					LogTrace(L"menu item " + std::to_wstring(index)
						+ L" y=" + std::to_wstring(static_cast<int>(point.Y))
						+ L" h=" + std::to_wstring(static_cast<int>(element.ActualHeight()))
						+ L" w=" + std::to_wstring(static_cast<int>(element.ActualWidth())));
				}
				++index;
			}
		}

		if (!AreSystemAnimationsEnabled())
			return;

		// A short stagger, tighter than the device panel: a menu is expected to be
		// ready almost immediately.
		m_entrance.Start(ItemPanel(), ItemPanel().Children(), 25, 200, 0.0, 1.0);
	}

	void ContextMenuView::AddCard(hstring const& label, Control const& control)
	{
		Grid layout;
		ColumnDefinition labelColumn;
		labelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
		ColumnDefinition controlColumn;
		controlColumn.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
		layout.ColumnDefinitions().Append(labelColumn);
		layout.ColumnDefinitions().Append(controlColumn);
		layout.ColumnSpacing(12);

		auto text = ViewHelpers::MakeText(label, 13);
		text.Foreground(m_palette.primary);
		text.VerticalAlignment(VerticalAlignment::Center);
		// Wraps only when the work area caps the popup's width; the tooltip carries
		// the whole string as well.
		text.TextWrapping(TextWrapping::Wrap);
		text.TextTrimming(TextTrimming::None);
		ToolTipService::SetToolTip(text, winrt::box_value(label));
		layout.Children().Append(text);

		control.VerticalAlignment(VerticalAlignment::Center);
		control.HorizontalAlignment(HorizontalAlignment::Right);
		layout.Children().Append(control);
		Grid::SetColumn(control, 1);

		Border card;
		card.CornerRadius(ViewHelpers::CornerRadius(8));
		card.BorderThickness(Thickness{ 1, 1, 1, 1 });
		card.Padding(Thickness{ 12, 10, 12, 10 });
		card.Background(m_palette.cardBackground);
		card.BorderBrush(m_palette.cardBorder);
		card.Child(layout);

		ItemPanel().Children().Append(card);
	}

	Button ContextMenuView::AddRow(
		Panel const& host,
		wchar_t const* glyph,
		bool accentGlyph,
		hstring const& text,
		Brush const& textBrush,
		std::function<void()> onClick)
	{
		Grid content;
		ColumnDefinition gutterColumn;
		gutterColumn.Width(GridLengthHelper::FromValueAndType(kGutterWidth, GridUnitType::Pixel));
		ColumnDefinition labelColumn;
		labelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
		content.ColumnDefinitions().Append(gutterColumn);
		content.ColumnDefinitions().Append(labelColumn);

		if (glyph != nullptr)
		{
			auto icon = ViewHelpers::MakeGlyph(glyph, 14, accentGlyph ? m_palette.accent : m_palette.secondary);
			icon.HorizontalAlignment(HorizontalAlignment::Center);
			icon.VerticalAlignment(VerticalAlignment::Center);
			content.Children().Append(icon);
		}

		auto label = ViewHelpers::MakeText(text, 14);
		label.Foreground(textBrush);
		label.TextWrapping(TextWrapping::Wrap);
		label.TextTrimming(TextTrimming::None);
		ToolTipService::SetToolTip(label, winrt::box_value(text));
		content.Children().Append(label);
		Grid::SetColumn(label, 1);

		Button button;
		button.Content(content);
		button.HorizontalAlignment(HorizontalAlignment::Stretch);
		button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
		button.VerticalContentAlignment(VerticalAlignment::Center);
		button.MinHeight(kRowHeight);
		button.Padding(Thickness{ 12, 6, 12, 6 });
		button.Margin(Thickness{ 0, 0, 0, 0 });
		button.Background(ViewHelpers::Transparent());
		button.BorderThickness(Thickness{ 0, 0, 0, 0 });
		button.CornerRadius(ViewHelpers::CornerRadius(4));

		// The label lives in a child grid, so the name is set explicitly.
		Automation::AutomationProperties::SetName(button, text);

		if (onClick)
			button.Click([onClick, text](IInspectable const&, RoutedEventArgs const&)
				{
					LogTrace(L"row click " + std::wstring(text));
					onClick();
				});

		if (!m_firstItem)
			m_firstItem = button;

		host.Children().Append(button);
		return button;
	}

	Button ContextMenuView::AddRow(wchar_t const* glyph, bool accentGlyph, hstring const& text, std::function<void()> onClick)
	{
		return AddRow(ItemPanel(), glyph, accentGlyph, text, m_palette.primary, std::move(onClick));
	}


	void ContextMenuView::Refresh()
	{
		m_palette = Palette{
			ProbePrimaryText().BorderBrush(),
			ProbeSecondaryText().BorderBrush(),
			ProbeAccentText().BorderBrush(),
			ProbeCardBackground().Background(),
			ProbeCardBorder().BorderBrush(),
		};

		ItemPanel().Children().Clear();
		m_firstItem = nullptr;

		if (IsTraceEnabled())
		{
			LogTrace(L"palette menu loaded=" + std::to_wstring(IsLoaded() ? 1 : 0)
				+ L" theme=" + std::to_wstring(static_cast<int>(ActualTheme()))
				+ L" primary=" + ViewHelpers::BrushText(m_palette.primary)
				+ L" secondary=" + ViewHelpers::BrushText(m_palette.secondary));
		}

		auto const& settings = Settings();

		// ---- keep the receiver ready: a settings card with a switch ----
		{
			ToggleSwitch allowSwitch;
			allowSwitch.MinWidth(kSwitchWidth);
			allowSwitch.OnContent(nullptr);
			allowSwitch.OffContent(nullptr);
			allowSwitch.IsOn(settings.allowPaired);
			Automation::AutomationProperties::SetAutomationId(allowSwitch, L"AllowPairedSwitch");
			Automation::AutomationProperties::SetName(allowSwitch, _(L"Allow paired devices to connect"));
			allowSwitch.Toggled([](IInspectable const& sender, RoutedEventArgs const&)
				{
					const auto toggle = sender.try_as<ToggleSwitch>();
					if (toggle && Shell().setAllowPaired)
						Shell().setAllowPaired(toggle.IsOn());
				});

			AddCard(_(L"Allow paired devices to connect"), allowSwitch);
			m_firstItem = allowSwitch;
		}

		// ---- notifications: a settings card with a switch ----
		{
			ToggleSwitch notificationsSwitch;
			notificationsSwitch.MinWidth(kSwitchWidth);
			notificationsSwitch.OnContent(nullptr);
			notificationsSwitch.OffContent(nullptr);
			notificationsSwitch.IsOn(settings.notifications);
			Automation::AutomationProperties::SetAutomationId(notificationsSwitch, L"NotificationsSwitch");
			Automation::AutomationProperties::SetName(notificationsSwitch, _(L"Notifications"));
			notificationsSwitch.Toggled([](IInspectable const& sender, RoutedEventArgs const&)
				{
					const auto toggle = sender.try_as<ToggleSwitch>();
					if (toggle && Shell().setNotifications)
						Shell().setNotifications(toggle.IsOn());
				});

			AddCard(_(L"Notifications"), notificationsSwitch);
		}

		// ---- start with Windows: a settings card with a switch ----
		{
			ToggleSwitch startupSwitch;
			startupSwitch.MinWidth(kSwitchWidth);
			startupSwitch.OnContent(nullptr);
			startupSwitch.OffContent(nullptr);
			// The registry is the state, not a setting of its own.
			startupSwitch.IsOn(IsStartWithWindowsEnabled());
			Automation::AutomationProperties::SetAutomationId(startupSwitch, L"StartupSwitch");
			Automation::AutomationProperties::SetName(startupSwitch, _(L"Start with Windows"));
			startupSwitch.Toggled([](IInspectable const& sender, RoutedEventArgs const&)
				{
					const auto toggle = sender.try_as<ToggleSwitch>();
					if (toggle && Shell().setStartWithWindows)
						Shell().setStartWithWindows(toggle.IsOn());
				});

			AddCard(_(L"Start with Windows"), startupSwitch);
		}

		// ---- theme: a settings card with a drop-down ----
		{
			struct ThemeOption
			{
				ThemeMode mode;
				hstring label;
			};

			const std::vector<ThemeOption> options{
				{ ThemeMode::System, _(L"System") },
				{ ThemeMode::Light,  _(L"Light") },
				{ ThemeMode::Dark,   _(L"Dark") },
			};

			ComboBox box;
			box.MinWidth(120);
			box.HorizontalAlignment(HorizontalAlignment::Right);

			int selected = 0;
			for (int index = 0; index < static_cast<int>(options.size()); ++index)
			{
				ComboBoxItem item;
				item.Content(winrt::box_value(options[index].label));
				Automation::AutomationProperties::SetAutomationId(
					item, L"ThemeComboItem" + std::to_wstring(index));
				box.Items().Append(item);

				if (options[index].mode == settings.theme)
					selected = index;
			}

			// Set before subscribing: assigning the selection raises the event.
			box.SelectedIndex(selected);
			Automation::AutomationProperties::SetAutomationId(box, L"ThemeCombo");
			Automation::AutomationProperties::SetName(box, _(L"Theme"));
			box.SelectionChanged([options](IInspectable const& sender, SelectionChangedEventArgs const&)
				{
					const auto combo = sender.try_as<ComboBox>();
					if (!combo || !Shell().setTheme)
						return;

					const int index = combo.SelectedIndex();
					if (index >= 0 && index < static_cast<int>(options.size()))
						Shell().setTheme(options[index].mode);
				});

			AddCard(_(L"Theme"), box);
		}

		// ---- language: a settings card with a drop-down ----
		{
			struct LanguageOption
			{
				std::wstring_view id;
				hstring label;
			};

			const std::vector<LanguageOption> options{
				{ kLanguageSystem,             _(L"System") },
				{ kLanguageSimplifiedChinese,  _(L"Chinese (Simplified)") },
				{ kLanguageTraditionalChinese, _(L"Chinese (Traditional)") },
				{ kLanguageEnglish,            _(L"English") },
			};

			ComboBox box;
			box.MinWidth(120);
			box.HorizontalAlignment(HorizontalAlignment::Right);

			int selected = 0;
			for (int index = 0; index < static_cast<int>(options.size()); ++index)
			{
				ComboBoxItem item;
				item.Content(winrt::box_value(options[index].label));
				Automation::AutomationProperties::SetAutomationId(
					item, L"LanguageComboItem" + std::to_wstring(index));
				box.Items().Append(item);

				if (std::wstring_view(settings.language) == options[index].id)
					selected = index;
			}

			// Set before subscribing: assigning the selection raises the event.
			box.SelectedIndex(selected);
			Automation::AutomationProperties::SetAutomationId(box, L"LanguageCombo");
			Automation::AutomationProperties::SetName(box, _(L"Language"));
			box.SelectionChanged([options](IInspectable const& sender, SelectionChangedEventArgs const&)
				{
					const auto combo = sender.try_as<ComboBox>();
					if (!combo || !Shell().setLanguage)
						return;

					const int index = combo.SelectedIndex();
					if (index >= 0 && index < static_cast<int>(options.size()))
						Shell().setLanguage(std::wstring(options[index].id));
				});

			AddCard(_(L"Language"), box);
		}

		// ---- the remaining commands ----
		auto bluetoothRow = AddRow(
			kGlyphSettings,
			false,
			_(L"Bluetooth Settings"),
			[]
			{
				if (Shell().openBluetoothSettings)
					Shell().openBluetoothSettings();
			});
		Automation::AutomationProperties::SetAutomationId(bluetoothRow, L"BluetoothItem");

		auto licensesRow = AddRow(
			kGlyphInfo,
			false,
			_(L"Licenses"),
			[]
			{
				if (Shell().showLicenses)
					Shell().showLicenses();
			});
		Automation::AutomationProperties::SetAutomationId(licensesRow, L"LicensesItem");

		auto exitRow = AddRow(
			kGlyphExit,
			false,
			_(L"Exit"),
			[]
			{
				if (Shell().requestExit)
					Shell().requestExit();
			});
		Automation::AutomationProperties::SetAutomationId(exitRow, L"ExitItem");

		// ---- the width the window has to be ----
		//
		// Every row was built just above and has not been laid out yet, so each one
		// still reports the width it wants rather than the one it was last given.
		double required = 0.0;
		for (auto const& child : ItemPanel().Children())
		{
			required = std::max(required, NaturalWidth(child.try_as<FrameworkElement>()));
		}

		ItemPanel().MinWidth(std::ceil(required));

		if (IsTraceEnabled())
		{
			LogTrace(L"menu content asks for a width of " + std::to_wstring(static_cast<int>(required))
				+ L" dip over " + std::to_wstring(ItemPanel().Children().Size()) + L" item(s)");
		}
	}
}

