#include "Annotations.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>

namespace ibom::features {

bool AnnotationStore::open(const std::filesystem::path& file)
{
    m_file = file;
    m_notes.clear();

    std::ifstream ifs(file);
    if (!ifs.good())
        return true;   // no file yet — empty store bound to this path

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        spdlog::warn("[notes] unparseable {} ({}) — starting empty",
                     file.string(), e.what());
        return false;
    }
    if (!root.is_object())
        return false;

    for (const auto& [ref, arr] : root.items()) {
        if (!arr.is_array()) continue;
        std::vector<Annotation> notes;
        for (const auto& n : arr) {
            if (!n.is_object()) continue;
            Annotation a;
            a.text      = n.value("text", "");
            a.timestamp = n.value("timestamp", "");
            a.face      = n.value("face", "");
            if (!a.text.empty())
                notes.push_back(std::move(a));
        }
        if (!notes.empty())
            m_notes.emplace(ref, std::move(notes));
    }
    return true;
}

void AnnotationStore::clear()
{
    m_file.clear();
    m_notes.clear();
}

std::string AnnotationStore::noteText(const std::string& ref) const
{
    const auto it = m_notes.find(ref);
    if (it == m_notes.end() || it->second.empty()) return {};
    return it->second.back().text;
}

bool AnnotationStore::hasNote(const std::string& ref) const
{
    return m_notes.count(ref) > 0;
}

std::unordered_set<std::string> AnnotationStore::annotatedRefs() const
{
    std::unordered_set<std::string> refs;
    refs.reserve(m_notes.size());
    for (const auto& [ref, notes] : m_notes)
        refs.insert(ref);
    return refs;
}

const std::vector<Annotation>& AnnotationStore::notesFor(const std::string& ref) const
{
    static const std::vector<Annotation> kEmpty;
    const auto it = m_notes.find(ref);
    return it == m_notes.end() ? kEmpty : it->second;
}

bool AnnotationStore::setNote(const std::string& ref, const std::string& text,
                              const std::string& timestamp, const std::string& face)
{
    if (text.empty())
        m_notes.erase(ref);
    else
        m_notes[ref] = { Annotation{text, timestamp, face} };
    return save();
}

bool AnnotationStore::save() const
{
    if (m_file.empty())
        return false;

    nlohmann::json root = nlohmann::json::object();
    for (const auto& [ref, notes] : m_notes) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : notes)
            arr.push_back({ {"text", a.text},
                            {"timestamp", a.timestamp},
                            {"face", a.face} });
        root[ref] = std::move(arr);
    }

    std::error_code ec;
    std::filesystem::create_directories(m_file.parent_path(), ec);
    // Atomic-ish write: tmp + rename, so a kill mid-write can't truncate the
    // store (same failure mode flagged for session_state.json in the audit).
    const std::filesystem::path tmp = m_file.string() + ".tmp";
    {
        std::ofstream ofs(tmp);
        if (!ofs.good()) {
            spdlog::warn("[notes] cannot write {}", tmp.string());
            return false;
        }
        ofs << root.dump(2);
        if (!ofs.good()) return false;
    }
    std::filesystem::rename(tmp, m_file, ec);
    if (ec) {
        spdlog::warn("[notes] rename {} failed: {}", m_file.string(), ec.message());
        return false;
    }
    return true;
}

} // namespace ibom::features
