#include "pch.h"
#include "LicenseDialogWindow.xaml.h"

#if __has_include("LicenseDialogWindow.g.cpp")
#include "LicenseDialogWindow.g.cpp"
#endif

#include "I18n.hpp"
#include "resource.h"
#include "Util.hpp"
#include "ViewHelpers.h"

// The Compression API (CreateDecompressor/Decompress), which reads the compressed
// notices resource; windows.h does not pull it in.
#include <compressapi.h>
#include <cstring>
// The notices are the only user of it, so the dependency is declared where it is
// used rather than in the project's link settings.
#pragma comment(lib, "cabinet.lib")

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace ::AudioPlaybackConnectorWinUI;

namespace winrt::AudioPlaybackConnectorWinUI::implementation
{
	namespace
	{
		// The window is the same size for every DPI; the text scrolls.
		constexpr double kDialogWidth = 720;
		constexpr double kDialogHeight = 560;
		constexpr double kScreenMargin = 8;

		/// <summary>
		/// How much of the notices is put into the TextBlock. The complete text is
		/// about 890,000 characters; laying all of it out blocks the UI thread for
		/// seconds, so the dialog shows this much of it and states how long the
		/// whole text is.
		/// </summary>
		constexpr size_t kMaxNoticeChars = 40000;

		// The notices are about 889 KB of text, so what RCDATA (301) holds is an
		// MSZIP stream of it, written by tools/assemble-third-party-notices.ps1:
		//
		//     0   'A' 'P' 'C' 'N'
		//     4   uint32  the length the stream decompresses to
		//     8   uint32  the Compression API algorithm id
		//     12  the stream
		//
		// The header is part of the resource rather than something this file knows,
		// so the build can change the format without a change here. The length is
		// checked against kMaxNoticeBytes before it is used as an allocation size.
		constexpr char kNoticesMagic[4] = { 'A', 'P', 'C', 'N' };
		constexpr size_t kNoticesHeaderBytes = 12;
		constexpr uint32_t kCompressionMszip = 2;  // COMPRESS_ALGORITHM_MSZIP
		constexpr uint32_t kMaxNoticeBytes = 64u * 1024u * 1024u;

		std::vector<uint8_t> ReadResourceBytes(int resourceId)
		{
			const HMODULE module = GetModuleHandleW(nullptr);
			const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
			if (found == nullptr)
				return {};

			const DWORD size = SizeofResource(module, found);
			const HGLOBAL loaded = LoadResource(module, found);
			if (loaded == nullptr || size == 0)
				return {};

			const void* data = LockResource(loaded);
			if (data == nullptr)
				return {};

			const auto* first = static_cast<const uint8_t*>(data);
			return std::vector<uint8_t>(first, first + size);
		}

		/// <summary>
		/// UTF-8 to UTF-16, substituting U+FFFD for a byte the strict conversion
		/// refuses. The notices are assembled from many upstream texts, and one
		/// invalid byte must not take the process down when the user opens the
		/// dialog.
		/// </summary>
		std::wstring Utf8ToUtf16Lenient(const std::string& bytes)
		{
			try
			{
				return Utf8ToUtf16(bytes);
			}
			catch (...)
			{
				// MultiByteToWideChar without the strict flag substitutes for what
				// it cannot read.
				std::wstring lossy(bytes.size(), L'\0');
				const int written = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
					static_cast<int>(bytes.size()), lossy.data(), static_cast<int>(lossy.size()));
				lossy.resize(written > 0 ? static_cast<size_t>(written) : 0);
				return lossy;
			}
		}

		/// <summary>
		/// The same text from the copy compiled into this executable, so what the
		/// dialog shows does not depend on any file next to the application.
		/// </summary>
		std::wstring ReadEmbeddedText(int resourceId)
		{
			const std::vector<uint8_t> bytes = ReadResourceBytes(resourceId);
			return Utf8ToUtf16Lenient(std::string(bytes.begin(), bytes.end()));
		}

		/// <summary>
		/// The third-party notices from the copy compiled into this executable,
		/// decompressed. The complete licence and notice text is compiled in, so
		/// what the dialog shows does not depend on any file next to the
		/// application.
		///
		/// A resource that cannot be read is worth a log line, not a process that
		/// dies or a dialog that refuses to open, so every failure returns the
		/// empty string and leaves the licence text on its own.
		/// </summary>
		std::wstring ReadEmbeddedNotices()
		{
			const std::vector<uint8_t> packed = ReadResourceBytes(IDR_NOTICES_TEXT);
			if (packed.size() < kNoticesHeaderBytes || memcmp(packed.data(), kNoticesMagic, sizeof(kNoticesMagic)) != 0)
			{
				LogFailure(L"licences", L"RCDATA 301 carries no APCN header; the notices cannot be shown");
				return {};
			}

			uint32_t plainBytes = 0;
			uint32_t algorithm = 0;
			memcpy(&plainBytes, packed.data() + 4, sizeof(plainBytes));
			memcpy(&algorithm, packed.data() + 8, sizeof(algorithm));

			if (plainBytes == 0 || plainBytes > kMaxNoticeBytes || algorithm != kCompressionMszip)
			{
				LogFailure(L"licences", L"RCDATA 301 declares an unusable length (" + std::to_wstring(plainBytes)
					+ L") or algorithm (" + std::to_wstring(algorithm) + L")");
				return {};
			}

			DECOMPRESSOR_HANDLE decompressor = nullptr;
			if (!CreateDecompressor(algorithm, nullptr, &decompressor))
			{
				LogFailure(L"licences", L"CreateDecompressor failed with error " + std::to_wstring(GetLastError()));
				return {};
			}

			std::string plain(plainBytes, '\0');
			SIZE_T written = 0;
			const BOOL ok = Decompress(decompressor, packed.data() + kNoticesHeaderBytes,
				packed.size() - kNoticesHeaderBytes, plain.data(), plain.size(), &written);
			const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
			CloseDecompressor(decompressor);

			if (!ok || written != plainBytes)
			{
				// The Compression API fills in the size it needed when it fails for
				// want of room, which is the useful half of this line.
				LogFailure(L"licences", L"the notices did not decompress: error " + std::to_wstring(error)
					+ L"; the resource declared " + std::to_wstring(plainBytes)
					+ L" bytes and the decompressor accounted for " + std::to_wstring(written));
				return {};
			}

			return Utf8ToUtf16Lenient(plain);
		}
	}

	LicenseDialogWindow::LicenseDialogWindow()
	{
		InitializeComponent();

		Title(L"AudioPlaybackConnectorWinUI");

		// Like a confirmation: it takes the foreground when it opens, but it does
		// not float above whatever the user switches to.
		ViewHelpers::ConfigurePopupPresenter(*this, false);
		AppWindow().IsShownInSwitchers(false);
		// Alt+F4 must not close this window: it is reused, and a closed WinUI window
		// cannot be shown again - the next Show() would throw from inside the tray
		// callback that asked for it. Hiding instead keeps it usable.
		AppWindow().Closing([](winrt::Microsoft::UI::Windowing::AppWindow const& sender,
			winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
		{
			args.Cancel(true);
			sender.Hide();
		});

		CloseButton().Content(winrt::box_value(hstring(_(L"Close"))));

		Automation::AutomationProperties::SetAutomationId(CloseButton(), L"LicenseDialogClose");

		CloseButton().Click([this](IInspectable const&, RoutedEventArgs const&) { Dismiss(); });

		Content().KeyDown([this](IInspectable const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
			{
				if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
				{
					Dismiss();
					args.Handled(true);
				}
			});
	}

	void LicenseDialogWindow::Dismiss()
	{
		AppWindow().Hide();
	}

	std::wstring LicenseDialogWindow::BuildText()
	{
		std::wstring text = ReadEmbeddedText(IDR_LICENSE_TEXT);

		text += L"\n\n\n";

		std::wstring notices = ReadEmbeddedNotices();

		const size_t totalChars = notices.size();
		if (totalChars > kMaxNoticeChars)
		{
			notices.resize(kMaxNoticeChars);
			text += notices;
			text += L"\n\n";

			// Says what was left out and that the program carries it all: the text
			// beside the executable is a copy for the reader, not the authority.
			wchar_t line[256]{};
			swprintf_s(line, _(L"The complete text of these notices is compiled into this program: %zu characters in all, of which the first %zu are shown here. The copy next to the program is not read."),
				totalChars, kMaxNoticeChars);
			text += line;
		}
		else
		{
			text += notices;
		}

		return text;
	}

	void LicenseDialogWindow::Show(ElementTheme theme)
	{
		// The window inherits nothing from the popup, so the theme is applied here
		// through the same property the other windows use.
		RootGrid().RequestedTheme(theme);

		TitleText().Text(_(L"Licenses"));
		LicenseText().Text(hstring{ BuildText() });

		const HWND hwnd = ViewHelpers::GetWindowHandle(*this);

		// Centre on the monitor the pointer is on, which is the one the user was
		// interacting with.
		POINT cursor{};
		GetCursorPos(&cursor);
		const RECT work = ViewHelpers::WorkAreaForPoint(cursor);

		const UINT creationDpi = GetDpiForWindow(hwnd);
		int width = ViewHelpers::ScaleToPixels(kDialogWidth, creationDpi);
		int height = ViewHelpers::ScaleToPixels(kDialogHeight, creationDpi);
		int margin = ViewHelpers::ScaleToPixels(kScreenMargin, creationDpi);
		int x = 0;
		int y = 0;

		ViewHelpers::ClampToWorkArea(work, margin, width, height);
		ViewHelpers::CentreInWorkArea(work, margin, width, height, x, y);
		const UINT dpi = ViewHelpers::MoveToAndGetDpi(*this, x, y, width, height);

		// The dialog has a fixed size, so at a high scaling factor it can be taller
		// than the work area - and because the window is not resizable, clamping only
		// its position would leave the Close button below the bottom edge with no way
		// to reach it. Let the size give way: the licence text is already inside a
		// ScrollViewer, which takes up the slack.
		margin = ViewHelpers::ScaleToPixels(kScreenMargin, dpi);
		width = ViewHelpers::ScaleToPixels(kDialogWidth, dpi);
		height = ViewHelpers::ScaleToPixels(kDialogHeight, dpi);
		ViewHelpers::ClampToWorkArea(work, margin, width, height);
		ViewHelpers::CentreInWorkArea(work, margin, width, height, x, y);
		ViewHelpers::MoveContentArea(*this, x, y, width, height);

		AppWindow().Show();
		Activate();
		ViewHelpers::ForceForeground(*this);

		if (IsTraceEnabled())
		{
			RECT rect{};
			GetWindowRect(hwnd, &rect);
			LogTrace(L"licence dialog shown at " + std::to_wstring(rect.left) + L","
				+ std::to_wstring(rect.top) + L" "
				+ std::to_wstring(rect.right - rect.left) + L"x"
				+ std::to_wstring(rect.bottom - rect.top)
				+ L" chars=" + std::to_wstring(LicenseText().Text().size()));
		}

		CloseButton().Focus(FocusState::Programmatic);
	}
}
