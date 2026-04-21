#ifndef _WIN32

#include "platform_capture.hpp"

#include <stdexcept>
#include <string>

class StubPlatformCapture final : public IPlatformCapture {
    public:
    void Start(Napi::Env) override {
        throw std::runtime_error("Screen capture backend is not implemented for this platform yet");
    }

    void Stop() override {}

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        return std::nullopt;
    }

    std::string GetBackendName() const override {
        return "stub";
    }
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture(const std::string& /*forceBackend*/) {
    return std::make_unique<StubPlatformCapture>();
}

#endif
