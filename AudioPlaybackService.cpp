#include "pch.h"
#include "AudioPlaybackService.h"
#include "AppSettings.h"
#include "BluetoothLink.h"
#include "I18n.hpp"
#include "Util.hpp"

#include <thread>

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Media::Audio;

namespace AudioPlaybackConnectorWinUI
{
	namespace
	{
		// How long the connection is given to come up. A sink whose device instance
		// is still settling refuses the open and accepts it a moment later, so the
		// open is retried rather than believed once.
		constexpr unsigned kOpenAttempts = 3;
		constexpr unsigned kOpenRetryDelayMs = 1500;

		// How long State() is watched for the open the request asked for. Open's own
		// result is not the connection's state: for a device that had already gone,
		// Open still returned Success, so the state is what decides.
		constexpr unsigned kStateConfirmAttempts = 8;
		constexpr unsigned kStateConfirmDelayMs = 500;

		// How often the streaming devices are asked whether their Bluetooth link is
		// still up. A pass is cheap: nothing streaming means no Bluetooth call.
		constexpr unsigned kLinkSweepIntervalMs = 2000;

		constexpr unsigned kLinkSweepStopWaitMs = 200;

		// How long Stop() waits for closes that ReleaseConnection handed to threads
		// of their own, before it goes on to close what is left itself.
		constexpr unsigned kPendingTeardownWaitMs = 1000;

		std::wstring FormatHresult(winrt::hresult_error const& ex)
		{
			wchar_t code[16]{};
			swprintf_s(code, L"(0x%08X)", static_cast<uint32_t>(ex.code()));

			std::wstring result(ex.message().c_str());
			result += L" ";
			result += code;
			return result;
		}

		std::wstring Hex(uint32_t value)
		{
			wchar_t text[16]{};
			swprintf_s(text, L"0x%08X", value);
			return text;
		}

		/// <summary>
		/// How a device's Bluetooth link reads: "up"/"down" when the Bluetooth APIs
		/// could answer, "unknown" when they could not (no radio, or a device the
		/// adapter does not know). The distinction matters at teardown-free
		/// attribution: only a link that is verifiably down is evidence a device is
		/// not streaming, while an unreadable one is not evidence of anything.
		/// </summary>
		std::wstring_view LinkText(std::optional<bool> const& link)
		{
			if (!link.has_value())
				return L"unknown";
			return *link ? L"up" : L"down";
		}

		/// <summary>
		/// The Bluetooth address embedded in an AudioPlaybackConnection device id, as
		/// twelve upper case hex digits, or empty when there is none.
		///
		/// The id is a device interface path and the address sits in it after "&0&".
		/// This is the reliable source: BluetoothDevice::FromIdAsync fails for some
		/// paired devices, so the paired-device lookup cannot supply it.
		/// </summary>
		std::wstring AddressFromDeviceId(std::wstring_view deviceId)
		{
			constexpr size_t kDigitCount = 12;

			const size_t marker = deviceId.find(L"&0&");
			if (marker == std::wstring_view::npos)
				return {};

			const size_t start = marker + 3;
			if (start + kDigitCount > deviceId.size())
				return {};

			std::wstring digits;
			digits.reserve(kDigitCount);

			for (size_t index = 0; index < kDigitCount; ++index)
			{
				const wchar_t character = static_cast<wchar_t>(std::towupper(deviceId[start + index]));

				const bool isDigit = character >= L'0' && character <= L'9';
				const bool isHexLetter = character >= L'A' && character <= L'F';
				if (!isDigit && !isHexLetter)
					return {};

				digits.push_back(character);
			}

			return digits;
		}

		/// <summary>A Bluetooth address as 12 upper case hex digits, e.g. C016931FF1CA.</summary>
		std::wstring AddressDigits(uint64_t address)
		{
			constexpr wchar_t kDigits[] = L"0123456789ABCDEF";

			std::wstring text(12, L'0');
			for (int index = 11; index >= 0; --index)
			{
				text[index] = kDigits[address & 0xF];
				address >>= 4;
			}
			return text;
		}

		/// <summary>
		/// The icon Windows gives this device, as the panel needs it: the
		/// "System.Devices.GlyphIcon" property of the device interface, which is a
		/// resource reference into DDORes.dll. That is the icon Windows Settings shows
		/// for the same device; the device's name and its Bluetooth class of device are
		/// not consulted, because neither describes what kind of device it is.
		/// </summary>
		std::wstring IconForDevice(DeviceInformation const& info)
		{
			for (auto const& property : info.Properties())
			{
				if (std::wstring_view(property.Key()) != L"System.Devices.GlyphIcon")
					continue;

				if (auto const text = property.Value().try_as<winrt::hstring>())
					return std::wstring(*text);

				break;
			}

			return {};
		}

		/// <summary>The remote service an A2DP sink connection uses (AudioSource, 0x110A).</summary>
		constexpr GUID kAudioSourceService{ 0x0000110a, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

		/// <summary>Twelve hex digits back to an address.</summary>
		uint64_t AddressFromDigits(std::wstring_view digits)
		{
			uint64_t address = 0;
			for (const wchar_t digit : digits)
			{
				address <<= 4;
				if (digit >= L'0' && digit <= L'9')
					address |= static_cast<uint64_t>(digit - L'0');
				else if (digit >= L'A' && digit <= L'F')
					address |= static_cast<uint64_t>(digit - L'A' + 10);
				else if (digit >= L'a' && digit <= L'f')
					address |= static_cast<uint64_t>(digit - L'a' + 10);
			}

			return address;
		}
	}

	bool AudioPlaybackService::SetAudioServiceEnabled(std::wstring const& addressDigits, bool enabled)
	{
		if (addressDigits.size() != 12)
			return false;

		BLUETOOTH_FIND_RADIO_PARAMS radioParams{ sizeof(BLUETOOTH_FIND_RADIO_PARAMS) };
		HANDLE radio = nullptr;
		const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&radioParams, &radio);
		if (find == nullptr)
		{
			LogFailure(L"Bluetooth service", L"no Bluetooth radio was found");
			return false;
		}

		BLUETOOTH_DEVICE_INFO info{};
		info.dwSize = sizeof(info);
		info.Address.ullLong = AddressFromDigits(addressDigits);

		// The device has to come from a search: BluetoothSetServiceState rejects an
		// address that was not found this way (ERROR_INVALID_PARAMETER).
		BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
		search.dwSize = sizeof(search);
		search.fReturnAuthenticated = TRUE;
		search.fReturnRemembered = TRUE;
		search.fReturnConnected = TRUE;
		search.fReturnUnknown = TRUE;
		search.fIssueInquiry = FALSE;
		search.cTimeoutMultiplier = 1;
		search.hRadio = radio;

		bool found = false;
		BLUETOOTH_DEVICE_INFO device{};
		device.dwSize = sizeof(device);
		const HBLUETOOTH_DEVICE_FIND findDevice = BluetoothFindFirstDevice(&search, &device);
		if (findDevice != nullptr)
		{
			do
			{
				if (device.Address.ullLong == info.Address.ullLong)
				{
					info = device;
					found = true;
					break;
				}
			} while (BluetoothFindNextDevice(findDevice, &device));

			BluetoothFindDeviceClose(findDevice);
		}

		const DWORD result = BluetoothSetServiceState(
			radio,
			&info,
			&kAudioSourceService,
			enabled ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE);

		CloseHandle(radio);
		BluetoothFindRadioClose(find);

		if (result != ERROR_SUCCESS)
		{
			// ERROR_ACCESS_DENIED would mean the service cannot be changed without
			// elevation, which this per-user application deliberately does not ask for.
			LogFailure(L"Bluetooth service",
				std::wstring(enabled ? L"could not enable" : L"could not disable")
				+ L" the A2DP service for " + addressDigits
				+ L" (found=" + std::to_wstring(found ? 1 : 0)
				+ L", error " + std::to_wstring(result) + L")");
			return false;
		}

		LogTrace(std::wstring(L"A2DP service ") + (enabled ? L"enabled" : L"disabled")
			+ L" for " + addressDigits);

		return true;
	}

	AudioPlaybackService& Playback()
	{
		static AudioPlaybackService service;
		return service;
	}

	AudioPlaybackService::~AudioPlaybackService()
	{
		Stop();
	}

	bool AudioPlaybackService::IsSupported()
	{
		try
		{
			using namespace winrt::Windows::Foundation::Metadata;
			return ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
			return false;
		}
	}

	void AudioPlaybackService::Start()
	{
		m_dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

		try
		{
			auto selector = AudioPlaybackConnection::GetDeviceSelector();
			// The panel draws the icon Windows publishes for a device, which is not
			// part of the default property set, so it is asked for by name.
			const auto requested = winrt::single_threaded_vector<winrt::hstring>({ L"System.Devices.GlyphIcon" });
			m_watcher = DeviceInformation::CreateWatcher(selector, requested);
			m_watcher.Added([this](DeviceWatcher const&, DeviceInformation const&)
			{
				RefreshDevicesAsync();
			});
			m_watcher.Removed([this](DeviceWatcher const&, DeviceInformationUpdate const& update)
			{
				OnDeviceRemoved(std::wstring(update.Id()));
				RefreshDevicesAsync();
			});
			m_watcher.Updated([this](DeviceWatcher const&, DeviceInformationUpdate const&) { RefreshDevicesAsync(); });
			m_watcher.Start();
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
		}

		// The connection set itself is event-driven, but the reports cannot cover two
		// cases: a device that stopped streaming while its link stayed up, and a
		// device-side end that changes no reported state. Those are what the sweep
		// covers. It is cancelled in Stop() before any connection is released.
		m_stopping = false;
		try
		{
			m_linkSweep = winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
				[this](winrt::Windows::System::Threading::ThreadPoolTimer const&)
				{
					// The tick runs on a thread-pool thread, where nothing else
					// catches for it.
					try
					{
						SweepLinks();
					}
					CATCH_LOG();
				},
				std::chrono::milliseconds(kLinkSweepIntervalMs));
		}
		CATCH_LOG();

		RefreshDevicesAsync();
	}

	void AudioPlaybackService::Stop()
	{
		if (m_watcher)
		{
			try
			{
				m_watcher.Stop();
				m_watcher = nullptr;
			}
			CATCH_LOG();
		}

		// The sweep goes before the connections do: a pass holds references to them
		// and must not be inside the Bluetooth stack while they are closed.
		StopLinkSweep();

		// A close handed to a thread of its own by ReleaseConnection is still a
		// Close() the process must not be killed in the middle of - that is the
		// teardown which leaves the A2DP profile refusing every later connection
		// with 0x8007001F. "Disconnect, then quit at once" is exactly that, so the
		// outstanding closes are drained here, bounded because the caller may be the
		// exit path the watchdog is already counting down.
		for (unsigned waited = 0;
			m_pendingTeardowns.load() > 0 && waited < kPendingTeardownWaitMs;
			waited += 10)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		if (const int pending = m_pendingTeardowns.load(); pending > 0)
		{
			LogFailure(L"exit", L"a close handed to another thread had not finished ("
				+ std::to_wstring(pending) + L" still running)");
		}

		// Every connection is closed here, on the calling thread. Releasing them is
		// what deactivates the A2DP transport in the Bluetooth audio service, and
		// skipping it leaves the profile behind in a state that refuses every later
		// connection with 0x8007001F (ERROR_GEN_FAILURE); a detached thread could be
		// terminated with the process before the last Close() ran.
		ReleaseAllConnections();
	}

	std::shared_ptr<AudioPlaybackService::Connection>
		AudioPlaybackService::FindConnection(std::wstring_view deviceId) const
	{
		const std::lock_guard<std::mutex> guard(m_connectionsMutex);

		const auto it = m_connections.find(std::wstring(deviceId));
		return it == m_connections.end() ? nullptr : it->second;
	}

	bool AudioPlaybackService::IsConnectionLive(std::shared_ptr<Connection> const& connection) const
	{
		if (connection == nullptr)
			return false;

		// The connection's own flag first: it is set by teardown before Close(), and
		// reading it under this lock is what makes the State() read in the callback
		// mutually exclusive with Close().
		{
			const std::lock_guard<std::mutex> guard(connection->mutex);
			if (connection->closed)
				return false;
		}

		const std::lock_guard<std::mutex> guard(m_connectionsMutex);

		for (auto const& [deviceId, held] : m_connections)
		{
			if (held == connection)
				return true;
		}

		return false;
	}

	bool AudioPlaybackService::IsConnectionOpen(std::shared_ptr<Connection> const& connection) const
	{
		if (connection == nullptr)
			return false;

		const std::lock_guard<std::mutex> guard(connection->mutex);
		return connection->open && !connection->closed;
	}

	void AudioPlaybackService::SetConnectionOpen(std::shared_ptr<Connection> const& connection, bool open)
	{
		if (connection == nullptr)
			return;

		const std::lock_guard<std::mutex> guard(connection->mutex);
		connection->open = open;

		// An open that is being recorded here has been attributed to this device, so
		// the connection has evidence of its own from now on.
		if (open)
			connection->openRequested = true;
	}

	bool AudioPlaybackService::IsOpenForThisDevice(
		std::shared_ptr<Connection> const& connection, std::wstring const& deviceId) const
	{
		// The device's own Bluetooth link is the only per-device fact the platform
		// leaves about which sink device is carried: the platform's Opened state
		// belongs to the process, not to this connection, so opening one connection
		// raises StateChanged(Opened) on every other connection of this process
		// (measured: one OpenAsync for one of this machine's two paired devices made
		// both connections report Opened in the same millisecond). What the report
		// says is therefore never taken as a fact about this device. A device that
		// has only been invited has no link at all, and one whose link is verifiably
		// down is not the device being carried - however loudly the report insists.
		//
		// This runs on the dispatcher - OnConnectionStateChanged reaches it - so the
		// link is answered from what the sweep last read rather than read here:
		// BluetoothLink::IsLinkConnected enumerates the radio and can block the UI.
		const std::wstring address = AddressForDevice(deviceId);
		if (!address.empty())
		{
			const auto link = CachedLink(address);
			if (link.has_value())
			{
				LogTrace(L"link cache hit for " + deviceId + L": its link is "
					+ std::wstring(LinkText(link)));
				return *link;
			}
		}

		// Nothing fresh is known about this device's link. The connection's own
		// evidence answers for now, and the sweep is asked for so that the link
		// itself answers the next report within a tick instead of at the next
		// periodic pass.
		m_sweepNow = true;

		LogTrace(L"link cache miss for " + deviceId
			+ L": this connection's own evidence decides, and the link watch runs now");

		// Only this connection's own evidence is believed: an open it requested, or
		// a device that took its invitation.
		return IsOpenRequested(connection) || IsAdvertised(connection);
	}

	std::optional<bool> AudioPlaybackService::CachedLink(std::wstring const& address) const
	{
		const std::lock_guard<std::mutex> guard(m_connectionsMutex);

		const auto it = m_linkCache.find(address);
		if (it == m_linkCache.end())
			return std::nullopt;

		if (std::chrono::steady_clock::now() - it->second.readAt > kLinkCacheFreshMs)
			return std::nullopt;

		return it->second.link;
	}

	bool AudioPlaybackService::IsOpenRequested(std::shared_ptr<Connection> const& connection) const
	{
		if (connection == nullptr)
			return false;

		const std::lock_guard<std::mutex> guard(connection->mutex);
		return connection->openRequested && !connection->closed;
	}

	bool AudioPlaybackService::IsAdvertised(std::shared_ptr<Connection> const& connection) const
	{
		if (connection == nullptr)
			return false;

		const std::lock_guard<std::mutex> guard(connection->mutex);
		return connection->advertised;
	}

	void AudioPlaybackService::ReleaseConnection(std::shared_ptr<Connection> const& connection)
	{
		if (connection == nullptr)
			return;

		// Cleared before the close rather than after it: IsConnected reads this flag,
		// so the panel must stop reporting the device as soon as it is let go.
		SetConnectionOpen(connection, false);

		{
			const std::lock_guard<std::mutex> guard(m_connectionsMutex);

			for (auto it = m_connections.begin(); it != m_connections.end(); ++it)
			{
				if (it->second != connection)
					continue;

				LogTrace(L"releasing the connection for " + it->first);
				m_connections.erase(it);
				break;
			}
		}

		// The close is handed to a thread of its own: revoking the token and Close()
		// both call into the media stack, and neither belongs on the thread that draws
		// the panel.
		m_pendingTeardowns.fetch_add(1);
		std::thread([this, connection]
			{
				Teardown(connection);

				// The last close that finishes is what lets the allow-paired switch
				// advertise again - see OnTeardownFinished.
				if (m_pendingTeardowns.fetch_sub(1) == 1)
					OnTeardownFinished();
			}).detach();
	}

	void AudioPlaybackService::ReleaseAllConnections()
	{
		std::vector<std::shared_ptr<Connection>> connections;
		{
			const std::lock_guard<std::mutex> guard(m_connectionsMutex);

			connections.reserve(m_connections.size());
			for (auto const& [deviceId, connection] : m_connections)
				connections.push_back(connection);

			m_connections.clear();
		}

		// On the calling thread, as Stop() requires: the process may not outlive the
		// call, and a detached thread could be terminated before it had closed
		// anything - leaving the A2DP profile behind in the state that refuses every
		// later connection with 0x8007001F.
		for (auto const& connection : connections)
		{
			if (connection != nullptr)
				Teardown(connection);
		}
	}

	void AudioPlaybackService::Teardown(std::shared_ptr<Connection> const& connection)
	{
		if (connection == nullptr)
			return;

		// Marked closed before anything is touched, under the same lock the
		// StateChanged callback takes before it reads State(). From here on the
		// callback can only observe "closed" and return, so no state read can happen
		// after Close() below.
		{
			const std::lock_guard<std::mutex> guard(connection->mutex);
			connection->closed = true;
		}

		// Order, once and for all: revoke the subscription, then Close, then drop the
		// reference. Touching a closed AudioPlaybackConnection at all dereferences a
		// null member inside Windows.Media.Devices.dll (0xC0000005 at
		// Windows.Media.Devices.dll+0x4D9BC), which is why nothing else in this file
		// is allowed to close a connection.
		try
		{
			if (connection->handle != nullptr)
				connection->handle.StateChanged(connection->token);
		}
		CATCH_LOG();

		try
		{
			if (connection->handle != nullptr)
				connection->handle.Close();
		}
		CATCH_LOG();

		// Dropping the handle releases the last reference to the object.
		connection->handle = nullptr;
	}

	DeviceEntry* AudioPlaybackService::Find(std::wstring_view deviceId)
	{
		auto it = std::find_if(m_devices.begin(), m_devices.end(),
			[deviceId](DeviceEntry const& entry) { return entry.id == deviceId; });
		return it == m_devices.end() ? nullptr : &*it;
	}

	bool AudioPlaybackService::IsConnected(std::wstring_view deviceId) const
	{
		const auto connection = FindConnection(deviceId);

		// A live connection is not a connection that is receiving. The allow-paired
		// switch keeps a started, deliberately unopened connection for every paired
		// device, and answering "connected" for one of those is what lit up devices
		// that had only been invited: the badge was filled with the accent colour and
		// the row's "not connected" text was drawn in the accent colour too, although
		// nothing was streaming. Only an open that was attributed to this device's own
		// connection counts.
		return connection != nullptr && IsConnectionLive(connection) && IsConnectionOpen(connection);
	}

	std::wstring AudioPlaybackService::AddressForDevice(std::wstring const& deviceId) const
	{
		return AddressFromDeviceId(deviceId);
	}

	std::vector<std::wstring> AudioPlaybackService::ConnectedDeviceIds() const
	{
		std::vector<std::wstring> ids;

		const std::lock_guard<std::mutex> guard(m_connectionsMutex);

		ids.reserve(m_connections.size());
		for (auto const& [deviceId, connection] : m_connections)
		{
			// The same question IsConnectionLive asks, answered under the map lock
			// this loop already holds.
			if (connection == nullptr)
				continue;

			{
				const std::lock_guard<std::mutex> connectionGuard(connection->mutex);
				if (connection->closed)
					continue;
			}

			ids.push_back(deviceId);
		}

		return ids;
	}

	size_t AudioPlaybackService::LiveConnectionCount() const
	{
		const std::lock_guard<std::mutex> guard(m_connectionsMutex);
		return m_connections.size();
	}

	bool AudioPlaybackService::HasConnectedDevice() const
	{
		for (auto const& entry : m_devices)
		{
			if (entry.state == DeviceState::Connected && IsConnected(entry.id))
				return true;
		}

		return false;
	}

	void AudioPlaybackService::Notify()
	{
		if (!m_onChanged)
			return;

		try
		{
			m_onChanged();
		}
		CATCH_LOG();
	}

	void AudioPlaybackService::SetState(std::wstring const& deviceId, DeviceState state, std::wstring message)
	{
		if (auto entry = Find(deviceId))
		{
			entry->state = state;
			entry->message = std::move(message);
		}
		else if (state != DeviceState::Disconnected)
		{
			// The device disappeared from the enumeration but a connection
			// attempt is still in flight - keep surfacing it to the UI.
			DeviceEntry pending;
			pending.id = deviceId;
			pending.name = deviceId;
			pending.state = state;
			pending.message = std::move(message);
			m_devices.push_back(std::move(pending));
		}

		// Every state a connect attempt can end in passes through here, so this is
		// where an outstanding request learns whether it is over: connected ends it,
		// and so does an attempt that used up its budget.
		if (state != DeviceState::Connecting)
			CompleteReconnect(deviceId, state == DeviceState::Connected);

		Notify();
	}

	void AudioPlaybackService::ReportAllDisconnected(std::vector<std::wstring> const& deviceIds)
	{
		// The devices that were streaming: only those are worth reporting as having
		// disconnected, and they are collected before the states below are cleared.
		std::vector<std::wstring> disconnected;

		for (auto const& deviceId : deviceIds)
		{
			if (auto entry = Find(deviceId))
			{
				if (entry->state == DeviceState::Connected)
					disconnected.push_back(deviceId);

				entry->state = DeviceState::Disconnected;
				entry->message.clear();
			}
			else
			{
				// A device that is still being connected has no entry yet (or the
				// enumeration has moved on); it still has to leave the panel
				// showing a connection that is gone.
				DeviceEntry pending;
				pending.id = deviceId;
				pending.name = deviceId;
				pending.state = DeviceState::Disconnected;
				m_devices.push_back(std::move(pending));
			}
		}

		// One notification for the whole set: the caller has already released these
		// connections, so the panel is told about all of them at once.
		Notify();

		for (auto const& deviceId : disconnected)
			ReportConnectionEvent(deviceId, false);
	}

	void AudioPlaybackService::OnConnectionStateChanged(
		std::shared_ptr<Connection> const& connection, std::wstring deviceId, bool open)
	{
		// Always on the dispatcher: the device list and the panel are only touched
		// on the thread they belong to.
		//
		// The report is applied to the connection that raised it, and only while that
		// connection is still the one this device holds: re-resolving the connection
		// from the device id alone records one connection's state change against
		// another device's row.
		if (connection == nullptr || FindConnection(deviceId) != connection || !IsConnectionLive(connection))
		{
			LogTrace(L"dropping a state change reported for " + deviceId
				+ L": its connection is no longer the live one");
			return;
		}

		LogTrace(std::wstring(open
			? L"the device came back for "
			: L"the device dropped the connection for ") + deviceId);

		const auto* entry = Find(deviceId);
		const bool wasConnected = entry != nullptr && entry->state == DeviceState::Connected;

		if (open)
		{
			// The platform reports the state on every connection of this process, so
			// "this connection says Opened" is not by itself evidence that *this*
			// device is the one streaming. Only the device the sink can actually be
			// carrying is marked, so that exactly one row turns connected and exactly
			// one notification names the device that did.
			if (!IsOpenForThisDevice(connection, deviceId))
			{
				LogTrace(L"ignoring the shared sink transport's open for " + deviceId
					+ L": this device is not the one being carried");
				return;
			}

			SetConnectionOpen(connection, /* open */ true);

			// The device took the invitation: this is an open now, not the standing
			// invitation the allow-paired switch put out, so switching that switch off
			// must not take it down. Cleared only for the connection the open belongs
			// to; a sibling that merely saw the process-wide report keeps its
			// invitation, and its own report is dropped above.
			{
				const std::lock_guard<std::mutex> guard(connection->mutex);
				connection->advertised = false;
			}

			MarkConnected(deviceId);
			return;
		}

		// This is the process-wide report, raised on every connection of this process
		// whenever any one of them is torn down. Measured: closing or releasing one
		// connection raises StateChanged(Closed) on the other while that other
		// device's Bluetooth link stays up and its audio keeps playing.
		//
		// Only a device whose own link is verifiably down has actually stopped, so
		// its own link decides. Here it is answered from the sweep's last reading
		// rather than read on the dispatcher thread.
		const std::wstring address = AddressForDevice(deviceId);
		const auto link = address.empty() ? std::optional<bool>{} : CachedLink(address);

		if (link.has_value())
			LogTrace(L"link cache hit for " + deviceId + L": its link is " + std::wstring(LinkText(link)));
		else
			LogTrace(L"link cache miss for " + deviceId + L": its connection's own evidence decides");

		// Silence is not a drop: the connection state is the Bluetooth connection, so
		// a device that is linked but not delivering audio is still connected here.
		const bool droppedForThisDevice = link.has_value()
			? !*link
			: (IsOpenRequested(connection) || IsAdvertised(connection));

		if (!droppedForThisDevice)
		{
			// A sibling was released and the platform reported it here as well. This
			// device is still streaming over its own link, so neither its open flag
			// nor its row is touched.
			LogTrace(L"ignoring the process-wide close reported by " + deviceId
				+ L": this device's link is still up, so a sibling was released");
			return;
		}

		// The connection is done: this device dropped it. It is released rather than
		// kept - a closed AudioPlaybackConnection cannot open again, and the next
		// Connect() builds a fresh one.
		SetConnectionOpen(connection, /* open */ false);
		SetState(deviceId, DeviceState::Disconnected);
		ReleaseConnection(connection);

		if (wasConnected)
			ReportConnectionEvent(deviceId, false);
	}

	void AudioPlaybackService::SweepLinks()
	{
		// What this decides: the connection state is the Bluetooth connection, so a
		// device whose link is up is connected even while it delivers no audio -
		// silence is not a drop. A link that has gone down, on the other hand, may
		// leave a connection still believed to be streaming, because the platform
		// raises no state change once a sibling release has already left this
		// connection reporting Closed. That gap is what this sweep exists for.
		//
		// One pass at a time: a tick that arrives while the previous pass is still
		// asking the Bluetooth stack is dropped, rather than queued behind it. A
		// pass a reader asked for - see IsOpenForThisDevice - is not dropped: it is
		// the one that has to happen, so it runs as this pass instead.
		bool idle = false;
		if (!m_sweepRunning.compare_exchange_strong(idle, true))
			return;

		m_sweepNow = false;

		const auto leaveSweep = wil::scope_exit([this]
			{
				m_sweepRunning = false;

				// A reader that asked for a pass while this one was already running
				// cannot be served by it - the snapshot below was taken before the
				// request - so it is carried into one more pass rather than dropped.
				// That pass clears the flag before it repeats.
				if (m_sweepNow && !m_stopping)
					SweepLinks();
			});

		if (m_stopping)
			return;

		// Only the connections believed to be streaming. The snapshot is taken under
		// the map lock; the Bluetooth calls below are made without it, because they
		// block on the Bluetooth stack and must never hold up the connection map.
		std::vector<std::pair<std::wstring, std::shared_ptr<Connection>>> streaming;
		{
			const std::lock_guard<std::mutex> guard(m_connectionsMutex);
			for (auto const& [deviceId, connection] : m_connections)
			{
				if (connection != nullptr && IsConnectionOpen(connection))
					streaming.emplace_back(deviceId, connection);
			}
		}

		// Nothing is streaming: no Bluetooth call, no allocation and no log.
		if (streaming.empty())
			return;

		std::vector<std::pair<std::wstring, std::shared_ptr<Connection>>> dropped;
		std::vector<std::pair<std::wstring, std::optional<bool>>> readings;

		for (auto const& [deviceId, connection] : streaming)
		{
			const std::wstring address = AddressForDevice(deviceId);
			if (address.empty())
				continue;

			const auto link = BluetoothLink::IsLinkConnected(address);

			// Recorded for the dispatcher, which answers whether a device is the
			// carried one without touching the radio itself.
			readings.emplace_back(address, link);

			// A link that could not be read at all - no radio, or a device the
			// adapter does not know - is not evidence of anything, so nothing is torn
			// down on it. A link that is up is the normal case and is not reported.
			if (!link.has_value() || *link)
				continue;

			LogTrace(L"link watch: the link of " + deviceId + L" is down; releasing");
			dropped.emplace_back(deviceId, connection);
		}

		if (!readings.empty())
		{
			const auto readAt = std::chrono::steady_clock::now();
			const std::lock_guard<std::mutex> guard(m_connectionsMutex);

			for (auto const& [address, link] : readings)
				m_linkCache.insert_or_assign(address, LinkReading{ link, readAt });
		}

		// One line per pass that read anything, so the watch can be seen running
		// and its readings can be seen reaching the cache.
		if (IsTraceEnabled())
		{
			LogTrace(L"link watch: the pass read " + std::to_wstring(readings.size())
				+ L" link(s) and found " + std::to_wstring(dropped.size()) + L" down");
		}

		if (dropped.empty())
			return;

		// The drop is applied where the device list and the panel live. A pass that
		// stopped in the meantime hands nothing over.
		const auto dispatcher = m_dispatcher;
		if (dispatcher == nullptr || m_stopping)
			return;

		dispatcher.TryEnqueue([this, dropped = std::move(dropped)]
			{
				for (auto const& [deviceId, connection] : dropped)
					ReleaseDroppedConnection(deviceId, connection);
			});
	}

	void AudioPlaybackService::ReleaseDroppedConnection(
		std::wstring deviceId, std::shared_ptr<Connection> const& connection)
	{
		// The pass ran against a snapshot: the device may have been released, or
		// connected again, before this reached the dispatcher. Only the connection
		// still held for this device, still live and still believed to be streaming,
		// is taken down - which is also what keeps a link found down twice from
		// releasing the same connection twice.
		if (FindConnection(deviceId) != connection
			|| !IsConnectionLive(connection)
			|| !IsConnectionOpen(connection))
		{
			return;
		}

		// The link is read again, on this thread: the pass that found it down ran up
		// to a dispatcher hop ago, and a device that reconnected in between is
		// streaming over a link that is up again - the same re-read the event-driven
		// drop makes. Only a link still verifiably down releases; one that is up
		// again leaves the connection alone, and one that cannot be read is not
		// evidence of a drop and leaves it alone too. This read is not the
		// dispatcher's usual problem: it happens once, for a device the sweep has
		// already found down, and it is what keeps a reconnect from being torn down.
		const std::wstring address = AddressForDevice(deviceId);
		const auto link = address.empty()
			? std::optional<bool>{}
			: BluetoothLink::IsLinkConnected(address);

		if (!link.has_value())
		{
			LogTrace(L"link watch: the link of " + deviceId
				+ L" can no longer be read; leaving its connection alone");
			return;
		}

		if (*link)
		{
			LogTrace(L"link watch: the link of " + deviceId
				+ L" is up again; leaving its connection alone");
			return;
		}

		LogTrace(L"link watch: the link of " + deviceId + L" is still down; releasing");

		// The same ordering as the event-driven drop: clear the open flag, mark the
		// row, release the connection, then report the edge it made.
		const auto* entry = Find(deviceId);
		const bool wasConnected = entry != nullptr && entry->state == DeviceState::Connected;

		SetConnectionOpen(connection, /* open */ false);
		SetState(deviceId, DeviceState::Disconnected);
		ReleaseConnection(connection);

		if (wasConnected)
			ReportConnectionEvent(deviceId, false);
	}

	void AudioPlaybackService::StopLinkSweep()
	{
		m_stopping = true;

		if (m_linkSweep != nullptr)
		{
			try
			{
				m_linkSweep.Cancel();
			}
			CATCH_LOG();

			m_linkSweep = nullptr;
		}

		// A pass that is already inside the Bluetooth stack cannot be cancelled, so
		// it is waited for - but only briefly, because nothing below needs it to have
		// finished: m_stopping is already set, so a pass that is still reading
		// records nothing new and hands no drop over, and a drop that did reach the
		// dispatcher re-reads the link before it acts. Waiting longer would only eat
		// into the exit watchdog's budget, and a watchdog that fires mid-teardown is
		// what leaves the A2DP profile refusing the next connection with 0x8007001F.
		for (unsigned waited = 0; m_sweepRunning && waited < kLinkSweepStopWaitMs; waited += 10)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));

		if (m_sweepRunning)
			LogFailure(L"link watch", L"a pass is still in flight; releasing the connections anyway");
	}

	void AudioPlaybackService::MarkConnected(std::wstring const& deviceId)
	{
		// The state is read before it is set: a device is announced once, when it
		// starts streaming, whatever path noticed - the open this application asked
		// for, or the state change that says the device began on its own.
		const auto* entry = Find(deviceId);
		const bool wasConnected = entry != nullptr && entry->state == DeviceState::Connected;

		// The connection of this device and no other: this is what IsConnected()
		// answers from, so the row that turns connected is this device's row alone.
		SetConnectionOpen(FindConnection(deviceId), /* open */ true);

		SetState(deviceId, DeviceState::Connected);

		if (!wasConnected)
			ReportConnectionEvent(deviceId, true);
	}

	void AudioPlaybackService::ReportConnectionEvent(std::wstring const& deviceId, bool connected)
	{
		if (!m_onConnectionEvent)
			return;

		std::wstring name = deviceId;
		if (const auto* entry = Find(deviceId); entry != nullptr && !entry->name.empty())
			name = entry->name;

		try
		{
			m_onConnectionEvent(name, connected);
		}
		CATCH_LOG();
	}

	void AudioPlaybackService::SubscribeStateChanges(
		std::shared_ptr<Connection> const& connection, std::wstring const& deviceId)
	{
		const auto dispatcher = m_dispatcher;

		// The state notifications are the only thing that tells the panel a device
		// went away, or came back without the user asking. The callback runs on a
		// media-stack thread and does exactly two things: read the state while the
		// connection is provably not closed, and hand the fact to the dispatcher. It
		// never blocks, because a state change arrives on an audio service thread
		// that must not be held up.
		connection->token = connection->handle.StateChanged(
			[this, connection, deviceId, dispatcher](AudioPlaybackConnection const& sender, winrt::Windows::Foundation::IInspectable const&)
			{
				bool open = false;

				// The id that is reported is the sender's own: DeviceId() is the
				// identity the connection object carries, so it cannot be the wrong
				// device's however the subscription was installed. The captured id is
				// only the fallback for a sender that reports none, and it is this
				// subscription's own device, captured by value - never a loop variable
				// and never a "current device" shared between connections.
				std::wstring raisedBy = deviceId;
				{
					// Under this lock, either teardown has not marked the connection
					// closed yet and the reads below are safe, or it has and they never
					// happen. Close() can therefore never take place before them.
					const std::lock_guard<std::mutex> guard(connection->mutex);
					if (connection->closed)
						return;

					open = sender.State() == AudioPlaybackConnectionState::Opened;

					try
					{
						const auto senderId = sender.DeviceId();
						if (!senderId.empty())
							raisedBy = std::wstring(senderId);
					}
					CATCH_LOG();
				}

				if (dispatcher == nullptr)
					return;

				dispatcher.TryEnqueue([this, connection, raisedBy, open]
					{
						OnConnectionStateChanged(connection, raisedBy, open);
					});
			});
	}

	void AudioPlaybackService::SetAllowPaired(bool allow)
	{
		if (m_dispatcher && !m_dispatcher.HasThreadAccess())
		{
			m_dispatcher.TryEnqueue([this, allow] { SetAllowPaired(allow); });
			return;
		}

		m_allowPaired = allow;

		if (allow)
		{
			// Every device is invited again: turning the switch on is what re-reads
			// the device list, and it is also the retry a device whose invitation
			// failed earlier gets.
			m_advertiseFailed.clear();
			AdvertiseDevices();
			return;
		}

		// Off: exactly the connections that are still nothing but an invitation go,
		// through the one teardown path every other close uses. A device that has
		// already started streaming is not one of them - its invitation was cleared
		// when it opened - so its connection is never released here, and neither is a
		// connection an active connect has asked to open: that request is the
		// difference between a device only being invited and one the user or the
		// settings file asked for, and turning this switch off must not cancel it.
		// Releasing an invitation takes that device's connection down and nothing
		// else: the platform tears each connection down on its own, so a device that
		// is streaming here is unaffected.
		std::vector<std::shared_ptr<Connection>> advertised;
		{
			const std::lock_guard<std::mutex> guard(m_connectionsMutex);

			for (auto const& [deviceId, connection] : m_connections)
			{
				if (IsAdvertised(connection) && !IsOpenRequested(connection))
					advertised.push_back(connection);
			}
		}

		for (auto const& connection : advertised)
			ReleaseConnection(connection);
	}

	void AudioPlaybackService::AdvertiseDevices()
	{
		if (!m_allowPaired)
			return;

		for (auto const& entry : m_devices)
		{
			// A device that already has a connection - opened by the user, being
			// opened, or already invited - has nothing to add.
			if (FindConnection(entry.id) != nullptr)
				continue;

			AdvertiseDevice(entry.id);
		}
	}

	void AudioPlaybackService::AdvertiseDevice(std::wstring const& deviceId)
	{
		if (!m_allowPaired || FindConnection(deviceId) != nullptr)
			return;

		// See m_advertiseFailed: the pass that follows a failed invitation must not
		// invite the same device again.
		if (m_advertiseFailed.find(deviceId) != m_advertiseFailed.end())
			return;

		const auto connection = std::make_shared<Connection>();

		// Created here, on the dispatcher thread, so the object belongs to the
		// apartment the application lives in; starting it is handed to a worker
		// thread below.
		try
		{
			connection->handle = AudioPlaybackConnection::TryCreateFromId(deviceId);
		}
		catch (winrt::hresult_error const& ex)
		{
			LOG_CAUGHT_EXCEPTION();
			LogFailure(L"Allow paired devices", deviceId + L": " + FormatHresult(ex));
			return;
		}

		if (connection->handle == nullptr)
		{
			LogFailure(L"Allow paired devices", deviceId + L": the device has no A2DP sink connection");
			return;
		}

		// Claimed before it is started, so a second pass while the first start is
		// still in flight does nothing.
		{
			const std::lock_guard<std::mutex> guard(m_connectionsMutex);
			if (m_connections.find(deviceId) != m_connections.end())
				return;

			m_connections[deviceId] = connection;
		}

		{
			const std::lock_guard<std::mutex> guard(connection->mutex);
			connection->advertised = true;
		}

		LogTrace(L"invited " + deviceId + L": Start only, never Open");

		SubscribeStateChanges(connection, deviceId);

		// Only Start(), never OpenAsync(): the device is being invited to stream, not
		// made to. Start - like the open - blocks in the media stack, so it runs on a
		// thread of this process rather than on the dispatcher's.
		const auto dispatcher = m_dispatcher;
		std::thread([this, connection, deviceId, dispatcher]
			{
				bool started = false;

				try
				{
					// Immediately before the handle is used, and under the lock
					// teardown takes: a release landing while this thread was
					// setting up left the connection closed and the handle null,
					// and Start() on either faults inside
					// Windows.Media.Devices.dll (0xC0000005 at +0x4D9BC). The
					// reference is taken here rather than after the guard so that
					// a handle nulled in between cannot be dereferenced.
					AudioPlaybackConnection handle{ nullptr };
					{
						const std::lock_guard<std::mutex> guard(connection->mutex);
						if (connection->closed)
							return;

						handle = connection->handle;
					}

					if (handle == nullptr)
						return;

					handle.StartAsync().get();
					started = true;
				}
				catch (winrt::hresult_error const& ex)
				{
					LOG_CAUGHT_EXCEPTION();
					LogFailure(L"Allow paired devices", deviceId + L": " + FormatHresult(ex));
				}

				if (started)
				{
					const std::lock_guard<std::mutex> guard(connection->mutex);
					connection->started = true;
				}

				if (dispatcher == nullptr)
					return;

				dispatcher.TryEnqueue([this, connection, deviceId, started]
					{
						OnAdvertiseFinished(deviceId, connection, started);
					});
			}).detach();
	}

	void AudioPlaybackService::OnAdvertiseFinished(
		std::wstring const& deviceId, std::shared_ptr<Connection> const& connection, bool started)
	{
		// An invitation that could not be started is given up rather than kept.
		if (started)
			return;

		// Recorded before the close below, because that close is what asks for the
		// next invitation pass, and this device must not be part of it.
		m_advertiseFailed.insert(deviceId);

		ReleaseConnection(connection);
	}

	void AudioPlaybackService::OnTeardownFinished()
	{
		if (!m_allowPaired)
			return;

		// Runs on the closing thread; the device list and the connection map of a
		// pass belong to the dispatcher.
		if (m_dispatcher == nullptr)
			return;

		m_dispatcher.TryEnqueue([this]
			{
				if (m_allowPaired)
					AdvertiseDevices();
			});
	}

	void AudioPlaybackService::OnDeviceRemoved(std::wstring deviceId)
	{
		if (m_dispatcher && !m_dispatcher.HasThreadAccess())
		{
			m_dispatcher.TryEnqueue([this, deviceId] { OnDeviceRemoved(std::move(deviceId)); });
			return;
		}

		// A device that has gone away takes its connection with it; keeping it would
		// leave the sink enabled for a device that is no longer there.
		if (const auto connection = FindConnection(deviceId))
		{
			LogTrace(L"device removed; releasing its connection: " + deviceId);
			SetState(deviceId, DeviceState::Disconnected);
			ReleaseConnection(connection);
		}
	}

	winrt::Windows::Foundation::IAsyncAction AudioPlaybackService::RefreshOnceAsync()
	{
		try
		{
			auto selector = AudioPlaybackConnection::GetDeviceSelector();
			const auto requested = winrt::single_threaded_vector<winrt::hstring>({ L"System.Devices.GlyphIcon" });
			auto deviceInfos = co_await DeviceInformation::FindAllAsync(selector, requested);

			if (IsTraceEnabled())
			{
				LogTrace(L"refresh found " + std::to_wstring(deviceInfos.Size()) + L" device(s)");
			}

			std::vector<DeviceEntry> devices;
			devices.reserve(deviceInfos.Size());

			for (auto const& info : deviceInfos)
			{
				DeviceEntry entry;
				entry.id = std::wstring(info.Id());
				entry.name = std::wstring(info.Name());
				entry.icon = IconForDevice(info);

				// Keep whatever live connection state we already know about.
				if (auto const* existing = Find(entry.id))
				{
					entry.state = existing->state;
					entry.message = existing->message;
				}

				if (IsTraceEnabled())
				{
					LogTrace(L"device icon='" + entry.icon + L"' name=" + entry.name
						+ L" id=" + entry.id);
				}

				devices.push_back(std::move(entry));
			}

			m_devices = std::move(devices);

			// A device that is gone is forgotten, so pairing it again gets a fresh
			// attempt; one that is still listed keeps the single attempt it already
			// had - see m_advertiseFailed.
			for (auto it = m_advertiseFailed.begin(); it != m_advertiseFailed.end();)
			{
				if (Find(*it) == nullptr)
					it = m_advertiseFailed.erase(it);
				else
					++it;
			}

			Notify();

			// A device that appeared while the allow-paired switch is on is invited
			// as soon as it is known, so the switch does not depend on what happened
			// to be enumerated when it was turned on.
			AdvertiseDevices();

			// The pass that answers for a device the enumeration had not described when
			// the request was made; the attempt is made here rather than dropped there.
			ReconnectPending();
		}
		catch (winrt::hresult_error const& ex)
		{
			LOG_CAUGHT_EXCEPTION();
			LogFailure(L"Device enumeration", std::wstring(ex.message()));
		}
	}

	winrt::fire_and_forget AudioPlaybackService::RefreshDevicesAsync()
	{
		if (m_dispatcher && !m_dispatcher.HasThreadAccess())
		{
			m_dispatcher.TryEnqueue([this] { RefreshDevicesAsync(); });
			co_return;
		}

		// Pairing or connecting a device makes the watcher report several changes in a
		// row. One pass already reads the whole current device set, so a request that
		// arrives while a pass is running only asks for one more pass.
		if (m_refreshRunning)
		{
			m_refreshAgain = true;
			co_return;
		}

		m_refreshRunning = true;
		const auto clearRunning = wil::scope_exit([this] { m_refreshRunning = false; });

		do
		{
			m_refreshAgain = false;
			co_await RefreshOnceAsync();
		} while (m_refreshAgain);
	}

	void AudioPlaybackService::Connect(std::wstring deviceId)
	{
		// The panel's Connect button: an active connect, exactly like a saved
		// reconnect, because that is the path the owner confirms produces sound.
		ConnectAsync(std::move(deviceId), /* reconnect */ false);
	}

	winrt::fire_and_forget AudioPlaybackService::ConnectAsync(std::wstring deviceId, bool reconnect)
	{
		if (m_dispatcher && !m_dispatcher.HasThreadAccess())
		{
			m_dispatcher.TryEnqueue([this, deviceId, reconnect] { ConnectAsync(deviceId, reconnect); });
			co_return;
		}

		std::shared_ptr<Connection> connection = FindConnection(deviceId);

		// An open connection is the one thing there is nothing to do for. Closing a
		// connection takes that device's A2DP sink stream down and no other device's,
		// so an already open connection is never released, and a connection that is
		// not open yet is promoted below instead of replaced. The invitation state
		// deliberately does not enter into it: asking an invited device to connect IS
		// the request the allow-paired switch was waiting for.
		if (IsConnectionOpen(connection))
			co_return;

		if (connection != nullptr)
		{
			if (IsConnectionLive(connection))
			{
				// Promoted instead of replaced. An invitation is started but never
				// opened, so the request turns it into an open on the very object the
				// invitation started. Replacing it would have to close it first.
				{
					const std::lock_guard<std::mutex> guard(connection->mutex);
					connection->advertised = false;
				}

				LogTrace(L"promoting the connection of " + deviceId
					+ (reconnect ? L" for a reconnect" : L" for the panel"));
			}
			else
			{
				// Anything else - a connection whose open failed, or one the device
				// dropped - is released and replaced: a device with no live
				// connection simply gets connected again.
				ReleaseConnection(connection);
				connection = nullptr;
			}
		}

		SetState(deviceId, DeviceState::Connecting);

		if (connection == nullptr)
		{
			// Opening needs the device's A2DP service installed. It normally is, but
			// Windows' own device properties or another application may have switched it
			// off, and then the connection cannot be opened at all - so ask first, and
			// only repair what is missing.
			const std::wstring address = AddressForDevice(deviceId);
			try
			{
				const auto serviceInstalled = BluetoothLink::IsAudioServiceInstalled(address);
				LogTrace(L"A2DP service installed="
					+ std::wstring(!serviceInstalled ? L"unknown" : (*serviceInstalled ? L"yes" : L"no"))
					+ L" for " + deviceId);
				if (!serviceInstalled || !*serviceInstalled)
					SetAudioServiceEnabled(address, true);
			}
			catch (winrt::hresult_error const& ex)
			{
				LOG_CAUGHT_EXCEPTION();
				LogFailure(L"A2DP service", FormatHresult(ex));
			}

			connection = std::make_shared<Connection>();

			// Created here, on the dispatcher thread, so that the object belongs to the
			// apartment the application lives in; starting and opening it is handed to a
			// worker thread below.
			try
			{
				connection->handle = AudioPlaybackConnection::TryCreateFromId(deviceId);
			}
			catch (winrt::hresult_error const& ex)
			{
				LOG_CAUGHT_EXCEPTION();
				LogFailure(L"Connect", deviceId + L": " + FormatHresult(ex));
				SetState(deviceId, DeviceState::Failed, FormatHresult(ex));
				co_return;
			}

			if (connection->handle == nullptr)
			{
				LogFailure(L"Connect", deviceId + L": the device has no A2DP sink connection");
				SetState(deviceId, DeviceState::Failed, L"0x80004005");
				co_return;
			}

			// Claimed before the connection is opened, so a second request for the same
			// device while the first is still opening does nothing.
			{
				const std::lock_guard<std::mutex> guard(m_connectionsMutex);
				m_connections[deviceId] = connection;
			}

			SubscribeStateChanges(connection, deviceId);
		}

		const auto dispatcher = m_dispatcher;

		// A promoted connection has already been started, and a started connection
		// is not started twice.
		bool startRequired = false;
		{
			const std::lock_guard<std::mutex> guard(connection->mutex);
			startRequired = !connection->started;
		}

		// Start and open run on a thread of this process rather than on the dispatcher,
		// because both block - the open is retried and State() is watched to confirm
		// it - and the panel is on the dispatcher's thread. A plain std::thread is
		// initialised as MTA, which is where the WinRT calls below belong.
		std::thread([this, connection, deviceId, dispatcher, startRequired]
		{
			HRESULT result = E_FAIL;
			std::wstring detail;

			try
			{
				// Marked with the open about to be issued rather than when the
				// platform reports it, so the window in between is covered: the
				// allow-paired switch must not release a connection an active connect
				// is opening, and this flag is what tells the two apart there.
				{
					const std::lock_guard<std::mutex> guard(connection->mutex);
					connection->openRequested = true;
				}

				// Immediately before the handle is used, and under the lock teardown
				// takes: a release landing while this thread was preparing the open
				// left the connection closed and the handle null, and Start() on
				// either faults inside Windows.Media.Devices.dll (0xC0000005 at
				// +0x4D9BC).
				AudioPlaybackConnection startHandle{ nullptr };
				{
					const std::lock_guard<std::mutex> guard(connection->mutex);
					if (connection->closed)
						return;

					startHandle = connection->handle;
				}

				if (startRequired)
				{
					if (startHandle == nullptr)
						return;

					startHandle.StartAsync().get();

					const std::lock_guard<std::mutex> guard(connection->mutex);
					connection->started = true;
				}

				for (unsigned attempt = 1; attempt <= kOpenAttempts; ++attempt)
				{
					// The same re-check immediately before the open. It also closes
					// the Start-then-Open window: an OpenAsync may land one to two
					// seconds after its StartAsync, well after a teardown began.
					AudioPlaybackConnection openHandle{ nullptr };
					{
						const std::lock_guard<std::mutex> guard(connection->mutex);
						if (connection->closed)
							return;

						openHandle = connection->handle;
					}

					if (openHandle == nullptr)
						return;

					try
					{
						const auto opened = openHandle.OpenAsync().get();
						const auto status = opened.Status();

						if (status == AudioPlaybackConnectionOpenResultStatus::DeniedBySystem)
						{
							// Nothing about waiting makes the system change its mind.
							result = static_cast<HRESULT>(opened.ExtendedError());
							detail = L"DeniedBySystem " + Hex(static_cast<uint32_t>(result));
							break;
						}

						if (status == AudioPlaybackConnectionOpenResultStatus::Success
							|| status == AudioPlaybackConnectionOpenResultStatus::RequestTimedOut
							|| status == AudioPlaybackConnectionOpenResultStatus::UnknownFailure)
						{
							// Open's result is not the connection's state; the state
							// decides, watched briefly.
							for (unsigned wait = 0; wait < kStateConfirmAttempts; ++wait)
							{
								bool openState = false;
								{
									const std::lock_guard<std::mutex> guard(connection->mutex);
									if (connection->closed || connection->handle == nullptr)
										return;

									openState = connection->handle.State() == AudioPlaybackConnectionState::Opened;
								}

								if (openState)
								{
									result = S_OK;
									break;
								}

								std::this_thread::sleep_for(std::chrono::milliseconds(kStateConfirmDelayMs));
							}

							if (SUCCEEDED(result))
								break;

							if (status == AudioPlaybackConnectionOpenResultStatus::Success)
							{
								detail = L"the connection did not open";
							}
							else
							{
								const uint32_t extended = static_cast<uint32_t>(opened.ExtendedError());
								result = extended != 0 ? static_cast<HRESULT>(extended) : E_FAIL;
								detail = Hex(static_cast<uint32_t>(result));
							}
						}
					}
					catch (winrt::hresult_error const& ex)
					{
						LOG_CAUGHT_EXCEPTION();
						result = ex.code();
						detail = FormatHresult(ex);
					}

					if (attempt < kOpenAttempts)
						std::this_thread::sleep_for(std::chrono::milliseconds(kOpenRetryDelayMs));
				}
			}
			catch (winrt::hresult_error const& ex)
			{
				LOG_CAUGHT_EXCEPTION();
				result = ex.code();
				detail = FormatHresult(ex);
			}

			// The request went out; whether the device started is a different question
			// the platform's process-wide state cannot answer, so its own link is asked.
			bool linkDown = false;

			if (SUCCEEDED(result))
			{
				const std::wstring address = AddressForDevice(deviceId);
				const auto link = address.empty()
					? std::optional<bool>{}
					: BluetoothLink::IsLinkConnected(address);

				LogTrace(L"open requested for " + deviceId + L"; its link is "
					+ std::wstring(LinkText(link)));

				linkDown = link.has_value() && !*link;

				if (linkDown)
					detail = L"the device did not start streaming (its link is down)";
			}

			if (dispatcher == nullptr)
				return;

			// Recorded on the dispatcher, which is the thread the connection map,
			// the device list and the panel belong to.
			dispatcher.TryEnqueue([this, deviceId, connection, result, detail, linkDown]
			{
				OnConnectFinished(deviceId, connection, result, detail, linkDown);
			});
		}).detach();
	}

	void AudioPlaybackService::OnConnectFinished(
		std::wstring deviceId,
		std::shared_ptr<Connection> const& connection,
		HRESULT result,
		std::wstring const& detail,
		bool linkDown)
	{
		// The device may have been disconnected while the open was in flight, in
		// which case this is no longer its connection and the result is thrown away.
		if (FindConnection(deviceId) != connection)
			return;

		// An open the device did not answer with a link is not streaming, whatever
		// the open itself reported, so it is not connected: the connection is let go
		// and the attempt recorded, which leaves an outstanding reconnect request
		// waiting to try again rather than a row claiming audio that is not there.
		if (SUCCEEDED(result) && !linkDown)
		{
			MarkConnected(deviceId);
			return;
		}

		// The connection that failed is dropped here; releasing it closes nothing
		// that was ever open.
		ReleaseConnection(connection);

		if (SUCCEEDED(result))
		{
			LogFailure(L"Connect", deviceId + L": " + detail);
			SetState(deviceId, DeviceState::Failed, detail);
			return;
		}

		LogFailure(L"Connect", deviceId + L": " + (detail.empty() ? Hex(static_cast<uint32_t>(result)) : detail));

		// The code Windows returned is what the panel shows, verbatim and
		// untranslated - "0x8007001f" and the like - because it is the reason the
		// open actually failed and nothing else knows it.
		SetState(deviceId, DeviceState::Failed, detail.empty() ? Hex(static_cast<uint32_t>(result)) : detail);
	}

	void AudioPlaybackService::Disconnect(std::wstring deviceId)
	{
		LogTrace(L"disconnect requested for " + deviceId);

		// One device, one connection. Closing a connection raises the process-wide
		// StateChanged(Closed) on this process's other connections as well, but it
		// takes only this one down: measurement has the other devices' Bluetooth
		// links stay up and their audio keep playing.
		const auto connection = FindConnection(deviceId);
		if (connection == nullptr)
		{
			// Nothing is held for it: the row is put back to disconnected and the
			// panel is told, which is all a device without a connection needs.
			SetState(deviceId, DeviceState::Disconnected);
			Notify();
			return;
		}

		// Off the UI thread, unlike Stop(): the panel is live and the user is waiting
		// for it, and ReleaseConnection hands the close to a thread of its own.
		ReleaseConnection(connection);

		// Only this device is reported as disconnected. Its own StateChanged callback
		// will also fire on the close and is harmless: this connection is no longer
		// the live one for the device, so the report is dropped.
		ReportAllDisconnected({ deviceId });
	}

	void AudioPlaybackService::Unpair(std::wstring deviceId)
	{
		UnpairAsync(std::move(deviceId));
	}

	winrt::fire_and_forget AudioPlaybackService::UnpairAsync(std::wstring deviceId)
	{
		if (m_dispatcher && !m_dispatcher.HasThreadAccess())
		{
			m_dispatcher.TryEnqueue([this, deviceId] { UnpairAsync(deviceId); });
			co_return;
		}

		try
		{
			// Stop streaming first: the pairing is about to be deleted, and the
			// connection would be torn down underneath the audio graph anyway.
			if (const auto connection = FindConnection(deviceId))
				ReleaseConnection(connection);

			SetState(deviceId, DeviceState::Disconnected);

			using winrt::Windows::Devices::Bluetooth::BluetoothDevice;

			// The AudioPlaybackConnection id is a device interface path, which
			// carries the Bluetooth address but is not a DeviceInformation id, so
			// the pairing is matched by that address instead.
			auto const bluetoothDevices = co_await DeviceInformation::FindAllAsync(
				BluetoothDevice::GetDeviceSelector());

			bool matched = false;

			for (auto const& info : bluetoothDevices)
			{
				auto const bluetooth = co_await BluetoothDevice::FromIdAsync(info.Id());
				if (!bluetooth)
					continue;

				if (!ContainsDigits(deviceId, AddressDigits(bluetooth.BluetoothAddress())))
					continue;

				matched = true;

				auto const result = co_await bluetooth.DeviceInformation().Pairing().UnpairAsync();
				if (IsTraceEnabled())
				{
					LogTrace(L"unpair status=" + std::to_wstring(static_cast<int>(result.Status())));
				}

				if (result.Status() != winrt::Windows::Devices::Enumeration::DeviceUnpairingResultStatus::Unpaired)
				{
					SetState(deviceId, DeviceState::Failed, _(L"The device could not be removed"));
				}

				break;
			}

			if (!matched)
			{
				LOG_IF_FAILED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
				SetState(deviceId, DeviceState::Failed, _(L"The device could not be removed"));
			}
		}
		catch (winrt::hresult_error const& ex)
		{
			LOG_CAUGHT_EXCEPTION();
			SetState(deviceId, DeviceState::Failed, _(L"The device could not be removed"));
			(void)ex;
		}

		RefreshDevicesAsync();
	}

	void AudioPlaybackService::ReconnectSaved()
	{
		// Active connect, one request per device: every device the user asked for is
		// noted here, and ReconnectPending carries each one out - at once for a
		// device the enumeration already answered for, and as soon as it appears
		// otherwise. Nothing here is allowed to drop a device (see ReconnectPending).
		if (IsTraceEnabled())
		{
			LogTrace(L"reconnect: " + std::to_wstring(Settings().reconnectDevices.size())
				+ L" saved device(s), " + std::to_wstring(m_devices.size()) + L" enumerated");
		}

		for (auto const& deviceId : Settings().reconnectDevices)
			EnsureReconnectRequest(deviceId);

		ReconnectPending();
	}

	void AudioPlaybackService::EnsureReconnectRequest(std::wstring const& deviceId)
	{
		const std::lock_guard<std::mutex> guard(m_reconnectMutex);

		// Put back to zero attempts: the request belongs to this launch, and this is
		// the launch that makes it.
		m_reconnectRequests[deviceId] = ReconnectRequest{};
	}

	void AudioPlaybackService::RecordReconnectAttempt(std::wstring const& deviceId)
	{
		const std::lock_guard<std::mutex> guard(m_reconnectMutex);

		if (const auto it = m_reconnectRequests.find(deviceId); it != m_reconnectRequests.end())
			++it->second.attempts;
	}

	void AudioPlaybackService::CompleteReconnect(std::wstring const& deviceId, bool connected)
	{
		// Read outside the lock: the device list belongs to the dispatcher this also
		// runs on, and taking the two locks together here would order them against
		// everything that takes them the other way round.
		if (!connected)
		{
			const auto* entry = Find(deviceId);
			connected = entry != nullptr && entry->state == DeviceState::Connected;
		}

		const std::lock_guard<std::mutex> guard(m_reconnectMutex);

		const auto it = m_reconnectRequests.find(deviceId);
		if (it == m_reconnectRequests.end())
			return;

		// Connected ends the request: it has been carried out. Still pending but out
		// of attempts ends it too, so a device that never accepts cannot drive an
		// endless open - the panel then shows the attempt's own failure.
		if (!connected && it->second.attempts < kReconnectMaxAttempts)
			return;

		LogTrace(L"reconnect: " + std::wstring(connected ? L"connected " : L"out of attempts for ")
			+ deviceId);
		m_reconnectRequests.erase(it);
	}

	void AudioPlaybackService::ReconnectPending()
	{
		std::vector<std::wstring> wanted;
		{
			const std::lock_guard<std::mutex> guard(m_reconnectMutex);

			wanted.reserve(m_reconnectRequests.size());
			for (auto const& [deviceId, request] : m_reconnectRequests)
			{
				if (request.attempts < kReconnectMaxAttempts)
					wanted.push_back(deviceId);
			}
		}

		// An empty enumeration is no answer for any of them: not one of these ids has
		// been described yet, so this is not the pass that carries the requests out.
		// Only the attempt is held back - never the request itself.
		if (wanted.empty())
			return;

		if (m_devices.empty())
		{
			LogTrace(L"reconnect: the device list has not arrived yet; "
				+ std::to_wstring(wanted.size()) + L" attempt(s) wait for it");
			return;
		}

		size_t deferred = 0;

		// Each device is dealt with on its own, and the loop never leaves early: one
		// device that cannot be attempted yet must not take the rest of the list with
		// it.
		for (auto const& deviceId : wanted)
		{
			// Already open, so the request has been carried out. An invitation is
			// deliberately NOT this case: an invite is exactly the connection the
			// reconnect request has to promote.
			if (IsConnectionOpen(FindConnection(deviceId)))
			{
				LogTrace(L"reconnect: " + deviceId + L" is already open; nothing to attempt");
				continue;
			}

			if (Find(deviceId) == nullptr)
			{
				// No row for it in this pass. The attempt is not made now - an id
				// nothing enumerates has no A2DP service to ask - and not dropped
				// either: the request stays for a later pass, which is the enumeration
				// arriving for this device.
				++deferred;
				continue;
			}

			// Already being connected by an attempt of its own that is still in
			// flight. The connection is not open yet, so this is not the finished
			// case above: a second attempt here would be a second open request on a
			// connection that is already opening, and the attempt that owns it sets
			// the state that ends the wait.
			if (const auto* entry = Find(deviceId); entry != nullptr && entry->state == DeviceState::Connecting)
			{
				LogTrace(L"reconnect: " + deviceId + L" is already being connected; nothing to attempt");
				continue;
			}

			if (IsTraceEnabled())
			{
				// Whether the device had already started is answered by the link the
				// worker reads once the open came back (see ConnectAsync); it is not
				// read here, because this runs on the dispatcher and enumerating the
				// radio from there can hold the panel.
				LogTrace(L"reconnect: attempting " + deviceId);
			}

			RecordReconnectAttempt(deviceId);
			ConnectAsync(deviceId, /* reconnect */ true);
		}

		if (deferred != 0)
		{
			LogTrace(L"reconnect: " + std::to_wstring(deferred)
				+ L" device(s) not enumerated yet; their attempt waits for the device list");
		}
	}
}
