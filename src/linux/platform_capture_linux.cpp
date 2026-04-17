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

    std::atomic<int> m_pendingPipewireFd{ -1 };

    void RunCaptureFlow() {
        int pipewireFd = -1;
        GMainContextPtr context;

        try {
            pw_init(nullptr, nullptr);

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
        int duplicatedFd = dup(fd);
        if (duplicatedFd < 0) {
            return;
        }

        std::unique_lock<std::shared_mutex> lock(m_stateMutex);
        m_sharedFd.reset(new int(duplicatedFd));
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
                // Logowanie typu bufora
                if (data.type == SPA_DATA_DmaBuf) {
                    std::cerr << "[PipeWire] Używam DMA-BUF (zerowe kopiowanie) – świetnie!" << std::endl;
                } else if (data.type == SPA_DATA_MemFd) {
                    std::cerr << "[PipeWire] Używam MemFd (kopiowanie przez CPU) – typowe dla NVIDIA Wayland." << std::endl;
                }
                {
                    std::lock_guard<std::shared_mutex> lock(self->m_stateMutex);
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

    std::string GetBackendName() const override {
        return "x11";
    }

    private:
    void CaptureLoop() {
        std::cerr << "[X11] Uruchamiam proces przechwytywania (CaptureLoop)..." << std::endl;
        Display* display = XOpenDisplay(nullptr);
        if (!display) {
            std::cerr << "[X11] Cannot open display" << std::endl;
            return;
        }

        int screen = DefaultScreen(display);
        Window root = RootWindow(display, screen);

        XWindowAttributes attr;
        XGetWindowAttributes(display, root, &attr);
        int width = attr.width;
        int height = attr.height;
        std::cerr << "[X11] Wymiary okna root: " << width << "x" << height << ", depth=" << attr.depth << std::endl;

        XShmSegmentInfo shminfo;
        XImage* image = XShmCreateImage(display, attr.visual, attr.depth, ZPixmap, nullptr, &shminfo, width, height);
        if (!image) {
            std::cerr << "[X11] Cannot create XShmImage" << std::endl;
            XCloseDisplay(display);
            return;
        }

        shminfo.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0600);
        shminfo.shmaddr = image->data = (char*)shmat(shminfo.shmid, 0, 0);
        shminfo.readOnly = False;

        if (!XShmAttach(display, &shminfo)) {
            std::cerr << "[X11] XShmAttach failed" << std::endl;
            XDestroyImage(image);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
            XCloseDisplay(display);
            return;
        }

        int memfd = memfd_create("x11_capture", MFD_CLOEXEC);
        if (memfd < 0) {
            std::cerr << "[X11] memfd_create failed" << std::endl;
            XShmDetach(display, &shminfo);
            XDestroyImage(image);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
            XCloseDisplay(display);
            return;
        }

        size_t size = image->bytes_per_line * image->height;
        if (ftruncate(memfd, size) < 0) {
            std::cerr << "[X11] ftruncate failed" << std::endl;
            close(memfd);
            XShmDetach(display, &shminfo);
            XDestroyImage(image);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
            XCloseDisplay(display);
            return;
        }

        void* memfd_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (memfd_ptr == MAP_FAILED) {
            close(memfd);
            XShmDetach(display, &shminfo);
            XDestroyImage(image);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
            XCloseDisplay(display);
            return;
        }

        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            m_sharedFd.reset(new int(dup(memfd)));
            std::cerr << "[X11] MemFd utworzony, FD: " << *m_sharedFd << " (size: " << size << ")" << std::endl;
            m_sharedHandle = SharedHandleInfo{
                static_cast<uint64_t>(*m_sharedFd),
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height),
                static_cast<uint32_t>(image->bytes_per_line),
                0, // offset
                static_cast<uint64_t>(size), // planeSize
                7, // SPA_VIDEO_FORMAT_BGRA domyślny format wizualizacji X11 dla ZPixmap z głębią 24/32
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

            if (XShmGetImage(display, root, image, 0, 0, AllPlanes)) {
                memcpy(memfd_ptr, image->data, size);

                std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                m_frameConsumed = false;
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

        // Czyszczenie
        munmap(memfd_ptr, size);
        close(memfd);
        XShmDetach(display, &shminfo);
        XDestroyImage(image);
        shmdt(shminfo.shmaddr);
        shmctl(shminfo.shmid, IPC_RMID, 0);
        XCloseDisplay(display);
    }
};

std::optional<std::vector<uint8_t>> WaylandPlatformCapture::GetPixelData(const std::string& desiredFormat) const {
    std::unique_lock<std::shared_mutex> lock(m_stateMutex);
    if (!m_sharedFd || *m_sharedFd < 0 || m_frameConsumed || !m_streamConfig) {
        return std::nullopt;
    }

    if (m_streamConfig->width == 0 || m_streamConfig->height == 0) {
        return std::nullopt;
    }

    size_t dataSize = m_planeSize ? static_cast<size_t>(m_planeSize)
        : static_cast<size_t>(m_stride) * static_cast<size_t>(m_streamConfig->height);
    if (dataSize == 0) {
        return std::nullopt;
    }

    size_t mapSize = m_planeSize ? static_cast<size_t>(m_planeSize) : dataSize + static_cast<size_t>(m_offset);
    void* mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, *m_sharedFd, 0);
    if (mapped == MAP_FAILED) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(dataSize);
    memcpy(buffer.data(), static_cast<uint8_t*>(mapped) + static_cast<size_t>(m_offset), dataSize);
    munmap(mapped, mapSize);

    std::string format = desiredFormat;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });

    uint32_t stride = m_stride ? m_stride : static_cast<uint32_t>(m_streamConfig->width * 4);
    std::vector<uint8_t> result = ConvertPixelBuffer(
        buffer.data(),
        buffer.size(),
        static_cast<uint32_t>(m_streamConfig->width),
        static_cast<uint32_t>(m_streamConfig->height),
        stride,
        m_streamConfig->pixelFormat,
        format
    );

    m_frameConsumed = true;
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

