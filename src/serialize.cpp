#include <string>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __linux__
#include <spa/param/video/format-utils.h>
#endif

#include "serialize.hpp"

static std::string PixelFormatToString(uint32_t pixelFormat) {
#ifdef __linux__
    switch (pixelFormat) {
    case SPA_VIDEO_FORMAT_BGRA:
        std::cerr << "Pixel format: BGRA" << std::endl;
        return "bgra";
    case SPA_VIDEO_FORMAT_RGBA:
        std::cerr << "Pixel format: RGBA" << std::endl;
        return "rgba";
    case SPA_VIDEO_FORMAT_BGRx:
        std::cerr << "Pixel format: BGRx" << std::endl;
        return "bgrx";
    case SPA_VIDEO_FORMAT_RGBx:
        std::cerr << "Pixel format: RGBx" << std::endl;
        return "rgbx";
    case SPA_VIDEO_FORMAT_xBGR:
        std::cerr << "Pixel format: xBGR" << std::endl;
        return "xbgr";
    case SPA_VIDEO_FORMAT_xRGB:
        std::cerr << "Pixel format: xRGB" << std::endl;
        return "xrgb";
    case SPA_VIDEO_FORMAT_NV12:
        std::cerr << "Pixel format: NV12" << std::endl;
        return "nv12";
    case SPA_VIDEO_FORMAT_I420:
        std::cerr << "Pixel format: I420" << std::endl;
        return "i420";
    case SPA_VIDEO_FORMAT_YUY2:
        std::cerr << "Pixel format: YUY2" << std::endl;
        return "yuy2";
    case SPA_VIDEO_FORMAT_AYUV:
        std::cerr << "Pixel format: AYUV" << std::endl;
        return "ayuv";
    case SPA_VIDEO_FORMAT_UYVY:
        std::cerr << "Pixel format: UYVY" << std::endl;
        return "uyvy";
    default:
        std::cerr << "Unknown pixel format: " << pixelFormat << " Falling back to BGRA" << std::endl;
        return "bgra";
    }
#else
    std::cerr << "Windows platform, defaulting to BGRA pixel format" << std::endl;
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

    Napi::Object obj = Napi::Object::New(env);

    obj.Set("pixelFormat", Napi::String::New(env, PixelFormatToString(shared->pixelFormat)));

    Napi::Object codedSize = Napi::Object::New(env);
    codedSize.Set("width", shared->width);
    codedSize.Set("height", shared->height);
    obj.Set("codedSize", codedSize);

    Napi::Object handle = Napi::Object::New(env);

#ifdef _WIN32
    // Windows expects ntHandle as a Buffer
    HANDLE rawHandle = reinterpret_cast<HANDLE>(shared->handle);
    Napi::Buffer<uint8_t> buffer = Napi::Buffer<uint8_t>::Copy(env, reinterpret_cast<uint8_t*>(&rawHandle), sizeof(HANDLE));
    handle.Set("ntHandle", buffer);
#elif defined(__linux__)
    // Linux expects nativePixmap object typically, or fd inside.
    // For now we assume typical fd mapping if used. If needed, can be mapped into nativePixmap...
    Napi::Object nativePixmap = Napi::Object::New(env);
    Napi::Array planes = Napi::Array::New(env, 1);
    Napi::Object plane = Napi::Object::New(env);
    plane.Set("stride", shared->stride);
    plane.Set("offset", shared->offset);
    plane.Set("size", static_cast<double>(shared->planeSize));
    plane.Set("fd", static_cast<uint32_t>(shared->handle));
    planes.Set(uint32_t(0), plane);
    nativePixmap.Set("planes", planes);
    nativePixmap.Set("modifier", Napi::String::New(env, std::to_string(shared->modifier))); // Just stringified modifier for now
    handle.Set("nativePixmap", nativePixmap);
#endif

    obj.Set("handle", handle);
    return obj;
}