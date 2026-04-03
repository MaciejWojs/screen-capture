#ifndef _WIN32

#include "platform_capture.hpp"

#include <stdexcept>

class StubPlatformCapture final : public IPlatformCapture {
    public:
    void Start(Napi::Env) override {
        throw std::runtime_error("Screen capture backend is not implemented for this platform yet");
    }

    void Stop() override {}

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        return std::nullopt;
    }
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
    return std::make_unique<StubPlatformCapture>();
}

#endif
