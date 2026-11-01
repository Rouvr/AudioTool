#pragma once
// ============================================================================
// AudioTool (AT) — Robust Diagnostic Logging & Crash Triage Subsystem
//
// Features:
//   - Structured formatting: [TIMESTAMP] [THREAD_ID] [LEVEL] [SUBSYSTEM] [FILE:LINE] message
//   - Dual-file persistence: timestamped log (audiotool_YYYY-MM-DD_HH-MM-SS.log) + latest.log
//   - Configurable X log retention policy (rotates out oldest logs beyond X on initialization)
//   - Thread-safe output with immediate auto-flush on WARN, ERROR, FATAL, and CRASH
//   - Windows SEH Unhandled Exception Filter capturing access violations, faulting addresses,
//     module offsets, and CPU context registers for post-mortem debugging
//   - Zero-overhead in audio realtime threads (audio processing/ticks are NEVER logged)
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace at {

enum class LogLevel : uint8_t {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4
};

class Logger {
public:
    static Logger& instance();

    // Initializes logging, rotates old logs to keep at most max_retained_logs, opens latest.log
    bool init(uint32_t max_retained_logs = 10, LogLevel min_level = LogLevel::Debug);

    // Installs Windows SEH unhandled exception filter for crash diagnosis
    void install_crash_handler();

    // Flushes all buffered log lines to disk
    void flush();

    // Shuts down logger and closes file handles
    void shutdown();

    void set_min_level(LogLevel level);
    LogLevel get_min_level() const;

    // Core log submission method
    void log(LogLevel level, const char* subsystem, const char* file, int line, const std::string& message);

    // Formats and logs a message using snprintf formatting
    void log_format(LogLevel level, const char* subsystem, const char* file, int line, const char* fmt, ...);

    std::string get_current_log_path() const;
    std::string get_latest_log_path() const;

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace at

// Convenient Logging Macros
#define AT_LOG_DEBUG(subsystem, ...) ::at::Logger::instance().log_format(::at::LogLevel::Debug, subsystem, __FILE__, __LINE__, __VA_ARGS__)
#define AT_LOG_INFO(subsystem, ...)  ::at::Logger::instance().log_format(::at::LogLevel::Info,  subsystem, __FILE__, __LINE__, __VA_ARGS__)
#define AT_LOG_WARN(subsystem, ...)  ::at::Logger::instance().log_format(::at::LogLevel::Warn,  subsystem, __FILE__, __LINE__, __VA_ARGS__)
#define AT_LOG_ERROR(subsystem, ...) ::at::Logger::instance().log_format(::at::LogLevel::Error, subsystem, __FILE__, __LINE__, __VA_ARGS__)
#define AT_LOG_FATAL(subsystem, ...) ::at::Logger::instance().log_format(::at::LogLevel::Fatal, subsystem, __FILE__, __LINE__, __VA_ARGS__)

