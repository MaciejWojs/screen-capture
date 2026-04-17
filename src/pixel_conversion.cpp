#ifdef __linux__
#include <spa/param/video/format-utils.h>
#endif

#include "pixel_conversion.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iostream>
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace {
    enum class PixelLayout {
        RGBA,
        BGRA,
        RGBX,
        BGRX,
        XRGB,
        XBGR,
        UNKNOWN,
    };

    using RowConverterFunc = void (*)(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout);

    static PixelLayout ParsePixelLayout(std::string format) {
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        if (format == "rgba") return PixelLayout::RGBA;
        if (format == "bgra") return PixelLayout::BGRA;
        if (format == "rgbx") return PixelLayout::RGBX;
        if (format == "bgrx") return PixelLayout::BGRX;
        if (format == "xrgb") return PixelLayout::XRGB;
        if (format == "xbgr") return PixelLayout::XBGR;
        return PixelLayout::UNKNOWN;
    }

    static PixelLayout DecodePixelLayout(uint32_t pixelFormat) {
#ifdef __linux__
        switch (pixelFormat) {
        case SPA_VIDEO_FORMAT_BGRA:
            return PixelLayout::BGRA;
        case SPA_VIDEO_FORMAT_RGBA:
            return PixelLayout::RGBA;
        case SPA_VIDEO_FORMAT_BGRx:
            return PixelLayout::BGRX;
        case SPA_VIDEO_FORMAT_RGBx:
            return PixelLayout::RGBX;
        case SPA_VIDEO_FORMAT_xRGB:
            return PixelLayout::XRGB;
        case SPA_VIDEO_FORMAT_xBGR:
            return PixelLayout::XBGR;
        default:
            return PixelLayout::UNKNOWN;
        }
#else
        return PixelLayout::UNKNOWN;
#endif
    }

    static inline uint32_t LoadUInt32(const uint8_t* ptr) {
        uint32_t value;
        std::memcpy(&value, ptr, sizeof(value));
        return value;
    }

    static inline void StoreUInt32(uint8_t* ptr, uint32_t value) {
        std::memcpy(ptr, &value, sizeof(value));
    }

    static inline uint32_t ConvertToRGBA(uint32_t pixel, PixelLayout layout) {
        switch (layout) {
        case PixelLayout::RGBA:
            return pixel;
        case PixelLayout::BGRA:
            return ((pixel & 0x000000FFu) << 16)
                | (pixel & 0x0000FF00u)
                | ((pixel & 0x00FF0000u) >> 16)
                | (pixel & 0xFF000000u);
        case PixelLayout::RGBX:
            return pixel | 0xFF000000u;
        case PixelLayout::BGRX:
            return ((pixel & 0x000000FFu) << 16)
                | (pixel & 0x0000FF00u)
                | ((pixel & 0x00FF0000u) >> 16)
                | 0xFF000000u;
        case PixelLayout::XRGB:
            return ((pixel & 0x0000FF00u) >> 8)
                | ((pixel & 0x00FF0000u) >> 8)
                | ((pixel & 0xFF000000u) >> 8)
                | 0xFF000000u;
        case PixelLayout::XBGR:
            return ((pixel & 0xFF000000u) >> 24)
                | ((pixel & 0x00FF0000u) >> 8)
                | ((pixel & 0x0000FF00u) << 8)
                | 0xFF000000u;
        default:
            return pixel;
        }
    }

    static inline uint32_t ConvertFromRGBA(uint32_t rgba, PixelLayout layout) {
        switch (layout) {
        case PixelLayout::RGBA:
            return rgba;
        case PixelLayout::BGRA:
            return ((rgba & 0x000000FFu) << 16)
                | (rgba & 0x0000FF00u)
                | ((rgba & 0x00FF0000u) >> 16)
                | (rgba & 0xFF000000u);
        case PixelLayout::RGBX:
            return rgba & 0x00FFFFFFu;
        case PixelLayout::BGRX:
            return ((rgba & 0x000000FFu) << 16)
                | (rgba & 0x0000FF00u)
                | ((rgba & 0x00FF0000u) >> 16);
        case PixelLayout::XRGB:
            return ((rgba & 0x000000FFu) << 8)
                | ((rgba & 0x0000FF00u) << 8)
                | ((rgba & 0x00FF0000u) << 8);
        case PixelLayout::XBGR:
            return ((rgba & 0x000000FFu) << 24)
                | ((rgba & 0x0000FF00u) << 8)
                | ((rgba & 0x00FF0000u) >> 8);
        default:
            return rgba;
        }
    }

    static bool IsFourByteFormat(uint32_t pixelFormat) {
#ifdef __linux__
        switch (pixelFormat) {
        case SPA_VIDEO_FORMAT_BGRA:
        case SPA_VIDEO_FORMAT_RGBA:
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_xBGR:
        case SPA_VIDEO_FORMAT_xRGB:
            return true;
        default:
            return false;
        }
#else
        return false;
#endif
    }

    static bool SupportsSSSE3() {
#if defined(__SSSE3__)
#if defined(_MSC_VER)
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        return (cpuInfo[2] & (1 << 9)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
        return __builtin_cpu_supports("ssse3");
#else
        return false;
#endif
#else
        return false;
#endif
    }

    static bool SupportsAVX2() {
#if defined(__AVX2__)
#if defined(_MSC_VER)
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        bool osUsesXSAVE_XRSTORE = (cpuInfo[2] & (1 << 27)) != 0;
        bool cpuAVXSupport = (cpuInfo[2] & (1 << 28)) != 0;
        if (!osUsesXSAVE_XRSTORE || !cpuAVXSupport) {
            return false;
        }
        unsigned long long xcrFeatureMask = _xgetbv(_XCR_XFEATURE_ENABLED_MASK);
        if ((xcrFeatureMask & 0x6ull) != 0x6ull) {
            return false;
        }
        __cpuid(cpuInfo, 7);
        return (cpuInfo[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
        return __builtin_cpu_supports("avx2");
#else
        return false;
#endif
#else
        return false;
#endif
    }

    static inline bool NeedsAlphaFill(PixelLayout layout) {
        return layout == PixelLayout::RGBX
            || layout == PixelLayout::BGRX
            || layout == PixelLayout::XRGB
            || layout == PixelLayout::XBGR;
    }

    static inline __m128i LoadShuffleMask(const signed char mask[16]) {
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask));
    }

    static inline __m256i BroadcastShuffleMask(const signed char mask[16]) {
        return _mm256_broadcastsi128_si256(LoadShuffleMask(mask));
    }

    static const signed char kSrcToRgbaMask[6][16] = {
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
        { 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15 },
        { 0, 1, 2, -128, 4, 5, 6, -128, 8, 9, 10, -128, 12, 13, 14, -128 },
        { 2, 1, 0, -128, 6, 5, 4, -128, 10, 9, 8, -128, 14, 13, 12, -128 },
        { 1, 2, 3, -128, 5, 6, 7, -128, 9, 10, 11, -128, 13, 14, 15, -128 },
        { 3, 2, 1, -128, 7, 6, 5, -128, 11, 10, 9, -128, 15, 14, 13, -128 },
    };

    static const signed char kRgbaToDstMask[6][16] = {
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
        { 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15 },
        { 0, 1, 2, -128, 4, 5, 6, -128, 8, 9, 10, -128, 12, 13, 14, -128 },
        { 2, 1, 0, -128, 6, 5, 4, -128, 10, 9, 8, -128, 14, 13, 12, -128 },
        { -128, 0, 1, 2, -128, 4, 5, 6, -128, 8, 9, 10, -128, 12, 13, 14 },
        { -128, 2, 1, 0, -128, 6, 5, 4, -128, 10, 9, 8, -128, 14, 13, 12 },
    };

    static void convertRow_scalar(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        for (size_t col = 0; col < width; ++col) {
            uint32_t pixel = LoadUInt32(src + col * 4);
            uint32_t normalized = ConvertToRGBA(pixel, srcLayout);
            uint32_t converted = ConvertFromRGBA(normalized, dstLayout);
            StoreUInt32(dst + col * 4, converted);
        }
    }

#if defined(__SSSE3__)
    static void convertRow_ssse3(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const __m128i srcMask = LoadShuffleMask(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const __m128i dstMask = LoadShuffleMask(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const __m128i alphaMask = _mm_set1_epi32(0xFF000000u);

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 4) {
            __m128i source = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            __m128i rgba = _mm_shuffle_epi8(source, srcMask);
            if (needAlphaFill) {
                rgba = _mm_or_si128(rgba, alphaMask);
            }
            __m128i result = _mm_shuffle_epi8(rgba, dstMask);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), result);
            src += 16;
            dst += 16;
            pixelsRemaining -= 4;
        }

        if (pixelsRemaining > 0) {
            convertRow_scalar(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#endif

#if defined(__AVX2__)
    static void convertRow_avx2(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const __m256i srcMask = BroadcastShuffleMask(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const __m256i dstMask = BroadcastShuffleMask(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const __m256i alphaMask = _mm256_set1_epi32(0xFF000000u);

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 8) {
            __m256i source = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
            __m256i rgba = _mm256_shuffle_epi8(source, srcMask);
            if (needAlphaFill) {
                rgba = _mm256_or_si256(rgba, alphaMask);
            }
            __m256i result = _mm256_shuffle_epi8(rgba, dstMask);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), result);
            src += 32;
            dst += 32;
            pixelsRemaining -= 8;
        }

        if (pixelsRemaining > 0) {
            convertRow_ssse3(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#endif
}

static std::atomic<bool> s_converterMethodLogged{ false };

static void LogConverterMethodOnce(const char* method) {
    bool expected = false;
    if (s_converterMethodLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::cerr << "[PixelConversion] Using converter: " << method << std::endl;
    }
}

std::vector<uint8_t> ConvertPixelBuffer(
    const uint8_t* src,
    size_t sourceSize,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t srcPixelFormat,
    const std::string& desiredPixelFormat) {
    PixelLayout dstLayout = ParsePixelLayout(desiredPixelFormat);
    PixelLayout srcLayout = DecodePixelLayout(srcPixelFormat);
    if (dstLayout == PixelLayout::UNKNOWN || srcLayout == PixelLayout::UNKNOWN) {
        return std::vector<uint8_t>(src, src + sourceSize);
    }

    std::vector<uint8_t> dst(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const size_t srcRowBytes = stride;
    const size_t dstRowBytes = static_cast<size_t>(width) * 4;

    RowConverterFunc converter = convertRow_scalar;
    const char* converterName = "scalar";
#if defined(__AVX2__)
    if (SupportsAVX2()) {
        converter = convertRow_avx2;
        converterName = "avx2";
    } else
#endif
#if defined(__SSSE3__)
        if (SupportsSSSE3()) {
            converter = convertRow_ssse3;
            converterName = "ssse3";
        }
#endif

    LogConverterMethodOnce(converterName);

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* srcRow = src + static_cast<size_t>(row) * srcRowBytes;
        uint8_t* dstRow = dst.data() + static_cast<size_t>(row) * dstRowBytes;

        if (srcLayout == dstLayout) {
            if (srcRowBytes == dstRowBytes) {
                std::memcpy(dstRow, srcRow, dstRowBytes);
            } else {
                for (uint32_t col = 0; col < width; ++col) {
                    StoreUInt32(dstRow + static_cast<size_t>(col) * 4,
                        LoadUInt32(srcRow + static_cast<size_t>(col) * 4));
                }
            }
        } else {
            converter(srcRow, dstRow, width, srcLayout, dstLayout);
        }
    }

    return dst;
}
