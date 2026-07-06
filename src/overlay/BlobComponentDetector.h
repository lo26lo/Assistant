#pragma once

#include <opencv2/core.hpp>
#include <vector>

#include "ai/InferenceEngine.h"  // ai::Detection

namespace ibom::overlay {

/// Model-free component detector: finds component-body-like blobs in a PCB
/// image by classic CV (MSER stable regions + size/aspect gating), and returns
/// them as ai::Detection (same shape a trained model would produce), so the
/// SAME ComponentReanchor::bootstrap() can register them against the iBOM
/// layout — WITHOUT any .onnx model.
///
/// This is what makes "detect components to align, even with an empty models/"
/// work: bootstrap() is RANSAC consensus and already tolerates heavy false
/// positives and dropouts, so a noisy geometric detector is enough to lock a
/// pose on a populated board. It is a best-effort fallback — less reliable
/// than a trained detector (misses tiny 0201s, can fire on solder blobs /
/// silkscreen), and it needs a *populated* board with visible component
/// bodies. A real model stays the better path; this removes the hard
/// dependency on having one.
///
/// @param image           BGR or gray camera frame.
/// @param scalePxPerMm    Physical scale prior (D405 fx/distance, or current
///                        px/mm). When > 0, blobs are size-gated to real
///                        component dimensions (~0.4–22 mm), which massively
///                        cuts false positives. 0 = unknown (wider, noisier).
/// @param maxDetections   Cap on returned blobs (strongest by area kept) — the
///                        bootstrap consensus loop is O(nComp × nDet).
/// @return Detections with classId 0 ("component"), bbox in image pixels.
std::vector<ai::Detection> detectComponentBlobs(const cv::Mat& image,
                                                double scalePxPerMm,
                                                int maxDetections = 300);

} // namespace ibom::overlay
