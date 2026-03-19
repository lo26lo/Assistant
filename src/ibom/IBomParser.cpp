#include "IBomParser.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <unordered_map>

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

    // Log top-level keys for debugging structure mismatches
    {
        std::string configKeys, pcbKeys;
        for (auto& [k, v] : m_configData.items()) configKeys += k + " ";
        for (auto& [k, v] : m_pcbData.items()) pcbKeys += k + " ";
        spdlog::info("iBOM config keys: {}", configKeys);
        spdlog::info("iBOM pcbdata keys: {}", pcbKeys);
    }

    try { parseConfig(m_configData, project); }
    catch (const std::exception& e) {
        spdlog::error("parseConfig failed: {}", e.what());
        return std::nullopt;
    }

    try { parseBoardInfo(m_pcbData, project); }
    catch (const std::exception& e) {
        spdlog::error("parseBoardInfo failed: {}", e.what());
        return std::nullopt;
    }

    try { parseBoardOutline(m_pcbData, project); }
    catch (const std::exception& e) {
        spdlog::error("parseBoardOutline failed: {}", e.what());
        return std::nullopt;
    }

    try { parseComponents(m_pcbData, project); }
    catch (const std::exception& e) {
        spdlog::error("parseComponents failed: {}", e.what());
        return std::nullopt;
    }

    try { parseBomGroups(m_pcbData, project); }
    catch (const std::exception& e) {
        spdlog::error("parseBomGroups failed: {}", e.what());
        return std::nullopt;
    }

    try { parseNets(m_pcbData, project); }
    catch (const std::exception& e) {
        spdlog::error("parseNets failed: {}", e.what());
        return std::nullopt;
    }

    // Cross-reference: populate component value/footprint from BOM groups
    // (footprint objects in iBOM don't carry val/footprint — only bom.fields does)
    {
        std::unordered_map<std::string, const BomGroup*> refToGroup;
        for (const auto& bg : project.bomGroups) {
            for (const auto& ref : bg.references) {
                refToGroup[ref] = &bg;
            }
        }
        int filled = 0;
        for (auto& comp : project.components) {
            auto it = refToGroup.find(comp.reference);
            if (it != refToGroup.end()) {
                if (comp.value.empty())     comp.value     = it->second->value;
                if (comp.footprint.empty()) comp.footprint = it->second->footprint;
                filled++;
            }
        }
        spdlog::info("Cross-referenced {}/{} components with BOM data", filled, project.components.size());
    }

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

    // Check for: JSON.parse(LZString.decompressFromBase64("..."))
    const std::string lzPrefix = "JSON.parse(LZString.decompressFromBase64(\"";
    if (html.compare(pos, lzPrefix.size(), lzPrefix) == 0) {
        pos += lzPrefix.size();
        // Find closing "))
        auto endPos = html.find("\"))", pos);
        if (endPos == std::string::npos) {
            spdlog::error("Cannot find end of LZString compressed data for '{}'", varName);
            return std::nullopt;
        }
        std::string base64Data = html.substr(pos, endPos - pos);
        spdlog::info("Found LZString compressed '{}': {} chars", varName, base64Data.size());
        auto decompressed = decompressLZString(base64Data);
        if (!decompressed) {
            spdlog::error("LZString decompression failed for '{}'", varName);
            return std::nullopt;
        }
        try {
            return nlohmann::json::parse(*decompressed);
        } catch (const std::exception& e) {
            spdlog::error("JSON parse error after LZString decompression for '{}': {}", varName, e.what());
            return std::nullopt;
        }
    }

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
    // C++ port of LZString.decompressFromBase64 (JS version 1.4.4)
    // See: https://pieroxy.net/blog/pages/lz-string/index.html
    if (encoded.empty()) return std::nullopt;

    static const std::string keyStr =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";

    // Build reverse lookup
    static std::vector<int> baseReverseDic = []() {
        std::vector<int> dic(256, -1);
        for (int i = 0; i < static_cast<int>(keyStr.size()); i++)
            dic[static_cast<unsigned char>(keyStr[i])] = i;
        return dic;
    }();

    auto getBaseValue = [&](size_t index) -> int {
        if (index >= encoded.size()) return 0;
        unsigned char c = static_cast<unsigned char>(encoded[index]);
        return (c < 256) ? baseReverseDic[c] : 0;
    };

    // _decompress(length, resetValue=32, getNextValue)
    int length = static_cast<int>(encoded.size());
    int resetValue = 32;

    struct DataState {
        int val;
        int position;
        int index;
    };

    DataState data;
    data.val = getBaseValue(0);
    data.position = resetValue;
    data.index = 1;

    std::vector<std::u16string> dictionary(3);
    dictionary[0] = u"0"; // placeholder
    dictionary[1] = u"1"; // placeholder
    dictionary[2] = u"2"; // placeholder
    int dictSize = 3; // next entry index (becomes 4 after first char)

    int numBits = 3;   // JS original: numBits=3 (3-bit codes in main loop)
    int enlargeIn = 4; // pow(2, numBits-1) = 4 slots before enlarging
    std::u16string entry;
    std::u16string w;
    std::u16string result;

    // Read first character
    int bits = 0;
    int maxpower = static_cast<int>(std::pow(2, 2));
    int power = 1;

    while (power != maxpower) {
        int resb = data.val & data.position;
        data.position >>= 1;
        if (data.position == 0) {
            data.position = resetValue;
            data.val = getBaseValue(data.index++);
        }
        bits |= (resb > 0 ? 1 : 0) * power;
        power <<= 1;
    }

    int next = bits;
    switch (next) {
    case 0: {
        bits = 0;
        maxpower = static_cast<int>(std::pow(2, 8));
        power = 1;
        while (power != maxpower) {
            int resb = data.val & data.position;
            data.position >>= 1;
            if (data.position == 0) {
                data.position = resetValue;
                data.val = getBaseValue(data.index++);
            }
            bits |= (resb > 0 ? 1 : 0) * power;
            power <<= 1;
        }
        char16_t c = static_cast<char16_t>(bits);
        dictionary.push_back(std::u16string(1, c));
        dictSize++;
        w = std::u16string(1, c);
        break;
    }
    case 1: {
        bits = 0;
        maxpower = static_cast<int>(std::pow(2, 16));
        power = 1;
        while (power != maxpower) {
            int resb = data.val & data.position;
            data.position >>= 1;
            if (data.position == 0) {
                data.position = resetValue;
                data.val = getBaseValue(data.index++);
            }
            bits |= (resb > 0 ? 1 : 0) * power;
            power <<= 1;
        }
        char16_t c = static_cast<char16_t>(bits);
        dictionary.push_back(std::u16string(1, c));
        dictSize++;
        w = std::u16string(1, c);
        break;
    }
    case 2:
        return ""; // empty string
    }

    result += w;

    // Main decompression loop
    while (true) {
        if (data.index > length) {
            return ""; // end of data
        }

        bits = 0;
        maxpower = static_cast<int>(std::pow(2, numBits));
        power = 1;
        while (power != maxpower) {
            int resb = data.val & data.position;
            data.position >>= 1;
            if (data.position == 0) {
                data.position = resetValue;
                data.val = getBaseValue(data.index++);
            }
            bits |= (resb > 0 ? 1 : 0) * power;
            power <<= 1;
        }

        int c_code = bits;
        switch (c_code) {
        case 0: {
            bits = 0;
            maxpower = static_cast<int>(std::pow(2, 8));
            power = 1;
            while (power != maxpower) {
                int resb = data.val & data.position;
                data.position >>= 1;
                if (data.position == 0) {
                    data.position = resetValue;
                    data.val = getBaseValue(data.index++);
                }
                bits |= (resb > 0 ? 1 : 0) * power;
                power <<= 1;
            }
            char16_t ch = static_cast<char16_t>(bits);
            dictionary.push_back(std::u16string(1, ch));
            c_code = dictSize++;
            enlargeIn--;
            break;
        }
        case 1: {
            bits = 0;
            maxpower = static_cast<int>(std::pow(2, 16));
            power = 1;
            while (power != maxpower) {
                int resb = data.val & data.position;
                data.position >>= 1;
                if (data.position == 0) {
                    data.position = resetValue;
                    data.val = getBaseValue(data.index++);
                }
                bits |= (resb > 0 ? 1 : 0) * power;
                power <<= 1;
            }
            char16_t ch = static_cast<char16_t>(bits);
            dictionary.push_back(std::u16string(1, ch));
            c_code = dictSize++;
            enlargeIn--;
            break;
        }
        case 2:
            goto done; // end marker
        }

        if (enlargeIn == 0) {
            enlargeIn = static_cast<int>(std::pow(2, numBits));
            numBits++;
        }

        if (c_code < static_cast<int>(dictionary.size()) && !dictionary[c_code].empty()) {
            entry = dictionary[c_code];
        } else if (c_code == dictSize) {
            entry = w + w[0];
        } else {
            spdlog::error("LZString: bad compressed data at code {}", c_code);
            return std::nullopt;
        }

        result += entry;

        // Add w + entry[0] to dictionary
        dictionary.push_back(w + entry[0]);
        dictSize++;
        enlargeIn--;

        if (enlargeIn == 0) {
            enlargeIn = static_cast<int>(std::pow(2, numBits));
            numBits++;
        }

        w = entry;
    }

done:
    // Convert u16string to UTF-8 std::string
    std::string output;
    output.reserve(result.size());
    for (char16_t ch : result) {
        if (ch < 0x80) {
            output.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            output.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            output.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }

    spdlog::info("LZString decompressed: {} chars input → {} chars output",
                 encoded.size(), output.size());
    return output;
}

// Helper: read a Point2D from a JSON array [x, y]
static Point2D readPoint(const nlohmann::json& j) {
    if (j.is_array() && j.size() >= 2) {
        return {j[0].get<double>(), j[1].get<double>()};
    }
    return {0.0, 0.0};
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
        } else if (type == "arc") {
            seg.type = DrawingSegment::Type::Arc;
            seg.radius = edge.value("radius", 0.0);
            seg.angle  = edge.value("angle", 0.0);
        } else if (type == "circle") {
            seg.type = DrawingSegment::Type::Circle;
            seg.radius = edge.value("radius", 0.0);
        }

        // Coordinates are arrays [x, y] in iBOM format
        if (edge.contains("start")) seg.start = readPoint(edge["start"]);
        if (edge.contains("end"))   seg.end   = readPoint(edge["end"]);
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

    // Position: center is [x, y] array in iBOM format
    if (fp.contains("center")) {
        comp.position = readPoint(fp["center"]);
    }
    comp.rotation = fp.value("rotation", 0.0);

    // Bounding box: has pos [x,y] and size [w,h] arrays
    if (fp.contains("bbox")) {
        auto& bb = fp["bbox"];
        if (bb.contains("pos") && bb.contains("size")) {
            auto pos  = readPoint(bb["pos"]);
            auto size = readPoint(bb["size"]);
            comp.bbox.minX = pos.x;
            comp.bbox.minY = pos.y;
            comp.bbox.maxX = pos.x + size.x;
            comp.bbox.maxY = pos.y + size.y;
        } else {
            // Fallback: minx/miny/maxx/maxy format
            comp.bbox.minX = bb.value("minx", 0.0);
            comp.bbox.minY = bb.value("miny", 0.0);
            comp.bbox.maxX = bb.value("maxx", 0.0);
            comp.bbox.maxY = bb.value("maxy", 0.0);
        }
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

        // Coordinates are arrays [x, y] in iBOM format
        if (p.contains("pos")) {
            pad.position = readPoint(p["pos"]);
        }
        pad.angle = p.value("angle", 0.0);

        // Size is array [w, h]
        if (p.contains("size")) {
            auto sz = readPoint(p["size"]);
            pad.sizeX = sz.x;
            pad.sizeY = sz.y;
        }

        // Shape
        std::string shapeStr = p.value("shape", "rect");
        if (shapeStr == "rect")           pad.shape = Pad::Shape::Rect;
        else if (shapeStr == "roundrect") pad.shape = Pad::Shape::RoundRect;
        else if (shapeStr == "circle")    pad.shape = Pad::Shape::Circle;
        else if (shapeStr == "oval")      pad.shape = Pad::Shape::Oval;
        else if (shapeStr == "trapezoid") pad.shape = Pad::Shape::Trapezoid;
        else                              pad.shape = Pad::Shape::Custom;

        // pin1 can be a boolean field or deduced from pin number
        if (p.contains("pin1") && p["pin1"].is_boolean()) {
            pad.isPin1 = p["pin1"].get<bool>();
        } else {
            pad.isPin1 = (pad.pinNumber == "1" || pad.pinNumber == "A1");
        }
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
            // Polygons are nested arrays: [ [[x1,y1],[x2,y2],...], ... ]
            if (d.contains("polygons")) {
                for (const auto& polygon : d["polygons"]) {
                    if (polygon.is_array()) {
                        for (const auto& pt : polygon) {
                            seg.points.push_back(readPoint(pt));
                        }
                    }
                }
            }
        } else if (type == "rect") {
            seg.type = DrawingSegment::Type::Rect;
        }

        // Coordinates are arrays [x, y] in iBOM format
        if (d.contains("start")) seg.start = readPoint(d["start"]);
        if (d.contains("end"))   seg.end   = readPoint(d["end"]);
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

    // fields dict: { "fpId": [value, footprint, ...extraFields] }
    nlohmann::json fields;
    if (bom.contains("fields")) {
        fields = bom["fields"];
    }

    // bom.both = [ group1, group2, ... ]
    // each group = [ [ref_name, fp_id], [ref_name, fp_id], ... ]
    for (const auto& group : bom["both"]) {
        if (!group.is_array() || group.empty()) continue;

        BomGroup bg;

        // Collect references and get field data from first item
        int firstFpId = -1;
        for (const auto& item : group) {
            if (item.is_array() && item.size() >= 2) {
                std::string refName = item[0].get<std::string>();
                bg.references.push_back(refName);
                if (firstFpId < 0 && item[1].is_number_integer()) {
                    firstFpId = item[1].get<int>();
                }
            }
        }

        // Look up value/footprint from fields dict
        if (firstFpId >= 0 && fields.contains(std::to_string(firstFpId))) {
            auto& f = fields[std::to_string(firstFpId)];
            if (f.is_array()) {
                if (f.size() > 0 && f[0].is_string()) bg.value     = f[0].get<std::string>();
                if (f.size() > 1 && f[1].is_string()) bg.footprint = f[1].get<std::string>();
                // Extra fields beyond value and footprint
                for (size_t i = 2; i < f.size(); ++i) {
                    if (f[i].is_string() && i - 2 < project.fields.size()) {
                        bg.extraFields[project.fields[i - 2]] = f[i].get<std::string>();
                    }
                }
            }
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
