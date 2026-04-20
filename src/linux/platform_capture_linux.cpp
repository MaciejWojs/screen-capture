#ifdef __linux__

#include "../platform_capture.hpp"
#include "../pixel_conversion.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
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
    static int cached = -1;
    if (cached >= 0) return cached;
    std::ifstream nvidia("/proc/driver/nvidia/version");
    cached = nvidia.good() ? 1 : 0;
    return cached;
}

namespace {

    std::string gen_token() {
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<int> dist(0, 15);
        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; i++) ss << dist(rng);
        return ss.str();
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

    using MmapPtr = std::unique_ptr<void, MmapDeleter>;
    using SharedFd = std::shared_ptr<int>;

    struct FrameBufferSlot {
        SharedFd fd;
        std::optional<SharedHandleInfo> handle;
        std::atomic<bool> ready{ false };
    };

    class FrameBufferPool {
        public:
        void Reset() {
            for (auto& slot : m_slots) {
                slot.fd.reset();
                slot.handle.reset();
                slot.ready.store(false, std::memory_order_release);
            }
            m_writeIndex.store(0, std::memory_order_relaxed);
            m_readIndex.store(0, std::memory_order_relaxed);
        }

        void PushFrame(SharedFd fd, std::optional<SharedHandleInfo> handle) {
            size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
            FrameBufferSlot& slot = m_slots[writeIdx];
            slot.fd = std::move(fd);
            slot.handle = std::move(handle);
            slot.ready.store(true, std::memory_order_release);
            m_readIndex.store(writeIdx, std::memory_order_release);
            m_writeIndex.store((writeIdx + 1) % m_slots.size(), std::memory_order_relaxed);
        }

        FrameBufferSlot* AcquireReadFrame() {
            size_t startIdx = m_readIndex.load(std::memory_order_acquire);
            for (size_t i = 0; i < m_slots.size(); ++i) {
                size_t idx = (startIdx + i) % m_slots.size();
                if (m_slots[idx].ready.load(std::memory_order_acquire)) {
                    m_readIndex.store(idx, std::memory_order_release);
                    return &m_slots[idx];
                }
            }
            return nullptr;
        }

        void ConsumeReadFrame() {
            size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
            m_slots[readIdx].ready.store(false, std::memory_order_release);
            m_readIndex.store((readIdx + 1) % m_slots.size(), std::memory_order_relaxed);
        }

        private:
        std::array<FrameBufferSlot, 4> m_slots;
        std::atomic<size_t> m_writeIndex{ 0 };
        std::atomic<size_t> m_readIndex{ 0 };
    };

    static std::optional<std::vector<uint8_t>> ReadPixelDataFromSharedFd(
        int fd,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        uint32_t offset,
        uint64_t planeSize,
        uint32_t pixelFormat,
        const std::string& desiredFormat) {
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

        std::string format = desiredFormat;
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        uint32_t actualStride = stride ? stride : static_cast<uint32_t>(width * 4);
        return ConvertPixelBuffer(
            buffer.data(),
            buffer.size(),
            width,
            height,
            actualStride,
            pixelFormat,
            format);
    }

    static std::optional<std::vector<uint8_t>> ReadPixelDataFromRawPointer(
        const uint8_t* data,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        uint64_t planeSize,
        uint32_t pixelFormat,
        const std::string& desiredFormat) {
        if (!data || width == 0 || height == 0) {
            return std::nullopt;
        }

        size_t expectedSize = planeSize ? static_cast<size_t>(planeSize)
            : static_cast<size_t>(stride) * static_cast<size_t>(height);
        if (expectedSize == 0) {
            return std::nullopt;
        }

        const size_t rowBytes = static_cast<size_t>(width) * 4;
        std::vector<uint8_t> buffer(expectedSize);

        if (stride == rowBytes) {
            std::memcpy(buffer.data(), data, expectedSize);
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                const uint8_t* srcRow = data + static_cast<size_t>(row) * stride;
                uint8_t* dstRow = buffer.data() + static_cast<size_t>(row) * rowBytes;
                std::memcpy(dstRow, srcRow, rowBytes);
            }
        }

        std::string sourceFormat = PixelFormatToString(pixelFormat);
        std::string normalizedDesired = desiredFormat;
        std::transform(normalizedDesired.begin(), normalizedDesired.end(), normalizedDesired.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        if (normalizedDesired == sourceFormat) {
            if (stride == rowBytes) {
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
            buffer.data(),
            buffer.size(),
            width,
            height,
            stride,
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
    constexpr size_t POD_BUFFER_SIZE_CONNECT = 2048;
    constexpr size_t POD_BUFFER_SIZE_UPDATE = 1024;
}

class BaseLinuxPlatformCapture : public IPlatformCapture {
    protected:
    mutable std::shared_mutex m_stateMutex;
    std::thread m_worker;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::mutex m_captureMutex;
    std::condition_variable m_captureCv;

    std::optional<SharedHandleInfo> m_sharedHandle;
    UniqueFd m_sharedFd;
    mutable std::atomic<bool> m_frameConsumed{ false };

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

        int duplicatedFd = dup(*m_sharedFd);
        if (duplicatedFd < 0) {
            return std::nullopt;
        }

        SharedHandleInfo info = *m_sharedHandle;
        info.handle = static_cast<uint64_t>(duplicatedFd);
        m_frameConsumed = true;
        return info;
    }

    int GetFps() const override {
        return m_lastFps.load();
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
        m_worker = std::thread(&WaylandPlatformCapture::RunCaptureFlow, this);
    }

    void Stop() override {
        m_stopRequested.store(true);

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

    std::optional<std::vector<uint8_t>> GetPixelData(const std::string& desiredFormat = "rgba") const override;
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

        int duplicatedFd = dup(*m_sharedFd);
        if (duplicatedFd < 0) {
            return std::nullopt;
        }

        SharedHandleInfo info = *m_sharedHandle;
        info.handle = static_cast<uint64_t>(duplicatedFd);
        m_frameConsumed = true;
        return info;
    }

    std::string GetBackendName() const override {
        return "wayland";
    }

    private:

    GMainLoopPtr m_glibLoop;
    GDBusConnectionPtr m_connection;

    StreamState m_streamState{};
    std::atomic<PortalStage> m_stage{ PortalStage::Idle };

    std::string m_sessionHandle;
    std::optional<StreamConfig> m_streamConfig;
    std::atomic<uint32_t> m_streamNodeId{ PW_ID_ANY };
    uint32_t m_stride = 0;
    uint32_t m_offset = 0;
    uint64_t m_planeSize = 0;
    uint32_t m_bufferType = 0;
    uint32_t m_chunkSize = 0;
    bool m_loggedNonDmabuf = false;
    std::chrono::steady_clock::time_point m_lastMemFdLogTime = std::chrono::steady_clock::time_point::min();

    std::atomic<int> m_pendingPipewireFd{ -1 };
    mutable FrameBufferPool m_frameBuffers;
    mutable MmapPtr m_cachedMapping;
    mutable int m_cachedFd = -1;
    mutable size_t m_cachedMapSize = 0;

    void RunCaptureFlow() {
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

            GVariantBuilderWrapper builder;
            g_variant_builder_add(builder, "{sv}", "session_handle_token", g_variant_new_string(("s" + gen_token()).c_str()));
            g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string(("t" + gen_token()).c_str()));

            m_stage = PortalStage::CreatingSession;
            CallPortalMethod("CreateSession", g_variant_new("(a{sv})", static_cast<GVariantBuilder*>(builder)));

            g_main_loop_run(m_glibLoop.get());

            if (m_stopRequested.load()) {
                throw std::runtime_error("Capture stopped before PipeWire remote was opened");
            }

            pipewireFd = m_pendingPipewireFd.exchange(-1);

            if (pipewireFd < 0) {
                throw std::runtime_error("PipeWire file descriptor was not received from the portal");
            }

            StartPipewireStream(pipewireFd);

            {
                std::unique_lock<std::mutex> waitLock(m_captureMutex);
                m_captureCv.wait(waitLock, [this]() { return m_stopRequested.load(); });
            }

        } catch (const std::exception& e) {
            std::cerr << "[Linux capture] " << e.what() << std::endl;
            if (pipewireFd >= 0) close(pipewireFd);
        }

        CleanupPortal();
        CleanupPipewire();

        if (context) {
            g_main_context_pop_thread_default(context.get());
            context.reset();
        }

        m_glibLoop.reset();

        m_stopRequested.store(false);
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
        g_variant_builder_add(builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
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
        m_streamState.context.reset(pw_context_new(loop, nullptr, 0));
        if (!m_streamState.context) {
            throw std::runtime_error("Unable to create the PipeWire context");
        }

        m_streamState.core.reset(pw_context_connect_fd(m_streamState.context.get(), std::exchange(pipewireFd, -1), nullptr, 0));
        if (!m_streamState.core) {
            throw std::runtime_error("Unable to connect to the PipeWire core through the portal FD");
        }

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
        const spa_pod* params[2];

        const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        const spa_rectangle defaultSize = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);
        const spa_fraction minFramerate = SPA_FRACTION(0, 1);
        const spa_fraction defaultFramerate = SPA_FRACTION(60, 1);
        const spa_fraction maxFramerate = SPA_FRACTION(144, 1);

        bool forceMemFd = IsNvidiaGPU();

        if (forceMemFd) {
            // On some NVIDIA drivers DMA-BUFs are problematic
            // if DMA-BUFs are not working, we should use raw getPixelData with format conversion (which is slower), instead of completely failing to capture
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
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
        } else {
            params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers,
                SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd))));
        }

        pw_stream_add_listener(
            m_streamState.stream.get(),
            &m_streamState.stream_listener,
            &kStreamEvents,
            this);

        uint32_t currentStreamNodeId = m_streamNodeId.load();
        uint32_t targetId = currentStreamNodeId == PW_ID_ANY ? PW_ID_ANY : currentStreamNodeId;
        int result = pw_stream_connect(
            m_streamState.stream.get(),
            PW_DIRECTION_INPUT,
            targetId,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params,
            2);

        if (result < 0) {
            throw std::runtime_error(std::string("pw_stream_connect failed: ") + spa_strerror(result));
        }

        pw_thread_loop_start(m_streamState.pw_loop.get());
    }

    void CleanupPortal() {
        m_connection.reset();

        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            m_sessionHandle.clear();
            m_glibLoop.reset();
        }

        m_streamNodeId = PW_ID_ANY;
        m_stage = PortalStage::Idle;
    }

    void CleanupPipewire() {
        if (m_streamState.pw_loop) {
            pw_thread_loop_stop(m_streamState.pw_loop.get());
        }
        m_streamState.stream.reset();
        m_streamState.core.reset();
        m_streamState.context.reset();
        m_streamState.pw_loop.reset();

        CleanupSharedHandle();
    }

    void CleanupSharedHandle() {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        m_sharedHandle.reset();

        m_sharedFd.reset();

        m_streamConfig.reset();
        m_stride = 0;
        m_offset = 0;
        m_planeSize = 0;
        m_bufferType = 0;
        m_chunkSize = 0;
        m_frameBuffers.Reset();
        m_cachedMapping.reset();
        m_cachedFd = -1;
        m_cachedMapSize = 0;
        m_loggedNonDmabuf = false;
        m_frameConsumed = false;
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

    void UpdateSharedHandleFromFd(int fd) {
        int bufferFd = dup(fd);
        if (bufferFd < 0) {
            return;
        }
        int sharedFd = dup(fd);
        if (sharedFd < 0) {
            close(bufferFd);
            return;
        }

        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        std::optional<SharedHandleInfo> handle;
        if (m_streamConfig) {
            handle = SharedHandleInfo{
                static_cast<uint64_t>(bufferFd),
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
        m_frameBuffers.PushFrame(SharedFd(new int(bufferFd), FdDeleter()), std::move(handle));

        m_cachedMapping.reset();
        m_cachedFd = -1;
        m_cachedMapSize = 0;

        m_sharedFd.reset(new int(sharedFd));
        m_frameConsumed = false;
        PublishSharedHandleLocked();
    }

    static void OnStreamStateChanged(void*, pw_stream_state oldState, pw_stream_state state, const char* error) {
        if (state == PW_STREAM_STATE_ERROR && error) {
            std::cerr << "[PipeWire] Stream error: " << error << std::endl;
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

        std::cerr << "[PipeWire] Chosen stream format: " << PixelFormatToString(info.format)
            << " (" << info.format << ")"
            << ", size=" << info.size.width << "x" << info.size.height
            << ", modifier=" << (hasModifier ? std::to_string(info.modifier) : "none")
            << ", forceMemFd=" << (forceMemFd ? "yes" : "no")
            << std::endl;

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
        const spa_pod* params[2];

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
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
        } else {
            params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers,
                SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd))));
        }

        pw_stream_update_params(self->m_streamState.stream.get(), params, 2);
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
                        self->m_planeSize = data.maxsize;
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
                    std::cerr << "[PipeWire] Używam DMA-BUF (zerowe kopiowanie) – świetnie!" << std::endl;
                } else if (shouldLogMemFd) {
                    std::cerr << "[PipeWire] Używam MemFd (kopiowanie przez CPU) – typowe dla NVIDIA Wayland." << std::endl;
                }

                self->UpdateSharedHandleFromFd(data.fd);
                self->m_loggedNonDmabuf = false;
            } else {
                std::lock_guard<std::shared_mutex> lock(self->m_stateMutex);
                self->m_bufferType = static_cast<uint32_t>(data.type);
                self->m_chunkSize = chunkSize;
            }

            if (data.type != SPA_DATA_DmaBuf && data.type != SPA_DATA_MemFd && !self->m_loggedNonDmabuf) {
                self->m_loggedNonDmabuf = true;
                std::cerr << "[PipeWire] Non-DMA buffer type received: " << data.type << std::endl;
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
                bool foundNode = false;
                uint32_t nodeId = PW_ID_ANY;

                while (results && g_variant_iter_next(results, "{sv}", &key, &value)) {
                    if (g_strcmp0(key, "streams") == 0 && g_variant_is_of_type(value, G_VARIANT_TYPE("a(ua{sv})"))) {
                        GVariantIter streamIter;
                        g_variant_iter_init(&streamIter, value);
                        GVariant* streamTuple = g_variant_iter_next_value(&streamIter);
                        if (streamTuple) {
                            GVariant* props = nullptr;
                            g_variant_get(streamTuple, "(u@a{sv})", &nodeId, &props);
                            if (props) g_variant_unref(props);
                            g_variant_unref(streamTuple);
                            foundNode = true;
                        }
                    }
                    g_variant_unref(value);
                }
                freeResults();

                if (!foundNode || nodeId == PW_ID_ANY) {
                    throw std::runtime_error("Start response did not include a valid PipeWire stream node id");
                }
                self->m_streamNodeId = nodeId;
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
            std::cerr << "[Portal] " << e.what() << std::endl;
            if (self->m_glibLoop) {
                g_main_loop_quit(self->m_glibLoop.get());
                GMainContext* ctx = g_main_loop_get_context(self->m_glibLoop.get());
                if (ctx) g_main_context_wakeup(ctx);
            }
        }
    }

    static const pw_stream_events kStreamEvents;
};

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
        m_worker = std::thread(&X11PlatformCapture::CaptureLoop, this);
    }

    void Stop() override {
        m_stopRequested.store(true);
        m_captureCv.notify_all();

        if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
            m_worker.join();
        }
        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            m_cachedMapping.reset();
            m_cachedFd = -1;
            m_cachedMapSize = 0;
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

    std::optional<std::vector<uint8_t>> GetPixelData(const std::string& desiredFormat = "rgba") const override;

    std::string GetBackendName() const override {
        return "x11";
    }

    private:
    mutable MmapPtr m_cachedMapping;
    mutable int m_cachedFd = -1;
    mutable size_t m_cachedMapSize = 0;
    mutable FrameBufferPool m_frameBuffers;
    std::array<UniqueFd, 2> m_captureFds;
    std::array<MmapPtr, 2> m_captureMappings;
    size_t m_captureWriteIndex = 0;

    void CaptureLoop() {
        std::cerr << "[X11] Uruchamiam proces przechwytywania (CaptureLoop)..." << std::endl;
        DisplayPtr display(XOpenDisplay(nullptr));
        if (!display) {
            std::cerr << "[X11] Cannot open display" << std::endl;
            return;
        }

        int screen = DefaultScreen(display.get());
        Window root = RootWindow(display.get(), screen);

        XWindowAttributes attr;
        XGetWindowAttributes(display.get(), root, &attr);
        int width = attr.width;
        int height = attr.height;
        std::cerr << "[X11] Wymiary okna root: " << width << "x" << height << ", depth=" << attr.depth << std::endl;

        XShmSegmentInfoWrapper shminfo;
        XImagePtr image(XShmCreateImage(display.get(), attr.visual, attr.depth, ZPixmap, nullptr, &shminfo.info, width, height));
        if (!image) {
            std::cerr << "[X11] Cannot create XShmImage" << std::endl;
            return;
        }

        shminfo.info.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0600);
        shminfo.info.shmaddr = image->data = static_cast<char*>(shmat(shminfo.info.shmid, 0, 0));
        shminfo.info.readOnly = False;

        if (!shminfo.Attach(display.get())) {
            std::cerr << "[X11] XShmAttach failed" << std::endl;
            return;
        }

        size_t size = image->bytes_per_line * image->height;
        for (size_t i = 0; i < m_captureFds.size(); ++i) {
            int memfd = memfd_create("x11_capture", MFD_CLOEXEC);
            if (memfd < 0) {
                std::cerr << "[X11] memfd_create failed" << std::endl;
                return;
            }
            m_captureFds[i].reset(new int(memfd));
            if (ftruncate(*m_captureFds[i], size) < 0) {
                std::cerr << "[X11] ftruncate failed" << std::endl;
                return;
            }
            void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, *m_captureFds[i], 0);
            if (mapping == MAP_FAILED) {
                std::cerr << "[X11] mmap failed for buffer " << i << std::endl;
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
            m_cachedMapping.reset();
            m_cachedFd = -1;
            m_cachedMapSize = 0;
            if (m_captureFds[0] && *m_captureFds[0] >= 0) {
                m_sharedFd.reset(new int(dup(*m_captureFds[0])));
            } else {
                m_sharedFd.reset();
            }
            if (m_sharedFd && *m_sharedFd >= 0) {
                std::cerr << "[X11] MemFd utworzony, FD: " << *m_sharedFd << " (size: " << size << ")" << std::endl;
            }
            std::cerr << "[X11] Wykryty format piksela X11: " << PixelFormatToString(detectedFormat) << " (depth=" << image->depth << ", bpp=" << image->bits_per_pixel << ")" << std::endl;
            m_sharedHandle = SharedHandleInfo{
                static_cast<uint64_t>(*m_sharedFd),
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height),
                static_cast<uint32_t>(image->bytes_per_line),
                0, // offset
                static_cast<uint64_t>(size), // planeSize
                detectedFormat,
                0, // modifier
                1, // Przywrócono typ 1 (MemFd), w 'preload' dodano wsparcie dla type=1
                static_cast<uint32_t>(size) // chunkSize
            };
            m_frameConsumed = false;
        }

        uint64_t frameCounter = 0;
        // Pętla przechwytywania (ok. 60 FPS)
        while (!m_stopRequested.load()) {
            auto start = std::chrono::steady_clock::now();

            if (XShmGetImage(display.get(), root, image.get(), 0, 0, AllPlanes)) {
                size_t writeIndex = m_captureWriteIndex;
                std::memcpy(m_captureMappings[writeIndex].get(), image->data, size);

                std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                int duplicatedBufferFd = dup(*m_captureFds[writeIndex]);
                if (duplicatedBufferFd < 0) {
                    std::cerr << "[X11] dup failed for capture buffer" << std::endl;
                } else {
                    SharedHandleInfo handle{
                        static_cast<uint64_t>(duplicatedBufferFd),
                        static_cast<uint32_t>(width),
                        static_cast<uint32_t>(height),
                        static_cast<uint32_t>(image->bytes_per_line),
                        0,
                        static_cast<uint64_t>(size),
                        detectedFormat,
                        0,
                        1,
                        static_cast<uint32_t>(size),
                    };
                    m_frameBuffers.PushFrame(SharedFd(new int(duplicatedBufferFd), FdDeleter()), handle);
                    m_sharedFd.reset(new int(dup(*m_captureFds[writeIndex])));
                    m_sharedHandle = handle;
                    m_frameConsumed = false;
                }

                m_captureWriteIndex = (writeIndex + 1) % m_captureFds.size();
                frameCounter++;
                if (frameCounter % 120 == 0) {
                    std::cerr << "[X11] Pomyślnie zrzucono klatkę, nr: " << frameCounter << std::endl;
                }
                RecordFrame();
            } else {
                std::cerr << "[X11] Błąd pobierania XShmGetImage!" << std::endl;
            }

            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            if (duration.count() < 16) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - duration);
            }
        }
        std::cerr << "[X11] Zatrzymywanie pętli CaptureLoop. Sklonowano klatek: " << frameCounter << std::endl;

        // RAII sprząta zasoby: mapowanie, plik, XShm i display.
    }
};

std::optional<std::vector<uint8_t>> WaylandPlatformCapture::GetPixelData(const std::string& desiredFormat) const {
    auto frame = m_frameBuffers.AcquireReadFrame();
    if (!frame || !frame->handle.has_value() || !frame->fd || *frame->fd < 0) {
        return std::nullopt;
    }

    SharedFd retainedFd = frame->fd;
    const SharedHandleInfo handle = *frame->handle;
    if (handle.width == 0 || handle.height == 0) {
        return std::nullopt;
    }

    size_t mapSize = handle.planeSize ? static_cast<size_t>(handle.planeSize)
        : static_cast<size_t>(handle.stride) * static_cast<size_t>(handle.height) + static_cast<size_t>(handle.offset);
    const uint8_t* mappedData = nullptr;

    {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        if (!m_cachedMapping || m_cachedFd != static_cast<int>(handle.handle) || m_cachedMapSize != mapSize) {
            m_cachedMapping.reset();
            void* ptr = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, static_cast<int>(handle.handle), 0);
            if (ptr == MAP_FAILED) {
                return std::nullopt;
            }
            m_cachedMapping = MmapPtr(ptr, MmapDeleter{ mapSize });
            m_cachedFd = static_cast<int>(handle.handle);
            m_cachedMapSize = mapSize;
        }
        mappedData = static_cast<const uint8_t*>(m_cachedMapping.get());
    }

    if (!mappedData) {
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> result = ReadPixelDataFromRawPointer(
        mappedData + static_cast<size_t>(handle.offset),
        handle.width,
        handle.height,
        handle.stride,
        handle.planeSize,
        handle.pixelFormat,
        desiredFormat);

    if (result) {
        m_frameBuffers.ConsumeReadFrame();
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        m_frameConsumed = true;
    }
    return result;
}

std::optional<std::vector<uint8_t>> X11PlatformCapture::GetPixelData(const std::string& desiredFormat) const {
    auto frame = m_frameBuffers.AcquireReadFrame();
    if (!frame || !frame->handle.has_value() || !frame->fd || *frame->fd < 0) {
        return std::nullopt;
    }

    SharedFd retainedFd = frame->fd;
    const SharedHandleInfo handle = *frame->handle;
    if (handle.width == 0 || handle.height == 0) {
        return std::nullopt;
    }

    size_t mapSize = handle.planeSize ? static_cast<size_t>(handle.planeSize)
        : static_cast<size_t>(handle.stride) * static_cast<size_t>(handle.height) + static_cast<size_t>(handle.offset);
    const uint8_t* mappedData = nullptr;

    {
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        if (!m_cachedMapping || m_cachedFd != static_cast<int>(handle.handle) || m_cachedMapSize != mapSize) {
            m_cachedMapping.reset();
            void* ptr = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, static_cast<int>(handle.handle), 0);
            if (ptr == MAP_FAILED) {
                return std::nullopt;
            }
            m_cachedMapping = MmapPtr(ptr, MmapDeleter{ mapSize });
            m_cachedFd = static_cast<int>(handle.handle);
            m_cachedMapSize = mapSize;
        }
        mappedData = static_cast<const uint8_t*>(m_cachedMapping.get());
    }

    if (!mappedData) {
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> result = ReadPixelDataFromRawPointer(
        mappedData + static_cast<size_t>(handle.offset),
        handle.width,
        handle.height,
        handle.stride,
        handle.planeSize,
        handle.pixelFormat,
        desiredFormat);

    if (result) {
        m_frameBuffers.ConsumeReadFrame();
        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        m_frameConsumed = true;
    }
    return result;
}

bool IsWayland() {
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay && waylandDisplay[0] != '\0') return true;

    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && std::string(sessionType) == "wayland") return true;

    return false;
}

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
    if (IsWayland()) {
        std::cout << "[Capture] Wykryto środowisko Wayland." << std::endl;
        return std::make_unique<WaylandPlatformCapture>();
    } else {
        std::cout << "[Capture] Wykryto środowisko X11." << std::endl;
        return std::make_unique<X11PlatformCapture>();
    }
}

#endif

