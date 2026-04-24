#ifdef _WIN32
#include "../logger.hpp"
#include "win_capture_internal.hpp"

#if HAS_WINRT_CAPTURE

#include <atomic>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
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
    WinPlatformCapture() {
        sc_logger::Info("WinPlatformCapture constructor called");
    }

    ~WinPlatformCapture() override {
        sc_logger::Info("WinPlatformCapture destructor called");
        StopInternal();
    }

    static void CleanupHook(void* arg) {
        sc_logger::Info("CleanupHook called");
        if (arg) {
            static_cast<WinPlatformCapture*>(arg)->Stop();
        }
    }

    void Start(Napi::Env env) override {
        sc_logger::Info("WinPlatformCapture::Start called");
        if (m_jthread.joinable()) {
            sc_logger::Warn("Start called but thread already running");
            return;
        }

        m_cleaned.store(false, std::memory_order_relaxed);
        m_env = env;
        sc_logger::Info("Screen capture started via WinRT Graphics Capture (jthread)");

        napi_add_env_cleanup_hook(m_env, CleanupHook, this);

        m_jthread = std::jthread([this](std::stop_token stopToken) {
            sc_logger::Info("Capture thread started");
            try {
                init_apartment(apartment_type::multi_threaded);
                sc_logger::Info("Apartment initialized (multi-threaded)");

                if (stopToken.stop_requested()) {
                    sc_logger::Info("Stop requested before capture initialization");
                    return;
                }

                InitializeD3D();

                HMONITOR monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
                sc_logger::Info("Using primary monitor handle: {}", reinterpret_cast<void*>(monitor));

                auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                check_hresult(
                    interop->CreateForMonitor(
                        monitor,
                        guid_of<GraphicsCaptureItem>(),
                        put_abi(m_item)
                    )
                );
                sc_logger::Info("GraphicsCaptureItem created for monitor");

                com_ptr<IDXGIDevice> dxgiDevice;
                m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());
                if (!dxgiDevice) {
                    throw std::runtime_error("DXGI device is null");
                }
                sc_logger::Info("DXGI device obtained");

                m_winrtDevice = CreateDirect3DDevice(dxgiDevice.get());
                sc_logger::Info("Direct3D device created for WinRT");

                {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_width = m_item.Size().Width;
                    m_height = m_item.Size().Height;
                }

                if (m_width == 0 || m_height == 0) {
                    sc_logger::Warn("Initial capture size is zero, using fallback until first frame");
                } else {
                    sc_logger::Info("Capture size: {}x{}", m_width, m_height);
                }

                CreateFramePoolAndSession(m_winrtDevice, m_item);

                m_lastFpsTimeNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
                m_running.store(true, std::memory_order_release);
                sc_logger::Info("Capture running flag set");

                // Wait until stop is requested or pool recreation is requested
                std::unique_lock lock(m_waitMutex);
                while (!stopToken.stop_requested()) {
                    m_waitCv.wait(lock, [&] {
                        return stopToken.stop_requested() || m_poolRecreationRequested.load();
                        });

                    if (stopToken.stop_requested()) {
                        sc_logger::Info("Stop requested, exiting wait loop");
                        break;
                    }

                    if (m_poolRecreationRequested.exchange(false)) {
                        sc_logger::Info("Recreating frame pool and session due to size change");
                        auto item = m_item;
                        auto device = m_winrtDevice;
                        {
                            std::lock_guard<std::mutex> stateLock(m_stateMutex);
                            item = m_item;
                            device = m_winrtDevice;
                        }
                        if (item && device) {
                            CreateFramePoolAndSession(device, item);
                        } else {
                            sc_logger::Warn("Cannot recreate pool: item or device is null");
                        }
                    }
                }

                CleanupCapture();
                sc_logger::Info("Capture thread finished cleanly");
            } catch (const hresult_error& e) {
                sc_logger::Error("WinRT capture thread error: {}", winrt::to_string(e.message()));
                CleanupCapture();
            } catch (const std::exception& e) {
                sc_logger::Error("WinRT capture thread error: {}", e.what());
                CleanupCapture();
            } catch (...) {
                sc_logger::Error("Capture thread unknown error");
                CleanupCapture();
            }
            });
    }

    void Stop() override {
        sc_logger::Info("WinPlatformCapture::Stop called");
        StopInternal();
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        sc_logger::Info("GetSharedHandle called");
        if (!m_captureStarted.load(std::memory_order_acquire) || !m_textureReady.load(std::memory_order_acquire)) {
            sc_logger::Warn("GetSharedHandle: capture not started or texture not ready");
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(m_stateMutex);
        HANDLE handle = m_sharedHandle.load();
        if (!handle) {
            sc_logger::Warn("GetSharedHandle: shared handle is null");
            return std::nullopt;
        }

        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            sc_logger::Error("GetSharedHandle: DuplicateHandle failed, error = {}", GetLastError());
            return std::nullopt;
        }

        SharedHandleInfo info;
        info.handle = static_cast<uint64_t>(std::bit_cast<std::uintptr_t>(duplicate));
        info.width = m_width;
        info.height = m_height;
        info.stride = static_cast<uint32_t>(m_width * 4);
        info.pixelFormat = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
        sc_logger::Info("GetSharedHandle succeeded: handle={}, size={}x{}", reinterpret_cast<void*>(duplicate), m_width, m_height);
        return info;
    }

    int GetWidth() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return static_cast<int>(m_width);
    }
    int GetHeight() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return static_cast<int>(m_height);
    }
    int GetStride() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return static_cast<int>(m_width * 4);
    }
    uint32_t GetPixelFormat() const override { return static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM); }
    std::string GetBackendName() const override { return "winrt"; }
    int GetFps() const override {
        return m_lastFps.load(std::memory_order_relaxed);
    }

    private:
    napi_env m_env{ nullptr };
    std::jthread m_jthread;
    std::mutex m_waitMutex;
    std::condition_variable_any m_waitCv;
    mutable std::mutex m_stateMutex;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_poolRecreationRequested{ false };
    std::atomic<bool> m_cleaned{ false };
    std::atomic<bool> m_deviceReady{ false };
    std::atomic<bool> m_textureReady{ false };
    std::atomic<bool> m_captureStarted{ false };

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

    std::atomic<uint64_t> m_frameCount{ 0 };
    std::atomic<int> m_lastFps{ 0 };
    std::atomic<int64_t> m_lastFpsTimeNs{ 0 };

    void InitializeD3D() {
        sc_logger::Info("InitializeD3D: creating D3D11 device");
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(
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
        );

        if (FAILED(hr)) {
            sc_logger::Warn("D3D11CreateDevice(HARDWARE) failed with error 0x{:08X}, falling back to WARP", hr);
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                ARRAYSIZE(levels),
                D3D11_SDK_VERSION,
                m_device.put(),
                nullptr,
                m_context.put()
            );
        }

        check_hresult(hr);
        sc_logger::Info("D3D11 device created successfully");
        m_deviceReady.store(true, std::memory_order_release);
    }

    void StopInternal() {
        sc_logger::Info("StopInternal called");
        m_running.store(false, std::memory_order_release);

        if (m_env) {
            napi_remove_env_cleanup_hook(m_env, CleanupHook, this);
            sc_logger::Info("Removed cleanup hook");
            m_env = nullptr;
        }

        if (m_jthread.joinable()) {
            sc_logger::Info("Requesting stop and joining capture thread");
            m_jthread.request_stop();
            m_waitCv.notify_all();
            m_jthread.join();
            sc_logger::Info("Capture thread joined");
        }
    }

    void CleanupCapture() {
        sc_logger::Info("CleanupCapture: starting cleanup");
        if (m_cleaned.exchange(true)) {
            sc_logger::Info("CleanupCapture already performed, skipping");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_session) {
                sc_logger::Info("Closing capture session");
                m_session.Close();
                m_session = nullptr;
            }
        }

        auto framePool = m_framePool;
        auto token = m_token;

        m_framePool = nullptr;
        m_token = {};

        if (framePool && token.value) {
            sc_logger::Info("Removing FrameArrived handler");
            framePool.FrameArrived(token);
        }

        if (framePool) {
            sc_logger::Info("Closing frame pool");
            framePool.Close();
            framePool = nullptr;
        }

        std::lock_guard<std::mutex> lock(m_stateMutex);

        m_item = nullptr;
        m_winrtDevice = nullptr;
        m_sharedTex = nullptr;
        m_device = nullptr;
        m_context = nullptr;
        m_deviceReady.store(false, std::memory_order_release);
        m_textureReady.store(false, std::memory_order_release);
        m_captureStarted.store(false, std::memory_order_release);

        HANDLE handle = m_sharedHandle.exchange(nullptr);
        if (handle) {
            sc_logger::Info("Closing shared handle");
            CloseHandle(handle);
        }
        sc_logger::Info("CleanupCapture finished");
    }

    void CreateFramePoolAndSession(IDirect3DDevice const& winrtDevice, GraphicsCaptureItem const& item) {
        sc_logger::Info("CreateFramePoolAndSession called");
        if (!winrtDevice) {
            sc_logger::Error("GraphicsCaptureDevice is null");
            throw std::runtime_error("WinRT device is null");
        }
        if (!item) {
            sc_logger::Error("GraphicsCaptureItem is null");
            throw std::runtime_error("GraphicsCaptureItem is null");
        }

        auto size = item.Size();
        sc_logger::Info("Item size from GraphicsCaptureItem: {}x{}", size.Width, size.Height);
        if (size.Width < 0 || size.Height < 0) {
            sc_logger::Warn("Invalid capture size {},{} - waiting for first frame", size.Width, size.Height);
            return;
        }
        if (size.Width == 0 || size.Height == 0) {
            sc_logger::Warn("CreateFramePoolAndSession: initial item size is zero, waiting for valid size");
            for (int retry = 0; retry < 20; retry++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                size = item.Size();
                if (size.Width != 0 && size.Height != 0) {
                    sc_logger::Info("CreateFramePoolAndSession: item size became valid {}x{}", size.Width, size.Height);
                    break;
                }
            }
        }

        if (size.Width < 0 || size.Height < 0) {
            sc_logger::Warn("Invalid capture size {},{} - waiting for first frame", size.Width, size.Height);
            return;
        }
        if (size.Width == 0 || size.Height == 0) {
            sc_logger::Warn("Invalid capture size 0x0 after wait, using fallback 1920x1080");
            size.Width = 1920;
            size.Height = 1080;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_width = size.Width;
            m_height = size.Height;
        }

        auto oldPool = m_framePool;
        auto oldToken = m_token;

        m_framePool = nullptr;
        m_token = {};

        if (oldPool && oldToken.value) {
            sc_logger::Info("Removing previous FrameArrived handler");
            oldPool.FrameArrived(oldToken);
        }

        if (oldPool) {
            sc_logger::Info("Closing previous frame pool");
            oldPool.Close();
            oldPool = nullptr;
        }

        if (m_session) {
            sc_logger::Info("Closing previous capture session");
            m_session.Close();
            m_session = nullptr;
        }

        m_textureReady.store(false, std::memory_order_release);
        m_captureStarted.store(false, std::memory_order_release);

        winrt::Windows::Graphics::SizeInt32 framePoolSize;
        framePoolSize.Width = static_cast<int32_t>(size.Width);
        framePoolSize.Height = static_cast<int32_t>(size.Height);

        sc_logger::Info("WINRT SIZE RAW: {}x{}", size.Width, size.Height);
        sc_logger::Info("WINRT DEVICE: {}", static_cast<void*>(winrt::get_abi(winrtDevice)));
        sc_logger::Info("WINRT ITEM VALID: {}", static_cast<bool>(item));

        sc_logger::Info("Creating shared texture before frame pool {}x{}", size.Width, size.Height);
        CreateOrRecreateSharedTexture();

        sc_logger::Info("Creating frame pool {}x{}", size.Width, size.Height);
        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            framePoolSize
        );
        sc_logger::Info("Frame pool created");

        sc_logger::Info("Creating session...");
        m_session = m_framePool.CreateCaptureSession(item);
        m_session.IsCursorCaptureEnabled(false);
        m_session.IsBorderRequired(false);
        m_session.IncludeSecondaryWindows(true);
        sc_logger::Info("Capture session configured");

        m_token = m_framePool.FrameArrived({ this, &WinPlatformCapture::OnFrame });
        sc_logger::Info("FrameArrived handler registered");
        sc_logger::Info("Starting capture...");
        m_session.StartCapture();
        m_captureStarted.store(true, std::memory_order_release);
        sc_logger::Info("Capture started");
    }

    void CreateOrRecreateSharedTexture() {
        sc_logger::Info("CreateOrRecreateSharedTexture called, size {}x{}", m_width, m_height);
        if (!m_device) {
            sc_logger::Error("Cannot create shared texture: D3D device is missing");
            return;
        }

        HANDLE oldHandle = m_sharedHandle.exchange(nullptr);
        if (oldHandle) {
            sc_logger::Info("Closing old shared handle");
            CloseHandle(oldHandle);
        }

        m_sharedTex = nullptr;

        if (m_width == 0 || m_height == 0) {
            sc_logger::Warn("Cannot create shared texture: zero dimensions");
            return;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_sharedTex.put());
        if (FAILED(hr)) {
            sc_logger::Error("CreateTexture2D failed with error 0x{:08X}", hr);
            throw std::runtime_error("Failed to create shared texture");
        }

        com_ptr<IDXGIResource1> res;
        hr = m_sharedTex->QueryInterface(__uuidof(IDXGIResource1), res.put_void());
        if (FAILED(hr)) {
            sc_logger::Error("QueryInterface for IDXGIResource1 failed with error 0x{:08X}", hr);
            throw std::runtime_error("Failed to get IDXGIResource1");
        }

        HANDLE handle = nullptr;
        HRESULT createHandleResult = E_FAIL;
        for (int i = 0; i < 3; i++) {
            createHandleResult = res->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr,
                &handle
            );
            if (SUCCEEDED(createHandleResult)) {
                break;
            }
            sc_logger::Warn("CreateSharedHandle failed retry {} with error 0x{:08X}", i, createHandleResult);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (FAILED(createHandleResult)) {
            sc_logger::Error("CreateSharedHandle failed after retries with error 0x{:08X}", createHandleResult);
            throw std::runtime_error("Failed to create shared handle");
        }

        m_sharedHandle.store(handle);
        m_textureReady.store(true, std::memory_order_release);
        sc_logger::Info("Shared texture and handle created successfully, handle = {}", reinterpret_cast<void*>(handle));
    }

    void OnFrame(Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const&) {
        static uint64_t frameLogCounter = 0;
        if (!m_running.load(std::memory_order_acquire)) {
            sc_logger::Warn("OnFrame called but capture not running");
            return;
        }

        while (true) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
                if (frameLogCounter++ % 300 == 0) {
                    sc_logger::Info("OnFrame: no frame available (might be normal)");
                }
                return;
            }

            auto size = frame.ContentSize();
            if (size.Width == 0 || size.Height == 0) {
                sc_logger::Warn("OnFrame: received frame with zero size, skipping");
                continue;
            }

            uint32_t currentWidth;
            uint32_t currentHeight;
            {
                std::lock_guard<std::mutex> stateLock(m_stateMutex);
                currentWidth = m_width;
                currentHeight = m_height;
            }

            if (size.Width != currentWidth || size.Height != currentHeight) {
                sc_logger::Info("OnFrame: size changed from {}x{} to {}x{}, requesting pool recreation", currentWidth, currentHeight, size.Width, size.Height);
                {
                    std::lock_guard<std::mutex> stateLock(m_stateMutex);
                    m_width = size.Width;
                    m_height = size.Height;
                    m_poolRecreationRequested.store(true);
                }
                m_waitCv.notify_all();
                return;
            }

            com_ptr<ID3D11Texture2D> localSharedTex;
            com_ptr<ID3D11DeviceContext> localContext;
            {
                std::lock_guard<std::mutex> stateLock(m_stateMutex);
                if (!m_sharedTex || !m_context) {
                    sc_logger::Warn("OnFrame: sharedTex or context is null");
                    return;
                }
                localSharedTex = m_sharedTex;
                localContext = m_context;
            }

            if (!localContext || !localSharedTex) {
                sc_logger::Warn("OnFrame: localSharedTex or localContext is null");
                return;
            }

            m_frameCount.fetch_add(1, std::memory_order_relaxed);

            try {
                auto surface = frame.Surface().as<IDirect3DSurface>();
                auto access = surface.as<IDirect3DDxgiInterfaceAccess>();

                com_ptr<ID3D11Texture2D> srcTex;
                HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), srcTex.put_void());
                if (FAILED(hr)) {
                    sc_logger::Error("OnFrame: GetInterface failed with error 0x{:08X}", hr);
                    return;
                }

                localContext->CopyResource(localSharedTex.get(), srcTex.get());
                // Log co 1000 klatek, aby nie spamować
                if (m_frameCount.load() % 1000 == 0) {
                    sc_logger::Info("OnFrame: copied frame #{}", m_frameCount.load());
                }

                auto now = std::chrono::steady_clock::now();
                int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                int64_t lastNs = m_lastFpsTimeNs.load(std::memory_order_relaxed);
                if (lastNs == 0) {
                    m_lastFpsTimeNs.store(nowNs, std::memory_order_relaxed);
                } else if (nowNs - lastNs >= 1000000000LL) {
                    uint64_t frames = m_frameCount.exchange(0, std::memory_order_relaxed);
                    m_lastFps.store(static_cast<int>(frames), std::memory_order_relaxed);
                    m_lastFpsTimeNs.store(nowNs, std::memory_order_relaxed);
                    sc_logger::Info("OnFrame: FPS updated to {}", frames);
                }
            } catch (const std::exception& e) {
                sc_logger::Error("Unknown error in OnFrame: {}", e.what());
                return;
            } catch (...) {
                sc_logger::Error("Unknown error in OnFrame");
                return;
            }
        }
    }
};

std::unique_ptr<IPlatformCapture> CreateWinRTCapture() {
    sc_logger::Info("CreateWinRTCapture called");
    return std::make_unique<WinPlatformCapture>();
}

#else // !HAS_WINRT_CAPTURE

std::unique_ptr<IPlatformCapture> CreateWinRTCapture() {
    sc_logger::Warn("CreateWinRTCapture called but WinRT capture is not supported");
    return nullptr;
}

#endif // HAS_WINRT_CAPTURE
#endif // _WIN32