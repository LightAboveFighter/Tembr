#include "audio/AudioEngine.h"

#include <QFile>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include "miniaudio.h"

extern "C" ma_result tembr_device_discard_playback(ma_device *pDevice);   // miniaudio_impl.c

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define TEMBR_HAVE_CH_LAYOUT 1
#endif

namespace {

constexpr int kChannels = 2;   // output is always interleaved stereo float

// Interleaved stereo float FIFO. Not thread-safe by itself: the engine guards
// it with a mutex, held only for the memcpy.
class RingBuffer
{
public:
    void reset(size_t capacityFrames)
    {
        m_buf.assign(capacityFrames * kChannels, 0.0f);
        m_cap = capacityFrames;
        clear();
    }
    void clear() { m_read = m_write = m_count = 0; }
    size_t size() const { return m_count; }

    size_t write(const float *src, size_t frames)
    {
        frames = std::min(frames, m_cap - m_count);
        for (size_t done = 0; done < frames;) {
            const size_t chunk = std::min(frames - done, m_cap - m_write);
            std::copy_n(src + done * kChannels, chunk * kChannels, m_buf.data() + m_write * kChannels);
            m_write = (m_write + chunk) % m_cap;
            done += chunk;
        }
        m_count += frames;
        return frames;
    }

    size_t read(float *dst, size_t frames)
    {
        frames = std::min(frames, m_count);
        for (size_t done = 0; done < frames;) {
            const size_t chunk = std::min(frames - done, m_cap - m_read);
            std::copy_n(m_buf.data() + m_read * kChannels, chunk * kChannels, dst + done * kChannels);
            m_read = (m_read + chunk) % m_cap;
            done += chunk;
        }
        m_count -= frames;
        return frames;
    }

private:
    std::vector<float> m_buf;
    size_t m_cap = 0, m_read = 0, m_write = 0, m_count = 0;
};

// FFmpeg demuxer + decoder + resampler for one file.
struct Decoder
{
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int stream = -1;
    AVRational tb{1, 1};
    int64_t streamStart = 0;     // in `tb`
    qint64 fileDurationMs = 0;
    bool seekable = false;

    // Input format the resampler was configured for.
    int swrFormat = -1, swrRate = 0, swrChannels = 0;
    uint64_t swrMask = 0;

    ~Decoder()
    {
        swr_free(&swr);
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }

    bool open(const QString &path, QString *error)
    {
        const QByteArray name = path.contains(QLatin1String("://")) ? path.toUtf8() : QFile::encodeName(path);
        if (avformat_open_input(&fmt, name.constData(), nullptr, nullptr) < 0) {
            *error = QStringLiteral("Не удалось открыть файл");
            return false;
        }
        avformat_find_stream_info(fmt, nullptr);
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream < 0) {
            *error = QStringLiteral("В файле нет звуковой дорожки");
            return false;
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (int(i) != stream)
                fmt->streams[i]->discard = AVDISCARD_ALL;   // e.g. cover art
        }
        AVStream *st = fmt->streams[stream];
        const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            *error = QStringLiteral("Нет декодера для %1").arg(QLatin1String(avcodec_get_name(st->codecpar->codec_id)));
            return false;
        }
        codec = avcodec_alloc_context3(dec);
        if (!codec || avcodec_parameters_to_context(codec, st->codecpar) < 0) {
            *error = QStringLiteral("Ошибка инициализации декодера");
            return false;
        }
        codec->pkt_timebase = st->time_base;
        if (avcodec_open2(codec, dec, nullptr) < 0) {
            *error = QStringLiteral("Ошибка открытия декодера");
            return false;
        }
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        tb = st->time_base;
        streamStart = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
            fileDurationMs = fmt->duration / (AV_TIME_BASE / 1000);
        else if (st->duration != AV_NOPTS_VALUE)
            fileDurationMs = av_rescale_q(st->duration, tb, AVRational{1, 1000});
        seekable = fmt->pb && (fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) && fileDurationMs > 0;
        return true;
    }

    // (Re)creates the resampler when the input format changes.
    bool ensureResampler(const AVFrame *f, int outRate)
    {
#ifdef TEMBR_HAVE_CH_LAYOUT
        const int channels = f->ch_layout.nb_channels;
        const uint64_t mask = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;
#else
        const int channels = f->channels;
        const uint64_t mask = f->channel_layout;
#endif
        if (swr && f->format == swrFormat && f->sample_rate == swrRate && channels == swrChannels && mask == swrMask)
            return true;
        swr_free(&swr);

#ifdef TEMBR_HAVE_CH_LAYOUT
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout inLayout{};
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&inLayout, channels);
        else
            av_channel_layout_copy(&inLayout, &f->ch_layout);
        const int r = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, outRate, &inLayout,
                                          AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        if (r < 0)
            return false;
#else
        const int64_t inLayout = mask ? int64_t(mask) : av_get_default_channel_layout(channels);
        swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, outRate, inLayout,
                                 AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
#endif
        if (!swr || swr_init(swr) < 0) {
            swr_free(&swr);
            return false;
        }
        swrFormat = f->format;
        swrRate = f->sample_rate;
        swrChannels = channels;
        swrMask = mask;
        return true;
    }
};

} // namespace

struct AudioEngine::Impl
{
    ma_device device{};
    bool deviceOk = false;
    int rate = 48000;

    // Shared between threads.
    std::mutex mutex;                     // guards `ring` and `decoderEof` transitions
    RingBuffer ring;
    bool decoderEof = false;              // all audio of the range is in `ring`
    std::atomic<bool> reachedEnd{false};  // set by the audio thread
    std::atomic<bool> quit{false};
    std::atomic<qint64> playFrame{0};     // absolute index of the next frame to be heard
    std::atomic<qint64> seekRequest{-1};  // absolute frame, consumed by the decoder thread
    std::atomic<float> gain{1.0f};

    // Owned by the decoder thread while it runs.
    std::unique_ptr<Decoder> dec;
    std::thread thread;
    std::vector<float> pending;           // decoded, not yet in `ring`
    size_t pendingPos = 0;                // frames of `pending` already written
    std::vector<float> scratch;
    qint64 writeFrame = -1;               // absolute index of the next decoded frame, -1 = unknown
    qint64 dropUntil = 0;                 // discard decoded audio before this frame (exact seek)
    bool inputDone = false;

    // Set by load(), read-only afterwards.
    qint64 startFrame = 0;
    qint64 endFrame = 0;                  // 0 = end of file
    qint64 durationMs = 0;
    bool seekable = false;
    bool loaded = false;
    bool atEnd = false;                   // GUI thread: finished playing, play() restarts

    QTimer timer;

    static void dataCallback(ma_device *device, void *output, const void *, ma_uint32 frameCount)
    {
        auto *self = static_cast<Impl *>(device->pUserData);
        auto *out = static_cast<float *>(output);
        size_t got;
        bool end;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            got = self->ring.read(out, frameCount);
            end = got < frameCount && self->decoderEof;
        }
        const float g = self->gain.load(std::memory_order_relaxed);
        for (size_t i = 0; i < got * kChannels; ++i)
            out[i] *= g;
        std::fill(out + got * kChannels, out + size_t(frameCount) * kChannels, 0.0f);
        self->playFrame.fetch_add(qint64(got), std::memory_order_relaxed);
        if (end)
            self->reachedEnd.store(true);
    }

    qint64 msToFrame(qint64 ms) const { return ms * rate / 1000; }

    void startThread()
    {
        quit = false;
        thread = std::thread([this] { decodeLoop(); });
    }

    void stopThread()
    {
        quit = true;
        if (thread.joinable())
            thread.join();
    }

    void decodeLoop()
    {
        using namespace std::chrono_literals;
        while (!quit) {
            const qint64 seekTo = seekRequest.exchange(-1);
            if (seekTo >= 0)
                doSeek(seekTo);

            if (!flushPending()) {        // ring is full
                std::this_thread::sleep_for(5ms);
                continue;
            }
            if (inputDone) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    decoderEof = true;
                }
                std::this_thread::sleep_for(10ms);
                continue;
            }
            decodeOnePacket();
        }
    }

    // Moves `pending` into the ring; true when nothing is left.
    bool flushPending()
    {
        const size_t total = pending.size() / kChannels;
        if (pendingPos < total) {
            std::lock_guard<std::mutex> lock(mutex);
            pendingPos += ring.write(pending.data() + pendingPos * kChannels, total - pendingPos);
        }
        if (pendingPos < total)
            return false;
        pending.clear();
        pendingPos = 0;
        return true;
    }

    void decodeOnePacket()
    {
        const int r = av_read_frame(dec->fmt, dec->pkt);
        if (r == AVERROR(EAGAIN))
            return;
        if (r < 0) {                       // end of file or read error: drain
            avcodec_send_packet(dec->codec, nullptr);
            receiveFrames();
            inputDone = true;
            return;
        }
        if (dec->pkt->stream_index == dec->stream && avcodec_send_packet(dec->codec, dec->pkt) >= 0)
            receiveFrames();
        av_packet_unref(dec->pkt);
    }

    void receiveFrames()
    {
        while (avcodec_receive_frame(dec->codec, dec->frame) == 0) {
            handleFrame(dec->frame);
            av_frame_unref(dec->frame);
        }
    }

    void handleFrame(const AVFrame *f)
    {
        if (inputDone || !dec->ensureResampler(f, rate))
            return;
        if (writeFrame < 0) {
            const int64_t ts = f->best_effort_timestamp;
            writeFrame = ts != AV_NOPTS_VALUE ? av_rescale_q(ts - dec->streamStart, dec->tb, AVRational{1, rate})
                                              : dropUntil;
        }

        const int maxOut = swr_get_out_samples(dec->swr, f->nb_samples);
        if (maxOut <= 0)
            return;
        scratch.resize(size_t(maxOut) * kChannels);
        auto *outPtr = reinterpret_cast<uint8_t *>(scratch.data());
        const int n = swr_convert(dec->swr, &outPtr, maxOut, const_cast<const uint8_t **>(f->extended_data),
                                  f->nb_samples);
        if (n <= 0)
            return;

        const float *src = scratch.data();
        qint64 first = writeFrame;
        qint64 frames = n;
        writeFrame += n;

        if (first < dropUntil) {           // before the seek target / CUE start
            const qint64 skip = std::min(frames, dropUntil - first);
            src += skip * kChannels;
            frames -= skip;
            first += skip;
        }
        if (endFrame > 0 && first + frames >= endFrame) {   // CUE track end
            frames = std::max<qint64>(0, endFrame - first);
            inputDone = true;
        }
        pending.insert(pending.end(), src, src + frames * kChannels);
    }

    void doSeek(qint64 frame)
    {
        if (dec->seekable) {
            const int64_t ts = av_rescale_q(frame, AVRational{1, rate}, dec->tb) + dec->streamStart;
            av_seek_frame(dec->fmt, dec->stream, ts, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(dec->codec);
        swr_free(&dec->swr);              // drop samples buffered in the resampler
        pending.clear();
        pendingPos = 0;
        writeFrame = -1;
        dropUntil = frame;
        inputDone = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ring.clear();
            decoderEof = false;
            playFrame = frame;
        }
        reachedEnd = false;
    }
};

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = 0;                               // device's native rate
    cfg.performanceProfile = ma_performance_profile_conservative;
    cfg.dataCallback = &Impl::dataCallback;
    cfg.pUserData = d.get();
    if (ma_device_init(nullptr, &cfg, &d->device) == MA_SUCCESS) {
        d->deviceOk = true;
        d->rate = int(d->device.sampleRate);
    } else {
        m_lastError = QStringLiteral("Не удалось открыть звуковое устройство");
    }
    d->ring.reset(size_t(d->rate));                   // 1 second

    d->timer.setInterval(50);
    connect(&d->timer, &QTimer::timeout, this, &AudioEngine::poll);
    d->timer.start();
}

AudioEngine::~AudioEngine()
{
    unload();
    if (d->deviceOk)
        ma_device_uninit(&d->device);
}

bool AudioEngine::isDeviceOk() const
{
    return d->deviceOk;
}

QString AudioEngine::deviceName() const
{
    return d->deviceOk ? QString::fromUtf8(d->device.playback.name) : QString();
}

bool AudioEngine::load(const QString &path, qint64 startMs, qint64 endMs)
{
    unload();

    auto dec = std::make_unique<Decoder>();
    QString error;
    if (!dec->open(path, &error)) {
        m_lastError = error;
        emit errorOccurred(error);
        return false;
    }

    d->startFrame = d->msToFrame(startMs);
    d->endFrame = endMs > 0 ? d->msToFrame(endMs) : 0;
    d->durationMs = endMs > 0 ? endMs - startMs : std::max<qint64>(0, dec->fileDurationMs - startMs);
    d->seekable = dec->seekable;
    d->dec = std::move(dec);

    d->pending.clear();
    d->pendingPos = 0;
    d->writeFrame = -1;
    d->dropUntil = d->startFrame;
    d->inputDone = false;
    d->decoderEof = false;
    d->ring.clear();
    d->reachedEnd = false;
    d->playFrame = d->startFrame;
    d->seekRequest = startMs > 0 && d->seekable ? d->startFrame : -1;
    d->loaded = true;
    d->atEnd = false;
    d->startThread();

    emit durationChanged(d->durationMs);
    emit positionChanged(0);
    return true;
}

// Stops the device and drops what it still buffers: the next start must not
// begin with the tail of what played before (another track, or the position
// before "stop"). Pausing keeps it.
void AudioEngine::haltDevice()
{
    if (!d->deviceOk)
        return;
    if (ma_device_is_started(&d->device))
        ma_device_stop(&d->device);
    tembr_device_discard_playback(&d->device);
}

void AudioEngine::unload()
{
    haltDevice();
    d->stopThread();
    d->dec.reset();
    d->loaded = false;
    setState(State::Stopped);
}

void AudioEngine::play()
{
    if (!d->loaded || !d->deviceOk)
        return;
    if (d->atEnd) {
        d->atEnd = false;
        seek(0);
    }
    if (!ma_device_is_started(&d->device) && ma_device_start(&d->device) != MA_SUCCESS) {
        m_lastError = QStringLiteral("Не удалось запустить воспроизведение");
        emit errorOccurred(m_lastError);
        return;
    }
    setState(State::Playing);
}

void AudioEngine::pause()
{
    if (m_state != State::Playing)
        return;
    if (ma_device_is_started(&d->device))
        ma_device_stop(&d->device);
    setState(State::Paused);
}

void AudioEngine::togglePause()
{
    if (m_state == State::Playing)
        pause();
    else
        play();
}

void AudioEngine::stop()
{
    haltDevice();
    if (d->loaded) {
        seek(0);
        d->atEnd = false;
    }
    setState(State::Stopped);
    emit positionChanged(0);
}

void AudioEngine::seek(qint64 ms)
{
    if (!d->loaded || !d->seekable)
        return;
    ms = std::clamp<qint64>(ms, 0, d->durationMs);
    const qint64 frame = d->startFrame + d->msToFrame(ms);
    d->seekRequest = frame;
    d->playFrame = frame;   // show the new position right away
    d->atEnd = false;
    emit seeked(ms);
}

void AudioEngine::setVolume(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    d->gain = v * v * v;
}

float AudioEngine::volume() const
{
    return std::cbrt(d->gain.load());
}

qint64 AudioEngine::positionMs() const
{
    if (!d->loaded)
        return 0;
    const qint64 ms = (d->playFrame.load() - d->startFrame) * 1000 / d->rate;
    return std::clamp<qint64>(ms, 0, std::max<qint64>(d->durationMs, 0));
}

qint64 AudioEngine::durationMs() const
{
    return d->loaded ? d->durationMs : 0;
}

bool AudioEngine::isSeekable() const
{
    return d->loaded && d->seekable;
}

void AudioEngine::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged(s);
}

void AudioEngine::poll()
{
    if (m_state != State::Playing)
        return;
    emit positionChanged(positionMs());
    if (d->reachedEnd.exchange(false)) {
        ma_device_stop(&d->device);
        d->atEnd = true;
        setState(State::Stopped);
        emit finished();
    }
}
