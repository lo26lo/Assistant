#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QComboBox>
#include <string>

namespace ibom::features {
class PickAndPlace;
class Measurement;
class SnapshotHistory;
}

namespace ibom::gui {

/// Persistent inspection workflow panel.
///
/// Coordinates PickAndPlace navigation, Measurement readouts, snapshot capture,
/// and report export — all without leaving the main window.
class InspectionPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectionPanel(QWidget* parent = nullptr);
    ~InspectionPanel() override = default;

    void setEnabled(bool enabled);

public slots:
    void onStepChanged(const QString& reference, const QString& value,
                       const QString& footprint, const QString& layer,
                       int currentStep, int totalSteps);
    void onProgress(int placed, int total);
    void onMeasurementResult(double valuePixels, double valueMM, const QString& unit);
    void onSnapshotTaken(int id, const QString& label);
    void onIBomLoaded(int componentCount);
    void onAllPlaced();

signals:
    void startInspectionClicked();
    void placedClicked();
    void skipClicked();
    void backClicked();
    void resetClicked();
    void measurementModeChanged(int mode);   // 0=Distance,1=Angle,2=Area,3=PinPitch,-1=Off
    void clearMeasurementsClicked();
    void snapshotClicked();
    void openSnapshotsFolderClicked();
    void exportRequested(const QString& format); // "csv","json","placement","bom","defects"

private:
    void buildUI();

    // Inspection section
    QLabel*       m_currentRefLabel    = nullptr;
    QLabel*       m_currentValueLabel  = nullptr;
    QLabel*       m_currentFpLabel     = nullptr;
    QLabel*       m_layerLabel         = nullptr;
    QLabel*       m_stepLabel          = nullptr;
    QProgressBar* m_progressBar        = nullptr;
    QPushButton*  m_btnStart           = nullptr;
    QPushButton*  m_btnBack            = nullptr;
    QPushButton*  m_btnPlaced          = nullptr;
    QPushButton*  m_btnSkip            = nullptr;
    QPushButton*  m_btnReset           = nullptr;

    // Measure section
    QComboBox*    m_measureMode        = nullptr;
    QLabel*       m_measureResult      = nullptr;
    QPushButton*  m_btnClearMeasure    = nullptr;

    // Snapshot section
    QLabel*       m_snapshotCount      = nullptr;
    QPushButton*  m_btnSnapshot        = nullptr;
    QPushButton*  m_btnOpenFolder      = nullptr;

    // Export section
    QPushButton*  m_btnExportCSV       = nullptr;
    QPushButton*  m_btnExportJSON      = nullptr;
    QPushButton*  m_btnExportPlacement = nullptr;
    QPushButton*  m_btnExportBOM       = nullptr;

    int m_snapshotTotal = 0;
};

} // namespace ibom::gui
