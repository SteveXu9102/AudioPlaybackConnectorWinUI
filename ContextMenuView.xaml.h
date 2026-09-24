#pragma once

#include "ContextMenuView.g.h"
#include "pch.h"

#include "ViewHelpers.h"

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	/// <summary>
	/// The notification-area context menu: the receiver and notification switches,
	/// the theme and language selectors, and the Bluetooth settings, licences and
	/// exit rows.
	///
	/// The application-level items only. A device's own commands - reconnect it on
	/// the next start, remove its pairing - live in that device's card in the
	/// device panel (FluentView), reached from the ellipsis button on its row,
	/// because that is where the device is.
	/// </summary>
	struct ContextMenuView : ContextMenuViewT<ContextMenuView>
	{
		ContextMenuView();

		void Refresh();
		void PlayEntranceAnimation();
		void FocusFirstItem();

	private:
		struct Palette
		{
			winrt::Microsoft::UI::Xaml::Media::Brush primary{ nullptr };
			winrt::Microsoft::UI::Xaml::Media::Brush secondary{ nullptr };
			winrt::Microsoft::UI::Xaml::Media::Brush accent{ nullptr };
			winrt::Microsoft::UI::Xaml::Media::Brush cardBackground{ nullptr };
			winrt::Microsoft::UI::Xaml::Media::Brush cardBorder{ nullptr };
		};

		void AddCard(winrt::hstring const& label, winrt::Microsoft::UI::Xaml::Controls::Control const& control);
		/// <summary>
		/// Interactive menu item, appended to <paramref name="host"/> - the menu
		/// itself for a top-level row. Pass nullptr for no gutter glyph.
		/// </summary>
		winrt::Microsoft::UI::Xaml::Controls::Button AddRow(
			winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
			wchar_t const* glyph,
			bool accentGlyph,
			winrt::hstring const& text,
			winrt::Microsoft::UI::Xaml::Media::Brush const& textBrush,
			std::function<void()> onClick);
		winrt::Microsoft::UI::Xaml::Controls::Button AddRow(
			wchar_t const* glyph, bool accentGlyph, winrt::hstring const& text, std::function<void()> onClick);

		Palette m_palette{};
		winrt::Microsoft::UI::Xaml::Controls::Control m_firstItem{ nullptr };
		::AudioPlaybackConnectorWinUI::ViewHelpers::StaggeredFade m_entrance{};
	};
}

namespace winrt::AudioPlaybackConnectorWinUI::factory_implementation
{
	struct ContextMenuView : ContextMenuViewT<ContextMenuView, implementation::ContextMenuView>
	{
	};
}
