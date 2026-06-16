#include "ViewModeBar.h"

#include <QPainter>
#include <QFontMetrics>
#include <QMouseEvent>

namespace ibom::gui {

static constexpr int kPadX   = 12;
static constexpr int kPadY   = 6;
static constexpr int kGap    = 6;

ViewModeBar::ViewModeBar(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setMouseTracking(false);
    setCursor(Qt::PointingHandCursor);
}

QSize ViewModeBar::sizeHint() const
{
    QFont f("Segoe UI", 9, QFont::DemiBold);
    QFontMetrics fm(f);
    const int h = fm.height() + 2 * kPadY;
    const int wD = fm.horizontalAdvance(tr("● Depth")) + 2 * kPadX;
    const int w3 = fm.horizontalAdvance(tr("● 3D"))    + 2 * kPadX;
    return QSize(wD + kGap + w3, h);
}

void ViewModeBar::setDepthEnabled(bool on)
{
    if (m_depthEnabled == on) return;
    m_depthEnabled = on;
    update();
}
void ViewModeBar::setCloudEnabled(bool on)
{
    if (m_cloudEnabled == on) return;
    m_cloudEnabled = on;
    update();
}
void ViewModeBar::setDepthActive(bool on)
{
    if (m_depthActive == on) return;
    m_depthActive = on;
    update();
}
void ViewModeBar::setCloudActive(bool on)
{
    if (m_cloudActive == on) return;
    m_cloudActive = on;
    update();
}

void ViewModeBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f("Segoe UI", 9, QFont::DemiBold);
    p.setFont(f);
    QFontMetrics fm(f);

    const int h  = fm.height() + 2 * kPadY;
    const int r  = h / 2;

    const QString depthLabel = tr("● Depth");
    const QString cloudLabel = tr("● 3D");
    const int wD = fm.horizontalAdvance(depthLabel) + 2 * kPadX;
    const int w3 = fm.horizontalAdvance(cloudLabel) + 2 * kPadX;

    m_depthRect = QRectF(0, 0, wD, h);
    m_cloudRect = QRectF(wD + kGap, 0, w3, h);

    auto drawPill = [&](const QRectF& rect, const QString& label, bool active, bool enabled) {
        const QColor bgOn (0, 150, 200, 190);
        const QColor bgOff(20, 20, 28, 160);
        const QColor bgDis(40, 40, 50,  90);
        QColor bg = !enabled ? bgDis : (active ? bgOn : bgOff);
        p.setPen(QPen(QColor(255, 255, 255, enabled ? 90 : 40), 1));
        p.setBrush(bg);
        p.drawRoundedRect(rect, r, r);
        p.setPen(QColor(235, 240, 250, enabled ? 230 : 100));
        p.drawText(rect, Qt::AlignCenter, label);
    };

    drawPill(m_depthRect, depthLabel, m_depthActive,  m_depthEnabled);
    drawPill(m_cloudRect, cloudLabel, m_cloudActive,  m_cloudEnabled);
}

void ViewModeBar::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) { ev->ignore(); return; }
    const QPointF pos = ev->position();
    if (m_depthEnabled && m_depthRect.contains(pos)) {
        emit depthToggled();
        ev->accept();
    } else if (m_cloudEnabled && m_cloudRect.contains(pos)) {
        emit cloudToggled();
        ev->accept();
    } else {
        ev->ignore();
    }
}

} // namespace ibom::gui
