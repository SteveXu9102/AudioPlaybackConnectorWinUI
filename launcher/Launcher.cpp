// Single-file launcher for AudioPlaybackConnectorWinUI.
//
// A WinUI 3 application cannot be shipped as one file: the Windows App SDK
// binaries and the compiled XAML resource index have to exist as real files on
// disk. This launcher carries the complete self-contained payload appended to
// its own executable, unpacks it once into %LOCALAPPDATA%, and then starts the
// real application from there.
//
// The launcher intentionally has no dependency beyond the Windows API and the
// static C runtime, so the packaged file runs on a machine with nothing
// installed.
//
// Payload container, appended to this executable:
//
//   [entry]...            entry = uint32 nameBytes | uint32 dataBytes
//                                 | wchar_t name[nameBytes / 2] (UTF-16LE)
//                                 | byte data[dataBytes]
//   [footer]              footer = uint64 payloadOffset
//                                 | uint64 payloadSize
//                                 | uint64 payloadId
//                                 | char   magic[8]

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	constexpr char kMagic[8] = { 'A', 'P', 'C', 'P', 'A', 'Y', '0', '1' };
	constexpr uint64_t kFooterSize = 8 + 8 + 8 + 8;
	constexpr uint32_t kChunkSize = 256 * 1024;
	constexpr uint32_t kMaxNameBytes = 4096;

	constexpr wchar_t kAppFileName[] = L"AudioPlaybackConnectorWinUI.exe";
	constexpr wchar_t kCompleteMarker[] = L".complete";
	constexpr wchar_t kSettingsDirVariable[] = L"APC_SETTINGS_DIR";
	// Full path of the distributed file. The application needs it for the
	// "start with Windows" registration: the running executable is the unpacked
	// copy, which would be the wrong thing to start at sign-in.
	constexpr wchar_t kLauncherPathVariable[] = L"APC_LAUNCHER_PATH";

	// Serialises unpacking between two launches that start at the same time.
	constexpr wchar_t kUnpackMutexName[] = L"Local\\AudioPlaybackConnectorWinUI.Unpack";
	constexpr DWORD kUnpackLockTimeoutMs = 60000;

	struct Footer
	{
		uint64_t offset = 0;
		uint64_t size = 0;
		uint64_t id = 0;
	};

	[[noreturn]] void Fail(std::wstring const& message)
	{
		MessageBoxW(nullptr, message.c_str(), L"AudioPlaybackConnectorWinUI", MB_OK | MB_ICONERROR);
		ExitProcess(1);
	}

	bool ReadExact(HANDLE file, void* buffer, uint32_t bytes)
	{
		auto* cursor = static_cast<BYTE*>(buffer);
		while (bytes > 0)
		{
			DWORD read = 0;
			if (!ReadFile(file, cursor, bytes, &read, nullptr) || read == 0)
				return false;

			cursor += read;
			bytes -= read;
		}
		return true;
	}

	bool ReadFooter(HANDLE file, uint64_t fileSize, Footer& footer)
	{
		if (fileSize < kFooterSize)
			return false;

		LARGE_INTEGER position{};
		position.QuadPart = static_cast<LONGLONG>(fileSize - kFooterSize);
		if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
			return false;

		BYTE raw[kFooterSize]{};
		if (!ReadExact(file, raw, static_cast<uint32_t>(kFooterSize)))
			return false;

		if (memcmp(raw + 24, kMagic, sizeof(kMagic)) != 0)
			return false;

		memcpy(&footer.offset, raw + 0, sizeof(uint64_t));
		memcpy(&footer.size, raw + 8, sizeof(uint64_t));
		memcpy(&footer.id, raw + 16, sizeof(uint64_t));

		// Subtracted rather than added: offset + size + kFooterSize wraps once the
		// footer's own fields are absurd, and a wrapped sum can equal fileSize and
		// make a tampered footer look valid.
		const uint64_t footerBytes = static_cast<uint64_t>(kFooterSize);
		if (footer.offset > fileSize || footerBytes > fileSize - footer.offset)
			return false;

		return footer.size == fileSize - footer.offset - footerBytes;
	}

	/// <summary>
	/// True when this path is already a directory that is not a reparse point, or has
	/// just been created as one.
	/// </summary>
	bool EnsureRealDirectory(std::filesystem::path const& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());

		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			std::error_code error;
			std::filesystem::create_directory(path, error);
			return !error;
		}

		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			return false;

		return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
	}

	/// <summary>
	/// Creates the components of <paramref name="relative"/> under
	/// <paramref name="root"/>, one at a time, refusing to follow a reparse point.
	///
	/// Entry names are checked lexically before this (IsSafeEntryName), so no name can
	/// escape the root on its own - but a junction planted at a component would
	/// redirect every write below it, and create_directories would follow it without
	/// a word. Each component the launcher owns is therefore required to be a real
	/// directory, whether it already existed or was just created.
	/// </summary>
	void CreateDirectoriesUnder(std::filesystem::path const& root, std::filesystem::path const& relative)
	{
		std::filesystem::path current = root;

		// The root is not the launcher's to police: %LOCALAPPDATA% is legitimately a
		// junction or a symlink on a machine whose profile lives elsewhere, and
		// refusing to start there would break a supported configuration. It only has
		// to be a directory; everything below it is created by the launcher, and a
		// reparse point there would redirect every write that follows.
		const DWORD rootAttributes = GetFileAttributesW(current.c_str());
		if (rootAttributes == INVALID_FILE_ATTRIBUTES
			|| (rootAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			Fail(L"The application payload could not be placed: " + current.wstring());
		}

		for (auto const& component : relative)
		{
			if (component.empty() || component == L".")
				continue;

			current /= component;

			if (!EnsureRealDirectory(current))
				Fail(L"The application payload directory is not a plain directory: " + current.wstring());
		}
	}

	std::filesystem::path TargetDirectory(uint64_t id)
	{
		wchar_t buffer[MAX_PATH * 4]{};
		const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));

		// No fallback to the temporary directory. When LOCALAPPDATA is absent - a
		// service-like or scheduled environment - %TEMP% can resolve to a location
		// shared with other users, and this is the directory an executable is unpacked
		// into and then run from. A payload and its completion marker left there by
		// somebody else would be run as whoever launches this file, so the unpack is
		// refused instead: without a private per-user directory there is nowhere safe
		// to put it.
		if (length == 0 || length >= ARRAYSIZE(buffer))
			Fail(L"AudioPlaybackConnectorWinUI needs a per-user application data directory, and this environment does not have one.");

		wchar_t idText[32]{};
		swprintf_s(idText, L"%016llX", static_cast<unsigned long long>(id));

		// The three components the launcher itself owns are created and checked here,
		// one at a time. The components above them are deliberately left alone: an
		// AppData redirected by a roaming profile or by the user is legitimate.
		const std::filesystem::path relative =
			std::filesystem::path(L"AudioPlaybackConnectorWinUI") / L"app" / idText;

		CreateDirectoriesUnder(buffer, relative);

		return std::filesystem::path(buffer) / relative;
	}

	/// <summary>
	/// True when a payload entry name is a plain relative path below the target
	/// directory.
	///
	/// The container is written by tools/package-single-file.ps1, but the launcher
	/// is a downloadable binary: a repacked or tampered file must not be able to
	/// make it write outside the directory it unpacks into.
	/// </summary>
	bool IsSafeEntryName(std::wstring const& name)
	{
		if (name.empty())
			return false;

		// A drive letter, or an alternate data stream.
		if (name.find(L':') != std::wstring::npos)
			return false;

		// A rooted path would make the join below ignore the target directory.
		if (name.front() == L'\\' || name.front() == L'/')
			return false;

		size_t start = 0;
		for (;;)
		{
			const size_t end = name.find_first_of(L"\\/", start);
			const size_t length = (end == std::wstring::npos ? name.size() : end) - start;
			const std::wstring component = name.substr(start, length);

			// ".." and an empty component ("a//b", or a trailing separator) are both
			// rejected: neither can occur in a payload this project produces.
			if (component.empty() || component == L"..")
				return false;

			if (end == std::wstring::npos)
				return true;

			start = end + 1;
		}
	}

	void ExtractPayload(HANDLE file, Footer const& footer, std::filesystem::path const& target)
	{
		LARGE_INTEGER position{};
		position.QuadPart = static_cast<LONGLONG>(footer.offset);
		if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
			Fail(L"Cannot read the bundled application payload.");

		std::vector<BYTE> buffer(kChunkSize);
		uint64_t remaining = footer.size;
		std::error_code error;

		while (remaining > 0)
		{
			if (remaining < 8)
				Fail(L"The bundled application payload is damaged.");

			uint32_t nameBytes = 0;
			uint32_t dataBytes = 0;
			if (!ReadExact(file, &nameBytes, 4) || !ReadExact(file, &dataBytes, 4))
				Fail(L"The bundled application payload is damaged.");
			remaining -= 8;

			// Both lengths are checked against what is left before either is
			// subtracted. Testing dataBytes alone let a name that did not fit wrap
			// `remaining` to about 2^64, after which the walk ran past the payload
			// into the footer and the file's end.
			if (nameBytes == 0 || nameBytes > kMaxNameBytes || (nameBytes % 2) != 0
				|| dataBytes > remaining || nameBytes > remaining - dataBytes)
			{
				Fail(L"The bundled application payload is damaged.");
			}

			std::wstring name(nameBytes / sizeof(wchar_t), L'\0');
			if (!ReadExact(file, name.data(), nameBytes))
				Fail(L"The bundled application payload is damaged.");
			remaining -= nameBytes;

			if (!IsSafeEntryName(name))
				Fail(L"The bundled application payload contains an invalid entry name.");

			auto destination = target / std::filesystem::path(name);

			// Every directory on the way is created one component at a time and
			// checked, so a junction planted at any of them is refused rather than
			// followed. The file itself is checked too: CREATE_ALWAYS follows a link.
			CreateDirectoriesUnder(target, std::filesystem::path(name).parent_path());

			const DWORD existing = GetFileAttributesW(destination.c_str());
			if (existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				Fail(L"The bundled application payload contains an entry that is a link: " + destination.wstring());

			HANDLE output = CreateFileW(
				destination.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (output == INVALID_HANDLE_VALUE)
				Fail(L"Cannot write " + destination.wstring());

			uint64_t left = dataBytes;
			while (left > 0)
			{
				DWORD wanted = static_cast<DWORD>(left < kChunkSize ? left : kChunkSize);
				if (!ReadExact(file, buffer.data(), wanted))
				{
					CloseHandle(output);
					Fail(L"The bundled application payload is damaged.");
				}

				DWORD written = 0;
				if (!WriteFile(output, buffer.data(), wanted, &written, nullptr) || written != wanted)
				{
					CloseHandle(output);
					Fail(L"Cannot write " + destination.wstring());
				}

				left -= wanted;
			}

			CloseHandle(output);
			remaining -= dataBytes;
		}
	}

	/// <summary>
	/// Whether the application is currently running from this payload directory.
	///
	/// A running instance holds its own executable open without sharing write
	/// access, so a write handle is exactly the test for "somebody is still using
	/// this". A directory whose executable is already gone is a leftover from an
	/// interrupted unpack and can be removed.
	/// </summary>
	bool IsPayloadInUse(std::filesystem::path const& directory)
	{
		const auto executable = directory / kAppFileName;

		// A payload an earlier prune left half-deleted has no executable to be in use,
		// so those leftovers are always removable - otherwise they accumulate for ever.
		std::error_code missing;
		if (!std::filesystem::exists(executable, missing))
			return false;

		// A running image is mapped without write sharing, so opening it for write
		// fails with a sharing violation - and that is the only failure that means
		// "somebody is using this". Any other failure - a read-only attribute, an ACL, a
		// half-written file - must not be read as "in use": doing that left a damaged
		// payload whose executable existed but could not be written with its marker
		// restored, so it was never unpacked again and the application never started.
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			const HANDLE probe = CreateFileW(executable.c_str(), GENERIC_WRITE,
				FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (probe != INVALID_HANDLE_VALUE)
			{
				CloseHandle(probe);
				return false;
			}

			const DWORD failure = GetLastError();

			if (failure != ERROR_SHARING_VIOLATION && failure != ERROR_LOCK_VIOLATION)
				return false;

			// A scanner can hold a freshly written image for a moment, so the sharing
			// violation is retried once before the directory counts as in use.
			Sleep(150);
		}

		return true;
	}

	/// <summary>Removes payload directories left behind by earlier versions.</summary>
	void PruneOldPayloads(std::filesystem::path const& current)
	{
		std::error_code error;
		auto parent = current.parent_path();

		if (!std::filesystem::exists(parent, error))
			return;

		for (auto const& entry : std::filesystem::directory_iterator(parent, error))
		{
			if (error)
				return;
			if (!entry.is_directory(error) || entry.path() == current)
				continue;

			// Removing the payload an older instance is still running from deletes
			// the files that instance may yet load: remove_all only fails on what is
			// currently mapped and happily deletes the rest. A payload in use is
			// therefore left alone, and a later launch prunes it.
			if (IsPayloadInUse(entry.path()))
				continue;

			std::filesystem::remove_all(entry.path(), error);
			error.clear();
		}
	}

	/// <summary>
	/// One argument, quoted so that the child's CommandLineToArgvW gives back
	/// exactly this string.
	///
	/// Wrapping in quotes is not enough: a backslash that ends an argument escapes
	/// the closing quote, so that "C:\tmp\" reparses as C:\tmp" and swallows the
	/// next argument, and a quote inside an argument merges arguments. The rule the
	/// runtime parses by is that backslashes are literal unless they precede a
	/// quote, where they must be doubled - and doubled once more, plus one, to
	/// escape a quote itself.
	/// </summary>
	std::wstring QuoteArgument(std::wstring_view argument)
	{
		std::wstring quoted;
		quoted.reserve(argument.size() + 2);
		quoted.push_back(L'"');

		size_t backslashes = 0;
		for (const wchar_t character : argument)
		{
			if (character == L'\\')
			{
				++backslashes;
				continue;
			}

			if (character == L'"')
			{
				quoted.append(backslashes * 2 + 1, L'\\');
				backslashes = 0;
				quoted.push_back(L'"');
				continue;
			}

			quoted.append(backslashes, L'\\');
			backslashes = 0;
			quoted.push_back(character);
		}

		// Whatever is left sits against the closing quote and would escape it.
		quoted.append(backslashes * 2, L'\\');
		quoted.push_back(L'"');

		return quoted;
	}

	std::wstring BuildCommandLine(std::filesystem::path const& appPath)
	{
		std::wstring commandLine = QuoteArgument(appPath.wstring());

		int argumentCount = 0;
		LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
		if (arguments == nullptr)
			return commandLine;

		for (int i = 1; i < argumentCount; ++i)
		{
			commandLine += L' ';
			commandLine += QuoteArgument(arguments[i]);
		}

		LocalFree(arguments);
		return commandLine;
	}
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	wchar_t selfPath[MAX_PATH * 4]{};
	DWORD selfLength = GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
	if (selfLength == 0 || selfLength >= ARRAYSIZE(selfPath))
		Fail(L"Cannot determine the path of this executable.");

	std::filesystem::path self{ selfPath };

	HANDLE file = CreateFileW(
		selfPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		Fail(L"Cannot open this executable.");

	LARGE_INTEGER fileSize{};
	Footer footer;
	if (!GetFileSizeEx(file, &fileSize) ||
		!ReadFooter(file, static_cast<uint64_t>(fileSize.QuadPart), footer))
	{
		Fail(L"This executable does not contain an AudioPlaybackConnectorWinUI payload.");
	}

	auto target = TargetDirectory(footer.id);
	std::error_code error;

	// Two launches at the same moment - a double click, or "start with Windows"
	// racing a manual start - must not unpack into the same directory at once: one
	// run would delete the files the other had just written and still leave the
	// completion marker behind, wedging every later launch on a half payload.
	const HANDLE unpackLock = CreateMutexW(nullptr, FALSE, kUnpackMutexName);
	if (unpackLock == nullptr)
		Fail(L"Cannot prepare the application payload for unpacking.");

	const DWORD unpackWait = WaitForSingleObject(unpackLock, kUnpackLockTimeoutMs);

	// WAIT_ABANDONED means the previous owner died mid-unpack; the marker is
	// re-checked below, so taking over is safe.
	if (unpackWait != WAIT_OBJECT_0 && unpackWait != WAIT_ABANDONED)
		Fail(L"Another copy of AudioPlaybackConnectorWinUI is still unpacking. Please try again in a moment.");

	auto marker = target / kCompleteMarker;

	// On every launch, not only on the one that unpacks. A stale directory that was
	// still in use when it was last looked at is skipped then, and it is this later
	// call that removes it once its instance has exited. Pruning only inside the
	// unpack branch left such a directory - 61 MB for a self-contained build - behind
	// for ever, because a launch that finds its own marker never unpacks again.
	PruneOldPayloads(target);

	if (!std::filesystem::exists(marker, error))
	{
		SetCursor(LoadCursorW(nullptr, IDC_WAIT));

		// The marker can be gone while the payload itself is intact - an interrupted
		// unpack, or a prune that was killed between deleting the marker and the
		// files. Re-unpacking over a payload a running instance is executing would
		// delete libraries that instance has not loaded yet, so that case only
		// restores the marker and leaves the files alone.
		if (!IsPayloadInUse(target))
		{
			std::filesystem::remove_all(target, error);
			if (error)
				Fail(L"The previous application payload could not be removed from " + target.wstring());

			std::filesystem::create_directories(target, error);
			if (error)
				Fail(L"The application payload directory could not be created at " + target.wstring());

			ExtractPayload(file, footer, target);
		}

		HANDLE complete = CreateFileW(
			marker.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

		// Without the marker every later launch unpacks again: slow, churning the
		// disk, and hiding whatever prevented the write.
		if (complete == INVALID_HANDLE_VALUE)
			Fail(L"The application payload could not be marked as complete in " + target.wstring());

		CloseHandle(complete);
	}

	CloseHandle(file);

	auto appPath = target / kAppFileName;
	if (!std::filesystem::exists(appPath, error))
		Fail(L"The bundled application could not be unpacked to " + target.wstring());

	// Keep the settings file beside the distributed single file: portable, and
	// preserved across payload upgrades.
	SetEnvironmentVariableW(kSettingsDirVariable, self.parent_path().c_str());

	// The distributed file itself, so the application can register that path for
	// "start with Windows" instead of the folder it was unpacked into.
	SetEnvironmentVariableW(kLauncherPathVariable, self.c_str());

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	PROCESS_INFORMATION processInfo{};

	std::wstring commandLine = BuildCommandLine(appPath);

	const BOOL started = CreateProcessW(
		appPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
		0, nullptr, target.c_str(), &startupInfo, &processInfo);

	// Held until the child exists. While it is held no other launcher can prune, so
	// this directory cannot be deleted between the checks above and this call by a
	// concurrent launch of a different payload. (Fail() ends the process, and the
	// kernel releases the mutex with it.)
	ReleaseMutex(unpackLock);
	CloseHandle(unpackLock);

	if (!started)
		Fail(L"Cannot start " + appPath.wstring());

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	return 0;
}
