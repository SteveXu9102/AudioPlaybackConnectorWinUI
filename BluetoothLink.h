#pragma once

// Link-level Bluetooth operations that the A2DP Sink API does not expose:
// AudioPlaybackConnection has no member that reports which services a device has
// installed, so that is asked of the Bluetooth APIs directly.

namespace AudioPlaybackConnectorWinUI::BluetoothLink
{
	/// <summary>
	/// Whether the device currently has its A2DP service installed, or std::nullopt
	/// when that could not be determined (no radio, or the device is not known to
	/// the adapter). A service that was switched off - by Windows' own device
	/// properties or by another application - is absent from the installed list,
	/// and opening a connection then fails until it is switched back on.
	/// </summary>
	std::optional<bool> IsAudioServiceInstalled(std::wstring const& addressDigits);

	/// <summary>
	/// Whether the device's Bluetooth link is up at this moment, or std::nullopt
	/// when that could not be determined.
	///
	/// This is the only per-device fact the platform leaves about which sink device
	/// is streaming: State() and every StateChanged event describe the process, so
	/// each connection reports the same state and opening one makes every other
	/// report Opened as well. A device that has only been invited has no link.
	/// </summary>
	std::optional<bool> IsLinkConnected(std::wstring const& addressDigits);
}
