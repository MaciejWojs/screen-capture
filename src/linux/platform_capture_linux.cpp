#ifdef __linux__

#include "../platform_capture.hpp"
#include "../pixel_conversion.hpp"
#include "../logger.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <pipewire/stream.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <unordered_map>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <random>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <stop_token>
#include <thread>
#include <future>
#include <utility>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>

#include <fstream>

static bool IsNvidiaGPU() {
    static std::once_flag initFlag;
    static bool cached = false;
    std::call_once(initFlag, [] {
        std::ifstream nvidia("/proc/driver/nvidia/version");
        cached = nvidia.good();
        });
    return cached;
}

namespace {

    std::string gen_token() {
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<int> dist(0, 15);
        std::string token;
        token.reserve(8);
        for (int i = 0; i < 8; ++i) {
            token += std::format("{:x}", dist(rng));
        }
        return token;
    }

    static std::string PixelFormatToString(uint32_t pixelFormat) {
        switch (pixelFormat) {
        case SPA_VIDEO_FORMAT_BGRA:
            return "bgra";
        case SPA_VIDEO_FORMAT_RGBA:
            return "rgba";
        case SPA_VIDEO_FORMAT_BGRx:
            return "bgrx";
        case SPA_VIDEO_FORMAT_RGBx:
            return "rgbx";
        case SPA_VIDEO_FORMAT_xBGR:
            return "xbgr";
        case SPA_VIDEO_FORMAT_xRGB:
            return "xrgb";
        case SPA_VIDEO_FORMAT_NV12:
            return "nv12";
        case SPA_VIDEO_FORMAT_I420:
            return "i420";
        case SPA_VIDEO_FORMAT_YUY2:
            return "yuy2";
        case SPA_VIDEO_FORMAT_AYUV:
            return "ayuv";
        case SPA_VIDEO_FORMAT_UYVY:
            return "uyvy";
        default:
            return "unknown";
        }
    }

    struct MmapDeleter {
        size_t length = 0;
        void operator()(void* ptr) const {
            if (ptr && ptr != MAP_FAILED && length > 0) {
                munmap(ptr, length);
            }
        }
    };

    using MmapPtr = std::shared_ptr<void>;
    using SharedFd = std::shared_ptr<int>;

    struct IntRect {
        uint32_t x, y, w, h;
    };

    struct FrameBufferSlot {
        SharedFd fd;
        MmapPtr mapping;
        std::optional<SharedHandleInfo> handle;
        std::vector<IntRect> damage;
        bool ready = false;
        bool fullUpdate = true;
    };

    class FrameBufferPool {
        public:
        void Reset() {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& slot : m_slots) {
                slot.fd.reset();
                slot.mapping.reset();
                slot.handle.reset();
                slot.damage.clear();
                slot.ready = false;
                slot.fullUpdate = true;
            }
            m_writeIndex = 0;
            m_latestIndex = -1;
        }

        void PushFrame(SharedFd fd, std::optional<SharedHandleInfo> handle, std::vector<IntRect> damage, bool fullUpdate, MmapPtr mapping = nullptr) {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t writeIdx = m_writeIndex;
            FrameBufferSlot& slot = m_slots[writeIdx];
            slot.fd = std::move(fd);
            slot.mapping = std::move(mapping);
            slot.handle = std::move(handle);
            slot.damage = std::move(damage);
            slot.ready = true;
            slot.fullUpdate = fullUpdate;
            m_latestIndex = writeIdx;
            m_writeIndex = (writeIdx + 1) % m_slots.size();
        }

        std::optional<FrameBufferSlot> AcquireReadFrame() {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_latestIndex == -1 || !m_slots[m_latestIndex].ready) {
                return std::nullopt;
            }

            // Zawsze pobieraj najnowszą klatkę (LIFO / Zero Latency)
            FrameBufferSlot result = m_slots[m_latestIndex];

            // Oznacz wszystkie klatki jako zużyte - nie chcemy czytać starych danych
            for (auto& slot : m_slots) {
                slot.ready = false;
            }
            m_latestIndex = -1;
            return result;
        }

        private:
        std::array<FrameBufferSlot, 3> m_slots;
        size_t m_writeIndex = 0;
        int m_latestIndex = -1;
        std::mutex m_mutex;
    };

    [[maybe_unused]] static std::optional<std::vector<uint8_t>> ReadPixelDataFromSharedFd(
        int fd,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        uint32_t offset,
        uint64_t planeSize,
        uint32_t pixelFormat,
        std::string_view desiredFormat) {
        if (fd < 0 || width == 0 || height == 0) {
            return std::nullopt;
        }

        size_t dataSize = planeSize ? static_cast<size_t>(planeSize)
            : static_cast<size_t>(stride) * static_cast<size_t>(height);
        if (dataSize == 0) {
            return std::nullopt;
        }

        size_t mapSize = planeSize ? static_cast<size_t>(planeSize)
            : dataSize + static_cast<size_t>(offset);
        MmapPtr mapped(mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0), MmapDeleter{ mapSize });
        if (!mapped || mapped.get() == MAP_FAILED) {
            return std::nullopt;
        }

        std::vector<uint8_t> buffer(dataSize);
        memcpy(buffer.data(), static_cast<uint8_t*>(mapped.get()) + static_cast<size_t>(offset), dataSize);

        std::string format = std::string(desiredFormat);
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        uint32_t actualStride = stride ? stride : static_cast<uint32_t>(width * 4);
        return ConvertPixelBuffer(
            std::span<const uint8_t>(buffer.data(), buffer.size()),
            width,
            height,
            actualStride,
            pixelFormat,
            format);
    }

    static std::optional<std::vector<uint8_t>> ReadPixelDataFromRawPointer(
        std::span<const uint8_t> data,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        uint64_t planeSize,
        uint32_t pixelFormat,
        std::string_view desiredFormat) {
        if (data.empty() || width == 0 || height == 0) {
            return std::nullopt;
        }

        const size_t rowBytes = static_cast<size_t>(width) * 4;
        uint32_t actualStride = stride ? stride : static_cast<uint32_t>(rowBytes);
        if (static_cast<size_t>(actualStride) < rowBytes) {
            return std::nullopt;
        }

        size_t expectedSize = planeSize ? static_cast<size_t>(planeSize)
            : static_cast<size_t>(actualStride) * static_cast<size_t>(height);
        if (expectedSize == 0 || expectedSize > data.size()) {
            return std::nullopt;
        }

        std::vector<uint8_t> buffer(expectedSize);

        if (actualStride == rowBytes) {
            std::memcpy(buffer.data(), data.data(), expectedSize);
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                size_t srcOffset = static_cast<size_t>(row) * actualStride;
                if (srcOffset + rowBytes > data.size()) {
                    return std::nullopt;
                }
                const uint8_t* srcRow = data.data() + srcOffset;
                uint8_t* dstRow = buffer.data() + static_cast<size_t>(row) * rowBytes;
                std::memcpy(dstRow, srcRow, rowBytes);
            }
        }

        std::string sourceFormat = PixelFormatToString(pixelFormat);
        std::string normalizedDesired = std::string(desiredFormat);
        std::transform(normalizedDesired.begin(), normalizedDesired.end(), normalizedDesired.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        if (normalizedDesired == sourceFormat) {
            if (actualStride == rowBytes) {
                return buffer;
            }
            std::vector<uint8_t> packedResult(rowBytes * static_cast<size_t>(height));
            for (uint32_t row = 0; row < height; ++row) {
                std::memcpy(
                    packedResult.data() + static_cast<size_t>(row) * rowBytes,
                    buffer.data() + static_cast<size_t>(row) * rowBytes,
                    rowBytes);
            }
            return packedResult;
        }

        return ConvertPixelBuffer(
            std::span<const uint8_t>(buffer.data(), buffer.size()),
            width,
            height,
            actualStride,
            pixelFormat,
            desiredFormat);
    }

    template <typename T, auto FreeFunc>
    struct GenericDeleter {
        void operator()(T* ptr) const { if (ptr) FreeFunc(ptr); }
    };

    struct GObjectDeleter {
        void operator()(void* ptr) const { if (ptr) g_object_unref(ptr); }
    };

    struct FdDeleter {
        void operator()(int* fd) const { if (fd && *fd >= 0) { close(*fd); delete fd; } }
    };

    struct DisplayDeleter {
        void operator()(Display* display) const {
            if (display) {
                XCloseDisplay(display);
            }
        }
    };

    using DisplayPtr = std::unique_ptr<Display, DisplayDeleter>;

    struct XImageDeleter {
        void operator()(XImage* image) const {
            if (image) {
                XDestroyImage(image);
            }
        }
    };

    using XImagePtr = std::unique_ptr<XImage, XImageDeleter>;

    struct XShmSegmentInfoWrapper {
        XShmSegmentInfo info{};
        Display* display = nullptr;
        bool attached = false;

        bool Attach(Display* display_) {
            display = display_;
            attached = XShmAttach(display, &info);
            return attached;
        }

        ~XShmSegmentInfoWrapper() {
            if (attached && display) {
                XShmDetach(display, &info);
            }
            if (info.shmaddr) {
                shmdt(info.shmaddr);
            }
            if (info.shmid >= 0) {
                shmctl(info.shmid, IPC_RMID, 0);
            }
        }
    };

    using GMainLoopPtr = std::unique_ptr<GMainLoop, GenericDeleter<GMainLoop, g_main_loop_unref>>;
    using GMainContextPtr = std::unique_ptr<GMainContext, GenericDeleter<GMainContext, g_main_context_unref>>;
    using GDBusConnectionPtr = std::unique_ptr<GDBusConnection, GObjectDeleter>;
    using PwThreadLoopPtr = std::unique_ptr<pw_thread_loop, GenericDeleter<pw_thread_loop, pw_thread_loop_destroy>>;
    using PwContextPtr = std::unique_ptr<pw_context, GenericDeleter<pw_context, pw_context_destroy>>;
    using PwCorePtr = std::unique_ptr<pw_core, GenericDeleter<pw_core, pw_core_disconnect>>;
    using PwStreamPtr = std::unique_ptr<pw_stream, GenericDeleter<pw_stream, pw_stream_destroy>>;
    using GVariantPtr = std::unique_ptr<GVariant, GenericDeleter<GVariant, g_variant_unref>>;
    using GErrorPtr = std::unique_ptr<GError, GenericDeleter<GError, g_error_free>>;
    using GUnixFDListPtr = std::unique_ptr<GUnixFDList, GObjectDeleter>;
    using UniqueFd = std::unique_ptr<int, FdDeleter>;

    struct PipeWireInitializer {
        bool initialized = false;

        void EnsureInit() {
            if (!initialized) {
                pw_init(nullptr, nullptr);
                initialized = true;
            }
        }

        ~PipeWireInitializer() {
            if (initialized) {
                pw_deinit();
            }
        }
    };

    struct GVariantBuilderWrapper {
        GVariantBuilder builder;
        GVariantBuilderWrapper() { g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}")); }
        ~GVariantBuilderWrapper() { g_variant_builder_clear(&builder); }
        operator GVariantBuilder* () { return &builder; }
    };

    struct StreamState {
        PwThreadLoopPtr pw_loop;
        PwContextPtr context;
        PwCorePtr core;
        PwStreamPtr stream;
        spa_hook stream_listener{};
    };

    enum class PortalStage {
        Idle,
        CreatingSession,
        SelectingSources,
        StartingSession,
        OpeningRemote,
    };

    struct MonitorInfo {
        uint32_t nodeId = PW_ID_ANY;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string connector;
        std::string title;
    };

} // namespace

struct StreamConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixelFormat = 0;
    uint64_t modifier = 0;
};

namespace Config {
    constexpr uint32_t DEFAULT_WIDTH = 1920;
    constexpr uint32_t DEFAULT_HEIGHT = 1080;
    constexpr uint32_t MAX_WIDTH = 8192;
    constexpr uint32_t MAX_HEIGHT = 8192;
    constexpr uint32_t DEFAULT_FPS_NUM = 60;
    constexpr uint32_t DEFAULT_FPS_DEN = 1;
    constexpr uint32_t MAX_FPS_NUM = 144;
    constexpr size_t POD_BUFFER_SIZE_CONNECT = 8192;
    constexpr size_t POD_BUFFER_SIZE_UPDATE = 4096;
}

class BaseLinuxPlatformCapture : public IPlatformCapture {
    protected:
    mutable std::shared_mutex m_stateMutex;
    std::jthread m_worker;
    std::atomic<bool> m_running{ false };
    std::mutex m_captureMutex;
    std::condition_variable m_captureCv;

    std::optional<SharedHandleInfo> m_sharedHandle;
    SharedFd m_sharedFd;
    mutable std::atomic<bool> m_frameConsumed{ false };

    std::function<void()> m_frameAvailableCallback;
    std::mutex m_frameCallbackMutex;
    std::function<void()> m_monitorChangedCallback;
    std::mutex m_monitorChangedCallbackMutex;

    std::mutex m_fpsMutex;
    std::atomic<int64_t> m_frameCount{ 0 };
    std::atomic<int> m_lastFps{ 0 };
    std::chrono::steady_clock::time_point m_lastFpsTime = std::chrono::steady_clock::now();

    public:
    virtual ~BaseLinuxPlatformCapture() = default;

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        if (!m_sharedHandle.has_value() || m_frameConsumed || !m_sharedFd || *m_sharedFd < 0) {
            return std::nullopt;
        }

        SharedHandleInfo info = *m_sharedHandle;
        info.handle = static_cast<uint64_t>(*m_sharedFd);
        m_frameConsumed = true;
        return info;
    }

    int GetFps() const override {
        return m_lastFps.load();
    }

    void SetFrameAvailableCallback(std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
        m_frameAvailableCallback = std::move(callback);
    }

    void SetMonitorChangedCallback(std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(m_monitorChangedCallbackMutex);
        m_monitorChangedCallback = std::move(callback);
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

    void InvokeMonitorChangedCallback() {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(m_monitorChangedCallbackMutex);
            callback = m_monitorChangedCallback;
        }
        if (callback) {
            callback();
        }
    }

    void RecordFrame() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_fpsMutex);
        m_frameCount.fetch_add(1, std::memory_order_relaxed);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastFpsTime).count();
        if (elapsed >= 1) {
            m_lastFps.store(static_cast<int>(m_frameCount.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            m_frameCount.store(0, std::memory_order_relaxed);
            m_lastFpsTime = now;
        }
        InvokeFrameAvailableCallback();
    }
};

class WaylandPlatformCapture final : public BaseLinuxPlatformCapture {
    public:
    WaylandPlatformCapture() = default;

    ~WaylandPlatformCapture() override {
        Stop();
    }

    void Start(Napi::Env) override {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) {
            return;
        }
        m_worker = std::jthread([this](std::stop_token stopToken) {
            RunCaptureFlow(stopToken);
            });
    }

    void Stop() override {
        if (m_worker.joinable()) {
            m_worker.request_stop();
        }

        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            if (m_glibLoop) {
                g_main_loop_quit(m_glibLoop.get());
                GMainContext* ctx = g_main_loop_get_context(m_glibLoop.get());
                if (ctx) g_main_context_wakeup(ctx);
            }
            if (m_streamState.pw_loop) {
                pw_thread_loop_stop(m_streamState.pw_loop.get());
                pw_thread_loop_signal(m_streamState.pw_loop.get(), false);
            }
        }

        m_captureCv.notify_all();

        if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
            m_worker.join();
        }

        CleanupSharedHandle();
        m_running.store(false);
    }

    std::optional<std::vector<uint8_t>> GetPixelData(std::string_view desiredFormat = "rgba") const override;
    int GetWidth() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_streamConfig ? static_cast<int>(m_streamConfig->width) : 0;
    }
    int GetHeight() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_streamConfig ? static_cast<int>(m_streamConfig->height) : 0;
    }
    int GetStride() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return static_cast<int>(m_stride);
    }
    uint32_t GetPixelFormat() const override {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        return m_streamConfig ? m_streamConfig->pixelFormat : 0;
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        if (!m_sharedHandle.has_value() || m_frameConsumed || !m_sharedFd || *m_sharedFd < 0) {
            return std::nullopt;
        }

        if (m_bufferType == SPA_DATA_MemFd) {
            return std::nullopt;
        }

        SharedHandleInfo info = *m_sharedHandle;
        info.handle = static_cast<uint64_t>(*m_sharedFd);
        m_frameConsumed = true;
        return info;
    }

    std::string GetBackendName() const override {
        return "wayland";
    }

    void SetExternalPortalSession(const std::optional<std::string>& sessionHandle, std::optional<int> pipewireRemoteFd) override {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        m_externalSessionHandle = sessionHandle;
        m_externalPipewireFd = pipewireRemoteFd;
    }

    int GetMonitorCount() const override;
        std::vector<MonitorMetadata> GetMonitors() const override;
    int GetCurrentMonitorIndex() const override;
    void NextMonitor() override;
    void SelectMonitor(int index) override;
    std::optional<MonitorMetadata> GetCurrentMonitorInfo() const override;

    private:

    GMainLoopPtr m_glibLoop;
    GDBusConnectionPtr m_connection;

    StreamState m_streamState{};
    std::atomic<PortalStage> m_stage{ PortalStage::Idle };

    std::string m_sessionHandle;
    std::optional<std::string> m_externalSessionHandle;
    std::optional<int> m_externalPipewireFd;
    std::optional<StreamConfig> m_streamConfig;
    std::atomic<uint32_t> m_streamNodeId{ PW_ID_ANY };
    std::vector<MonitorInfo> m_monitors;
    std::atomic<int> m_currentMonitorIndex{ 0 };
    std::atomic<int> m_requestedMonitorIndex{ -1 };
    uint32_t m_stride = 0;
    uint32_t m_offset = 0;
    uint64_t m_planeSize = 0;
    uint32_t m_bufferType = 0;
    uint32_t m_chunkSize = 0;
    bool m_loggedNonDmabuf = false;
    std::chrono::steady_clock::time_point m_lastMemFdLogTime = std::chrono::steady_clock::time_point::min();

    std::atomic<int> m_pendingPipewireFd{ -1 };
    mutable FrameBufferPool m_frameBuffers;
    mutable std::unordered_map<int, MmapPtr> m_mappingCache;
    std::unordered_map<int, SharedFd> m_fdCache;
    mutable std::vector<uint8_t> m_pixelCache;
    mutable std::string m_cacheFormat;

    void RunCaptureFlow(std::stop_token stopToken) {
        int pipewireFd = -1;
        GMainContextPtr context;

        try {
            static PipeWireInitializer pipewireInitializer;
            pipewireInitializer.EnsureInit();

            context.reset(g_main_context_new());
            g_main_context_push_thread_default(context.get());

            {
                std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                m_glibLoop.reset(g_main_loop_new(context.get(), FALSE));
            }

            GError* rawAddressError = nullptr;
            std::unique_ptr<gchar, GenericDeleter<gchar, g_free>> address(
                g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &rawAddressError)
            );
            GErrorPtr dbusAddressError(rawAddressError);

            if (!address) {
                std::string msg = dbusAddressError ? dbusAddressError->message : "Unknown error";
                throw std::runtime_error("Unable to get session D-Bus address: " + msg);
            }

            GError* rawConnError = nullptr;
            m_connection.reset(g_dbus_connection_new_for_address_sync(
                address.get(),
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION | G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
                nullptr,
                nullptr,
                &rawConnError));
            GErrorPtr dbusConnError(rawConnError);

            if (!m_connection) {
                std::string msg = dbusConnError ? dbusConnError->message : "Unknown DBus error";
                throw std::runtime_error("Unable to connect to private session D-Bus: " + msg);
            }

            g_dbus_connection_signal_subscribe(
                m_connection.get(),
                "org.freedesktop.portal.Desktop",
                "org.freedesktop.portal.Request",
                "Response",
                nullptr,
                nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                &WaylandPlatformCapture::OnPortalResponse,
                this,
                nullptr);

            bool shouldRunPortalFlow = true;
            {
                std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                if (m_externalPipewireFd && *m_externalPipewireFd >= 0) {
                    pipewireFd = *m_externalPipewireFd;
                    shouldRunPortalFlow = false;
                    sc_logger::Info("Wayland: using external PipeWire FD from another addon");
                } else if (m_externalSessionHandle && !m_externalSessionHandle->empty()) {
                    m_sessionHandle = *m_externalSessionHandle;
                    try {
                        sc_logger::Info("Wayland: reusing external portal session handle");
                        OpenPipeWireRemote();
                        shouldRunPortalFlow = false;
                    } catch (const std::exception& e) {
                        sc_logger::Warn("Wayland: external portal session failed ({}), creating own session", e.what());
                        m_sessionHandle.clear();
                    }
                }
            }

            if (shouldRunPortalFlow) {
                GVariantBuilderWrapper builder;
                g_variant_builder_add(builder, "{sv}", "session_handle_token", g_variant_new_string(("s" + gen_token()).c_str()));
                g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string(("t" + gen_token()).c_str()));

                m_stage = PortalStage::CreatingSession;
                CallPortalMethod("CreateSession", g_variant_new("(a{sv})", static_cast<GVariantBuilder*>(builder)));
                g_main_loop_run(m_glibLoop.get());
            }

            if (stopToken.stop_requested()) {
                throw std::runtime_error("Capture stopped before PipeWire remote was opened");
            }

            if (pipewireFd < 0) {
                pipewireFd = m_pendingPipewireFd.exchange(-1);
            }

            if (pipewireFd < 0) {
                throw std::runtime_error("PipeWire file descriptor was not received from the portal");
            }

            StartPipewireStream(pipewireFd);

            while (!stopToken.stop_requested()) {
                int requestedIndex = -1;
                {
                    std::unique_lock<std::mutex> waitLock(m_captureMutex);
                    m_captureCv.wait(waitLock, [&stopToken, this] {
                        return stopToken.stop_requested() || m_requestedMonitorIndex.load() >= 0;
                        });
                    if (stopToken.stop_requested()) {
                        break;
                    }
                    requestedIndex = m_requestedMonitorIndex.exchange(-1);
                }

                if (requestedIndex >= 0) {
                    {
                        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                        if (requestedIndex >= 0 && requestedIndex < static_cast<int>(m_monitors.size())) {
                            sc_logger::Info("Wayland: Switching monitor to index {}", requestedIndex);
                            m_currentMonitorIndex.store(requestedIndex);
                            m_streamNodeId.store(m_monitors[requestedIndex].nodeId);
                            StopCurrentPipewireStream();
                            CleanupSharedHandleLocked();
                            CreatePipewireStream(m_streamNodeId.load());
                            
                            lock.unlock();
                            InvokeMonitorChangedCallback();
                        }
                    }
                }
            }

        } catch (const std::exception& e) {
            sc_logger::Error("Linux capture error: {}", e.what());
            if (pipewireFd >= 0) close(pipewireFd);
        }

        CleanupPortal();
        CleanupPipewire();

        if (context) {
            g_main_context_pop_thread_default(context.get());
            context.reset();
        }

        m_glibLoop.reset();

        m_running.store(false);
    }

    void CallPortalMethod(const char* method, GVariant* params) {
        GError* rawError = nullptr;
        GVariantPtr result(g_dbus_connection_call_sync(
            m_connection.get(),
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            method,
            params,
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &rawError));

        GErrorPtr error(rawError);

        if (!result) {
            std::string message = error ? error->message : "Unknown portal error";
            throw std::runtime_error(message);
        }
    }

    void StopCurrentPipewireStream() {
        if (!m_streamState.stream) return;

        if (m_streamState.pw_loop) {
            pw_thread_loop* loop = m_streamState.pw_loop.get();
            pw_thread_loop_lock(loop);
            if (m_streamState.stream) {
                pw_stream* raw = m_streamState.stream.release();
                if (raw) pw_stream_destroy(raw);
            }
            pw_thread_loop_unlock(loop);
        } else {
            m_streamState.stream.reset();
        }
    }

    void RecreatePipewireStream(uint32_t targetNodeId) {
        std::lock_guard<std::shared_mutex> lock(m_stateMutex);
        StopCurrentPipewireStream();
        CleanupSharedHandleLocked();
        CreatePipewireStream(targetNodeId);
    }

    void CreatePipewireStream(uint32_t targetNodeId) {
        struct LoopLockGuard {
            pw_thread_loop* loop;
            explicit LoopLockGuard(pw_thread_loop* l) : loop(l) { if (loop) pw_thread_loop_lock(loop); }
            ~LoopLockGuard() { if (loop) pw_thread_loop_unlock(loop); }
        };

        LoopLockGuard pwLock(m_streamState.pw_loop.get());

        m_streamState.stream.reset(pw_stream_new(
            m_streamState.core.get(),
            "electron-capture",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Video",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Screen",
                nullptr)));

        if (!m_streamState.stream) {
            throw std::runtime_error("Unable to create the PipeWire stream");
        }

        uint8_t buffer[Config::POD_BUFFER_SIZE_CONNECT];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[3];

        const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        const spa_rectangle defaultSize = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);
        const spa_fraction minFramerate = SPA_FRACTION(0, 1);
        const spa_fraction defaultFramerate = SPA_FRACTION(60, 1);
        const spa_fraction maxFramerate = SPA_FRACTION(144, 1);

        bool forceMemFd = IsNvidiaGPU();

        if (forceMemFd) {
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format,
                SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType,
                SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,
                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,
                SPA_POD_CHOICE_ENUM_Id(6, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_xRGB),
                SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
                SPA_FORMAT_VIDEO_framerate,
                SPA_POD_CHOICE_RANGE_Fraction(&defaultFramerate, &minFramerate, &maxFramerate)));
        } else {
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format,
                SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType,
                SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,
                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,
                SPA_POD_CHOICE_ENUM_Id(6, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_xRGB),
                SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
                SPA_FORMAT_VIDEO_framerate,
                SPA_POD_CHOICE_RANGE_Fraction(&defaultFramerate, &minFramerate, &maxFramerate),
                SPA_FORMAT_VIDEO_modifier,
                SPA_POD_CHOICE_ENUM_Long(3, 0ULL, 0ULL, 0x00ffffffffffffffULL)));
        }

        if (forceMemFd) {
            params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers,
                SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
        } else {
            params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers,
                SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd))));
        }

        params[2] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamMeta,
            SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region))));

        if (!params[0] || !params[1] || !params[2]) {
            sc_logger::Error("Failed to build PW connection params (format={}, buffers={}, meta={})",
                static_cast<const void*>(params[0]), static_cast<const void*>(params[1]), static_cast<const void*>(params[2]));
            throw std::runtime_error("PipeWire POD buffer overflow during stream connection initialization");
        }

        pw_stream_add_listener(
            m_streamState.stream.get(),
            &m_streamState.stream_listener,
            &kStreamEvents,
            this);

        uint32_t targetId = targetNodeId == PW_ID_ANY ? PW_ID_ANY : targetNodeId;
        int result = pw_stream_connect(
            m_streamState.stream.get(),
            PW_DIRECTION_INPUT,
            targetId,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params,
            3);

        if (result < 0) {
            throw std::runtime_error(std::string("pw_stream_connect failed: ") + spa_strerror(result));
        }
    }

    void StartSession() {
        GVariantBuilderWrapper builder;
        m_stage = PortalStage::StartingSession;
        std::string handle;
        {
            std::shared_lock<std::shared_mutex> lock(m_stateMutex);
            handle = m_sessionHandle;
        }
        CallPortalMethod("Start", g_variant_new("(osa{sv})", handle.c_str(), "", static_cast<GVariantBuilder*>(builder)));
    }

    void SelectSources() {
        GVariantBuilderWrapper builder;
        g_variant_builder_add(builder, "{sv}", "types", g_variant_new_uint32(1));
        g_variant_builder_add(builder, "{sv}", "multiple", g_variant_new_boolean(TRUE));
        g_variant_builder_add(builder, "{sv}", "cursor_mode", g_variant_new_uint32(1));

        m_stage = PortalStage::SelectingSources;
        std::string handle;
        {
            std::shared_lock<std::shared_mutex> lock(m_stateMutex);
            handle = m_sessionHandle;
        }
        CallPortalMethod("SelectSources", g_variant_new("(oa{sv})", handle.c_str(), static_cast<GVariantBuilder*>(builder)));
    }

    void OpenPipeWireRemote() {
        GVariantBuilderWrapper builder;
        GError* rawResultError = nullptr;
        GUnixFDList* rawOutFdList = nullptr;

        std::string handle;
        {
            std::shared_lock<std::shared_mutex> lock(m_stateMutex);
            handle = m_sessionHandle;
        }

        GVariantPtr result(g_dbus_connection_call_with_unix_fd_list_sync(
            m_connection.get(),
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote",
            g_variant_new("(oa{sv})", handle.c_str(), static_cast<GVariantBuilder*>(builder)),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &rawOutFdList,
            nullptr,
            &rawResultError));

        GErrorPtr resultError(rawResultError);
        GUnixFDListPtr outFdList(rawOutFdList);

        if (!result) {
            std::string message = resultError ? resultError->message : "Unknown portal error";
            throw std::runtime_error(message);
        }

        gint32 fdIndex = -1;
        g_variant_get(result.get(), "(h)", &fdIndex);

        GError* rawFdError = nullptr;
        int fd = g_unix_fd_list_get(outFdList.get(), fdIndex, &rawFdError);
        GErrorPtr fdError(rawFdError);

        if (fd < 0) {
            std::string message = fdError ? fdError->message : "Unable to extract PipeWire FD";
            throw std::runtime_error(message);
        }

        m_pendingPipewireFd.store(fd);

        m_stage = PortalStage::OpeningRemote;
        g_main_loop_quit(m_glibLoop.get());
        GMainContext* ctx = g_main_loop_get_context(m_glibLoop.get());
        if (ctx) g_main_context_wakeup(ctx);
    }

    void StartPipewireStream(int& pipewireFd) {
        m_streamState.pw_loop.reset(pw_thread_loop_new("pw-capt", nullptr));
        if (!m_streamState.pw_loop) {
            throw std::runtime_error("Unable to create the PipeWire main loop");
        }

        pw_loop* loop = pw_thread_loop_get_loop(m_streamState.pw_loop.get());

        pw_thread_loop_lock(m_streamState.pw_loop.get());

        m_streamState.context.reset(pw_context_new(loop, nullptr, 0));
        if (!m_streamState.context) {
            pw_thread_loop_unlock(m_streamState.pw_loop.get());
            throw std::runtime_error("Unable to create the PipeWire context");
        }

        m_streamState.core.reset(pw_context_connect_fd(m_streamState.context.get(), std::exchange(pipewireFd, -1), nullptr, 0));
        if (!m_streamState.core) {
            pw_thread_loop_unlock(m_streamState.pw_loop.get());
            throw std::runtime_error("Unable to connect to the PipeWire core through the portal FD");
        }

        pw_thread_loop_unlock(m_streamState.pw_loop.get());

        CreatePipewireStream(m_streamNodeId.load());

        if (m_streamState.pw_loop) {
            pw_thread_loop_start(m_streamState.pw_loop.get());
        }
    }

    void CleanupPortal() {
        m_connection.reset();

        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            m_sessionHandle.clear();
            m_glibLoop.reset();
            m_monitors.clear();
            m_currentMonitorIndex = 0;
            m_requestedMonitorIndex = -1;
        }

        m_streamNodeId = PW_ID_ANY;
        m_stage = PortalStage::Idle;
    }

    void CleanupPipewire() {
        // Ensure destruction of PipeWire objects happens on the PipeWire thread-loop
        if (m_streamState.pw_loop) {
            pw_thread_loop* loop = m_streamState.pw_loop.get();
            pw_thread_loop_lock(loop);
            if (m_streamState.stream) {
                pw_stream* raw = m_streamState.stream.release();
                if (raw) pw_stream_destroy(raw);
            }
            if (m_streamState.core) {
                pw_core* raw = m_streamState.core.release();
                if (raw) pw_core_disconnect(raw);
            }
            if (m_streamState.context) {
                pw_context* raw = m_streamState.context.release();
                if (raw) pw_context_destroy(raw);
            }
            pw_thread_loop_unlock(loop);

            // Stop and destroy the thread loop
            pw_thread_loop_stop(loop);
            pw_thread_loop_signal(loop, false);
            m_streamState.pw_loop.reset();
        } else {
            m_streamState.stream.reset();
            m_streamState.core.reset();
            m_streamState.context.reset();
        }

        CleanupSharedHandle();
    }

    void CleanupSharedHandleLocked() {
        m_sharedHandle.reset();
        m_sharedFd.reset();
        m_streamConfig.reset();
        m_stride = 0;
        m_offset = 0;
        m_planeSize = 0;
        m_bufferType = 0;
        m_chunkSize = 0;
        m_frameBuffers.Reset();
        m_mappingCache.clear();
        m_fdCache.clear();
        m_pixelCache.clear();
        m_cacheFormat.clear();
        m_loggedNonDmabuf = false;
        m_frameConsumed = false;
    }

    void CleanupSharedHandle() {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        CleanupSharedHandleLocked();
    }

    void PublishSharedHandleLocked() {
        if (!m_sharedFd || *m_sharedFd < 0 || !m_streamConfig || m_streamConfig->width == 0 || m_streamConfig->height == 0) {
            return;
        }

        m_sharedHandle = SharedHandleInfo{
            static_cast<uint64_t>(*m_sharedFd),
            m_streamConfig->width,
            m_streamConfig->height,
            m_stride,
            m_offset,
            m_planeSize,
            m_streamConfig->pixelFormat,
            m_streamConfig->modifier,
            m_bufferType,
            m_chunkSize,
        };
    }

    void UpdateSharedHandleFromFd(int fd, std::vector<IntRect> damage, bool fullUpdate) {
        SharedFd sfd;
        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            auto it = m_fdCache.find(fd);
            if (it != m_fdCache.end()) {
                sfd = it->second;
            } else {
                int ownedFd = dup(fd);
                if (ownedFd >= 0) {
                    sfd = SharedFd(new int(ownedFd), FdDeleter());
                    m_fdCache[fd] = sfd;
                }
            }
        }

        if (!sfd) return;

        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        m_sharedFd = sfd;
        m_frameConsumed = false;

        std::optional<SharedHandleInfo> handle;
        if (m_streamConfig) {
            uint32_t stride = m_stride ? m_stride : static_cast<uint32_t>(m_streamConfig->width) * 4;
            handle = SharedHandleInfo{
                static_cast<uint64_t>(*sfd),
                m_streamConfig->width,
                m_streamConfig->height,
                stride,
                m_offset,
                m_planeSize,
                m_streamConfig->pixelFormat,
                m_streamConfig->modifier,
                m_bufferType,
                m_chunkSize,
            };
            m_sharedHandle = handle;
        }

        // Push the same shared pointer to the pool - no extra dups needed
        m_frameBuffers.PushFrame(sfd, handle, std::move(damage), fullUpdate);
        PublishSharedHandleLocked();
    }

    static void OnStreamStateChanged(void*, pw_stream_state oldState, pw_stream_state state, const char* error) {
        if (state == PW_STREAM_STATE_ERROR && error) {
            sc_logger::Error("PipeWire stream error: {}", error);
        }
    }

    static void OnStreamParamChanged(void* data, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<WaylandPlatformCapture*>(data);
        if (!param || id != SPA_PARAM_Format) {
            return;
        }

        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) {
            return;
        }

        bool forceMemFd = IsNvidiaGPU();
        bool hasModifier = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;

        sc_logger::Info("Chosen stream format: {} ({}) , size={}x{} , modifier={} , forceMemFd={}",
            PixelFormatToString(info.format),
            static_cast<uint32_t>(info.format),
            info.size.width,
            info.size.height,
            (hasModifier ? std::to_string(info.modifier) : std::string("none")),
            (forceMemFd ? "yes" : "no"));

        {
            std::lock_guard<std::shared_mutex> lock(self->m_stateMutex);
            if (!self->m_streamConfig) {
                self->m_streamConfig = StreamConfig{};
            }
            self->m_streamConfig->width = info.size.width;
            self->m_streamConfig->height = info.size.height;
            self->m_streamConfig->pixelFormat = static_cast<uint32_t>(info.format);
            self->m_streamConfig->modifier = hasModifier ? info.modifier : 0;
            self->PublishSharedHandleLocked();
        }

        uint8_t buffer[Config::POD_BUFFER_SIZE_UPDATE];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[3];

        if (hasModifier && !forceMemFd) {
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format,
                SPA_PARAM_Format,
                SPA_FORMAT_mediaType,
                SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,
                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,
                SPA_POD_Id(info.format),
                SPA_FORMAT_VIDEO_size,
                SPA_POD_Rectangle(&info.size),
                SPA_FORMAT_VIDEO_framerate,
                SPA_POD_Fraction(&info.framerate),
                SPA_FORMAT_VIDEO_modifier,
                SPA_POD_Long(info.modifier)));
        } else {
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format,
                SPA_PARAM_Format,
                SPA_FORMAT_mediaType,
                SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,
                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,
                SPA_POD_Id(info.format),
                SPA_FORMAT_VIDEO_size,
                SPA_POD_Rectangle(&info.size),
                SPA_FORMAT_VIDEO_framerate,
                SPA_POD_Fraction(&info.framerate)));
        }

        if (forceMemFd) {
            params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers,
                SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
        } else {
            params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers,
                SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd))));
        }

        // Ensure metadata request is preserved during parameter updates
        params[2] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamMeta,
            SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region))));

        if (!params[2]) {
            sc_logger::Warn("Failed to build meta update param (possible buffer overflow). Damage tracking disabled.");
            // without meta - stream will continue but without dirty rects
            pw_stream_update_params(self->m_streamState.stream.get(), params, 2);
        } else {
            pw_stream_update_params(self->m_streamState.stream.get(), params, 3);
        }
    }

    static void OnStreamProcess(void* userdata) {
        auto* self = static_cast<WaylandPlatformCapture*>(userdata);
        if (!self->m_streamState.stream) {
            return;
        }

        pw_buffer* buffer = pw_stream_dequeue_buffer(self->m_streamState.stream.get());
        if (!buffer) {
            return;
        }

        spa_buffer* spaBuffer = buffer->buffer;
        if (spaBuffer && spaBuffer->n_datas > 0) {
            spa_data& data = spaBuffer->datas[0];

            std::vector<IntRect> damageRects;
            bool fullUpdate = true;

            auto* meta = spa_buffer_find_meta(spaBuffer, SPA_META_VideoDamage);

            if (meta) {
                sc_logger::Debug("Wayland: Damage meta found");
                const spa_meta_region* damage =
                    (const spa_meta_region*)meta;
                const spa_region& r = damage[0].region;

                if (r.size.width == 0 || r.size.height == 0) {
                    fullUpdate = true;
                } else {
                    fullUpdate = false;
                    damageRects.push_back({
                        (uint32_t)r.position.x,
                        (uint32_t)r.position.y,
                        (uint32_t)r.size.width,
                        (uint32_t)r.size.height
                        });
                }
            } else {
                static std::once_flag noMetaWarning;
                std::call_once(noMetaWarning, []() {
                    sc_logger::Warn("Compositor does not provide VideoDamage meta – all frames will be full updates.");
                    });
                fullUpdate = true;
            }

            const uint32_t chunkSize = data.chunk ? data.chunk->size : 0;
            if ((data.type == SPA_DATA_DmaBuf || data.type == SPA_DATA_MemFd) && data.fd >= 0) {
                const auto now = std::chrono::steady_clock::now();
                bool shouldLogDmaBuf = false;
                bool shouldLogMemFd = false;
                {
                    std::lock_guard<std::shared_mutex> lock(self->m_stateMutex);
                    const uint32_t previousType = self->m_bufferType;
                    self->m_bufferType = static_cast<uint32_t>(data.type);
                    self->m_chunkSize = chunkSize;
                    if (data.chunk) {
                        self->m_stride = data.chunk->stride;
                        self->m_offset = data.chunk->offset;
                        self->m_planeSize = data.chunk->size;
                    } else {
                        self->m_stride = 0;
                        self->m_offset = 0;
                        self->m_planeSize = data.maxsize;
                    }

                    if (data.type == SPA_DATA_DmaBuf) {
                        shouldLogDmaBuf = previousType != SPA_DATA_DmaBuf;
                    } else {
                        shouldLogMemFd = previousType != SPA_DATA_MemFd
                            || (now - self->m_lastMemFdLogTime >= std::chrono::seconds(1));
                        if (shouldLogMemFd) {
                            self->m_lastMemFdLogTime = now;
                        }
                    }
                }

                if (shouldLogDmaBuf) {
                    sc_logger::Info("Using DMA-BUF zero-copy buffer");
                } else if (shouldLogMemFd) {
                    sc_logger::Info("Using MemFd CPU-copy buffer, common for NVIDIA Wayland");
                }

                self->UpdateSharedHandleFromFd(data.fd, std::move(damageRects), fullUpdate);
                self->m_loggedNonDmabuf = false;
            } else {
                std::lock_guard<std::shared_mutex> lock(self->m_stateMutex);
                self->m_bufferType = static_cast<uint32_t>(data.type);
                self->m_chunkSize = chunkSize;
            }

            if (data.type != SPA_DATA_DmaBuf && data.type != SPA_DATA_MemFd && !self->m_loggedNonDmabuf) {
                self->m_loggedNonDmabuf = true;
                sc_logger::Warn("Non-DMA buffer type received: {}", data.type);
            }

            self->RecordFrame();
        }

        pw_stream_queue_buffer(self->m_streamState.stream.get(), buffer);
    }

    static void OnPortalResponse(
        GDBusConnection*,
        const gchar*,
        const gchar*,
        const gchar*,
        const gchar*,
        GVariant* parameters,
        gpointer userData) {
        auto* self = static_cast<WaylandPlatformCapture*>(userData);

        guint32 responseCode = 1;
        GVariantIter* results = nullptr;
        g_variant_get(parameters, "(ua{sv})", &responseCode, &results);

        auto freeResults = [&results]() {
            if (results) {
                g_variant_iter_free(results);
                results = nullptr;
            }
            };

        if (responseCode != 0) {
            freeResults();
            if (self->m_glibLoop) {
                g_main_loop_quit(self->m_glibLoop.get());
                GMainContext* ctx = g_main_loop_get_context(self->m_glibLoop.get());
                if (ctx) g_main_context_wakeup(ctx);
            }
            return;
        }

        try {
            if (self->m_stage == PortalStage::CreatingSession) {
                const gchar* key = nullptr;
                GVariant* value = nullptr;
                bool foundSession = false;

                while (results && g_variant_iter_next(results, "{sv}", &key, &value)) {
                    if (g_strcmp0(key, "session_handle") == 0) {
                        std::unique_lock<std::shared_mutex> lock(self->m_stateMutex);
                        self->m_sessionHandle = g_variant_get_string(value, nullptr);
                        foundSession = true;
                    }
                    g_variant_unref(value);
                }
                freeResults();

                if (!foundSession || self->m_sessionHandle.empty()) {
                    throw std::runtime_error("CreateSession response did not include a session handle");
                }
                self->SelectSources();
                return;
            }

            if (self->m_stage == PortalStage::SelectingSources) {
                freeResults();
                self->StartSession();
                return;
            }

            if (self->m_stage == PortalStage::StartingSession) {
                const gchar* key = nullptr;
                GVariant* value = nullptr;
                std::vector<MonitorInfo> monitors;

                while (results && g_variant_iter_next(results, "{sv}", &key, &value)) {
                    if (g_strcmp0(key, "streams") == 0 && g_variant_is_of_type(value, G_VARIANT_TYPE("a(ua{sv})"))) {
                        GVariantIter streamIter;
                        g_variant_iter_init(&streamIter, value);
                        GVariant* streamTuple = nullptr;

                        while ((streamTuple = g_variant_iter_next_value(&streamIter)) != nullptr) {
                            uint32_t nodeId = PW_ID_ANY;
                            GVariant* props = nullptr;
                            g_variant_get(streamTuple, "(u@a{sv})", &nodeId, &props);

                            MonitorInfo monitor;
                            monitor.nodeId = nodeId;

                            if (props) {
                                GVariantIter propIter;
                                g_variant_iter_init(&propIter, props);
                                const gchar* propKey = nullptr;
                                GVariant* propVal = nullptr;

                                while (g_variant_iter_next(&propIter, "{sv}", &propKey, &propVal)) {
                                    if (g_strcmp0(propKey, "size") == 0 && g_variant_is_of_type(propVal, G_VARIANT_TYPE("(ii)"))) {
                                        int32_t width = 0;
                                        int32_t height = 0;
                                        g_variant_get(propVal, "(ii)", &width, &height);
                                        monitor.width = static_cast<uint32_t>(width);
                                        monitor.height = static_cast<uint32_t>(height);
                                    } else if (g_strcmp0(propKey, "position") == 0 && g_variant_is_of_type(propVal, G_VARIANT_TYPE("(ii)"))) {
                                        int32_t x = 0;
                                        int32_t y = 0;
                                        g_variant_get(propVal, "(ii)", &x, &y);
                                        monitor.x = x;
                                        monitor.y = y;
                                    } else if (g_strcmp0(propKey, "connector") == 0 && g_variant_is_of_type(propVal, G_VARIANT_TYPE("s"))) {
                                        monitor.connector = g_variant_get_string(propVal, nullptr);
                                    } else if (g_strcmp0(propKey, "title") == 0 && g_variant_is_of_type(propVal, G_VARIANT_TYPE("s"))) {
                                        monitor.title = g_variant_get_string(propVal, nullptr);
                                    }
                                    g_variant_unref(propVal);
                                }
                                g_variant_unref(props);
                            }

                            monitors.push_back(std::move(monitor));
                            g_variant_unref(streamTuple);
                        }
                    }
                    g_variant_unref(value);
                }
                freeResults();

                if (monitors.empty()) {
                    throw std::runtime_error("Start response did not include any PipeWire streams");
                }

                {
                    std::lock_guard<std::shared_mutex> lock(self->m_stateMutex);
                    self->m_monitors = std::move(monitors);
                    self->m_currentMonitorIndex = 0;
                    self->m_streamNodeId = self->m_monitors[0].nodeId;
                }

                self->InvokeMonitorChangedCallback();

                sc_logger::Info("Wayland capture selected {} monitor(s)", static_cast<int>(self->m_monitors.size()));
                self->OpenPipeWireRemote();
                return;
            }

            freeResults();
            if (self->m_glibLoop) {
                g_main_loop_quit(self->m_glibLoop.get());
                GMainContext* ctx = g_main_loop_get_context(self->m_glibLoop.get());
                if (ctx) g_main_context_wakeup(ctx);
            }
        } catch (const std::exception& e) {
            freeResults();
            sc_logger::Error("Portal error: {}", e.what());
            if (self->m_glibLoop) {
                g_main_loop_quit(self->m_glibLoop.get());
                GMainContext* ctx = g_main_loop_get_context(self->m_glibLoop.get());
                if (ctx) g_main_context_wakeup(ctx);
            }
        }
    }

    static const pw_stream_events kStreamEvents;
};

int WaylandPlatformCapture::GetMonitorCount() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return static_cast<int>(m_monitors.size());
}

std::vector<MonitorMetadata> WaylandPlatformCapture::GetMonitors() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    std::vector<MonitorMetadata> monitors;
    monitors.reserve(m_monitors.size());
    
    for (size_t i = 0; i < m_monitors.size(); ++i) {
        const auto& m = m_monitors[i];
        MonitorMetadata info;
        info.id = std::to_string(m.nodeId);
        info.name = !m.title.empty() ? m.title : m.connector;
        info.index = static_cast<int>(i);
        info.x = m.x;
        info.y = m.y;
        info.width = static_cast<int>(m.width);
        info.height = static_cast<int>(m.height);
        if (m.nodeId != PW_ID_ANY) {
            info.pipewireStream = m.nodeId;
        }
        monitors.push_back(std::move(info));
    }
    
    return monitors;
}

int WaylandPlatformCapture::GetCurrentMonitorIndex() const {
    return m_currentMonitorIndex.load();
}

void WaylandPlatformCapture::NextMonitor() {
    size_t count = 0;
    {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        count = m_monitors.size();
    }

    if (count <= 1) {
        return;
    }

    int currentIdx = m_currentMonitorIndex.load();
    int nextIndex = (currentIdx + 1) % static_cast<int>(count);
    m_requestedMonitorIndex.store(nextIndex);
    m_captureCv.notify_all();
}

void WaylandPlatformCapture::SelectMonitor(int index) {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    if (index < 0 || index >= static_cast<int>(m_monitors.size())) {
        return;
    }
    if (index != m_currentMonitorIndex.load()) {
        m_requestedMonitorIndex.store(index);
        m_captureCv.notify_all();
    }
}

std::optional<MonitorMetadata> WaylandPlatformCapture::GetCurrentMonitorInfo() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    const int idx = m_currentMonitorIndex.load();
    if (idx < 0 || idx >= static_cast<int>(m_monitors.size())) {
        return std::nullopt;
    }

    const auto& monitor = m_monitors[idx];
    MonitorMetadata info;
    info.id = std::to_string(monitor.nodeId);
    info.name = !monitor.title.empty() ? monitor.title : monitor.connector;
    info.index = idx;
    info.x = monitor.x;
    info.y = monitor.y;
    info.width = static_cast<int>(monitor.width);
    info.height = static_cast<int>(monitor.height);
    if (monitor.nodeId != PW_ID_ANY) {
        info.pipewireStream = monitor.nodeId;
    }
    return info;
}

const pw_stream_events WaylandPlatformCapture::kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = WaylandPlatformCapture::OnStreamStateChanged;
    events.param_changed = WaylandPlatformCapture::OnStreamParamChanged;
    events.process = WaylandPlatformCapture::OnStreamProcess;
    return events;
    }();


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

std::optional<std::vector<uint8_t>> WaylandPlatformCapture::GetPixelData(std::string_view desiredFormat) const {
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
        sc_logger::Warn("Wayland invalid offset: {} >= mapSize {}", handle.offset, mapSize);
        return std::nullopt;
    }

    size_t available = mapSize - static_cast<size_t>(handle.offset);
    if (dataSize > available) {
        sc_logger::Warn("Wayland dataSize {} exceeds available {}", dataSize, available);
        return std::nullopt;
    }

    sc_logger::Debug("Wayland frame: fd={} width={} height={} stride={} offset={} planeSize={} dataSize={} mapSize={} available={}",
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
        int fdVal = *frame->fd;
        auto it = m_mappingCache.find(fdVal);
        if (it != m_mappingCache.end()) {
            localMapping = it->second;
        } else {
            void* ptr = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fdVal, 0);
            if (ptr != MAP_FAILED) {
                localMapping = MmapPtr(ptr, MmapDeleter{ mapSize });
                m_mappingCache[fdVal] = localMapping;
            } else {
                sc_logger::Error("Wayland mmap failed: {}", strerror(errno));
                return std::nullopt;
            }
        }
        mappedData = static_cast<const uint8_t*>(localMapping.get());
    }

    if (!mappedData) {
        return std::nullopt;
    }

    std::unique_lock<std::shared_mutex> lock(m_stateMutex);

    // Cache management: resize if dimensions or format changed
    size_t targetSize = static_cast<size_t>(handle.width) * handle.height * 4;
    if (m_pixelCache.size() != targetSize || m_cacheFormat != desiredFormat || frame->fullUpdate) {
        m_pixelCache.resize(targetSize);
        m_cacheFormat = std::string(desiredFormat);
        sc_logger::Debug("Wayland: Full cache refresh (size: {})", targetSize);

        m_pixelCache = ConvertPixelBuffer(
            std::span<const uint8_t>(mappedData + static_cast<size_t>(handle.offset), dataSize),
            handle.width,
            handle.height,
            handle.stride,
            handle.pixelFormat,
            desiredFormat);
    } else {
        // Partial update via dirty rectangles
        sc_logger::Debug("Wayland: Partial update using {} dirty rects", frame->damage.size());
        uint32_t dstStride = handle.width * 4;
        for (const auto& rect : frame->damage) {
            // Clamp rectangle to frame boundaries
            uint32_t rx = std::min(rect.x, handle.width);
            uint32_t ry = std::min(rect.y, handle.height);
            uint32_t rw = std::min(rect.w, handle.width - rx);
            uint32_t rh = std::min(rect.h, handle.height - ry);

            if (rw == 0 || rh == 0) continue;

            const uint8_t* srcPtr = mappedData + handle.offset + (ry * handle.stride) + (rx * 4);
            uint8_t* dstPtr = m_pixelCache.data() + (ry * dstStride) + (rx * 4);

            ConvertPixelRegion(
                srcPtr,
                dstPtr,
                rw, rh,
                handle.stride,
                dstStride,
                handle.pixelFormat,
                desiredFormat);
        }
    }

    m_frameConsumed = true;
    return m_pixelCache;
}

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

    // Na X11 w tej wersji robimy pełną konwersję, ale do istniejącego już bufora m_pixelCache
    // To eliminuje "stuttering" spowodowany GC i alokacjami pamięci w Node.js/V8
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
