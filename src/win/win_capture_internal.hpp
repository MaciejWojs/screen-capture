#pragma once

#ifdef _WIN32

#include "../platform_capture.hpp"
#include <memory>

#if defined(__has_include)
#  if __has_include(<winrt/Windows.Graphics.Capture.h>)
#    define HAS_WINRT_CAPTURE 1
#  else
#    define HAS_WINRT_CAPTURE 0
#  endif
#else
#  define HAS_WINRT_CAPTURE 0
#endif

std::unique_ptr<IPlatformCapture> CreateWinRTCapture();
std::unique_ptr<IPlatformCapture> CreateDXGICapture();
std::unique_ptr<IPlatformCapture> CreateGDICapture();

#endif // _WIN32