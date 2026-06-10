#pragma once

#include <filesystem>
#include <string>

namespace ibom::utils {

/**
 * @brief Root directory for all persistent application data.
 *
 * Single source of truth for config.json, calibration.yml, snapshots and the
 * TensorRT engine cache, so that one mounted Docker volume persists everything.
 *
 * Resolution order:
 *   1. $IBOM_DATA_DIR if set (used by the Docker images — see compose.yml).
 *   2. ~/.local/share/MicroscopeIBOM — matches Qt's
 *      QStandardPaths::AppDataLocation on Linux.
 *   3. Current working directory as a last resort.
 *
 * The directory is created if it does not exist.
 */
std::filesystem::path dataDir();

/// dataDir()/sub, created if needed. E.g. dataSubDir("tensorrt-cache").
std::filesystem::path dataSubDir(const std::string& sub);

} // namespace ibom::utils
