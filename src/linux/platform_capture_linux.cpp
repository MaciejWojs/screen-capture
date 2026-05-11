#ifdef __linux__

#include "../platform_capture.hpp"
#include "../pixel_conversion.hpp"
#include "../logger.hpp"
#include "linux_raii.hpp"
#include "platform_capture_base.hpp"
#include "wayland_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

class X11PlatformCapture final : public BaseLinuxPlatformCapture {
    public:
    X11PlatformCapture() = default;

    ~X11PlatformCapture() override {
        Stop();
    }

    void Start(Napi::Env) override {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) {
            return;
        }
        m_worker = std::jthread([this](std::stop_token stopToken) {
            CaptureLoop(stopToken);
            });
    }

    void Stop() override {
        if (m_worker.joinable()) {
            m_worker.request_stop();
        }
        m_captureCv.notify_all();

        if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
            m_worker.join();
        }
        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            m_frameBuffers.Reset();
            for (auto& mapping : m_captureMappings) {
                mapping.reset();
            }
            for (auto& fd : m_captureFds) {
                fd.reset();
            }
            m_captureWriteIndex = 0;
        }
        m_running.store(false);
    }

    int GetWidth() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_sharedHandle ? static_cast<int>(m_sharedHandle->width) : 0;
    }

    int GetHeight() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_sharedHandle ? static_cast<int>(m_sharedHandle->height) : 0;
    }

    int GetStride() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_sharedHandle ? static_cast<int>(m_sharedHandle->stride) : 0;
    }

    uint32_t GetPixelFormat() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_sharedHandle ? m_sharedHandle->pixelFormat : 0;
    }

    std::optional<std::vector<uint8_t>> GetPixelData(std::string_view desiredFormat = "rgba") const override;

        std::vector<MonitorMetadata> GetMonitors() const override {
            auto info = GetCurrentMonitorInfo();
            if (info) return { *info };
            return {};
        }

    std::string GetBackendName() const override {
        return "x11";
    }

    std::optional<MonitorMetadata> GetCurrentMonitorInfo() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        if (!m_sharedHandle) {
            return std::nullopt;
        }

        MonitorMetadata info;
        info.id = "0";
        info.name = "x11-root";
        info.index = 0;
        info.x = 0;
        info.y = 0;
        info.width = static_cast<int>(m_sharedHandle->width);
        info.height = static_cast<int>(m_sharedHandle->height);
        return info;
    }

    private:
    mutable FrameBufferPool m_frameBuffers;
    std::array<UniqueFd, 2> m_captureFds;
    std::array<MmapPtr, 2> m_captureMappings;
    size_t m_captureWriteIndex = 0;

    // Cache dla przeliczone klatki (identycznie jak w Wayland)
    mutable std::vector<uint8_t> m_pixelCache;
    mutable std::string m_cacheFormat;

    void CaptureLoop(std::stop_token stopToken) {
        sc_logger::Info("Starting X11 capture loop");
        DisplayPtr display(XOpenDisplay(nullptr));
        if (!display) {
            sc_logger::Error("Cannot open X11 display");
            return;
        }

        int screen = DefaultScreen(display.get());
        Window root = RootWindow(display.get(), screen);

        XWindowAttributes attr;
        XGetWindowAttributes(display.get(), root, &attr);
        int width = attr.width;
        int height = attr.height;
        sc_logger::Info("X11 root window size: {}x{} depth={}", width, height, attr.depth);

        XShmSegmentInfoWrapper shminfo;
        XImagePtr image(XShmCreateImage(display.get(), attr.visual, attr.depth, ZPixmap, nullptr, &shminfo.info, width, height));
        if (!image) {
            sc_logger::Error("Cannot create XShmImage");
            return;
        }

        shminfo.info.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0600);
        shminfo.info.shmaddr = image->data = static_cast<char*>(shmat(shminfo.info.shmid, 0, 0));
        shminfo.info.readOnly = False;

        if (!shminfo.Attach(display.get())) {
            sc_logger::Error("XShmAttach failed");
            return;
        }

        size_t size = image->bytes_per_line * image->height;
        for (size_t i = 0; i < m_captureFds.size(); ++i) {
            int memfd = memfd_create("x11_capture", MFD_CLOEXEC);
            if (memfd < 0) {
                sc_logger::Error("X11 memfd_create failed");
                return;
            }
            m_captureFds[i].reset(new int(memfd));
            if (ftruncate(*m_captureFds[i], size) < 0) {
                sc_logger::Error("X11 ftruncate failed");
                return;
            }
            void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, *m_captureFds[i], 0);
            if (mapping == MAP_FAILED) {
                sc_logger::Error("X11 mmap failed for buffer {}", i);
                return;
            }
            m_captureMappings[i] = MmapPtr(mapping, MmapDeleter{ size });
        }

        XImage* rawImage = image.get();
        auto DetectX11PixelFormat = [rawImage]() -> uint32_t {
            if (!rawImage || rawImage->bits_per_pixel != 32) {
                return SPA_VIDEO_FORMAT_BGRA;
            }

            const unsigned long redMask = rawImage->red_mask;
            const unsigned long greenMask = rawImage->green_mask;
            const unsigned long blueMask = rawImage->blue_mask;
            const unsigned long alphaMask = ~(redMask | greenMask | blueMask);

            if (redMask == 0x00ff0000UL && greenMask == 0x0000ff00UL && blueMask == 0x000000ffUL) {
                if (alphaMask == 0xff000000UL) {
                    return SPA_VIDEO_FORMAT_BGRA;
                }
                return rawImage->byte_order == LSBFirst ? SPA_VIDEO_FORMAT_BGRx : SPA_VIDEO_FORMAT_xRGB;
            }

            if (redMask == 0x000000ffUL && greenMask == 0x0000ff00UL && blueMask == 0x00ff0000UL) {
                if (alphaMask == 0xff000000UL) {
                    return SPA_VIDEO_FORMAT_RGBA;
                }
                return rawImage->byte_order == LSBFirst ? SPA_VIDEO_FORMAT_RGBx : SPA_VIDEO_FORMAT_xBGR;
            }

            return SPA_VIDEO_FORMAT_BGRA;
            };

        uint32_t detectedFormat = DetectX11PixelFormat();

        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            if (m_captureFds[0] && *m_captureFds[0] >= 0) {
                m_sharedFd.reset(new int(dup(*m_captureFds[0])));
            } else {
                m_sharedFd.reset();
            }
            if (m_sharedFd && *m_sharedFd >= 0) {
                sc_logger::Info("X11 MemFd created, FD={} size={}", *m_sharedFd, size);
            }
            sc_logger::Info("X11 pixel format: {} (depth={}, bpp={})",
                PixelFormatToString(detectedFormat), image->depth, image->bits_per_pixel);
            m_sharedHandle = SharedHandleInfo{
                static_cast<uint64_t>(*m_sharedFd),
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height),
                static_cast<uint32_t>(image->bytes_per_line),
                0, // offset
                static_cast<uint64_t>(size), // planeSize
                detectedFormat,
                0, // modifier
                SPA_DATA_MemFd, // MemFd handle type for X11 shared memory capture
                static_cast<uint32_t>(size) // chunkSize
            };
            m_frameConsumed = false;
        }

        uint64_t frameCounter = 0;
        auto nextFrameTime = std::chrono::steady_clock::now();
        while (!stopToken.stop_requested()) {
            nextFrameTime += std::chrono::microseconds(16666); // Target ~60fps

            if (XShmGetImage(display.get(), root, image.get(), 0, 0, AllPlanes)) {
                size_t writeIndex = m_captureWriteIndex;
                std::memcpy(m_captureMappings[writeIndex].get(), image->data, size);

                {
                    std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                    SharedHandleInfo handle{
                            0, // Will be filled below
                            static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height),
                            static_cast<uint32_t>(image->bytes_per_line),
                            0,
                            static_cast<uint64_t>(size),
                            detectedFormat,
                            0,
                            SPA_DATA_MemFd,
                            static_cast<uint32_t>(size),
                    };

                    SharedFd sfd(new int(dup(*m_captureFds[writeIndex])), FdDeleter());
                    if (*sfd >= 0) {
                        handle.handle = static_cast<uint64_t>(*sfd);
                        m_sharedFd = sfd;
                        m_sharedHandle = handle;
                        m_frameBuffers.PushFrame(sfd, handle, {}, true, m_captureMappings[writeIndex]);
                        m_frameConsumed = false;
                    }

                    m_captureWriteIndex = (writeIndex + 1) % m_captureFds.size();
                }

                frameCounter++;
                if (frameCounter % 120 == 0) {
                    sc_logger::Info("Captured X11 frame number {}", frameCounter);
                }
                RecordFrame();
            } else {
                sc_logger::Warn("X11 failed to grab XShmGetImage");
            }

            // Adaptive sleep to maintain constant framerate
            std::this_thread::sleep_until(nextFrameTime);
        }
        sc_logger::Info("Stopping X11 capture loop. Frames cloned: {}", frameCounter);

        // RAII sprząta zasoby: mapowanie, plik, XShm i display.
    }
};

std::optional<std::vector<uint8_t>> X11PlatformCapture::GetPixelData(std::string_view desiredFormat) const {
    auto frame = m_frameBuffers.AcquireReadFrame();
    if (!frame.has_value() || !frame->handle.has_value() || !frame->fd || *frame->fd < 0) {
        return std::nullopt;
    }

    SharedFd retainedFd = frame->fd;
    const SharedHandleInfo handle = *frame->handle;
    if (handle.width == 0 || handle.height == 0) {
        return std::nullopt;
    }

    size_t dataSize = handle.planeSize ? static_cast<size_t>(handle.planeSize)
        : static_cast<size_t>(handle.stride) * static_cast<size_t>(handle.height);
    size_t mapSize = handle.planeSize ? std::max(static_cast<size_t>(handle.planeSize), dataSize + static_cast<size_t>(handle.offset))
        : dataSize + static_cast<size_t>(handle.offset);

    if (static_cast<size_t>(handle.offset) >= mapSize) {
        sc_logger::Warn("X11 invalid offset: {} >= mapSize {}", handle.offset, mapSize);
        return std::nullopt;
    }
    size_t available = mapSize - static_cast<size_t>(handle.offset);
    if (dataSize > available) {
        sc_logger::Warn("X11 dataSize {} exceeds available {}", dataSize, available);
        return std::nullopt;
    }

    sc_logger::Debug("X11 frame: fd={} width={} height={} stride={} offset={} planeSize={} dataSize={} mapSize={} available={}",
        handle.handle,
        handle.width,
        handle.height,
        handle.stride,
        handle.offset,
        handle.planeSize,
        dataSize,
        mapSize,
        available);

    MmapPtr localMapping;
    const uint8_t* mappedData = nullptr;

    {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        if (frame->mapping) {
            localMapping = frame->mapping;
        } else {
            sc_logger::Error("X11 frame mapping missing");
            return std::nullopt;
        }
        mappedData = static_cast<const uint8_t*>(localMapping.get());
    }

    if (!mappedData) {
        return std::nullopt;
    }

    std::unique_lock<std::shared_mutex> lock(m_stateMutex);

    // Optymalizacja cache dla X11: unikamy alokacji wektora przy każdej klatce
    size_t targetSize = static_cast<size_t>(handle.width) * handle.height * 4;
    if (m_pixelCache.size() != targetSize || m_cacheFormat != desiredFormat) {
        m_pixelCache.resize(targetSize);
        m_cacheFormat = std::string(desiredFormat);
        sc_logger::Debug("X11: Initializing/Resizing pixel cache");
    }

    auto temp = ConvertPixelBuffer(
        std::span<const uint8_t>(mappedData + static_cast<size_t>(handle.offset), dataSize),
        handle.width,
        handle.height,
        handle.stride,
        handle.pixelFormat,
        desiredFormat);

    m_pixelCache = std::move(temp);

    m_frameConsumed = true;
    return m_pixelCache;
}

bool IsWayland() {
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay && waylandDisplay[0] != '\0') return true;

    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && std::string(sessionType) == "wayland") return true;

    return false;
}

std::unique_ptr<IPlatformCapture> CreatePlatformCapture(const std::string& /*forceBackend*/) {
    if (IsWayland()) {
        sc_logger::Info("Detected Wayland environment");
        return std::make_unique<WaylandPlatformCapture>();
    } else {
        sc_logger::Info("Detected X11 environment");
        return std::make_unique<X11PlatformCapture>();
    }
}

#endif
