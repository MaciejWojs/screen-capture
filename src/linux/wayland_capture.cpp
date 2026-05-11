#include "wayland_capture.hpp"
#include "wayland_helpers.hpp"
#include "../pixel_conversion.hpp"
#include "../logger.hpp"
#include <spa/utils/result.h>
#include <format>

using namespace std::literals;

WaylandPlatformCapture::~WaylandPlatformCapture() = default;

void WaylandPlatformCapture::Start(Napi::Env) {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        return;
    }
    m_worker = std::jthread([this](std::stop_token stopToken) {
        RunCaptureFlow(stopToken);
        });
}

void WaylandPlatformCapture::Stop() {
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
        sc_logger::Debug("Wayland: Partial update using {} dirty rects", frame->damage.size());
        uint32_t dstStride = handle.width * 4;
        for (const auto& rect : frame->damage) {
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

int WaylandPlatformCapture::GetWidth() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return m_streamConfig ? static_cast<int>(m_streamConfig->width) : 0;
}

int WaylandPlatformCapture::GetHeight() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return m_streamConfig ? static_cast<int>(m_streamConfig->height) : 0;
}

int WaylandPlatformCapture::GetStride() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return static_cast<int>(m_stride);
}

uint32_t WaylandPlatformCapture::GetPixelFormat() const {
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return m_streamConfig ? m_streamConfig->pixelFormat : 0;
}

std::optional<SharedHandleInfo> WaylandPlatformCapture::GetSharedHandle() const {
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

std::string WaylandPlatformCapture::GetBackendName() const {
    return "wayland";
}

void WaylandPlatformCapture::SetExternalPortalSession(
    const std::optional<std::string>& sessionHandle,
    std::optional<int> pipewireRemoteFd,
    std::optional<std::vector<MonitorMetadata>> portalMonitors) {
    std::unique_lock<std::shared_mutex> lock(m_stateMutex);
    m_externalSessionHandle = sessionHandle;
    m_externalPipewireFd = pipewireRemoteFd;
    if (portalMonitors && !portalMonitors->empty()) {
        std::vector<MonitorInfo> monitors;
        monitors.reserve(portalMonitors->size());
        for (const auto& monitor : *portalMonitors) {
            MonitorInfo nativeMonitor;
            nativeMonitor.x = monitor.x;
            nativeMonitor.y = monitor.y;
            nativeMonitor.width = monitor.width > 0 ? static_cast<uint32_t>(monitor.width) : 0;
            nativeMonitor.height = monitor.height > 0 ? static_cast<uint32_t>(monitor.height) : 0;
            nativeMonitor.title = monitor.name;
            if (monitor.pipewireStream.has_value()) {
                nativeMonitor.nodeId = *monitor.pipewireStream;
            } else if (!monitor.id.empty()) {
                try {
                    nativeMonitor.nodeId = static_cast<uint32_t>(std::stoul(monitor.id));
                } catch (...) {
                    nativeMonitor.nodeId = PW_ID_ANY;
                }
            }
            monitors.push_back(std::move(nativeMonitor));
        }
        m_externalMonitors = std::move(monitors);
    } else {
        m_externalMonitors.reset();
    }
}

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

void WaylandPlatformCapture::RunCaptureFlow(std::stop_token stopToken) {
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
            if (m_externalSessionHandle && !m_externalSessionHandle->empty()) {
                m_sessionHandle = *m_externalSessionHandle;
            }
        }

        if (!m_sessionHandle.empty() && m_externalSessionHandle) {
            try {
                sc_logger::Info("Wayland capture: Reusing external portal session handle: {}", m_sessionHandle);
                const bool waitingForStartResponse = StartSession();
                m_stage = PortalStage::StartingSession;
                shouldRunPortalFlow = waitingForStartResponse;
                if (!waitingForStartResponse) {
                    bool hasExternalFd = false;
                    {
                        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
                        hasExternalFd = m_externalPipewireFd.has_value() && *m_externalPipewireFd >= 0;
                    }
                    if (!hasExternalFd) {
                        sc_logger::Info("Wayland capture: Start already active on reused session, opening PipeWire remote directly");
                        OpenPipeWireRemote();
                    } else {
                        sc_logger::Info("Wayland capture: Start already active; skipping OpenPipeWireRemote because external FD is provided");
                    }
                }
            } catch (const std::exception& e) {
                sc_logger::Warn("Wayland: external portal session failed ({}), creating own session", e.what());
                std::unique_lock<std::shared_mutex> lock(m_stateMutex);
                m_sessionHandle.clear();
            }
        }

        bool monitorSeededFromExternal = false;
        size_t seededMonitorCount = 0;
        {
            std::unique_lock<std::shared_mutex> lock(m_stateMutex);
            if (m_externalPipewireFd && *m_externalPipewireFd >= 0) {
                pipewireFd = dup(*m_externalPipewireFd);
                m_pendingPipewireFd.store(pipewireFd);
                sc_logger::Info("Wayland capture: Using external PipeWire FD: {}", pipewireFd);
                shouldRunPortalFlow = false;

                if (m_externalMonitors && !m_externalMonitors->empty()) {
                    m_monitors = *m_externalMonitors;
                    m_currentMonitorIndex.store(0);
                    m_streamNodeId.store(m_monitors[0].nodeId);
                    monitorSeededFromExternal = true;
                    seededMonitorCount = m_monitors.size();
                }
            }
        }
        if (monitorSeededFromExternal) {
            sc_logger::Info("Wayland capture: Seeded {} monitor(s) from external metadata", static_cast<int>(seededMonitorCount));
            InvokeMonitorChangedCallback();
        }

        if (shouldRunPortalFlow) {
            if (m_sessionHandle.empty()) {
                GVariantBuilderWrapper builder;
                g_variant_builder_add(builder, "{sv}", "session_handle_token", g_variant_new_string("sc_session"));
                g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string("sc_create"));

                m_stage = PortalStage::CreatingSession;
                CallPortalMethod("CreateSession", g_variant_new("(a{sv})", static_cast<GVariantBuilder*>(builder)));
            }
            g_main_loop_run(m_glibLoop.get());
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

bool WaylandPlatformCapture::CallPortalMethod(const char* method, GVariant* params, const char* interfaceName) {
    GError* rawError = nullptr;
    GVariantPtr result(g_dbus_connection_call_sync(
        m_connection.get(),
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        interfaceName,
        method,
        params,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &rawError));

    GErrorPtr error(rawError);

    if (!result) {
        if (std::string(method) == "Start" && error && 
            (g_error_matches(error.get(), G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED) || 
             std::string(error->message).find("Invalid session") != std::string::npos)) {
            return false;
        }
        std::string message = error ? error->message : "Unknown portal error";
        throw std::runtime_error(message);
    }
    return true;
}

void WaylandPlatformCapture::StopCurrentPipewireStream() {
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

void WaylandPlatformCapture::RecreatePipewireStream(uint32_t targetNodeId) {
    std::lock_guard<std::shared_mutex> lock(m_stateMutex);
    StopCurrentPipewireStream();
    CleanupSharedHandleLocked();
    CreatePipewireStream(targetNodeId);
}

void WaylandPlatformCapture::CreatePipewireStream(uint32_t targetNodeId) {
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

bool WaylandPlatformCapture::StartSession() {
    GVariantBuilderWrapper builder;
    m_stage = PortalStage::StartingSession;
    g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string("sc_start"));
    std::string handle;
    {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        handle = m_sessionHandle;
    }
    const char* iface = m_externalSessionHandle ? "org.freedesktop.portal.RemoteDesktop" 
                                               : "org.freedesktop.portal.ScreenCast";
    return CallPortalMethod("Start", g_variant_new("(osa{sv})", handle.c_str(), "", static_cast<GVariantBuilder*>(builder)), iface);
}

void WaylandPlatformCapture::SelectSources() {
    GVariantBuilderWrapper builder;
    g_variant_builder_add(builder, "{sv}", "types", g_variant_new_uint32(1));
    g_variant_builder_add(builder, "{sv}", "multiple", g_variant_new_boolean(TRUE));
    g_variant_builder_add(builder, "{sv}", "cursor_mode", g_variant_new_uint32(2));
    g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string("sc_sources"));

    m_stage = PortalStage::SelectingSources;
    std::string handle;
    {
        std::shared_lock<std::shared_mutex> lock(m_stateMutex);
        handle = m_sessionHandle;
    }
    CallPortalMethod("SelectSources", g_variant_new("(oa{sv})", handle.c_str(), static_cast<GVariantBuilder*>(builder)));
}

void WaylandPlatformCapture::OpenPipeWireRemote() {
    GVariantBuilderWrapper builder;

    if (m_pendingPipewireFd.load() >= 0) {
        sc_logger::Info("Wayland capture: Skipping OpenPipeWireRemote, using external FD");
        m_stage = PortalStage::OpeningRemote;
        g_main_loop_quit(m_glibLoop.get());
        return;
    }

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

void WaylandPlatformCapture::StartPipewireStream(int& pipewireFd) {
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

void WaylandPlatformCapture::CleanupPortal() {
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

void WaylandPlatformCapture::CleanupPipewire() {
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

void WaylandPlatformCapture::CleanupSharedHandleLocked() {
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

void WaylandPlatformCapture::CleanupSharedHandle() {
    std::unique_lock<std::shared_mutex> lock(m_stateMutex);
    CleanupSharedHandleLocked();
}

void WaylandPlatformCapture::PublishSharedHandleLocked() {
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

void WaylandPlatformCapture::UpdateSharedHandleFromFd(int fd, std::vector<IntRect> damage, bool fullUpdate) {
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

    m_frameBuffers.PushFrame(sfd, handle, std::move(damage), fullUpdate);
    PublishSharedHandleLocked();
}

void WaylandPlatformCapture::OnStreamStateChanged(void*, pw_stream_state oldState, pw_stream_state state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR && error) {
        sc_logger::Error("PipeWire stream error: {}", error);
    }
}

void WaylandPlatformCapture::OnStreamParamChanged(void* data, uint32_t id, const spa_pod* param) {
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

    params[2] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_ParamMeta,
        SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region))));

    if (!params[2]) {
        sc_logger::Warn("Failed to build meta update param (possible buffer overflow). Damage tracking disabled.");
        pw_stream_update_params(self->m_streamState.stream.get(), params, 2);
    } else {
        pw_stream_update_params(self->m_streamState.stream.get(), params, 3);
    }
}

void WaylandPlatformCapture::OnStreamProcess(void* userdata) {
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

void WaylandPlatformCapture::OnPortalResponse(
    GDBusConnection*,
    const gchar* senderName,
    const gchar* objectPath,
    const gchar* interfaceName,
    const gchar* signalName,
    GVariant* parameters,
    gpointer userData) {
    auto* self = static_cast<WaylandPlatformCapture*>(userData);

    guint32 responseCode = 1;
    GVariantIter* results = nullptr;
    g_variant_get(parameters, "(ua{sv})", &responseCode, &results);

    const bool isCreateResp = g_str_has_suffix(objectPath, "sc_create");
    const bool isSourcesResp = g_str_has_suffix(objectPath, "sc_sources");
    const bool isStartResp = g_str_has_suffix(objectPath, "sc_start") || g_str_has_suffix(objectPath, "ib_start");
    const bool isClipboardResp = g_str_has_suffix(objectPath, "sc_clipboard") || g_str_has_suffix(objectPath, "ib_clipboard");
    
    if (!isCreateResp && !isSourcesResp && !isStartResp && !isClipboardResp) {
        if (results) g_variant_iter_free(results);
        return;
    }

    auto freeResults = [&results]() {
        if (results) {
            g_variant_iter_free(results);
            results = nullptr;
        }
    };

    if (responseCode != 0 && (isCreateResp || isSourcesResp || isStartResp)) {
        freeResults();
        if (self->m_glibLoop) {
            g_main_loop_quit(self->m_glibLoop.get());
            GMainContext* ctx = g_main_loop_get_context(self->m_glibLoop.get());
            if (ctx) g_main_context_wakeup(ctx);
        }
        return;
    }

    try {
        if (isCreateResp) {
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

        if (isSourcesResp) {
            freeResults();
            self->StartSession();
            return;
        }

        if (isStartResp) {
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

const pw_stream_events WaylandPlatformCapture::kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = WaylandPlatformCapture::OnStreamStateChanged;
    events.param_changed = WaylandPlatformCapture::OnStreamParamChanged;
    events.process = WaylandPlatformCapture::OnStreamProcess;
    return events;
}();

