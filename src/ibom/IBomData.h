#pragma once

#include <string>
#include <vector>
#include <array>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>

namespace ibom {

// ============================================================================
// Data structures representing parsed iBOM data
// ============================================================================

/// 2D point in iBOM coordinate space (PCB units, typically mm).
struct Point2D {
    double x = 0.0;
    double y = 0.0;
};

/// Bounding box in PCB coordinates.
struct BBox {
    double minX = 0, minY = 0;
    double maxX = 0, maxY = 0;

    double width() const { return maxX - minX; }
    double height() const { return maxY - minY; }
    Point2D center() const { return {(minX + maxX) / 2.0, (minY + maxY) / 2.0}; }
};

/// Drawing element (line segment, arc, circle, polygon, etc.).
struct DrawingSegment {
    enum class Type { Line, Arc, Circle, Polygon, Rect };
    Type type = Type::Line;

    Point2D start;
    Point2D end;
    double  radius = 0.0;
    double  angle  = 0.0;
    double  width  = 0.0;

    std::vector<Point2D> points; // For polygons
};

/// A pad on the PCB.
struct Pad {
    std::string  netName;
    std::string  pinNumber;
    Point2D      position;
    double       angle = 0.0;

    // Shape info
    enum class Shape { Rect, RoundRect, Circle, Oval, Trapezoid, Custom };
    Shape  shape = Shape::Rect;
    double sizeX = 0.0;
    double sizeY = 0.0;

    bool   isPin1 = false; // True if this is the first pin
    bool   isSMD  = true;  // True if surface-mount
};

/// Which layer a component is on.
enum class Layer {
    Front,  // F (top)
    Back    // B (bottom)
};

/// A single component footprint on the board.
struct Component {
    std::string  reference;   // e.g., "R1", "C5", "U3"
    std::string  value;       // e.g., "10k", "100nF", "STM32F4"
    std::string  footprint;   // e.g., "Resistor_SMD:R_0402_1005Metric"
    Layer        layer = Layer::Front;

    Point2D      position;    // Center position
    double       rotation = 0.0; // Degrees

    BBox         bbox;        // Bounding box
    std::vector<Pad>             pads;
    std::vector<DrawingSegment>  drawings; // Silkscreen / courtyard

    // BOM fields (from the extra_fields in iBOM)
    std::map<std::string, std::string> extraFields;

    // Tracking (populated at runtime)
    std::map<std::string, bool> checkboxes; // e.g., {"Sourced": true, "Placed": false}
};

/// A net (electrical connection).
struct Net {
    std::string name;
    std::vector<std::string> pads; // "ref:pin" format
};

/// Board-level metadata.
struct BoardInfo {
    std::string title;
    std::string revision;
    std::string date;
    std::string company;
    BBox        boardBBox;   // Overall board outline bounding box
};

/// Grouping of components for BOM display.
struct BomGroup {
    std::string              value;
    std::string              footprint;
    std::vector<std::string> references; // All refs in this group
    std::map<std::string, std::string> extraFields;
};

/// The complete parsed iBOM data.
struct IBomProject {
    BoardInfo                   boardInfo;
    std::vector<Component>      components;
    std::vector<Net>            nets;
    std::vector<BomGroup>       bomGroups;
    std::vector<DrawingSegment> boardOutline;

    // Config from the iBOM file
    bool        darkMode = false;
    bool        showPads = true;
    bool        showFabrication = false;
    bool        showSilkscreen = true;
    std::string highlightPin1 = "none";
    std::vector<std::string> checkboxColumns;
    std::vector<std::string> fields;
};

} // namespace ibom
