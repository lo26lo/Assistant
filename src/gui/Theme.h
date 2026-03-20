#pragma once

#include <QColor>

namespace ibom::gui::theme {

// ── Status Colors (consistent across all panels) ─────────────
inline constexpr QColor placedColor()  { return QColor(72, 200, 72);  }   // #48c848
inline constexpr QColor missingColor() { return QColor(255, 80, 80);  }   // #ff5050
inline constexpr QColor defectColor()  { return QColor(255, 165, 0);  }   // #ffa500
inline constexpr QColor pendingColor() { return QColor(140, 148, 172); }  // #8c94ac

// ── Accent ───────────────────────────────────────────────────
inline constexpr QColor accentDark()   { return QColor(100, 136, 232); }  // #6488e8
inline constexpr QColor accentLight()  { return QColor(68, 102, 204);  }  // #4466cc

// ── Layout Constants ─────────────────────────────────────────
constexpr int PanelMargin   = 10;
constexpr int PanelSpacing  = 8;
constexpr int GroupMarginH  = 6;   // horizontal inside group
constexpr int GroupMarginV  = 10;  // vertical inside group
constexpr int GroupSpacing  = 8;
constexpr int ToolbarIcon   = 28;
constexpr int StatusPadding = 6;

// ── Component State Colors (overlay rendering) ──────────────
inline QColor inspectedColor()        { return QColor(0, 200, 200);       }  // teal
inline QColor defaultComponentColor() { return QColor(0, 180, 255);       }  // light blue

// ── Overlay Rendering ────────────────────────────────────────
inline QColor padSelectedColor()   { return QColor(255, 200, 0, 200);  }  // gold
inline QColor padNormalColor()     { return QColor(180, 160, 80, 180); }  // muted gold
inline QColor padPin1Color()       { return QColor(255, 50, 50);       }  // red
inline QColor padRegularColor()    { return QColor(0, 180, 220);       }  // cyan
inline QColor silkSelectedColor()  { return QColor(255, 255, 100, 220);}  // bright yellow
inline QColor silkNormalColor()    { return QColor(170, 170, 68, 180); }  // muted yellow
inline QColor labelSelectedColor() { return QColor(255, 255, 200);     }  // warm white
inline QColor labelNormalColor()   { return QColor(68, 170, 170, 200); }  // muted teal
inline QColor boardOutlineColor()  { return QColor(255, 255, 0);       }  // yellow

// ── Homography Picking ───────────────────────────────────────
inline QColor pickPointColor()     { return QColor(255, 50, 50);       }  // red
inline QColor pickPointFill()      { return QColor(255, 50, 50, 100);  }  // red translucent
inline QColor pickLineColor()      { return QColor(255, 100, 100, 180);}  // light red

// ── CSS class names (for inline style overrides) ─────────────
// Status label CSS (dark mode) — embed via setStyleSheet
inline QString placedCSS()  { return QStringLiteral("color: #48c848; font-weight: bold;"); }
inline QString missingCSS() { return QStringLiteral("color: #ff5050; font-weight: bold;"); }
inline QString defectCSS()  { return QStringLiteral("color: #ffa500; font-weight: bold;"); }

} // namespace ibom::gui::theme
