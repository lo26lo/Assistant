#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLShaderProgram>
#include <QVector3D>
#include <QMatrix4x4>
#include <QPoint>
#include "../camera/PointCloudData.h"
#include <memory>
#include <mutex>
#include <vector>

namespace ibom::gui {

/**
 * @brief Interactive 3D point cloud view (like the RealSense Viewer "3D" panel).
 *
 * Renders a colored point cloud built from a RealSense depth map (CV_16UC1, mm)
 * and the aligned color frame, deprojected through the factory intrinsics.
 * Orbit with the left mouse button, pan with the middle/right button, zoom with
 * the wheel.
 *
 * The cloud is computed upstream (capture thread, rs2::pointcloud), staged via
 * setCloud(), and uploaded to a VBO lazily inside paintGL. Portable across
 * desktop GL 3.3 and OpenGL ES 3.0 (shader version chosen at runtime).
 */
class PointCloudView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit PointCloudView(QWidget* parent = nullptr);
    ~PointCloudView() override;

    /// Set the cloud to render. Built upstream (capture thread) via
    /// rs2::pointcloud — vertices in metres, camera frame. Cheap: just stages
    /// the interleaved buffer for the next paint.
    void setCloud(const ibom::camera::PointCloudRef& cloud);

    /// Drop the current cloud (e.g. when leaving 3D mode or stopping the camera).
    void clear();

    /// Reset the orbit camera to frame the current cloud.
    void resetView();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void uploadIfDirty();
    QMatrix4x4 cameraMatrix() const;

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    bool m_glReady = false;

    // Interleaved vertex data: x,y,z (mm, Y/Z flipped for a natural view), r,g,b.
    std::mutex          m_dataMutex;
    std::vector<float>  m_vertices;     // staging (GUI thread writes)
    int                 m_pointCount = 0;
    bool                m_dirty = false;

    // Orbit camera (units: metres, matching rs2::points)
    float     m_yaw   = 0.0f;     // degrees
    float     m_pitch = -20.0f;   // degrees
    float     m_dist  = 0.4f;     // metres from target
    QVector3D m_target{0, 0, 0};  // look-at point (cloud centroid)
    bool      m_haveCloud = false;

    QPoint    m_lastMouse;
    bool      m_rotating = false;
    bool      m_panning  = false;
};

} // namespace ibom::gui
