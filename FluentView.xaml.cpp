#include "pch.h"
#include "FluentView.xaml.h"

#if __has_include("FluentView.g.cpp")
#include "FluentView.g.cpp"
#endif

#include "AppShell.h"
#include "AudioPlaybackService.h"
#include "DeviceIcon.hpp"
#include "I18n.hpp"
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
				constexpr wchar_t kGlyphBluetooth[] = L"\uE702";
		constexpr wchar_t kGlyphRemove[] = L"\uE74D";
				constexpr wchar_t kGlyphMore[] = L"\uE712";

		constexpr double kCardRadius = 8;
		constexpr double kBadgeSize = 38;
				constexpr double kDeviceIconSize = 20;

		/// <summary>
		/// The badge content: the icon Windows gives the device, or a generic
		/// Bluetooth glyph when it published none.
		/// </summary>
		UIElement MakeDeviceIcon(std::wstring const& icon, Windows::UI::Color const& tint)
		{
			// Cached per (resource, tint): the panel is rebuilt on every device-list
			// change and rendering one costs ExtractIconExW plus a per-pixel pass. There
			// are at most a couple of combinations per device, and this runs on the UI
			// thread only.
			const uint32_t rgba = (static_cast<uint32_t>(tint.A) << 24)
				| (static_cast<uint32_t>(tint.R) << 16)
				| (static_cast<uint32_t>(tint.G) << 8)
				| static_cast<uint32_t>(tint.B);
			const std::wstring key = icon + L"|" + std::to_wstring(rgba);

			static std::unordered_map<std::wstring, winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap> cache;
			if (auto const cached = cache.find(key); cached != cache.end())
			{
				Image hit;
				hit.Source(cached->second);
				hit.Width(kDeviceIconSize);
				hit.Height(kDeviceIconSize);
				return hit;
			}

			if (auto const bitmap = DeviceIcon::Render(icon, tint))
			{
				cache.emplace(key, bitmap);

				Image image;
				image.Source(bitmap);
				image.Width(kDeviceIconSize);
				image.Height(kDeviceIconSize);
				return image;
			}

			return ViewHelpers::MakeGlyph(kGlyphBluetooth, 18, SolidColorBrush(tint));
		}

		/// <summary>Brushes read once per rebuild from the view's palette probe.</summary>
		struct Palette
		{
			Brush cardBackground;
			Brush cardBorder;
			Brush secondary;
			Brush tertiary;
			Brush accentText;
			Brush accentFill;
			Brush error;
			Brush transparent;
		};

				Border MakeCard(Palette const& palette)
		{
			Border card;
			card.CornerRadius(ViewHelpers::CornerRadius(kCardRadius));
			card.BorderThickness(Thickness{ 1, 1, 1, 1 });
			card.Padding(Thickness{ 14, 12, 14, 12 });
			card.Background(palette.cardBackground);
			card.BorderBrush(palette.cardBorder);
			return card;
		}
	}

	FluentView::FluentView()
	{
		InitializeComponent();

		PairButton().Click([](IInspectable const&, RoutedEventArgs const&)
			{
				if (Shell().openBluetoothSettings)
					Shell().openBluetoothSettings();
			});

		Automation::AutomationProperties::SetAutomationId(PairButton(), L"PairButton");

		// Assigned in code so that a failure to load the SVG is visible in the trace.
		AppIconImage().ImageOpened([](auto&&, auto&&) { LogTrace(L"badge icon loaded"); });
		AppIconImage().ImageFailed([](auto&&, auto&&) { LogTrace(L"badge icon FAILED to load"); });
		AppIconImage().Source(winrt::Microsoft::UI::Xaml::Media::Imaging::SvgImageSource(
			winrt::Windows::Foundation::Uri(L"ms-appx:///AudioPlaybackConnector.Tile.svg")));
	}

	void FluentView::PlayEntranceAnimation()
	{
		if (!AreSystemAnimationsEnabled())
			return;

		m_entrance.Start(DevicePanel(), DevicePanel().Children(), 40, 220, 0.0, 1.0);
	}

	void FluentView::CollapseExpandedSubmenu()
	{
		if (m_expandedDetails)
			m_expandedDetails.Visibility(Visibility::Collapsed);

		m_expandedDetails = nullptr;
		m_expandedDeviceId.clear();
	}

	void FluentView::Refresh()
	{
		auto const& devices = Playback().Devices();

		// Which submenu was open is remembered by id, not by element, so it survives
		// the rebuild below.
		m_expandedDetails = nullptr;

		Palette palette{
			ProbeCardBackground().Background(),
			ProbeCardBorder().BorderBrush(),
			ProbeSecondaryText().BorderBrush(),
			ProbeTertiaryText().BorderBrush(),
			ProbeAccentText().BorderBrush(),
			ProbeAccentFill().Background(),
			ProbeError().BorderBrush(),
			ViewHelpers::Transparent(),
		};

		// ---- header -------------------------------------------------------
		if (IsTraceEnabled())
		{
			LogTrace(L"palette panel loaded=" + std::to_wstring(IsLoaded() ? 1 : 0)
				+ L" theme=" + std::to_wstring(static_cast<int>(ActualTheme()))
				+ L" card=" + ViewHelpers::BrushText(palette.cardBackground)
				+ L" secondary=" + ViewHelpers::BrushText(palette.secondary)
				+ L" accent=" + ViewHelpers::BrushText(palette.accentText)
				+ L" connectedGlyph=" + ViewHelpers::BrushText(ViewHelpers::White()));
		}

		TitleText().Text(_(L"AudioPlaybackConnectorWinUI"));
		SubtitleText().Text(_(L"Bluetooth audio playback (A2DP Sink)"));
		SubtitleText().Foreground(palette.secondary);

		VersionText().Text(hstring{ GetAppVersion() });
		VersionText().Foreground(palette.tertiary);
		Automation::AutomationProperties::SetAutomationId(VersionText(), L"VersionText");

		DevicesHeaderText().Text(_(L"Audio devices"));
		DevicesHeaderText().Foreground(palette.tertiary);

		PairButtonText().Text(_(L"Pair a new device"));
		PairButton().Foreground(palette.accentText);
		PairButton().Background(palette.cardBackground);
		PairButton().BorderBrush(palette.cardBorder);
		Automation::AutomationProperties::SetName(PairButton(), _(L"Pair a new device"));

		// ---- devices ------------------------------------------------------
		DevicePanel().Children().Clear();

		if (devices.empty())
		{
			EmptyStateText().Text(_(L"No Bluetooth audio devices found. Add one in Bluetooth settings first."));
			EmptyStateText().Foreground(palette.tertiary);
			EmptyStateText().Visibility(Visibility::Visible);
			return;
		}

		EmptyStateText().Visibility(Visibility::Collapsed);

		for (int index = 0; index < static_cast<int>(devices.size()); ++index)
		{
			auto const& device = devices[index];
			hstring deviceId{ device.id };
			hstring displayName{ device.name.empty() ? device.id : device.name };
			const bool connected = Playback().IsConnected(device.id);

			const bool failed = device.state == DeviceState::Failed;
			const bool connecting = device.state == DeviceState::Connecting;

			auto card = MakeCard(palette);

			// ---- the device's own submenu, revealed by its ellipsis button ----
			StackPanel details;
			details.Spacing(6);
			details.Visibility(Visibility::Collapsed);

			// ---- reconnect this device on the next start ----
			{
				Grid setting;
				ColumnDefinition labelColumn;
				labelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
				ColumnDefinition switchColumn;
				switchColumn.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
				setting.ColumnDefinitions().Append(labelColumn);
				setting.ColumnDefinitions().Append(switchColumn);
				setting.ColumnSpacing(12);

				hstring const reconnectText = _(L"Reconnect on next start");

				auto label = ViewHelpers::MakeText(reconnectText, 13);
				label.Foreground(palette.secondary);
				label.TextWrapping(TextWrapping::Wrap);
				label.TextTrimming(TextTrimming::None);
				ToolTipService::SetToolTip(label, winrt::box_value(reconnectText));
				setting.Children().Append(label);

				ToggleSwitch reconnectSwitch;
				reconnectSwitch.MinWidth(0);
				reconnectSwitch.OnContent(nullptr);
				reconnectSwitch.OffContent(nullptr);
				reconnectSwitch.IsOn(IsReconnectEnabled(device.id));
				reconnectSwitch.VerticalAlignment(VerticalAlignment::Center);
				Automation::AutomationProperties::SetAutomationId(
					reconnectSwitch, L"DeviceReconnectSwitch" + std::to_wstring(index));
				Automation::AutomationProperties::SetName(
					reconnectSwitch, displayName + L" " + reconnectText);
				// The switch is read from the sender, not captured: capturing the
				// control it is registered on is a reference cycle.
				reconnectSwitch.Toggled([deviceId](IInspectable const& sender, RoutedEventArgs const&)
					{
						const auto toggle = sender.try_as<ToggleSwitch>();
						if (toggle && Shell().setReconnectDevice)
							Shell().setReconnectDevice(std::wstring(deviceId), toggle.IsOn());
					});

				setting.Children().Append(reconnectSwitch);
				Grid::SetColumn(reconnectSwitch, 1);

				details.Children().Append(setting);
			}

			// ---- remove this device ----
			{
				// The glyph and its label are one centred unit in the row: the colour
				// stays the theme's critical brush rather than the panel's own, so the
				// destructive row is legible in both themes.
				hstring const removeText = _(L"Remove device");

				StackPanel content;
				content.Orientation(Orientation::Horizontal);
				content.Spacing(6);
				content.HorizontalAlignment(HorizontalAlignment::Center);
				content.VerticalAlignment(VerticalAlignment::Center);

				auto glyph = ViewHelpers::MakeGlyph(kGlyphRemove, 14, palette.error);
				glyph.VerticalAlignment(VerticalAlignment::Center);
				content.Children().Append(glyph);

				auto deleteLabel = ViewHelpers::MakeText(removeText, 14);
				deleteLabel.Foreground(palette.error);
				deleteLabel.TextWrapping(TextWrapping::Wrap);
				deleteLabel.TextTrimming(TextTrimming::None);
				ToolTipService::SetToolTip(deleteLabel, winrt::box_value(removeText));
				content.Children().Append(deleteLabel);

				Button remove;
				remove.Content(content);
				remove.HorizontalAlignment(HorizontalAlignment::Stretch);
				remove.HorizontalContentAlignment(HorizontalAlignment::Center);
				remove.VerticalContentAlignment(VerticalAlignment::Center);
				remove.MinHeight(36);
				remove.Padding(Thickness{ 8, 4, 8, 4 });
				remove.Background(palette.transparent);
				remove.BorderThickness(Thickness{ 0, 0, 0, 0 });
				remove.CornerRadius(ViewHelpers::CornerRadius(4));
				Automation::AutomationProperties::SetName(remove, displayName + L" " + removeText);
				Automation::AutomationProperties::SetAutomationId(
					remove, L"DeviceDeleteItem" + std::to_wstring(index));
				remove.Click([this, deviceId, displayName](IInspectable const&, RoutedEventArgs const&)
					{
						DispatcherQueue().TryEnqueue([deviceId, displayName]
							{
								if (Shell().removeDevice)
									Shell().removeDevice(std::wstring(deviceId), std::wstring(displayName));
							});
					});

				details.Children().Append(remove);
			}

			const bool expanded = !m_expandedDeviceId.empty() && m_expandedDeviceId == device.id;
			if (expanded)
			{
				details.Visibility(Visibility::Visible);
				m_expandedDetails = details;
			}

			Grid row;
			ColumnDefinition badgeColumn;
			badgeColumn.Width(GridLengthHelper::Auto());
			ColumnDefinition textColumn;
			textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
			ColumnDefinition actionColumn;
			actionColumn.Width(GridLengthHelper::Auto());
			row.ColumnDefinitions().Append(badgeColumn);
			row.ColumnDefinitions().Append(textColumn);
			row.ColumnDefinitions().Append(actionColumn);
			row.ColumnSpacing(12);

			Border badge;
			badge.Width(kBadgeSize);
			badge.Height(kBadgeSize);
			badge.CornerRadius(ViewHelpers::CornerRadius(kCardRadius));
			badge.Background(connected ? palette.accentFill : palette.transparent);
			badge.BorderThickness(Thickness{ 1, 1, 1, 1 });
			badge.BorderBrush(connected ? palette.transparent : palette.cardBorder);
			badge.VerticalAlignment(VerticalAlignment::Center);
			const auto tint = connected
				? winrt::Microsoft::UI::Colors::White()
				: ViewHelpers::ColorOf(palette.secondary);
			badge.Child(MakeDeviceIcon(device.icon, tint));

			StackPanel text;
			text.Spacing(2);
			text.VerticalAlignment(VerticalAlignment::Center);

			auto name = ViewHelpers::MakeText(displayName, 14);
			name.TextTrimming(TextTrimming::CharacterEllipsis);
			text.Children().Append(name);

			auto status = ViewHelpers::MakeText(
				failed && !device.message.empty() ? hstring{ device.message } : ViewHelpers::StateText(device.state),
				12);
			status.Foreground(connected ? palette.accentText : (failed ? palette.error : palette.tertiary));
			status.TextWrapping(TextWrapping::Wrap);
			text.Children().Append(status);

			row.Children().Append(badge);
			row.Children().Append(text);
			Grid::SetColumn(text, 1);

			// The ellipsis is on the row whatever the connection is doing: a device
			// that is still connecting can still be asked to come back, or removed.
			StackPanel actions;
			actions.Orientation(Orientation::Horizontal);
			actions.Spacing(4);
			actions.VerticalAlignment(VerticalAlignment::Center);

			if (connecting)
			{
				auto spinner = ViewHelpers::MakeSpinner(20);
				spinner.VerticalAlignment(VerticalAlignment::Center);
				actions.Children().Append(spinner);
			}
			else
			{
				Button action;
				action.VerticalAlignment(VerticalAlignment::Center);
				action.MinWidth(84);
				Automation::AutomationProperties::SetAutomationId(action, L"DeviceActionButton");

				if (connected)
				{
					action.Content(winrt::box_value(hstring(_(L"Disconnect"))));
					// Named with the device: a screen reader would otherwise announce
					// a row of identical buttons.
					Automation::AutomationProperties::SetName(
						action, hstring(displayName) + L" " + _(L"Disconnect"));
					action.Click([this, deviceId](IInspectable const&, RoutedEventArgs const&)
						{
							// Deferred: the rebuild destroys this button, and the automation
							// peer that delivered the click, while it is still being handled.
							DispatcherQueue().TryEnqueue([deviceId]
								{
									try
									{
										if (Shell().disconnect)
											Shell().disconnect(std::wstring(deviceId));
									}
									catch (winrt::hresult_error const& ex)
									{
										LOG_CAUGHT_EXCEPTION();
										LogFailure(L"Disconnect button", std::wstring(ex.message()));
									}
								});
						});
				}
				else
				{
					action.Content(winrt::box_value(hstring(_(L"Connect"))));
					Automation::AutomationProperties::SetName(
						action, hstring(displayName) + L" " + _(L"Connect"));
					action.Click([this, deviceId](IInspectable const&, RoutedEventArgs const&)
						{
							DispatcherQueue().TryEnqueue([deviceId]
								{
									try
									{
										if (Shell().connect)
											Shell().connect(std::wstring(deviceId));
									}
									catch (winrt::hresult_error const& ex)
									{
										LOG_CAUGHT_EXCEPTION();
										LogFailure(L"Connect button", std::wstring(ex.message()));
									}
								});
						});
				}

				actions.Children().Append(action);
			}

			{
				hstring const moreText = _(L"More");

				Button more;
				more.Width(32);
				more.Height(32);
				more.MinWidth(0);
				more.Padding(Thickness{ 0, 0, 0, 0 });
				more.VerticalAlignment(VerticalAlignment::Center);
				more.Content(ViewHelpers::MakeGlyph(kGlyphMore, 14, palette.secondary));
				more.Background(palette.transparent);
				more.BorderThickness(Thickness{ 0, 0, 0, 0 });
				more.CornerRadius(ViewHelpers::CornerRadius(4));
				ToolTipService::SetToolTip(more, winrt::box_value(moreText));
				Automation::AutomationProperties::SetName(
					more, hstring(displayName) + L" " + moreText);
				Automation::AutomationProperties::SetAutomationId(
					more, L"DeviceMoreButton" + std::to_wstring(index));
				more.Click([this, deviceId, details](IInspectable const&, RoutedEventArgs const&)
					{
						const bool opening = details.Visibility() != Visibility::Visible;

						CollapseExpandedSubmenu();

						details.Visibility(opening ? Visibility::Visible : Visibility::Collapsed);
						if (opening)
						{
							m_expandedDeviceId = std::wstring(deviceId);
							m_expandedDetails = details;
						}

						// Re-measured around the section that just appeared: the panel is
						// sized exactly to its content, and a rebuild would collapse it.
						if (Shell().resizePopup)
							Shell().resizePopup();
					});

				actions.Children().Append(more);
			}

			row.Children().Append(actions);
			Grid::SetColumn(actions, 2);

			StackPanel cardContent;
			cardContent.Spacing(8);
			cardContent.Children().Append(row);
			cardContent.Children().Append(details);
			card.Child(cardContent);

			if (failed && !device.message.empty())
				ToolTipService::SetToolTip(card, winrt::box_value(hstring{ device.message }));

			DevicePanel().Children().Append(card);
		}

	}
}
