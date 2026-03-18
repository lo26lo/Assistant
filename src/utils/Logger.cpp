#include "Logger.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>
#include <vector>

namespace ibom::utils {

std::string Logger::s_logFilePath;

void Logger::initialize(const std::string& appName,
                        const std::string& logDir,
                        spdlog::level::level_enum level)
{
    try {
        // Ensure log directory exists
        if (!logDir.empty()) {
            std::filesystem::create_directories(logDir);
        }

        // Build log file path
        s_logFilePath = logDir.empty()
            ? appName + ".log"
            : logDir + "/" + appName + ".log";

        // Create sinks
        std::vector<spdlog::sink_ptr> sinks;

        // Console sink (colored)
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(level);
        console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
        sinks.push_back(console_sink);

        // File sink (rotating, 5MB max, 3 files)
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            s_logFilePath, 5 * 1024 * 1024, 3);
        file_sink->set_level(spdlog::level::trace);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
        sinks.push_back(file_sink);

        // Create and register default logger
        auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());
        logger->set_level(level);
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(logger);
        spdlog::info("Logger initialized — file: {}", s_logFilePath);

    } catch (const spdlog::spdlog_ex& ex) {
        // Fallback to stderr
        fprintf(stderr, "Logger initialization failed: %s\n", ex.what());
    }
}

void Logger::setLevel(spdlog::level::level_enum level)
{
    spdlog::set_level(level);
}

std::string Logger::logFilePath()
{
    return s_logFilePath;
}

void Logger::flush()
{
    spdlog::default_logger()->flush();
}

void Logger::shutdown()
{
    spdlog::shutdown();
}

} // namespace ibom::utils
