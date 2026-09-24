#pragma once

// Application settings, persisted as JSON next to the executable.
//
// Missing members fall back to their defaults, and unknown members are ignored,
// so a JSON file that carries something this build does not know about still
// loads instead of resetting everything.

namespace AudioPlaybackConnectorWinUI
{
	enum class ThemeMode
	{
		/// <summary>Follow the Windows app theme.</summary>
		System,
		Light,
		Dark,
	};

	struct AppSettings
	{
		ThemeMode theme = ThemeMode::System;
		/// <summary>
		/// Keep an advertised connection - started, not opened - for every paired
		/// sink device, so a device can begin streaming on its own.
		/// </summary>
		bool allowPaired = false;
		/// <summary>Announce readiness and connection changes through the tray icon.</summary>
		bool notifications = false;
		/// <summary>
		/// Devices to connect again on the next launch. Per device rather than one
		/// global switch, because which devices are wanted back differs between
		/// them.
		/// </summary>
		std::vector<std::wstring> reconnectDevices;
		/// <summary>
		/// Language of the interface: "system", "zh-CN", "zh-TW" or "en-US".
		/// </summary>
		std::wstring language = L"system";
	};

	/// <summary>Process-wide settings instance.</summary>
	AppSettings& Settings();

	void LoadSettings();
	void SaveSettings();

	std::wstring_view ToString(ThemeMode mode);
	ThemeMode ThemeFromString(std::wstring_view value);

	/// <summary>True when the device is one to connect again on the next launch.</summary>
	bool IsReconnectEnabled(std::wstring_view deviceId);
	/// <summary>Adds or removes the device from that list.</summary>
	void SetReconnectEnabled(std::wstring_view deviceId, bool enabled);

	/// <summary>Language identifiers the language setting accepts.</summary>
	inline constexpr std::wstring_view kLanguageSystem = L"system";
	inline constexpr std::wstring_view kLanguageSimplifiedChinese = L"zh-CN";
	inline constexpr std::wstring_view kLanguageTraditionalChinese = L"zh-TW";
	inline constexpr std::wstring_view kLanguageEnglish = L"en-US";

	/// <summary>True when the value is one of those identifiers.</summary>
	bool IsValidLanguage(std::wstring_view value);
	/// <summary>
	/// The catalog language a setting selects, as a Win32 language id, or 0 for
	/// "ask the thread's UI language" - which is what "system" means.
	/// </summary>
	uint16_t LanguageIdFromSetting(std::wstring_view value);
}
