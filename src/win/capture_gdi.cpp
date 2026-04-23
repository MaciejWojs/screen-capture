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

class LegacyWinPlatformCapture final : public IPlatformCapture {
    public:
    LegacyWinPlatformCapture() = default;
    ~LegacyWinPlatformCapture() override { Stop(); }

    static void CleanupHook(void* arg) {
        if (arg) static_cast<LegacyWinPlatformCapture*>(arg)->Stop();
    }

    void Start(Napi::Env env) override {
        if (m_thread.joinable()) return;   // już działa

        m_env = env;
        sc_logger::Info("Screen capture started via GDI BitBlt fallback (C++20 jthread)");

        napi_add_env_cleanup_hook(m_env, CleanupHook, this);

        // Uruchom wątek z obsługą stop_token
        m_thread = std::jthread([this](std::stop_token stopToken) {
            InitializeDirect3D();

            std::unique_lock lock(m_cvMutex);
            while (!stopToken.stop_requested()) {
                CaptureScreenGDI();

                // Czekaj max 16 ms lub do sygnału stop
                m_cv.wait_for(lock, std::chrono::milliseconds(16),
                    [&stopToken] { return stopToken.stop_requested(); });
            }

            CleanupDirect3D();
            });
    }

    void Stop() override {
        if (m_env) {
            napi_remove_env_cleanup_hook(m_env, CleanupHook, this);
            m_env = nullptr;
        }

        if (m_thread.joinable()) {
            m_thread.request_stop();      // wysyła sygnał zatrzymania
            m_cv.notify_all();            // budzi wątek, jeśli czeka
            m_thread.join();              // czeka na zakończenie
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
    std::string GetBackendName() const override { return "gdi"; }
    int GetFps() const override { return m_lastFps.load(); }

    private:
    napi_env m_env{ nullptr };
    std::jthread m_thread;                       // automatyczne zarządzanie wątkiem
    mutable std::mutex m_cvMutex;                // dla condition_variable
    std::condition_variable_any m_cv;            // może czekać na stop_token

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTex;
    std::atomic<HANDLE> m_sharedHandle{ nullptr };

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_pixels;

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

        // staging texture (CPU write)
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateTexture2D(&desc, nullptr, &m_stagingTex);

        // shared texture (GPU default)
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
        HANDLE handle = m_sharedHandle.exchange(nullptr);
        if (handle) CloseHandle(handle);
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

        BitBlt(hMemoryDC, 0, 0, m_width, m_height, hScreenDC, 0, 0, SRCCOPY | CAPTUREBLT);

        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = m_width;
        bi.biHeight = -static_cast<LONG>(m_height);  // top-down
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        m_pixels.resize(m_width * m_height * 4);
        GetDIBits(hMemoryDC, hBitmap, 0, m_height, m_pixels.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

        // kopiuj do staging texture
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_stagingTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (uint32_t y = 0; y < m_height; ++y) {
                memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    m_pixels.data() + y * m_width * 4,
                    m_width * 4);
            }
            m_context->Unmap(m_stagingTex.Get(), 0);
            m_context->CopyResource(m_sharedTex.Get(), m_stagingTex.Get());
            m_context->Flush();
        }

        // zwolnij GDI
        SelectObject(hMemoryDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);

        // FPS
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastFpsTime).count();
        if (elapsed >= 1) {
            uint64_t frames = m_frameCount.exchange(0);
            m_lastFps = static_cast<int>(frames / elapsed);
            m_lastFpsTime = now;
        }
    }
};

std::unique_ptr<IPlatformCapture> CreateGDICapture() {
    return std::make_unique<LegacyWinPlatformCapture>();
}

#endif // _WIN32