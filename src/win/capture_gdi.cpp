#ifdef _WIN32
#include "win_capture_internal.hpp"

#include <atomic>
#include <bit>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class LegacyWinPlatformCapture final : public IPlatformCapture {
    public:
    LegacyWinPlatformCapture() = default;
    ~LegacyWinPlatformCapture() override {
        Stop();
    }

    static void CleanupHook(void* arg) {
        if (arg) {
            static_cast<LegacyWinPlatformCapture*>(arg)->Stop();
        }
    }

    void Start(Napi::Env env) override {
        if (m_isRunning) return;

        m_env = env;

        // Logging to JS console (Electron/Node.js)
        try {
            auto console = env.Global().Get("console").As<Napi::Object>();
            auto log = console.Get("log").As<Napi::Function>();
            log.Call(console, { Napi::String::New(env, "[ScreenCapture] INFO: Screen capture started via GDI BitBlt fallback") });
        } catch (...) {}

        napi_add_env_cleanup_hook(m_env, CleanupHook, this);

        m_isRunning = true;
        m_captureThread = std::thread([this]() {
            InitializeDirect3D();

            while (m_isRunning) {
                CaptureScreenGDI();
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
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
        info.handle = static_cast<uint64_t>(std::bit_cast<std::uintptr_t>(handle));
        info.width = m_width;
        info.height = m_height;
        return info;
    }

    int GetWidth() const override {
        return static_cast<int>(m_width);
    }

    int GetHeight() const override {
        return static_cast<int>(m_height);
    }

    int GetStride() const override {
        return static_cast<int>(m_width * 4);
    }

    uint32_t GetPixelFormat() const override {
        return static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    }

    std::string GetBackendName() const override {
        return "gdi";
    }

    private:
    napi_env m_env{ nullptr };
    std::thread m_captureThread;
    std::atomic<bool> m_isRunning{ false };

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTex;
    std::atomic<HANDLE> m_sharedHandle{ nullptr };

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_pixels;

    // FPS counter
    std::atomic<uint64_t> m_frameCount{ 0 };
    std::atomic<int> m_lastFps{ 0 };
    std::chrono::steady_clock::time_point m_lastFpsTime = std::chrono::steady_clock::now();

    void InitializeDirect3D() {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &m_device, nullptr, &m_context
        );
        if (FAILED(hr)) return;

        m_width = GetSystemMetrics(SM_CXSCREEN);
        m_height = GetSystemMetrics(SM_CYSCREEN);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;

        // Standard CPU-write supported texture to which we will transfer from RAM
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateTexture2D(&desc, nullptr, &m_stagingTex);

        // Shared texture
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        hr = m_device->CreateTexture2D(&desc, nullptr, &m_sharedTex);
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
            if (SUCCEEDED(m_sharedTex.As(&dxgiRes))) {
                HANDLE sharedHandle = nullptr;
                if (SUCCEEDED(dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle))) {
                    m_sharedHandle.store(sharedHandle);
                }
            }
        }
    }

    void CleanupDirect3D() {
        m_sharedHandle.store(nullptr);
        m_sharedTex = nullptr;
        m_stagingTex = nullptr;
        m_context = nullptr;
        m_device = nullptr;
    }

    void CaptureScreenGDI() {
        m_frameCount++;
        if (!m_stagingTex || !m_sharedTex) return;

        HDC hScreenDC = GetDC(nullptr);
        HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, m_width, m_height);
        HGDIOBJ hOldBitmap = SelectObject(hMemoryDC, hBitmap);

        // Screen copy
        BitBlt(hMemoryDC, 0, 0, m_width, m_height, hScreenDC, 0, 0, SRCCOPY | CAPTUREBLT);

        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = m_width;
        bi.biHeight = -static_cast<LONG>(m_height); // bottom-up to top-down
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        // Get pixels
        m_pixels.resize(m_width * m_height * 4);
        GetDIBits(hMemoryDC, hBitmap, 0, m_height, m_pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

        // Replace with buffer in D3D11
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_stagingTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (uint32_t y = 0; y < m_height; ++y) {
                memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    m_pixels.data() + y * m_width * 4,
                    m_width * 4);
            }
            m_context->Unmap(m_stagingTex.Get(), 0);

            // Copy finished texture to the shared one using GPU
            m_context->CopyResource(m_sharedTex.Get(), m_stagingTex.Get());
            m_context->Flush();
        }

        SelectObject(hMemoryDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);

        // FPS calculation
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastFpsTime).count();
        if (elapsed >= 1) {
            uint64_t frames = m_frameCount.exchange(0);
            m_lastFps = static_cast<int>(frames / elapsed);
            m_lastFpsTime = now;
        }
    }

    public:
    int GetFps() const override {
        return m_lastFps.load();
    }
};

std::unique_ptr<IPlatformCapture> CreateGDICapture() {
    return std::make_unique<LegacyWinPlatformCapture>();
}

#endif