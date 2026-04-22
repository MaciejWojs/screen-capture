#include <bit>
#include <string>
#include <vector>

#include "logger.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __linux__
#include <spa/param/video/format.h>
#include <spa/buffer/buffer.h>
#endif

#include "serialize.hpp"

static std::string PixelFormatToString(uint32_t pixelFormat) {
#ifdef __linux__
    switch (pixelFormat) {
    case SPA_VIDEO_FORMAT_BGRA:
        sc_logger::Debug("Pixel format: BGRA");
        return "bgra";
    case SPA_VIDEO_FORMAT_RGBA:
        sc_logger::Debug("Pixel format: RGBA");
        return "rgba";
    case SPA_VIDEO_FORMAT_BGRx:
        sc_logger::Debug("Pixel format: BGRx");
        return "bgrx";
    case SPA_VIDEO_FORMAT_RGBx:
        sc_logger::Debug("Pixel format: RGBx");
        return "rgbx";
    case SPA_VIDEO_FORMAT_xBGR:
        sc_logger::Debug("Pixel format: xBGR");
        return "xbgr";
    case SPA_VIDEO_FORMAT_xRGB:
        sc_logger::Debug("Pixel format: xRGB");
        return "xrgb";
    case SPA_VIDEO_FORMAT_NV12:
        sc_logger::Debug("Pixel format: NV12");
        return "nv12";
    case SPA_VIDEO_FORMAT_I420:
        sc_logger::Debug("Pixel format: I420");
        return "i420";
    case SPA_VIDEO_FORMAT_YUY2:
        sc_logger::Debug("Pixel format: YUY2");
        return "yuy2";
    case SPA_VIDEO_FORMAT_AYUV:
        sc_logger::Debug("Pixel format: AYUV");
        return "ayuv";
    case SPA_VIDEO_FORMAT_UYVY:
        sc_logger::Debug("Pixel format: UYVY");
        return "uyvy";
    default:
        sc_logger::Warn("Unknown pixel format: {} Falling back to BGRA", pixelFormat);
        return "bgra";
    }
#else
    sc_logger::Info("Windows platform, defaulting to BGRA pixel format");
    return "bgra";
#endif
    }

Napi::Value SerializeSharedHandleLegacy(Napi::Env env, const std::optional<SharedHandleInfo>& shared) {
    if (!shared.has_value()) return env.Null();

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("handle", Napi::BigInt::New(env, shared->handle));
    obj.Set("width", shared->width);
    obj.Set("height", shared->height);
    obj.Set("stride", shared->stride);
    obj.Set("offset", shared->offset);
    obj.Set("planeSize", Napi::BigInt::New(env, shared->planeSize));
    obj.Set("pixelFormat", shared->pixelFormat);
    obj.Set("modifier", Napi::BigInt::New(env, shared->modifier));
    obj.Set("bufferType", shared->bufferType);
    obj.Set("chunkSize", shared->chunkSize);
    return obj;
}

Napi::Value SerializeSharedTextureInfo(Napi::Env env, const std::optional<SharedHandleInfo>& shared) {
    if (!shared.has_value()) return env.Null();

#ifdef __linux__
    if (shared->bufferType != SPA_DATA_DmaBuf) {
        if (shared->bufferType == SPA_DATA_MemFd) {
            sc_logger::Warn("You should use getPixelData for SPA_DATA_MemFd buffer type, as it is not supported in Electron shared textures.");
        }
        return env.Null();
    }
#endif

    Napi::Object obj = Napi::Object::New(env);

    std::string pixelFormat = PixelFormatToString(shared->pixelFormat);

    obj.Set("pixelFormat", Napi::String::New(env, pixelFormat));

    Napi::Object codedSize = Napi::Object::New(env);
    codedSize.Set("width", shared->width);
    codedSize.Set("height", shared->height);
    obj.Set("codedSize", codedSize);

    Napi::Object handle = Napi::Object::New(env);

#ifdef _WIN32
    // Windows expects ntHandle as a Buffer
    HANDLE rawHandle = std::bit_cast<HANDLE>(static_cast<std::uintptr_t>(shared->handle));
    Napi::Buffer<uint8_t> buffer = Napi::Buffer<uint8_t>::Copy(env, reinterpret_cast<uint8_t*>(&rawHandle), sizeof(HANDLE));
    handle.Set("ntHandle", buffer);
#elif defined(__linux__)
    // Linux expects nativePixmap object typically, or fd inside.
    Napi::Object nativePixmap = Napi::Object::New(env);
    Napi::Array planes = Napi::Array::New(env, 1);
    Napi::Object plane = Napi::Object::New(env);
    plane.Set("stride", shared->stride);
    plane.Set("offset", shared->offset);
    plane.Set("size", static_cast<double>(shared->planeSize));
    plane.Set("fd", static_cast<uint32_t>(shared->handle));
    planes.Set(uint32_t(0), plane);
    nativePixmap.Set("planes", planes);
    nativePixmap.Set("modifier", Napi::BigInt::New(env, shared->modifier));
    handle.Set("nativePixmap", nativePixmap);
#endif

    obj.Set("handle", handle);
    return obj;
        }

Napi::Value SerializePixelData(Napi::Env env, const std::optional<std::vector<uint8_t>>& pixels) {
    if (!pixels.has_value()) {
        return env.Null();
    }
    return Napi::Buffer<uint8_t>::Copy(env, pixels->data(), pixels->size());
}
