#include "core/FileTrash.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

class TestFileTrash : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;   // stands in for ~/.local/share: never the real trash

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_DATA_HOME", m_home.filePath(QStringLiteral("share")).toUtf8());
        QDir().mkpath(m_home.filePath(QStringLiteral("share")));
    }

    void toTrashCanBeUndone()
    {
        const QString song = m_home.filePath(QStringLiteral("music/Album/01 Song.mp3"));
        QDir().mkpath(QFileInfo(song).path());
        QFile f(song);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("abc");
        f.close();
        const QString missing = m_home.filePath(QStringLiteral("music/gone.mp3"));

        const FileTrash::Result r = FileTrash::moveToTrash({song, missing});
        QCOMPARE(r.done, (QStringList{song, missing}));
        QVERIFY(r.failed.isEmpty());
        QVERIFY(!QFile::exists(song));
        // In the trash given by XDG_DATA_HOME, with its .trashinfo for restoring.
        const QDir trash(m_home.filePath(QStringLiteral("share/Trash")));
        QVERIFY(trash.exists(QStringLiteral("files/01 Song.mp3")));
        QVERIFY(trash.exists(QStringLiteral("info/01 Song.mp3.trashinfo")));
    }

    void removeForever()
    {
        const QString song = m_home.filePath(QStringLiteral("music/02.mp3"));
        QFile f(song);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const FileTrash::Result r = FileTrash::removeForever({song});
        QCOMPARE(r.done, QStringList{song});
        QVERIFY(!QFile::exists(song));
    }

    void failureIsReported()
    {
        if (geteuid() == 0)
            QSKIP("root may delete anything");
        const QString dir = m_home.filePath(QStringLiteral("locked"));
        QDir().mkpath(dir);
        const QString song = dir + QStringLiteral("/03.mp3");
        QFile f(song);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::ExeOwner);   // no deleting in it
        const FileTrash::Result r = FileTrash::removeForever({song});
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QCOMPARE(r.failed, QStringList{song});
        QVERIFY(QFile::exists(song));
    }
};

QTEST_GUILESS_MAIN(TestFileTrash)
#include "test_filetrash.moc"
