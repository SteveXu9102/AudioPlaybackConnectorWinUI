#include "pch.h"
#include "StartupRegistration.h"

#include "Util.hpp"

using namespace winrt;

namespace AudioPlaybackConnectorWinUI
{
	namespace
	{
		constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
		/// <summary>
		/// Where Explorer records whether the user switched a Run entry off in Task
		/// Manager. The value is twelve bytes; the first byte is 2 while the entry is
		/// enabled and 3 once it has been disabled there.
		/// </summary>
		constexpr wchar_t kApprovedKey[] =
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
		constexpr wchar_t kValueName[] = L"AudioPlaybackConnectorWinUI";
		constexpr wchar_t kLauncherPathVariable[] = L"APC_LAUNCHER_PATH";

		std::wstring Quote(std::wstring const& path)
		{
			return L"\"" + path + L"\"";
		}

		/// <summary>
		/// The executable a registered command line names, or empty. Parsed the way
		/// the shell parses it, so a quoted path, an unquoted one and one that carries
		/// arguments all give the program rather than the whole line.
		/// </summary>
		std::wstring ExecutableOf(std::wstring const& commandLine)
		{
			int count = 0;
			LPWSTR* arguments = CommandLineToArgvW(commandLine.c_str(), &count);
			if (arguments == nullptr || count == 0)
				return {};

			std::wstring path = arguments[0];
			LocalFree(arguments);
			return path;
		}

		/// <summary>
		/// True when this path is inside the payload the single-file launcher unpacks.
		///
		/// That directory is pruned as soon as a newer package has been unpacked, so a
		/// Run entry pointing into it stops working at the next upgrade and is then
		/// left pointing at a directory that is gone. It is only ever reached when the
		/// application is started straight out of the payload, with no launcher to name
		/// itself.
		/// </summary>
		bool IsInsidePayload(std::wstring const& path)
		{
			wchar_t local[1024]{};
			const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
			if (length == 0 || length >= ARRAYSIZE(local))
				return false;

			const std::wstring root = std::wstring(local) + L"\\AudioPlaybackConnectorWinUI\\app\\";
			if (path.size() < root.size())
				return false;

			return CompareStringOrdinal(path.c_str(), static_cast<int>(root.size()), root.c_str(),
				static_cast<int>(root.size()), TRUE) == CSTR_EQUAL;
		}

		/// <summary>The registered command line, or an empty string when there is none.</summary>
		std::wstring ReadRunValue()
		{
			wchar_t buffer[1024]{};
			DWORD bytes = sizeof(buffer);
			DWORD type = 0;

			const LSTATUS status = RegGetValueW(
				HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, &type, buffer, &bytes);

			if (status == ERROR_MORE_DATA)
			{
				// bytes now holds the size the value needs.
				std::vector<wchar_t> grown(bytes / sizeof(wchar_t) + 1);
				if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, &type,
					grown.data(), &bytes) != ERROR_SUCCESS || type != REG_SZ)
				{
					return {};
				}

				return grown.data();
			}

			if (status != ERROR_SUCCESS || type != REG_SZ)
				return {};

			return buffer;
		}

		/// <summary>Deletes a value from a per-user key, ignoring a missing key or value.</summary>
		void DeleteValue(wchar_t const* key, wchar_t const* name)
		{
			wil::unique_hkey handle;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, handle.put()) != ERROR_SUCCESS)
				return;

			RegDeleteValueW(handle.get(), name);
		}

		/// <summary>
		/// True when Task Manager has this entry switched off.
		///
		/// </summary>
		bool IsDisabledByUser()
		{
			BYTE buffer[64]{};
			DWORD bytes = sizeof(buffer);
			DWORD type = 0;

			LSTATUS status = RegGetValueW(
				HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, &type, buffer, &bytes);

			// A value wider than the buffer is not an answer, and treating it as one
			// reported a switched-off entry as enabled - the exact misreport this
			// function exists to prevent. The first byte is all that is wanted, but
			// the retry still has to offer room for the whole value: asking for one
			// byte returns ERROR_MORE_DATA again for ever, so that "recovery" would
			// always fall through to "not disabled".
			if (status == ERROR_MORE_DATA)
			{
				std::vector<BYTE> grown(bytes);
				DWORD needed = bytes;

				status = RegGetValueW(
					HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, &type, grown.data(), &needed);

				if (status != ERROR_SUCCESS || needed == 0)
					return false;

				return grown[0] != 2;
			}

			if (status != ERROR_SUCCESS || bytes == 0)
				return false;

			return buffer[0] != 2;
		}

		/// <summary>
		/// Writes the Run value so that signing in starts <paramref name="path"/>.
		/// The StartupApproved state is left alone, because the two callers want
		/// different things from it.
		/// </summary>
		bool WriteRunValue(std::wstring const& path)
		{
			wil::unique_hkey key;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, key.put()) != ERROR_SUCCESS)
			{
				LogFailure(L"Start with Windows", L"the Run key could not be opened");
				return false;
			}

			const std::wstring command = Quote(path);
			const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));

			if (RegSetValueExW(
				key.get(), kValueName, 0, REG_SZ,
				reinterpret_cast<BYTE const*>(command.c_str()), bytes) != ERROR_SUCCESS)
			{
				LogFailure(L"Start with Windows", L"the Run value could not be written");
				return false;
			}

			return true;
		}

		/// <summary>Removes the Run value, if there is one.</summary>
		void DeleteRunValue()
		{
			wil::unique_hkey key;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, key.put()) != ERROR_SUCCESS)
				return;

			RegDeleteValueW(key.get(), kValueName);
		}
	}

	std::wstring DistributedExecutablePath()
	{
		wchar_t buffer[MAX_PATH * 4]{};

		const DWORD length = GetEnvironmentVariableW(
			kLauncherPathVariable, buffer, ARRAYSIZE(buffer));
		if (length > 0 && length < ARRAYSIZE(buffer))
		{
			// Checked before it is written into the registry, because it comes from the
			// environment rather than from the module: what the launcher sets is an
			// absolute path to a file that exists, and anything else would register a
			// sign-in command that cannot run. This is not a trust boundary - whatever
			// can set this process's environment can write HKCU\Run itself - but it
			// keeps a malformed value out of the user's registry.
			if (PathIsRelativeW(buffer) == FALSE
				&& GetFileAttributesW(buffer) != INVALID_FILE_ATTRIBUTES)
			{
				return buffer;
			}

			LogFailure(L"Start with Windows",
				L"the launcher path in the environment is not an existing absolute file; ignoring it");
		}

		// No launcher above us: this is a folder build, so the module path is the file
		// the user runs - unless that file sits inside the payload the launcher
		// unpacks, which is pruned on the next upgrade. Refused here rather than
		// written, because an entry pointing into it would start nothing and could not
		// be repaired from the launcher either.
		const DWORD moduleLength = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
		if (moduleLength > 0 && moduleLength < ARRAYSIZE(buffer) && !IsInsidePayload(buffer))
			return buffer;

		return {};
	}

	bool IsStartWithWindowsEnabled()
	{
		const std::wstring registered = ReadRunValue();
		if (registered.empty())
			return false;

		const std::wstring executable = ExecutableOf(registered);
		if (executable.empty() || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
			return false;

		return !IsDisabledByUser();
	}

	bool SetStartWithWindowsEnabled(bool enabled)
	{
		if (enabled)
		{
			const std::wstring path = DistributedExecutablePath();
			if (path.empty())
			{
				LogFailure(L"Start with Windows",
					L"nothing to register: this copy runs from the unpacked payload, or its own path is unknown");
				return IsStartWithWindowsEnabled();
			}

			if (!WriteRunValue(path))
				return IsStartWithWindowsEnabled();

			DeleteValue(kApprovedKey, kValueName);

			LogTrace(L"start with Windows enabled: " + path);
		}
		else
		{
			DeleteRunValue();
			DeleteValue(kApprovedKey, kValueName);

			LogTrace(L"start with Windows disabled");
		}

		return IsStartWithWindowsEnabled();
	}

	void RepairStartWithWindows()
	{
		const std::wstring registered = ReadRunValue();
		if (registered.empty())
			return;

		const std::wstring path = DistributedExecutablePath();
		if (path.empty())
			return;

		const std::wstring existing = ExecutableOf(registered);
		if (CompareStringOrdinal(existing.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
			return;

		LogTrace(L"start with Windows points at " + existing + L", updating to " + path);

		if (WriteRunValue(path))
			LogTrace(L"start with Windows repaired");
	}
}
