#pragma once

#include <napi.h>
#include <cstdint>
#include <memory>
#include <optional>

struct SharedHandleInfo {
    uint64_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class IPlatformCapture {
    public:
    virtual ~IPlatformCapture() = default;

    virtual void Start(Napi::Env env) = 0;
    virtual void Stop() = 0;
    virtual std::optional<SharedHandleInfo> GetSharedHandle() const = 0;
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture();
