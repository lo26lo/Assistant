#include "PointCloudView.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QOpenGLContext>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace ibom::gui {

namespace {
// Cap the uploaded cloud so the GUI-thread build + VBO upload stays cheap.
constexpr int kMaxPoints = 200000;
}

PointCloudView::PointCloudView(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
}

PointCloudView::~PointCloudView()
{
    // Ensure the GL context is current to release the VBO cleanly.
    if (m_glReady) {
        makeCurrent();
        m_vbo.destroy();
        m_vao.destroy();
        doneCurrent();
    }
}

void PointCloudView::clear()
{
    {
        std::lock_guard lock(m_dataMutex);
        m_vertices.clear();
        m_pointCount = 0;
        m_dirty = true;
    }
    m_haveCloud = false;
    update();
}

void PointCloudView::resetView()
{
    m_yaw = 0.0f;
    m_pitch = -20.0f;
    update();
}

void PointCloudView::updateCloud(const cv::Mat& depthMm, const cv::Mat& colorBgr,
                                 float fx, float fy, float ppx, float ppy)
{
    if (depthMm.empty() || depthMm.type() != CV_16UC1 || fx <= 0 || fy <= 0)
        return;

    const int rows = depthMm.rows, cols = depthMm.cols;
    const bool haveColor = (!colorBgr.empty()
                            && colorBgr.rows == rows && colorBgr.cols == cols
                            && colorBgr.type() == CV_8UC3);

    // Downsample so we stay under kMaxPoints regardless of resolution.
    const int total = rows * cols;
    int step = 1;
    while ((total / (step * step)) > kMaxPoints) ++step;

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(total / (step * step)) * 6);

    double sx = 0, sy = 0, sz = 0;
    int n = 0;
    for (int v = 0; v < rows; v += step) {
        const uint16_t* drow = depthMm.ptr<uint16_t>(v);
        const cv::Vec3b* crow = haveColor ? colorBgr.ptr<cv::Vec3b>(v) : nullptr;
        for (int u = 0; u < cols; u += step) {
            const uint16_t z = drow[u];
            if (z == 0) continue;                  // invalid sample
            const float Z = static_cast<float>(z);
            const float X = (u - ppx) * Z / fx;
            const float Y = (v - ppy) * Z / fy;
            // Flip Y and Z so the cloud faces the camera in a natural way.
            verts.push_back(X);
            verts.push_back(-Y);
            verts.push_back(-Z);
            if (crow) {
                const cv::Vec3b& bgr = crow[u];
                verts.push_back(bgr[2] / 255.0f);
                verts.push_back(bgr[1] / 255.0f);
                verts.push_back(bgr[0] / 255.0f);
            } else {
                // Colorless: shade by depth (cyan→red over working range).
                const float t = std::clamp((Z - 30.0f) / 570.0f, 0.0f, 1.0f);
                verts.push_back(t);
                verts.push_back(1.0f - t);
                verts.push_back(1.0f - t);
            }
            sx += X; sy += -Y; sz += -Z; ++n;
        }
    }

    if (n == 0) { clear(); return; }

    const QVector3D centroid(static_cast<float>(sx / n),
                             static_cast<float>(sy / n),
                             static_cast<float>(sz / n));

    {
        std::lock_guard lock(m_dataMutex);
        m_vertices.swap(verts);
        m_pointCount = n;
        m_dirty = true;
    }

    // Frame the cloud the first time we get data (and keep the orbit afterwards).
    if (!m_haveCloud) {
        m_target = centroid;
        m_dist = std::max(150.0f, std::abs(centroid.z()) * 1.6f);
        m_haveCloud = true;
    } else {
        m_target = centroid;  // follow the scene depth, keep orbit angle/zoom
    }
    update();
}

void PointCloudView::initializeGL()
{
    initializeFunctions();
    glClearColor(0.06f, 0.06f, 0.09f, 1.0f);
    glEnable(GL_DEPTH_TEST);
#ifdef GL_PROGRAM_POINT_SIZE
    glEnable(GL_PROGRAM_POINT_SIZE);  // allow gl_PointSize in the vertex shader
#endif

    const bool es = context() && context()->isOpenGLES();
    const QString header = es ? "#version 300 es\nprecision mediump float;\n"
                              : "#version 330 core\n";

    const QString vs = header + R"(
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec3 aColor;
        uniform mat4 uMvp;
        out vec3 vColor;
        void main() {
            gl_Position = uMvp * vec4(aPos, 1.0);
            gl_PointSize = 2.0;
            vColor = aColor;
        }
    )";

    const QString fs = header + R"(
        in vec3 vColor;
        out vec4 fragColor;
        void main() { fragColor = vec4(vColor, 1.0); }
    )";

    m_program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vs) ||
        !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fs) ||
        !m_program->link()) {
        spdlog::error("PointCloudView: shader build failed: {}",
                      m_program->log().toStdString());
        m_program.reset();
        return;
    }

    m_vao.create();
    m_vbo.create();
    m_glReady = true;
}

void PointCloudView::uploadIfDirty()
{
    std::lock_guard lock(m_dataMutex);
    if (!m_dirty) return;
    m_dirty = false;
    m_vbo.bind();
    if (!m_vertices.empty())
        m_vbo.allocate(m_vertices.data(),
                       static_cast<int>(m_vertices.size() * sizeof(float)));
    m_vbo.release();
}

QMatrix4x4 PointCloudView::cameraMatrix() const
{
    QMatrix4x4 proj;
    const float aspect = height() > 0 ? float(width()) / float(height()) : 1.0f;
    proj.perspective(45.0f, aspect, 1.0f, 20000.0f);

    QMatrix4x4 view;
    view.translate(0, 0, -m_dist);
    view.rotate(m_pitch, 1, 0, 0);
    view.rotate(m_yaw,   0, 1, 0);
    view.translate(-m_target);
    return proj * view;
}

void PointCloudView::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!m_glReady || !m_program) {
        // Shader unavailable — draw a hint via QPainter so the panel isn't blank.
        QPainter p(this);
        p.setPen(QColor(150, 160, 190));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("3D point cloud unavailable (OpenGL shader build failed)."));
        return;
    }

    uploadIfDirty();
    if (m_pointCount == 0) {
        QPainter p(this);
        p.setPen(QColor(120, 130, 160));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("Waiting for depth… (RealSense must be streaming)"));
        return;
    }

    m_program->bind();
    m_program->setUniformValue("uMvp", cameraMatrix());

    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);
    m_vbo.bind();
    m_program->enableAttributeArray(0);
    m_program->setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
    m_program->enableAttributeArray(1);
    m_program->setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));

    glDrawArrays(GL_POINTS, 0, m_pointCount);

    m_program->disableAttributeArray(0);
    m_program->disableAttributeArray(1);
    m_vbo.release();
    m_program->release();
}

void PointCloudView::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void PointCloudView::mousePressEvent(QMouseEvent* event)
{
    m_lastMouse = event->pos();
    if (event->button() == Qt::LeftButton) {
        m_rotating = true;
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::MiddleButton ||
               event->button() == Qt::RightButton) {
        m_panning = true;
        setCursor(Qt::SizeAllCursor);
    }
}

void PointCloudView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint delta = event->pos() - m_lastMouse;
    m_lastMouse = event->pos();

    if (m_rotating) {
        m_yaw   += delta.x() * 0.4f;
        m_pitch += delta.y() * 0.4f;
        m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
        update();
    } else if (m_panning) {
        // Pan in the view plane; scale by distance so it feels constant.
        const float k = m_dist * 0.0015f;
        const float yawRad = m_yaw * 3.14159265f / 180.0f;
        QVector3D right(std::cos(yawRad), 0, -std::sin(yawRad));
        QVector3D up(0, 1, 0);
        m_target -= right * (delta.x() * k);
        m_target += up    * (delta.y() * k);
        update();
    }
}

void PointCloudView::mouseReleaseEvent(QMouseEvent* /*event*/)
{
    m_rotating = false;
    m_panning = false;
    unsetCursor();
}

void PointCloudView::wheelEvent(QWheelEvent* event)
{
    const float steps = event->angleDelta().y() / 120.0f;
    m_dist *= std::pow(0.88f, steps);
    m_dist = std::clamp(m_dist, 20.0f, 15000.0f);
    update();
}

} // namespace ibom::gui
