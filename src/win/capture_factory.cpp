#ifdef _WIN32
#include "../logger.hpp"
#include "win_capture_internal.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <windows.h>
#include <stdio.h>
#include <vector>

#if HAS_WINRT_CAPTURE
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Metadata.h>
#endif

// Useful for debugging the 'force_api' variable from prebuildify/node-gyp
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#pragma message(">>> GYP 'force_api' VARIABLE VALUE: " STR(GYP_FORCE_API) " <<<")

// Logging the selected flag during compilation (visible in MSVC)
#ifdef FORCE_API_GDI
#pragma message(">>> COMPILING WITH FORCED API: GDI <<<")
#elif defined(FORCE_API_DXGI)
#pragma message(">>> COMPILING WITH FORCED API: DXGI <<<")
#elif defined(FORCE_API_WINRT)
#pragma message(">>> COMPILING WITH FORCED API: WINRT <<<")
#else
#pragma message(">>> COMPILING WITH API: AUTO (RUNTIME SELECTION) <<<")
#endif

bool IsWinRTCaptureAvailable() {
#if HAS_WINRT_CAPTURE
    // Optional safeguard: ensure the core Graphics library is actually present in system
    HMODULE mod = LoadLibraryW(L"Windows.Graphics.dll");
    if (!mod) {
        sc_logger::Warn("Windows.Graphics.dll not found on the system!");
        return false;
    }
    FreeLibrary(mod);

    try {
        // C++/WinRT requires COM to be initialized for ApiInformation::IsTypePresent to not throw
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool isInitializedByUs = SUCCEEDED(hr);

        bool isPresent = winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession"
        );

        if (isInitializedByUs) {
            CoUninitialize();
        }

        return isPresent;
    } catch (const winrt::hresult_error& e) {
        sc_logger::Warn("WinRT Exception during IsTypePresent check: {}", winrt::to_string(e.message()));
        return false;
    } catch (...) {
        sc_logger::Warn("Unknown C++ exception during IsTypePresent check.");
        return false;
    }
#else
    return false;
#endif
}

struct WindowsMonitor {
    HMONITOR handle;
    RECT area;
    std::wstring deviceName;
};

static std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string();
    }
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), len, nullptr, nullptr);
    out.pop_back();
    return out;
}

class WindowsPlatformCapture final : public IPlatformCapture {
    public:
    WindowsPlatformCapture(const std::string& forceBackend) : m_forcedBackend(forceBackend) {
        EnumerateMonitors();
        sc_logger::Info("WindowsPlatformCapture: initialized with {} monitor(s)", m_monitors.size());
    }

    ~WindowsPlatformCapture() override {
        Stop();
    }

    void Start(Napi::Env env) override {
        if (m_running) return;
        m_env = env;
        m_running = true;
        // Initialize previous monitors list for delta detection
        m_previousMonitors = GetMonitors();
        m_thread = std::jthread([this](std::stop_token st) { RunCaptureLoop(st); });
    }

    void Stop() override {
        m_running = false;
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();

        std::unique_ptr<IPlatformCapture> active;
        {
            std::lock_guard lock(m_activeMutex);
            active = std::move(m_activeCapture);
        }
        if (active) active->Stop();
    }

    // Proxy methods to active backend
    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetSharedHandle();
        return m_activeCapture ? m_activeCapture->GetSharedHandle() : std::nullopt;
    }

    std::optional<std::vector<uint8_t>> GetPixelData(std::string_view desiredFormat) const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetPixelData(desiredFormat);
        return m_activeCapture ? m_activeCapture->GetPixelData(desiredFormat) : std::nullopt;
    }

    int GetWidth() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetWidth();
        return m_activeCapture ? m_activeCapture->GetWidth() : 0;
    }
    int GetHeight() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetHeight() > 0) return m_pendingCapture->GetHeight();
        return m_activeCapture ? m_activeCapture->GetHeight() : 0;
    }
    int GetStride() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetStride();
        return m_activeCapture ? m_activeCapture->GetStride() : 0;
    }
    uint32_t GetPixelFormat() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetPixelFormat();
        return m_activeCapture ? m_activeCapture->GetPixelFormat() : 0;
    }
    std::string GetBackendName() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetBackendName();
        return m_activeCapture ? m_activeCapture->GetBackendName() : "windows-wrapper";
    }
    int GetFps() const override {
        std::lock_guard lock(m_activeMutex);
        if (m_pendingCapture && m_pendingCapture->GetWidth() > 0) return m_pendingCapture->GetFps();
        return m_activeCapture ? m_activeCapture->GetFps() : 0;
    }

    void SetFrameAvailableCallback(std::function<void()> callback) override {
        std::lock_guard lock(m_callbackMutex);
        m_userCallback = std::move(callback);
    }

    void SetMonitorChangedCallback(std::function<void()> callback) override {
        std::lock_guard lock(m_monitorCallbackMutex);
        m_monitorChangedCallback = std::move(callback);
    }

    void SetConfigurationChangedCallback(std::function<void(const std::vector<ConfigurationChange>&)> callback) override {
        std::lock_guard lock(m_configCallbackMutex);
        m_configurationChangedCallback = std::move(callback);
    }

    int GetMonitorCount() const override { return static_cast<int>(m_monitors.size()); }
    int GetCurrentMonitorIndex() const override { return m_currentIdx.load(); }
    std::optional<MonitorMetadata> GetCurrentMonitorInfo() const override {
        const int index = m_currentIdx.load();
        if (index < 0 || index >= static_cast<int>(m_monitors.size())) {
            return std::nullopt;
        }
        return WinMonitorToMetadata(m_monitors[static_cast<size_t>(index)], index);
    }

    std::vector<MonitorMetadata> GetMonitors() const override {
        std::vector<MonitorMetadata> result;
        result.reserve(m_monitors.size());
        for (size_t i = 0; i < m_monitors.size(); ++i) {
            result.push_back(WinMonitorToMetadata(m_monitors[i], static_cast<int>(i)));
        }
        return result;
    }

    void NextMonitor() override {
        int count = GetMonitorCount();
        if (count <= 1) return;
        SelectMonitor((m_currentIdx.load() + 1) % count);
    }

    void SelectMonitor(int index) override {
        if (index >= 0 && index < GetMonitorCount()) {
            sc_logger::Info("WindowsPlatformCapture: switching to monitor index {}", index);
            m_requestedIdx.store(index);
            m_cv.notify_all();
        }
    }

    private:
    std::string m_forcedBackend;
    std::vector<WindowsMonitor> m_monitors;
    std::atomic<int> m_currentIdx{ 0 };
    std::atomic<int> m_requestedIdx{ -1 };
    std::atomic<uint64_t> m_captureGeneration{ 0 };
    std::atomic<bool> m_running{ false };

    mutable std::mutex m_activeMutex;
    std::unique_ptr<IPlatformCapture> m_activeCapture;
    std::unique_ptr<IPlatformCapture> m_pendingCapture;

    struct ZombieCapture {
        std::unique_ptr<IPlatformCapture> capture;
        std::chrono::steady_clock::time_point retiredAt;
    };
    std::vector<ZombieCapture> m_zombieCaptures;
    std::mutex m_zombieMutex;

    std::jthread m_thread;
    std::condition_variable_any m_cv;

    Napi::Env m_env{ nullptr };
    std::function<void()> m_userCallback;
    std::mutex m_callbackMutex;
    std::function<void()> m_monitorChangedCallback;
    std::mutex m_monitorCallbackMutex;
    std::function<void(const std::vector<ConfigurationChange>&)> m_configurationChangedCallback;
    std::mutex m_configCallbackMutex;
    std::vector<MonitorMetadata> m_previousMonitors;

    MonitorMetadata WinMonitorToMetadata(const WindowsMonitor& mon, int index) const {
        MonitorMetadata info;
        info.id = std::to_string(reinterpret_cast<std::uintptr_t>(mon.handle));
        info.name = WideToUtf8(mon.deviceName);
        info.index = index;
        info.x = mon.area.left;
        info.y = mon.area.top;
        info.width = mon.area.right - mon.area.left;
        info.height = mon.area.bottom - mon.area.top;
        // pipewireStream is std::nullopt by default, which is correct for Windows.
        return info;
    }

    void InvokeMonitorChangedCallback() {
        std::function<void()> cb;
        {
            std::lock_guard lock(m_monitorCallbackMutex);
            cb = m_monitorChangedCallback;
        }
        if (cb) {
            cb();
        }
    }

    void CheckAndInvokeConfigurationChanged() {
        auto currentMonitors = GetMonitors();
        std::vector<ConfigurationChange> changes;

        // Check for added/removed monitors
        for (const auto& current : currentMonitors) {
            auto it = std::find_if(m_previousMonitors.begin(), m_previousMonitors.end(),
                [&current](const MonitorMetadata& prev) {
                    return prev.id == current.id;
                });

            if (it == m_previousMonitors.end()) {
                // Monitor added
                changes.emplace_back(ConfigurationChange{
                    ConfigurationChangeType::Added,
                    current.id,
                    std::nullopt,
                    current.index
                });
            }
        }

        for (const auto& prev : m_previousMonitors) {
            auto it = std::find_if(currentMonitors.begin(), currentMonitors.end(),
                [&prev](const MonitorMetadata& current) {
                    return current.id == prev.id;
                });

            if (it == currentMonitors.end()) {
                // Monitor removed
                changes.emplace_back(ConfigurationChange{
                    ConfigurationChangeType::Removed,
                    prev.id,
                    prev.index,
                    std::nullopt
                });
            }
        }

        if (!changes.empty()) {
            std::function<void(const std::vector<ConfigurationChange>&)> cb;
            {
                std::lock_guard lock(m_configCallbackMutex);
                cb = m_configurationChangedCallback;
            }
            if (cb) {
                cb(changes);
            }
            m_previousMonitors = currentMonitors;
        }
    }

    void EnumerateMonitors() {
        m_monitors.clear();
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* mons = reinterpret_cast<std::vector<WindowsMonitor>*>(lp);
            MONITORINFOEXW mi = { sizeof(mi) };
            if (GetMonitorInfoW(h, &mi)) mons->push_back({ h, mi.rcMonitor, mi.szDevice });
            return TRUE;
            }, reinterpret_cast<LPARAM>(&m_monitors));
    }

    void CleanupZombieCaptures() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_zombieMutex);
        m_zombieCaptures.erase(
            std::remove_if(m_zombieCaptures.begin(), m_zombieCaptures.end(),
                [now](const ZombieCapture& z) {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - z.retiredAt).count() > 2000;
                }),
            m_zombieCaptures.end());
    }

    void RunCaptureLoop(std::stop_token st) {
        while (!st.stop_requested() && m_running) {
            CleanupZombieCaptures();
            int target = m_requestedIdx.exchange(-1);

            if (target == -1 && m_activeCapture) {
                std::unique_lock lock(m_activeMutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(500),
                    [this] { return m_requestedIdx.load() != -1; });
                continue;
            }

            if (target == -1) target = m_currentIdx.load();
            if (target >= (int)m_monitors.size()) continue;

            uint64_t newGeneration = m_captureGeneration.fetch_add(1) + 1;

            auto nextCapture = CreateInternalCapture(m_monitors[target].handle);
            if (nextCapture) {
                nextCapture->SetFrameAvailableCallback([this, newGeneration]() {
                    if (m_captureGeneration.load(std::memory_order_acquire) != newGeneration) {
                        return;
                    }
                    std::function<void()> cb;
                    { std::lock_guard cbLock(m_callbackMutex); cb = m_userCallback; }
                    if (cb) cb();
                    });

                {
                    std::lock_guard lock(m_activeMutex);
                    m_pendingCapture = std::move(nextCapture);
                }
                m_pendingCapture->Start(nullptr);

                bool realFrameReceived = false;
                for (int i = 0; i < 300 && !st.stop_requested(); ++i) {
                    // Note: DXGI now returns nullopt handle until first real frame
                    if (m_pendingCapture->GetWidth() > 0 && m_pendingCapture->GetSharedHandle().has_value()) {
                        realFrameReceived = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                if (!realFrameReceived && !st.stop_requested()) {
                    sc_logger::Warn("WindowsPlatformCapture: no real frame within 3s – forcing swap with current texture");
                }

                std::unique_ptr<IPlatformCapture> oldCapture;
                {
                    std::lock_guard lock(m_activeMutex);
                    oldCapture = std::move(m_activeCapture);
                    m_activeCapture = std::move(m_pendingCapture);
                    m_currentIdx.store(target);
                }

                InvokeMonitorChangedCallback();

                if (oldCapture) {
                    oldCapture->Stop();
                    std::lock_guard lock(m_zombieMutex);
                    m_zombieCaptures.push_back({ std::move(oldCapture), std::chrono::steady_clock::now() });
                }
            }
        }
    }

    std::unique_ptr<IPlatformCapture> CreateInternalCapture(HMONITOR mon);
};

static std::string NormalizeBackendName(std::string backend) {
    std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
    return backend;
}

std::unique_ptr<IPlatformCapture> WindowsPlatformCapture::CreateInternalCapture(HMONITOR mon) {
    const std::string backend = NormalizeBackendName(m_forcedBackend);

#ifdef FORCE_API_GDI
    return CreateGDICapture(mon);
#elif defined(FORCE_API_DXGI)
    return CreateDXGICapture(mon);
#elif defined(FORCE_API_WINRT)
#if HAS_WINRT_CAPTURE
    return CreateWinRTCapture(mon);
#else
    return nullptr;
#endif
#else
    if (!backend.empty()) {
        if (backend == "winrt") {
#if HAS_WINRT_CAPTURE
            return CreateWinRTCapture(mon);
#else
            return nullptr;
#endif
        }
        if (backend == "dxgi") return CreateDXGICapture(mon);
        if (backend == "gdi") return CreateGDICapture(mon);
        return nullptr;
    }

#if HAS_WINRT_CAPTURE
    if (IsWinRTCaptureAvailable()) {
        if (auto cap = CreateWinRTCapture(mon)) return cap;
    }
#endif
    if (auto cap = CreateDXGICapture(mon)) return cap;
    return CreateGDICapture(mon);
#endif
}

std::unique_ptr<IPlatformCapture> CreatePlatformCapture(const std::string& forceBackend) {
    return std::make_unique<WindowsPlatformCapture>(forceBackend);
}

#endif