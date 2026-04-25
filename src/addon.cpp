#include <napi.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "logger.hpp"
#include "platform_capture.hpp"
#include "serialize.hpp"

class ScreenCapture : public Napi::ObjectWrap<ScreenCapture> {
    public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {

        Napi::Function func = DefineClass(env, "ScreenCapture", {
            InstanceMethod("start", &ScreenCapture::Start),
            InstanceMethod("stop", &ScreenCapture::Stop),
            InstanceMethod("getSharedHandle", &ScreenCapture::GetSharedHandleLegacy),
            InstanceMethod("getSharedTextureInfo", &ScreenCapture::GetSharedTextureInfo),
            InstanceMethod("getPixelData", &ScreenCapture::GetPixelData),
            InstanceMethod("onFrame", &ScreenCapture::OnFrame),
            InstanceMethod("offFrame", &ScreenCapture::OffFrame),
            InstanceMethod("forceBackend", &ScreenCapture::ForceBackend),
            InstanceMethod("getBackend", &ScreenCapture::GetBackend),
            InstanceMethod("getWidth", &ScreenCapture::GetWidth),
            InstanceMethod("getHeight", &ScreenCapture::GetHeight),
            InstanceMethod("getStride", &ScreenCapture::GetStride),
            InstanceMethod("getPixelFormat", &ScreenCapture::GetPixelFormat),
            InstanceMethod("getFps", &ScreenCapture::GetFps)
            });

        auto* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        env.SetInstanceData(constructor);

        exports.Set("ScreenCapture", func);
        return exports;
    }

    explicit ScreenCapture(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<ScreenCapture>(info),
        m_backend(CreatePlatformCapture()) {
        if (info.Length() > 0 && info[0].IsObject()) {
            auto options = info[0].As<Napi::Object>();
            bool disableLogging = false;
            if (options.Has("disableLogging") && options.Get("disableLogging").IsBoolean()) {
                disableLogging = options.Get("disableLogging").As<Napi::Boolean>().Value();
            }

            if (disableLogging) {
                sc_logger::SetLogLevel(sc_logger::LogLevel::None);
            } else if (options.Has("logLevel") && options.Get("logLevel").IsString()) {
                const std::string levelName = options.Get("logLevel").As<Napi::String>().Utf8Value();
                sc_logger::SetLogLevel(sc_logger::ParseLogLevel(levelName));
            }
        }
    }

    ~ScreenCapture() override {
        ResetFrameCallback();
    }

    private:
    std::unique_ptr<IPlatformCapture> m_backend;
    std::mutex m_frameCallbackMutex;
    Napi::ThreadSafeFunction m_frameCallback;
    bool m_frameCallbackActive = false;

    void AttachFrameCallbackToBackend() {
        if (!m_backend) {
            return;
        }

        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
            if (!m_frameCallbackActive) {
                callback = nullptr;
            } else {
                callback = [this]() { NotifyFrameAvailable(); };
            }
        }

        m_backend->SetFrameAvailableCallback(std::move(callback));
    }

    void NotifyFrameAvailable() {
        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
        if (!m_frameCallbackActive) {
            return;
        }

        auto status = m_frameCallback.BlockingCall(this, [](Napi::Env env, Napi::Function jsCallback, ScreenCapture* self) {
            self->InvokeFrameCallback(env, jsCallback);
            });
        if (status != napi_ok) {
            sc_logger::Warn("Failed to queue frame callback to JS");
        }
    }

    void InvokeFrameCallback(Napi::Env env, Napi::Function jsCallback) {
        if (!m_backend) {
            return;
        }

        auto shared = m_backend->GetSharedHandle();
        auto pixels = m_backend->GetPixelData("rgba");

        Napi::Object frame = Napi::Object::New(env);
        frame.Set("backend", Napi::String::New(env, m_backend->GetBackendName()));
        frame.Set("width", m_backend->GetWidth());
        frame.Set("height", m_backend->GetHeight());
        frame.Set("stride", m_backend->GetStride());
        frame.Set("pixelFormat", m_backend->GetPixelFormat());
        frame.Set("sharedTextureInfo", SerializeSharedTextureInfo(env, shared));
        frame.Set("sharedHandle", SerializeSharedHandleLegacy(env, shared));
        frame.Set("pixelData", SerializePixelData(env, pixels));

        jsCallback.Call({ frame });
    }

    void ResetFrameCallback() {
        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
        if (m_frameCallbackActive) {
            m_frameCallback.Abort();
            m_frameCallback = {};
        }
        m_frameCallbackActive = false;
    }

    Napi::Value OnFrame(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined()) {
            return OffFrame(info);
        }
        if (!info[0].IsFunction()) {
            Napi::TypeError::New(env, "onFrame requires a function callback").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
        if (m_frameCallbackActive) {
            m_frameCallback.Abort();
            m_frameCallback = {};
        }

        auto callback = info[0].As<Napi::Function>();
        m_frameCallback = Napi::ThreadSafeFunction::New(env, callback, "ScreenCaptureFrameCallback", 0, 1);
        m_frameCallbackActive = true;
        AttachFrameCallbackToBackend();
        return env.Undefined();
    }

    Napi::Value OffFrame(const Napi::CallbackInfo& info) {
        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
        if (m_frameCallbackActive) {
            m_frameCallback.Abort();
            m_frameCallback = {};
        }
        m_frameCallbackActive = false;
        if (m_backend) {
            m_backend->SetFrameAvailableCallback(nullptr);
        }
        return info.Env().Undefined();
    }

    Napi::Value GetFps(const Napi::CallbackInfo& info) {
        int fps = -1;
        if (m_backend) {
            fps = m_backend->GetFps();
        }
        return Napi::Number::New(info.Env(), fps);
    }

    Napi::Value Start(const Napi::CallbackInfo& info) {
        try {
            m_backend->Start(info.Env());
        } catch (const std::exception& e) {
            Napi::Error::New(info.Env(), e.what()).ThrowAsJavaScriptException();
        }
        return info.Env().Undefined();
    }

    Napi::Value Stop(const Napi::CallbackInfo& info) {
        m_backend->Stop();
        return info.Env().Undefined();
    }

    Napi::Value GetSharedHandleLegacy(const Napi::CallbackInfo& info) {
        auto shared = m_backend->GetSharedHandle();
        return SerializeSharedHandleLegacy(info.Env(), shared);
    }

    Napi::Value GetSharedTextureInfo(const Napi::CallbackInfo& info) {
        auto shared = m_backend->GetSharedHandle();
        return SerializeSharedTextureInfo(info.Env(), shared);
    }

    Napi::Value ForceBackend(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        if (info.Length() == 0 || !info[0].IsString()) {
            Napi::TypeError::New(env, "forceBackend requires a backend name string").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string backend = info[0].As<Napi::String>().Utf8Value();
        std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

#ifndef _WIN32
        Napi::Error::New(env, "forceBackend is only supported on Windows").ThrowAsJavaScriptException();
        return env.Undefined();
#endif

        auto nextBackend = CreatePlatformCapture(backend);
        if (!nextBackend) {
            Napi::Error::New(env, "Requested backend is unavailable or unsupported").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        if (m_backend) {
            m_backend->Stop();
        }

        m_backend = std::move(nextBackend);
        AttachFrameCallbackToBackend();
        return env.Undefined();
    }

    Napi::Value GetPixelData(const Napi::CallbackInfo& info) {
        std::string desiredFormat = "rgba";
        if (info.Length() > 0 && info[0].IsString()) {
            desiredFormat = info[0].As<Napi::String>().Utf8Value();
            std::transform(desiredFormat.begin(), desiredFormat.end(), desiredFormat.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
                });
        }

        auto pixels = m_backend->GetPixelData(desiredFormat);
        return SerializePixelData(info.Env(), pixels);
    }

    Napi::Value GetBackend(const Napi::CallbackInfo& info) {
        std::string backend = "unknown";
        if (m_backend) {
            backend = m_backend->GetBackendName();
        }
        return Napi::String::New(info.Env(), backend);
    }

    Napi::Value GetWidth(const Napi::CallbackInfo& info) {
        int width = 0;
        if (m_backend) {
            width = m_backend->GetWidth();
        }
        return Napi::Number::New(info.Env(), width);
    }

    Napi::Value GetHeight(const Napi::CallbackInfo& info) {
        int height = 0;
        if (m_backend) {
            height = m_backend->GetHeight();
        }
        return Napi::Number::New(info.Env(), height);
    }

    Napi::Value GetStride(const Napi::CallbackInfo& info) {
        int stride = 0;
        if (m_backend) {
            stride = m_backend->GetStride();
        }
        return Napi::Number::New(info.Env(), stride);
    }

    Napi::Value GetPixelFormat(const Napi::CallbackInfo& info) {
        uint32_t pixelFormat = 0;
        if (m_backend) {
            pixelFormat = m_backend->GetPixelFormat();
        }
        return Napi::Number::New(info.Env(), pixelFormat);
    }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return ScreenCapture::Init(env, exports);
}

NODE_API_MODULE(screen_capture_addon, InitAll)