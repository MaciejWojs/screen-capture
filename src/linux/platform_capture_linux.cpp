#ifdef __linux__

#include "../platform_capture.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
struct GVariantBuilderWrapper {
    GVariantBuilder builder;
    GVariantBuilderWrapper(const GVariantType* type) {
        g_variant_builder_init(&builder, type);
    }
    ~GVariantBuilderWrapper() {
        g_variant_builder_clear(&builder);
    }
    operator GVariantBuilder*() { return &builder; }
};

std::string gen_token() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;
    return "electron_capture_" + std::to_string(dis(gen));
}

template <typename T, auto FreeFunc>
struct GenericDeleter {
    void operator()(T* ptr) const { if (ptr) FreeFunc(ptr); }
};

struct GObjectDeleter {
    void operator()(void* ptr) const { if (ptr) g_object_unref(ptr); }
};

using GMainLoopPtr = std::unique_ptr<GMainLoop, GenericDeleter<GMainLoop, g_main_loop_unref>>;
using GDBusConnectionPtr = std::unique_ptr<GDBusConnection, GObjectDeleter>;
using PwThreadLoopPtr = std::unique_ptr<pw_thread_loop, GenericDeleter<pw_thread_loop, pw_thread_loop_destroy>>;
using PwContextPtr = std::unique_ptr<pw_context, GenericDeleter<pw_context, pw_context_destroy>>;
using PwCorePtr = std::unique_ptr<pw_core, GenericDeleter<pw_core, pw_core_disconnect>>;
using PwStreamPtr = std::unique_ptr<pw_stream, GenericDeleter<pw_stream, pw_stream_destroy>>;
using GVariantPtr = std::unique_ptr<GVariant, GenericDeleter<GVariant, g_variant_unref>>;
using GErrorPtr = std::unique_ptr<GError, GenericDeleter<GError, g_error_free>>;
using GUnixFDListPtr = std::unique_ptr<GUnixFDList, GObjectDeleter>;
using GCancellablePtr = std::unique_ptr<GCancellable, GObjectDeleter>;

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

class LinuxPlatformCapture final : public IPlatformCapture {
    public:
    LinuxPlatformCapture() = default;

    ~LinuxPlatformCapture() override {
        Stop();
        if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
            m_worker.join();
        }
    }

    void Start(Napi::Env) override {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) {
            return;
        }

        m_worker = std::thread(&LinuxPlatformCapture::RunCaptureFlow, this);
    }

    void Stop() override {
        m_stopRequested.store(true);

        if (m_cancellable) {
            g_cancellable_cancel(m_cancellable.get());
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_pwLoop) {
                pw_thread_loop_stop(m_pwLoop);
            }
            if (m_glibLoop) {
                g_main_loop_quit(m_glibLoop.get());
                GMainContext* ctx = g_main_loop_get_context(m_glibLoop.get());
                if (ctx) {
                    g_main_context_wakeup(ctx);
                }
            }
        }
        m_captureCv.notify_all();

        CleanupSharedHandle();
        m_running.store(false);
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (!m_sharedHandle.has_value() || m_frameConsumed || m_sharedFd < 0) {
            return std::nullopt;
        }

        SharedHandleInfo info = *m_sharedHandle;
        // Dup the FD for JS so it takes exclusive ownership of the new FD.
        info.handle = static_cast<uint64_t>(dup(m_sharedFd));
        m_frameConsumed = true;
        return info;
    }

    private:
    mutable std::mutex m_stateMutex;
    mutable std::mutex m_frameMutex;
    std::mutex m_captureMutex;
    std::condition_variable m_captureCv;
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_portalFailed{false};

    GMainLoopPtr m_glibLoop;
    GDBusConnectionPtr m_connection;
    GCancellablePtr m_cancellable;
    pw_thread_loop* m_pwLoop = nullptr;
    StreamState m_streamState{};
    PortalStage m_stage = PortalStage::Idle;

    std::string m_sessionHandle;
    std::string m_currentRequestPath;
    std::optional<SharedHandleInfo> m_sharedHandle;
    uint32_t m_streamNodeId = PW_ID_ANY;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_stride = 0;
    uint32_t m_offset = 0;
    uint64_t m_planeSize = 0;
    uint32_t m_pixelFormat = 0;
    uint64_t m_modifier = 0;
    uint32_t m_bufferType = 0;
    uint32_t m_chunkSize = 0;
    bool m_loggedNonDmabuf = false;
    mutable bool m_frameConsumed = false;

    int m_pendingPipewireFd = -1;
    int m_sharedFd = -1;

    void RunCaptureFlow() {
        int pipewireFd = -1;
        GMainContext* context = nullptr;

        try {
            pw_init(nullptr, nullptr);

            context = g_main_context_new();
            g_main_context_push_thread_default(context);

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_glibLoop.reset(g_main_loop_new(context, FALSE));
            }

            m_portalFailed.store(false);

            m_connection.reset(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr));
            if (!m_connection) {
                throw std::runtime_error("Unable to connect to the session D-Bus");
            }

            m_cancellable.reset(g_cancellable_new());

            g_dbus_connection_signal_subscribe(
                m_connection.get(),
                "org.freedesktop.portal.Desktop",
                "org.freedesktop.portal.Request",
                "Response",
                nullptr,
                nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                &LinuxPlatformCapture::OnPortalResponse,
                this,
                nullptr);

            std::cerr << "[1/4] CreateSession..." << std::endl;
            GVariantBuilderWrapper builder(G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(builder, "{sv}", "session_handle_token", g_variant_new_string(("s" + gen_token()).c_str()));
            g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string(("t" + gen_token()).c_str()));

            m_stage = PortalStage::CreatingSession;
            m_currentRequestPath = CallPortalMethod("CreateSession", g_variant_new("(a{sv})", builder));

            g_main_loop_run(m_glibLoop.get());

            if (m_stopRequested.load()) {
                throw std::runtime_error("Capture stopped before PipeWire remote was opened");
            }

            if (m_portalFailed.load()) {
                throw std::runtime_error("Portal request failed or user cancelled");
            }

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                pipewireFd = m_pendingPipewireFd;
                m_pendingPipewireFd = -1;
            }

            if (pipewireFd < 0) {
                throw std::runtime_error("PipeWire file descriptor was not received from the portal");
            }

            StartPipewireStream(pipewireFd);
        } catch (const std::exception& e) {
            std::cerr << "[Linux capture] " << e.what() << std::endl;
            if (pipewireFd >= 0) {
                close(pipewireFd);
            }
        }

        if (context) {
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            context = nullptr;
        }

        std::unique_lock<std::mutex> waitLock(m_captureMutex);
        m_captureCv.wait(waitLock, [this]() { return m_stopRequested.load(); });
        
        CleanupPortal();
        CleanupPipewire();

        m_stopRequested.store(false);
        m_running.store(false);
    }

    std::string CallPortalMethod(const char* method, GVariant* params) {
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
            m_cancellable.get(),
            &rawError));

        GErrorPtr error(rawError);

        if (!result) {
            std::string message = error ? error->message : "Unknown portal error";
            throw std::runtime_error(message);
        }

        const gchar* path = nullptr;
        g_variant_get(result.get(), "(o)", &path);
        return path ? path : "";
    }

    void StartSession() {
        std::cerr << "[3/4] Start..." << std::endl;
        GVariantBuilderWrapper builder(G_VARIANT_TYPE("a{sv}"));
        m_stage = PortalStage::StartingSession;
        m_currentRequestPath = CallPortalMethod("Start", g_variant_new("(osa{sv})", m_sessionHandle.c_str(), "", builder));
    }

    void SelectSources() {
        std::cerr << "[2/4] SelectSources..." << std::endl;
        GVariantBuilderWrapper builder(G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(builder, "{sv}", "types", g_variant_new_uint32(1));
        g_variant_builder_add(builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
        g_variant_builder_add(builder, "{sv}", "cursor_mode", g_variant_new_uint32(2));

        m_stage = PortalStage::SelectingSources;
        m_currentRequestPath = CallPortalMethod("SelectSources", g_variant_new("(oa{sv})", m_sessionHandle.c_str(), builder));
    }

    void OpenPipeWireRemote() {
        std::cerr << "[4/4] OpenPipeWireRemote..." << std::endl;
        GVariantBuilderWrapper builder(G_VARIANT_TYPE("a{sv}"));

        GError* rawError = nullptr;
        GUnixFDList* rawOutFdList = nullptr;

        GVariantPtr result(g_dbus_connection_call_with_unix_fd_list_sync(
            m_connection.get(),
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote",
            g_variant_new("(oa{sv})", m_sessionHandle.c_str(), builder),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &rawOutFdList,
            m_cancellable.get(),
            &rawError));

        GErrorPtr error(rawError);
        GUnixFDListPtr outFdList(rawOutFdList);

        if (!result) {
            std::string message = error ? error->message : "Unknown portal error";
            throw std::runtime_error(message);
        }

        gint32 fdIndex = -1;
        g_variant_get(result.get(), "(h)", &fdIndex);
        
        rawError = nullptr;
        int fd = g_unix_fd_list_get(outFdList.get(), fdIndex, &rawError);
        error.reset(rawError);

        if (fd < 0) {
            std::string message = error ? error->message : "Unable to extract PipeWire FD";
            throw std::runtime_error(message);
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pendingPipewireFd = fd;
        }

        m_stage = PortalStage::OpeningRemote;
        g_main_loop_quit(m_glibLoop.get());
        GMainContext* ctx = g_main_loop_get_context(m_glibLoop.get());
        if (ctx) g_main_context_wakeup(ctx);
    }

    void BuildFormatParams(spa_pod_builder* builder, const spa_pod** params) {
        const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        const spa_rectangle defaultSize = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);

        const spa_fraction minFramerate = SPA_FRACTION(0, 1);
        const spa_fraction defaultFramerate = SPA_FRACTION(60, 1);
        const spa_fraction maxFramerate = SPA_FRACTION(144, 1);

        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            builder,
            SPA_TYPE_OBJECT_Format,
            SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,
            SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_xRGB),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(&defaultFramerate, &minFramerate, &maxFramerate),
            SPA_FORMAT_VIDEO_modifier,
            SPA_POD_CHOICE_ENUM_Long(3, 0ULL, 0ULL, 0x00ffffffffffffffULL)));

        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            builder,
            SPA_TYPE_OBJECT_ParamBuffers,
            SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf)));
    }

    void StartPipewireStream(int pipewireFd) {
        m_streamState.pw_loop.reset(pw_thread_loop_new("capture-loop", nullptr));
        if (!m_streamState.pw_loop) {
            throw std::runtime_error("Unable to create the PipeWire main loop");
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pwLoop = m_streamState.pw_loop.get();
        }

        pw_loop* loop = pw_thread_loop_get_loop(m_streamState.pw_loop.get());
        m_streamState.context.reset(pw_context_new(loop, nullptr, 0));
        if (!m_streamState.context) {
            throw std::runtime_error("Unable to create the PipeWire context");
        }

        m_streamState.core.reset(pw_context_connect_fd(m_streamState.context.get(), pipewireFd, nullptr, 0));
        if (!m_streamState.core) {
            close(pipewireFd);
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

        uint8_t buffer[2048];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[2];

        BuildFormatParams(&builder, params);

        pw_stream_add_listener(
            m_streamState.stream.get(),
            &m_streamState.stream_listener,
            &kStreamEvents,
            this);

        std::cerr << "[PipeWire] Connecting stream..." << std::endl;
        uint32_t targetId = m_streamNodeId == PW_ID_ANY ? PW_ID_ANY : m_streamNodeId;
        int result = pw_stream_connect(
            m_streamState.stream.get(),
            PW_DIRECTION_INPUT,
            targetId,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT),
            params,
            2);

        if (result < 0) {
            throw std::runtime_error(std::string("pw_stream_connect failed: ") + spa_strerror(result));
        }

        pw_thread_loop_start(m_streamState.pw_loop.get());
    }

    void CleanupPortal() {
        m_connection.reset();
        m_glibLoop.reset();

        m_sessionHandle.clear();
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

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pwLoop = nullptr;
        }

        if (m_pendingPipewireFd >= 0) {
            close(m_pendingPipewireFd);
            m_pendingPipewireFd = -1;
        }

        CleanupSharedHandle();
    }

    void CleanupSharedHandle() {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_sharedHandle.reset();

        if (m_sharedFd >= 0) {
            close(m_sharedFd);
            m_sharedFd = -1;
        }

        m_width = 0;
        m_height = 0;
        m_stride = 0;
        m_offset = 0;
        m_planeSize = 0;
        m_pixelFormat = 0;
        m_modifier = 0;
        m_bufferType = 0;
        m_chunkSize = 0;
        m_loggedNonDmabuf = false;
        m_frameConsumed = false;
    }

    void PublishSharedHandleLocked() {
        if (m_sharedFd < 0 || m_width == 0 || m_height == 0) {
            return;
        }

        m_sharedHandle = SharedHandleInfo{
            static_cast<uint64_t>(m_sharedFd),
            m_width,
            m_height,
            m_stride,
            m_offset,
            m_planeSize,
            m_pixelFormat,
            m_modifier,
            m_bufferType,
            m_chunkSize,
        };
    }

    void UpdateSharedHandleFromFdLocked(int fd) {
        int duplicatedFd = dup(fd);
        if (duplicatedFd < 0) {
            return;
        }

        if (m_sharedFd >= 0) {
            close(m_sharedFd);
        }
        m_sharedFd = duplicatedFd;
        m_frameConsumed = false;
        PublishSharedHandleLocked();
    }

    static void OnStreamStateChanged(void*, pw_stream_state oldState, pw_stream_state state, const char* error) {
        std::cerr << "[PipeWire] State: " << pw_stream_state_as_string(state)
                  << " (old: " << pw_stream_state_as_string(oldState) << ")" << std::endl;
        if (state == PW_STREAM_STATE_ERROR && error) {
            std::cerr << "[PipeWire] Stream error: " << error << std::endl;
        }
    }

    static void OnStreamParamChanged(void* data, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<LinuxPlatformCapture*>(data);
        if (!self) return;
        if (!param || id != SPA_PARAM_Format) {
            return;
        }

        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(self->m_frameMutex);
            self->m_width = info.size.width;
            self->m_height = info.size.height;
            self->m_pixelFormat = static_cast<uint32_t>(info.format);
            self->m_modifier = info.modifier;
            self->PublishSharedHandleLocked();
        }

        uint8_t buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[1];
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

        pw_stream_update_params(self->m_streamState.stream.get(), params, 1);
    }

    static void OnStreamProcess(void* userdata) {
        auto* self = static_cast<LinuxPlatformCapture*>(userdata);
        if (!self) return;
        std::lock_guard<std::mutex> lock(self->m_frameMutex);
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
                {
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
                self->UpdateSharedHandleFromFdLocked(data.fd);
                self->m_loggedNonDmabuf = false;
            } else {
                self->m_bufferType = static_cast<uint32_t>(data.type);
                self->m_chunkSize = chunkSize;
            }

            if (data.type != SPA_DATA_DmaBuf && data.type != SPA_DATA_MemFd && !self->m_loggedNonDmabuf) {
                self->m_loggedNonDmabuf = true;
            }
        }

        pw_stream_queue_buffer(self->m_streamState.stream.get(), buffer);
    }

    static void OnPortalResponse(
        GDBusConnection*,
        const gchar*,
        const gchar* object_path,
        const gchar*,
        const gchar*,
        GVariant* parameters,
        gpointer userData) {
        auto* self = static_cast<LinuxPlatformCapture*>(userData);
        if (!self || self->m_currentRequestPath != object_path) return;

        guint32 responseCode = 1;
        GVariant* results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &responseCode, &results);
        GVariantPtr resultsPtr(results);

        if (responseCode != 0) {
            if (responseCode == 1) {
                std::cerr << "[Portal] User cancelled screen sharing." << std::endl;
            } else {
                std::cerr << "[Portal] Response error: " << responseCode << std::endl;
            }
            self->m_portalFailed.store(true);
            if (self->m_glibLoop) g_main_loop_quit(self->m_glibLoop.get());
            return;
        }

        try {
            if (self->m_stage == PortalStage::CreatingSession) {
                const gchar* session_handle = nullptr;
                if (g_variant_lookup(results, "session_handle", "&s", &session_handle)) {
                    self->m_sessionHandle = session_handle;
                    self->SelectSources();
                    return;
                }
                throw std::runtime_error("CreateSession response did not include a session handle");
            }
            if (self->m_stage == PortalStage::SelectingSources) {
                self->StartSession();
                return;
            }
            if (self->m_stage == PortalStage::StartingSession) {
                GVariant* streamsVar = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
                if (streamsVar) {
                    GVariantPtr streamsPtr(streamsVar);
                    GVariantIter streamIter;
                    g_variant_iter_init(&streamIter, streamsVar);
                    uint32_t nodeId = PW_ID_ANY;
                    GVariant* streamTuple = g_variant_iter_next_value(&streamIter);
                    if (streamTuple) {
                        GVariantPtr tuplePtr(streamTuple);
                        g_variant_get(streamTuple, "(u@a{sv})", &nodeId, nullptr);
                    }
                    if (nodeId != PW_ID_ANY) {
                        self->m_streamNodeId = nodeId;
                        self->OpenPipeWireRemote();
                        return;
                    }
                }
                throw std::runtime_error("Start response did not include a valid PipeWire stream node id");
            }

            self->m_portalFailed.store(true);
            if (self->m_glibLoop) g_main_loop_quit(self->m_glibLoop.get());
        } catch (const std::exception& e) {
            std::cerr << "[Portal] " << e.what() << std::endl;
            self->m_portalFailed.store(true);
            if (self->m_glibLoop) g_main_loop_quit(self->m_glibLoop.get());
        }
    }

    static const pw_stream_events kStreamEvents;
};

const pw_stream_events LinuxPlatformCapture::kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = LinuxPlatformCapture::OnStreamStateChanged;
    events.param_changed = LinuxPlatformCapture::OnStreamParamChanged;
    events.process = LinuxPlatformCapture::OnStreamProcess;
    return events;
}();

std::unique_ptr<IPlatformCapture> CreatePlatformCapture() {
    return std::make_unique<LinuxPlatformCapture>();
}

#endif
