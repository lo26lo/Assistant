#pragma once

#include "IBomData.h"
#include <string>
#include <optional>

namespace ibom {

/**
 * @brief Parses InteractiveHtmlBom HTML files.
 *
 * Extracts the embedded JSON data (pcbdata, config, bom) from
 * the self-contained iBOM HTML file and builds an IBomProject.
 *
 * The iBOM HTML contains JavaScript variables like:
 *   var pcbdata = {...};
 *   var config = {...};
 * which we extract via regex then parse as JSON.
 */
class IBomParser {
public:
    IBomParser() = default;
    ~IBomParser() = default;

    /// Parse an iBOM HTML file from disk.
    /// @return The parsed project data, or nullopt on failure.
    std::optional<IBomProject> parseFile(const std::string& filePath);

    /// Parse iBOM HTML content from a string.
    std::optional<IBomProject> parseString(const std::string& htmlContent);

    /// Get the raw pcbdata JSON (after extraction).
    const nlohmann::json& rawPcbData() const { return m_pcbData; }

    /// Get the raw config JSON.
    const nlohmann::json& rawConfig() const { return m_configData; }

    /// Get the last error message.
    const std::string& lastError() const { return m_lastError; }

private:
    /// Extract a JavaScript variable assignment as JSON from HTML.
    /// Looks for: var <varName> = <JSON>;
    std::optional<nlohmann::json> extractJsVar(const std::string& html,
                                                const std::string& varName);

    /// Extract LZString compressed data if present.
    std::optional<std::string> decompressLZString(const std::string& encoded);

    /// Parse the config block.
    void parseConfig(const nlohmann::json& config, IBomProject& project);

    /// Parse board metadata.
    void parseBoardInfo(const nlohmann::json& pcbdata, IBomProject& project);

    /// Parse the edges/outline of the board.
    void parseBoardOutline(const nlohmann::json& pcbdata, IBomProject& project);

    /// Parse all component footprints.
    void parseComponents(const nlohmann::json& pcbdata, IBomProject& project);

    /// Parse a single footprint from pcbdata.
    Component parseFootprint(const nlohmann::json& fp, Layer layer);

    /// Parse pads for a component.
    std::vector<Pad> parsePads(const nlohmann::json& padsJson);

    /// Parse drawing segments (silkscreen, courtyard, etc.).
    std::vector<DrawingSegment> parseDrawings(const nlohmann::json& drawingsJson);

    /// Parse the BOM table groupings.
    void parseBomGroups(const nlohmann::json& pcbdata, IBomProject& project);

    /// Parse nets.
    void parseNets(const nlohmann::json& pcbdata, IBomProject& project);

    nlohmann::json m_pcbData;
    nlohmann::json m_configData;
    std::string    m_lastError;
};

} // namespace ibom
