#include "Config.h"
#include "utils/Paths.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace ibom {

namespace fs = std::filesystem;

Config::Config() = default;

void Config::addRecentIbomFile(const std::string& path)
{
    if (path.empty()) return;
    auto it = std::find(m_recentIbomFiles.begin(), m_recentIbomFiles.end(), path);
    if (it != m_recentIbomFiles.end())
        m_recentIbomFiles.erase(it);
    m_recentIbomFiles.insert(m_recentIbomFiles.begin(), path);
    if (m_recentIbomFiles.size() > 5)
        m_recentIbomFiles.resize(5);
}

std::string Config::defaultConfigPath() const
{
    // Unified data dir (honors $IBOM_DATA_DIR) — shared with calibration,
    // snapshots and the TensorRT cache. See src/utils/Paths.h.
    return (utils::dataDir() / "config.json").string();
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
            m_cameraHwDecode = cam.value("hw_decode", m_cameraHwDecode);
        }

        // iBOM
        m_ibomFilePath   = j.value("ibom_file", m_ibomFilePath);
        m_autoReloadIbom = j.value("ibom_auto_reload", m_autoReloadIbom);
        if (j.contains("ibom_recent") && j["ibom_recent"].is_array()) {
            m_recentIbomFiles.clear();
            for (const auto& e : j["ibom_recent"])
                if (e.is_string()) m_recentIbomFiles.push_back(e.get<std::string>());
        }

        // AI
        if (j.contains("ai")) {
            auto& ai = j["ai"];
            m_modelsPath          = ai.value("models_path", m_modelsPath);
            m_aiEnabled           = ai.value("enabled", m_aiEnabled);
            m_detectorModel       = ai.value("detector_model", m_detectorModel);
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
            m_trackingDownscale  = trk.value("downscale", m_trackingDownscale);

            // Migration: legacy match_distance_ratio values were distance multipliers
            // (typically 2.0); the new semantics is Lowe's ratio test in [0,1].
            if (m_matchDistanceRatio >= 1.0) {
                spdlog::info("Migrating legacy match_distance_ratio {} → Lowe ratio 0.75",
                             m_matchDistanceRatio);
                m_matchDistanceRatio = 0.75;
            }
        }

        // Calibration
        if (j.contains("calibration")) {
            auto& cal = j["calibration"];
            m_calibBoardCols  = cal.value("board_cols", m_calibBoardCols);
            m_calibBoardRows  = cal.value("board_rows", m_calibBoardRows);
            m_calibSquareSize = cal.value("square_size_mm", m_calibSquareSize);
            m_scaleMethod     = static_cast<ScaleMethod>(cal.value("scale_method",
                                    static_cast<int>(m_scaleMethod)));
            m_opticalMultiplier = cal.value("optical_multiplier", m_opticalMultiplier);
        }

        // BOM
        if (j.contains("bom") && j["bom"].contains("checkbox_columns")) {
            m_checkboxColumns = j["bom"]["checkbox_columns"]
                .get<std::vector<std::string>>();
        }

        // Inspection
        if (j.contains("inspection")) {
            auto& insp = j["inspection"];
            m_sortMethod = static_cast<SortMethod>(insp.value("sort_method",
                                static_cast<int>(m_sortMethod)));
            m_selectedColorHex     = insp.value("color_selected",      m_selectedColorHex);
            m_placedColorHex       = insp.value("color_placed",        m_placedColorHex);
            m_normalColorHex       = insp.value("color_normal",        m_normalColorHex);
            m_placedOpacity        = insp.value("placed_opacity",      m_placedOpacity);
            m_selectedOutlineWidth = insp.value("selected_outline_px", m_selectedOutlineWidth);
        }

        // Dataset capture
        if (j.contains("dataset")) {
            auto& ds = j["dataset"];
            m_datasetMinInliers         = ds.value("min_inliers", m_datasetMinInliers);
            m_datasetMaxReprojErrPx     = ds.value("max_reproj_err_px", m_datasetMaxReprojErrPx);
            m_datasetMinSharpness       = ds.value("min_sharpness", m_datasetMinSharpness);
            m_datasetMaxBadExposureFrac = ds.value("max_bad_exposure_frac", m_datasetMaxBadExposureFrac);
            m_datasetMaxHomographyAgeMs = ds.value("max_homography_age_ms", m_datasetMaxHomographyAgeMs);
            m_datasetSaveIntervalMs     = ds.value("save_interval_ms", m_datasetSaveIntervalMs);
            m_datasetMinPoseDeltaPx     = ds.value("min_pose_delta_px", m_datasetMinPoseDeltaPx);
            m_datasetBboxShrink         = ds.value("bbox_shrink", m_datasetBboxShrink);
            m_datasetMinBoxPx           = ds.value("min_box_px", m_datasetMinBoxPx);
            m_datasetMinVisibleFrac     = ds.value("min_visible_frac", m_datasetMinVisibleFrac);
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
            {"fps",    m_cameraFps},
            {"hw_decode", m_cameraHwDecode}
        };

        // iBOM
        j["ibom_file"]        = m_ibomFilePath;
        j["ibom_recent"]      = m_recentIbomFiles;
        j["ibom_auto_reload"] = m_autoReloadIbom;

        // AI
        j["ai"] = {
            {"models_path",    m_modelsPath},
            {"enabled",        m_aiEnabled},
            {"detector_model", m_detectorModel},
            {"use_tensorrt",   m_useTensorRT},
            {"confidence",     m_detectionConfidence}
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
            {"ransac_threshold",     m_ransacThreshold},
            {"downscale",            m_trackingDownscale}
        };

        // Calibration
        j["calibration"] = {
            {"board_cols",     m_calibBoardCols},
            {"board_rows",     m_calibBoardRows},
            {"square_size_mm", m_calibSquareSize},
            {"scale_method",   static_cast<int>(m_scaleMethod)},
            {"optical_multiplier", m_opticalMultiplier}
        };

        // BOM
        j["bom"] = {
            {"checkbox_columns", m_checkboxColumns}
        };

        // Inspection
        j["inspection"] = {
            {"sort_method",         static_cast<int>(m_sortMethod)},
            {"color_selected",      m_selectedColorHex},
            {"color_placed",        m_placedColorHex},
            {"color_normal",        m_normalColorHex},
            {"placed_opacity",      m_placedOpacity},
            {"selected_outline_px", m_selectedOutlineWidth}
        };

        // Dataset capture
        j["dataset"] = {
            {"min_inliers",           m_datasetMinInliers},
            {"max_reproj_err_px",     m_datasetMaxReprojErrPx},
            {"min_sharpness",         m_datasetMinSharpness},
            {"max_bad_exposure_frac", m_datasetMaxBadExposureFrac},
            {"max_homography_age_ms", m_datasetMaxHomographyAgeMs},
            {"save_interval_ms",      m_datasetSaveIntervalMs},
            {"min_pose_delta_px",     m_datasetMinPoseDeltaPx},
            {"bbox_shrink",           m_datasetBboxShrink},
            {"min_box_px",            m_datasetMinBoxPx},
            {"min_visible_frac",      m_datasetMinVisibleFrac}
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
