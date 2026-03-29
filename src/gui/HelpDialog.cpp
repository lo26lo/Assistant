#include "HelpDialog.h"

#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QPushButton>

namespace ibom::gui {

HelpDialog::HelpDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("MicroscopeIBOM — Help"));
    resize(720, 560);

    auto* layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    layout->addWidget(m_tabs);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(btnBox);

    createTabs();
}

void HelpDialog::showTab(int index)
{
    if (index >= 0 && index < m_tabs->count())
        m_tabs->setCurrentIndex(index);
    show();
    raise();
    activateWindow();
}

// ---------------------------------------------------------------------------
static QTextBrowser* makePage(const QString& html)
{
    auto* browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setHtml(html);
    return browser;
}

void HelpDialog::createTabs()
{
    // ── 0 — Getting Started ─────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Getting Started</h2>"
        "<ol>"
        "<li><b>Connect your camera</b> — plug in the USB microscope before launching the app.</li>"
        "<li><b>Select your camera</b> — go to <i>Settings → Camera</i> (Ctrl+,) and pick the correct device from the dropdown.</li>"
        "<li><b>Start the camera</b> — click the Camera toggle button (C) on the toolbar or Control Panel.</li>"
        "<li><b>Load an iBOM file</b> — <i>File → Open iBOM</i> (Ctrl+O). This is the JSON file exported by KiCad's Interactive BOM plugin.</li>"
        "<li><b>Align the overlay</b> — use one of the alignment methods (see the Alignment tab).</li>"
        "<li><b>Inspect!</b> — click components in the BOM list to highlight them. Check/uncheck placement status.</li>"
        "</ol>"
        "<h3>Keyboard Shortcuts</h3>"
        "<table border='0' cellpadding='4'>"
        "<tr><td><b>C</b></td><td>Toggle camera</td></tr>"
        "<tr><td><b>P</b></td><td>Take screenshot</td></tr>"
        "<tr><td><b>K</b></td><td>Calibrate camera</td></tr>"
        "<tr><td><b>F11</b></td><td>Fullscreen</td></tr>"
        "<tr><td><b>Ctrl+O</b></td><td>Open iBOM file</td></tr>"
        "<tr><td><b>Ctrl+,</b></td><td>Settings</td></tr>"
        "<tr><td><b>Esc</b></td><td>Exit fullscreen</td></tr>"
        "</table>"
    )), tr("Getting Started"));

    // ── 1 — Calibration ─────────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Camera Calibration</h2>"
        "<p>Calibration corrects lens distortion and establishes the <b>pixels-per-mm</b> ratio, "
        "which is essential for accurate overlay alignment and measurements.</p>"

        "<h3>Calibration Checkerboard</h3>"
        "<ol>"
        "<li>Go to <i>Camera → Generate Checkerboard...</i> to create a custom checkerboard image (PNG or print).</li>"
        "<li>Or use <i>Camera → Open Calibration Patterns PDF...</i> for pre-made patterns at 0.5mm, 1mm, and 2mm square sizes.</li>"
        "<li><b>Print at 100%% scale</b> (no fit-to-page!) and verify dimensions with calipers.</li>"
        "</ol>"

        "<h3>Recommended Square Sizes for Microscopes</h3>"
        "<table border='1' cellpadding='6' style='border-collapse:collapse;'>"
        "<tr><th>FOV (field of view)</th><th>Square Size</th><th>Board Size (8×6)</th></tr>"
        "<tr><td>~5-10 mm</td><td>0.5 mm</td><td>4 × 3 mm</td></tr>"
        "<tr><td>~10-20 mm</td><td>1.0 mm</td><td>8 × 6 mm</td></tr>"
        "<tr><td>~20-40 mm</td><td>2.0 mm</td><td>16 × 12 mm</td></tr>"
        "</table>"

        "<h3>Calibration Procedure</h3>"
        "<ol>"
        "<li>Set the correct checkerboard parameters in <i>Settings → Camera</i>: cols, rows, square size.</li>"
        "<li>Place the printed checkerboard under the microscope, flat and in focus.</li>"
        "<li>Press <b>K</b> or <i>Camera → Calibrate Camera</i>.</li>"
        "<li>The app captures frames and detects inner corners automatically.</li>"
        "<li>On success, you'll see the reprojection error and computed pixels/mm in the status bar.</li>"
        "</ol>"

        "<h3>Tips</h3>"
        "<ul>"
        "<li>The checkerboard must be <b>completely visible</b> in the camera view — all inner corners detected.</li>"
        "<li>A reprojection error &lt; 0.5 px is excellent; &lt; 1.0 px is acceptable.</li>"
        "<li>The calibration is saved automatically and reloaded on next launch.</li>"
        "<li>If you change the camera resolution, recalibrate.</li>"
        "</ul>"
    )), tr("Calibration"));

    // ── 2 — Alignment ───────────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Overlay Alignment</h2>"
        "<p>Alignment maps the 2D iBOM PCB coordinates onto the camera image "
        "so the overlay (pads, silkscreen, components) appears on top of the real PCB.</p>"

        "<h3>Method 1: 4-Corner Alignment (Full PCB Visible)</h3>"
        "<ol>"
        "<li>Click <b>\"Align Overlay\"</b> in the Control Panel.</li>"
        "<li>Click the 4 corners of the PCB in order: <b>Top-Left → Top-Right → Bottom-Right → Bottom-Left</b>.</li>"
        "<li>The homography is computed automatically from the 4 point correspondences.</li>"
        "</ol>"
        "<p><i>Best for: cameras that can see the entire PCB at once.</i></p>"

        "<h3>Method 2: 2-Component Alignment (Microscope / Small FOV)</h3>"
        "<ol>"
        "<li>Click <b>\"Align on 2 Components\"</b> in the Control Panel.</li>"
        "<li>Select a component from the BOM list (e.g. R1) → its PCB position is recorded.</li>"
        "<li>Move the microscope so that R1 is centered in the camera view. Click on it.</li>"
        "<li>Select a <b>second component</b> from the BOM list (e.g. U3, far from R1).</li>"
        "<li>Move the microscope to U3, click on it.</li>"
        "<li>A similarity transform (rotation + scale + translation) is computed from the 2 pairs.</li>"
        "</ol>"
        "<p><i>Best for: microscopes with small FOV that can't see the whole PCB.</i></p>"

        "<h3>Dynamic Scale Tracking</h3>"
        "<p>When you change the microscope zoom, the overlay can adjust automatically. "
        "Configure this in <i>Settings → Camera → Dynamic scale</i>:</p>"
        "<ul>"
        "<li><b>None</b>: Fixed calibration, no auto-update.</li>"
        "<li><b>From homography</b>: Scale is updated from live feature tracking.</li>"
        "<li><b>From iBOM pads</b>: Scale is computed from known pad distances.</li>"
        "</ul>"
    )), tr("Alignment"));

    // ── 3 — Lens Adapter ────────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Lens Adapters (Optical Multiplier)</h2>"
        "<p>Stereo microscopes often use <b>Barlow lenses</b> or <b>reduction rings</b> "
        "to change the field of view and magnification.</p>"

        "<h3>Common Adapters</h3>"
        "<table border='1' cellpadding='6' style='border-collapse:collapse;'>"
        "<tr><th>Adapter</th><th>Effect on FOV</th><th>Effect on px/mm</th><th>Use Case</th></tr>"
        "<tr><td><b>0.5x</b></td><td>×2 (doubles FOV)</td><td>÷2</td><td>Overview, larger PCBs, calibration</td></tr>"
        "<tr><td><b>0.75x</b></td><td>×1.33</td><td>÷1.33</td><td>Good compromise</td></tr>"
        "<tr><td><b>1x</b></td><td>No change</td><td>No change</td><td>Default (no adapter)</td></tr>"
        "<tr><td><b>1.5x</b></td><td>÷1.5</td><td>×1.5</td><td>Fine-pitch inspection</td></tr>"
        "<tr><td><b>2x</b></td><td>÷2 (halves FOV)</td><td>×2</td><td>BGA / QFP close-up</td></tr>"
        "</table>"

        "<h3>How to Use</h3>"
        "<ol>"
        "<li><b>Calibrate once</b> at 1x (no adapter).</li>"
        "<li>Attach the reduction ring (e.g. 0.5x).</li>"
        "<li>Go to <i>Settings → Camera → Lens adapter</i> and select <b>0.5x</b>.</li>"
        "<li>The pixels/mm ratio is automatically adjusted — no need to recalibrate.</li>"
        "</ol>"

        "<h3>Recommended Camera Upgrades</h3>"
        "<table border='1' cellpadding='6' style='border-collapse:collapse;'>"
        "<tr><th>Camera</th><th>Resolution</th><th>px/mm at 0.5x (FOV ~20mm)</th><th>Good For</th></tr>"
        "<tr><td>USB 1080p (basic)</td><td>1920×1080</td><td>~96</td><td>0603+ components</td></tr>"
        "<tr><td>USB 5MP (e.g. MU500)</td><td>2592×1944</td><td>~130</td><td>0402, QFP</td></tr>"
        "<tr><td>USB 4K</td><td>3840×2160</td><td>~192</td><td>0201, BGA, ultra-fine</td></tr>"
        "</table>"
    )), tr("Lens Adapters"));

    // ── 4 — Inspection ──────────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>PCB Inspection</h2>"

        "<h3>BOM Panel (left side)</h3>"
        "<p>The BOM (Bill of Materials) list shows all components from the iBOM file. "
        "Click on a component to highlight it in the camera view. "
        "The overlay will show the component's pads and silkscreen outline.</p>"
        "<ul>"
        "<li><b>Checkbox columns</b>: Use \"Sourced\" / \"Placed\" checkboxes to track your assembly progress.</li>"
        "<li><b>Search</b>: Type in the search bar to filter by reference, value, or footprint.</li>"
        "<li><b>Layer toggle</b>: Switch between Front / Back layer views.</li>"
        "</ul>"

        "<h3>Inspection Wizard</h3>"
        "<p>Start a guided inspection via <i>Inspection → Start Inspection</i>. "
        "The wizard walks through each component one by one.</p>"

        "<h3>AI-Assisted Inspection (future)</h3>"
        "<p>When AI models are loaded (<i>Settings → AI</i>), the system can:</p>"
        "<ul>"
        "<li><b>Detect components</b>: Identify placed/missing components automatically.</li>"
        "<li><b>Inspect solder joints</b>: Flag cold joints, bridges, insufficient solder.</li>"
        "<li><b>OCR</b>: Read component markings to verify correct placement.</li>"
        "</ul>"
    )), tr("Inspection"));

    // ── 5 — Overlay ─────────────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Overlay Settings</h2>"
        "<p>The overlay renders the iBOM board data on top of the live camera image.</p>"

        "<h3>Settings (Settings → Overlay tab)</h3>"
        "<ul>"
        "<li><b>Opacity</b>: Adjust transparency (0%% = invisible, 100%% = opaque).</li>"
        "<li><b>Show Pads</b>: Display copper pad outlines.</li>"
        "<li><b>Show Silkscreen</b>: Display silkscreen text and outlines.</li>"
        "<li><b>Show Fabrication</b>: Display fabrication layer markings.</li>"
        "</ul>"

        "<h3>Heatmap</h3>"
        "<p>Enable the heatmap in the Control Panel to see a color-coded view of "
        "inspection confidence levels across the board.</p>"
    )), tr("Overlay"));

    // ── 6 — Export ──────────────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Export & Reports</h2>"
        "<p>After inspection, export your results via <i>File → Export Report</i>.</p>"

        "<h3>Formats</h3>"
        "<ul>"
        "<li><b>CSV</b>: Spreadsheet-compatible, with columns for reference, value, footprint, status, defect type, confidence.</li>"
        "<li><b>JSON</b>: Machine-readable structured data with full component details.</li>"
        "<li><b>KiCad Placement</b>: Compatible with KiCad's pick-and-place format.</li>"
        "<li><b>BOM with checkboxes</b>: Re-importable BOM file with placed/not-placed status.</li>"
        "<li><b>Defects CSV</b>: Exports only components flagged as defective or missing.</li>"
        "</ul>"

        "<h3>Screenshots</h3>"
        "<p>Press <b>P</b> or <i>File → Screenshot</i> to save the current camera view "
        "(with overlay) as a PNG image.</p>"
    )), tr("Export"));

    // ── 7 — Troubleshooting ─────────────────────────────────────
    m_tabs->addTab(makePage(tr(
        "<h2>Troubleshooting</h2>"

        "<h3>Camera not detected</h3>"
        "<ul>"
        "<li>Make sure the USB cable is connected <b>before</b> launching the app.</li>"
        "<li>Go to <i>Settings → Camera</i> and click <b>Refresh</b> to re-scan devices.</li>"
        "<li>On Windows, check Device Manager for the camera driver.</li>"
        "</ul>"

        "<h3>Wrong camera selected</h3>"
        "<ul>"
        "<li>Open <i>Settings → Camera</i> and select the correct device from the dropdown.</li>"
        "<li>The microscope camera typically shows as \"USB Camera\" or by its brand name.</li>"
        "</ul>"

        "<h3>Overlay misaligned</h3>"
        "<ul>"
        "<li>Re-do the alignment: click <b>Align Overlay</b> or <b>Align on 2 Components</b>.</li>"
        "<li>If using 4-corner mode, make sure to click corners in order: TL → TR → BR → BL.</li>"
        "<li>Ensure the PCB hasn't moved since alignment.</li>"
        "</ul>"

        "<h3>Calibration fails</h3>"
        "<ul>"
        "<li>The entire checkerboard must be visible and in focus.</li>"
        "<li>Check that board cols/rows/square size in Settings match the actual printed pattern.</li>"
        "<li>Try with a different (larger) square size if the camera can't resolve the pattern.</li>"
        "</ul>"

        "<h3>Application logs</h3>"
        "<p>Logs are written to <code>build/bin/logs/</code> and <code>build/logs/</code>. "
        "Check <code>ibom_YYYYMMDD.log</code> for detailed error information.</p>"
    )), tr("Troubleshooting"));
}

} // namespace ibom::gui
