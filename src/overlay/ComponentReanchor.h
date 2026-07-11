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
    /// Where the expected (iBOM-side) constellation comes from.
    enum class Constellation {
        /// Component bbox centers — populated board, detections are component
        /// bodies. The historical behaviour.
        Components,
        /// Pad positions (absolute PCB mm, straight from the parser) — bare or
        /// partially populated board, where the model-free blobs are the shiny
        /// tinned PADS, not bodies. Matching pad blobs against component
        /// centers is a constellation coincidence and aliases to a wrong pose
        /// (field case ERREUR #57: 40/117 inliers accepted as score 1.00).
        Pads
    };

    struct Params {
        /// Gating radius (px) around a component's predicted image position.
        /// A detection farther than this from every predicted position is
        /// unmatched. Should comfortably exceed expected drift.
        double maxMatchDistPx = 60.0;

        /// When both are > 0 the matching gate becomes PHYSICAL:
        /// clamp(matchGateMm × scalePxPerMm, 15, 90) px replaces
        /// maxMatchDistPx. A fixed 60 px is 13.6 mm at a D405 wide view
        /// (4.4 px/mm) — wide enough to gate almost anything to something —
        /// yet only 1.2 mm under a microscope. Same principle as
        /// bootstrapTolMm (INVESTIGATION_360 §1.1).
        double matchGateMm  = 0.0;
        double scalePxPerMm = 0.0;

        /// Expected-constellation source. Pads is the right choice on a bare
        /// board (hand assembly starts with one); the callers with model-free
        /// blob detections try both and keep the better-supported fit.
        Constellation constellation = Constellation::Components;

        /// Reject a fit supported by fewer than this fraction of the gated
        /// matches. An aliased pose on a repetitive/bare layout can pass the
        /// absolute gates (minInliers, median error) with 30-40 % support —
        /// a healthy lock sits at 60-90 %. 0 disables.
        double minInlierRatio = 0.4;

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

        /// Fit a 4-DOF similarity (estimateAffinePartial2D) instead of the
        /// 8-DOF findHomography. With noisy detection centers (blob path) the
        /// two perspective terms of a full homography are fit on pure noise on
        /// a fronto-parallel scene; they barely move the interior reprojection
        /// error but lever tens of px at the board corners — which is exactly
        /// what the periodic drift gate measures, hence the 13-63 px jitter of
        /// 2026-07-03. The similarity fit cuts that corner jitter ~4× at
        /// identical detections (docs/BLOB_REANCHOR_JITTER_ANALYSE.md). Leave
        /// false for trained-model detections (repeatable centers, and a real
        /// camera tilt then benefits from the full model).
        bool fitSimilarity = false;

        // ── bootstrap() (prior-free registration) knobs ──
        /// RANSAC pair→pair hypotheses tried before giving up.
        int    bootstrapIterations = 3000;
        /// Consensus match tolerance in PCB millimetres (converted to px via
        /// the hypothesis scale) — physical tolerance, so it stays meaningful
        /// from wide D405 views to high microscope magnification.
        double bootstrapTolMm      = 1.2;
        /// Reject the bootstrap when a DISTINCT pose reaches at least this
        /// fraction of the best consensus: a (near-)symmetric layout aliases,
        /// and picking between two equal poses is a coin flip — refuse and say
        /// so instead of applying a maybe-mirrored overlay (ERREUR #58).
        /// Deliberately tight: partial repetition (alias at 80-95 % of the
        /// true pose) must still lock on the best one. 0 disables.
        double bootstrapAmbiguityRatio = 0.97;
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
        const std::vector<int>& classOfComponent,
        const Params& params);

    /// Convenience overload: default detection/RANSAC tuning (Params{}), no
    /// class prior. Split from the form above — `Params` is a nested
    /// aggregate, and giving it a `= {}` default argument here (evaluated
    /// while ComponentReanchor itself is still being defined) hits a
    /// standard-mandated restriction: a default member initializer cannot be
    /// required to construct a member's default before the end of its
    /// enclosing class. Defined out-of-line in the .cpp, where
    /// ComponentReanchor (and Params) is already a complete type, so building
    /// Params{} there is unproblematic.
    static ComponentReanchorResult estimate(
        const std::vector<ai::Detection>& detections,
        const ibom::IBomProject& project,
        const Homography& currentPose,
        ibom::Layer activeLayer,
        const std::vector<int>& classOfComponent = {});

    /// Bootstrap a pose from detections alone — NO current-pose prior.
    ///
    /// estimate() can only *correct* an existing pose (it gates matches around
    /// positions predicted by the prior); with no pose, or a stale one after
    /// the board was moved/picked up, it is useless. bootstrap() solves the
    /// global problem instead: register the detected-component constellation
    /// against the iBOM component layout under an unknown similarity
    /// transform, by RANSAC over pair→pair hypotheses — each (detection pair,
    /// component pair) correspondence fully determines scale+rotation+
    /// translation; consensus is the number of components landing on a
    /// detection within a physical tolerance. The winning similarity is then
    /// handed to estimate() as prior for the precise homography fit and its
    /// existing inlier/reprojection validation.
    ///
    /// Needs no visible board outline — works where BoardLocator structurally
    /// fails (board filling the frame), and turns Auto-Align into "put the
    /// board under the camera and it aligns itself" once a detector model is
    /// loaded. Deterministic (fixed internal RNG seed).
    ///
    /// Caveat: a presence-only model on a highly repetitive layout (regular
    /// grid of identical passives) can alias to a symmetric pose; the
    /// estimate() validation bounds the damage, and a class-aware model
    /// (Piste A) removes the ambiguity.
    ///
    /// @param scalePriorPxPerMm  When > 0 (e.g. D405 pinhole fx/distance, or
    ///        the current px/mm), hypotheses outside ~[0.55, 1.8]× of it are
    ///        rejected early — fewer iterations wasted, fewer aliases.
    /// @param classOfComponent  Optional (same contract as estimate()): the
    ///        MODEL class id expected per component. The pair→pair consensus
    ///        stays class-blind, but the prior is forwarded to the refine
    ///        estimate() — where found/inliers are decided — so an aliased
    ///        pose that lands components on wrong-class detections starves
    ///        there (a connector cannot match a resistor: the 180° alias of
    ///        a class-asymmetric board becomes impossible). Only active when
    ///        params.useClassPrior is set.
    static ComponentReanchorResult bootstrap(
        const std::vector<ai::Detection>& detections,
        const ibom::IBomProject& project,
        ibom::Layer activeLayer,
        double scalePriorPxPerMm,
        const Params& params,
        const std::vector<int>& classOfComponent = {});

    /// Convenience overload (default Params) — defined in the .cpp for the
    /// same nested-aggregate reason as the estimate() overload above.
    static ComponentReanchorResult bootstrap(
        const std::vector<ai::Detection>& detections,
        const ibom::IBomProject& project,
        ibom::Layer activeLayer,
        double scalePriorPxPerMm = 0.0);

    /// Orientation vote on a LOCATED board quad (field insight, suite 142):
    /// a board outline (BoardLocator contour/depth) reliably fixes position,
    /// scale and the board surface — but a quad is 4-fold ambiguous in
    /// orientation, and the edge-agreement score that picks one is weakly
    /// discriminative on a busy PCB. The constellation decides instead: build
    /// the pose for each of the 4 corner assignments and keep the one whose
    /// support (inlier ratio) wins. Four discrete hypotheses with a known
    /// surface beat an open-ended prior-free search — this is what makes
    /// "contour + pads" the robust bare-board path.
    ///
    /// @param imageCorners  The located quad, ordered to match the PCB bbox
    ///                      corners {TL, TR, BR, BL} under SOME orientation
    ///                      guess (BoardLocateResult::imageCorners as-is).
    /// Returns the best-supported estimate() result; message is prefixed with
    /// the winning rotation.
    static ComponentReanchorResult estimateOrientations(
        const std::vector<ai::Detection>& detections,
        const ibom::IBomProject& project,
        const std::vector<cv::Point2f>& imageCorners,
        ibom::Layer activeLayer,
        const Params& params);
};

} // namespace ibom::overlay
