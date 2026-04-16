#ifdef __linux__
#include <spa/param/video/format-utils.h>
#endif

#include "pixel_conversion.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

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

    static void DecodePixel(uint32_t pixelFormat, const uint8_t* pixel, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
        r = g = b = 0;
        a = 255;

#ifdef __linux__
        switch (pixelFormat) {
        case SPA_VIDEO_FORMAT_BGRA:
            b = pixel[0];
            g = pixel[1];
            r = pixel[2];
            a = pixel[3];
            break;
        case SPA_VIDEO_FORMAT_BGRx:
            b = pixel[0];
            g = pixel[1];
            r = pixel[2];
            a = 255;
            break;
        case SPA_VIDEO_FORMAT_RGBA:
            r = pixel[0];
            g = pixel[1];
            b = pixel[2];
            a = pixel[3];
            break;
        case SPA_VIDEO_FORMAT_RGBx:
            r = pixel[0];
            g = pixel[1];
            b = pixel[2];
            a = 255;
            break;
        case SPA_VIDEO_FORMAT_xRGB:
            r = pixel[1];
            g = pixel[2];
            b = pixel[3];
            a = 255;
            break;
        case SPA_VIDEO_FORMAT_xBGR:
            b = pixel[1];
            g = pixel[2];
            r = pixel[3];
            a = 255;
            break;
        default:
            r = pixel[0];
            g = pixel[1];
            b = pixel[2];
            a = pixel[3];
            break;
        }
#endif
    }

    static void EncodePixel(PixelLayout destFormat, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t* dst) {
        switch (destFormat) {
        case PixelLayout::RGBA:
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = a;
            break;
        case PixelLayout::BGRA:
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = a;
            break;
        case PixelLayout::RGBX:
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = 0;
            break;
        case PixelLayout::BGRX:
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = 0;
            break;
        case PixelLayout::XRGB:
            dst[0] = 0;
            dst[1] = r;
            dst[2] = g;
            dst[3] = b;
            break;
        case PixelLayout::XBGR:
            dst[0] = 0;
            dst[1] = b;
            dst[2] = g;
            dst[3] = r;
            break;
        default:
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = a;
            break;
        }
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
    if (dstLayout == PixelLayout::UNKNOWN || !IsFourByteFormat(srcPixelFormat)) {
        return std::vector<uint8_t>(src, src + sourceSize);
    }

    std::vector<uint8_t> dst(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const size_t srcRowBytes = stride;
    const size_t dstRowBytes = static_cast<size_t>(width) * 4;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* srcRow = src + static_cast<size_t>(row) * srcRowBytes;
        uint8_t* dstRow = dst.data() + static_cast<size_t>(row) * dstRowBytes;

        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t* pixel = srcRow + static_cast<size_t>(col) * 4;
            uint8_t r, g, b, a;
            DecodePixel(srcPixelFormat, pixel, r, g, b, a);
            EncodePixel(dstLayout, r, g, b, a, dstRow + static_cast<size_t>(col) * 4);
        }
    }

    return dst;
}
