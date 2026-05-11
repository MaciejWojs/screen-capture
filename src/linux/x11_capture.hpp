#pragma once

#include "platform_capture_base.hpp"
#include "wayland_helpers.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

class X11PlatformCapture final : public BaseLinuxPlatformCapture {
    public:
    X11PlatformCapture() = default;
    ~X11PlatformCapture() override;

    void Start(Napi::Env env) override;
    void Stop() override;
    std::optional<std::vector<uint8_t>> GetPixelData(std::string_view desiredFormat = "rgba") const override;
    int GetWidth() const override;
    int GetHeight() const override;
    int GetStride() const override;
    uint32_t GetPixelFormat() const override;

    std::vector<MonitorMetadata> GetMonitors() const override;
    std::string GetBackendName() const override;
    std::optional<MonitorMetadata> GetCurrentMonitorInfo() const override;

    private:
    mutable FrameBufferPool m_frameBuffers;
    std::array<UniqueFd, 2> m_captureFds;
    std::array<MmapPtr, 2> m_captureMappings;
    size_t m_captureWriteIndex = 0;

    mutable std::vector<uint8_t> m_pixelCache;
    mutable std::string m_cacheFormat;

    void CaptureLoop(std::stop_token stopToken);
};