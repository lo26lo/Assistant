#include "IBomParser.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

namespace ibom {

std::optional<IBomProject> IBomParser::parseFile(const std::string& filePath)
{
    spdlog::info("Parsing iBOM file: {}", filePath);

    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        m_lastError = "Cannot open file: " + filePath;
        spdlog::error("{}", m_lastError);
        return std::nullopt;
    }

    std::ostringstream ss;
    ss << ifs.rdbuf();
    return parseString(ss.str());
}

std::optional<IBomProject> IBomParser::parseString(const std::string& htmlContent)
{
    if (htmlContent.empty()) {
        m_lastError = "Empty HTML content.";
        spdlog::error("{}", m_lastError);
        return std::nullopt;
    }

    // Extract the main data variables from the HTML
    auto configOpt = extractJsVar(htmlContent, "config");
    if (!configOpt) {
        m_lastError = "Failed to extract 'config' variable from iBOM HTML.";
        spdlog::error("{}", m_lastError);
        return std::nullopt;
    }
    m_configData = *configOpt;

    // Try extracting pcbdata directly
    auto pcbdataOpt = extractJsVar(htmlContent, "pcbdata");
    if (!pcbdataOpt) {
        // pcbdata might be LZString compressed
        spdlog::debug("pcbdata not found directly, trying compressed format...");
        auto compressedOpt = extractJsVar(htmlContent, "compressed_pcbdata");
        if (compressedOpt && compressedOpt->is_string()) {
            auto decompressed = decompressLZString(compressedOpt->get<std::string>());
            if (decompressed) {
                try {
                    m_pcbData = nlohmann::json::parse(*decompressed);
                } catch (const std::exception& e) {
                    m_lastError = "Failed to parse decompressed pcbdata: " + std::string(e.what());
                    spdlog::error("{}", m_lastError);
                    return std::nullopt;
                }
            } else {
                m_lastError = "Failed to decompress LZString pcbdata.";
                spdlog::error("{}", m_lastError);
                return std::nullopt;
            }
        } else {
            m_lastError = "Failed to extract 'pcbdata' from iBOM HTML.";
            spdlog::error("{}", m_lastError);
            return std::nullopt;
        }
    } else {
        m_pcbData = *pcbdataOpt;
    }

    // Build the project data
    IBomProject project;

    parseConfig(m_configData, project);
    parseBoardInfo(m_pcbData, project);
    parseBoardOutline(m_pcbData, project);
    parseComponents(m_pcbData, project);
    parseBomGroups(m_pcbData, project);
    parseNets(m_pcbData, project);

    spdlog::info("iBOM parsed: {} components, {} nets, {} BOM groups",
        project.components.size(), project.nets.size(), project.bomGroups.size());

    return project;
}

std::optional<nlohmann::json> IBomParser::extractJsVar(const std::string& html,
                                                        const std::string& varName)
{
    // Pattern: var <varName> = <JSON_value>
    // We need to handle potentially huge JSON blocks, so we find the start
    // and then use brace matching to find the end.

    std::string searchStr = "var " + varName + " = ";
    auto pos = html.find(searchStr);
    if (pos == std::string::npos) {
        // Also try: var <varName>=
        searchStr = "var " + varName + "=";
        pos = html.find(searchStr);
    }
    if (pos == std::string::npos) {
        return std::nullopt;
    }

    pos += searchStr.length();

    // Skip whitespace
    while (pos < html.size() && std::isspace(html[pos])) pos++;

    if (pos >= html.size()) return std::nullopt;

    // Check if it starts with { or [ or "
    char startChar = html[pos];

    if (startChar == '{' || startChar == '[') {
        // Brace matching to find the end of the JSON
        char openChar = startChar;
        char closeChar = (startChar == '{') ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        size_t start = pos;

        for (size_t i = pos; i < html.size(); ++i) {
            char c = html[i];

            if (escaped) {
                escaped = false;
                continue;
            }

            if (c == '\\') {
                escaped = true;
                continue;
            }

            if (c == '"') {
                inString = !inString;
                continue;
            }

            if (!inString) {
                if (c == openChar) depth++;
                else if (c == closeChar) {
                    depth--;
                    if (depth == 0) {
                        std::string jsonStr = html.substr(start, i - start + 1);
                        try {
                            return nlohmann::json::parse(jsonStr);
                        } catch (const std::exception& e) {
                            spdlog::error("JSON parse error for '{}': {}", varName, e.what());
                            return std::nullopt;
                        }
                    }
                }
            }
        }
    } else if (startChar == '"') {
        // Simple string value
        size_t endPos = html.find('"', pos + 1);
        while (endPos != std::string::npos && html[endPos - 1] == '\\') {
            endPos = html.find('"', endPos + 1);
        }
        if (endPos != std::string::npos) {
            std::string str = html.substr(pos, endPos - pos + 1);
            return nlohmann::json::parse(str);
        }
    } else {
        // Boolean, number, etc. — find the semicolon or newline
        size_t endPos = html.find_first_of(";\n", pos);
        if (endPos != std::string::npos) {
            std::string val = html.substr(pos, endPos - pos);
            // Trim
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            try {
                return nlohmann::json::parse(val);
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> IBomParser::decompressLZString(const std::string& encoded)
{
    // LZString decompression — this is a simplified C++ port of the JS LZString library
    // For now we log a warning; full implementation would port the decompressFromBase64 function.
    // TODO: Implement full LZString decompression in C++
    spdlog::warn("LZString decompression not yet implemented. Use uncompressed iBOM files.");
    return std::nullopt;
}

void IBomParser::parseConfig(const nlohmann::json& config, IBomProject& project)
{
    project.darkMode       = config.value("dark_mode", false);
    project.showPads       = config.value("show_pads", true);
    project.showFabrication = config.value("show_fabrication", false);
    project.showSilkscreen = config.value("show_silkscreen", true);
    project.highlightPin1  = config.value("highlight_pin1", "none");

    if (config.contains("checkboxes")) {
        std::string cbStr = config["checkboxes"].get<std::string>();
        // Split by comma
        std::istringstream iss(cbStr);
        std::string token;
        while (std::getline(iss, token, ',')) {
            // Trim whitespace
            token.erase(0, token.find_first_not_of(" "));
            token.erase(token.find_last_not_of(" ") + 1);
            if (!token.empty()) {
                project.checkboxColumns.push_back(token);
            }
        }
    }

    if (config.contains("fields")) {
        project.fields = config["fields"].get<std::vector<std::string>>();
    }
}

void IBomParser::parseBoardInfo(const nlohmann::json& pcbdata, IBomProject& project)
{
    if (pcbdata.contains("metadata")) {
        auto& meta = pcbdata["metadata"];
        project.boardInfo.title    = meta.value("title", "");
        project.boardInfo.revision = meta.value("revision", "");
        project.boardInfo.date     = meta.value("date", "");
        project.boardInfo.company  = meta.value("company", "");
    }

    // Board bounding box
    if (pcbdata.contains("edges_bbox")) {
        auto& bb = pcbdata["edges_bbox"];
        project.boardInfo.boardBBox.minX = bb.value("minx", 0.0);
        project.boardInfo.boardBBox.minY = bb.value("miny", 0.0);
        project.boardInfo.boardBBox.maxX = bb.value("maxx", 0.0);
        project.boardInfo.boardBBox.maxY = bb.value("maxy", 0.0);
    }
}

void IBomParser::parseBoardOutline(const nlohmann::json& pcbdata, IBomProject& project)
{
    if (!pcbdata.contains("edges")) return;

    for (const auto& edge : pcbdata["edges"]) {
        DrawingSegment seg;
        std::string type = edge.value("type", "line");

        if (type == "segment" || type == "line") {
            seg.type = DrawingSegment::Type::Line;
            if (edge.contains("start")) {
                seg.start.x = edge["start"].value("x", 0.0);
                seg.start.y = edge["start"].value("y", 0.0);
            }
            if (edge.contains("end")) {
                seg.end.x = edge["end"].value("x", 0.0);
                seg.end.y = edge["end"].value("y", 0.0);
            }
        } else if (type == "arc") {
            seg.type = DrawingSegment::Type::Arc;
            if (edge.contains("start")) {
                seg.start.x = edge["start"].value("x", 0.0);
                seg.start.y = edge["start"].value("y", 0.0);
            }
            if (edge.contains("end")) {
                seg.end.x = edge["end"].value("x", 0.0);
                seg.end.y = edge["end"].value("y", 0.0);
            }
            seg.radius = edge.value("radius", 0.0);
            seg.angle  = edge.value("angle", 0.0);
        } else if (type == "circle") {
            seg.type = DrawingSegment::Type::Circle;
            if (edge.contains("start")) {
                seg.start.x = edge["start"].value("x", 0.0);
                seg.start.y = edge["start"].value("y", 0.0);
            }
            seg.radius = edge.value("radius", 0.0);
        }

        seg.width = edge.value("width", 0.0);
        project.boardOutline.push_back(seg);
    }
}

void IBomParser::parseComponents(const nlohmann::json& pcbdata, IBomProject& project)
{
    if (!pcbdata.contains("footprints")) return;

    for (const auto& fp : pcbdata["footprints"]) {
        // Determine layer
        Layer layer = Layer::Front;
        if (fp.contains("layer")) {
            std::string layerStr = fp["layer"].get<std::string>();
            if (layerStr == "B" || layerStr == "back" || layerStr == "B.Cu") {
                layer = Layer::Back;
            }
        }

        Component comp = parseFootprint(fp, layer);
        project.components.push_back(std::move(comp));
    }

    spdlog::debug("Parsed {} components", project.components.size());
}

Component IBomParser::parseFootprint(const nlohmann::json& fp, Layer layer)
{
    Component comp;
    comp.layer     = layer;
    comp.reference = fp.value("ref", "");
    comp.value     = fp.value("val", "");
    comp.footprint = fp.value("footprint", "");

    // Position and rotation
    if (fp.contains("center")) {
        comp.position.x = fp["center"].value("x", 0.0);
        comp.position.y = fp["center"].value("y", 0.0);
    }
    comp.rotation = fp.value("rotation", 0.0);

    // Bounding box
    if (fp.contains("bbox")) {
        auto& bb = fp["bbox"];
        comp.bbox.minX = bb.value("minx", 0.0);
        comp.bbox.minY = bb.value("miny", 0.0);
        comp.bbox.maxX = bb.value("maxx", 0.0);
        comp.bbox.maxY = bb.value("maxy", 0.0);
    }

    // Pads
    if (fp.contains("pads")) {
        comp.pads = parsePads(fp["pads"]);
    }

    // Drawings (silkscreen, courtyard, etc.)
    if (fp.contains("drawings")) {
        comp.drawings = parseDrawings(fp["drawings"]);
    }

    // Extra fields
    if (fp.contains("extra_fields")) {
        for (auto& [key, val] : fp["extra_fields"].items()) {
            if (val.is_string()) {
                comp.extraFields[key] = val.get<std::string>();
            }
        }
    }

    return comp;
}

std::vector<Pad> IBomParser::parsePads(const nlohmann::json& padsJson)
{
    std::vector<Pad> pads;

    for (const auto& p : padsJson) {
        Pad pad;
        pad.pinNumber = p.value("num", "");
        pad.netName   = p.value("net", "");

        if (p.contains("pos")) {
            pad.position.x = p["pos"].value("x", 0.0);
            pad.position.y = p["pos"].value("y", 0.0);
        }
        pad.angle = p.value("angle", 0.0);

        // Size
        if (p.contains("size")) {
            pad.sizeX = p["size"].value("x", 0.0);
            pad.sizeY = p["size"].value("y", 0.0);
        }

        // Shape
        std::string shapeStr = p.value("shape", "rect");
        if (shapeStr == "rect")           pad.shape = Pad::Shape::Rect;
        else if (shapeStr == "roundrect") pad.shape = Pad::Shape::RoundRect;
        else if (shapeStr == "circle")    pad.shape = Pad::Shape::Circle;
        else if (shapeStr == "oval")      pad.shape = Pad::Shape::Oval;
        else if (shapeStr == "trapezoid") pad.shape = Pad::Shape::Trapezoid;
        else                              pad.shape = Pad::Shape::Custom;

        pad.isPin1 = (pad.pinNumber == "1" || pad.pinNumber == "A1");
        pad.isSMD  = p.value("type", "smd") == "smd";

        pads.push_back(pad);
    }

    return pads;
}

std::vector<DrawingSegment> IBomParser::parseDrawings(const nlohmann::json& drawingsJson)
{
    std::vector<DrawingSegment> drawings;

    for (const auto& d : drawingsJson) {
        DrawingSegment seg;
        std::string type = d.value("type", "segment");

        if (type == "segment" || type == "line") {
            seg.type = DrawingSegment::Type::Line;
        } else if (type == "arc") {
            seg.type = DrawingSegment::Type::Arc;
            seg.radius = d.value("radius", 0.0);
            seg.angle  = d.value("angle", 0.0);
        } else if (type == "circle") {
            seg.type = DrawingSegment::Type::Circle;
            seg.radius = d.value("radius", 0.0);
        } else if (type == "polygon") {
            seg.type = DrawingSegment::Type::Polygon;
            if (d.contains("polygons")) {
                for (const auto& pt : d["polygons"]) {
                    seg.points.push_back({pt.value("x", 0.0), pt.value("y", 0.0)});
                }
            }
        } else if (type == "rect") {
            seg.type = DrawingSegment::Type::Rect;
        }

        if (d.contains("start")) {
            seg.start.x = d["start"].value("x", 0.0);
            seg.start.y = d["start"].value("y", 0.0);
        }
        if (d.contains("end")) {
            seg.end.x = d["end"].value("x", 0.0);
            seg.end.y = d["end"].value("y", 0.0);
        }
        seg.width = d.value("width", 0.0);

        drawings.push_back(seg);
    }

    return drawings;
}

void IBomParser::parseBomGroups(const nlohmann::json& pcbdata, IBomProject& project)
{
    if (!pcbdata.contains("bom")) return;
    auto& bom = pcbdata["bom"];

    if (!bom.contains("both")) return;

    for (const auto& group : bom["both"]) {
        BomGroup bg;

        // group is typically: [ [index, refs...], value, footprint, ... ]
        if (group.is_array() && group.size() >= 3) {
            // References are in the first element (array of ref indices or names)
            if (group[0].is_array()) {
                for (const auto& ref : group[0]) {
                    if (ref.is_string()) {
                        bg.references.push_back(ref.get<std::string>());
                    } else if (ref.is_number_integer()) {
                        // Index into the components list — resolve later
                        bg.references.push_back(std::to_string(ref.get<int>()));
                    }
                }
            }
            if (group[1].is_string()) bg.value     = group[1].get<std::string>();
            if (group[2].is_string()) bg.footprint  = group[2].get<std::string>();
        }

        project.bomGroups.push_back(std::move(bg));
    }
}

void IBomParser::parseNets(const nlohmann::json& pcbdata, IBomProject& project)
{
    if (!pcbdata.contains("nets")) return;

    for (auto& [name, pads] : pcbdata["nets"].items()) {
        Net net;
        net.name = name;
        if (pads.is_array()) {
            for (const auto& p : pads) {
                if (p.is_string()) {
                    net.pads.push_back(p.get<std::string>());
                }
            }
        }
        project.nets.push_back(std::move(net));
    }
}

} // namespace ibom
