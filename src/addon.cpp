#include <napi.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
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

    private:
    std::unique_ptr<IPlatformCapture> m_backend;

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