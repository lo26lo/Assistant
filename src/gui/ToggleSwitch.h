#pragma once

#include <QAbstractButton>
#include <QPropertyAnimation>

namespace ibom::gui {

/**
 * @brief A compact iOS/RealSense-Viewer-style on/off toggle switch.
 *
 * Drop-in replacement for a QCheckBox when a switch reads better (sensor
 * enable, filter enable…). Checkable QAbstractButton: emits toggled(bool).
 * The knob position is animated via the `offset` property.
 */
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(float offset READ offset WRITE setOffset)

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    float offset() const { return m_offset; }
    void  setOffset(float o) { m_offset = o; update(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    void animateTo(bool checked);

    float m_offset = 0.0f;   // 0 = off (left), 1 = on (right)
    QPropertyAnimation m_anim{this, "offset"};
};

} // namespace ibom::gui
