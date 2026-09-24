#pragma once

// Link-level Bluetooth operations that the A2DP Sink API does not expose.
//
// AudioPlaybackConnection can only open and release the audio profile; it has no
// member that reports which services a device has installed, so that is asked of
// the Bluetooth APIs directly.

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
	/// is streaming: AudioPlaybackConnection::State() belongs to the A2DP sink
	/// transport, which is shared per process, so opening any one connection makes
	/// every other connection in the process report Opened as well. The link does
	/// not lie: the sink receives over a link that is up, and a device that has only
	/// been invited to stream has no link at all.
	/// </summary>
	std::optional<bool> IsLinkConnected(std::wstring const& addressDigits);
}
