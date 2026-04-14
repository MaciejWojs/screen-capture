#pragma once

#include <napi.h>
#include <cstdint>
#include <memory>
#include <optional>

struct SharedHandleInfo {
    uint64_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t offset = 0;
    uint64_t planeSize = 0;
    uint32_t pixelFormat = 0;
    uint64_t modifier = 0;
    uint32_t bufferType = 0;
    uint32_t chunkSize = 0;
};


class IPlatformCapture {
public:
    virtual ~IPlatformCapture() = default;

    virtual void Start(Napi::Env env) = 0;
    virtual void Stop() = 0;
    virtual std::optional<SharedHandleInfo> GetSharedHandle() const = 0;
    // Returns FPS or -1 if not implemented
    virtual int GetFps() const { return -1; }
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture();
