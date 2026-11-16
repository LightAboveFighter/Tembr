// Manual end-to-end check of collection, tag reading and playback on a real
// audio device (volume 0, so nothing is heard):
//
//   engine_smoke <folder with audio files and/or .cue sheets>
//
// Not registered with ctest because it needs a sound device.

#include "audio/AudioEngine.h"
#include "core/MediaCollector.h"
#include "core/MetadataReader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", qPrintable(what));
    if (!ok)
        ++failures;
}

// Plays a track to the end (optionally seeking after 500 ms); returns wall time.
qint64 playThrough(AudioEngine &engine, const Track &t, qint64 seekTo = -1)
{
    QEventLoop loop;
    QElapsedTimer clock;
    bool finished = false;
    QString failed;
    QObject::connect(&engine, &AudioEngine::finished, &loop, [&] { finished = true; loop.quit(); });
    QObject::connect(&engine, &AudioEngine::loadFailed, &loop, [&](const QString &e) { failed = e; loop.quit(); });
    QTimer::singleShot(t.durationMs + 4000, &loop, &QEventLoop::quit);
    if (seekTo >= 0) {
        QTimer::singleShot(500, &loop, [&] {
            engine.seek(seekTo);
            QTimer::singleShot(200, &loop, [&] {
                const qint64 pos = engine.positionMs();
                check(pos >= seekTo + 100 && pos <= seekTo + 400,
                      QStringLiteral("position after seek to %1 ms: %2 ms").arg(seekTo).arg(pos));
            });
        });
    }
    clock.start();
    engine.load(t.path, t.startMs, t.endMs);
    engine.play();
    loop.exec();
    QObject::disconnect(&engine, nullptr, &loop, nullptr);
    if (!failed.isEmpty()) {
        check(false, QStringLiteral("load: ") + failed);
        return -1;
    }
    check(finished, QStringLiteral("finished signal"));
    return clock.elapsed();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: engine_smoke <folder>\n");
        return 2;
    }

    std::printf("Collecting %s\n", argv[1]);
    TrackList tracks = MediaCollector::collect({QString::fromLocal8Bit(argv[1])});
    for (Track &t : tracks) {
        t = MetadataReader::read(t);
        std::printf("  %-28s | %-22s | %6s | %-26s | %s\n", qPrintable(t.displayTitle().left(28)),
                    qPrintable(t.album.left(22)), qPrintable(formatDuration(t.durationMs)),
                    qPrintable(t.formatSummary()), qPrintable(t.fileName()));
    }

    AudioEngine engine;
    check(engine.isDeviceOk(), QStringLiteral("audio device: ") + engine.deviceName());
    if (!engine.isDeviceOk())
        return 1;
    engine.setVolume(0.0f);

    bool seekTested = false;
    for (const Track &t : std::as_const(tracks)) {
        std::printf("Playing %s (%s)\n", qPrintable(t.displayTitle()), qPrintable(formatDuration(t.durationMs)));
        // Seek once, in the first track long enough for it.
        const bool doSeek = !seekTested && t.durationMs > 2500;
        seekTested = seekTested || doSeek;
        const qint64 seekTo = doSeek ? t.durationMs - 1000 : -1;
        const qint64 elapsed = playThrough(engine, t, seekTo);
        // With a seek to 1 s before the end, playback should take ~0.5 s + 1 s.
        const qint64 expected = seekTo >= 0 ? 1500 : t.durationMs;
        check(elapsed > 0 && qAbs(elapsed - expected) < 700,
              QStringLiteral("wall time %1 ms, expected ~%2 ms").arg(elapsed).arg(expected));
    }

    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
