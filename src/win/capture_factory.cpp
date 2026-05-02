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

    int GetMonitorCount() const override { return static_cast<int>(m_monitors.size()); }
    int GetCurrentMonitorIndex() const override { return m_currentIdx.load(); }

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
    std::unique_ptr<IPlatformCapture> m_zombieCapture;

    std::jthread m_thread;
    std::condition_variable_any m_cv;

    Napi::Env m_env{ nullptr };
    std::function<void()> m_userCallback;
    std::mutex m_callbackMutex;

    void EnumerateMonitors() {
        m_monitors.clear();
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* mons = reinterpret_cast<std::vector<WindowsMonitor>*>(lp);
            MONITORINFOEXW mi = { sizeof(mi) };
            if (GetMonitorInfoW(h, &mi)) mons->push_back({ h, mi.rcMonitor, mi.szDevice });
            return TRUE;
            }, reinterpret_cast<LPARAM>(&m_monitors));
    }

    void RunCaptureLoop(std::stop_token st) {
        while (!st.stop_requested() && m_running) {
            int target = m_requestedIdx.exchange(-1);

            // Wait if we already have an active capture and no new request
            if (target == -1 && m_activeCapture) {
                std::unique_lock lock(m_activeMutex);
                m_cv.wait(lock, st, [this] { return m_requestedIdx.load() != -1; });
                continue;
            }

            // Initial capture or manual switch
            if (target == -1) target = m_currentIdx.load();
            if (target >= (int)m_monitors.size()) continue;

            // 1. Zwiększamy generację przed stworzeniem nowego backendu
            uint64_t newGeneration = m_captureGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

            // 2. Tworzymy i uruchamiamy NOWY backend bez zabijania starego
            auto next = CreateInternalCapture(m_monitors[target].handle);
            if (next) {
                next->SetFrameAvailableCallback([this, newGeneration]() {
                    // Odrzucamy callbacki z poprzednich generacji (starych backendów)
                    if (m_captureGeneration.load(std::memory_order_acquire) != newGeneration) {
                        return;
                    }
                    std::function<void()> cb;
                    { std::lock_guard cbLock(m_callbackMutex); cb = m_userCallback; }
                    if (cb) cb();
                    });

                {
                    std::lock_guard lock(m_activeMutex);
                    m_pendingCapture = std::move(next);
                }
                m_pendingCapture->Start(nullptr);

                // 3. SEAMLESS HANDOFF: Czekamy, aż nowy backend faktycznie ma klatkę (do 3 sekund)
                for (int i = 0; i < 300; ++i) {
                    if (m_pendingCapture->GetWidth() > 0 && m_pendingCapture->GetSharedHandle().has_value()) {
                        break;
                    }
                    if (st.stop_requested()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // 4. Atomowa podmiana - w tym momencie JS zacznie dostawać nowe uchwyty
                std::unique_ptr<IPlatformCapture> oldCapture;
                {
                    std::lock_guard lock(m_activeMutex);
                    oldCapture = std::move(m_activeCapture);
                    m_activeCapture = std::move(m_pendingCapture);
                    m_currentIdx.store(target);
                }

                // 5. Dopiero teraz bezpiecznie zamykamy stary backend
                if (oldCapture) {
                    oldCapture->Stop();
                    // Przenosimy do zombie - zostanie trwale zniszczony DOPIERO przy kolejnej zmianie monitora
                    m_zombieCapture = std::move(oldCapture);
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