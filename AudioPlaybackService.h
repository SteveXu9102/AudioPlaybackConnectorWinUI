#pragma once

#include <atomic>
#include <mutex>

// Bluetooth A2DP Sink connection management.
//
// WinUI 3 has no equivalent of the UWP DevicePicker, so devices are enumerated
// through the Windows.Devices.Enumeration selector that AudioPlaybackConnection
// publishes and the panel renders them itself.
//
// The A2DP sink transport is shared per process, so releasing one connection takes
// every other connection of this process down with it. Holding the
// AudioPlaybackConnection objects here makes this application the single owner of
// every connection it opens, and "when the program side disconnects, everything
// disconnects" is the intended behaviour.

namespace AudioPlaybackConnectorWinUI
{
	enum class DeviceState
	{
		Disconnected,
		Connecting,
		Connected,
		Failed,
	};

	/// <summary>One device the panel offers, as much of it as the panel needs.</summary>
	struct DeviceEntry
	{
		std::wstring id;
		std::wstring name;
		DeviceState state = DeviceState::Disconnected;
		/// <summary>Localised failure text; only meaningful when state == Failed.</summary>
		std::wstring message;
		/// <summary>
		/// A resource reference into DDORes.dll, as Windows publishes it for this
		/// device. Empty when Windows published none; the panel then draws a glyph
		/// of its own.
		/// </summary>
		std::wstring icon;
	};

	class AudioPlaybackService
	{
	public:
		AudioPlaybackService() = default;
		~AudioPlaybackService();

		AudioPlaybackService(AudioPlaybackService const&) = delete;
		AudioPlaybackService& operator=(AudioPlaybackService const&) = delete;

		/// <summary>True when this Windows build exposes the A2DP Sink API.</summary>
		static bool IsSupported();

		/// <summary>Enumerate devices and start watching for arrivals/removals.</summary>
		void Start();
		/// <summary>Close every connection and stop watching. Used on exit.</summary>
		void Stop();

		std::vector<DeviceEntry> const& Devices() const { return m_devices; }
		bool IsConnected(std::wstring_view deviceId) const;
		/// <summary>Ids of every device with a live connection, in UI order.</summary>
		std::vector<std::wstring> ConnectedDeviceIds() const;
		/// <summary>
		/// How many connections this service holds. Not the connected-device count:
		/// a connection enters the map as soon as Connect() is asked for it and stays
		/// until it is released. Stop() closes exactly this set, so this is what the
		/// exit watchdog is sized from.
		/// </summary>
		size_t LiveConnectionCount() const;
		/// <summary>
		/// True when at least one device has an established A2DP connection. Devices
		/// that are still connecting do not count: nothing is streaming yet.
		/// </summary>
		bool HasConnectedDevice() const;

		void Connect(std::wstring deviceId);
		/// <summary>
		/// Disconnects everything: the A2DP sink transport is shared per process, so
		/// closing one connection takes the others down with it whatever the caller
		/// asked for. Every connection this service holds is closed and every device
		/// is reported as disconnected in one update.
		/// </summary>
		void Disconnect(std::wstring deviceId);
		/// <summary>
		/// Deletes the device's pairing from Windows, after closing any connection
		/// to it. Runs asynchronously: the device list is refreshed when it
		/// finishes, and a device that could not be removed reports why.
		/// </summary>
		void Unpair(std::wstring deviceId);
		/// <summary>Connect every device remembered in the settings file.</summary>
		void ReconnectSaved();
		/// <summary>
		/// Keeps the receiver ready for every paired sink device: while this is on,
		/// each device that has no connection of its own is given one that is
		/// started - advertised - but deliberately not opened, so the device can
		/// begin streaming by itself. OpenAsync is never called on the device's
		/// behalf here: that would force a connection instead of allowing one.
		///
		/// Turning it off closes exactly those advertised connections, through the
		/// one teardown path; a device that is already streaming is not one of them
		/// and is left alone.
		/// </summary>
		void SetAllowPaired(bool allow);

		/// <summary>Invoked (on the UI thread) whenever Devices() changes.</summary>
		void SetChangeHandler(std::function<void()> handler) { m_onChanged = std::move(handler); }
		/// <summary>
		/// Invoked (on the UI thread) when a device starts or stops streaming, with
		/// the name to show for it. Edges only: a device that is already connected
		/// does not report again.
		/// </summary>
		void SetConnectionEventHandler(std::function<void(std::wstring const& deviceName, bool connected)> handler)
		{
			m_onConnectionEvent = std::move(handler);
		}

	private:
		/// <summary>
		/// Bluetooth address of a device as twelve upper case hex digits, or empty
		/// when the id carries none. Taken from the device id itself, which is
		/// authoritative and always present.
		/// </summary>
		std::wstring AddressForDevice(std::wstring const& deviceId) const;

		/// <summary>
		/// Turns the device's A2DP service on this machine on or off. Switching the
		/// service off removes its driver for that device, which takes the device out
		/// of the selector this class enumerates, so it is only used to repair a
		/// service something else switched off - never to disconnect.
		/// </summary>
		static bool SetAudioServiceEnabled(std::wstring const& addressDigits, bool enabled);

		/// <summary>
		/// One AudioPlaybackConnection and the bookkeeping its teardown needs.
		///
		/// This is where the platform's measured teardown rule lives: a closed
		/// AudioPlaybackConnection keeps a null member inside Windows.Media.Devices.dll,
		/// and reading State() on it afterwards faults with 0xC0000005 at
		/// Windows.Media.Devices.dll+0x4D9BC. So a connection is closed in exactly one
		/// place, Teardown(), in exactly one order: revoke the StateChanged token, then
		/// Close(), then drop every reference. <c>closed</c> is read and written under
		/// <c>mutex</c> and so is the callback's State() read, which makes "State()
		/// after Close()" impossible rather than merely unlikely.
		/// </summary>
		struct Connection
		{
			winrt::Windows::Media::Audio::AudioPlaybackConnection handle{ nullptr };
			/// <summary>StateChanged subscription; revoked once, at teardown.</summary>
			winrt::event_token token{};
			/// <summary>True once teardown has begun. Read/written under <c>mutex</c>.</summary>
			bool closed = false;
			/// <summary>
			/// True while this is a connection the allow-paired switch started but
			/// did not open: the device has been invited to stream and nothing has
			/// asked it to. Cleared, under <c>mutex</c>, when an active connect
			/// promotes the connection and when the device opens it, so switching
			/// that switch off cannot disturb a device something has asked to
			/// connect - see SetAllowPaired.
			/// </summary>
			bool advertised = false;
			/// <summary>
			/// True once Start() succeeded. A promoted connection is one the platform
			/// has already started, and a started connection is not started twice.
			/// </summary>
			bool started = false;
			/// <summary>
			/// True once an open has been attributed to this connection's device.
			///
			/// This is what "connected" means, and it is deliberately not the same
			/// question as "is this connection live": the allow-paired switch keeps a
			/// started but unopened connection for every paired device, and reporting
			/// one of those as connected is what lit up devices that had only been
			/// invited.
			///
			/// The A2DP sink transport is shared per process, so the platform raises
			/// StateChanged(Opened) on *every* connection of this process when any one
			/// of them opens; the flag is therefore set only for the connection the
			/// open could be attributed to (see IsOpenForThisDevice) and never from a
			/// sibling's report. Read and written under <c>mutex</c>.
			/// </summary>
			bool open = false;
			/// <summary>
			/// True once an open has been asked for on this connection's own object.
			///
			/// Set when the open is issued, not when it is reported, because the
			/// window between the two is the one where the connection is neither an
			/// invitation any more (SetAllowPaired must not release it) nor open yet
			/// (IsConnected must not report it). It is also the evidence that
			/// survives the shared transport: StateChanged(Opened) alone is
			/// process-wide, while an open this application asked for and then saw
			/// reported here belongs to this device. Read and written under
			/// <c>mutex</c>.
			/// </summary>
			bool openRequested = false;
			/// <summary>
			/// Makes the flags above and the callback's State() read mutually
			/// exclusive. Only ever held for a state read or a flag flip - never
			/// across Close().
			/// </summary>
			std::mutex mutex;
		};

		/// <summary>The connection held for a device, or nullptr when there is none.</summary>
		std::shared_ptr<Connection> FindConnection(std::wstring_view deviceId) const;
		/// <summary>The device's row in the panel, or nullptr when it has none.</summary>
		DeviceEntry* Find(std::wstring_view deviceId);
		/// <summary>True while this connection is still in the map and not closed.</summary>
		bool IsConnectionLive(std::shared_ptr<Connection> const& connection) const;
		/// <summary>
		/// True while this connection's device is the one the sink transport is
		/// carrying: an open was attributed to it and it has not been released.
		/// </summary>
		bool IsConnectionOpen(std::shared_ptr<Connection> const& connection) const;
		/// <summary>Records that this connection's device is, or is no longer, receiving.</summary>
		void SetConnectionOpen(std::shared_ptr<Connection> const& connection, bool open);
		/// <summary>
		/// True when an open this connection reported may be attributed to its own
		/// device. See the definition for why the platform's own state cannot answer
		/// this and the Bluetooth link can.
		/// </summary>
		bool IsOpenForThisDevice(
			std::shared_ptr<Connection> const& connection, std::wstring const& deviceId) const;
		/// <summary>True while this connection is a standing invitation, not an open.</summary>
		bool IsAdvertised(std::shared_ptr<Connection> const& connection) const;
		/// <summary>
		/// Forgets a connection on the calling (UI) thread and closes it on a thread
		/// of its own, so a released device stops being reported as connected at once
		/// even though the close is still in flight. The actual close is Teardown().
		/// </summary>
		void ReleaseConnection(std::shared_ptr<Connection> const& connection);
		/// <summary>
		/// Every entry in the map, forgotten and closed.
		/// </summary>
		/// <param name="synchronous">
		/// True closes them on the calling thread before returning. Stop() uses that,
		/// because the process may not outlive the call and a detached thread could be
		/// terminated before it had closed anything - leaving the A2DP profile behind
		/// in the state that refuses every later connection with 0x8007001F. False
		/// hands each close to a thread of its own, which is what Disconnect() needs:
		/// the panel is live and the user is waiting.
		/// </param>
		void ReleaseAllConnections(bool synchronous);
		/// <summary>
		/// The one place a connection is closed: revoke the StateChanged subscription,
		/// then Close() (each guarded), then drop the reference - in that order and
		/// never the other way round. See Connection for why.
		/// </summary>
		static void Teardown(std::shared_ptr<Connection> const& connection);
		/// <summary>
		/// Releases the connection of a device the watcher reported as removed: a sink
		/// that is still advertised would otherwise keep a device that is no longer
		/// there receiving.
		/// </summary>
		void OnDeviceRemoved(std::wstring deviceId);
		/// <summary>
		/// Records that a connection opened, or that it is no longer open. Always runs
		/// on the dispatcher, which is the thread the device list and the panel belong
		/// to.
		///
		/// The connection that raised the report is carried here rather than looked up
		/// again from the device id: the shared transport makes every connection in
		/// this process report the same open, so a report may only ever be applied to
		/// the connection that produced it - never to whatever connection the device
		/// id happens to name at the time.
		/// </summary>
		void OnConnectionStateChanged(
			std::shared_ptr<Connection> const& connection, std::wstring deviceId, bool open);
		/// <summary>
		/// Subscribes to a connection's state changes. The callback runs on a media
		/// stack thread and does exactly two things: read the state while the
		/// connection is provably not closed, and hand the fact to the dispatcher. It
		/// never blocks, because a state change arrives on an audio service thread
		/// that must not be held up.
		///
		/// <paramref name="deviceId"/> is this connection's own device, captured by
		/// value per subscription - never a loop variable or a shared "current
		/// device". The id that is reported is the sender's own DeviceId() whenever it
		/// has one, so the identity travels with the object that raised the event.
		/// </summary>
		void SubscribeStateChanges(std::shared_ptr<Connection> const& connection, std::wstring const& deviceId);
		/// <summary>
		/// Gives every device in the list that has no connection of its own a started
		/// but unopened one, while the allow-paired switch is on. Runs on the
		/// dispatcher.
		/// </summary>
		void AdvertiseDevices();
		/// <summary>Starts the invitation for one device. Runs on the dispatcher.</summary>
		void AdvertiseDevice(std::wstring const& deviceId);
		/// <summary>
		/// Reports that an invitation could not be started, so it is given up rather
		/// than kept as a connection nothing can use.
		/// </summary>
		void OnAdvertiseFinished(std::wstring const& deviceId, std::shared_ptr<Connection> const& connection, bool started);
		/// <summary>
		/// Notes that one close finished. The invitation pass runs once the last of
		/// them has, because a Close() takes the shared A2DP transport down with it
		/// and an invitation started during one would be torn down underneath it.
		/// </summary>
		void OnTeardownFinished();
		/// <summary>
		/// Reports a device as streaming, and announces it the one time it becomes
		/// so. Both the open this application asked for and the state change that
		/// says the device began on its own land here.
		/// </summary>
		void MarkConnected(std::wstring const& deviceId);
		/// <summary>
		/// Hands a device's streaming edge to the connection-event handler, with the
		/// name to show for it.
		/// </summary>
		void ReportConnectionEvent(std::wstring const& deviceId, bool connected);
		void Notify();
		void SetState(std::wstring const& deviceId, DeviceState state, std::wstring message = {});
		/// <summary>
		/// Reports every device that has a live connection as disconnected, and
		/// notifies once. Used by Disconnect, which takes them all down together.
		/// </summary>
		void ReportAllDisconnected(std::vector<std::wstring> const& deviceIds);
		/// <summary>
		/// Records the outcome of a connect. Always called on the dispatcher, and
		/// ignored when the connection it belongs to is no longer the live one for
		/// that device - the user may have disconnected while the open was in flight.
		/// </summary>
		void OnConnectFinished(
			std::wstring deviceId,
			std::shared_ptr<Connection> const& connection,
			HRESULT result,
			std::wstring const& detail,
			bool linkDown);

		/// <summary>
		/// One device's outstanding "connect again on the next launch" request, held
		/// under <c>m_reconnectMutex</c>. An entry is added when the request is made
		/// - by ReconnectSaved at startup, or by a connect the user or the
		/// allow-paired path asked for - and removed when the device is connected, or
		/// when the request has been given its attempts.
		/// </summary>
		struct ReconnectRequest
		{
			/// <summary>Connect attempts made so far for this request.</summary>
			unsigned attempts = 0;
		};

		/// <summary>How many connect attempts one request is given before it gives up.</summary>
		static constexpr unsigned kReconnectMaxAttempts = 3;

		/// <summary>
		/// Notes that the device has a connect request outstanding, and whether it is
		/// one that has to be carried out - a reconnect request that no connection
		/// has been made for yet. Runs on the dispatcher.
		///
		/// The question asked here is about the request, not about the device: an
		/// allow-paired invitation is a connection and not an open, so it neither
		/// answers nor cancels a reconnect.
		/// </summary>
		bool EnsureReconnectRequest(std::wstring const& deviceId, bool savedForReconnect);

		/// <summary>Records one attempt against the device's request.</summary>
		void RecordReconnectAttempt(std::wstring const& deviceId);

		/// <summary>
		/// Records what became of an attempt: a device that is not connected and not
		/// linked keeps its request for a later attempt, and one that is connected,
		/// out of attempts, or was never a reconnect request loses it. Runs on the
		/// dispatcher.
		/// </summary>
		void CompleteReconnect(std::wstring const& deviceId, bool connected);

		/// <summary>
		/// Carries out every outstanding request that is not open yet: the devices
		/// the enumeration had not answered for when the request was made are
		/// attempted as soon as it does, and one whose link was down is attempted
		/// again when the device reports a change. A connection that is started but
		/// unopened - an invitation - is attempted too, and the attempt promotes it.
		/// Runs on the dispatcher.
		/// </summary>
		void ReconnectPending();

		/// <summary>
		/// True when an open reported for this connection is one this application
		/// asked for and confirmed - the evidence that survives the platform's
		/// process-wide transport report.
		/// </summary>
		bool IsOpenRequested(std::shared_ptr<Connection> const& connection) const;

		/// <summary>
		/// One full enumeration pass. Only RefreshDevicesAsync calls it, because that
		/// is what keeps two passes from overlapping.
		/// </summary>
		winrt::Windows::Foundation::IAsyncAction RefreshOnceAsync();
		winrt::fire_and_forget RefreshDevicesAsync();
		winrt::fire_and_forget ConnectAsync(std::wstring deviceId, bool reconnect);
		winrt::fire_and_forget UnpairAsync(std::wstring deviceId);

		std::vector<DeviceEntry> m_devices;
		/// <summary>
		/// One AudioPlaybackConnection per device, guarded by
		/// <c>m_connectionsMutex</c>: the worker thread that opens a connection asks
		/// whether its connection is still the live one while the UI thread may be
		/// removing entries. The map remembers which connections are this
		/// application's to release.
		/// </summary>
		std::unordered_map<std::wstring, std::shared_ptr<Connection>> m_connections;
		mutable std::mutex m_connectionsMutex;
		/// <summary>
		/// Devices with "connect again on the next launch" still to be dealt with:
		/// one entry per device in the settings list, removed once the device is
		/// connected, or once its one attempt has failed. The first attempt is
		/// deferred when the enumeration has not answered for the device yet, and
		/// retried when it does - it is never dropped.
		/// </summary>
		std::unordered_map<std::wstring, ReconnectRequest> m_reconnectRequests;
		/// <summary>Guards m_reconnectRequests; see ReconnectRequest.</summary>
		mutable std::mutex m_reconnectMutex;
		winrt::Windows::Devices::Enumeration::DeviceWatcher m_watcher{ nullptr };
		winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
		std::function<void()> m_onChanged;
		std::function<void(std::wstring const& deviceName, bool connected)> m_onConnectionEvent;
		/// <summary>
		/// Set while the allow-paired switch is on. Read from the closing threads as
		/// well as the dispatcher, because a close that finishes decides whether the
		/// invitations are to be renewed.
		/// </summary>
		std::atomic<bool> m_allowPaired{ false };
		/// <summary>Closes still running on threads of their own. See OnTeardownFinished.</summary>
		std::atomic<int> m_pendingTeardowns{ 0 };
		/// <summary>
		/// Devices whose invitation could not be started. An invitation that fails
		/// ends in the close that asks for the next pass, so retrying at once would
		/// be a loop; a device is therefore invited once, and again when the switch
		/// is turned on - which is also what re-reads the device list.
		/// </summary>
		std::unordered_set<std::wstring> m_advertiseFailed;
		/// <summary>
		/// Set while a refresh is enumerating. A watcher event that arrives during one
		/// asks for a single extra pass instead of starting an enumeration of its own,
		/// so a burst of device events costs at most two passes. Only ever touched on
		/// the dispatcher thread.
		/// </summary>
		bool m_refreshRunning = false;
		/// <summary>Set by the request that arrived while a refresh was already running.</summary>
		bool m_refreshAgain = false;
	};

	/// <summary>Process-wide A2DP service instance.</summary>
	AudioPlaybackService& Playback();
}
