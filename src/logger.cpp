#include "logger.hpp"

#include <iostream>

namespace sc_logger {

    static std::atomic<LogLevel> s_logLevel{ LogLevel::Info };

    void SetLogLevel(LogLevel level) noexcept {
        s_logLevel.store(level, std::memory_order_relaxed);
    }

    LogLevel GetLogLevel() noexcept {
        return s_logLevel.load(std::memory_order_relaxed);
    }

    LogLevel ParseLogLevel(std::string_view levelName) noexcept {
        if (levelName == "none") {
            return LogLevel::None;
        }
        if (levelName == "error") {
            return LogLevel::Error;
        }
        if (levelName == "warn") {
            return LogLevel::Warn;
        }
        if (levelName == "debug") {
            return LogLevel::Debug;
        }
        return LogLevel::Info;
    }

    void LogToDebugOutput(const std::string& message) {
#ifdef _WIN32
        OutputDebugStringA(message.c_str());
#endif
    }

    bool ShouldLog(LogLevel level) noexcept {
        return GetLogLevel() >= level;
    }

} // namespace sc_logger
