#include "Config.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>

namespace ibom {

namespace fs = std::filesystem;

Config::Config() = default;

std::string Config::defaultConfigPath() const
{
    // Store config next to the executable or in user's appdata
#ifdef IBOM_PLATFORM_WINDOWS
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        fs::path dir = fs::path(appdata) / "MicroscopeIBOM";
        fs::create_directories(dir);
        return (dir / "config.json").string();
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        fs::path dir = fs::path(home) / ".config" / "MicroscopeIBOM";
        fs::create_directories(dir);
        return (dir / "config.json").string();
    }
#endif
    return "config.json";
}

bool Config::load(const std::string& path)
{
    std::string filePath = path.empty() ? defaultConfigPath() : path;

    if (!fs::exists(filePath)) {
        spdlog::info("No config file found at '{}', using defaults.", filePath);
        save(filePath); // Create default config
        return true;
    }

    try {
        std::ifstream ifs(filePath);
        nlohmann::json j = nlohmann::json::parse(ifs);

        // Camera
        if (j.contains("camera")) {
            auto& cam = j["camera"];
            m_cameraIndex  = cam.value("index", m_cameraIndex);
            m_cameraWidth  = cam.value("width", m_cameraWidth);
            m_cameraHeight = cam.value("height", m_cameraHeight);
            m_cameraFps    = cam.value("fps", m_cameraFps);
        }

        // iBOM
        m_ibomFilePath = j.value("ibom_file", m_ibomFilePath);

        // AI
        if (j.contains("ai")) {
            auto& ai = j["ai"];
            m_modelsPath          = ai.value("models_path", m_modelsPath);
            m_useTensorRT         = ai.value("use_tensorrt", m_useTensorRT);
            m_detectionConfidence = ai.value("confidence", m_detectionConfidence);
        }

        // UI
        if (j.contains("ui")) {
            auto& ui = j["ui"];
            m_darkMode       = ui.value("dark_mode", m_darkMode);
            m_overlayOpacity = ui.value("overlay_opacity", m_overlayOpacity);
            m_showPads       = ui.value("show_pads", m_showPads);
            m_showSilkscreen = ui.value("show_silkscreen", m_showSilkscreen);
            m_showFabrication = ui.value("show_fabrication", m_showFabrication);
        }

        // Features
        if (j.contains("features")) {
            auto& feat = j["features"];
            m_voiceControl   = feat.value("voice_control", m_voiceControl);
            m_remoteView     = feat.value("remote_view", m_remoteView);
            m_remoteViewPort = feat.value("remote_view_port", m_remoteViewPort);
        }

        // Tracking
        if (j.contains("tracking")) {
            auto& trk = j["tracking"];
            m_trackingIntervalMs = trk.value("interval_ms", m_trackingIntervalMs);
            m_orbKeypoints       = trk.value("orb_keypoints", m_orbKeypoints);
            m_minMatchCount      = trk.value("min_matches", m_minMatchCount);
            m_matchDistanceRatio = trk.value("match_distance_ratio", m_matchDistanceRatio);
            m_ransacThreshold    = trk.value("ransac_threshold", m_ransacThreshold);
        }

        // Calibration
        if (j.contains("calibration")) {
            auto& cal = j["calibration"];
            m_calibBoardCols  = cal.value("board_cols", m_calibBoardCols);
            m_calibBoardRows  = cal.value("board_rows", m_calibBoardRows);
            m_calibSquareSize = cal.value("square_size_mm", m_calibSquareSize);
            m_scaleMethod     = static_cast<ScaleMethod>(cal.value("scale_method",
                                    static_cast<int>(m_scaleMethod)));
        }

        // BOM
        if (j.contains("bom") && j["bom"].contains("checkbox_columns")) {
            m_checkboxColumns = j["bom"]["checkbox_columns"]
                .get<std::vector<std::string>>();
        }

        spdlog::info("Config loaded from '{}'", filePath);
        return true;

    } catch (const std::exception& ex) {
        spdlog::error("Failed to load config from '{}': {}", filePath, ex.what());
        return false;
    }
}

bool Config::save(const std::string& path) const
{
    std::string filePath = path.empty() ? defaultConfigPath() : path;

    try {
        nlohmann::json j;

        // Camera
        j["camera"] = {
            {"index",  m_cameraIndex},
            {"width",  m_cameraWidth},
            {"height", m_cameraHeight},
            {"fps",    m_cameraFps}
        };

        // iBOM
        j["ibom_file"] = m_ibomFilePath;

        // AI
        j["ai"] = {
            {"models_path",  m_modelsPath},
            {"use_tensorrt", m_useTensorRT},
            {"confidence",   m_detectionConfidence}
        };

        // UI
        j["ui"] = {
            {"dark_mode",       m_darkMode},
            {"overlay_opacity", m_overlayOpacity},
            {"show_pads",       m_showPads},
            {"show_silkscreen", m_showSilkscreen},
            {"show_fabrication", m_showFabrication}
        };

        // Features
        j["features"] = {
            {"voice_control",    m_voiceControl},
            {"remote_view",      m_remoteView},
            {"remote_view_port", m_remoteViewPort}
        };

        // Tracking
        j["tracking"] = {
            {"interval_ms",          m_trackingIntervalMs},
            {"orb_keypoints",        m_orbKeypoints},
            {"min_matches",          m_minMatchCount},
            {"match_distance_ratio", m_matchDistanceRatio},
            {"ransac_threshold",     m_ransacThreshold}
        };

        // Calibration
        j["calibration"] = {
            {"board_cols",     m_calibBoardCols},
            {"board_rows",     m_calibBoardRows},
            {"square_size_mm", m_calibSquareSize},
            {"scale_method",   static_cast<int>(m_scaleMethod)}
        };

        // BOM
        j["bom"] = {
            {"checkbox_columns", m_checkboxColumns}
        };

        std::ofstream ofs(filePath);
        ofs << j.dump(4);

        spdlog::debug("Config saved to '{}'", filePath);
        return true;

    } catch (const std::exception& ex) {
        spdlog::error("Failed to save config to '{}': {}", filePath, ex.what());
        return false;
    }
}

} // namespace ibom
