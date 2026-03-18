#pragma once

#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace ibom::utils {

/// Initialize application logging with file + console sinks
class Logger {
public:
    /// Initialize spdlog with default configuration
    /// @param appName Application name (used for log file naming)
    /// @param logDir  Directory for log files (empty = current dir)
    /// @param level   Default log level
    static void initialize(const std::string& appName = "pcb_inspector",
                           const std::string& logDir  = "logs",
                           spdlog::level::level_enum level = spdlog::level::info);

    /// Set log level at runtime
    static void setLevel(spdlog::level::level_enum level);

    /// Get the log file path
    static std::string logFilePath();

    /// Flush all loggers
    static void flush();

    /// Shutdown logging
    static void shutdown();

private:
    static std::string s_logFilePath;
};

} // namespace ibom::utils
