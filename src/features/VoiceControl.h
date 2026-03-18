#pragma once

#include <QObject>
#include <QAudioSource>
#include <QMediaDevices>
#include <QIODevice>
#include <QString>
#include <memory>
#include <map>
#include <functional>

namespace ibom::features {

/// Voice control for hands-free operation during assembly/inspection
class VoiceControl : public QObject {
    Q_OBJECT

public:
    explicit VoiceControl(QObject* parent = nullptr);
    ~VoiceControl() override;

    /// Register a voice command with a callback
    void registerCommand(const QString& keyword, std::function<void()> callback);

    /// Start listening
    void startListening();

    /// Stop listening
    void stopListening();

    bool isListening() const { return m_listening; }

    /// Get available audio input devices
    static QStringList availableDevices();

    /// Set audio input device
    void setDevice(const QString& deviceName);

signals:
    void commandRecognized(const QString& command);
    void listeningStarted();
    void listeningStopped();
    void audioLevelChanged(float level);
    void error(const QString& message);

private slots:
    void processAudioData();

private:
    void initializeAudio();
    float calculateRMS(const QByteArray& data) const;

    // Audio capture
    std::unique_ptr<QAudioSource> m_audioSource;
    QIODevice*                    m_audioDevice = nullptr;

    // Command map
    std::map<QString, std::function<void()>> m_commands;

    // State
    bool    m_listening  = false;
    float   m_threshold  = 0.02f;   // Voice activity detection threshold
    QString m_deviceName;

    // Buffer for audio processing
    QByteArray m_audioBuffer;
    int        m_sampleRate = 16000;
};

} // namespace ibom::features
