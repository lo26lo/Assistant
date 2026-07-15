#pragma once

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QImage>
#include <QTimer>
#include <memory>
#include <vector>

namespace ibom::features {

/// Stream camera + overlay to remote browser via WebSocket (MJPEG-style)
class RemoteView : public QObject {
    Q_OBJECT

public:
    explicit RemoteView(QObject* parent = nullptr);
    ~RemoteView() override;

    /// Start WebSocket server
    bool start(quint16 port = 8080);

    /// Stop server
    void stop();

    /// Push a new frame to all connected clients
    void pushFrame(const QImage& frame);

    /// Push inspection status as JSON
    void pushStatus(const QString& jsonStatus);

    bool isRunning() const;
    int  clientCount() const;
    quint16 port() const { return m_port; }

    /// Set JPEG quality for streaming (1-100)
    void setJpegQuality(int quality);

    /// Set max FPS for streaming (throttle)
    void setMaxFps(int fps);

    /// Access token clients must present (`AUTH <token>` as their first text
    /// message) before receiving frames/status. Empty = open access (LAN
    /// trust). Set BEFORE start(); embedded in the generated viewer URL hint.
    void setToken(const QString& token) { m_token = token; }
    QString token() const { return m_token; }

    /// Self-contained HTML viewer page (connects back over WebSocket).
    /// Written to disk by the application so it can be opened in a browser.
    /// includeToken embeds the access token in the page (only safe for the
    /// copy written to the host's own disk — never over the wire unauthed).
    QString generateHTMLViewer(bool includeToken = true) const;

signals:
    void clientConnected(const QString& address);
    void clientDisconnected(const QString& address);
    void serverStarted(quint16 port);
    void serverStopped();
    void error(const QString& message);

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onTextMessage(const QString& message);

private:
    void broadcastFrame();
    QByteArray compressFrame(const QImage& frame) const;
    bool isAuthed(const QWebSocket* socket) const;

    std::unique_ptr<QWebSocketServer> m_server;
    std::vector<QWebSocket*>          m_clients;

    QImage   m_latestFrame;
    QTimer*  m_frameTimer    = nullptr;
    quint16  m_port          = 8080;
    int      m_jpegQuality   = 70;
    int      m_maxFps        = 15;
    bool     m_frameDirty    = false;
    QString  m_token;
};

} // namespace ibom::features
