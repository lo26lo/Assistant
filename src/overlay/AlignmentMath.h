#pragma once

#include <opencv2/core.hpp>

namespace ibom::overlay {

/**
 * @brief Pure similarity-alignment math shared by every "similarity" pose
 *        path in Application (1-point anchor click, minimap anchor, 2-comp
 *        alignment, multi-align with 2 landmarks).
 *
 * Convention (identical across all callers, see applyMultiAlignment):
 * the homography maps RAW PCB mm → image px. On the back side the camera
 * sees the layout mirrored, which a similarity cannot represent directly —
 * the fit is done in the view frame (PCB x pre-multiplied by vx = −1), so
 * the returned 3×3, expressed against raw PCB coords, carries the mirror
 * (negative determinant).
 *
 * Extracted from four near-identical inline copies in Application.cpp
 * (audit B8 was caused by one of them drifting from the others) — first
 * step of the AlignmentController split (INVESTIGATION_360 §7.1).
 */
namespace alignmath {

/// Similarity from an explicit scale (px/mm), rotation (radians) and one
/// anchor correspondence pcbAnchor → imgAnchor. vx = +1 front, −1 back.
/// Always returns a valid 3×3 CV_64F (degenerate only if scale == 0).
cv::Mat similarityFromAnchor(double scalePxPerMm, double rotRad, double vx,
                             cv::Point2f pcbAnchor, cv::Point2f imgAnchor);

/// Similarity from two correspondences (scale and rotation are derived from
/// the segment A→B). Returns an empty Mat when the points are too close to
/// fit reliably (PCB distance < 0.1 mm or image distance < 1 px — the same
/// guards the inline copies used). Optional out-params expose the derived
/// scale (px/mm) and rotation (radians).
cv::Mat similarityFromTwoPoints(cv::Point2f pcbA, cv::Point2f pcbB,
                                cv::Point2f imgA, cv::Point2f imgB,
                                double vx,
                                double* outScalePxPerMm = nullptr,
                                double* outRotRad = nullptr);

} // namespace alignmath
} // namespace ibom::overlay
