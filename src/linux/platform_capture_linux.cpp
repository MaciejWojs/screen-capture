#ifdef __linux__

#include "../platform_capture.hpp"
#include "../logger.hpp"
#include "x11_capture.hpp"
#include "wayland_capture.hpp"

bool IsWayland() {
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay && waylandDisplay[0] != '\0') return true;

    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && std::string(sessionType) == "wayland") return true;

    return false;
}

std::unique_ptr<IPlatformCapture> CreatePlatformCapture(const std::string& /*forceBackend*/) {
    if (IsWayland()) {
        sc_logger::Info("Detected Wayland environment");
        return std::make_unique<WaylandPlatformCapture>();
    } else {
        sc_logger::Info("Detected X11 environment");
        return std::make_unique<X11PlatformCapture>();
    }
}

#endif
