#pragma once

#include <QMetaType>
#include <cstdint>
#include <memory>
#include <vector>

namespace ibom::camera {

/// A ready-to-render colored point cloud, computed on the capture thread via
/// rs2::pointcloud (the canonical librealsense path: map_to(color) +
/// calculate(depth)). Vertices are in METRES in the camera frame
/// (X right, Y down, Z forward) — exactly what rs2::points::get_vertices()
/// returns. Colors are sampled from the mapped color frame (RGB, 0–255).
///
/// Shared immutably: passing PointCloudRef across a queued connection copies
/// only the shared_ptr, never the (large) buffers.
struct PointCloudData {
    std::vector<float>   xyz;    ///< count*3 floats, metres, camera frame
    std::vector<uint8_t> rgb;    ///< count*3 bytes, R,G,B
    int                  count = 0;
};

using PointCloudRef = std::shared_ptr<const PointCloudData>;

} // namespace ibom::camera

Q_DECLARE_METATYPE(ibom::camera::PointCloudRef)
