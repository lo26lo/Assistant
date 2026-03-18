#include "app/Application.h"
#include "utils/Logger.h"
#include "utils/GpuUtils.h"

#include <QApplication>
#include <spdlog/spdlog.h>
#include <iostream>

int main(int argc, char* argv[])
{
    // Initialize logging first
    ibom::utils::Logger::initialize("pcb_inspector", "logs", spdlog::level::info);

    spdlog::info("=== PCB Inspector — iBOM AI Overlay ===");
    spdlog::info("Version 1.0.0");

    // Log GPU info
    ibom::utils::GpuUtils::logGpuInfo();

    // Warm up GPU if available
    if (ibom::utils::GpuUtils::isCudaAvailable()) {
        ibom::utils::GpuUtils::warmUp();
    }

    // Create Qt application
    QApplication qtApp(argc, argv);
    QApplication::setApplicationName("PCBInspector");
    QApplication::setOrganizationName("PCBInspector");
    QApplication::setApplicationVersion("1.0.0");

    // High DPI support
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // Create and run application
    ibom::Application app;
    if (!app.initialize(argc, argv)) {
        spdlog::error("Application initialization failed");
        return 1;
    }

    spdlog::info("Application started — entering main loop");
    int result = qtApp.exec();

    spdlog::info("Application exiting with code {}", result);
    ibom::utils::Logger::shutdown();

    return result;
}
