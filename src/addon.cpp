#include <napi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifdef __linux__
#include <spa/buffer/buffer.h>
#endif

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
            InstanceMethod("onMonitorChanged", &ScreenCapture::OnMonitorChanged),
            InstanceMethod("offMonitorChanged", &ScreenCapture::OffMonitorChanged),
            InstanceMethod("onConfigurationChanged", &ScreenCapture::OnConfigurationChanged),
            InstanceMethod("offConfigurationChanged", &ScreenCapture::OffConfigurationChanged),
            InstanceMethod("forceBackend", &ScreenCapture::ForceBackend),
            InstanceMethod("getBackend", &ScreenCapture::GetBackend),
            InstanceMethod("getWidth", &ScreenCapture::GetWidth),
            InstanceMethod("getHeight", &ScreenCapture::GetHeight),
            InstanceMethod("getStride", &ScreenCapture::GetStride),
            InstanceMethod("getPixelFormat", &ScreenCapture::GetPixelFormat),
            InstanceMethod("getFps", &ScreenCapture::GetFps),
            InstanceMethod("getMonitorCount", &ScreenCapture::GetMonitorCount),
            InstanceMethod("getCurrentMonitorIndex", &ScreenCapture::GetCurrentMonitorIndex),
            InstanceMethod("getCurrentMonitor", &ScreenCapture::GetCurrentMonitor),
            InstanceMethod("nextMonitor", &ScreenCapture::NextMonitor),
            InstanceMethod("selectMonitor", &ScreenCapture::SelectMonitor),
            InstanceMethod("getMonitors", &ScreenCapture::GetMonitors),
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
            std::optional<std::string> externalSessionHandle;
            std::optional<int> externalPipewireFd;
            std::optional<std::vector<MonitorMetadata>> portalMonitors;
            if (options.Has("disableLogging") && options.Get("disableLogging").IsBoolean()) {
                disableLogging = options.Get("disableLogging").As<Napi::Boolean>().Value();
            }

            if (disableLogging) {
                sc_logger::SetLogLevel(sc_logger::LogLevel::Off);
            } else if (options.Has("logLevel") && options.Get("logLevel").IsString()) {
                const std::string levelName = options.Get("logLevel").As<Napi::String>().Utf8Value();
                sc_logger::SetLogLevel(sc_logger::ParseLogLevel(levelName));
            }

            if (options.Has("portalSessionHandle") && options.Get("portalSessionHandle").IsString()) {
                externalSessionHandle = options.Get("portalSessionHandle").As<Napi::String>().Utf8Value();
            }
            if (options.Has("pipewireRemoteFd") && options.Get("pipewireRemoteFd").IsNumber()) {
                externalPipewireFd = options.Get("pipewireRemoteFd").As<Napi::Number>().Int32Value();
            }
            if (options.Has("portalMonitors") && options.Get("portalMonitors").IsArray()) {
                Napi::Array monitorsArray = options.Get("portalMonitors").As<Napi::Array>();
                std::vector<MonitorMetadata> parsed;
                parsed.reserve(monitorsArray.Length());

                for (uint32_t i = 0; i < monitorsArray.Length(); ++i) {
                    Napi::Value item = monitorsArray.Get(i);
                    if (!item.IsObject()) {
                        continue;
                    }

                    Napi::Object monitorObj = item.As<Napi::Object>();
                    MonitorMetadata monitor;

                    if (monitorObj.Has("id") && monitorObj.Get("id").IsString()) {
                        monitor.id = monitorObj.Get("id").As<Napi::String>().Utf8Value();
                    }
                    if (monitorObj.Has("name") && monitorObj.Get("name").IsString()) {
                        monitor.name = monitorObj.Get("name").As<Napi::String>().Utf8Value();
                    }
                    if (monitorObj.Has("index") && monitorObj.Get("index").IsNumber()) {
                        monitor.index = monitorObj.Get("index").As<Napi::Number>().Int32Value();
                    }
                    if (monitorObj.Has("x") && monitorObj.Get("x").IsNumber()) {
                        monitor.x = monitorObj.Get("x").As<Napi::Number>().Int32Value();
                    }
                    if (monitorObj.Has("y") && monitorObj.Get("y").IsNumber()) {
                        monitor.y = monitorObj.Get("y").As<Napi::Number>().Int32Value();
                    }
                    if (monitorObj.Has("width") && monitorObj.Get("width").IsNumber()) {
                        monitor.width = monitorObj.Get("width").As<Napi::Number>().Int32Value();
                    }
                    if (monitorObj.Has("height") && monitorObj.Get("height").IsNumber()) {
                        monitor.height = monitorObj.Get("height").As<Napi::Number>().Int32Value();
                    }
                    if (monitorObj.Has("pipewireStream") && monitorObj.Get("pipewireStream").IsNumber()) {
                        const int32_t pipewireStream = monitorObj.Get("pipewireStream").As<Napi::Number>().Int32Value();
                        if (pipewireStream >= 0) {
                            monitor.pipewireStream = static_cast<uint32_t>(pipewireStream);
                        }
                    }

                    parsed.push_back(std::move(monitor));
                }

                if (!parsed.empty()) {
                    portalMonitors = std::move(parsed);
                }
            }
            if (m_backend) {
                m_backend->SetExternalPortalSession(externalSessionHandle, externalPipewireFd, portalMonitors);
            }
        }
    }

    ~ScreenCapture() override {
        if (m_backend) m_backend->Stop();
        ResetCallbacks();
    }

    private:
    struct FrameCallbackPayload {
        std::string backend;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint32_t pixelFormat = 0;
        std::optional<SharedHandleInfo> sharedHandle;
        int64_t timestamp = 0;
        std::optional<std::vector<uint8_t>> pixelData;

        ~FrameCallbackPayload() {
            // Handles are managed by the platform capture backend to ensure stability for Electron/Chromium.
        }
    };

    std::unique_ptr<IPlatformCapture> m_backend;
    std::mutex m_callbackMutex;
    Napi::ThreadSafeFunction m_frameCallback;
    bool m_frameCallbackActive = false;
    Napi::ThreadSafeFunction m_monitorChangedCallback;
    bool m_monitorChangedCallbackActive = false;
    Napi::ThreadSafeFunction m_configurationChangedCallback;
    bool m_configurationChangedCallbackActive = false;
    std::vector<MonitorMetadata> m_previousMonitors;
    bool m_frameDispatchScheduled = false;
    std::unique_ptr<FrameCallbackPayload> m_pendingFrame;

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

    void AttachMonitorChangedCallbackToBackend() {
        std::function<void()> callback;
        IPlatformCapture* backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_monitorChangedCallbackActive) {
                callback = [this]() { NotifyMonitorChanged(); };
            }
            backend = m_backend.get();
        }

        if (backend) {
            backend->SetMonitorChangedCallback(std::move(callback));
        }
    }

    void AttachConfigurationChangedCallbackToBackend() {
        std::function<void(const std::vector<ConfigurationChange>&)> callback;
        IPlatformCapture* backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_configurationChangedCallbackActive) {
                callback = [this](const std::vector<ConfigurationChange>& changes) { NotifyConfigurationChanged(changes); };
            }
            backend = m_backend.get();
        }

        if (backend) {
            backend->SetConfigurationChangedCallback(std::move(callback));
        }
    }

    void NotifyFrameAvailable() {
        Napi::ThreadSafeFunction tsfn;
        IPlatformCapture* backend = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (!m_frameCallbackActive || !m_backend) {
                return;
            }
            if (m_frameDispatchScheduled) {
                // A frame is already queued for dispatch; skip expensive pixel data conversion
                // until the previous frame reaches the JS side.
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
        payload->timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();

        // Optimization: Only fetch slow pixel data if GPU shared handle is not available,
        // or if the shared handle is not usable as an Electron shared texture.
        bool shouldFetchPixelData = !payload->sharedHandle;
#ifdef __linux__
        if (payload->sharedHandle && payload->sharedHandle->bufferType != SPA_DATA_DmaBuf) {
            shouldFetchPixelData = true;
        }
#endif
        if (shouldFetchPixelData) {
            payload->pixelData = backend->GetPixelData("rgba");
        }

        if (!payload->sharedHandle && !payload->pixelData) return;

        bool scheduleDispatch = false;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_pendingFrame = std::move(payload);
            if (!m_frameDispatchScheduled) {
                m_frameDispatchScheduled = true;
                scheduleDispatch = true;
            }
        }

        if (!scheduleDispatch) {
            return;
        }

        auto status = tsfn.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
            DispatchPendingFrame(env, jsCallback);
            });

        if (status != napi_ok) {
            sc_logger::Warn("Failed to queue frame callback to JS");
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_frameDispatchScheduled = false;
        }
    }

    void DispatchPendingFrame(Napi::Env env, Napi::Function jsCallback) {
        std::unique_ptr<FrameCallbackPayload> payload;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            payload = std::move(m_pendingFrame);
        }

        if (!payload) {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_frameDispatchScheduled = false;
            return;
        }

        Napi::Object frame = Napi::Object::New(env);
        frame.Set("backend", Napi::String::New(env, payload->backend));
        frame.Set("width", payload->width);
        frame.Set("height", payload->height);
        frame.Set("stride", payload->stride);
        frame.Set("pixelFormat", payload->pixelFormat);
        frame.Set("timestamp", Napi::Number::New(env, static_cast<double>(payload->timestamp)));
        frame.Set("sharedTextureInfo", SerializeSharedTextureInfo(env, payload->sharedHandle));
        frame.Set("sharedHandle", SerializeSharedHandleLegacy(env, payload->sharedHandle));
        frame.Set("pixelData", SerializePixelData(env, payload->pixelData));

        try {
            jsCallback.Call({ frame });
        } catch (const Napi::Error& e) {
            sc_logger::Error("Frame callback threw JS error: {}", e.Message());
        } catch (...) {
            sc_logger::Error("Frame callback threw unknown JS error");
        }

        bool needReschedule = false;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            needReschedule = static_cast<bool>(m_pendingFrame);
            if (!needReschedule) {
                m_frameDispatchScheduled = false;
            }
        }

        if (needReschedule) {
            Napi::ThreadSafeFunction tsfn;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                if (!m_frameCallbackActive) {
                    m_frameDispatchScheduled = false;
                    return;
                }
                tsfn = m_frameCallback;
            }

            if (!tsfn) {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                m_frameDispatchScheduled = false;
                return;
            }

            auto status = tsfn.NonBlockingCall([this](Napi::Env env, Napi::Function jsCallback) {
                DispatchPendingFrame(env, jsCallback);
                });

            if (status != napi_ok) {
                sc_logger::Warn("Failed to queue frame callback to JS");
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                m_frameDispatchScheduled = false;
            }
        }
    }

    void NotifyMonitorChanged() {
        Napi::ThreadSafeFunction tsfn;
        std::optional<MonitorMetadata> monitorInfo;
        std::string backendName = "unknown";

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (!m_monitorChangedCallbackActive || !m_backend) {
                return;
            }
            tsfn = m_monitorChangedCallback;
            monitorInfo = m_backend->GetCurrentMonitorInfo();
            backendName = m_backend->GetBackendName();
        }

        if (!tsfn) {
            return;
        }

        if (!monitorInfo.has_value()) {
            return;
        }

        auto status = tsfn.NonBlockingCall([monitorInfo = std::move(monitorInfo), backendName = std::move(backendName)](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object monitor = Napi::Object::New(env);
            monitor.Set("backend", Napi::String::New(env, backendName));
            monitor.Set("id", Napi::String::New(env, monitorInfo->id));
            monitor.Set("name", Napi::String::New(env, monitorInfo->name));
            monitor.Set("index", monitorInfo->index);
            monitor.Set("x", monitorInfo->x);
            monitor.Set("y", monitorInfo->y);
            monitor.Set("width", monitorInfo->width);
            monitor.Set("height", monitorInfo->height);
            if (monitorInfo->pipewireStream.has_value()) {
                monitor.Set("pipewireStream", monitorInfo->pipewireStream.value());
            }

            try {
                jsCallback.Call({ monitor });
            } catch (const Napi::Error& e) {
                sc_logger::Error("Monitor callback threw JS error: {}", e.Message());
            } catch (...) {
                sc_logger::Error("Monitor callback threw unknown JS error");
            }
            });

        if (status != napi_ok) {
            sc_logger::Warn("Failed to queue monitor callback to JS");
        }
    }

    void NotifyConfigurationChanged(const std::vector<ConfigurationChange>& changes) {
        Napi::ThreadSafeFunction tsfn;
        std::vector<MonitorMetadata> currentMonitors;
        std::string backendName = "unknown";

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (!m_configurationChangedCallbackActive || !m_backend) {
                return;
            }
            tsfn = m_configurationChangedCallback;
            currentMonitors = m_backend->GetMonitors();
            backendName = m_backend->GetBackendName();
        }

        if (!tsfn || changes.empty()) {
            return;
        }

        auto status = tsfn.NonBlockingCall([changes, currentMonitors = std::move(currentMonitors), backendName = std::move(backendName)](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object update = Napi::Object::New(env);
            update.Set("backend", Napi::String::New(env, backendName));

            // Marshal configuration changes
            Napi::Array changesArray = Napi::Array::New(env, changes.size());
            for (size_t i = 0; i < changes.size(); ++i) {
                const auto& change = changes[i];
                Napi::Object changeObj = Napi::Object::New(env);

                const char* typeStr = "unknown";
                switch (change.type) {
                    case ConfigurationChangeType::Added:
                        typeStr = "added";
                        break;
                    case ConfigurationChangeType::Removed:
                        typeStr = "removed";
                        break;
                    case ConfigurationChangeType::Enabled:
                        typeStr = "enabled";
                        break;
                    case ConfigurationChangeType::Disabled:
                        typeStr = "disabled";
                        break;
                }

                changeObj.Set("type", Napi::String::New(env, typeStr));
                changeObj.Set("monitorId", Napi::String::New(env, change.monitorId));

                if (change.previousIndex.has_value()) {
                    changeObj.Set("previousIndex", change.previousIndex.value());
                }
                if (change.currentIndex.has_value()) {
                    changeObj.Set("currentIndex", change.currentIndex.value());
                }

                changesArray.Set(i, changeObj);
            }

            update.Set("changes", changesArray);

            // Marshal monitor list
            Napi::Array monitorsArray = Napi::Array::New(env, currentMonitors.size());
            for (size_t i = 0; i < currentMonitors.size(); ++i) {
                const auto& monitor = currentMonitors[i];
                Napi::Object monitorObj = Napi::Object::New(env);

                monitorObj.Set("id", Napi::String::New(env, monitor.id));
                monitorObj.Set("name", Napi::String::New(env, monitor.name));
                monitorObj.Set("index", monitor.index);
                monitorObj.Set("x", monitor.x);
                monitorObj.Set("y", monitor.y);
                monitorObj.Set("width", monitor.width);
                monitorObj.Set("height", monitor.height);
                monitorObj.Set("enabled", monitor.enabled);

                if (monitor.pipewireStream.has_value()) {
                    monitorObj.Set("pipewireStream", monitor.pipewireStream.value());
                }

                monitorsArray.Set(i, monitorObj);
            }

            update.Set("monitors", monitorsArray);

            try {
                jsCallback.Call({ update });
            } catch (const Napi::Error& e) {
                sc_logger::Error("Configuration changed callback threw JS error: {}", e.Message());
            } catch (...) {
                sc_logger::Error("Configuration changed callback threw unknown JS error");
            }
            });

        if (status != napi_ok) {
            sc_logger::Warn("Failed to queue configuration changed callback to JS");
        }
    }

    void ResetCallbacks() {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_backend) {
            m_backend->SetFrameAvailableCallback(nullptr);
            m_backend->SetMonitorChangedCallback(nullptr);
            m_backend->SetConfigurationChangedCallback(nullptr);
        }
        if (m_frameCallbackActive) {
            m_frameCallback.Abort();
            m_frameCallback = {};
        }
        if (m_monitorChangedCallbackActive) {
            m_monitorChangedCallback.Abort();
            m_monitorChangedCallback = {};
        }
        if (m_configurationChangedCallbackActive) {
            m_configurationChangedCallback.Abort();
            m_configurationChangedCallback = {};
        }
        m_frameCallbackActive = false;
        m_monitorChangedCallbackActive = false;
        m_configurationChangedCallbackActive = false;
        m_previousMonitors.clear();
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

    Napi::Value OnMonitorChanged(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined()) {
            return OffMonitorChanged(info);
        }
        if (!info[0].IsFunction()) {
            Napi::TypeError::New(env, "onMonitorChanged requires a function callback").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_monitorChangedCallbackActive) {
                m_monitorChangedCallback.Abort();
                m_monitorChangedCallback = {};
            }

            auto callback = info[0].As<Napi::Function>();
            m_monitorChangedCallback = Napi::ThreadSafeFunction::New(env, callback, "ScreenCaptureMonitorChangedCallback", 0, 1);
            m_monitorChangedCallbackActive = true;
        }

        AttachMonitorChangedCallbackToBackend();
        NotifyMonitorChanged();
        sc_logger::Info("ScreenCapture onMonitorChanged callback registered");
        return env.Undefined();
    }

    Napi::Value OffMonitorChanged(const Napi::CallbackInfo& info) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_monitorChangedCallbackActive) {
            m_monitorChangedCallback.Abort();
            m_monitorChangedCallback = {};
        }
        m_monitorChangedCallbackActive = false;
        if (m_backend) {
            m_backend->SetMonitorChangedCallback(nullptr);
        }
        return info.Env().Undefined();
    }

    Napi::Value OnConfigurationChanged(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined()) {
            return OffConfigurationChanged(info);
        }
        if (!info[0].IsFunction()) {
            Napi::TypeError::New(env, "onConfigurationChanged requires a function callback").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_configurationChangedCallbackActive) {
                m_configurationChangedCallback.Abort();
                m_configurationChangedCallback = {};
            }

            auto callback = info[0].As<Napi::Function>();
            m_configurationChangedCallback = Napi::ThreadSafeFunction::New(env, callback, "ScreenCaptureConfigurationChangedCallback", 0, 1);
            m_configurationChangedCallbackActive = true;
            // Store current monitor state for delta computation
            if (m_backend) {
                m_previousMonitors = m_backend->GetMonitors();
            }
        }

        AttachConfigurationChangedCallbackToBackend();
        sc_logger::Info("ScreenCapture onConfigurationChanged callback registered");
        return env.Undefined();
    }

    Napi::Value OffConfigurationChanged(const Napi::CallbackInfo& info) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_configurationChangedCallbackActive) {
            m_configurationChangedCallback.Abort();
            m_configurationChangedCallback = {};
        }
        m_configurationChangedCallbackActive = false;
        m_previousMonitors.clear();
        if (m_backend) {
            m_backend->SetConfigurationChangedCallback(nullptr);
        }
        return info.Env().Undefined();
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

    Napi::Value GetMonitorCount(const Napi::CallbackInfo& info) {
        int count = 0;
        if (m_backend) {
            count = m_backend->GetMonitorCount();
        }
        return Napi::Number::New(info.Env(), count);
    }

    Napi::Value GetCurrentMonitorIndex(const Napi::CallbackInfo& info) {
        int index = 0;
        if (m_backend) {
            index = m_backend->GetCurrentMonitorIndex();
        }
        return Napi::Number::New(info.Env(), index);
    }

    Napi::Value GetCurrentMonitor(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!m_backend) return env.Null();

        auto monitorInfo = m_backend->GetCurrentMonitorInfo();
        if (!monitorInfo.has_value()) return env.Null();

        Napi::Object obj = Napi::Object::New(env);
        obj.Set("id", monitorInfo->id);
        obj.Set("name", monitorInfo->name);
        obj.Set("index", monitorInfo->index);
        obj.Set("x", monitorInfo->x);
        obj.Set("y", monitorInfo->y);
        obj.Set("width", monitorInfo->width);
        obj.Set("height", monitorInfo->height);
        obj.Set("enabled", monitorInfo->enabled);
        if (monitorInfo->pipewireStream.has_value()) {
            obj.Set("pipewireStream", monitorInfo->pipewireStream.value());
        }

        return obj;
    }

    Napi::Value NextMonitor(const Napi::CallbackInfo& info) {
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_pendingFrame.reset();
        }
        if (m_backend) {
            m_backend->NextMonitor();
        }
        return info.Env().Undefined();
    }

    Napi::Value SelectMonitor(const Napi::CallbackInfo& info) {
        if (info.Length() == 0 || !info[0].IsNumber()) {
            Napi::TypeError::New(info.Env(), "selectMonitor requires an index number").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_pendingFrame.reset();
        }

        int index = info[0].As<Napi::Number>().Int32Value();
        if (m_backend) {
            m_backend->SelectMonitor(index);
        }
        return info.Env().Undefined();
    }

    Napi::Value Start(const Napi::CallbackInfo& info) {
        try {
            m_backend->Start(info.Env());
            NotifyMonitorChanged();
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
            m_pendingFrame.reset();

            m_backend = std::move(nextBackend);
        }

        AttachFrameCallbackToBackend();
        AttachMonitorChangedCallbackToBackend();
        NotifyMonitorChanged();

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

    Napi::Value GetMonitors(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!m_backend) return env.Null();
    
        auto monitors = m_backend->GetMonitors();
        Napi::Array result = Napi::Array::New(env, monitors.size());
    
        for (uint32_t i = 0; i < monitors.size(); ++i) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("id", monitors[i].id);
            obj.Set("name", monitors[i].name);
            obj.Set("index", monitors[i].index);
            obj.Set("x", monitors[i].x);
            obj.Set("y", monitors[i].y);
            obj.Set("width", monitors[i].width);
            obj.Set("height", monitors[i].height);
            obj.Set("enabled", monitors[i].enabled);
            if (monitors[i].pipewireStream.has_value()) {
                obj.Set("pipewireStream", monitors[i].pipewireStream.value());
            }
            result.Set(i, obj);
        }
    
        return result;
    }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return ScreenCapture::Init(env, exports);
}

NODE_API_MODULE(screen_capture_addon, InitAll)