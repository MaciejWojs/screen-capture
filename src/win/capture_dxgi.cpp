#ifdef _WIN32
#include "win_capture_internal.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class DXGIPlatformCapture final : public IPlatformCapture {
    public:
    DXGIPlatformCapture() = default;
    ~DXGIPlatformCapture() override {
        Stop();
    }

    static void CleanupHook(void* arg) {
        if (arg) {
            static_cast<DXGIPlatformCapture*>(arg)->Stop();
        }
    }

    void Start(Napi::Env env) override {
        if (m_isRunning) return;

        m_env = env;

        // Logging to JS console (Electron/Node.js)
        try {
            auto console = env.Global().Get("console").As<Napi::Object>();
            auto log = console.Get("log").As<Napi::Function>();
            log.Call(console, { Napi::String::New(env, "[ScreenCapture] INFO: Screen capture started via DXGI Desktop Duplication API") });
        } catch (...) {}

        napi_add_env_cleanup_hook(m_env, CleanupHook, this);

        m_isRunning = true;
        m_captureThread = std::thread([this]() {
            if (!InitializeDirect3D()) {
                m_isRunning = false;
                return;
            }

            while (m_isRunning) {
                if (!CaptureFrame()) {
                    // After session loss (e.g. screen resolution change / disconnect) - we refresh
                    CleanupDirect3D();
                    if (!InitializeDirect3D()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Waiting for screen to return
                    }
                }
            }

            CleanupDirect3D();
            });
    }

    void Stop() override {
        if (m_env) {
            napi_remove_env_cleanup_hook(m_env, CleanupHook, this);
            m_env = nullptr;
        }

        if (!m_isRunning.exchange(false)) return;

        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        HANDLE handle = m_sharedHandle.load();
        if (!handle) return std::nullopt;

        SharedHandleInfo info;
        info.handle = reinterpret_cast<uint64_t>(handle);
        info.width = m_width;
        info.height = m_height;
        return info;
    }

    private:
    napi_env m_env{ nullptr };
    std::thread m_captureThread;
    std::atomic<bool> m_isRunning{ false };

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTex;
    std::atomic<HANDLE> m_sharedHandle{ nullptr };

    uint32_t m_width = 0;
    uint32_t m_height = 0;

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
        if (FAILED(dxgiOutput.As(&dxgiOutput1))) return false; // This will throw error e.g. on Win7 without Platform Update

        hr = dxgiOutput1->DuplicateOutput(m_device.Get(), &m_duplication);
        if (FAILED(hr)) return false; // E_ACCESSDENIED in Win8 full-screen apps, etc.

        DXGI_OUTDUPL_DESC desc;
        m_duplication->GetDesc(&desc);
        if (desc.DesktopImageInSystemMemory) return false; // We don't want to copy this way

        m_width = desc.ModeDesc.Width;
        m_height = desc.ModeDesc.Height;

        // Creating "Shared Texture"
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = m_width;
        texDesc.Height = m_height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        // DXGI DDA returns B8G8R8A8_UNORM per specification
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

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
        m_sharedHandle.store(nullptr);
        m_sharedTex = nullptr;
        m_duplication = nullptr;
        m_context = nullptr;
        m_device = nullptr;
    }

    bool CaptureFrame() {
        if (!m_duplication) return false;

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

        // Timeout e.g. 250ms, so thread doesn't block infinitely. Gives chance to respond to m_isRunning=false flag
        HRESULT hr = m_duplication->AcquireNextFrame(250, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return true; // Nothing changed on screen, maintaining connection
        }
        if (FAILED(hr)) {
            // This usually means ACCESS_LOST (lost full-screen or resolution change)
            return false;
        }

        if (frameInfo.AccumulatedFrames == 0 || frameInfo.LastPresentTime.QuadPart == 0) {
            // Frame not refreshed, just releasing
            m_duplication->ReleaseFrame();
            return true;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
        if (SUCCEEDED(desktopResource.As(&desktopTexture))) {
            // We copy texture obtained from Desktop Duplication directly to our SharedTexture on hardware side
            m_context->CopyResource(m_sharedTex.Get(), desktopTexture.Get());
            m_context->Flush();
        }

        m_duplication->ReleaseFrame();
        return true;
    }
};

// Check if OS is available to initialize DXGI DDA (Windows 8+)
bool IsWin8OrGreaterForDXGI() {
    HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr fxPtr = (RtlGetVersionPtr)::GetProcAddress(hMod, "RtlGetVersion");
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
    // Windows 8 (DXGI 1.2) API
    if (IsWin8OrGreaterForDXGI()) {
        return std::make_unique<DXGIPlatformCapture>();
    }
    return nullptr;
}

#endif