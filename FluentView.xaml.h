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
		/// <summary>
		/// Stops the section timer. A started DispatcherQueueTimer is kept alive by the
		/// dispatcher, so its Tick handler would otherwise outlive this view.
		/// </summary>
		~FluentView();

		void Refresh();
		void PlayEntranceAnimation();

		/// <summary>
		/// One frame of the hosting popup's height transition, given the height it
		/// still has to grow. The section the user just opened appears as soon as the
		/// window is big enough to hold it; zero - the window is not growing, or has
		/// arrived - reveals it at once. Called by TrayWindow through the
		/// implementation type, because the entry point is between two animations
		/// rather than part of the view's own interface.
		/// </summary>
		void OnPopupGrowth(int remainingPx);

	private:
		::AudioPlaybackConnectorWinUI::ViewHelpers::StaggeredFade m_entrance{};

		/// <summary>
		/// The revealed section's own entrance, on a clock of its own: a section can be
		/// opened while the cards are still fading in, and neither may cancel the other.
		/// </summary>
		::AudioPlaybackConnectorWinUI::ViewHelpers::StaggeredFade m_section{};

		/// <summary>
		/// The device whose submenu is open, and the section that shows it. Kept by
		/// id rather than by element so that it survives the rebuild a device-list
		/// change causes - only one device's submenu is open at a time.
		/// </summary>
		std::wstring m_expandedDeviceId;
		winrt::Microsoft::UI::Xaml::Controls::StackPanel m_expandedDetails{ nullptr };

		/// <summary>
		/// Set between the press that opens a section and the frame of the popup's
		/// height transition that can hold it: the section is in the tree, and in the
		/// height the window is growing to, but still invisible.
		/// </summary>
		bool m_sectionPending{ false };
		/// <summary>Runs when the section that is leaving has finished fading out.</summary>
		std::function<void()> m_sectionDone{};
		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_sectionTimer{ nullptr };

		bool IsSectionOpen(std::wstring const& deviceId) const;
		void OpenSection(std::wstring const& deviceId, winrt::Microsoft::UI::Xaml::Controls::StackPanel const& details);
		void CloseSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& details);
		void RevealSection();
		void StartSectionTimer(int delayMs);
		void OnSectionTimer();
		void CollapseExpandedSubmenu();
	};
}

namespace winrt::AudioPlaybackConnectorWinUI::factory_implementation
{
	struct FluentView : FluentViewT<FluentView, implementation::FluentView>
	{
	};
}
