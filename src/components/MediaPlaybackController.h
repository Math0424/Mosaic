#pragma once

#include "../media/VideoDecoder.h"

#include <QElapsedTimer>
#include <QIODevice>
#include <QObject>
#include <QString>

class QAudioSink;

namespace Mosaic {

// Pull-style audio device: PCM served straight from the decoder's
// resampled buffer (thread-safe).
class MosaicAudioDevice : public QIODevice {
   public:
    explicit MosaicAudioDevice(VideoDecoder* decoder) : m_decoder(decoder) {}

   protected:
    qint64 bytesAvailable() const override { return QIODevice::bytesAvailable() + 4096; }

    qint64 readData(char* data, qint64 maxSize) override;

    qint64 writeData(const char* data, qint64 len) override;

   private:
    VideoDecoder* m_decoder = nullptr;
};

// Owns the viewer's playback pipeline: video decoder, audio sink and
// frame-pacing timer. Driven by ViewerWindow.
class MediaPlaybackController : public QObject {
    Q_OBJECT

   public:
    explicit MediaPlaybackController(QObject* parent = nullptr);
    ~MediaPlaybackController() override;

    void start(VideoDecoder* decoder);

    void stop();

    bool isActive() const { return m_decoder && m_decoder->isOpen(); }
    bool isPlaying() const { return m_playing; }

    VideoDecoder* decoder() const { return m_decoder; }

    qint64 durationMs() const { return m_decoder ? m_decoder->durationMs() : 0; }

    VideoDecoder::Frame nextFrame();

    bool togglePlayPause();

    void pause();

    void resume();

    VideoDecoder::Frame seekTo(qint64 ms);

    VideoDecoder::Frame stepFrames(int frameCount, qint64 fromMs, qint64 maxMs);

    void setVolume(int percent);

   signals:
    void frameReady(const Mosaic::VideoDecoder::Frame& frame);

   private:
    VideoDecoder* m_decoder = nullptr;
    MosaicAudioDevice* m_audioDevice = nullptr;
    QAudioSink* m_audioSink = nullptr;
    QTimer* m_timer = nullptr;
    bool m_playing = false;
};

}  // namespace Mosaic
