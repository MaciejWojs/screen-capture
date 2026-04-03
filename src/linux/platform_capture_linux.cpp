#ifdef __linux__

#include "../platform_capture.hpp"

#include <stdexcept>

class LinuxPlatformCapture final : public IPlatformCapture {
    public:
    void Start(Napi::Env) override {
        throw std::runtime_error(
            "Linux backend is not implemented yet. Add Wayland (PipeWire portal) or X11 capture here."
        );
    }

    void Stop() override {}

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        return std::nullopt;
    }
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
    return std::make_unique<LinuxPlatformCapture>();
}

#endif
