#include "AlignmentMath.h"

#include <cmath>

namespace ibom::overlay::alignmath {

cv::Mat similarityFromAnchor(double scalePxPerMm, double rotRad, double vx,
                             cv::Point2f pcbAnchor, cv::Point2f imgAnchor)
{
    const double c = std::cos(rotRad) * scalePxPerMm;
    const double s = std::sin(rotRad) * scalePxPerMm;
    // x' = c·vx·px − s·py + tx ;  y' = s·vx·px + c·py + ty, with (tx, ty)
    // chosen so the anchor lands exactly on imgAnchor.
    const double tx = imgAnchor.x - (c * vx * pcbAnchor.x - s * pcbAnchor.y);
    const double ty = imgAnchor.y - (s * vx * pcbAnchor.x + c * pcbAnchor.y);
    return (cv::Mat_<double>(3, 3) << c * vx, -s, tx,
                                      s * vx,  c, ty,
                                      0,       0, 1);
}

cv::Mat similarityFromTwoPoints(cv::Point2f pcbA, cv::Point2f pcbB,
                                cv::Point2f imgA, cv::Point2f imgB,
                                double vx,
                                double* outScalePxPerMm, double* outRotRad)
{
    // Segment A→B in the (possibly mirrored) view frame vs in the image.
    const double dxp = vx * (pcbB.x - pcbA.x);
    const double dyp = pcbB.y - pcbA.y;
    const double dxi = imgB.x - imgA.x;
    const double dyi = imgB.y - imgA.y;
    const double dp = std::hypot(dxp, dyp);
    const double di = std::hypot(dxi, dyi);
    if (dp < 0.1 || di < 1.0)
        return {};   // points too close — scale/rotation unreliable

    const double scale = di / dp;
    const double rot   = std::atan2(dyi, dxi) - std::atan2(dyp, dxp);
    if (outScalePxPerMm) *outScalePxPerMm = scale;
    if (outRotRad)       *outRotRad = rot;
    return similarityFromAnchor(scale, rot, vx, pcbA, imgA);
}

} // namespace ibom::overlay::alignmath
