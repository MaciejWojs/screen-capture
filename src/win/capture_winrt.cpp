#ifdef _WIN32
#include "win_capture_internal.hpp"

#if HAS_WINRT_CAPTURE

#include <atomic>
#include <bit>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
    IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

inline IDirect3DDevice CreateDirect3DDevice(IDXGIDevice* dxgiDevice) {
    com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

class WinPlatformCapture final : public IPlatformCapture {
    public:
    WinPlatformCapture() = default;

    ~WinPlatformCapture() override {
        StopInternal();
    }

    static void CleanupHook(void* arg) {
        if (arg) {
            static_cast<WinPlatformCapture*>(arg)->StopInternal();
        }
    }

    void Start(Napi::Env env) override {
        if (m_isRunning) return;

        m_env = env;

        // Logging to JS console (Electron/Node.js)
        try {
            auto console = env.Global().Get("console").As<Napi::Object>();
            auto log = console.Get("log").As<Napi::Function>();
            log.Call(console, { Napi::String::New(env, "[ScreenCapture] INFO: Screen capture started via WinRT Graphics Capture") });
        } catch (...) {}

        napi_add_env_cleanup_hook(m_env, CleanupHook, this);

        m_isRunning = true;
        m_captureThread = std::thread([this]() {
            try {
                init_apartment(apartment_type::multi_threaded);
                InitializeD3D();

                HMONITOR monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);

                auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                check_hresult(
                    interop->CreateForMonitor(
                        monitor,
                        guid_of<GraphicsCaptureItem>(),
                        put_abi(m_item)
                    )
                );

                com_ptr<IDXGIDevice> dxgiDevice;
                m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());

                m_winrtDevice = CreateDirect3DDevice(dxgiDevice.get());

                m_width = m_item.Size().Width;
                m_height = m_item.Size().Height;

                CreateFramePoolAndSession();

                // Wait until StopInternal() is called
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(100), [this]() { return !m_isRunning.load(); });
                while (m_isRunning.load()) {
                    m_cv.wait_for(lock, std::chrono::milliseconds(100), [this]() { return !m_isRunning.load(); });
                }

                CleanupCapture();

            } catch (const hresult_error& e) {
                OutputDebugStringW(e.message().c_str());
            } catch (const std::exception& e) {
                OutputDebugStringA(e.what());
            } catch (...) {
                OutputDebugStringA("Capture thread error");
            }
            });
    }

    void Stop() override {
        StopInternal();
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
        return "winrt";
    }

    private:
    napi_env m_env{ nullptr };
    std::thread m_captureThread;
    std::atomic<bool> m_isRunning{ false };
    std::mutex m_mutex;
    std::condition_variable m_cv;

    com_ptr<ID3D11Device> m_device;
    com_ptr<ID3D11DeviceContext> m_context;

    IDirect3DDevice m_winrtDevice{ nullptr };

    com_ptr<ID3D11Texture2D> m_sharedTex;
    std::atomic<HANDLE> m_sharedHandle{ nullptr };

    GraphicsCaptureItem m_item{ nullptr };
    Direct3D11CaptureFramePool m_framePool{ nullptr };
    GraphicsCaptureSession m_session{ nullptr };
    winrt::event_token m_token{};

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // FPS counter
    std::atomic<uint64_t> m_frameCount{ 0 };
    std::atomic<int> m_lastFps{ 0 };
    std::chrono::steady_clock::time_point m_lastFpsTime = std::chrono::steady_clock::now();

    void InitializeD3D() {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            m_device.put(),
            nullptr,
            m_context.put()
        ));
    }

    void StopInternal() {
        if (m_env) {
            napi_remove_env_cleanup_hook(m_env, CleanupHook, this);
            m_env = nullptr;
        }

        if (!m_isRunning.exchange(false)) return;
        m_cv.notify_all();

        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
    }

    void CleanupCapture() {
        if (m_framePool) {
            if (m_token.value) {
                m_framePool.FrameArrived(m_token);
                m_token.value = 0;
            }
            m_framePool.Close();
            m_framePool = nullptr;
        }

        if (m_session) {
            m_session.Close();
            m_session = nullptr;
        }

        m_item = nullptr;
        m_winrtDevice = nullptr;
        m_sharedTex = nullptr;
        m_device = nullptr;
        m_context = nullptr;

        HANDLE handle = m_sharedHandle.exchange(nullptr);
        if (handle) CloseHandle(handle);
    }

    void CreateFramePoolAndSession() {
        if (m_framePool) {
            if (m_token.value) {
                m_framePool.FrameArrived(m_token);
                m_token.value = 0;
            }
            m_framePool.Close();
            m_framePool = nullptr;
        }
        if (m_session) {
            m_session.Close();
            m_session = nullptr;
        }

        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            m_item.Size()
        );

        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(false);
        m_session.IsBorderRequired(false);
        m_session.IncludeSecondaryWindows(true);

        m_token = m_framePool.FrameArrived({ this, &WinPlatformCapture::OnFrame });
        m_session.StartCapture();
    }

    void OnFrame(Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const&) {
        m_frameCount++;
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            auto size = frame.ContentSize();

            if (size.Width == 0 || size.Height == 0) return;

            if (size.Width != m_width || size.Height != m_height) {
                m_width = size.Width;
                m_height = size.Height;
                CreateFramePoolAndSession();
            }

            auto surface = frame.Surface().as<IDirect3DSurface>();
            auto access = surface.as<IDirect3DDxgiInterfaceAccess>();

            com_ptr<ID3D11Texture2D> srcTex;
            access->GetInterface(__uuidof(ID3D11Texture2D), srcTex.put_void());

            HANDLE expected = nullptr;
            if (m_sharedHandle.compare_exchange_strong(expected, nullptr)) {
                com_ptr<IDXGIResource1> res;
                srcTex->QueryInterface(__uuidof(IDXGIResource1), res.put_void());

                HANDLE handle = nullptr;
                res->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &handle
                );

                if (handle) {
                    m_sharedHandle.store(handle);
                    m_sharedTex = srcTex;
                }
            }

            // FPS calculation
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastFpsTime).count();
            if (elapsed >= 1) {
                uint64_t frames = m_frameCount.exchange(0);
                m_lastFps = static_cast<int>(frames / elapsed);
                m_lastFpsTime = now;
            }

        } catch (const std::exception& e) {
            OutputDebugStringA(e.what());
        } catch (...) {
            OutputDebugStringA("Unknown error in OnFrame");
        }
    }

    public:
    int GetFps() const override {
        return m_lastFps.load();
    }
};

std::unique_ptr<IPlatformCapture> CreateWinRTCapture() {
    return std::make_unique<WinPlatformCapture>();
}

#else

std::unique_ptr<IPlatformCapture> CreateWinRTCapture() {
    return nullptr;
}

#endif // HAS_WINRT_CAPTURE
#endif // _WIN32
