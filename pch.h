#ifndef PCH_H
#define PCH_H

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <BluetoothAPIs.h>
#include <bthsdpdef.h>

// windows.h defines GetCurrentTime as a macro, which clashes with the
// Storyboard::GetCurrentTime member projected by C++/WinRT.
#undef GetCurrentTime

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Windows Implementation Library
#ifndef _DEBUG
#define RESULT_DIAGNOSTICS_LEVEL 1
#endif
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <wil/resource.h>

// C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>

// WinUI 3 (Windows App SDK)
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Windows.Graphics.h>

#endif // PCH_H
