#include "pch.h"
#include "AppSettings.h"
#include "Util.hpp"

using namespace winrt::Windows::Data::Json;

namespace AudioPlaybackConnectorWinUI
{
	namespace
	{
		constexpr auto CONFIG_NAME = L"AudioPlaybackConnectorWinUI.json";
		constexpr DWORD BUFFER_SIZE = 4096;

		constexpr auto kThemeKey = L"themeMode";
		constexpr auto kAllowPairedKey = L"allowPaired";
		constexpr auto kNotificationsKey = L"notifications";
		constexpr auto kReconnectDevicesKey = L"reconnectDevices";
		constexpr auto kLanguageKey = L"language";

		std::filesystem::path SettingsPath()
		{
			return GetDataDirectory() / CONFIG_NAME;
		}

		std::wstring ReadAllText(std::filesystem::path const& filePath)
		{
			wil::unique_hfile hFile(CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			THROW_LAST_ERROR_IF(!hFile);

			std::string contents;
			while (true)
			{
				size_t size = contents.size();
				contents.resize(size + BUFFER_SIZE);

				DWORD read = 0;
				THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), contents.data() + size, BUFFER_SIZE, &read, nullptr));
				contents.resize(size + read);
				if (read == 0)
					break;
			}

			return Utf8ToUtf16(contents);
		}

		// Written through a sibling temporary file and swapped in, because a
		// truncating write is not recoverable: a crash - or the exit watchdog ending
		// the process - between the truncation and the last byte leaves a half
		// written file, and LoadSettings then reads that as "no settings" and
		// silently resets everything.
		void WriteAllText(std::filesystem::path const& filePath, std::wstring_view text)
		{
			std::filesystem::path temporary = filePath;
			temporary += L".tmp";

			const auto discardTemporary = wil::scope_exit([&temporary]
			{
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
			});

			{
				wil::unique_hfile hFile(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
				THROW_LAST_ERROR_IF(!hFile);

				std::string utf8 = Utf16ToUtf8(text);
				DWORD written = 0;
				THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
				THROW_HR_IF(E_FAIL, written != utf8.size());

				// Durable before it replaces the previous file, so a power loss cannot
				// leave the replacement present but empty.
				THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(hFile.get()));
			}

			// ReplaceFileW keeps the identity and the attributes of the existing
			// file; the very first run has nothing to replace, so the move is the
			// fallback for it.
			if (!ReplaceFileW(filePath.c_str(), temporary.c_str(), nullptr,
				REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
			{
				const DWORD error = GetLastError();
				if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
					THROW_WIN32(error);

				THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(
					temporary.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING));
			}
		}

		/// <summary>
		/// A JSON array of strings, or empty when the key is absent or does not hold
		/// an array. A member of the wrong type is ignored rather than failing the
		/// whole load: one unreadable member must not reset every other setting.
		/// </summary>
		std::vector<std::wstring> ReadStringArray(JsonObject const& jsonObj, wchar_t const* key)
		{
			std::vector<std::wstring> values;

			try
			{
				if (!jsonObj.HasKey(key))
					return values;

				auto const array = jsonObj.GetNamedArray(key);
				if (array == nullptr)
					return values;

				values.reserve(array.Size());
				for (auto const& item : array)
				{
					if (item.ValueType() == JsonValueType::String)
						values.push_back(std::wstring(item.GetString()));
				}
			}
			CATCH_LOG();

			return values;
		}

		JsonArray ToJsonArray(std::vector<std::wstring> const& values)
		{
			JsonArray array;
			for (auto const& value : values)
				array.Append(JsonValue::CreateStringValue(value));

			return array;
		}
	}

	AppSettings& Settings()
	{
		static AppSettings settings;
		return settings;
	}

	std::wstring_view ToString(ThemeMode mode)
	{
		switch (mode)
		{
		case ThemeMode::Light:  return L"light";
		case ThemeMode::Dark:   return L"dark";
		case ThemeMode::System: return L"system";
		}
		return L"system";
	}

	ThemeMode ThemeFromString(std::wstring_view value)
	{
		if (value == L"light")
			return ThemeMode::Light;
		if (value == L"dark")
			return ThemeMode::Dark;
		return ThemeMode::System;
	}

	bool IsValidLanguage(std::wstring_view value)
	{
		return value == kLanguageSystem
			|| value == kLanguageSimplifiedChinese
			|| value == kLanguageTraditionalChinese
			|| value == kLanguageEnglish;
	}

	uint16_t LanguageIdFromSetting(std::wstring_view value)
	{
		if (value == kLanguageSimplifiedChinese)
			return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
		if (value == kLanguageTraditionalChinese)
			return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
		if (value == kLanguageEnglish)
			return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

		// "system", and anything unrecognised, follows the language Windows is
		// running this thread's user interface in.
		return 0;
	}

	bool IsReconnectEnabled(std::wstring_view deviceId)
	{
		for (auto const& saved : Settings().reconnectDevices)
		{
			if (std::wstring_view(saved) == deviceId)
				return true;
		}

		return false;
	}

	void SetReconnectEnabled(std::wstring_view deviceId, bool enabled)
	{
		auto& devices = Settings().reconnectDevices;

		const auto found = std::find_if(devices.begin(), devices.end(),
			[deviceId](std::wstring const& saved) { return std::wstring_view(saved) == deviceId; });

		if (enabled)
		{
			if (found == devices.end())
				devices.emplace_back(deviceId);
			return;
		}

		if (found != devices.end())
			devices.erase(found);
	}

	void LoadSettings()
	{
		auto& settings = Settings();
		settings = AppSettings{};

		try
		{
			auto jsonObj = JsonObject::Parse(ReadAllText(SettingsPath()));

			if (jsonObj.HasKey(kThemeKey))
				settings.theme = ThemeFromString(jsonObj.GetNamedString(kThemeKey, L"system"));

			if (jsonObj.HasKey(kAllowPairedKey))
				settings.allowPaired = jsonObj.GetNamedBoolean(kAllowPairedKey, false);

			if (jsonObj.HasKey(kNotificationsKey))
				settings.notifications = jsonObj.GetNamedBoolean(kNotificationsKey, false);

			settings.reconnectDevices = ReadStringArray(jsonObj, kReconnectDevicesKey);

			if (jsonObj.HasKey(kLanguageKey))
			{
				const std::wstring language(jsonObj.GetNamedString(kLanguageKey, L"system"));
				if (IsValidLanguage(language))
					settings.language = language;
			}

			// Unknown members are tolerated by design, so such a file keeps loading.
		}
		CATCH_LOG();
	}

	void SaveSettings()
	{
		try
		{
			auto const& settings = Settings();

			JsonObject jsonObj;
			jsonObj.Insert(kThemeKey, JsonValue::CreateStringValue(ToString(settings.theme)));
			jsonObj.Insert(kAllowPairedKey, JsonValue::CreateBooleanValue(settings.allowPaired));
			jsonObj.Insert(kNotificationsKey, JsonValue::CreateBooleanValue(settings.notifications));
			jsonObj.Insert(kReconnectDevicesKey, ToJsonArray(settings.reconnectDevices));
			jsonObj.Insert(kLanguageKey, JsonValue::CreateStringValue(settings.language));

			WriteAllText(SettingsPath(), jsonObj.Stringify());
		}
		CATCH_LOG();
	}
}
