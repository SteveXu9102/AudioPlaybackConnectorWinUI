#pragma once

// Rasterises the bundled SVG logo into an HICON so the notification-area icon
// can be tinted for light/dark taskbars. WinUI 3 has no API for producing tray
// icons, so this stays on Direct2D + GDI.

inline void DrawSvgTohDC(std::string_view svg, HDC hdc, LONG width, LONG height, const D2D1_COLOR_F& color)
{
	winrt::com_ptr<ID2D1Factory> factory;
	winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()));

	D2D1_RENDER_TARGET_PROPERTIES props = {
		.type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
		.pixelFormat = {
			.format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED
		},
		.dpiX = 0, .dpiY = 0,
		.usage = D2D1_RENDER_TARGET_USAGE_NONE,
		.minLevel = D2D1_FEATURE_LEVEL_DEFAULT
	};
	winrt::com_ptr<ID2D1DCRenderTarget> gdiDCRT;
	winrt::check_hresult(factory->CreateDCRenderTarget(&props, gdiDCRT.put()));

	RECT rect = { 0, 0, width, height };
	winrt::check_hresult(gdiDCRT->BindDC(hdc, &rect));

	auto istream = SHCreateMemStream(reinterpret_cast<const BYTE*>(svg.data()), static_cast<UINT>(svg.size()));
	THROW_IF_NULL_ALLOC(istream);
	winrt::com_ptr<IStream> stream(istream, winrt::take_ownership_from_abi);

	auto dc = gdiDCRT.as<ID2D1DeviceContext5>();
	// Per-primitive rather than aliased: the tray icon is rasterised at about 16 px,
	// where aliasing leaves visibly stepped edges on the logo's diagonals.
	dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

	winrt::com_ptr<ID2D1SvgDocument> svgDoc;
	winrt::check_hresult(dc->CreateSvgDocument(stream.get(), { static_cast<FLOAT>(width), static_cast<FLOAT>(height) }, svgDoc.put()));

	winrt::com_ptr<ID2D1SvgElement> svgRoot;
	// ID2D1SvgDocument::GetRoot returns void rather than an HRESULT, so the pointer
	// is what has to be checked: the dereference below would fault on a null one.
	svgDoc->GetRoot(svgRoot.put());
	THROW_HR_IF_NULL(E_UNEXPECTED, svgRoot.get());

	// Best effort: without it the icon is drawn in the SVG's own colour, which is
	// still an icon, so a failure here must not take the icon away.
	svgRoot->SetAttributeValue(L"fill", color);

	dc->BeginDraw();
	dc->DrawSvgDocument(svgDoc.get());

	// Logged rather than thrown. A failed flush means an untinted or blank icon -
	// something the user can still click - whereas throwing would take the whole
	// application down before its tray icon ever appears.
	LOG_IF_FAILED(dc->EndDraw());
}

inline auto CreateDIB(HDC hdc, LONG width, LONG height, WORD bitCount)
{
	BITMAPINFO bmi = { .bmiHeader = {
		.biSize = sizeof(BITMAPINFOHEADER),
		.biWidth = width,
		.biHeight = height,
		.biPlanes = 1,
		.biBitCount = bitCount,
		.biCompression = BI_RGB,
	} };
	return wil::unique_hbitmap(CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0));
}

inline HICON SvgTohIcon(std::string_view svg, LONG width, LONG height, const D2D1_COLOR_F& color)
{
	wil::unique_hdc hdc(CreateCompatibleDC(nullptr));
	THROW_IF_NULL_ALLOC(hdc);

	auto hBitmap = CreateDIB(hdc.get(), width, height, 32);
	THROW_IF_NULL_ALLOC(hBitmap);
	auto hBitmapMask = CreateDIB(hdc.get(), width, height, 1);
	THROW_IF_NULL_ALLOC(hBitmapMask);

	auto select = wil::SelectObject(hdc.get(), hBitmap.get());
	DrawSvgTohDC(svg, hdc.get(), width, height, color);

	ICONINFO iconInfo = {
		.fIcon = TRUE,
		.hbmMask = hBitmapMask.get(),
		.hbmColor = hBitmap.get()
	};
	HICON hIcon = CreateIconIndirect(&iconInfo);
	THROW_LAST_ERROR_IF_NULL(hIcon);

	return hIcon;
}

// Loads the SVG logo embedded as RCDATA of type "SVG" in the module resources and
// returns its bytes. The id is a required parameter rather than a default so that
// every caller names what it loads; a lookup that finds nothing FAIL_FASTs, which
// is a hard crash on startup rather than a silently missing icon.
inline std::string LoadEmbeddedSvg(HMODULE hModule, WORD resourceId)
{
	auto hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(hModule, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(hModule, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	return std::string(svgData, size);
}
