#pragma once

#include "../platform_capture.hpp"
#include "linux_raii.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <stop_token>
#include <thread>
#include <string>
#include <utility>
#include <vector>

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
    std::function<void(const std::vector<ConfigurationChange>&)> m_configurationChangedCallback;
    std::mutex m_configurationChangedCallbackMutex;
    std::vector<MonitorMetadata> m_previousMonitors;

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

    void SetConfigurationChangedCallback(std::function<void(const std::vector<ConfigurationChange>&)> callback) override {
        std::lock_guard<std::mutex> lock(m_configurationChangedCallbackMutex);
        m_configurationChangedCallback = std::move(callback);
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

    void InvokeConfigurationChangedCallback(const std::vector<ConfigurationChange>& changes) {
        std::function<void(const std::vector<ConfigurationChange>&)> callback;
        {
            std::lock_guard<std::mutex> lock(m_configurationChangedCallbackMutex);
            callback = m_configurationChangedCallback;
        }
        if (callback && !changes.empty()) {
            callback(changes);
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
