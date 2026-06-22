#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "ai/InferenceEngine.h"   // ai::Detection
#include "ibom/IBomData.h"
#include "overlay/Homography.h"

namespace ibom::overlay {

/// Result of a component-level re-anchor (see docs/AI_MODEL_DATASETS_PLAN.md,
/// "Piste B"). Unlike BoardLocator this needs the board to be visible and
/// populated, NOT framed whole — so it works precisely where BoardLocator
/// fails (D405 close-up / microscope, board filling the frame).
struct ComponentReanchorResult {
    bool found = false;

    /// New PCB -> image homography (3x3, CV_64F) when found. Empty otherwise.
    cv::Mat homography;

    int    matches = 0;          ///< component<->detection correspondences kept
    int    inliers = 0;          ///< RANSAC inliers among those matches
    double medianReprojPx = 0.0; ///< median inlier reprojection error (px)

    /// Human-readable diagnostic. Always set (reason for failure or summary).
    std::string message;
};

/// Re-estimates the PCB->image homography from AI component detections by
/// matching each detection to the nearest expected iBOM component position
/// (using the current pose as a prior), then running RANSAC on the resulting
/// point correspondences.
///
/// A *coarse* model is enough: matching is spatial, so even a single-class
/// "component presence" detector (Piste B) drives this. Class information, when
/// available (Piste A), is an optional gating prior — see Params::useClassPrior.
class ComponentReanchor {
public:
    struct Params {
        /// Gating radius (px) around a component's predicted image position.
        /// A detection farther than this from every predicted position is
        /// unmatched. Should comfortably exceed expected drift.
        double maxMatchDistPx = 60.0;

        /// Minimum kept correspondences before attempting findHomography.
        int minMatches = 8;

        /// RANSAC reprojection threshold (px).
        double ransacThreshPx = 6.0;

        /// Minimum RANSAC inliers to accept the result.
        int minInliers = 8;

        /// Reject if the median inlier reprojection error exceeds this (px).
        double maxMedianReprojPx = 8.0;

        /// When true, only match a detection to a component whose mapped class
        /// equals the detection's classId (requires classOfComponent to be
        /// populated). Leave false for a presence-only model.
        bool useClassPrior = false;
    };

    /// @param detections   AI detections in image space (bbox in px).
    /// @param project      Loaded iBOM project.
    /// @param currentPose  Current PCB->image homography used as the matching
    ///                     prior. Must be valid.
    /// @param activeLayer  Only components on this layer are considered.
    /// @param classOfComponent  Optional, same length as project.components:
    ///                     the detector class id expected for each component
    ///                     (-1 = unknown). Only used when params.useClassPrior.
    /// @param params       Tuning knobs.
    static ComponentReanchorResult estimate(
        const std::vector<ai::Detection>& detections,
        const ibom::IBomProject& project,
        const Homography& currentPose,
        ibom::Layer activeLayer,
        const std::vector<int>& classOfComponent = {},
        const Params& params = {});
};

} // namespace ibom::overlay
