#pragma once

#include "linux_raii.hpp"
#include "frame_buffer_pool.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include "../pixel_conversion.hpp"

#include <chrono>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <format>

static bool IsNvidiaGPU() {
    static std::once_flag initFlag;
    static bool cached = false;
    std::call_once(initFlag, [] {
        std::ifstream nvidia("/proc/driver/nvidia/version");
        cached = nvidia.good();
        });
    return cached;
}

static inline std::string gen_token() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(0, 15);
    std::string token;
    token.reserve(8);
    for (int i = 0; i < 8; ++i) {
        token += std::format("{:x}", dist(rng));
    }
    return token;
}

static inline std::string PixelFormatToString(uint32_t pixelFormat) {
    switch (pixelFormat) {
    case SPA_VIDEO_FORMAT_BGRA:
        return "bgra";
    case SPA_VIDEO_FORMAT_RGBA:
        return "rgba";
    case SPA_VIDEO_FORMAT_BGRx:
        return "bgrx";
    case SPA_VIDEO_FORMAT_RGBx:
        return "rgbx";
    case SPA_VIDEO_FORMAT_xBGR:
        return "xbgr";
    case SPA_VIDEO_FORMAT_xRGB:
        return "xrgb";
    case SPA_VIDEO_FORMAT_NV12:
        return "nv12";
    case SPA_VIDEO_FORMAT_I420:
        return "i420";
    case SPA_VIDEO_FORMAT_YUY2:
        return "yuy2";
    case SPA_VIDEO_FORMAT_AYUV:
        return "ayuv";
    case SPA_VIDEO_FORMAT_UYVY:
        return "uyvy";
    default:
        return "unknown";
    }
}

static inline std::optional<std::vector<uint8_t>> ReadPixelDataFromSharedFd(
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t offset,
    uint64_t planeSize,
    uint32_t pixelFormat,
    std::string_view desiredFormat) {
    if (fd < 0 || width == 0 || height == 0) {
        return std::nullopt;
    }

    size_t dataSize = planeSize ? static_cast<size_t>(planeSize)
        : static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (dataSize == 0) {
        return std::nullopt;
    }

    size_t mapSize = planeSize ? static_cast<size_t>(planeSize)
        : dataSize + static_cast<size_t>(offset);
    MmapPtr mapped(mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0), MmapDeleter{ mapSize });
    if (!mapped || mapped.get() == MAP_FAILED) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(dataSize);
    memcpy(buffer.data(), static_cast<uint8_t*>(mapped.get()) + static_cast<size_t>(offset), dataSize);

    std::string format = std::string(desiredFormat);
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });

    uint32_t actualStride = stride ? stride : static_cast<uint32_t>(width * 4);
    return ConvertPixelBuffer(
        std::span<const uint8_t>(buffer.data(), buffer.size()),
        width,
        height,
        actualStride,
        pixelFormat,
        format);
}

static inline std::optional<std::vector<uint8_t>> ReadPixelDataFromRawPointer(
    std::span<const uint8_t> data,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint64_t planeSize,
    uint32_t pixelFormat,
    std::string_view desiredFormat) {
    if (data.empty() || width == 0 || height == 0) {
        return std::nullopt;
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    uint32_t actualStride = stride ? stride : static_cast<uint32_t>(rowBytes);
    if (static_cast<size_t>(actualStride) < rowBytes) {
        return std::nullopt;
    }

    size_t expectedSize = planeSize ? static_cast<size_t>(planeSize)
        : static_cast<size_t>(actualStride) * static_cast<size_t>(height);
    if (expectedSize == 0 || expectedSize > data.size()) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(expectedSize);

    if (actualStride == rowBytes) {
        std::memcpy(buffer.data(), data.data(), expectedSize);
    } else {
        for (uint32_t row = 0; row < height; ++row) {
            size_t srcOffset = static_cast<size_t>(row) * actualStride;
            if (srcOffset + rowBytes > data.size()) {
                return std::nullopt;
            }
            const uint8_t* srcRow = data.data() + srcOffset;
            uint8_t* dstRow = buffer.data() + static_cast<size_t>(row) * rowBytes;
            std::memcpy(dstRow, srcRow, rowBytes);
        }
    }

    std::string sourceFormat = PixelFormatToString(pixelFormat);
    std::string normalizedDesired = std::string(desiredFormat);
    std::transform(normalizedDesired.begin(), normalizedDesired.end(), normalizedDesired.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });

    if (normalizedDesired == sourceFormat) {
        if (actualStride == rowBytes) {
            return buffer;
        }
        std::vector<uint8_t> packedResult(rowBytes * static_cast<size_t>(height));
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(
                packedResult.data() + static_cast<size_t>(row) * rowBytes,
                buffer.data() + static_cast<size_t>(row) * rowBytes,
                rowBytes);
        }
        return packedResult;
    }

    return ConvertPixelBuffer(
        std::span<const uint8_t>(buffer.data(), buffer.size()),
        width,
        height,
        actualStride,
        pixelFormat,
        desiredFormat);
}

struct PipeWireInitializer {
    bool initialized = false;

    void EnsureInit() {
        if (!initialized) {
            pw_init(nullptr, nullptr);
            initialized = true;
        }
    }

    ~PipeWireInitializer() {
        if (initialized) {
            pw_deinit();
        }
    }
};

struct GVariantBuilderWrapper {
    GVariantBuilder builder;
    GVariantBuilderWrapper() { g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}")); }
    ~GVariantBuilderWrapper() { g_variant_builder_clear(&builder); }
    operator GVariantBuilder* () { return &builder; }
};

struct StreamState {
    PwThreadLoopPtr pw_loop;
    PwContextPtr context;
    PwCorePtr core;
    PwStreamPtr stream;
    spa_hook stream_listener{};
};

enum class PortalStage {
    Idle,
    CreatingSession,
    SelectingSources,
    StartingSession,
    OpeningRemote,
};

struct MonitorInfo {
    uint32_t nodeId = PW_ID_ANY;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string connector;
    std::string title;
};

struct StreamConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixelFormat = 0;
    uint64_t modifier = 0;
};

namespace Config {
    constexpr uint32_t DEFAULT_WIDTH = 1920;
    constexpr uint32_t DEFAULT_HEIGHT = 1080;
    constexpr uint32_t MAX_WIDTH = 8192;
    constexpr uint32_t MAX_HEIGHT = 8192;
    constexpr uint32_t DEFAULT_FPS_NUM = 60;
    constexpr uint32_t DEFAULT_FPS_DEN = 1;
    constexpr uint32_t MAX_FPS_NUM = 144;
    constexpr size_t POD_BUFFER_SIZE_CONNECT = 8192;
    constexpr size_t POD_BUFFER_SIZE_UPDATE = 4096;
}
