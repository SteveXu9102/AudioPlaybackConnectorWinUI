#pragma once
#include "FnvHash.hpp"

extern HINSTANCE g_hInst;

// Lightweight translation catalog.
//
// Translations are compiled into the executable as a "YMO" resource: a hash
// table mapping the FNV-1a hash of each English source string to its UTF-16
// translation. Translators edit the .po files under translate/source/.
//
// The resource is language tagged, so the catalog is loaded for the language the
// settings selected; "en-US" selects a language no .ymo carries, which leaves the
// table empty and every string on its English source text - the fallback, not an
// error. That is also what a missing or unreadable catalog does.

inline std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

// Source literal to translation, filled by Translate(). Keyed by the address of
// the English literal, which is stable for the life of the process - so reloading
// the catalog for another language has to empty it (LoadTranslateData does).
inline std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

/// <summary>
/// Loads the translation catalog for a Win32 language id. A zero language asks the
/// thread's UI language, which is what the "system" setting means.
/// </summary>
inline void LoadTranslateData(uint16_t language)
{
	hashToStrMap.clear();
	ptrToStrMap.clear();

	const uint16_t target = language != 0 ? language : static_cast<uint16_t>(GetThreadUILanguage());
	auto hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), target);
	if (hRes)
	{
		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto ymo = reinterpret_cast<const YMOData*>(LockResource(hResData));
			if (ymo)
			{
				const DWORD size = SizeofResource(g_hInst, hRes);

				// A resource that does not describe itself is not read past its end.
				if (size < 8 || 2u + static_cast<size_t>(ymo->len) * 6u > size)
					return;

				hashToStrMap.reserve(ymo->len);

				for (int i = 0; i < ymo->len; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = ymo->table[i].offset;
					if (offset + sizeof(wchar_t) > size)
						continue;
					// Offsets are relative to the resource data, which is what
					// LockResource returned; going through the HGLOBAL instead only
					// happens to work because it is a pointer for a resource.
					auto str = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(ymo) + offset);

					if (wcsnlen(str, (size - offset) / sizeof(wchar_t)) == (size - offset) / sizeof(wchar_t))
						continue;

					hashToStrMap.emplace(hash, str);
				}
			}
		}
	}
}

inline const wchar_t* Translate(const wchar_t* str)
{
	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

#define _(str) Translate(str)
