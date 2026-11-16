#include "core/SessionFiles.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

PlaylistData playlist(const QString &name, int tracks)
{
    PlaylistData p;
    p.name = name;
    for (int i = 0; i < tracks; ++i)
        p.tracks << Track::fromPath(QStringLiteral("/m/%1/%2.mp3").arg(name).arg(i));
    return p;
}

QStringList xspfFiles(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral("*.xspf")}, QDir::Files, QDir::Name);
}

} // namespace

class TestSessionFiles : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(SessionFiles::save(dir.path(), {playlist(QStringLiteral("A"), 2), playlist(QStringLiteral("B"), 3)})
                    .isEmpty());
        const SessionFiles::Loaded l = SessionFiles::load(dir.path());
        QCOMPARE(l.playlists.size(), 2);
        QCOMPARE(l.playlists[0].name, QStringLiteral("A"));
        QCOMPARE(l.playlists[1].tracks.size(), 3);
        QVERIFY(l.keptAside.isEmpty());
    }

    // Fewer tabs than before: the files of the closed ones go.
    void oldFilesRemovedAfterASave()
    {
        QTemporaryDir dir;
        SessionFiles::save(dir.path(), {playlist(QStringLiteral("A"), 1), playlist(QStringLiteral("B"), 1),
                                        playlist(QStringLiteral("C"), 1)});
        SessionFiles::save(dir.path(), {playlist(QStringLiteral("A"), 1)});
        QCOMPARE(xspfFiles(dir.path()), QStringList{QStringLiteral("000.xspf")});
    }

    // A crash after the files were written but before the count was saved:
    // all files are read, not just as many as the old count says.
    void allFilesAreRead()
    {
        QTemporaryDir dir;
        SessionFiles::save(dir.path(), {playlist(QStringLiteral("A"), 1), playlist(QStringLiteral("B"), 1),
                                        playlist(QStringLiteral("C"), 1)});
        QCOMPARE(SessionFiles::load(dir.path()).playlists.size(), 3);
    }

    // A damaged file is copied aside before the next save replaces it; what
    // could be read from it is loaded.
    void damagedFileIsKeptAside()
    {
        QTemporaryDir dir;
        SessionFiles::save(dir.path(), {playlist(QStringLiteral("A"), 3), playlist(QStringLiteral("B"), 1)});
        QFile f(dir.filePath(QStringLiteral("000.xspf")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray xml = f.readAll();
        f.close();
        xml.insert(xml.indexOf("/m/A/2.mp3"), "<broken");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(xml);
        f.close();

        const SessionFiles::Loaded l = SessionFiles::load(dir.path());
        QCOMPARE(l.playlists.size(), 2);
        QCOMPARE(l.playlists[0].tracks.size(), 2);
        QCOMPARE(l.keptAside.size(), 1);
        QFile kept(l.keptAside.first());
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), xml);

        SessionFiles::save(dir.path(), l.playlists);   // the copy survives the next save
        QVERIFY(QFileInfo::exists(l.keptAside.first()));
    }

    // When one file cannot be written, none of the old ones is removed.
    void failedSaveRemovesNothing()
    {
        QTemporaryDir dir;
        SessionFiles::save(dir.path(), {playlist(QStringLiteral("A"), 1), playlist(QStringLiteral("B"), 1)});
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("002.xspf")));   // a directory: cannot be written
        QFile::remove(dir.filePath(QStringLiteral("001.xspf")));
        QFile stale(dir.filePath(QStringLiteral("005.xspf")));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        const QStringList failed = SessionFiles::save(
            dir.path(), {playlist(QStringLiteral("A"), 1), playlist(QStringLiteral("B"), 1), playlist(QStringLiteral("C"), 1)});
        QCOMPARE(failed.size(), 1);
        QVERIFY(QFileInfo::exists(stale.fileName()));
    }
};

QTEST_GUILESS_MAIN(TestSessionFiles)
#include "test_sessionfiles.moc"
