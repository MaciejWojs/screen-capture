#ifdef __linux__

#include "../platform_capture.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <fstream>

namespace {

std::ofstream& GetLogFile() {
    static std::ofstream logFile("/tmp/screen-capture-electron.log", std::ios::app);
    return logFile;
}

#define LOG(msg) do { GetLogFile() << msg << std::endl; } while(0)

std::string gen_token() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(0, 15);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) {
        ss << dist(rng);
    }
    return ss.str();
}

struct StreamState {
    pw_main_loop* pw_loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
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

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_glibLoop) {
                g_main_loop_quit(m_glibLoop);
            }
            if (m_pwLoop) {
                pw_main_loop_quit(m_pwLoop);
            }
        }

        if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
            m_worker.join();
        }

        CleanupSharedHandle();
        m_running.store(false);
    }

    std::optional<SharedHandleInfo> GetSharedHandle() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
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
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    GMainLoop* m_glibLoop = nullptr;
    GDBusConnection* m_connection = nullptr;
    pw_main_loop* m_pwLoop = nullptr;
    StreamState m_streamState{};
    PortalStage m_stage = PortalStage::Idle;

    std::string m_sessionHandle;
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

        try {
            pw_init(nullptr, nullptr);

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_glibLoop = g_main_loop_new(nullptr, FALSE);
            }

            m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
            if (!m_connection) {
                throw std::runtime_error("Unable to connect to the session D-Bus");
            }

            g_dbus_connection_signal_subscribe(
                m_connection,
                "org.freedesktop.portal.Desktop",
                "org.freedesktop.portal.Request",
                "Response",
                nullptr,
                nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                &LinuxPlatformCapture::OnPortalResponse,
                this,
                nullptr);

            GetLogFile() << "[1/4] CreateSession..." << std::endl;
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(("s" + gen_token()).c_str()));
            g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(("t" + gen_token()).c_str()));

            m_stage = PortalStage::CreatingSession;
            CallPortalMethod("CreateSession", g_variant_new("(a{sv})", &builder));

            g_main_loop_run(m_glibLoop);

            if (m_stopRequested.load()) {
                throw std::runtime_error("Capture stopped before PipeWire remote was opened");
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
            GetLogFile() << "[Linux capture] " << e.what() << std::endl;
            if (pipewireFd >= 0) {
                close(pipewireFd);
            }
        }

        CleanupPortal();
        CleanupPipewire();

        m_stopRequested.store(false);
        m_running.store(false);
    }

    void CallPortalMethod(const char* method, GVariant* params) {
        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            m_connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            method,
            params,
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error);

        if (!result) {
            std::string message = error ? error->message : "Unknown portal error";
            if (error) {
                g_error_free(error);
            }
            throw std::runtime_error(message);
        }

        g_variant_unref(result);
    }

    void StartSession() {
        GetLogFile() << "[3/4] Start..." << std::endl;
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        m_stage = PortalStage::StartingSession;
        CallPortalMethod("Start", g_variant_new("(osa{sv})", m_sessionHandle.c_str(), "", &builder));
    }

    void SelectSources() {
        GetLogFile() << "[2/4] SelectSources..." << std::endl;
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(1));
        g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(1));

        m_stage = PortalStage::SelectingSources;
        CallPortalMethod("SelectSources", g_variant_new("(oa{sv})", m_sessionHandle.c_str(), &builder));
    }

    void OpenPipeWireRemote() {
        GetLogFile() << "[4/4] OpenPipeWireRemote..." << std::endl;
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

        GError* error = nullptr;
        GUnixFDList* outFdList = nullptr;

        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            m_connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote",
            g_variant_new("(oa{sv})", m_sessionHandle.c_str(), &builder),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &outFdList,
            nullptr,
            &error);

        if (!result) {
            std::string message = error ? error->message : "Unknown portal error";
            if (error) {
                g_error_free(error);
            }
            throw std::runtime_error(message);
        }

        gint32 fdIndex = -1;
        g_variant_get(result, "(h)", &fdIndex);
        int fd = g_unix_fd_list_get(outFdList, fdIndex, &error);

        g_variant_unref(result);
        if (outFdList) {
            g_object_unref(outFdList);
        }

        if (fd < 0) {
            std::string message = error ? error->message : "Unable to extract PipeWire FD";
            if (error) {
                g_error_free(error);
            }
            throw std::runtime_error(message);
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pendingPipewireFd = fd;
        }

        m_stage = PortalStage::OpeningRemote;
        g_main_loop_quit(m_glibLoop);
    }

    void StartPipewireStream(int pipewireFd) {
        m_streamState.pw_loop = pw_main_loop_new(nullptr);
        if (!m_streamState.pw_loop) {
            throw std::runtime_error("Unable to create the PipeWire main loop");
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pwLoop = m_streamState.pw_loop;
        }

        pw_loop* loop = pw_main_loop_get_loop(m_streamState.pw_loop);
        m_streamState.context = pw_context_new(loop, nullptr, 0);
        if (!m_streamState.context) {
            throw std::runtime_error("Unable to create the PipeWire context");
        }

        m_streamState.core = pw_context_connect_fd(m_streamState.context, pipewireFd, nullptr, 0);
        if (!m_streamState.core) {
            close(pipewireFd);
            throw std::runtime_error("Unable to connect to the PipeWire core through the portal FD");
        }

        m_streamState.stream = pw_stream_new(
            m_streamState.core,
            "electron-capture",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Video",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Screen",
                nullptr));

        if (!m_streamState.stream) {
            throw std::runtime_error("Unable to create the PipeWire stream");
        }

        uint8_t buffer[2048];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[2];

        const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        const spa_rectangle defaultSize = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);

        const spa_fraction minFramerate = SPA_FRACTION(0, 1);
        const spa_fraction defaultFramerate = SPA_FRACTION(60, 1);
        const spa_fraction maxFramerate = SPA_FRACTION(144, 1);

        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_Format,
            SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,
            SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_xRGB),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(&defaultFramerate, &minFramerate, &maxFramerate),
            SPA_FORMAT_VIDEO_modifier,
            SPA_POD_CHOICE_ENUM_Long(3, 0ULL, 0ULL, 0x00ffffffffffffffULL)));

        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamBuffers,
            SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf)));

        pw_stream_add_listener(
            m_streamState.stream,
            &m_streamState.stream_listener,
            &kStreamEvents,
            this);

        GetLogFile() << "[PipeWire] Connecting stream..." << std::endl;
        uint32_t targetId = m_streamNodeId == PW_ID_ANY ? PW_ID_ANY : m_streamNodeId;
        int result = pw_stream_connect(
            m_streamState.stream,
            PW_DIRECTION_INPUT,
            targetId,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT),
            params,
            2);

        if (result < 0) {
            throw std::runtime_error(std::string("pw_stream_connect failed: ") + spa_strerror(result));
        }

        pw_main_loop_run(m_streamState.pw_loop);
    }

    void CleanupPortal() {
        if (m_connection) {
            g_object_unref(m_connection);
            m_connection = nullptr;
        }

        if (m_glibLoop) {
            g_main_loop_unref(m_glibLoop);
            m_glibLoop = nullptr;
        }

        m_sessionHandle.clear();
        m_streamNodeId = PW_ID_ANY;
        m_stage = PortalStage::Idle;
    }

    void CleanupPipewire() {
        if (m_streamState.stream) {
            pw_stream_destroy(m_streamState.stream);
            m_streamState.stream = nullptr;
        }

        if (m_streamState.core) {
            pw_core_disconnect(m_streamState.core);
            m_streamState.core = nullptr;
        }

        if (m_streamState.context) {
            pw_context_destroy(m_streamState.context);
            m_streamState.context = nullptr;
        }

        if (m_streamState.pw_loop) {
            pw_main_loop_destroy(m_streamState.pw_loop);
            m_streamState.pw_loop = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pwLoop = nullptr;
        }

        CleanupSharedHandle();
    }

    void CleanupSharedHandle() {
        std::lock_guard<std::mutex> lock(m_stateMutex);
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

    void UpdateSharedHandleFromFd(int fd) {
        int duplicatedFd = dup(fd);
        if (duplicatedFd < 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_sharedFd >= 0) {
            close(m_sharedFd);
        }
        m_sharedFd = duplicatedFd;
        m_frameConsumed = false;
        PublishSharedHandleLocked();
    }

    static void OnStreamStateChanged(void*, pw_stream_state oldState, pw_stream_state state, const char* error) {
        GetLogFile() << "[PipeWire] State: " << pw_stream_state_as_string(state)
                  << " (old: " << pw_stream_state_as_string(oldState) << ")" << std::endl;
        if (state == PW_STREAM_STATE_ERROR && error) {
            GetLogFile() << "[PipeWire] Stream error: " << error << std::endl;
        }
    }

    static void OnStreamParamChanged(void* data, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<LinuxPlatformCapture*>(data);
        if (!param || id != SPA_PARAM_Format) {
            return;
        }

        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(self->m_stateMutex);
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

        pw_stream_update_params(self->m_streamState.stream, params, 1);
    }

    static void OnStreamProcess(void* userdata) {
        auto* self = static_cast<LinuxPlatformCapture*>(userdata);
        if (!self->m_streamState.stream) {
            return;
        }

        pw_buffer* buffer = pw_stream_dequeue_buffer(self->m_streamState.stream);
        if (!buffer) {
            return;
        }

        spa_buffer* spaBuffer = buffer->buffer;
        if (spaBuffer && spaBuffer->n_datas > 0) {
            spa_data& data = spaBuffer->datas[0];
            const uint32_t chunkSize = data.chunk ? data.chunk->size : 0;
            if ((data.type == SPA_DATA_DmaBuf || data.type == SPA_DATA_MemFd) && data.fd >= 0) {
                {
                    std::lock_guard<std::mutex> lock(self->m_stateMutex);
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
                std::lock_guard<std::mutex> lock(self->m_stateMutex);
                self->m_bufferType = static_cast<uint32_t>(data.type);
                self->m_chunkSize = chunkSize;
            }

            if (data.type != SPA_DATA_DmaBuf && data.type != SPA_DATA_MemFd && !self->m_loggedNonDmabuf) {
                self->m_loggedNonDmabuf = true;
                GetLogFile() << "[PipeWire] Non-DMA buffer type received: " << data.type
                          << " (expecting SPA_DATA_DmaBuf=" << SPA_DATA_DmaBuf << ")" << std::endl;
            }

            if (data.type == SPA_DATA_DmaBuf) {
                GetLogFile() << "[PipeWire] Frame DMA-BUF: chunk_size=" << chunkSize << " maxsize=" << data.maxsize << std::endl;
            } else if (data.type == SPA_DATA_MemFd) {
                GetLogFile() << "[PipeWire] Frame MemFd: chunk_size=" << chunkSize << " maxsize=" << data.maxsize << std::endl;
            } else if (data.type == SPA_DATA_MemPtr) {
                GetLogFile() << "[PipeWire] Frame MemPtr: chunk_size=" << chunkSize << " maxsize=" << data.maxsize << std::endl;
            }
        }

        pw_stream_queue_buffer(self->m_streamState.stream, buffer);
    }

    static void OnPortalResponse(
        GDBusConnection*,
        const gchar*,
        const gchar*,
        const gchar*,
        const gchar*,
        GVariant* parameters,
        gpointer userData) {
        auto* self = static_cast<LinuxPlatformCapture*>(userData);

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
            GetLogFile() << "[Portal] Response error: " << responseCode << std::endl;
            freeResults();
            if (self->m_glibLoop) {
                g_main_loop_quit(self->m_glibLoop);
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
                            if (props) {
                                g_variant_unref(props);
                            }
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
                g_main_loop_quit(self->m_glibLoop);
            }
        } catch (const std::exception& e) {
            freeResults();
            GetLogFile() << "[Portal] " << e.what() << std::endl;
            if (self->m_glibLoop) {
                g_main_loop_quit(self->m_glibLoop);
            }
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
