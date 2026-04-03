#ifdef _WIN32

#include "../platform_capture.hpp"

#include <atomic>
#include <stdexcept>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
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
        init_apartment(apartment_type::multi_threaded);
        InitializeD3D();
    }

    ~WinPlatformCapture() override {
        StopInternal();
    }

    void Start(Napi::Env) override {
        if (m_session) return;

        try {
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

            auto winrtDevice = CreateDirect3DDevice(dxgiDevice.get());

            m_width = m_item.Size().Width;
            m_height = m_item.Size().Height;

            m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice,
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
        } catch (const hresult_error& e) {
            StopInternal();
            throw std::runtime_error(to_string(e.message()));
        }
    }

    void Stop() override {
        StopInternal();
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        HANDLE handle = m_sharedHandle.load();
        if (!handle) return std::nullopt;

        SharedHandleInfo info;
        info.handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
        info.width = m_width;
        info.height = m_height;
        return info;
    }

    private:
    com_ptr<ID3D11Device> m_device;
    com_ptr<ID3D11DeviceContext> m_context;

    com_ptr<ID3D11Texture2D> m_sharedTex;
    std::atomic<HANDLE> m_sharedHandle{ nullptr };

    GraphicsCaptureItem m_item{ nullptr };
    Direct3D11CaptureFramePool m_framePool{ nullptr };
    GraphicsCaptureSession m_session{ nullptr };
    winrt::event_token m_token{};

    uint32_t m_width = 0;
    uint32_t m_height = 0;

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
        if (m_framePool) {
            if (m_token.value) m_framePool.FrameArrived(m_token);
            m_token = {};
            m_framePool.Close();
            m_framePool = nullptr;
        }

        if (m_session) {
            m_session.Close();
            m_session = nullptr;
        }

        m_sharedTex = nullptr;

        HANDLE handle = m_sharedHandle.exchange(nullptr);
        if (handle) CloseHandle(handle);
    }

    void OnFrame(Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const&) {
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            auto size = frame.ContentSize();

            if (size.Width != m_width || size.Height != m_height) {
                m_width = size.Width;
                m_height = size.Height;

                m_sharedTex = nullptr;

                HANDLE oldHandle = m_sharedHandle.exchange(nullptr);
                if (oldHandle) CloseHandle(oldHandle);
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
        } catch (const std::exception& e) {
            OutputDebugStringA(e.what());
        } catch (...) {
            OutputDebugStringA("Unknown error in OnFrame");
        }
    }
};

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
    return std::make_unique<WinPlatformCapture>();
}

#endif
