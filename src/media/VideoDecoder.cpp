#include "VideoDecoder.h"

#include "../core/Shutdown.h"

#include <QDebug>

#include <QMutexLocker>

#include <cstring>


extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Mosaic {

namespace {

QString ffError(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

// Lets FFmpeg abandon blocking I/O when the app is closing.
int interruptCallback(void*) { return Shutdown::requested() ? 1 : 0; }

// Corrupt packets are skipped, not treated as end of stream; only the
// first few are logged so a damaged file can't flood the log.
constexpr int kMaxLoggedDecodeErrors = 5;

// Upper bound on packets read to measure a missing duration.
constexpr int kMaxDurationScanPackets = 20000;

}  // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const QString& path) {
    QMutexLocker lock(&m_mutex);
    return openLocked(path);
}

bool VideoDecoder::rewindByReopen() const {
    // FFmpeg's animated WebP demuxer has no working seek: av_seek_frame
    // "succeeds" but leaves it reading garbage ("Skipping chunk: [0][0][0][0]")
    // and the animation never restarts.
    return m_fmt && m_fmt->iformat && m_fmt->iformat->name &&
           std::strcmp(m_fmt->iformat->name, "webp_anim") == 0;
}

bool VideoDecoder::openLocked(const QString& path) {
    close();
    m_path = path;
    m_videoFlushed = false;
    m_decodeErrors = 0;

    // Open container

    m_fmt = avformat_alloc_context();
    if (!m_fmt)
        return false;
    m_fmt->interrupt_callback.callback = interruptCallback;
    m_fmt->interrupt_callback.opaque = nullptr;
    // Frees and nulls m_fmt on failure.
    if (avformat_open_input(&m_fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        qWarning().noquote() << "VideoDecoder: cannot open container:" << path;
        return false;
    }

    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        qWarning().noquote() << "VideoDecoder: cannot find stream info:" << path;
        close();
        return false;
    }

    // Video stream

    m_videoStreamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (m_videoStreamIndex < 0) {
        qWarning().noquote() << "VideoDecoder: no video stream found:" << path;
        close();
        return false;
    }

    m_videoStream = m_fmt->streams[m_videoStreamIndex];

    const AVCodec* videoCodec = avcodec_find_decoder(m_videoStream->codecpar->codec_id);

    if (!videoCodec) {
        qWarning().noquote() << "VideoDecoder: no decoder for codec"
                             << m_videoStream->codecpar->codec_id << "--" << path;
        close();
        return false;
    }

    m_videoCodec = avcodec_alloc_context3(videoCodec);

    if (!m_videoCodec) {
        close();
        return false;
    }

    if (avcodec_parameters_to_context(m_videoCodec, m_videoStream->codecpar) < 0) {
        close();
        return false;
    }

    if (avcodec_open2(m_videoCodec, videoCodec, nullptr) < 0) {
        close();
        return false;
    }

    // Frame rate / duration

    m_videoTimeBase = av_q2d(m_videoStream->time_base);

    if (m_fmt->duration != AV_NOPTS_VALUE && m_fmt->duration > 0) {
        m_durationMs = m_fmt->duration / 1000;
    } else if (m_videoStream->duration != AV_NOPTS_VALUE && m_videoStream->duration > 0) {
        m_durationMs = static_cast<qint64>(m_videoStream->duration * m_videoTimeBase * 1000.0);
    }

    // No duration reported: fall back to container bitrate.
    if (m_durationMs <= 0 && m_fmt->bit_rate > 0) {
        const qint64 fileSize = avio_size(m_fmt->pb);
        if (fileSize > 0)
            m_durationMs = fileSize * 8 / m_fmt->bit_rate;
    }

    const AVRational fr = av_guess_frame_rate(m_fmt, m_videoStream, nullptr);
    double fps = fr.den > 0 ? static_cast<double>(fr.num) / fr.den : 0.0;

    const AVRational avgFr = m_videoStream->avg_frame_rate;
    if (avgFr.den > 0 && avgFr.num > 0) {
        const double avgFps = static_cast<double>(avgFr.num) / avgFr.den;
        if (avgFps > 0.0 && (fps <= 0.0 || avgFps < fps))
            fps = avgFps;
    }

    if ((fps <= 0.0 || fps > 240.0) && m_videoStream->nb_frames > 1 && m_durationMs > 0) {
        fps = m_videoStream->nb_frames * 1000.0 / static_cast<double>(m_durationMs);
    }

    if (fps <= 0.0 || fps > 240.0)
        fps = 30.0;

    m_fps = fps;

    // Container reports no duration (FFmpeg's animated WebP demuxer): add up
    // the video packet durations once, then start over from the top.
    if (m_durationMs <= 0 && path == m_measuredPath) {
        m_durationMs = m_measuredDurationMs;
    } else if (m_durationMs <= 0) {
        m_measuredPath = path;
        m_measuredDurationMs = 0;
        AVPacket* packet = av_packet_alloc();
        if (packet) {
            int64_t total = 0;
            for (int i = 0; i < kMaxDurationScanPackets && av_read_frame(m_fmt, packet) >= 0; ++i) {
                if (packet->stream_index == m_videoStreamIndex && packet->duration > 0)
                    total += packet->duration;
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
            if (total > 0) {
                m_measuredDurationMs =
                    av_rescale_q(total, m_videoStream->time_base, AVRational{1, 1000});
            }
        }
        // The scan consumed the stream; reopen (some demuxers can't seek back).
        return openLocked(QString(path));
    }

    // Audio stream

    m_audioStreamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_audioStreamIndex >= 0) {
        m_audioStream = m_fmt->streams[m_audioStreamIndex];

        const AVCodec* audioCodec = avcodec_find_decoder(m_audioStream->codecpar->codec_id);

        if (audioCodec) {
            m_audioCodec = avcodec_alloc_context3(audioCodec);

            if (m_audioCodec &&
                avcodec_parameters_to_context(m_audioCodec, m_audioStream->codecpar) >= 0 &&
                avcodec_open2(m_audioCodec, audioCodec, nullptr) >= 0) {
                m_audioFrame = av_frame_alloc();

                if (!m_audioFrame) {
                    avcodec_free_context(&m_audioCodec);
                    m_audioCodec = nullptr;
                }
            }
        }
    }

    // Frames / packet

    m_videoFrame = av_frame_alloc();
    m_pkt = av_packet_alloc();

    if (!m_videoFrame || !m_pkt) {
        close();
        return false;
    }

    // Audio resampler

    if (m_audioCodec) {
        m_swr = swr_alloc();

        if (!m_swr) {
            close();
            return false;
        }

        if (av_opt_set_int(m_swr, "in_sample_fmt", m_audioCodec->sample_fmt, 0) < 0 ||
            av_opt_set_int(m_swr, "in_sample_rate", m_audioCodec->sample_rate, 0) < 0 ||
            av_opt_set_chlayout(m_swr, "in_chlayout", &m_audioCodec->ch_layout, 0) < 0 ||
            av_opt_set_int(m_swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0) < 0 ||
            av_opt_set_int(m_swr, "out_sample_rate", kAudioSampleRate, 0) < 0) {
            close();
            return false;
        }

        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

        if (av_opt_set_chlayout(m_swr, "out_chlayout", &stereo, 0) < 0 || swr_init(m_swr) < 0) {
            close();
            return false;
        }
    }

    return true;
}

void VideoDecoder::close() {
    // Caller may already hold the mutex.
    // Don't lock here.

    m_videoFrames.clear();
    m_audioBuffer.clear();

    m_audioServedBytes = 0;

    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }

    m_swsPixelFormat = AV_PIX_FMT_NONE;
    m_swsWidth = 0;
    m_swsHeight = 0;

    if (m_swr)
        swr_free(&m_swr);

    if (m_videoFrame)
        av_frame_free(&m_videoFrame);

    if (m_audioFrame)
        av_frame_free(&m_audioFrame);

    if (m_pkt)
        av_packet_free(&m_pkt);

    if (m_videoCodec)
        avcodec_free_context(&m_videoCodec);

    if (m_audioCodec)
        avcodec_free_context(&m_audioCodec);

    if (m_fmt)
        avformat_close_input(&m_fmt);

    m_videoStream = nullptr;
    m_audioStream = nullptr;

    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;

    m_fps = 0.0;
    m_videoTimeBase = 0.0;
    m_durationMs = 0;
    m_animationChecked = false;
    m_isAnimated = false;
}

bool VideoDecoder::isAnimated() {
    QMutexLocker lock(&m_mutex);

    if (!m_fmt || !m_videoCodec)
        return false;
    if (m_animationChecked)
        return m_isAnimated;

    m_animationChecked = true;
    if (m_videoStream->nb_frames > 1) {
        m_isAnimated = true;
        return true;
    }

    while (m_videoFrames.size() < 2) {
        if (!pump())
            break;
    }

    m_isAnimated = m_videoFrames.size() > 1;
    return m_isAnimated;
}

bool VideoDecoder::hasAudio() const { return m_audioCodec != nullptr; }

int VideoDecoder::width() const {
    QMutexLocker lock(&m_mutex);
    return m_videoCodec ? m_videoCodec->width : 0;
}

int VideoDecoder::height() const {
    QMutexLocker lock(&m_mutex);
    return m_videoCodec ? m_videoCodec->height : 0;
}

// Video frame conversion

bool VideoDecoder::convertVideoFrame(AVFrame* frame, Frame& output) {
    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);

    if (format == AV_PIX_FMT_NONE)
        return false;

    const int width = frame->width;
    const int height = frame->height;

    if (width <= 0 || height <= 0)
        return false;

    // Keep transparency (animated WebP/GIF/PNG, VP9 alpha); RGB24 would turn
    // transparent areas black.
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    const bool hasAlpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
    const AVPixelFormat dstFormat = hasAlpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    const QImage::Format qtFormat = hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    const int bytesPerPixel = hasAlpha ? 4 : 3;

    // Recreate SwsContext if the actual decoded format changes (the output
    // format follows the input format, so it is covered too).
    if (!m_sws || m_swsPixelFormat != format || m_swsWidth != width || m_swsHeight != height) {
        if (m_sws)
            sws_freeContext(m_sws);

        m_sws = sws_getContext(width, height, format, width, height, dstFormat, SWS_BILINEAR,
                               nullptr, nullptr, nullptr);

        if (!m_sws) {
            const char* name = av_get_pix_fmt_name(format);
            qWarning("sws_getContext failed for %s %dx%d", name ? name : "?", width, height);
            return false;
        }

        m_swsPixelFormat = format;
        m_swsWidth = width;
        m_swsHeight = height;
    }

    // swscale's SIMD paths may write up to a vector width past the end of a
    // row, so a QImage's own unpadded rows are not a safe destination (heap
    // corruption on the last row). Convert into 64-byte aligned rows with
    // slack at the end, then wrap that buffer as the QImage without copying.
    constexpr int kAlign = 64;
    const int linesize = (width * bytesPerPixel + kAlign - 1) & ~(kAlign - 1);
    const size_t bytes = static_cast<size_t>(linesize) * static_cast<size_t>(height) + kAlign;
    auto* buffer = static_cast<uchar*>(av_malloc(bytes));
    if (!buffer)
        return false;

    uint8_t* dst[4] = {buffer, nullptr, nullptr, nullptr};
    const int dstLinesize[4] = {linesize, 0, 0, 0};
    if (sws_scale(m_sws, frame->data, frame->linesize, 0, height, dst, dstLinesize) < 0) {
        av_free(buffer);
        return false;
    }

    QImage image(buffer, width, height, linesize, qtFormat,
                 [](void* data) { av_free(data); }, buffer);
    if (image.isNull()) {
        av_free(buffer);
        return false;
    }
    output.image = std::move(image);

    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        output.ptsMs = av_rescale_q(frame->best_effort_timestamp, m_videoStream->time_base,
                                    AVRational{1, 1000});
    }
    if (frame->duration > 0) {
        output.durationMs =
            av_rescale_q(frame->duration, m_videoStream->time_base, AVRational{1, 1000});
    }

    return true;
}

// Decode video

bool VideoDecoder::receiveVideoFrames() {
    while (true) {
        const int ret = avcodec_receive_frame(m_videoCodec, m_videoFrame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }

        if (ret < 0) {
            if (ret == AVERROR_EXIT || ret == AVERROR(ENOMEM))
                return false;
            if (++m_decodeErrors <= kMaxLoggedDecodeErrors)
                qWarning("Video decode error (skipped): %s", qPrintable(ffError(ret)));
            return true;
        }

        Frame frame;

        if (convertVideoFrame(m_videoFrame, frame)) {
            m_videoFrames.enqueue(std::move(frame));
            // takeAudio() decodes video it does not consume; if the video
            // side stalls, keep only the newest frames (stale ones would be
            // skipped for A/V sync anyway).
            while (m_videoFrames.size() > kMaxQueuedFrames)
                m_videoFrames.dequeue();
        }

        av_frame_unref(m_videoFrame);
    }
}

bool VideoDecoder::decodeVideoPacket(AVPacket* packet) {
    const int ret = avcodec_send_packet(m_videoCodec, packet);

    if (ret < 0) {
        if (ret == AVERROR_EXIT || ret == AVERROR(ENOMEM))
            return false;
        if (++m_decodeErrors <= kMaxLoggedDecodeErrors)
            qWarning("avcodec_send_packet(video, skipped): %s", qPrintable(ffError(ret)));
        return true;
    }

    return receiveVideoFrames();
}

// Decode audio

bool VideoDecoder::receiveAudioFrames() {
    while (true) {
        const int ret = avcodec_receive_frame(m_audioCodec, m_audioFrame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }

        if (ret < 0) {
            if (ret == AVERROR_EXIT || ret == AVERROR(ENOMEM))
                return false;
            if (++m_decodeErrors <= kMaxLoggedDecodeErrors)
                qWarning("Audio decode error (skipped): %s", qPrintable(ffError(ret)));
            return true;
        }

        // swr_get_out_samples() undershoots for WebMs with preroll metadata;
        // size the buffer from the resampler's actual pending samples.
        const int64_t pending = swr_get_delay(m_swr, m_audioCodec->sample_rate);
        const int maxSamples =
            static_cast<int>(av_rescale_rnd(pending + m_audioFrame->nb_samples, kAudioSampleRate,
                                            m_audioCodec->sample_rate, AV_ROUND_UP) +
                             8);

        if (maxSamples > 0) {
            QByteArray pcm(maxSamples * kAudioChannels * kBytesPerSample, Qt::Uninitialized);

            uint8_t* out = reinterpret_cast<uint8_t*>(pcm.data());

            const int converted = swr_convert(
                m_swr, &out, maxSamples, const_cast<const uint8_t**>(m_audioFrame->extended_data),
                m_audioFrame->nb_samples);

            if (converted > 0) {
                pcm.truncate(converted * kAudioChannels * kBytesPerSample);

                m_audioBuffer.append(pcm);
                // nextFrame() decodes audio it does not consume; when nobody
                // drains it, drop the oldest PCM but count it as served so
                // audioPositionMs() keeps following the stream.
                const qint64 excess = m_audioBuffer.size() - kMaxQueuedAudioBytes;
                if (excess > 0) {
                    m_audioBuffer.remove(0, static_cast<int>(excess));
                    m_audioServedBytes += excess;
                }
            }
        }

        av_frame_unref(m_audioFrame);
    }
}

bool VideoDecoder::decodeAudioPacket(AVPacket* packet) {
    if (!m_audioCodec || !m_swr)
        return true;

    const int ret = avcodec_send_packet(m_audioCodec, packet);

    if (ret < 0) {
        if (ret == AVERROR_EXIT || ret == AVERROR(ENOMEM))
            return false;
        if (++m_decodeErrors <= kMaxLoggedDecodeErrors)
            qWarning("avcodec_send_packet(audio, skipped): %s", qPrintable(ffError(ret)));
        return true;
    }

    return receiveAudioFrames();
}

// Demux

bool VideoDecoder::pump() {
    // Demux/decode until a video frame is ready; audio alone is not progress
    // for callers waiting on video.
    while (true) {
        const int ret = av_read_frame(m_fmt, m_pkt);

        if (ret < 0)
            return false;

        bool ok = true;

        if (m_pkt->stream_index == m_videoStreamIndex) {
            ok = decodeVideoPacket(m_pkt);
        } else if (m_pkt->stream_index == m_audioStreamIndex) {
            ok = decodeAudioPacket(m_pkt);
        }

        av_packet_unref(m_pkt);
        if (!ok)
            return false;
        if (!m_videoFrames.isEmpty())
            return true;
    }
}

// Video

VideoDecoder::Frame VideoDecoder::nextFrame() {
    QMutexLocker lock(&m_mutex);

    if (!m_fmt || !m_videoCodec)
        return {};

    if (!m_videoFrames.isEmpty())
        return m_videoFrames.dequeue();
    while (pump()) {
        if (!m_videoFrames.isEmpty())
            return m_videoFrames.dequeue();
    }

    if (!m_videoFlushed) {
        m_videoFlushed = true;
        const int ret = avcodec_send_packet(m_videoCodec, nullptr);
        if (ret >= 0)
            receiveVideoFrames();
    }

    if (!m_videoFrames.isEmpty())
        return m_videoFrames.dequeue();
    return {};
}

// Seeking

bool VideoDecoder::seek(qint64 ms) {
    QMutexLocker lock(&m_mutex);

    if (!m_fmt || !m_videoCodec || !m_videoStream)
        return false;

    ms = qMax<qint64>(0, ms);

    // Rewinding a demuxer that can't seek: reopen from the start instead.
    // Any other target on such a demuxer also restarts (frames before it are
    // skipped by the caller).
    if (rewindByReopen() && !m_path.isEmpty())
        return openLocked(QString(m_path));

    const int64_t timestamp = av_rescale_q(ms, AVRational{1, 1000}, m_videoStream->time_base);

    if (av_seek_frame(m_fmt, m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD) < 0 &&
        av_seek_frame(m_fmt, m_videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        // Last resort for a rewind: start over from the file.
        if (ms == 0 && !m_path.isEmpty())
            return openLocked(QString(m_path));
        return false;
    }

    avcodec_flush_buffers(m_videoCodec);
    if (m_audioCodec)
        avcodec_flush_buffers(m_audioCodec);

    if (m_swr) {
        swr_close(m_swr);

        if (swr_init(m_swr) < 0)
            return false;
    }

    m_videoFrames.clear();
    m_audioBuffer.clear();
    m_audioServedBytes = 0;

    m_videoFlushed = false;

    av_packet_unref(m_pkt);
    av_frame_unref(m_videoFrame);

    if (m_audioFrame)
        av_frame_unref(m_audioFrame);

    return true;
}

// Audio

QByteArray VideoDecoder::takeAudio(qint64 maxBytes) {
    if (maxBytes <= 0)
        return {};

    QMutexLocker lock(&m_mutex);

    if (!m_audioCodec || !m_swr)
        return {};

    while (m_audioBuffer.size() < maxBytes) {
        if (!pump())
            break;
    }

    if (m_audioBuffer.isEmpty())
        return {};

    const qint64 count = qMin(maxBytes, static_cast<qint64>(m_audioBuffer.size()));

    QByteArray output = m_audioBuffer.left(static_cast<int>(count));

    m_audioBuffer.remove(0, static_cast<int>(count));

    m_audioServedBytes += count;

    return output;
}

qint64 VideoDecoder::audioPositionMs() const {
    QMutexLocker lock(&m_mutex);

    return (m_audioServedBytes * 1000) / kBytesPerSecond;
}

}  // namespace Mosaic