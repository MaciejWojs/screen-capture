#pragma once

#include <napi.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    virtual std::optional<std::vector<uint8_t>> GetPixelData(std::string_view desiredFormat = "rgba") const { return std::nullopt; }
    virtual int GetWidth() const { return 0; }
    virtual int GetHeight() const { return 0; }
    virtual int GetStride() const { return 0; }
    virtual uint32_t GetPixelFormat() const { return 0; }
    virtual std::string GetBackendName() const { return "unknown"; }
    // Returns FPS or -1 if not implemented
    virtual int GetFps() const { return -1; }
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture(const std::string& forceBackend = "");
