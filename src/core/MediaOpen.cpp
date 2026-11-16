#include "core/MediaOpen.h"

#include <QFile>

#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace MediaOpen {

namespace {

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int interrupted(void *opaque)
{
    const auto *i = static_cast<const Interrupt *>(opaque);
    if (i->quit && i->quit->load(std::memory_order_relaxed))
        return 1;
    const qint64 deadline = i->deadlineMs.load(std::memory_order_relaxed);
    return deadline > 0 && nowMs() > deadline ? 1 : 0;
}

QString schemeOf(const QString &path)
{
    const int at = path.indexOf(QLatin1String("://"));
    return at > 0 ? path.left(at).toLower() : QString();
}

} // namespace

bool isAllowed(const QString &path)
{
    const QString scheme = schemeOf(path);
    return scheme.isEmpty() || scheme == QLatin1String("file") || scheme == QLatin1String("http")
           || scheme == QLatin1String("https");
}

int open(AVFormatContext **ctx, const QString &path, Interrupt *interrupt, bool findStreamInfo, int timeoutMs)
{
    *ctx = nullptr;
    if (!isAllowed(path))
        return AVERROR_PROTOCOL_NOT_FOUND;
    const QString scheme = schemeOf(path);
    const bool network = scheme == QLatin1String("http") || scheme == QLatin1String("https");

    AVFormatContext *c = avformat_alloc_context();
    if (!c)
        return AVERROR(ENOMEM);
    c->interrupt_callback.callback = &interrupted;
    c->interrupt_callback.opaque = interrupt;

    AVDictionary *opts = nullptr;
    // Also for what the demuxer opens by itself (HLS segments and the like).
    av_dict_set(&opts, "protocol_whitelist", network ? "http,https,tcp,tls,crypto,httpproxy" : "file", 0);
    if (network)
        av_dict_set(&opts, "rw_timeout", "10000000", 0);   // µs: a stalled stream ends instead of hanging

    interrupt->deadlineMs = nowMs() + timeoutMs;
    const QByteArray name = scheme.isEmpty() ? QFile::encodeName(path) : path.toUtf8();
    int r = avformat_open_input(&c, name.constData(), nullptr, &opts);   // frees `c` on failure
    av_dict_free(&opts);
    if (r >= 0 && findStreamInfo)
        avformat_find_stream_info(c, nullptr);   // best effort, as before: some files lack it
    interrupt->deadlineMs = 0;
    if (r < 0)
        return r;
    *ctx = c;
    return 0;
}

} // namespace MediaOpen
