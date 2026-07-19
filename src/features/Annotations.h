#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace ibom::features {

/// One pinned note on a component (FEATURE_PROPOSALS B2).
struct Annotation {
    std::string text;
    std::string timestamp;   ///< ISO-8601, set when the note is written
    std::string face;        ///< "front" / "back" — side active when noted
};

/**
 * @brief Per-board component annotation store (B2) — pure, unit-testable.
 *
 * Keyed by the board's pcbdata content hash, NOT its file path (same keying
 * as the golden store and the board library): the JSON lives at
 * `dataDir()/annotations/<hash>.json` as { ref: [ {text, timestamp, face} ] }.
 * The on-disk value is an array for forward compatibility (multiple notes per
 * ref); the V1 editor keeps a single note per ref (edit replaces).
 *
 * All methods are best-effort: I/O failures never throw, they return false /
 * leave the store empty — a broken annotations file must never block loading
 * a board.
 */
class AnnotationStore {
public:
    /// Bind to (and load, if it exists) the store file for one board.
    /// Missing file ⇒ empty store, returns true. Unparseable file ⇒ empty
    /// store, returns false.
    bool open(const std::filesystem::path& file);

    /// Forget everything, including the bound file (no further saves).
    void clear();

    /// Latest note text for a ref, empty if none.
    std::string noteText(const std::string& ref) const;

    bool hasNote(const std::string& ref) const;

    /// All refs carrying at least one note (minimap markers, report join).
    std::unordered_set<std::string> annotatedRefs() const;

    /// Full note list for a ref (empty vector if none).
    const std::vector<Annotation>& notesFor(const std::string& ref) const;

    /// Set/replace the note for a ref and persist immediately. Empty text
    /// removes the ref's notes. Returns false if the write failed (the
    /// in-memory state is updated regardless).
    bool setNote(const std::string& ref, const std::string& text,
                 const std::string& timestamp, const std::string& face);

    std::size_t count() const { return m_notes.size(); }

private:
    bool save() const;

    std::filesystem::path m_file;
    std::map<std::string, std::vector<Annotation>> m_notes;
};

} // namespace ibom::features
