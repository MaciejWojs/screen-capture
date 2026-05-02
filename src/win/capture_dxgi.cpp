#ifdef _WIN32
#include "../logger.hpp"
#include "win_capture_internal.hpp"

#include <atomic>
#include <bit>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class DXGIPlatformCapture final : public IPlatformCapture {
    public:
    DXGIPlatformCapture(HMONITOR monitor) : m_targetMonitor(monitor) {}
    ~DXGIPlatformCapture() override { Stop(); }

    static void CleanupHook(void* arg) {
        if (arg) static_cast<DXGIPlatformCapture*>(arg)->Stop();
    }

    void Start(Napi::Env env) override {
        if (m_thread.joinable()) return;
        m_env = env;
        sc_logger::Info("Screen capture started via DXGI Desktop Duplication API with jthread");

        if (m_env) {
            napi_add_env_cleanup_hook(m_env, CleanupHook, this);
        }

        m_thread = std::jthread([this](std::stop_token stopToken) {
            sc_logger::Info("DXGI capture thread started");
            if (!InitializeDirect3D()) {
                sc_logger::Error("DXGI: Initialization failed");
                return;
            }

            while (!stopToken.stop_requested()) {
                if (!CaptureFrame(stopToken)) {
                    if (stopToken.stop_requested()) break;
                    sc_logger::Warn("DXGI: Capture session interrupted, reinitializing...");
                    CleanupDirect3D();

                    while (!stopToken.stop_requested()) {
                        if (InitializeDirect3D()) {
                            sc_logger::Info("DXGI: Reinitialized successfully");
                            break;
                        }
                        std::unique_lock lock(m_reinitMutex);
                        m_reinitCv.wait_for(lock, std::chrono::milliseconds(250),
                            [&stopToken] { return stopToken.stop_requested(); });
                    }
                }
            }

            CleanupDirect3D();
            sc_logger::Info("DXGI capture thread stopped");
            });
    }

    void Stop() override {
        if (m_env && m_thread.joinable()) {
            napi_remove_env_cleanup_hook(m_env, CleanupHook, this);
            m_env = nullptr;
        }

        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_reinitCv.notify_all();
            m_thread.join();
        }
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        // Nie zwracaj uchwytu, jeśli nie mamy jeszcze ani jednej rzeczywistej klatki
        if (!m_firstFrameCaptured.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        HANDLE handle = m_sharedHandle.load(std::memory_order_acquire);
        uint32_t w = m_width.load(std::memory_order_relaxed);
        uint32_t h = m_height.load(std::memory_order_relaxed);

        if (!handle || w == 0 || h == 0) return std::nullopt;

        SharedHandleInfo info;
        info.handle = static_cast<uint64_t>(std::bit_cast<std::uintptr_t>(handle));
        info.width = w;
        info.height = h;
        info.stride = w * 4;
        info.pixelFormat = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);

        sc_logger::Debug("DXGI GetSharedHandle: handle={}", reinterpret_cast<void*>(handle));
        return info;
    }

    int GetWidth() const override { return static_cast<int>(m_width.load(std::memory_order_relaxed)); }
    int GetHeight() const override { return static_cast<int>(m_height.load(std::memory_order_relaxed)); }
    int GetStride() const override { return static_cast<int>(m_width.load(std::memory_order_relaxed) * 4); }
    uint32_t GetPixelFormat() const override { return static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM); }
    std::string GetBackendName() const override { return "dxgi"; }
    int GetFps() const override { return m_lastFps.load(std::memory_order_relaxed); }

    void SetFrameAvailableCallback(std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
        m_frameAvailableCallback = std::move(callback);
    }

    void InvokeFrameAvailableCallback() {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
            callback = m_frameAvailableCallback;
        }
        if (callback) {
            callback();
        }
    }

    private:
    napi_env m_env{ nullptr };
    std::jthread m_thread;                      // automatyczne zarządzanie wątkiem
    mutable std::mutex m_reinitMutex;           // dla condition_variable przy ponownej inicjalizacji
    HMONITOR m_targetMonitor = nullptr;
    std::condition_variable_any m_reinitCv;     // może czekać na stop_token

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTex[2];
    HANDLE m_sharedHandleInternal[2]{ nullptr, nullptr };
    Microsoft::WRL::ComPtr<ID3D11Query> m_query[2];
    std::atomic<HANDLE> m_sharedHandle{ nullptr };
    int m_currentIndex = 0;

    std::function<void()> m_frameAvailableCallback;
    std::mutex m_frameCallbackMutex;

    std::atomic<uint32_t> m_width{ 0 };
    std::atomic<uint32_t> m_height{ 0 };
    uint32_t m_targetWidth = 0;
    uint32_t m_targetHeight = 0;

    std::atomic<bool> m_firstFrameCaptured{ false };
    std::atomic<uint64_t> m_frameCount{ 0 };
    std::atomic<int> m_lastFps{ 0 };
    std::chrono::steady_clock::time_point m_lastFpsTime = std::chrono::steady_clock::now();

    Microsoft::WRL::ComPtr<IDXGIOutput1> FindTargetOutput(IDXGIAdapter* adapter) {
        for (UINT i = 0; ; ++i) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);

            if (!m_targetMonitor || desc.Monitor == m_targetMonitor) {
                Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
                if (SUCCEEDED(output.As(&output1))) return output1;
            }
        }
        return nullptr;
    }

    bool InitializeDirect3D() {
        CleanupDirect3D();

        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter> targetAdapter;
        Microsoft::WRL::ComPtr<IDXGIOutput1> targetOutput;

        for (UINT i = 0; ; ++i) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

            targetOutput = FindTargetOutput(adapter.Get());
            if (targetOutput) {
                targetAdapter = adapter;
                break;
            }
        }

        if (!targetAdapter || !targetOutput) {
            sc_logger::Error("DXGI: Could not find target monitor output");
            return false;
        }

        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1
        };

        HRESULT hr = D3D11CreateDevice(
            targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &m_device, nullptr, &m_context
        );

        if (FAILED(hr)) {
            sc_logger::Error("DXGI: D3D11CreateDevice failed with 0x{:08X}", hr);
            return false;
        }

        hr = targetOutput->DuplicateOutput(m_device.Get(), m_duplication.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            sc_logger::Error("DXGI: DuplicateOutput failed with 0x{:08X}", hr);
            return false;
        }

        DXGI_OUTDUPL_DESC desc;
        m_duplication->GetDesc(&desc);
        if (desc.DesktopImageInSystemMemory) {
            sc_logger::Error("DXGI: DesktopImageInSystemMemory path is not supported");
            return false;
        }

        m_targetWidth = desc.ModeDesc.Width;
        m_targetHeight = desc.ModeDesc.Height;
        sc_logger::Info("DXGI: target capture size {}x{}", m_targetWidth, m_targetHeight);
        m_firstFrameCaptured.store(false, std::memory_order_release);

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = m_targetWidth;
        texDesc.Height = m_targetHeight;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < 2; i++) {
            hr = m_device->CreateTexture2D(&texDesc, nullptr, m_sharedTex[i].ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                sc_logger::Error("DXGI: Failed to create shared texture {}", i);
                return false;
            }

            // Clear textures to black to prevent white flashes or garbage data during transitions
            float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(m_device->CreateRenderTargetView(m_sharedTex[i].Get(), nullptr, &rtv))) {
                m_context->ClearRenderTargetView(rtv.Get(), black);
            }

            Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
            m_sharedTex[i].As(&dxgiRes);
            hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &m_sharedHandleInternal[i]);
            if (FAILED(hr)) {
                sc_logger::Error("DXGI: Failed to create shared handle {}", i);
                return false;
            }
        }

        D3D11_QUERY_DESC qDesc = { D3D11_QUERY_EVENT, 0 };
        for (int i = 0; i < 2; i++) {
            hr = m_device->CreateQuery(&qDesc, m_query[i].ReleaseAndGetAddressOf());
            if (FAILED(hr)) return false;
        }

        m_context->Flush();
        return true;
    }

    void CleanupDirect3D() {
        // 1. Od razu unieważnij publiczny uchwyt i stan gotowości
        m_sharedHandle.store(nullptr, std::memory_order_release);
        m_width.store(0, std::memory_order_relaxed);
        m_height.store(0, std::memory_order_relaxed);
        m_firstFrameCaptured.store(false, std::memory_order_release);

        // 2. Poczekaj na zakończenie poleceń GPU (np. CopyResource)
        if (m_context) {
            m_context->Flush();
            // Daj GPU chwilę na domknięcie operacji przed zwolnieniem uchwytów
            std::this_thread::sleep_for(std::chrono::milliseconds(32));
            m_context->ClearState();
        }

        // 3. Teraz bezpiecznie zamknij lokalne uchwyty i zwolnij tekstury
        for (int i = 0; i < 2; i++) {
            if (m_sharedHandleInternal[i]) CloseHandle(m_sharedHandleInternal[i]);
            m_sharedHandleInternal[i] = nullptr;
            m_sharedTex[i] = nullptr;
            m_query[i] = nullptr;
        }
        m_duplication = nullptr;
        m_context = nullptr;
        m_device = nullptr;
    }

    bool CaptureFrame(std::stop_token stopToken) {
        if (!m_duplication) {
            sc_logger::Error("DXGI: CaptureFrame called with no duplication object");
            return false;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

        HRESULT hr = m_duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return true;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
            sc_logger::Info("DXGI: Access lost or session disconnected (0x{:08X})", hr);
            return false;
        }

        if (FAILED(hr)) {
            sc_logger::Warn("DXGI: AcquireNextFrame failed with 0x{:08X}", hr);
            return false;
        }

        if (stopToken.stop_requested()) {
            m_duplication->ReleaseFrame();
            return false;
        }

        // Skip mouse-only frames during initialization to prevent flashes of uninitialized textures.
        // Chromium's GPU process can crash if we provide textures that aren't fully ready.
       // DOBRZE:
        if (frameInfo.AccumulatedFrames == 0) {
            m_duplication->ReleaseFrame();
            return true;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
        if (SUCCEEDED(desktopResource.As(&desktopTexture))) {
            D3D11_TEXTURE2D_DESC texDesc;
            desktopTexture->GetDesc(&texDesc);

            if (texDesc.Width != m_targetWidth || texDesc.Height != m_targetHeight) {
                sc_logger::Info("DXGI: Surface size mismatch (prawdopodobnie zmiana rozdzielczości), reinit.");
                m_duplication->ReleaseFrame();
                return false;
            }

            m_currentIndex = (m_currentIndex + 1) % 2;
            m_context->CopyResource(m_sharedTex[m_currentIndex].Get(), desktopTexture.Get());

            if (m_query[m_currentIndex]) {
                m_context->End(m_query[m_currentIndex].Get());
                m_context->Flush();
                while (m_context->GetData(m_query[m_currentIndex].Get(), nullptr, 0, 0) != S_OK) {
                    if (stopToken.stop_requested()) return false;
                    Sleep(0);
                }
            }

            m_sharedHandle.store(m_sharedHandleInternal[m_currentIndex], std::memory_order_release);
            if (!m_firstFrameCaptured.load(std::memory_order_acquire)) {
                m_width.store(m_targetWidth, std::memory_order_relaxed);
                m_height.store(m_targetHeight, std::memory_order_relaxed);
                m_firstFrameCaptured.store(true, std::memory_order_release);
                sc_logger::Info("DXGI: First frame captured, backend ready");
            }

            m_frameCount++;
            InvokeFrameAvailableCallback();
        }

        m_duplication->ReleaseFrame();

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastFpsTime).count();
        if (elapsed >= 1) {
            uint64_t frames = m_frameCount.exchange(0);
            m_lastFps = static_cast<int>(frames / elapsed);
            m_lastFpsTime = now;
        }

        return true;
    }
};

// Check if OS is available to initialize DXGI DDA (Windows 8+)
bool IsWin8OrGreaterForDXGI() {
    HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        using RtlGetVersionPtr = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto fxPtr = reinterpret_cast<RtlGetVersionPtr>(::GetProcAddress(hMod, "RtlGetVersion"));
        if (fxPtr) {
            RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (fxPtr(&rovi) == 0) {
                // Win 8 is build 9200 (Major 6, Minor 2)
                if (rovi.dwMajorVersion > 6) return true;
                if (rovi.dwMajorVersion == 6 && rovi.dwMinorVersion >= 2) return true;
            }
        }
    }
    return false;
}

std::unique_ptr<IPlatformCapture> CreateDXGICapture(HMONITOR monitor) {
    if (IsWin8OrGreaterForDXGI()) {
        return std::make_unique<DXGIPlatformCapture>(monitor);
    }
    return nullptr;
}

#endif // _WIN32