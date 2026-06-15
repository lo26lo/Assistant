#pragma once

#include <QDialog>
#include <QPointer>

#include "../camera/RealSenseCapture.h"

class QScrollArea;
class QLabel;
class QGroupBox;
class QFormLayout;
class QWidget;

namespace ibom::gui {

/**
 * @brief Dynamic RealSense sensor-options panel (like the RealSense Viewer).
 *
 * Queries every supported option of the live device and builds one control per
 * option (checkbox for booleans, spin box otherwise). Each control's tooltip is
 * the SDK-provided option description, so the user can understand what it does.
 * Changes are applied immediately to the running camera.
 */
class RealSenseControlsDialog : public QDialog {
    Q_OBJECT
public:
    explicit RealSenseControlsDialog(camera::RealSenseCapture* camera,
                                     QWidget* parent = nullptr);

private:
    void rebuild();          // (re)query options and (re)build the widgets
    void applyProfile(int index);   // apply a resolution/parameter profile

    // Build the editor widget for one option: checkbox (bool), combo (enum),
    // or slider+value box (numeric) — matching the RealSense Viewer.
    QWidget* buildControlRow(const camera::RsControl& c);
    // A checkable group box whose content collapses when unchecked.
    QGroupBox* makeCollapsibleGroup(const QString& title, QFormLayout*& formOut);

    // QPointer auto-nulls if the camera is destroyed (e.g. backend hot-swap),
    // so a delayed rebuild() or a control callback never dereferences a dangler.
    QPointer<camera::RealSenseCapture> m_camera;
    QScrollArea* m_scroll = nullptr;     // hosts the dynamically-built content
    QLabel* m_profileDesc = nullptr;     // explains the selected profile
};

} // namespace ibom::gui
