#pragma once

#include "../platform_capture.hpp"
#include "linux_raii.hpp"

#include <algorithm>
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
    std::mutex m_configurationStateMutex;
    std::jthread m_configurationWatcher;
    std::mutex m_configurationWatcherMutex;

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
        {
            std::lock_guard<std::mutex> lock(m_configurationChangedCallbackMutex);
            m_configurationChangedCallback = std::move(callback);
        }

        if (!m_configurationChangedCallback) {
            {
                std::lock_guard<std::mutex> stateLock(m_configurationStateMutex);
                m_previousMonitors.clear();
            }
            std::lock_guard<std::mutex> watcherLock(m_configurationWatcherMutex);
            if (m_configurationWatcher.joinable()) {
                m_configurationWatcher.request_stop();
                m_configurationWatcher = {};
            }
            return;
        }

        {
            std::lock_guard<std::mutex> stateLock(m_configurationStateMutex);
            if (m_previousMonitors.empty()) {
                m_previousMonitors = GetMonitors();
            }
        }

        std::lock_guard<std::mutex> watcherLock(m_configurationWatcherMutex);
        if (m_configurationWatcher.joinable()) {
            return;
        }

        m_configurationWatcher = std::jthread([this](std::stop_token stopToken) {
            while (!stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));

                {
                    std::lock_guard<std::mutex> configLock(m_configurationChangedCallbackMutex);
                    if (!m_configurationChangedCallback) {
                        continue;
                    }
                }

                std::vector<MonitorMetadata> currentMonitors = GetMonitors();
                std::vector<ConfigurationChange> changes;

                {
                    std::lock_guard<std::mutex> stateLock(m_configurationStateMutex);
                    if (m_previousMonitors.empty()) {
                        m_previousMonitors = std::move(currentMonitors);
                        continue;
                    }

                    auto findById = [](const std::vector<MonitorMetadata>& monitors, const std::string& id) {
                        return std::find_if(monitors.begin(), monitors.end(), [&id](const MonitorMetadata& monitor) {
                            return monitor.id == id;
                        });
                    };

                    for (const auto& monitor : currentMonitors) {
                        auto previousIt = findById(m_previousMonitors, monitor.id);
                        if (previousIt == m_previousMonitors.end()) {
                            changes.push_back(ConfigurationChange{
                                ConfigurationChangeType::Added,
                                monitor.id,
                                std::nullopt,
                                monitor.index,
                            });
                            continue;
                        }

                        if (previousIt->enabled != monitor.enabled) {
                            changes.push_back(ConfigurationChange{
                                monitor.enabled ? ConfigurationChangeType::Enabled : ConfigurationChangeType::Disabled,
                                monitor.id,
                                previousIt->index,
                                monitor.index,
                            });
                        }
                    }

                    for (const auto& previous : m_previousMonitors) {
                        auto currentIt = findById(currentMonitors, previous.id);
                        if (currentIt == currentMonitors.end()) {
                            changes.push_back(ConfigurationChange{
                                ConfigurationChangeType::Removed,
                                previous.id,
                                previous.index,
                                std::nullopt,
                            });
                        }
                    }

                    m_previousMonitors = std::move(currentMonitors);
                }

                if (!changes.empty()) {
                    InvokeConfigurationChangedCallback(changes);
                    InvokeMonitorChangedCallback();
                }
            }
        });
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
