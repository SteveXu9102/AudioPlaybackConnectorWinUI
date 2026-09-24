#include "pch.h"
#include "BluetoothLink.h"
#include "Util.hpp"

namespace
{
	using namespace AudioPlaybackConnectorWinUI;

	// The AudioSource service (A2DP), which is the profile this application receives.
	constexpr GUID kAudioSourceService{ 0x0000110A, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

	std::wstring DescribeError(DWORD error)
	{
		return L"error " + std::to_wstring(error);
	}

	/// <summary>True when the text is twelve hexadecimal digits.</summary>
	bool IsValidAddress(std::wstring_view addressDigits)
	{
		return addressDigits.size() == 12 && std::all_of(addressDigits.begin(), addressDigits.end(),
			[](wchar_t digit) { return iswxdigit(digit) != 0; });
	}

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

	/// <summary>
	/// The device as the Bluetooth APIs know it. The address has to come from a
	/// search rather than being filled in by hand: BluetoothEnumerateInstalledServices
	/// rejects a structure that was not produced this way (ERROR_INVALID_PARAMETER).
	/// </summary>
	std::optional<BLUETOOTH_DEVICE_INFO> FindDevice(HANDLE radio, std::wstring const& addressDigits)
	{
		BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
		search.dwSize = sizeof(search);
		search.fReturnAuthenticated = TRUE;
		search.fReturnRemembered = TRUE;
		search.fReturnConnected = TRUE;
		search.fReturnUnknown = TRUE;
		search.fIssueInquiry = FALSE;
		search.cTimeoutMultiplier = 1;
		search.hRadio = radio;

		const uint64_t address = AddressFromDigits(addressDigits);

		BLUETOOTH_DEVICE_INFO device{};
		device.dwSize = sizeof(device);
		const HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, &device);
		if (find == nullptr)
		{
			LogFailure(L"Bluetooth link",
				L"the device " + addressDigits + L" is not known to the adapter ("
				+ DescribeError(GetLastError()) + L")");
			return std::nullopt;
		}
		const auto closeFind = wil::scope_exit([find] { BluetoothFindDeviceClose(find); });

		do
		{
			if (device.Address.ullLong == address)
				return device;
		} while (BluetoothFindNextDevice(find, &device));

		LogFailure(L"Bluetooth link", L"the device " + addressDigits + L" is not paired with this adapter");
		return std::nullopt;
	}

	/// <summary>
	/// Runs <paramref name="action"/> with the first Bluetooth radio and closes it
	/// afterwards whatever the action did. Answers std::nullopt when there is no
	/// radio, which is what every caller here reports as "cannot be determined".
	/// </summary>
	template <typename TAction>
	std::optional<bool> WithFirstRadio(TAction&& action)
	{
		BLUETOOTH_FIND_RADIO_PARAMS radioParams{ sizeof(radioParams) };
		HANDLE radio = nullptr;
		const HBLUETOOTH_RADIO_FIND findRadio = BluetoothFindFirstRadio(&radioParams, &radio);
		if (findRadio == nullptr)
		{
			LogFailure(L"Bluetooth link", L"no Bluetooth radio was found");
			return std::nullopt;
		}
		const auto closeRadio = wil::scope_exit([radio, findRadio]
		{
			CloseHandle(radio);
			BluetoothFindRadioClose(findRadio);
		});

		return action(radio);
	}
}

namespace AudioPlaybackConnectorWinUI::BluetoothLink
{
	std::optional<bool> IsAudioServiceInstalled(std::wstring const& addressDigits)
	{
		if (!IsValidAddress(addressDigits))
			return std::nullopt;

		return WithFirstRadio([&addressDigits](HANDLE radio) -> std::optional<bool>
		{
			const auto device = FindDevice(radio, addressDigits);
			if (!device)
				return std::nullopt;

			DWORD count = 0;
			DWORD result = BluetoothEnumerateInstalledServices(radio, &*device, &count, nullptr);
			if (result == ERROR_SUCCESS && count == 0)
				return false;
			if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA)
			{
				LogFailure(L"Bluetooth link",
					L"could not list the services of " + addressDigits + L" (" + DescribeError(result) + L")");
				return std::nullopt;
			}

			std::vector<GUID> services(count);
			result = BluetoothEnumerateInstalledServices(radio, &*device, &count, services.data());
			if (result != ERROR_SUCCESS)
			{
				LogFailure(L"Bluetooth link",
					L"could not list the services of " + addressDigits + L" (" + DescribeError(result) + L")");
				return std::nullopt;
			}

			services.resize(count);
			return std::any_of(services.begin(), services.end(),
				[](GUID const& service) { return IsEqualGUID(service, kAudioSourceService) != FALSE; });
		});
	}

	std::optional<bool> IsLinkConnected(std::wstring const& addressDigits)
	{
		if (!IsValidAddress(addressDigits))
			return std::nullopt;

		return WithFirstRadio([&addressDigits](HANDLE radio) -> std::optional<bool>
		{
			const auto device = FindDevice(radio, addressDigits);
			if (!device)
				return std::nullopt;

			return device->fConnected != FALSE;
		});
	}
}
