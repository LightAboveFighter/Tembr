#include "core/MetadataReader.h"

#include "core/FileTimes.h"
#include "core/MediaOpen.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace {

struct FormatContext
{
    AVFormatContext *ctx = nullptr;
    MediaOpen::Interrupt interrupt;   // ctx is closed in the destructor body, before this goes
    ~FormatContext() { avformat_close_input(&ctx); }

    bool open(const QString &path, bool findStreamInfo = true)
    {
        return MediaOpen::open(&ctx, path, &interrupt, findStreamInfo) >= 0;
    }
};

QString dictValue(AVDictionary *dict, const char *key)
{
    // av_dict_get is case-insensitive unless AV_DICT_MATCH_CASE is passed.
    const AVDictionaryEntry *e = av_dict_get(dict, key, nullptr, 0);
    return e ? QString::fromUtf8(e->value).trimmed() : QString();
}

int leadingInt(const QString &s)
{
    // "3/12" -> 3, "03" -> 3
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d+)"));
    const auto m = re.match(s);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

QString yearOf(const QString &date)
{
    static const QRegularExpression re(QStringLiteral("(\\d{4})"));
    const auto m = re.match(date);
    return m.hasMatch() ? m.captured(1) : date;
}

int channelCount(const AVCodecParameters *p)
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    return p->ch_layout.nb_channels;
#else
    return p->channels;
#endif
}

// Tags from a CUE sheet win over the file's (one file holds the whole album);
// for ordinary files the file wins over hints from #EXTINF and the like.
void merge(QString &field, const QString &fromFile, bool preferExisting)
{
    if (fromFile.isEmpty() || (preferExisting && !field.isEmpty()))
        return;
    field = fromFile;
}

} // namespace

namespace MetadataReader {

Track read(Track t)
{
    t.scanned = true;
    if (!t.isUrl()) {
        QFileInfo fi(t.path);
        if (!fi.isFile()) {
            t.missing = true;
            return t;
        }
        t.fileSize = fi.size();
        t.modifiedMs = FileTimes::modifiedMs(fi);
        if (t.addedMs == 0)
            t.addedMs = FileTimes::addedMs(fi);
    }

    FormatContext fc;
    if (!fc.open(t.path)) {
        t.missing = true;
        return t;
    }
    t.missing = false;

    const int si = av_find_best_stream(fc.ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVStream *st = si >= 0 ? fc.ctx->streams[si] : nullptr;

    // Ogg/Opus keep tags on the stream, most other containers on the format.
    auto tag = [&](const char *key) {
        QString v = dictValue(fc.ctx->metadata, key);
        if (v.isEmpty() && st)
            v = dictValue(st->metadata, key);
        return v;
    };

    const bool cue = t.isCueTrack();
    QString date = tag("date");
    if (date.isEmpty())
        date = tag("year");
    QString albumArtist = tag("album_artist");
    if (albumArtist.isEmpty())
        albumArtist = tag("albumartist");

    if (!cue) // per-track tags of a whole-album file are meaningless
        merge(t.title, tag("title"), false);
    merge(t.artist, tag("artist"), cue);
    merge(t.album, tag("album"), cue);
    merge(t.albumArtist, albumArtist, cue);
    merge(t.genre, tag("genre"), cue);
    merge(t.year, yearOf(date), cue);
    merge(t.composer, tag("composer"), cue);
    merge(t.comment, tag("comment"), cue);
    if (!cue && leadingInt(tag("track")) > 0)
        t.trackNo = leadingInt(tag("track"));
    if (leadingInt(tag("disc")) > 0 && (!cue || t.discNo == 0))
        t.discNo = leadingInt(tag("disc"));

    qint64 fileDurationMs = 0;
    if (fc.ctx->duration != AV_NOPTS_VALUE && fc.ctx->duration > 0)
        fileDurationMs = fc.ctx->duration / (AV_TIME_BASE / 1000);
    else if (st && st->duration != AV_NOPTS_VALUE)
        fileDurationMs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000});

    if (t.endMs > 0)
        t.durationMs = t.endMs - t.startMs;
    else
        t.durationMs = qMax<qint64>(0, fileDurationMs - t.startMs);

    if (st) {
        const AVCodecParameters *p = st->codecpar;
        t.codec = QString::fromLatin1(avcodec_get_name(p->codec_id));
        t.sampleRate = p->sample_rate;
        t.channels = channelCount(p);
        const qint64 br = p->bit_rate > 0 ? p->bit_rate : fc.ctx->bit_rate;
        t.bitrateKbps = int(br / 1000);
    }
    // "pcm_s16le" & co. are all just PCM to a listener.
    if (t.codec.startsWith(QLatin1String("pcm_")))
        t.codec = QStringLiteral("PCM");
    return t;
}

QImage readCover(const QString &path)
{
    FormatContext fc;
    if (fc.open(path, false)) {   // attached pictures are known without stream info
        for (unsigned i = 0; i < fc.ctx->nb_streams; ++i) {
            AVStream *s = fc.ctx->streams[i];
            if ((s->disposition & AV_DISPOSITION_ATTACHED_PIC) && s->attached_pic.size > 0) {
                QImage img = QImage::fromData(s->attached_pic.data, s->attached_pic.size);
                if (!img.isNull())
                    return img;
            }
        }
    }

    const QDir dir = QFileInfo(path).absoluteDir();
    const QStringList candidates = dir.entryList(
        {QStringLiteral("cover.*"), QStringLiteral("folder.*"), QStringLiteral("front.*"),
         QStringLiteral("albumart*.*")},
        QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QString &name : candidates) {
        QImage img(dir.filePath(name));
        if (!img.isNull())
            return img;
    }
    return {};
}

QList<QPair<QString, QString>> readAllTags(const QString &path)
{
    QList<QPair<QString, QString>> out;
    FormatContext fc;
    if (!fc.open(path))
        return out;

    auto dump = [&](AVDictionary *dict) {
        const AVDictionaryEntry *e = nullptr;
        while ((e = av_dict_get(dict, "", e, AV_DICT_IGNORE_SUFFIX)))
            out.append({QString::fromUtf8(e->key), QString::fromUtf8(e->value)});
    };
    dump(fc.ctx->metadata);
    const int si = av_find_best_stream(fc.ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si >= 0)
        dump(fc.ctx->streams[si]->metadata);
    return out;
}

} // namespace MetadataReader
