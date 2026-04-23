#ifdef _WIN32
#include "../logger.hpp"
#include "win_capture_internal.hpp"

#include <atomic>
#include <bit>
#include <condition_variable>
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
    DXGIPlatformCapture() = default;
    ~DXGIPlatformCapture() override { Stop(); }

    static void CleanupHook(void* arg) {
        if (arg) static_cast<DXGIPlatformCapture*>(arg)->Stop();
    }

    void Start(Napi::Env env) override {
        if (m_thread.joinable()) return;
        m_env = env;
        sc_logger::Info("Screen capture started via DXGI Desktop Duplication API with jthread");

        napi_add_env_cleanup_hook(m_env, CleanupHook, this);

        m_thread = std::jthread([this](std::stop_token stopToken) {
            if (!InitializeDirect3D()) {
                sc_logger::Error("DXGI: Initialization failed");
                return;
            }

            // Main capture loop
            while (!stopToken.stop_requested()) {
                if (!CaptureFrame(stopToken)) {
                    sc_logger::Info("DXGI: Session lost, reinitializing...");
                    CleanupDirect3D();

                    while (!stopToken.stop_requested()) {
                        if (InitializeDirect3D())
                            break;
                        std::unique_lock lock(m_reinitMutex);
                        m_reinitCv.wait_for(lock, std::chrono::milliseconds(100),
                            [&stopToken] { return stopToken.stop_requested(); });
                    }
                }
            }

            CleanupDirect3D();
            sc_logger::Info("DXGI capture thread stopped");
            });
    }

    void Stop() override {
        if (m_env) {
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
        HANDLE handle = m_sharedHandle.load();
        if (!handle) return std::nullopt;

        SharedHandleInfo info;
        info.handle = static_cast<uint64_t>(std::bit_cast<std::uintptr_t>(handle));
        info.width = m_width;
        info.height = m_height;
        return info;
    }

    int GetWidth() const override { return static_cast<int>(m_width); }
    int GetHeight() const override { return static_cast<int>(m_height); }
    int GetStride() const override { return static_cast<int>(m_width * 4); }
    uint32_t GetPixelFormat() const override { return static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM); }
    std::string GetBackendName() const override { return "dxgi"; }
    int GetFps() const override { return m_lastFps.load(); }

    private:
    napi_env m_env{ nullptr };
    std::jthread m_thread;                      // automatyczne zarządzanie wątkiem
    mutable std::mutex m_reinitMutex;           // dla condition_variable przy ponownej inicjalizacji
    std::condition_variable_any m_reinitCv;     // może czekać na stop_token

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTex;
    std::atomic<HANDLE> m_sharedHandle{ nullptr };

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    std::atomic<uint64_t> m_frameCount{ 0 };
    std::atomic<int> m_lastFps{ 0 };
    std::chrono::steady_clock::time_point m_lastFpsTime = std::chrono::steady_clock::now();

    bool InitializeDirect3D() {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &m_device, nullptr, &m_context
        );
        if (FAILED(hr)) return false;

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device.As(&dxgiDevice))) return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
        if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter))) return false;

        Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
        if (FAILED(dxgiAdapter->EnumOutputs(0, &dxgiOutput))) return false;

        Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
        if (FAILED(dxgiOutput.As(&dxgiOutput1))) return false; // wymaga DXGI 1.2 (Win8+)

        hr = dxgiOutput1->DuplicateOutput(m_device.Get(), &m_duplication);
        if (FAILED(hr)) return false; // np. E_ACCESSDENIED

        DXGI_OUTDUPL_DESC desc;
        m_duplication->GetDesc(&desc);
        if (desc.DesktopImageInSystemMemory) return false; // nie chcemy tej ścieżki

        m_width = desc.ModeDesc.Width;
        m_height = desc.ModeDesc.Height;

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = m_width;
        texDesc.Height = m_height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_sharedTex);
        if (FAILED(hr)) return false;

        Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
        if (SUCCEEDED(m_sharedTex.As(&dxgiRes))) {
            HANDLE sharedHandle = nullptr;
            if (SUCCEEDED(dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle))) {
                m_sharedHandle.store(sharedHandle);
                return true;
            }
        }
        return false;
    }

    void CleanupDirect3D() {
        HANDLE handle = m_sharedHandle.exchange(nullptr);
        if (handle) CloseHandle(handle);
        m_sharedTex = nullptr;
        m_duplication = nullptr;
        m_context = nullptr;
        m_device = nullptr;
    }

    bool CaptureFrame(std::stop_token stopToken) {
        if (!m_duplication) return false;

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

        HRESULT hr = m_duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return true;
        }
        if (FAILED(hr)) {
            // DXGI_ERROR_ACCESS_LOST etc.
            return false;
        }

        // check if stop was requested while waiting for the frame
        if (stopToken.stop_requested()) {
            m_duplication->ReleaseFrame();
            return false;
        }

        if (frameInfo.AccumulatedFrames == 0 || frameInfo.LastPresentTime.QuadPart == 0) {
            m_duplication->ReleaseFrame();
            return true;
        }

        m_frameCount++;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
        if (SUCCEEDED(desktopResource.As(&desktopTexture))) {
            m_context->CopyResource(m_sharedTex.Get(), desktopTexture.Get());
            m_context->Flush();
        }

        m_duplication->ReleaseFrame();

        // FPS
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

std::unique_ptr<IPlatformCapture> CreateDXGICapture() {
    if (IsWin8OrGreaterForDXGI()) {
        return std::make_unique<DXGIPlatformCapture>();
    }
    return nullptr;
}

#endif // _WIN32