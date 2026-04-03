#include <napi.h>

#include <exception>
#include <memory>

#include "platform_capture.hpp"

class ScreenCapture : public Napi::ObjectWrap<ScreenCapture> {
    public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "ScreenCapture", {
            InstanceMethod("start", &ScreenCapture::Start),
            InstanceMethod("stop", &ScreenCapture::Stop),
            InstanceMethod("getSharedHandle", &ScreenCapture::GetSharedHandle)
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
    }

    private:
    std::unique_ptr<IPlatformCapture> m_backend;

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

    Napi::Value GetSharedHandle(const Napi::CallbackInfo& info) {
        auto shared = m_backend->GetSharedHandle();
        if (!shared.has_value()) return info.Env().Null();

        Napi::Object obj = Napi::Object::New(info.Env());
        obj.Set("handle", Napi::BigInt::New(info.Env(), shared->handle));
        obj.Set("width", shared->width);
        obj.Set("height", shared->height);
        return obj;
    }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return ScreenCapture::Init(env, exports);
}

NODE_API_MODULE(screen_capture_addon, InitAll)