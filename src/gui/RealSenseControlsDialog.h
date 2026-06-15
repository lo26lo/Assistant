#pragma once

#include <QDialog>

class QScrollArea;
class QLabel;

namespace ibom::camera { class RealSenseCapture; }

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

    camera::RealSenseCapture* m_camera;
    QScrollArea* m_scroll = nullptr;     // hosts the dynamically-built content
    QLabel* m_profileDesc = nullptr;     // explains the selected profile
};

} // namespace ibom::gui
