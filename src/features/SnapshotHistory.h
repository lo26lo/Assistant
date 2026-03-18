#pragma once

#include <QObject>
#include <QImage>
#include <QDateTime>
#include <QString>
#include <vector>
#include <optional>

namespace ibom::features {

/// Snapshot capture, annotation, and history management
class SnapshotHistory : public QObject {
    Q_OBJECT

public:
    explicit SnapshotHistory(QObject* parent = nullptr);
    ~SnapshotHistory() override = default;

    struct Snapshot {
        int       id = 0;
        QImage    image;
        QDateTime timestamp;
        QString   label;
        QString   componentRef;   // Associated component (if any)
        QString   notes;
        QString   filePath;       // On-disk path
    };

    /// Set the directory for saving snapshots
    void setStorageDir(const QString& dir);
    QString storageDir() const { return m_storageDir; }

    /// Take a snapshot of the given image
    int takeSnapshot(const QImage& image, const QString& label = {},
                     const QString& componentRef = {});

    /// Add notes to a snapshot
    void addNotes(int id, const QString& notes);

    /// Delete a snapshot
    bool deleteSnapshot(int id);

    /// Get snapshot by ID
    std::optional<Snapshot> getSnapshot(int id) const;

    /// Get all snapshots (most recent first)
    const std::vector<Snapshot>& snapshots() const { return m_snapshots; }

    /// Get snapshots for a specific component
    std::vector<Snapshot> snapshotsForComponent(const QString& reference) const;

    /// Clear all snapshots
    void clear();

    /// Export snapshot to file
    bool exportSnapshot(int id, const QString& path) const;

    int count() const { return static_cast<int>(m_snapshots.size()); }

signals:
    void snapshotTaken(int id, const QString& label);
    void snapshotDeleted(int id);
    void snapshotsCleared();

private:
    QString generateFilename(const QString& label) const;

    QString m_storageDir;
    std::vector<Snapshot> m_snapshots;
    int m_nextId = 1;
};

} // namespace ibom::features
