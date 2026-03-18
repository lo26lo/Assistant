#include "SnapshotHistory.h"

#include <QDir>
#include <QFileInfo>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ibom::features {

SnapshotHistory::SnapshotHistory(QObject* parent)
    : QObject(parent)
{
}

void SnapshotHistory::setStorageDir(const QString& dir)
{
    m_storageDir = dir;
    QDir d(dir);
    if (!d.exists()) {
        d.mkpath(".");
    }
    spdlog::info("SnapshotHistory: storage dir set to '{}'", dir.toStdString());
}

int SnapshotHistory::takeSnapshot(const QImage& image, const QString& label,
                                   const QString& componentRef)
{
    if (image.isNull()) return -1;

    Snapshot snap;
    snap.id           = m_nextId++;
    snap.image        = image;
    snap.timestamp    = QDateTime::currentDateTime();
    snap.label        = label.isEmpty() ? QString("Snapshot_%1").arg(snap.id) : label;
    snap.componentRef = componentRef;

    // Save to disk
    if (!m_storageDir.isEmpty()) {
        snap.filePath = m_storageDir + "/" + generateFilename(snap.label);
        if (snap.image.save(snap.filePath, "PNG")) {
            spdlog::info("SnapshotHistory: saved snapshot #{} to '{}'",
                         snap.id, snap.filePath.toStdString());
        } else {
            spdlog::warn("SnapshotHistory: failed to save snapshot #{}", snap.id);
        }
    }

    m_snapshots.insert(m_snapshots.begin(), snap); // Most recent first
    emit snapshotTaken(snap.id, snap.label);

    return snap.id;
}

void SnapshotHistory::addNotes(int id, const QString& notes)
{
    for (auto& snap : m_snapshots) {
        if (snap.id == id) {
            snap.notes = notes;
            return;
        }
    }
}

bool SnapshotHistory::deleteSnapshot(int id)
{
    auto it = std::find_if(m_snapshots.begin(), m_snapshots.end(),
                           [id](const Snapshot& s) { return s.id == id; });
    if (it == m_snapshots.end()) return false;

    // Remove file
    if (!it->filePath.isEmpty()) {
        QFile::remove(it->filePath);
    }

    m_snapshots.erase(it);
    emit snapshotDeleted(id);
    return true;
}

std::optional<SnapshotHistory::Snapshot> SnapshotHistory::getSnapshot(int id) const
{
    for (const auto& snap : m_snapshots) {
        if (snap.id == id) return snap;
    }
    return std::nullopt;
}

std::vector<SnapshotHistory::Snapshot>
SnapshotHistory::snapshotsForComponent(const QString& reference) const
{
    std::vector<Snapshot> result;
    for (const auto& snap : m_snapshots) {
        if (snap.componentRef == reference) {
            result.push_back(snap);
        }
    }
    return result;
}

void SnapshotHistory::clear()
{
    // Remove all files
    for (const auto& snap : m_snapshots) {
        if (!snap.filePath.isEmpty()) {
            QFile::remove(snap.filePath);
        }
    }
    m_snapshots.clear();
    emit snapshotsCleared();
}

bool SnapshotHistory::exportSnapshot(int id, const QString& path) const
{
    auto snap = getSnapshot(id);
    if (!snap) return false;

    return snap->image.save(path);
}

QString SnapshotHistory::generateFilename(const QString& label) const
{
    QString safe = label;
    safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    return QString("%1_%2.png").arg(timestamp, safe);
}

} // namespace ibom::features
