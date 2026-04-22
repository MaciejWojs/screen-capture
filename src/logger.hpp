#pragma once

#include <atomic>
#include <format>
#include <iostream>
#include <mutex>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sc_logger {

    enum class LogLevel : int {
        None = 0,
        Error = 1,
        Warn = 2,
        Info = 3,
        Debug = 4,
    };

    void SetLogLevel(LogLevel level) noexcept;
    LogLevel GetLogLevel() noexcept;
    LogLevel ParseLogLevel(std::string_view levelName) noexcept;

    void LogToDebugOutput(const std::string& message);
    bool ShouldLog(LogLevel level) noexcept;

    template <typename... Args>
    void LogMessage(LogLevel level, const char* prefix, std::string_view format, Args&&... args) {
        static std::mutex mutex;
        std::string fmt(format.data(), format.size());
        auto message = std::vformat(fmt, std::make_format_args(args...));
        std::string output = std::format("[ScreenCapture] {}: {}\n", prefix, message);
        std::lock_guard lock(mutex);
        LogToDebugOutput(output);
        if (!ShouldLog(level)) {
            return;
        }
        std::cerr << output;
    }

    template <typename... Args>
    void Error(std::string_view format, Args&&... args) {
        LogMessage(LogLevel::Error, "ERROR", format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Warn(std::string_view format, Args&&... args) {
        LogMessage(LogLevel::Warn, "WARN", format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Info(std::string_view format, Args&&... args) {
        LogMessage(LogLevel::Info, "INFO", format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Debug(std::string_view format, Args&&... args) {
        LogMessage(LogLevel::Debug, "DEBUG", format, std::forward<Args>(args)...);
    }
}