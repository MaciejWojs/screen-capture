#pragma once

#include "platform_capture_base.hpp"
#include "wayland_helpers.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <optional>
#include <string>
#include <vector>

class WaylandPlatformCapture final : public BaseLinuxPlatformCapture {
    public:
    WaylandPlatformCapture() = default;
    ~WaylandPlatformCapture() override;

    void Start(Napi::Env env) override;
    void Stop() override;
    std::optional<std::vector<uint8_t>> GetPixelData(std::string_view desiredFormat = "rgba") const override;
    int GetWidth() const override;
    int GetHeight() const override;
    int GetStride() const override;
    uint32_t GetPixelFormat() const override;

    std::optional<SharedHandleInfo> GetSharedHandle() const override;

    std::string GetBackendName() const override;

    void SetExternalPortalSession(
        const std::optional<std::string>& sessionHandle,
        std::optional<int> pipewireRemoteFd,
        std::optional<std::vector<MonitorMetadata>> portalMonitors = std::nullopt) override;

    int GetMonitorCount() const override;
    std::vector<MonitorMetadata> GetMonitors() const override;
    int GetCurrentMonitorIndex() const override;
    void NextMonitor() override;
    void SelectMonitor(int index) override;
    std::optional<MonitorMetadata> GetCurrentMonitorInfo() const override;

    private:
    // Internal helpers will be defined in wayland_capture.cpp
    GMainLoopPtr m_glibLoop;
    GDBusConnectionPtr m_connection;

    StreamState m_streamState{};
    std::atomic<PortalStage> m_stage{ PortalStage::Idle };

    std::string m_sessionHandle;
    std::optional<std::string> m_externalSessionHandle;
    std::optional<int> m_externalPipewireFd;
    std::optional<std::vector<MonitorInfo>> m_externalMonitors;
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

    void RunCaptureFlow(std::stop_token stopToken);
    bool CallPortalMethod(const char* method, GVariant* params, const char* interfaceName = "org.freedesktop.portal.ScreenCast");
    void StopCurrentPipewireStream();
    void RecreatePipewireStream(uint32_t targetNodeId);
    void CreatePipewireStream(uint32_t targetNodeId);
    bool StartSession();
    void SelectSources();
    void OpenPipeWireRemote();
    void StartPipewireStream(int& pipewireFd);
    void CleanupPortal();
    void CleanupPipewire();
    void CleanupSharedHandleLocked();
    void CleanupSharedHandle();
    void PublishSharedHandleLocked();
    void UpdateSharedHandleFromFd(int fd, std::vector<IntRect> damage, bool fullUpdate);

    static void OnStreamStateChanged(void*, pw_stream_state oldState, pw_stream_state state, const char* error);
    static void OnStreamParamChanged(void* data, uint32_t id, const spa_pod* param);
    static void OnStreamProcess(void* userdata);
    static void OnPortalResponse(
        GDBusConnection*,
        const gchar* senderName,
        const gchar* objectPath,
        const gchar* interfaceName,
        const gchar* signalName,
        GVariant* parameters,
        gpointer userData);
    static void OnSessionClosed(
        GDBusConnection*,
        const gchar* senderName,
        const gchar* objectPath,
        const gchar* interfaceName,
        const gchar* signalName,
        GVariant* parameters,
        gpointer userData);

    void SubscribeToSessionClosed();
    void HandleSessionClosed();

    static const pw_stream_events kStreamEvents;
};
