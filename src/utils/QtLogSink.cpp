#include "QtLogSink.h"

namespace ibom::utils {

LogBridge::LogBridge(QObject* parent) : QObject(parent) {}

LogBridge& LogBridge::instance()
{
    static LogBridge s_instance;
    return s_instance;
}

void LogBridge::post(int level, const QString& logger, const QString& message)
{
    emit messageLogged(level, logger, message);
}

} // namespace ibom::utils
