#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ibom::features {

/// One known board in the library (C2). Keyed by the iBOM content hash — the
/// same key the golden store and annotations use, so everything the app knows
/// about a board joins on it.
struct BoardLibraryEntry {
    std::string hash;         ///< 12-hex content hash (primary key)
    std::string path;         ///< last known iBOM file path (may go stale)
    std::string title;        ///< boardInfo.title, or the file name
    std::string lastOpened;   ///< ISO-8601 of the most recent load
    int  components = 0;
    int  placed     = 0;      ///< inspection progress at last load/save
    bool hasGolden  = false;  ///< golden/<hash>/ exists
    bool hasNotes   = false;  ///< annotations/<hash>.json has entries
};

/**
 * @brief Board library (FEATURE_PROPOSALS C2) — pure, unit-testable registry
 *        of every board opened so far, persisted as
 *        `dataDir()/board_library.json`.
 *
 * The library only records; the per-board data itself stays where each
 * feature put it (golden/, annotations/, session_state.json), all keyed by
 * the same content hash. Best-effort I/O: failures never block a board load.
 */
class BoardLibrary {
public:
    /// Bind to (and load, if present) the library file. Missing file ⇒ empty
    /// library, returns true; unparseable ⇒ empty, returns false.
    bool open(const std::filesystem::path& file);

    /// Insert or refresh a board (keyed by entry.hash) and persist. Entries
    /// with an empty hash are ignored (unhashable file). Returns false when
    /// the write failed (in-memory state updated regardless).
    bool touch(const BoardLibraryEntry& entry);

    /// All known boards, most recently opened first.
    std::vector<BoardLibraryEntry> entries() const;

    /// Remove one board from the library (does NOT delete its stored data).
    bool remove(const std::string& hash);

    std::size_t count() const { return m_entries.size(); }

private:
    bool save() const;

    std::filesystem::path m_file;
    std::map<std::string, BoardLibraryEntry> m_entries;   // hash → entry
};

} // namespace ibom::features
