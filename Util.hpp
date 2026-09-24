#pragma once

#include <cstdio>
#include <filesystem>
#include <vector>

// Declares the version resource readers (GetFileVersionInfoW, VerQueryValueW) and
// VS_FIXEDFILEINFO; windows.h does not pull it in.
#include <winver.h>

namespace fs = std::filesystem;

// https://msdn.microsoft.com/en-us/magazine/mt763237
inline std::wstring Utf8ToUtf16(std::string_view utf8)
{
	if (utf8.empty())
	{
		return {};
	}

	constexpr DWORD kFlags = MB_ERR_INVALID_CHARS;
	const int utf8Length = static_cast<int>(utf8.length());
	const int utf16Length = MultiByteToWideChar(
		CP_UTF8,
		kFlags,
		utf8.data(),
		utf8Length,
		nullptr,
		0
	);
	THROW_LAST_ERROR_IF(utf16Length == 0);

	std::wstring utf16(utf16Length, L'\0');
	const int result = MultiByteToWideChar(
		CP_UTF8,
		kFlags,
		utf8.data(),
		utf8Length,
		utf16.data(),
		utf16Length
	);
	THROW_LAST_ERROR_IF(result == 0);

	return utf16;
}

inline std::string Utf16ToUtf8(std::wstring_view utf16)
{
	if (utf16.empty())
	{
		return {};
	}

	constexpr DWORD kFlags = WC_ERR_INVALID_CHARS;
	const int utf16Length = static_cast<int>(utf16.length());
	const int utf8Length = WideCharToMultiByte(
		CP_UTF8,
		kFlags,
		utf16.data(),
		utf16Length,
		nullptr,
		0,
		nullptr, nullptr
	);
	THROW_LAST_ERROR_IF(utf8Length == 0);

	std::string utf8(utf8Length, '\0');
	const int result = WideCharToMultiByte(
		CP_UTF8,
		kFlags,
		utf16.data(),
		utf16Length,
		utf8.data(),
		utf8Length,
		nullptr, nullptr
	);
	THROW_LAST_ERROR_IF(result == 0);

	return utf8;
}

// https://docs.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/author-coclasses#add-helper-types-and-functions
// License: see the https://github.com/MicrosoftDocs/windows-uwp/blob/docs/LICENSE-CODE file
inline auto GetModuleFsPath(HMODULE hModule)
{
	std::wstring path(MAX_PATH, L'\0');
	DWORD actualSize;

	while (1)
	{
		actualSize = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));

		if (static_cast<size_t>(actualSize) + 1 > path.size())
			path.resize(path.size() * 2);
		else
			break;
	}

	path.resize(actualSize);
	return fs::path(path);
}

// True when `text` contains `digits`, ignoring case. Bluetooth addresses appear in
// device ids with different separators, so a run of hex digits is looked for.
inline bool ContainsDigits(std::wstring_view text, std::wstring_view digits)
{
	if (digits.empty() || digits.size() > text.size())
		return false;

	for (size_t start = 0; start + digits.size() <= text.size(); ++start)
	{
		bool match = true;
		for (size_t index = 0; index < digits.size(); ++index)
		{
			if (std::towupper(text[start + index]) != std::towupper(digits[index]))
			{
				match = false;
				break;
			}
		}

		if (match)
			return true;
	}

	return false;
}

inline fs::path GetModuleDirectory(HMODULE hModule)
{
	return GetModuleFsPath(hModule).remove_filename();
}

// Directory for the settings file and the diagnostic log. The single-file launcher
// points APC_SETTINGS_DIR at the directory of the distributed executable, so both
// stay portable rather than living inside the unpacked payload folder.
inline fs::path GetDataDirectory()
{
	wchar_t buffer[MAX_PATH * 4]{};
	DWORD length = GetEnvironmentVariableW(L"APC_SETTINGS_DIR", buffer, ARRAYSIZE(buffer));
	if (length > 0 && length < ARRAYSIZE(buffer))
		return fs::path(buffer);

	return GetModuleDirectory(nullptr);
}

// Version of this executable, read from its own version resource rather than
// compiled in, so the number the panel shows is the number the binary carries and
// there is nothing to keep in sync with the resource script. The fourth field is
// left out when it is zero.
inline std::wstring GetAppVersion()
{
	auto path = GetModuleFsPath(nullptr);

	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0)
		return {};

	std::vector<BYTE> data(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
		return {};

	VS_FIXEDFILEINFO* info = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || info == nullptr)
		return {};

	const DWORD ms = info->dwFileVersionMS;
	const DWORD ls = info->dwFileVersionLS;

	const unsigned major = HIWORD(ms);
	const unsigned minor = LOWORD(ms);
	const unsigned build = HIWORD(ls);
	const unsigned revision = LOWORD(ls);

	wchar_t text[32]{};
	if (revision != 0)
		swprintf_s(text, L"%u.%u.%u.%u", major, minor, build, revision);
	else
		swprintf_s(text, L"%u.%u.%u", major, minor, build);

	return text;
}

inline bool IsSystemUsingLightTheme()
{
	DWORD value = 0;
	DWORD cbValue = sizeof(value);
	if (FAILED(RegGetValueW(
		HKEY_CURRENT_USER,
		LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
		L"SystemUsesLightTheme",
		RRF_RT_REG_DWORD,
		nullptr,
		&value,
		&cbValue)))
	{
		return false;
	}
	return value != 0;
}

inline bool AreSystemAnimationsEnabled()
{
	BOOL enabled = TRUE;
	if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
		return true;

	return enabled != FALSE;
}

inline void LogFailure(std::wstring_view context, std::wstring_view message)
{
	SYSTEMTIME now{};
	GetLocalTime(&now);

	wchar_t stamp[32]{};
	swprintf_s(stamp, L"%02u:%02u:%02u.%03u", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

	auto line = L"[" + std::wstring(stamp) + L"] [" + std::wstring(context) + L"] " + std::wstring(message) + L"\r\n";
	OutputDebugStringW(line.c_str());

	try
	{
		auto path = GetDataDirectory() / L"AudioPlaybackConnectorWinUI.log";

		// Bounded: the log is append-only and tracing can be left on for a long
		// session, so without this a tray application that runs for weeks grows the
		// file without limit. One previous generation is kept, and it is rotated by
		// rename - one atomic call, where trimming the oldest lines would mean every
		// writer reading and rewriting the file.
		constexpr unsigned long long kMaxLogBytes = 1024 * 1024;
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
		{
			const unsigned long long size =
				(static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
			if (size > kMaxLogBytes)
			{
				const std::wstring previous = path.wstring() + L".old";
				MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
			}
		}

		// Sharing read and write on purpose: the file is opened for append by the
		// application, and a handle that shared only read would make a second writer
		// - an editor, or a diagnostic tool - fail with a sharing violation, which
		// the catch below then swallows. Sharing also keeps the rename above working
		// while the file is open.
		wil::unique_hfile hFile(CreateFileW(
			path.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		auto utf8 = Utf16ToUtf8(line);
		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
	}
	catch (...)
	{
	}
}

// Popup show/hide tracing, off by default: APC_TRACE=1 turns it on.
//
// Exactly "1" turns it on. Any other value - including "0", which is what someone
// reaching for the switch to turn tracing *off* would set - leaves it off, so the
// variable does what its name suggests in both directions.
inline bool IsTraceEnabled()
{
	static const bool enabled = []
	{
		wchar_t buffer[8]{};
		const DWORD length = GetEnvironmentVariableW(L"APC_TRACE", buffer, ARRAYSIZE(buffer));
		return length == 1 && buffer[0] == L'1';
	}();
	return enabled;
}

inline void LogTrace(std::wstring_view message)
{
	if (IsTraceEnabled())
		LogFailure(L"trace", message);
}
