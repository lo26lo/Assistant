#include "ToggleSwitch.h"

#include <QPainter>
#include <QMouseEvent>
#include <QEnterEvent>

namespace ibom::gui {

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    m_anim.setDuration(120);
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        animateTo(on);
    });
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(44, 22);
}

void ToggleSwitch::animateTo(bool checked)
{
    m_anim.stop();
    m_anim.setStartValue(m_offset);
    m_anim.setEndValue(checked ? 1.0f : 0.0f);
    m_anim.start();
}

void ToggleSwitch::enterEvent(QEnterEvent* e)
{
    QAbstractButton::enterEvent(e);
    update();
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* e)
{
    // Snap the offset to the (toggled) state without waiting for a click cycle.
    QAbstractButton::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton)
        animateTo(isChecked());
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const float h = height();
    const float w = width();
    const float r = h / 2.0f;
    const float margin = 2.0f;
    const float knobR = r - margin;

    // Track: grey when off, accent (green-ish) when on; dim when disabled.
    const bool on = isChecked();
    QColor trackOff(90, 95, 110);
    QColor trackOn(72, 200, 120);
    if (!isEnabled()) { trackOff.setAlpha(110); trackOn.setAlpha(110); }
    // Blend by offset for a smooth color change during the animation.
    QColor track(
        int(trackOff.red()   + (trackOn.red()   - trackOff.red())   * m_offset),
        int(trackOff.green() + (trackOn.green() - trackOff.green()) * m_offset),
        int(trackOff.blue()  + (trackOn.blue()  - trackOff.blue())  * m_offset));
    (void)on;

    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(QRectF(0, 0, w, h), r, r);

    // Knob slides from left to right with the offset.
    const float cx = margin + knobR + (w - 2 * (margin + knobR)) * m_offset;
    p.setBrush(QColor(245, 247, 250));
    p.drawEllipse(QPointF(cx, h / 2.0f), knobR, knobR);
}

} // namespace ibom::gui
