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
    struct FrameCallbackPayload {
        std::string backend;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint32_t pixelFormat = 0;
        std::optional<SharedHandleInfo> sharedHandle;
        std::optional<std::vector<uint8_t>> pixelData;

        ~FrameCallbackPayload() {
            if (sharedHandle && sharedHandle->handle) {
#ifdef _WIN32
                CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(sharedHandle->handle)));
#else
                close(static_cast<int>(sharedHandle->handle));
#endif
            }
        }
    };

    std::unique_ptr<IPlatformCapture> m_backend;
    std::mutex m_callbackMutex;
    Napi::ThreadSafeFunction m_frameCallback;
    bool m_frameCallbackActive = false;
    std::atomic<int> m_framesInFlight{ 0 };

    void AttachFrameCallbackToBackend() {
        std::function<void()> callback;
        IPlatformCapture* backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_frameCallbackActive) {
                callback = [this]() { NotifyFrameAvailable(); };
            }
            backend = m_backend.get();
        }

        if (backend) {
            backend->SetFrameAvailableCallback(std::move(callback));
        }
    }

    void NotifyFrameAvailable() {
        // Backpressure: Tylko 1 klatka w potoku IPC na raz. 
        // Przy 8MB na klatkę, większa ilość zapycha proces główny Electrona.
        if (m_framesInFlight.load(std::memory_order_relaxed) >= 1) {
            return;
        }

        Napi::ThreadSafeFunction tsfn;
        IPlatformCapture* backend = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (!m_frameCallbackActive || !m_backend) {
                return;
            }
            tsfn = m_frameCallback;
            backend = m_backend.get();
        }

        if (!tsfn || !backend) return;

        auto payload = std::make_unique<FrameCallbackPayload>();
        // Pobieramy metadane (szybkie)
        payload->backend = backend->GetBackendName();
        payload->width = backend->GetWidth();
        payload->height = backend->GetHeight();
        payload->stride = backend->GetStride();
        payload->pixelFormat = backend->GetPixelFormat();
        payload->sharedHandle = backend->GetSharedHandle();

        // Optimization: Only fetch slow pixel data if GPU shared handle is not available.
        // This drastically reduces latency on the fast path (Wayland DMA-BUF / WinRT).
        if (!payload->sharedHandle) {
            payload->pixelData = backend->GetPixelData("rgba");
        }

        if (!payload->sharedHandle && !payload->pixelData) return;


        sc_logger::Debug("NotifyFrameAvailable: Dispatching frame to JS (backend: {})", payload->backend);

        m_framesInFlight.fetch_add(1, std::memory_order_relaxed);

        // Przekazujemy payload do TSFN. Od tego momentu TSFN odpowiada za pamięć.
        auto* rawPayload = payload.release();

        auto status = tsfn.NonBlockingCall(rawPayload, [this](Napi::Env env, Napi::Function jsCallback, FrameCallbackPayload* p) {
            m_framesInFlight.fetch_sub(1, std::memory_order_relaxed);
            Napi::Object frame = Napi::Object::New(env);
            frame.Set("backend", Napi::String::New(env, p->backend));
            frame.Set("width", p->width);
            frame.Set("height", p->height);
            frame.Set("stride", p->stride);
            frame.Set("pixelFormat", p->pixelFormat);
            frame.Set("sharedTextureInfo", SerializeSharedTextureInfo(env, p->sharedHandle));
            frame.Set("sharedHandle", SerializeSharedHandleLegacy(env, p->sharedHandle));
            frame.Set("pixelData", SerializePixelData(env, p->pixelData));

            try {
                jsCallback.Call({ frame });
            } catch (const Napi::Error& e) {
                sc_logger::Error("Frame callback threw JS error: {}", e.Message());
            } catch (...) {
                sc_logger::Error("Frame callback threw unknown JS error");
            }
            delete p;
            });

        if (status != napi_ok) {
            sc_logger::Warn("Failed to queue frame callback to JS");
            m_framesInFlight.fetch_sub(1, std::memory_order_relaxed);
            delete rawPayload;
        }
    }

    void ResetFrameCallback() {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_backend) {
            m_backend->SetFrameAvailableCallback(nullptr);
        }
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

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_frameCallbackActive) {
                m_frameCallback.Abort();
                m_frameCallback = {};
            }

            auto callback = info[0].As<Napi::Function>();
            m_frameCallback = Napi::ThreadSafeFunction::New(env, callback, "ScreenCaptureFrameCallback", 0, 1);
            m_frameCallbackActive = true;
        }

        AttachFrameCallbackToBackend();
        sc_logger::Info("ScreenCapture onFrame callback registered");
        return env.Undefined();
    }

    Napi::Value OffFrame(const Napi::CallbackInfo& info) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
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

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_backend) {
                m_backend->Stop();
            }

            m_backend = std::move(nextBackend);
        }

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