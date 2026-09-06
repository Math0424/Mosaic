#include "MediaPlaybackController.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QDebug>
#include <QTimer>

#include <cstring>

namespace Mosaic {

qint64 MosaicAudioDevice::readData(char* data, qint64 maxSize) {
    if (!m_decoder || maxSize <= 0)
        return 0;

    const QByteArray pcm = m_decoder->takeAudio(maxSize);
    if (!pcm.isEmpty()) {
        std::memcpy(data, pcm.constData(), static_cast<size_t>(pcm.size()));
        return pcm.size();
    }

    std::memset(data, 0, static_cast<size_t>(maxSize));
    return maxSize;
}

qint64 MosaicAudioDevice::writeData(const char* data, qint64 len) {
    Q_UNUSED(data);
    Q_UNUSED(len);
    return 0;
}

// controller

MediaPlaybackController::MediaPlaybackController(QObject* parent) : QObject(parent) {
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (!m_playing || !m_decoder)
            return;
        VideoDecoder::Frame frame = m_decoder->nextFrame();

        // Keep video in sync with the audio clock: drop stale frames.
        if (m_decoder->hasAudio()) {
            const qint64 audioPos = m_decoder->audioPositionMs();
            while (!frame.image.isNull() && frame.ptsMs >= 0 && frame.ptsMs + 60 < audioPos) {
                VideoDecoder::Frame next = m_decoder->nextFrame();
                if (next.image.isNull())
                    break;
                frame = std::move(next);
            }
        }

        emit frameReady(frame);
    });
}

MediaPlaybackController::~MediaPlaybackController() { stop(); }

void MediaPlaybackController::start(VideoDecoder* decoder) {
    stop();
    if (!decoder || !decoder->isOpen()) {
        qWarning().noquote() << "Playback: decoder failed to open media";
        delete decoder;
        return;
    }

    m_decoder = decoder;
    qDebug().noquote() << "Playback: started" << m_decoder->fps()
                       << "fps, audio:" << m_decoder->hasAudio();

    if (m_decoder->hasAudio()) {
        m_audioDevice = new MosaicAudioDevice(m_decoder);
        if (!m_audioDevice->open(QIODevice::ReadOnly)) {
            qWarning().noquote() << "Playback: audio device open failed - video only";
            delete m_audioDevice;
            m_audioDevice = nullptr;
        } else {
            QAudioFormat format;
            format.setSampleRate(VideoDecoder::kAudioSampleRate);
            format.setChannelCount(VideoDecoder::kAudioChannels);
            format.setSampleFormat(QAudioFormat::Int16);
            m_audioSink = new QAudioSink(format, this);
            m_audioSink->setVolume(0.8);
            m_audioSink->start(m_audioDevice);
        }
    }

    const double fps = m_decoder->fps() > 1.0 ? m_decoder->fps() : 25.0;
    const int interval = qMax(1, qRound(1000.0 / fps));
    m_timer->setInterval(interval);
    m_playing = true;
    m_timer->start();
}

void MediaPlaybackController::stop() {
    m_timer->stop();
    m_playing = false;

    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
    if (m_audioDevice) {
        m_audioDevice->close();
        delete m_audioDevice;
        m_audioDevice = nullptr;
    }
    if (m_decoder) {
        m_decoder->close();
        delete m_decoder;
        m_decoder = nullptr;
    }
}

VideoDecoder::Frame MediaPlaybackController::nextFrame() {
    if (!m_decoder || !m_decoder->isOpen())
        return {};
    return m_decoder->nextFrame();
}

bool MediaPlaybackController::togglePlayPause() {
    if (!isActive())
        return m_playing;

    if (m_playing) {
        pause();
    } else {
        m_playing = true;
        if (m_audioSink)
            m_audioSink->resume();
        m_timer->start();
    }
    return m_playing;
}

void MediaPlaybackController::pause() {
    m_playing = false;
    m_timer->stop();
    if (m_audioSink)
        m_audioSink->suspend();
}

void MediaPlaybackController::resume() {
    m_playing = true;
    if (m_audioSink)
        m_audioSink->resume();
    m_timer->start();
}

VideoDecoder::Frame MediaPlaybackController::seekTo(qint64 ms) {
    VideoDecoder::Frame frame;
    if (!m_decoder || !m_decoder->isOpen())
        return frame;

    ms = qBound<qint64>(0, ms, qMax<qint64>(0, m_decoder->durationMs()));
    if (!m_decoder->seek(ms))
        return frame;

    // Skip frames still before the target (keyframe gap).
    while (true) {
        frame = m_decoder->nextFrame();
        if (frame.image.isNull())
            break;
        if (frame.ptsMs < 0 || frame.ptsMs >= ms)
            break;
    }
    return frame;
}

VideoDecoder::Frame MediaPlaybackController::stepFrames(int frameCount, qint64 fromMs,
                                                        qint64 maxMs) {
    if (!m_decoder || !m_decoder->isOpen())
        return {};

    const bool wasPlaying = m_playing;
    if (wasPlaying)
        pause();

    const double fps = m_decoder->fps() > 1.0 ? m_decoder->fps() : 25.0;
    const qint64 stepMs = qMax<qint64>(1, qRound(1000.0 / fps));
    const qint64 target = qBound<qint64>(0, fromMs + stepMs * frameCount, qMax<qint64>(0, maxMs));

    VideoDecoder::Frame frame = seekTo(target);

    if (wasPlaying)
        resume();
    return frame;
}

void MediaPlaybackController::setVolume(int percent) {
    if (m_audioSink)
        m_audioSink->setVolume(percent / 100.0);
}

}  // namespace Mosaic
