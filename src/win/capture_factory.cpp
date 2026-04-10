#ifdef _WIN32
#include "win_capture_internal.hpp"
#include <windows.h>
#include <stdio.h>

#if HAS_WINRT_CAPTURE
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Metadata.h>
#endif

// Useful for debugging the 'force_api' variable from prebuildify/node-gyp
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#pragma message(">>> GYP 'force_api' VARIABLE VALUE: " STR(GYP_FORCE_API) " <<<")

// Logging the selected flag during compilation (visible in MSVC)
#ifdef FORCE_API_GDI
#pragma message(">>> COMPILING WITH FORCED API: GDI <<<")
#elif defined(FORCE_API_DXGI)
#pragma message(">>> COMPILING WITH FORCED API: DXGI <<<")
#elif defined(FORCE_API_WINRT)
#pragma message(">>> COMPILING WITH FORCED API: WINRT <<<")
#else
#pragma message(">>> COMPILING WITH API: AUTO (RUNTIME SELECTION) <<<")
#endif

bool IsWinRTCaptureAvailable() {
#if HAS_WINRT_CAPTURE
    HMODULE mod = LoadLibraryW(L"windows.graphics.capture.dll");
    if (!mod) return false;
    FreeLibrary(mod);

    // LoadLibrary succeeds, now we can safely use the API
    return winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession"
    );
#else
    return false;
#endif
}

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
#ifdef FORCE_API_GDI
    OutputDebugStringA("[ScreenCapture] INFO: Initializing with FORCED API = GDI (Compilation)\n");
    return CreateGDICapture();
#elif defined(FORCE_API_DXGI)
    OutputDebugStringA("[ScreenCapture] INFO: Initializing with FORCED API = DXGI Desktop Duplication (Compilation)\n");
    return CreateDXGICapture();
#elif defined(FORCE_API_WINRT)
    OutputDebugStringA("[ScreenCapture] INFO: Initializing with FORCED API = WinRT Graphics Capture (Compilation)\n");
#if HAS_WINRT_CAPTURE
    return CreateWinRTCapture();
#else
    OutputDebugStringA("[ScreenCapture] WARN: WinRT not available in compiler. Returning nullptr.\n");
    return nullptr;
#endif
#else
    // Automatic, standard fallback
    OutputDebugStringA("[ScreenCapture] INFO: Initializing in AUTO mode...\n");
#if HAS_WINRT_CAPTURE
    if (IsWinRTCaptureAvailable()) {
        OutputDebugStringA("[ScreenCapture] INFO: WinRT support detected - using WinRT capture.\n");
        if (auto cap = CreateWinRTCapture()) {
            return cap;
        }
    } else {
        OutputDebugStringA("[ScreenCapture] INFO: WinRT is not available on this Windows version.\n");
    }
#endif

    // Fallback to DXGI Desktop Duplication API (Works great on Windows 8 and older builds of Windows 10)
    OutputDebugStringA("[ScreenCapture] INFO: Starting DXGI Desktop Duplication API...\n");
    if (auto cap = CreateDXGICapture()) {
        OutputDebugStringA("[ScreenCapture] INFO: Success. Using DXGI Desktop Duplication.\n");
        return cap;
    }

    // Oldest fallback (Windows 7 / XP)
    OutputDebugStringA("[ScreenCapture] INFO: Old system or DXGI failure detected. Last resort. Using GDI BitBlt.\n");
    return CreateGDICapture();
#endif
}

#endif