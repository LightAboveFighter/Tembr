#include "audio/AudioEngine.h"

#include "core/MediaOpen.h"

#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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

using namespace std::chrono_literals;

constexpr int kChannels = 2;   // output is always interleaved stereo float

// Interleaved stereo float FIFO. Not thread-safe by itself: the session
// guards it with a mutex, held only for the memcpy.
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
    MediaOpen::Interrupt interrupt;   // closed over by `fmt`: destroyed after it (see ~Decoder)
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
        if (!MediaOpen::isAllowed(path)) {
            *error = QStringLiteral("Этот вид адреса не поддерживается");
            return false;
        }
        if (MediaOpen::open(&fmt, path, &interrupt) < 0) {
            *error = QStringLiteral("Не удалось открыть файл");
            return false;
        }
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
        if (!pkt || !frame) {
            *error = QStringLiteral("Недостаточно памяти");
            return false;
        }
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

// One load(): the file, its decoder thread and the audio decoded so far. The
// thread owns a reference, so a session the GUI has let go of lives on until
// its thread notices `quit` - possibly after a read that FFmpeg cannot
// interrupt (a local file on a stalled disk). The GUI never waits for it.
struct Session
{
    enum Status { Opening, Ready, Failed };

    // Set before the thread starts, read-only afterwards.
    QString path;
    int rate = 48000;
    qint64 startFrame = 0;
    qint64 endFrame = 0;                  // 0 = end of file
    qint64 startMs = 0, endMs = 0;

    // Written by the thread before `status` leaves Opening (release), read by
    // the GUI after it sees that (acquire).
    std::atomic<int> status{Opening};
    QString error;
    qint64 durationMs = 0;
    bool seekable = false;

    // Shared between threads.
    std::atomic<bool> quit{false};
    std::mutex mutex;                     // guards `ring`, `decoderEof`; `wake` waits on it
    std::condition_variable wake;         // seek or quit, for the decoder thread
    RingBuffer ring;
    bool decoderEof = false;              // all audio of the range is in `ring`
    std::atomic<bool> reachedEnd{false};  // set by the audio thread
    std::atomic<qint64> playFrame{0};     // absolute index of the next frame to be heard
    std::atomic<qint64> seekRequest{-1};  // absolute frame, consumed by the decoder thread

    // The decoder thread's own.
    Decoder dec;
    std::vector<float> pending;           // decoded, not yet in `ring`
    size_t pendingPos = 0;                // frames of `pending` already written
    std::vector<float> scratch;
    qint64 writeFrame = -1;               // absolute index of the next decoded frame, -1 = unknown
    qint64 dropUntil = 0;                 // discard decoded audio before this frame (exact seek)
    bool inputDone = false;

    Session() { dec.interrupt.quit = &quit; }   // also interrupts the open

    void requestSeek(qint64 frame)
    {
        seekRequest = frame;
        std::lock_guard<std::mutex> lock(mutex);   // no lost wakeup between the check and the wait
        wake.notify_all();
    }

    void requestQuit()
    {
        quit = true;
        std::lock_guard<std::mutex> lock(mutex);
        wake.notify_all();
    }

    // Sleeps until a seek or quit is asked for, or `timeout` passes.
    template<class Duration>
    void waitFor(Duration timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, timeout, [this] { return quit.load() || seekRequest.load() >= 0; });
    }

    void run()
    {
        QString err;
        if (!dec.open(path, &err)) {
            error = err;
            status.store(Failed, std::memory_order_release);
            return;
        }
        durationMs = endMs > 0 ? endMs - startMs : std::max<qint64>(0, dec.fileDurationMs - startMs);
        seekable = dec.seekable;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ring.reset(size_t(rate));     // 1 second
        }
        dropUntil = startFrame;
        playFrame = startFrame;
        if (startFrame > 0 && seekable)
            doSeek(startFrame);           // CUE track: skip to its start
        status.store(Ready, std::memory_order_release);
        decodeLoop();
    }

    void decodeLoop()
    {
        while (!quit) {
            const qint64 seekTo = seekRequest.exchange(-1);
            if (seekTo >= 0)
                doSeek(seekTo);

            if (!flushPending()) {        // ring is full: ~1 s ahead of the device
                waitFor(50ms);
                continue;
            }
            if (inputDone) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    decoderEof = true;
                }
                waitFor(1h);              // until a seek (play again) or quit
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
        const int r = av_read_frame(dec.fmt, dec.pkt);
        if (r == AVERROR(EAGAIN)) {
            waitFor(10ms);
            return;
        }
        if (r < 0) {                       // end of file or read error: drain
            avcodec_send_packet(dec.codec, nullptr);
            receiveFrames();
            inputDone = true;
            return;
        }
        if (dec.pkt->stream_index == dec.stream && avcodec_send_packet(dec.codec, dec.pkt) >= 0)
            receiveFrames();
        av_packet_unref(dec.pkt);
    }

    void receiveFrames()
    {
        while (avcodec_receive_frame(dec.codec, dec.frame) == 0) {
            handleFrame(dec.frame);
            av_frame_unref(dec.frame);
        }
    }

    void handleFrame(const AVFrame *f)
    {
        if (inputDone || !dec.ensureResampler(f, rate))
            return;
        if (writeFrame < 0) {
            const int64_t ts = f->best_effort_timestamp;
            writeFrame = ts != AV_NOPTS_VALUE ? av_rescale_q(ts - dec.streamStart, dec.tb, AVRational{1, rate})
                                              : dropUntil;
        }
        const int maxOut = swr_get_out_samples(dec.swr, f->nb_samples);
        if (maxOut <= 0)
            return;
        scratch.resize(size_t(maxOut) * kChannels);
        auto *outPtr = reinterpret_cast<uint8_t *>(scratch.data());
        const int n = swr_convert(dec.swr, &outPtr, maxOut, const_cast<const uint8_t **>(f->extended_data),
                                  f->nb_samples);
        if (n > 0)
            append(scratch.data(), n);
    }

    // Converted audio at `writeFrame`: cut to the seek target and the CUE end.
    void append(const float *src, qint64 frames)
    {
        qint64 first = writeFrame;
        writeFrame += frames;
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
        if (dec.seekable) {
            const int64_t ts = av_rescale_q(frame, AVRational{1, rate}, dec.tb) + dec.streamStart;
            av_seek_frame(dec.fmt, dec.stream, ts, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(dec.codec);
        swr_free(&dec.swr);               // drop samples buffered in the resampler
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

// Decoder threads still running, also those of sessions already let go of.
struct ThreadCount
{
    std::mutex mutex;
    std::condition_variable done;
    int running = 0;
};

} // namespace

struct AudioEngine::Impl
{
    ma_context context{};
    bool contextOk = false;
    ma_device device{};
    bool deviceOk = false;
    int rate = 48000;
    std::atomic<float> gain{1.0f};

    // The session the audio thread plays. Changed only while the device is
    // stopped, so the audio thread never sees one go away.
    std::atomic<Session *> current{nullptr};

    // GUI thread only.
    std::shared_ptr<Session> session;     // null: nothing loaded
    std::shared_ptr<ThreadCount> threads = std::make_shared<ThreadCount>();
    bool opened = false;                  // `session` is Ready and the GUI has taken it in
    qint64 pendingSeekMs = -1;            // asked for while opening
    bool atEnd = false;                   // finished playing, play() restarts
    QTimer timer;

    static void dataCallback(ma_device *device, void *output, const void *, ma_uint32 frameCount)
    {
        auto *self = static_cast<Impl *>(device->pUserData);
        auto *out = static_cast<float *>(output);
        Session *s = self->current.load(std::memory_order_acquire);
        size_t got = 0;
        bool end = false;
        if (s) {
            std::lock_guard<std::mutex> lock(s->mutex);
            got = s->ring.read(out, frameCount);
            end = got < frameCount && s->decoderEof;
        }
        const float g = self->gain.load(std::memory_order_relaxed);
        for (size_t i = 0; i < got * kChannels; ++i)
            out[i] *= g;
        std::fill(out + got * kChannels, out + size_t(frameCount) * kChannels, 0.0f);
        if (s) {
            s->playFrame.fetch_add(qint64(got), std::memory_order_relaxed);
            if (end)
                s->reachedEnd.store(true);
        }
    }

    qint64 msToFrame(qint64 ms) const { return ms * rate / 1000; }
    qint64 startMs() const { return session ? session->startMs : 0; }
};

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    ma_context *ctx = nullptr;
    if (const char *backend = std::getenv("TEMBR_AUDIO_BACKEND"); backend && std::strcmp(backend, "null") == 0) {
        const ma_backend null = ma_backend_null;
        d->contextOk = ma_context_init(&null, 1, nullptr, &d->context) == MA_SUCCESS;
        if (d->contextOk)
            ctx = &d->context;
    }
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = 0;                               // device's native rate
    cfg.performanceProfile = ma_performance_profile_conservative;
    cfg.dataCallback = &Impl::dataCallback;
    cfg.pUserData = d.get();
    if (ma_device_init(ctx, &cfg, &d->device) == MA_SUCCESS) {
        d->deviceOk = true;
        d->rate = int(d->device.sampleRate);
    } else {
        m_lastError = QStringLiteral("Не удалось открыть звуковое устройство");
    }

    d->timer.setInterval(50);
    connect(&d->timer, &QTimer::timeout, this, &AudioEngine::poll);
    d->timer.start();
}

AudioEngine::~AudioEngine()
{
    unload();
    if (d->deviceOk)
        ma_device_uninit(&d->device);
    if (d->contextOk)
        ma_context_uninit(&d->context);
    // Decoder threads only touch their own session, but give them a moment to
    // close their files before the process goes. One stuck in the kernel is left.
    std::unique_lock<std::mutex> lock(d->threads->mutex);
    d->threads->done.wait_for(lock, 2s, [this] { return d->threads->running == 0; });
}

bool AudioEngine::isDeviceOk() const
{
    return d->deviceOk;
}

QString AudioEngine::deviceName() const
{
    return d->deviceOk ? QString::fromUtf8(d->device.playback.name) : QString();
}

void AudioEngine::load(const QString &path, qint64 startMs, qint64 endMs)
{
    unload();

    auto s = std::make_shared<Session>();
    s->path = path;
    s->rate = d->rate;
    s->startMs = startMs;
    s->endMs = endMs;
    s->startFrame = d->msToFrame(startMs);
    s->endFrame = endMs > 0 ? d->msToFrame(endMs) : 0;

    std::shared_ptr<ThreadCount> threads = d->threads;
    {
        std::lock_guard<std::mutex> lock(threads->mutex);
        ++threads->running;
    }
    std::thread([s, threads]() mutable {
        s->run();
        s.reset();                        // closes the file if the GUI has let go: counted as running
        std::lock_guard<std::mutex> lock(threads->mutex);
        --threads->running;
        threads->done.notify_all();
    }).detach();

    d->session = std::move(s);
    d->current.store(d->session.get(), std::memory_order_release);   // the device is stopped: see unload()
    d->opened = false;
    d->pendingSeekMs = -1;
    d->atEnd = false;
    d->timer.setInterval(5);              // take in the result of the open quickly

    emit durationChanged(0);
    emit positionChanged(0);
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
    haltDevice();                         // the audio thread is done with `current`
    d->current.store(nullptr, std::memory_order_release);
    if (d->session) {
        d->session->requestQuit();        // also interrupts an open or read in progress
        d->session.reset();               // its thread lets go of it when it ends
    }
    d->opened = false;
    d->pendingSeekMs = -1;
    d->atEnd = false;
    d->timer.setInterval(50);
    setState(State::Stopped);
}

bool AudioEngine::startDevice()
{
    if (ma_device_is_started(&d->device) || ma_device_start(&d->device) == MA_SUCCESS)
        return true;
    m_lastError = QStringLiteral("Не удалось запустить воспроизведение");
    emit errorOccurred(m_lastError);
    return false;
}

void AudioEngine::play()
{
    if (!d->session || !d->deviceOk)
        return;
    if (d->atEnd) {
        d->atEnd = false;
        seek(0);
    }
    if (d->opened && !startDevice())
        return;
    setState(State::Playing);             // while opening: starts in checkOpened()
}

void AudioEngine::pause()
{
    if (m_state != State::Playing)
        return;
    if (d->deviceOk && ma_device_is_started(&d->device))
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
    if (d->session) {
        seek(0);
        d->atEnd = false;
    }
    setState(State::Stopped);
    emit positionChanged(0);
}

void AudioEngine::seek(qint64 ms)
{
    if (!d->session)
        return;
    if (!d->opened) {
        d->pendingSeekMs = std::max<qint64>(0, ms);   // applied once the file is open
        return;
    }
    if (!d->session->seekable)
        return;
    ms = std::clamp<qint64>(ms, 0, d->session->durationMs);
    const qint64 frame = d->session->startFrame + d->msToFrame(ms);
    d->session->playFrame = frame;        // show the new position right away
    d->session->requestSeek(frame);
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

bool AudioEngine::isLoaded() const
{
    return bool(d->session);
}

bool AudioEngine::isLoading() const
{
    return d->session && !d->opened;
}

qint64 AudioEngine::positionMs() const
{
    if (!d->session)
        return 0;
    if (!d->opened)
        return std::max<qint64>(0, d->pendingSeekMs);
    const qint64 ms = (d->session->playFrame.load() - d->session->startFrame) * 1000 / d->rate;
    return std::clamp<qint64>(ms, 0, std::max<qint64>(d->session->durationMs, 0));
}

qint64 AudioEngine::durationMs() const
{
    return d->opened ? d->session->durationMs : 0;
}

bool AudioEngine::isSeekable() const
{
    return d->opened && d->session->seekable;
}

void AudioEngine::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged(s);
}

// Takes in the result of the open, once the decoder thread has one.
// Signal handlers may load another track or unload: after each emit the
// session is checked again.
void AudioEngine::checkOpened()
{
    const std::shared_ptr<Session> s = d->session;
    const int status = s->status.load(std::memory_order_acquire);
    if (status == Session::Opening)
        return;
    d->timer.setInterval(50);
    if (status == Session::Failed) {
        m_lastError = s->error;
        unload();
        emit loadFailed(s->error);
        return;
    }
    d->opened = true;
    const qint64 seekTo = d->pendingSeekMs;
    d->pendingSeekMs = -1;
    emit durationChanged(s->durationMs);
    if (d->session != s)
        return;
    if (seekTo > 0)
        seek(seekTo);
    if (d->session != s)
        return;
    emit positionChanged(positionMs());
    if (d->session != s)
        return;
    if (m_state == State::Playing && (!d->deviceOk || !startDevice()))
        setState(State::Stopped);
    if (d->session == s)
        emit loaded();
}

void AudioEngine::poll()
{
    if (d->session && !d->opened)
        checkOpened();
    const std::shared_ptr<Session> s = d->session;
    if (!s || !d->opened || m_state != State::Playing)
        return;
    emit positionChanged(positionMs());
    if (d->session == s && m_state == State::Playing && s->reachedEnd.exchange(false)) {
        ma_device_stop(&d->device);
        d->atEnd = true;
        setState(State::Stopped);
        emit finished();
    }
}
