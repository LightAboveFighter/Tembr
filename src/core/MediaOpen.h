#pragma once

#include <QString>

#include <atomic>

struct AVFormatContext;

// Opens media with FFmpeg the same way everywhere. A playlist, a drop or
// D-Bus can name any path, so FFmpeg may only use the file protocol for
// local paths and HTTP(S) for streams; network reads time out, and a
// blocking open or read can be interrupted.
namespace MediaOpen {

struct Interrupt
{
    const std::atomic<bool> *quit = nullptr;   // when set: give up at once (e.g. the decoder stops)
    std::atomic<qint64> deadlineMs{0};         // steady clock; 0 = none
};

// Whether `path` is something FFmpeg may open: a local path, file://, http(s)://.
bool isAllowed(const QString &path);

// avformat_open_input() and, if asked, avformat_find_stream_info(). The
// whole open gives up after `timeoutMs`. `interrupt` must outlive `*ctx`:
// FFmpeg calls back into it on every blocking read. Returns an AVERROR (< 0)
// on failure, with *ctx left null.
int open(AVFormatContext **ctx, const QString &path, Interrupt *interrupt, bool findStreamInfo = true,
         int timeoutMs = 15000);

} // namespace MediaOpen
