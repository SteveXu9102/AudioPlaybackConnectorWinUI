#pragma once

// The icon Windows itself gives a device.
//
// Windows publishes it on the device interface as a resource reference into
// DDORes.dll - "C:\Windows\System32\DDORes.dll,-3022" - and that is what this
// renders, so the panel agrees with the icon Windows Settings shows for the same
// device. Neither the device's name nor its Bluetooth class of device is used
// instead: neither describes what kind of device it is.
//
// The glyph is drawn in the caller's colour rather than in its own, because the
// resource is a single-colour shape that the panel tints for the theme and for
// whether the device is connected.

namespace AudioPlaybackConnectorWinUI::DeviceIcon
{
	/// <summary>
	/// Renders the icon at "&lt;path&gt;,&lt;index&gt;" in <paramref name="tint"/>,
	/// at the size the resource provides. Returns null when it cannot be read, so
	/// the caller can fall back to a glyph of its own.
	/// </summary>
	inline winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap Render(
		std::wstring const& resource, winrt::Windows::UI::Color const& tint)
	{
		// "C:\Windows\System32\DDORes.dll,-3022": the index after the last comma,
		// negative to mean "resource identifier 3022" to ExtractIconEx.
		const size_t comma = resource.rfind(L',');
		if (comma == std::wstring::npos || comma + 1 >= resource.size())
			return nullptr;

		int index = 0;
		try
		{
			index = std::stoi(resource.substr(comma + 1));
		}
		catch (std::exception const&)
		{
			return nullptr;
		}

		if (index == 0)
			return nullptr;

		// Not named "large": that is a macro in rpcndr.h.
		HICON big = nullptr;
		HICON mini = nullptr;
		if (ExtractIconExW(resource.substr(0, comma).c_str(), -std::abs(index),
			&big, &mini, 1) == 0)
		{
			return nullptr;
		}

		// Either size is usable: a resource that carries only a small icon still has a
		// shape worth drawing, and rejecting it would leave the badge generic.
		wil::unique_hicon icon(big != nullptr ? big : mini);
		if (big != nullptr && mini != nullptr)
			DestroyIcon(mini);

		if (icon == nullptr)
			return nullptr;

		// The colour plane, read as the alpha mask. Deliberately not DrawIconEx:
		// that composites the icon against the destination, and an icon whose alpha
		// GDI declines to use would come back as an opaque square. The bits are the
		// icon's own, so the shape either is there or is not.
		ICONINFO parts{};
		if (!GetIconInfo(icon.get(), &parts))
			return nullptr;

		const wil::unique_hbitmap colour(parts.hbmColor);
		const wil::unique_hbitmap mask(parts.hbmMask);

		BITMAP header{};
		if (colour == nullptr || GetObjectW(colour.get(), sizeof(header), &header) == 0)
			return nullptr;

		if (header.bmWidth <= 0 || header.bmHeight <= 0)
			return nullptr;

		const int width = header.bmWidth;
		const int height = header.bmHeight;

		wil::unique_hdc hdc(CreateCompatibleDC(nullptr));
		if (!hdc)
			return nullptr;

		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height; // top-down, matching the buffer written below
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;

		const size_t bytes = static_cast<size_t>(width) * height * 4;
		std::vector<uint8_t> icon32(bytes);
		if (GetDIBits(hdc.get(), colour.get(), 0, static_cast<UINT>(height),
			icon32.data(), &info, DIB_RGB_COLORS) == 0)
		{
			return nullptr;
		}

		// A mask-only icon has no alpha of its own. Windows falls back to the mask
		// plane for those, and so does the caller, which draws its own glyph.
		// highest decides whether the icon has an alpha channel at all, so it is always
		// computed; lowest only feeds the trace line below.
		const bool tracing = IsTraceEnabled();
		uint8_t lowest = 255;
		uint8_t highest = 0;
		for (size_t pixel = 3; pixel < bytes; pixel += 4)
		{
			highest = (std::max)(highest, icon32[pixel]);
			if (tracing)
				lowest = (std::min)(lowest, icon32[pixel]);
		}

		if (highest == 0)
		{
			LogTrace(L"icon " + resource + L": no alpha, " + std::to_wstring(width) + L"x"
				+ std::to_wstring(height));
			return nullptr;
		}

		winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap bitmap(width, height);
		auto buffer = bitmap.PixelBuffer();
		if (static_cast<size_t>(buffer.Length()) < bytes)
			return nullptr;

		uint8_t* pixels = buffer.data();
		for (size_t pixel = 0; pixel < bytes; pixel += 4)
		{
			// Premultiplied, which is what an image source expects.
			const uint32_t alpha = icon32[pixel + 3] * tint.A / 255;
			pixels[pixel + 0] = static_cast<uint8_t>(tint.B * alpha / 255);
			pixels[pixel + 1] = static_cast<uint8_t>(tint.G * alpha / 255);
			pixels[pixel + 2] = static_cast<uint8_t>(tint.R * alpha / 255);
			pixels[pixel + 3] = static_cast<uint8_t>(alpha);
		}

		LogTrace(L"icon " + resource + L": " + std::to_wstring(width) + L"x"
			+ std::to_wstring(height) + L" alpha " + std::to_wstring(lowest) + L".."
			+ std::to_wstring(highest));

		return bitmap;
	}
}
