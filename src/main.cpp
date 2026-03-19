#include "app/Application.h"
#include "utils/Logger.h"
#include "utils/GpuUtils.h"

#include <QApplication>
#include <spdlog/spdlog.h>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep)
{
    spdlog::critical("UNHANDLED EXCEPTION 0x{:08X} at address 0x{:016X}",
                     ep->ExceptionRecord->ExceptionCode,
                     reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
    spdlog::default_logger()->flush();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif

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

    // Create and run application
    ibom::Application app(qapp);
    if (!app.initialize()) {
        spdlog::error("Application initialization failed");
        return 1;
    }

    spdlog::info("Application started — entering main loop");
    int result = qapp.exec();

    spdlog::info("Application exiting with code {}", result);
    ibom::utils::Logger::shutdown();

    return result;
}
