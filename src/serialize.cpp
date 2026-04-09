#ifdef _WIN32
#include <windows.h>
#endif

#include "serialize.hpp"

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

    obj.Set("pixelFormat", Napi::String::New(env, "bgra")); // We use B8G8R8A8 on Windows

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