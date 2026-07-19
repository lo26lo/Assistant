#include "BoardLibrary.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

namespace ibom::features {

bool BoardLibrary::open(const std::filesystem::path& file)
{
    m_file = file;
    m_entries.clear();

    std::ifstream ifs(file);
    if (!ifs.good())
        return true;   // first run — empty library bound to this path

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        spdlog::warn("[library] unparseable {} ({}) — starting empty",
                     file.string(), e.what());
        return false;
    }
    if (!root.is_object())
        return false;

    for (const auto& [hash, j] : root.items()) {
        if (!j.is_object() || hash.empty()) continue;
        BoardLibraryEntry e;
        e.hash       = hash;
        e.path       = j.value("path", "");
        e.title      = j.value("title", "");
        e.lastOpened = j.value("last_opened", "");
        e.components = j.value("components", 0);
        e.placed     = j.value("placed", 0);
        e.hasGolden  = j.value("has_golden", false);
        e.hasNotes   = j.value("has_notes", false);
        m_entries.emplace(hash, std::move(e));
    }
    return true;
}

bool BoardLibrary::touch(const BoardLibraryEntry& entry)
{
    if (entry.hash.empty())
        return false;
    m_entries[entry.hash] = entry;
    return save();
}

std::vector<BoardLibraryEntry> BoardLibrary::entries() const
{
    std::vector<BoardLibraryEntry> out;
    out.reserve(m_entries.size());
    for (const auto& [hash, e] : m_entries)
        out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const BoardLibraryEntry& a, const BoardLibraryEntry& b) {
                  return a.lastOpened > b.lastOpened;   // ISO-8601 sorts lexically
              });
    return out;
}

bool BoardLibrary::remove(const std::string& hash)
{
    if (m_entries.erase(hash) == 0)
        return false;
    return save();
}

bool BoardLibrary::save() const
{
    if (m_file.empty())
        return false;

    nlohmann::json root = nlohmann::json::object();
    for (const auto& [hash, e] : m_entries) {
        root[hash] = { {"path", e.path},
                       {"title", e.title},
                       {"last_opened", e.lastOpened},
                       {"components", e.components},
                       {"placed", e.placed},
                       {"has_golden", e.hasGolden},
                       {"has_notes", e.hasNotes} };
    }

    std::error_code ec;
    std::filesystem::create_directories(m_file.parent_path(), ec);
    const std::filesystem::path tmp = m_file.string() + ".tmp";
    {
        std::ofstream ofs(tmp);
        if (!ofs.good()) {
            spdlog::warn("[library] cannot write {}", tmp.string());
            return false;
        }
        ofs << root.dump(2);
        if (!ofs.good()) return false;
    }
    std::filesystem::rename(tmp, m_file, ec);
    if (ec) {
        spdlog::warn("[library] rename {} failed: {}", m_file.string(), ec.message());
        return false;
    }
    return true;
}

} // namespace ibom::features
