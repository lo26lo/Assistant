#pragma once

#include <QObject>
#include <QString>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/log_msg.h>
#include <mutex>

namespace ibom::utils {

/// Bridges spdlog records to the Qt world.
///
/// A single instance (singleton) emits `messageLogged` for each record routed
/// through the `qt_signal_sink`. The sink may be invoked from any thread
/// (camera/tracking workers), but because the GUI receiver lives in the main
/// thread, Qt::AutoConnection marshals the slot call onto the GUI thread.
class LogBridge : public QObject {
    Q_OBJECT
public:
    static LogBridge& instance();

    /// Called from the spdlog sink — thread-safe (Qt signals may be emitted
    /// from any thread).
    void post(int level, const QString& logger, const QString& message);

signals:
    /// `level` is the spdlog::level::level_enum value as int
    /// (trace=0, debug=1, info=2, warn=3, err=4, critical=5).
    void messageLogged(int level, const QString& logger, const QString& message);

private:
    explicit LogBridge(QObject* parent = nullptr);
};

/// spdlog sink that forwards each record's level + payload to LogBridge.
/// The text formatting is left to the GUI; only the raw payload is passed.
template <typename Mutex>
class qt_signal_sink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const QString text = QString::fromUtf8(
            msg.payload.data(), static_cast<int>(msg.payload.size()));
        const QString logger = QString::fromUtf8(
            msg.logger_name.data(), static_cast<int>(msg.logger_name.size()));
        LogBridge::instance().post(static_cast<int>(msg.level), logger, text);
    }
    void flush_() override {}
};

using qt_signal_sink_mt = qt_signal_sink<std::mutex>;

} // namespace ibom::utils
