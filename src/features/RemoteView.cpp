#include "RemoteView.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <spdlog/spdlog.h>

namespace ibom::features {

RemoteView::RemoteView(QObject* parent)
    : QObject(parent)
{
    m_frameTimer = new QTimer(this);
    connect(m_frameTimer, &QTimer::timeout, this, &RemoteView::broadcastFrame);
}

RemoteView::~RemoteView()
{
    stop();
}

bool RemoteView::start(quint16 port)
{
    if (m_server && m_server->isListening()) {
        spdlog::warn("RemoteView: server already running on port {}", m_port);
        return false;
    }

    m_port = port;
    m_server = std::make_unique<QWebSocketServer>(
        "PCBInspector", QWebSocketServer::NonSecureMode, this);

    if (!m_server->listen(QHostAddress::Any, port)) {
        emit error(QString("Failed to start WebSocket server on port %1: %2")
                       .arg(port).arg(m_server->errorString()));
        spdlog::error("RemoteView: failed to start on port {}", port);
        return false;
    }

    connect(m_server.get(), &QWebSocketServer::newConnection,
            this, &RemoteView::onNewConnection);

    m_frameTimer->start(1000 / m_maxFps);

    emit serverStarted(port);
    spdlog::info("RemoteView: WebSocket server started on port {}", port);
    return true;
}

void RemoteView::stop()
{
    m_frameTimer->stop();

    // Disconnect all clients
    for (auto* client : m_clients) {
        client->close();
        client->deleteLater();
    }
    m_clients.clear();

    if (m_server) {
        m_server->close();
        m_server.reset();
    }

    emit serverStopped();
    spdlog::info("RemoteView: server stopped");
}

void RemoteView::pushFrame(const QImage& frame)
{
    m_latestFrame = frame;
    m_frameDirty = true;
}

void RemoteView::pushStatus(const QString& jsonStatus)
{
    for (auto* client : m_clients) {
        if (client->isValid() && isAuthed(client)) {
            client->sendTextMessage(jsonStatus);
        }
    }
}

bool RemoteView::isRunning() const
{
    return m_server && m_server->isListening();
}

int RemoteView::clientCount() const
{
    return static_cast<int>(m_clients.size());
}

void RemoteView::setJpegQuality(int quality)
{
    m_jpegQuality = std::clamp(quality, 1, 100);
}

void RemoteView::setMaxFps(int fps)
{
    m_maxFps = std::clamp(fps, 1, 60);
    if (m_frameTimer->isActive()) {
        m_frameTimer->setInterval(1000 / m_maxFps);
    }
}

// ── Private Slots ────────────────────────────────────────────────

void RemoteView::onNewConnection()
{
    auto* socket = m_server->nextPendingConnection();
    if (!socket) return;

    connect(socket, &QWebSocket::textMessageReceived,
            this, &RemoteView::onTextMessage);
    connect(socket, &QWebSocket::disconnected,
            this, &RemoteView::onClientDisconnected);

    m_clients.push_back(socket);

    QString addr = socket->peerAddress().toString();
    emit clientConnected(addr);
    spdlog::info("RemoteView: client connected from {}", addr.toStdString());

    // With a token configured, the client sees no frames/status until it
    // authenticates (`AUTH <token>`); tell it so instead of staying silent.
    if (!m_token.isEmpty()) {
        QJsonObject hello;
        hello["auth"] = "required";
        socket->sendTextMessage(QJsonDocument(hello).toJson(QJsonDocument::Compact));
    }

    // Send the HTML viewer page on first text request
    // (Initial binary frames will be the video stream)
}

void RemoteView::onClientDisconnected()
{
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    QString addr = socket->peerAddress().toString();
    m_clients.erase(
        std::remove(m_clients.begin(), m_clients.end(), socket),
        m_clients.end());
    socket->deleteLater();

    emit clientDisconnected(addr);
    spdlog::info("RemoteView: client disconnected: {}", addr.toStdString());
}

void RemoteView::onTextMessage(const QString& message)
{
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    if (message.startsWith(QStringLiteral("AUTH "))) {
        const QString presented = message.mid(5).trimmed();
        QJsonObject reply;
        if (m_token.isEmpty() || presented == m_token) {
            socket->setProperty("ibomAuthed", true);
            reply["auth"] = "ok";
            socket->sendTextMessage(QJsonDocument(reply).toJson(QJsonDocument::Compact));
        } else {
            reply["auth"] = "denied";
            socket->sendTextMessage(QJsonDocument(reply).toJson(QJsonDocument::Compact));
            spdlog::warn("RemoteView: bad token from {} — closing",
                         socket->peerAddress().toString().toStdString());
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated);
        }
        return;
    }

    // The viewer page may be requested before auth (it's how a client gets
    // the UI), but the baked-in token only goes to authenticated clients —
    // otherwise GET_HTML would leak the very secret AUTH protects.
    if (message == "GET_HTML") {
        socket->sendTextMessage(generateHTMLViewer(isAuthed(socket)));
    } else if (message == "GET_STATUS") {
        if (!isAuthed(socket)) return;
        QJsonObject status;
        status["clients"] = clientCount();
        status["streaming"] = !m_latestFrame.isNull();
        socket->sendTextMessage(QJsonDocument(status).toJson(QJsonDocument::Compact));
    }
}

bool RemoteView::isAuthed(const QWebSocket* socket) const
{
    return m_token.isEmpty() || socket->property("ibomAuthed").toBool();
}

// ── Private ──────────────────────────────────────────────────────

void RemoteView::broadcastFrame()
{
    if (!m_frameDirty || m_latestFrame.isNull() || m_clients.empty()) return;

    QByteArray compressed = compressFrame(m_latestFrame);
    m_frameDirty = false;

    for (auto* client : m_clients) {
        if (client->isValid() && isAuthed(client)) {
            client->sendBinaryMessage(compressed);
        }
    }
}

QByteArray RemoteView::compressFrame(const QImage& frame) const
{
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);

    // Scale down if needed
    QImage scaled = frame;
    if (frame.width() > 1280) {
        scaled = frame.scaledToWidth(1280, Qt::SmoothTransformation);
    }

    scaled.save(&buffer, "JPEG", m_jpegQuality);
    return data;
}

QString RemoteView::generateHTMLViewer(bool includeToken) const
{
    QString html = R"(<!DOCTYPE html>
<html>
<head>
    <title>PCB Inspector — Remote View</title>
    <style>
        body { margin: 0; background: #1e1e2e; display: flex; justify-content: center; align-items: center; height: 100vh; }
        canvas { max-width: 100%; max-height: 100%; }
        #status { position: fixed; top: 8px; left: 8px; color: #a6adc8; font-family: monospace; font-size: 12px; }
    </style>
</head>
<body>
    <div id="status">Connecting...</div>
    <canvas id="view"></canvas>
    <script>
        const canvas = document.getElementById('view');
        const ctx = canvas.getContext('2d');
        const status = document.getElementById('status');
        // The page is opened as a local file (the server speaks WebSocket
        // only): target host/port come from the URL query, e.g.
        //   remote_view.html?host=192.168.1.42
        const params = new URLSearchParams(location.search);
        const host = params.get('host') || location.hostname || 'localhost';
        const port = params.get('port') || '__WS_PORT__';
        // Access token: taken from the URL (?token=...), falling back to the
        // value baked in when this page was generated on the host itself.
        const token = params.get('token') || '__WS_TOKEN__';
        const ws = new WebSocket('ws://' + host + ':' + port);
        ws.binaryType = 'arraybuffer';
        let frames = 0;
        setInterval(() => { status.textContent = frames + ' FPS'; frames = 0; }, 1000);

        ws.onmessage = (e) => {
            if (typeof e.data === 'string') {
                try {
                    const msg = JSON.parse(e.data);
                    if (msg.auth === 'denied') status.textContent = 'Access denied (bad token)';
                    else if (msg.auth === 'required' && !token) status.textContent = 'Token required (?token=...)';
                } catch (_) {}
                return;
            }
            const blob = new Blob([e.data], { type: 'image/jpeg' });
            const url = URL.createObjectURL(blob);
            const img = new Image();
            img.onload = () => {
                canvas.width = img.width;
                canvas.height = img.height;
                ctx.drawImage(img, 0, 0);
                URL.revokeObjectURL(url);
                frames++;
            };
            img.src = url;
        };

        ws.onopen = () => {
            status.textContent = 'Connected';
            if (token) ws.send('AUTH ' + token);
        };
        ws.onclose = () => status.textContent = 'Disconnected';
    </script>
</body>
</html>)";
    html.replace(QStringLiteral("__WS_PORT__"), QString::number(m_port));
    html.replace(QStringLiteral("__WS_TOKEN__"), includeToken ? m_token : QString());
    return html;
}

} // namespace ibom::features
