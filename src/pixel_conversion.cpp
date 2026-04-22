#ifdef __linux__
#include <spa/param/video/format-utils.h>
#include "logger.hpp"
#endif

#include <span>
#include <string_view>
#include "pixel_conversion.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstring>
#include <iostream>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__linux__) && defined(__arm__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TARGET_ATTR(x) __attribute__((target(x)))
#else
#define TARGET_ATTR(x)
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
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

    static PixelLayout ParsePixelLayout(std::string_view format) {
        std::string normalized(format);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        if (normalized == "rgba") return PixelLayout::RGBA;
        if (normalized == "bgra") return PixelLayout::BGRA;
        if (normalized == "rgbx") return PixelLayout::RGBX;
        if (normalized == "bgrx") return PixelLayout::BGRX;
        if (normalized == "xrgb") return PixelLayout::XRGB;
        if (normalized == "xbgr") return PixelLayout::XBGR;
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
#if defined(_MSC_VER)
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        return (cpuInfo[2] & (1 << 9)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
        return __builtin_cpu_supports("ssse3");
#else
        return false;
#endif
    }

    static bool SupportsAVX2() {
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
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
        return __builtin_cpu_supports("avx2");
#else
        return false;
#endif
    }

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    static bool SupportsAVX512() {
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
        return (cpuInfo[1] & (1 << 30)) != 0; // AVX512BW
#elif defined(__GNUC__) || defined(__clang__)
        return __builtin_cpu_supports("avx512bw");
#else
        return false;
#endif
    }
#else
    static bool SupportsAVX512() {
        return false;
    }
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    static bool SupportsNEON() {
#if defined(__aarch64__)
        // AArch64 always has Advanced SIMD
        return true;
#elif defined(__arm__)
#if defined(__linux__)
        unsigned long hwcaps = getauxval(AT_HWCAP);
#ifdef HWCAP_NEON
        return (hwcaps & HWCAP_NEON) != 0;
#else
        return false;
#endif
#else
        return false;
#endif
#else
        return false;
#endif
    }
#endif

    static inline bool NeedsAlphaFill(PixelLayout layout) {
        return layout == PixelLayout::RGBX
            || layout == PixelLayout::BGRX
            || layout == PixelLayout::XRGB
            || layout == PixelLayout::XBGR;
    }

    static inline bool ShouldPrefetch(size_t width) {
        return width >= 256;
    }

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    static inline void PrefetchIfNeeded(const uint8_t* ptr, bool enabled) {
        if (enabled) {
            _mm_prefetch(std::bit_cast<const char*>(ptr), _MM_HINT_T0);
        }
    }

    static inline __m128i LoadShuffleMask(const signed char mask[16]) {
        return _mm_loadu_si128(std::bit_cast<const __m128i*>(mask));
    }

    TARGET_ATTR("avx2") static inline __m256i BroadcastShuffleMask(const signed char mask[16]) {
        return _mm256_broadcastsi128_si256(LoadShuffleMask(mask));
    }

    TARGET_ATTR("avx512bw") static inline __m512i BroadcastShuffleMask512(const signed char mask[16]) {
        alignas(64) unsigned char buffer[64];
        for (int i = 0; i < 4; ++i) {
            std::memcpy(buffer + i * 16, mask, 16);
        }
        return _mm512_loadu_si512(buffer);
    }
#else
    static inline void PrefetchIfNeeded(const uint8_t*, bool) {}
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    static inline uint8x16_t LoadNeonShuffleMask(const signed char mask[16]) {
        return vld1q_u8(std::bit_cast<const uint8_t*>(mask));
    }

    static inline uint8x16_t LoadNeonAlphaMask() {
        static const uint8_t alphaValues[16] = {
            0x00, 0x00, 0x00, 0xFF,
            0x00, 0x00, 0x00, 0xFF,
            0x00, 0x00, 0x00, 0xFF,
            0x00, 0x00, 0x00, 0xFF,
        };
        return vld1q_u8(alphaValues);
    }
#endif

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

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    TARGET_ATTR("ssse3") static void convertRow_ssse3(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const bool prefetch = ShouldPrefetch(width);
        const __m128i srcMask = LoadShuffleMask(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const __m128i dstMask = LoadShuffleMask(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const __m128i alphaMask = _mm_set1_epi32(0xFF000000u);
        const uint8_t* end = src + width * 4;
        const uint8_t* prefetchPtr = src + 64;

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 8) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 32;
            }

            __m128i source0 = _mm_loadu_si128(std::bit_cast<const __m128i*>(src));
            __m128i source1 = _mm_loadu_si128(std::bit_cast<const __m128i*>(src + 16));
            __m128i rgba0 = _mm_shuffle_epi8(source0, srcMask);
            __m128i rgba1 = _mm_shuffle_epi8(source1, srcMask);
            if (needAlphaFill) {
                rgba0 = _mm_or_si128(rgba0, alphaMask);
                rgba1 = _mm_or_si128(rgba1, alphaMask);
            }
            __m128i result0 = _mm_shuffle_epi8(rgba0, dstMask);
            __m128i result1 = _mm_shuffle_epi8(rgba1, dstMask);
            _mm_storeu_si128(std::bit_cast<__m128i*>(dst), result0);
            _mm_storeu_si128(std::bit_cast<__m128i*>(dst + 16), result1);
            src += 32;
            dst += 32;
            pixelsRemaining -= 8;
        }

        while (pixelsRemaining >= 4) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 16;
            }
            __m128i source = _mm_loadu_si128(std::bit_cast<const __m128i*>(src));
            __m128i rgba = _mm_shuffle_epi8(source, srcMask);
            if (needAlphaFill) {
                rgba = _mm_or_si128(rgba, alphaMask);
            }
            __m128i result = _mm_shuffle_epi8(rgba, dstMask);
            _mm_storeu_si128(std::bit_cast<__m128i*>(dst), result);
            src += 16;
            dst += 16;
            pixelsRemaining -= 4;
        }

        if (pixelsRemaining > 0) {
            convertRow_scalar(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    static void convertRow_avx2(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout);
    TARGET_ATTR("avx512bw") static void convertRow_avx512(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const bool prefetch = ShouldPrefetch(width);
        const __m512i srcMask = BroadcastShuffleMask512(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const __m512i dstMask = BroadcastShuffleMask512(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const __m512i alphaMask = _mm512_set1_epi32(0xFF000000u);
        const uint8_t* end = src + width * 4;
        const uint8_t* prefetchPtr = src + 64;

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 16) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 64;
            }

            __m512i source = _mm512_loadu_si512(std::bit_cast<const void*>(src));
            __m512i rgba = _mm512_shuffle_epi8(source, srcMask);
            if (needAlphaFill) {
                rgba = _mm512_or_si512(rgba, alphaMask);
            }
            __m512i result = _mm512_shuffle_epi8(rgba, dstMask);
            _mm512_storeu_si512(std::bit_cast<void*>(dst), result);
            src += 64;
            dst += 64;
            pixelsRemaining -= 16;
        }

        if (pixelsRemaining > 0) {
            convertRow_avx2(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    static void convertRow_neon(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const uint8x16_t srcMask = LoadNeonShuffleMask(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const uint8x16_t dstMask = LoadNeonShuffleMask(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const uint8x16_t alphaMask = LoadNeonAlphaMask();

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 8) {
            uint8x16_t source0 = vld1q_u8(src);
            uint8x16_t source1 = vld1q_u8(src + 16);
            uint8x16_t rgba0 = vqtbl1q_u8(source0, srcMask);
            uint8x16_t rgba1 = vqtbl1q_u8(source1, srcMask);
            if (needAlphaFill) {
                rgba0 = vorrq_u8(rgba0, alphaMask);
                rgba1 = vorrq_u8(rgba1, alphaMask);
            }
            uint8x16_t result0 = vqtbl1q_u8(rgba0, dstMask);
            uint8x16_t result1 = vqtbl1q_u8(rgba1, dstMask);
            vst1q_u8(dst, result0);
            vst1q_u8(dst + 16, result1);
            src += 32;
            dst += 32;
            pixelsRemaining -= 8;
        }

        while (pixelsRemaining >= 4) {
            uint8x16_t source = vld1q_u8(src);
            uint8x16_t rgba = vqtbl1q_u8(source, srcMask);
            if (needAlphaFill) {
                rgba = vorrq_u8(rgba, alphaMask);
            }
            uint8x16_t result = vqtbl1q_u8(rgba, dstMask);
            vst1q_u8(dst, result);
            src += 16;
            dst += 16;
            pixelsRemaining -= 4;
        }

        if (pixelsRemaining > 0) {
            convertRow_scalar(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
    TARGET_ATTR("avx2") static void convertRow_avx2(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const bool prefetch = ShouldPrefetch(width);
        const __m256i srcMask = BroadcastShuffleMask(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const __m256i dstMask = BroadcastShuffleMask(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const __m256i alphaMask = _mm256_set1_epi32(0xFF000000u);
        const uint8_t* end = src + width * 4;
        const uint8_t* prefetchPtr = src + 64;

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 16) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 64;
            }

            __m256i source0 = _mm256_loadu_si256(std::bit_cast<const __m256i*>(src));
            __m256i source1 = _mm256_loadu_si256(std::bit_cast<const __m256i*>(src + 32));
            __m256i rgba0 = _mm256_shuffle_epi8(source0, srcMask);
            __m256i rgba1 = _mm256_shuffle_epi8(source1, srcMask);
            if (needAlphaFill) {
                rgba0 = _mm256_or_si256(rgba0, alphaMask);
                rgba1 = _mm256_or_si256(rgba1, alphaMask);
            }
            __m256i result0 = _mm256_shuffle_epi8(rgba0, dstMask);
            __m256i result1 = _mm256_shuffle_epi8(rgba1, dstMask);
            _mm256_storeu_si256(std::bit_cast<__m256i*>(dst), result0);
            _mm256_storeu_si256(std::bit_cast<__m256i*>(dst + 32), result1);
            src += 64;
            dst += 64;
            pixelsRemaining -= 16;
        }

        while (pixelsRemaining >= 8) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 32;
            }
            __m256i source = _mm256_loadu_si256(std::bit_cast<const __m256i*>(src));
            __m256i rgba = _mm256_shuffle_epi8(source, srcMask);
            if (needAlphaFill) {
                rgba = _mm256_or_si256(rgba, alphaMask);
            }
            __m256i result = _mm256_shuffle_epi8(rgba, dstMask);
            _mm256_storeu_si256(std::bit_cast<__m256i*>(dst), result);
            src += 32;
            dst += 32;
            pixelsRemaining -= 8;
        }

        if (pixelsRemaining > 0) {
            convertRow_ssse3(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#elif defined(__AVX2__)
    static void convertRow_avx2(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        const bool needAlphaFill = NeedsAlphaFill(srcLayout);
        const bool prefetch = ShouldPrefetch(width);
        const __m256i srcMask = BroadcastShuffleMask(kSrcToRgbaMask[static_cast<size_t>(srcLayout)]);
        const __m256i dstMask = BroadcastShuffleMask(kRgbaToDstMask[static_cast<size_t>(dstLayout)]);
        const __m256i alphaMask = _mm256_set1_epi32(0xFF000000u);
        const uint8_t* end = src + width * 4;
        const uint8_t* prefetchPtr = src + 64;

        size_t pixelsRemaining = width;
        while (pixelsRemaining >= 16) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 64;
            }

            __m256i source0 = _mm256_loadu_si256(std::bit_cast<const __m256i*>(src));
            __m256i source1 = _mm256_loadu_si256(std::bit_cast<const __m256i*>(src + 32));
            __m256i rgba0 = _mm256_shuffle_epi8(source0, srcMask);
            __m256i rgba1 = _mm256_shuffle_epi8(source1, srcMask);
            if (needAlphaFill) {
                rgba0 = _mm256_or_si256(rgba0, alphaMask);
                rgba1 = _mm256_or_si256(rgba1, alphaMask);
            }
            __m256i result0 = _mm256_shuffle_epi8(rgba0, dstMask);
            __m256i result1 = _mm256_shuffle_epi8(rgba1, dstMask);
            _mm256_storeu_si256(std::bit_cast<__m256i*>(dst), result0);
            _mm256_storeu_si256(std::bit_cast<__m256i*>(dst + 32), result1);
            src += 64;
            dst += 64;
            pixelsRemaining -= 16;
        }

        while (pixelsRemaining >= 8) {
            if (prefetch && prefetchPtr < end) {
                PrefetchIfNeeded(prefetchPtr, true);
                prefetchPtr += 32;
            }
            __m256i source = _mm256_loadu_si256(std::bit_cast<const __m256i*>(src));
            __m256i rgba = _mm256_shuffle_epi8(source, srcMask);
            if (needAlphaFill) {
                rgba = _mm256_or_si256(rgba, alphaMask);
            }
            __m256i result = _mm256_shuffle_epi8(rgba, dstMask);
            _mm256_storeu_si256(std::bit_cast<__m256i*>(dst), result);
            src += 32;
            dst += 32;
            pixelsRemaining -= 8;
        }

        if (pixelsRemaining > 0) {
            convertRow_ssse3(src, dst, pixelsRemaining, srcLayout, dstLayout);
        }
    }
#else
    static void convertRow_avx2(const uint8_t* src, uint8_t* dst, size_t width, PixelLayout srcLayout, PixelLayout dstLayout) {
        convertRow_scalar(src, dst, width, srcLayout, dstLayout);
    }
#endif
#endif
}

static std::atomic<bool> s_converterMethodLogged{ false };

static void LogConverterMethodOnce(const char* method) {
    bool expected = false;
    if (s_converterMethodLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        sc_logger::Info("Using converter: {}", method);
    }
}

std::vector<uint8_t> ConvertPixelBuffer(
    std::span<const uint8_t> src,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t srcPixelFormat,
    std::string_view desiredPixelFormat) {
    PixelLayout dstLayout = ParsePixelLayout(desiredPixelFormat);
    PixelLayout srcLayout = DecodePixelLayout(srcPixelFormat);
    if (dstLayout == PixelLayout::UNKNOWN || srcLayout == PixelLayout::UNKNOWN) {
        return std::vector<uint8_t>(src.data(), src.data() + src.size());
    }

    std::vector<uint8_t> dst(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const size_t srcRowBytes = stride;
    const size_t dstRowBytes = static_cast<size_t>(width) * 4;
    const size_t totalPixels = static_cast<size_t>(width) * height;
    const bool isPacked = srcRowBytes == dstRowBytes;

    RowConverterFunc converter = convertRow_scalar;
    const char* converterName = "scalar";
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (SupportsAVX512()) {
        converter = convertRow_avx512;
        converterName = "avx512";
    } else
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        if (SupportsAVX2()) {
            converter = convertRow_avx2;
            converterName = "avx2";
        } else
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            if (SupportsSSSE3()) {
                converter = convertRow_ssse3;
                converterName = "ssse3";
            }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (converter == convertRow_scalar && SupportsNEON()) {
            converter = convertRow_neon;
            converterName = "neon";
        }
#endif

        LogConverterMethodOnce(converterName);

        if (srcLayout == dstLayout) {
            if (isPacked) {
                std::memcpy(dst.data(), src.data(), totalPixels * 4);
            } else {
                for (uint32_t row = 0; row < height; ++row) {
                    const uint8_t* srcRow = src.data() + static_cast<size_t>(row) * srcRowBytes;
                    uint8_t* dstRow = dst.data() + static_cast<size_t>(row) * dstRowBytes;
                    for (uint32_t col = 0; col < width; ++col) {
                        StoreUInt32(dstRow + static_cast<size_t>(col) * 4,
                            LoadUInt32(srcRow + static_cast<size_t>(col) * 4));
                    }
                }
            }
        } else if (isPacked) {
            converter(src.data(), dst.data(), totalPixels, srcLayout, dstLayout);
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                const uint8_t* srcRow = src.data() + static_cast<size_t>(row) * srcRowBytes;
                uint8_t* dstRow = dst.data() + static_cast<size_t>(row) * dstRowBytes;
                converter(srcRow, dstRow, width, srcLayout, dstLayout);
            }
        }

        return dst;
};
