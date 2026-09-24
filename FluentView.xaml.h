#pragma once

#include "FluentView.g.h"
#include "pch.h"

#include "ViewHelpers.h"

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	/// <summary>
	/// The Fluent Design device panel. One card per enumerated device, each with the
	/// actions that belong to that device: connect or disconnect, and - behind the
	/// ellipsis button on the row - its own submenu, where it can be asked to
	/// reconnect on the next start or have its pairing removed.
	/// </summary>
	struct FluentView : FluentViewT<FluentView>
	{
		FluentView();

		void Refresh();
		void PlayEntranceAnimation();

	private:
		::AudioPlaybackConnectorWinUI::ViewHelpers::StaggeredFade m_entrance{};

		/// <summary>
		/// The device whose submenu is open, and the section that shows it. Kept by
		/// id rather than by element so that it survives the rebuild a device-list
		/// change causes - only one device's submenu is open at a time.
		/// </summary>
		std::wstring m_expandedDeviceId;
		winrt::Microsoft::UI::Xaml::Controls::StackPanel m_expandedDetails{ nullptr };

		void CollapseExpandedSubmenu();
	};
}

namespace winrt::AudioPlaybackConnectorWinUI::factory_implementation
{
	struct FluentView : FluentViewT<FluentView, implementation::FluentView>
	{
	};
}
