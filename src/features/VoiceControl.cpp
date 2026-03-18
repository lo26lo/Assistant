#include "VoiceControl.h"

#include <QAudioFormat>
#include <QMediaDevices>
#include <spdlog/spdlog.h>
#include <cmath>

namespace ibom::features {

VoiceControl::VoiceControl(QObject* parent)
    : QObject(parent)
{
}

VoiceControl::~VoiceControl()
{
    stopListening();
}

void VoiceControl::registerCommand(const QString& keyword, std::function<void()> callback)
{
    m_commands[keyword.toLower()] = std::move(callback);
    spdlog::info("VoiceControl: registered command '{}'", keyword.toStdString());
}

void VoiceControl::startListening()
{
    if (m_listening) return;

    initializeAudio();
    if (!m_audioSource) {
        emit error(tr("Failed to initialize audio capture"));
        return;
    }

    m_audioDevice = m_audioSource->start();
    if (!m_audioDevice) {
        emit error(tr("Failed to start audio capture"));
        return;
    }

    connect(m_audioDevice, &QIODevice::readyRead, this, &VoiceControl::processAudioData);

    m_listening = true;
    emit listeningStarted();
    spdlog::info("VoiceControl: started listening");
}

void VoiceControl::stopListening()
{
    if (!m_listening) return;

    if (m_audioSource) {
        m_audioSource->stop();
    }
    m_audioDevice = nullptr;
    m_listening = false;

    emit listeningStopped();
    spdlog::info("VoiceControl: stopped listening");
}

QStringList VoiceControl::availableDevices()
{
    QStringList devices;
    for (const auto& device : QMediaDevices::audioInputs()) {
        devices << device.description();
    }
    return devices;
}

void VoiceControl::setDevice(const QString& deviceName)
{
    m_deviceName = deviceName;
    if (m_listening) {
        stopListening();
        startListening();
    }
}

void VoiceControl::processAudioData()
{
    if (!m_audioDevice) return;

    QByteArray data = m_audioDevice->readAll();
    if (data.isEmpty()) return;

    // Calculate audio level
    float rms = calculateRMS(data);
    emit audioLevelChanged(rms);

    // Voice activity detection
    if (rms < m_threshold) return;

    // Accumulate audio buffer
    m_audioBuffer.append(data);

    // Process when we have ~1 second of audio
    int bytesPerSecond = m_sampleRate * 2; // 16-bit mono
    if (m_audioBuffer.size() >= bytesPerSecond) {
        // TODO: Send to speech recognition engine (Whisper.cpp, Vosk, etc.)
        // For now, this is a placeholder for the recognition pipeline.
        // In production, you would:
        // 1. Convert audio buffer to float
        // 2. Run through Whisper.cpp or similar STT model
        // 3. Match recognized text against commands

        spdlog::debug("VoiceControl: processing {} bytes of audio", m_audioBuffer.size());

        // Placeholder: simple keyword matching would go here
        // QString recognized = runSTT(m_audioBuffer);
        // for (auto& [keyword, callback] : m_commands) {
        //     if (recognized.contains(keyword)) {
        //         callback();
        //         emit commandRecognized(keyword);
        //     }
        // }

        m_audioBuffer.clear();
    }
}

void VoiceControl::initializeAudio()
{
    QAudioFormat format;
    format.setSampleRate(m_sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device;
    if (!m_deviceName.isEmpty()) {
        for (const auto& d : QMediaDevices::audioInputs()) {
            if (d.description() == m_deviceName) {
                device = d;
                break;
            }
        }
    }

    if (device.isNull()) {
        device = QMediaDevices::defaultAudioInput();
    }

    if (!device.isFormatSupported(format)) {
        spdlog::warn("VoiceControl: requested format not supported, using nearest");
    }

    m_audioSource = std::make_unique<QAudioSource>(device, format, this);
    m_audioSource->setBufferSize(m_sampleRate * 2); // 1 second buffer
}

float VoiceControl::calculateRMS(const QByteArray& data) const
{
    if (data.size() < 2) return 0.0f;

    const auto* samples = reinterpret_cast<const int16_t*>(data.constData());
    int count = data.size() / 2;

    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        double s = samples[i] / 32768.0;
        sum += s * s;
    }

    return static_cast<float>(std::sqrt(sum / count));
}

} // namespace ibom::features
