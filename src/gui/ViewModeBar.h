#pragma once

#include <QWidget>
#include <QRectF>

namespace ibom::gui {

/**
 * Floating transparent overlay drawn on top of the central view stack.
 * Renders two pill buttons ("● Depth" and "● 3D") in the top-right corner
 * that allow switching between color/depth/3D views from any page of the stack.
 */
class ViewModeBar : public QWidget {
    Q_OBJECT
public:
    explicit ViewModeBar(QWidget* parent = nullptr);

    void setDepthEnabled(bool on);
    void setCloudEnabled(bool on);
    void setDepthActive(bool on);
    void setCloudActive(bool on);

    QSize sizeHint() const override;

signals:
    void depthToggled();
    void cloudToggled();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    bool m_depthEnabled = false;
    bool m_cloudEnabled = false;
    bool m_depthActive  = false;
    bool m_cloudActive  = false;

    QRectF m_depthRect;
    QRectF m_cloudRect;
};

} // namespace ibom::gui
