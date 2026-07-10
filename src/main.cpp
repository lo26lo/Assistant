#include "app/Application.h"
#include "utils/Logger.h"
#include "utils/GpuUtils.h"

#include <QApplication>
#include <spdlog/spdlog.h>
#include <iostream>

#include <csignal>
#include <execinfo.h>
#include <unistd.h>
extern "C" void posixCrashHandler(int sig)
{
    void* buf[32];
    int n = backtrace(buf, 32);
    // stderr FIRST: backtrace_symbols_fd is async-signal-safe and works even
    // when the fault happened after spdlog::shutdown(). Logging on the dead
    // default logger re-faulted INSIDE this handler (ERREUR #60), which is why
    // the field exit-crash produced a bare "Segmentation fault" with no trace.
    backtrace_symbols_fd(buf, n, STDERR_FILENO);
    if (auto* logger = spdlog::default_logger_raw()) {
        spdlog::critical("UNHANDLED SIGNAL {} ({})", sig, strsignal(sig));
        logger->flush();
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main(int argc, char* argv[])
{
    std::signal(SIGSEGV, posixCrashHandler);
    std::signal(SIGABRT, posixCrashHandler);
    std::signal(SIGFPE,  posixCrashHandler);
    std::signal(SIGILL,  posixCrashHandler);
    std::signal(SIGBUS,  posixCrashHandler);

    // Initialize logging first
    ibom::utils::Logger::initialize("pcb_inspector", "logs", spdlog::level::info);

    spdlog::info("=== PCB Inspector — iBOM AI Overlay ===");
    spdlog::info("Version 0.1.0");

    // Log GPU info
    ibom::utils::GpuUtils::logGpuInfo();

    // Warm up GPU if available
    if (ibom::utils::GpuUtils::isCudaAvailable()) {
        ibom::utils::GpuUtils::warmUp();
    }

    // Create QApplication FIRST — required before any QObject
    QApplication qapp(argc, argv);
    qapp.setApplicationName("MicroscopeIBOM");
    qapp.setApplicationVersion("0.1.0");
    qapp.setOrganizationName("MicroscopeIBOM");

    // Create and run application. Scoped on purpose: ~Application (and its
    // member destructors — camera capture stop, Config::save, GUI teardown)
    // logs via spdlog, so it must run BEFORE Logger::shutdown(). The previous
    // order (shutdown at end of main, ~Application after it at end of scope)
    // made the very first log on the destroyed default logger segfault at
    // EVERY normal exit (ERREUR #60).
    int result = 0;
    {
        ibom::Application app(qapp);
        if (!app.initialize()) {
            spdlog::error("Application initialization failed");
            result = 1;
        } else {
            spdlog::info("Application started — entering main loop");
            result = qapp.exec();
            spdlog::info("Application exiting with code {}", result);
        }
    }   // ~Application runs here, logger still alive

    ibom::utils::Logger::shutdown();
    return result;
}
