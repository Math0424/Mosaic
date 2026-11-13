#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Mosaic {

class VideoDecoder {
   public:
    static constexpr int kAudioSampleRate = 48000;
    static constexpr int kAudioChannels = 2;
    static constexpr int kBytesPerSample = static_cast<int>(sizeof(int16_t));
    static constexpr int kBytesPerSecond = kAudioSampleRate * kAudioChannels * kBytesPerSample;
    // Bounds on decoded-but-unconsumed data (pump() decodes both streams).
    static constexpr int kMaxQueuedFrames = 8;
    static constexpr int kMaxQueuedAudioBytes = kBytesPerSecond * 10;

    struct Frame {
        QImage image;
        qint64 ptsMs = 0;
        // How long this frame should stay on screen (0 = unknown). Animated
        // GIF/WebP frames each carry their own delay.
        qint64 durationMs = 0;
    };

    VideoDecoder();
    ~VideoDecoder();

    bool open(const QString& path);
    void close();

    bool hasAudio() const;

    bool isOpen() const { return m_fmt != nullptr && m_videoCodec != nullptr; }

    int width() const;
    int height() const;

    double fps() const { return m_fps; }
    qint64 durationMs() const { return m_durationMs; }

    bool isAnimated();
    Frame nextFrame();
    bool seek(qint64 ms);

    QByteArray takeAudio(qint64 maxBytes);
    qint64 audioPositionMs() const;

   private:
    bool openLocked(const QString& path);  // open() body; caller holds m_mutex
    // True when this demuxer cannot seek reliably, so a rewind reopens the file.
    bool rewindByReopen() const;

    bool pump();

    bool decodeVideoPacket(AVPacket* packet);
    bool decodeAudioPacket(AVPacket* packet);

    bool receiveVideoFrames();
    bool receiveAudioFrames();

    bool convertVideoFrame(AVFrame* frame, Frame& output);

    void flushDecoders();

   private:
    AVFormatContext* m_fmt = nullptr;

    AVCodecContext* m_videoCodec = nullptr;
    AVCodecContext* m_audioCodec = nullptr;

    AVStream* m_videoStream = nullptr;
    AVStream* m_audioStream = nullptr;

    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    AVPacket* m_pkt = nullptr;

    AVFrame* m_videoFrame = nullptr;
    AVFrame* m_audioFrame = nullptr;

    SwrContext* m_swr = nullptr;
    SwsContext* m_sws = nullptr;

    AVPixelFormat m_swsPixelFormat = AV_PIX_FMT_NONE;
    int m_swsWidth = 0;
    int m_swsHeight = 0;

    QQueue<Frame> m_videoFrames;
    QByteArray m_audioBuffer;

    qint64 m_audioServedBytes = 0;
    int m_decodeErrors = 0;

    double m_fps = 0.0;
    double m_videoTimeBase = 0.0;
    qint64 m_durationMs = 0;
    bool m_videoFlushed = false;
    bool m_animationChecked = false;
    bool m_isAnimated = false;

    QString m_path;  // for rewinds that reopen the input
    // Duration measured from packet timings for containers that report none
    // (animated WebP); kept across close() so loop rewinds don't re-measure.
    QString m_measuredPath;
    qint64 m_measuredDurationMs = 0;

    mutable QMutex m_mutex;
};

}  // namespace Mosaic