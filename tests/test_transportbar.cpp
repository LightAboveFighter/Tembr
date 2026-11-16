// The transport bar driving a PlaybackController, with the engine on
// miniaudio's null device.

#include "TestAudio.h"
#include "audio/AudioEngine.h"
#include "ui/PlaybackController.h"
#include "ui/PlaylistModel.h"
#include "ui/TransportBar.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

class TestTransportBar : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    struct Rig
    {
        AudioEngine engine;
        PlaybackController playback{&engine};
        PlaylistModel model;
        QAction previous{QStringLiteral("Предыдущий")};
        QAction play{QStringLiteral("Воспроизвести")};
        QAction next{QStringLiteral("Следующий")};
        QAction stop{QStringLiteral("Стоп")};
        TransportBar bar{&playback, {&previous, &play, &next, &stop}};

        explicit Rig(const QString &file)
        {
            Track t = Track::fromPath(file);
            t.scanned = true;
            model.insertTracks(-1, {t});
            // As the window does: stopped, the first row.
            QObject::connect(&play, &QAction::triggered, &playback, [this] {
                if (!playback.pauseOrResume())
                    playback.playRow(&model, 0);
            });
            bar.resize(800, 60);
            bar.show();
        }
    };

private slots:
    void initTestCase()
    {
        qputenv("TEMBR_AUDIO_BACKEND", "null");
        QVERIFY(m_dir.isValid());
    }

    void playButtonToggles()
    {
        Rig r(TestAudio::writeWav(m_dir.filePath(QStringLiteral("t.wav")), 5000));
        const qint64 playIcon = r.play.icon().cacheKey();
        QTest::mouseClick(r.bar.playButton(), Qt::LeftButton);
        QCOMPARE(r.engine.state(), AudioEngine::State::Playing);
        QCOMPARE(r.play.text(), QStringLiteral("Пауза"));
        const qint64 pauseIcon = r.play.icon().cacheKey();
        QVERIFY(pauseIcon != playIcon);

        QTest::mouseClick(r.bar.playButton(), Qt::LeftButton);
        QCOMPARE(r.engine.state(), AudioEngine::State::Paused);
        QCOMPARE(r.play.text(), QStringLiteral("Воспроизвести"));
        QVERIFY(r.play.icon().cacheKey() != pauseIcon);
        r.playback.stop();
    }

    // Disabled until the file is open; then its range is the length. While
    // the user holds it the position does not move it; letting go seeks.
    void seekSlider()
    {
        Rig r(TestAudio::writeWav(m_dir.filePath(QStringLiteral("s.wav")), 5000));
        QSlider *seek = r.bar.seekSlider();
        QVERIFY(!seek->isEnabled());
        QSignalSpy opened(&r.playback, &PlaybackController::trackOpened);
        r.play.trigger();
        QVERIFY(opened.wait(3000));
        QTRY_VERIFY(seek->isEnabled());
        QCOMPARE(qint64(seek->maximum()), r.engine.durationMs());
        QVERIFY(qAbs(seek->maximum() - 5000) < 50);
        QTRY_VERIFY(seek->value() > 0);   // follows the position
        QVERIFY(r.bar.durationLabel()->text() == formatDuration(r.engine.durationMs() + 1));

        seek->setSliderDown(true);
        seek->setValue(4000);   // dragged there
        QTest::qWait(300);
        QCOMPARE(seek->value(), 4000);   // the position did not pull it back
        QVERIFY(r.engine.positionMs() < 2000);
        seek->setSliderDown(false);   // released: seek
        QTRY_VERIFY(r.engine.positionMs() >= 4000);
        r.playback.stop();
    }

    void volumeSlider()
    {
        Rig r(TestAudio::writeWav(m_dir.filePath(QStringLiteral("v.wav")), 300));
        QSlider *volume = r.bar.volumeSlider();
        QCOMPARE(volume->value(), r.playback.volume());
        volume->setValue(40);
        QCOMPARE(r.playback.volume(), 40);
        QVERIFY(qAbs(r.engine.volume() - 0.4f) < 0.001f);
        QCOMPARE(volume->toolTip(), QStringLiteral("Громкость: 40%"));
        r.playback.changeVolume(-5);   // Ctrl+Down, MPRIS, ...
        QCOMPARE(volume->value(), 35);
        QCOMPARE(volume->toolTip(), QStringLiteral("Громкость: 35%"));
    }
};

QTEST_MAIN(TestTransportBar)
#include "test_transportbar.moc"
