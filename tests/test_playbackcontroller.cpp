// PlaybackController with real models and the engine on miniaudio's null
// device (real time, nothing is heard): short files keep it quick.

#include "TestAudio.h"
#include "audio/AudioEngine.h"
#include "ui/PlaybackController.h"
#include "ui/PlaylistModel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using State = AudioEngine::State;

class TestPlaybackController : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    QString wav(const QString &name, int ms = 300) { return TestAudio::writeWav(m_dir.filePath(name), ms); }

    // A playlist of `names`; "dead:" ones point to files that are not there.
    static void fill(PlaylistModel &model, const QStringList &paths)
    {
        TrackList tracks;
        for (const QString &p : paths) {
            Track t = Track::fromPath(p);
            t.scanned = true;   // no tag reading in the background
            tracks << t;
        }
        model.insertTracks(-1, tracks);
    }

    QStringList wavs(const QString &prefix, int n, int ms = 300)
    {
        QStringList out;
        for (int i = 0; i < n; ++i)
            out << wav(QStringLiteral("%1%2.wav").arg(prefix).arg(i), ms);
        return out;
    }

private slots:
    void initTestCase()
    {
        qputenv("TEMBR_AUDIO_BACKEND", "null");
        QVERIFY(m_dir.isValid());
    }

    void endOfTrackGoesOnThenStops()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        fill(model, wavs(QStringLiteral("e"), 2, 200));
        QSignalSpy follow(&pc, &PlaybackController::followRow);
        pc.playRow(&model, 0);
        QCOMPARE(pc.playingModel(), &model);
        QCOMPARE(engine.state(), State::Playing);
        QTRY_COMPARE_WITH_TIMEOUT(model.currentRow(), 1, 3000);
        QCOMPARE(follow.size(), 1);
        QCOMPARE(follow[0][1].toInt(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), State::Stopped, 3000);   // after the last one
        QCOMPARE(model.currentRow(), 1);
    }

    void previousRestartsOrGoesBack()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        fill(model, {wav(QStringLiteral("p0.wav")), wav(QStringLiteral("long.wav"), 5000)});
        QSignalSpy opened(&pc, &PlaybackController::trackOpened);

        pc.playRow(&model, 1);
        QVERIFY(opened.wait(3000));
        engine.seek(3500);
        QTRY_VERIFY(engine.positionMs() > 3000);
        pc.previous();   // into the track: back to its start
        QCOMPARE(model.currentRow(), 1);
        QTRY_VERIFY(engine.positionMs() < 1000);

        pc.previous();   // at its start: the track before
        QCOMPARE(model.currentRow(), 0);
        pc.pause();
        pc.previous();   // on the first row: nothing
        QCOMPARE(model.currentRow(), 0);
    }

    // Started from nothing: next() plays the first row of the playlist given.
    void nextFromNothingStartsTheFallback()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        fill(model, wavs(QStringLiteral("f"), 2));
        QVERIFY(pc.hasNext(&model));
        QVERIFY(!pc.hasNext(nullptr));
        QVERIFY(!pc.hasPrevious());
        pc.next(&model);
        QCOMPARE(model.currentRow(), 0);
        QVERIFY(pc.hasNext(&model));
        QVERIFY(!pc.hasPrevious());
        pc.stop();
    }

    void deadFilesAreSkippedOnce()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        const QString good = wav(QStringLiteral("good.wav"));
        fill(model, {m_dir.filePath(QStringLiteral("gone1.wav")), good});
        QSignalSpy messages(&pc, &PlaybackController::message);
        pc.playRow(&model, 0);
        QTRY_COMPARE_WITH_TIMEOUT(model.currentRow(), 1, 3000);
        QVERIFY(model.track(0).missing);
        QVERIFY(!messages.isEmpty());
        QVERIFY(messages[0][0].toString().startsWith(QStringLiteral("gone1.wav")));
        pc.stop();

        // Only dead files: one pass, then it stops instead of spinning.
        PlaylistModel dead;
        fill(dead, {m_dir.filePath(QStringLiteral("d1.wav")), m_dir.filePath(QStringLiteral("d2.wav")),
                    m_dir.filePath(QStringLiteral("d3.wav"))});
        QSignalSpy started(&pc, &PlaybackController::trackStarted);
        pc.playRow(&dead, 0);
        QTRY_VERIFY_WITH_TIMEOUT(dead.track(0).missing && dead.track(1).missing && dead.track(2).missing, 3000);
        QTest::qWait(300);
        QCOMPARE(started.size(), 3);   // each tried once, none again
        QCOMPARE(engine.isLoaded(), false);
    }

    // Paused or stopped while the file was opening: no skipping on failure.
    void noSkipWhenStoppedWhileOpening()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        fill(model, {m_dir.filePath(QStringLiteral("nothere.wav")), wav(QStringLiteral("n1.wav"))});
        QSignalSpy changed(&pc, &PlaybackController::currentChanged);
        pc.playRow(&model, 0);
        pc.stop();
        QTRY_VERIFY_WITH_TIMEOUT(model.track(0).missing, 3000);
        QTest::qWait(200);
        QCOMPARE(model.currentRow(), 0);
    }

    void anotherPlaylistTakesOver()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel a, b;
        fill(a, wavs(QStringLiteral("a"), 2));
        fill(b, wavs(QStringLiteral("b"), 2));
        pc.playRow(&a, 1);
        pc.playRow(&b, 0);
        QCOMPARE(pc.playingModel(), &b);
        QCOMPARE(a.currentUid(), quint64(0));
        QCOMPARE(b.currentRow(), 0);
        pc.stop();
    }

    // The row of the playing track is removed: it plays on, and "next" is
    // the track that came after it. Stopped: the mark just goes.
    void removedRowPlaysOn()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        fill(model, wavs(QStringLiteral("r"), 3, 2000));
        pc.playRow(&model, 1);
        const quint64 third = model.track(2).uid;
        model.removeRowList({1});
        QVERIFY(model.isCurrentDetached());
        QCOMPARE(engine.state(), State::Playing);
        QVERIFY(pc.currentTrack());
        pc.next();
        QCOMPARE(model.currentUid(), third);

        pc.stop();
        QSignalSpy changed(&pc, &PlaybackController::currentChanged);
        model.removeRowList({model.currentRow()});
        QCOMPARE(model.currentUid(), quint64(0));
        QVERIFY(!pc.currentTrack());
        QCOMPARE(changed.size(), 1);
    }

    void deletedFilesSkipToTheNextSurvivor()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        const QStringList files = wavs(QStringLiteral("del"), 4, 2000);
        fill(model, files);

        // Playing: on at once to the next file not deleted (still listed, as
        // in Local files, marked missing).
        pc.playRow(&model, 0);
        pc.skipDeleted({files[0], files[1]});
        QCOMPARE(model.currentRow(), 2);
        QCOMPARE(engine.state(), State::Playing);

        // Paused: chosen, not played.
        pc.pause();
        pc.skipDeleted({files[2]});
        QCOMPARE(model.currentRow(), 3);
        QVERIFY(engine.state() != State::Playing);

        // Nothing after it: stop, no current track.
        pc.playRow(&model, 3);
        pc.skipDeleted({files[3]});
        QCOMPARE(engine.state(), State::Stopped);
        QCOMPARE(model.currentUid(), quint64(0));
    }

    void releasedPlaylistUnloads()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        auto *model = new PlaylistModel;
        fill(*model, wavs(QStringLiteral("rel"), 1, 2000));
        pc.playRow(model, 0);
        pc.release(model);
        QCOMPARE(pc.playingModel(), nullptr);
        QVERIFY(!engine.isLoaded());
        pc.playRow(model, 0);
        delete model;   // gone without release(): forgotten
        QCOMPARE(pc.playingModel(), nullptr);
        QVERIFY(!pc.currentTrack());
        pc.stop();
    }

    void setCurrentDoesNotOpen()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        PlaylistModel model;
        fill(model, wavs(QStringLiteral("sc"), 2));
        pc.setCurrent(&model, 1);
        QCOMPARE(model.currentRow(), 1);
        QVERIFY(!engine.isLoaded());
        QCOMPARE(pc.pauseOrResume(), false);   // stopped: up to the caller
        pc.setCurrent(&model, -1);
        QCOMPARE(model.currentUid(), quint64(0));
    }

    void volumeIsBounded()
    {
        AudioEngine engine;
        PlaybackController pc(&engine);
        QSignalSpy changed(&pc, &PlaybackController::volumeChanged);
        pc.setVolume(98);
        pc.changeVolume(5);
        QCOMPARE(pc.volume(), 100);
        pc.changeVolume(5);   // no change, no signal
        QCOMPARE(changed.size(), 2);
        pc.setVolume(3);
        pc.changeVolume(-5);
        QCOMPARE(pc.volume(), 0);
        QVERIFY(qAbs(engine.volume() - 0.0f) < 0.001f);
        pc.setVolume(50);
        QVERIFY(qAbs(engine.volume() - 0.5f) < 0.001f);
    }
};

QTEST_MAIN(TestPlaybackController)
#include "test_playbackcontroller.moc"
