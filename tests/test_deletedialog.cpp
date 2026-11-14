#include "ui/DeleteFilesDialog.h"

#include <QApplication>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>

class TestDeleteDialog : public QObject
{
    Q_OBJECT

    const QStringList m_paths{QStringLiteral("/m/A/01.mp3"), QStringLiteral("/m/B/02.flac"), QStringLiteral("/m/C/03.ogg")};

    static QPushButton *button(QDialog &d, const QString &text)
    {
        for (QPushButton *b : d.findChildren<QPushButton *>()) {
            if (b->text() == text)
                return b;
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("tembr-test"));
        QCoreApplication::setApplicationName(QStringLiteral("tembr-test-deletedialog"));
        QSettings().clear();
    }
    void cleanupTestCase() { QSettings().clear(); }

    // Enter presses the focused button, as Space does: "Details" opens the
    // list and the dialog stays.
    void enterOnDetails()
    {
        DeleteFilesDialog d(DeleteFilesDialog::Kind::ToTrash, m_paths, 0);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        QSignalSpy finished(&d, &QDialog::finished);
        d.detailsButton()->setFocus(Qt::TabFocusReason);
        QTest::keyClick(d.detailsButton(), Qt::Key_Return);
        QVERIFY(d.table()->isVisibleTo(&d));
        QCOMPARE(d.detailsButton()->text(), QStringLiteral("Скрыть список"));
        QTest::keyClick(d.detailsButton(), Qt::Key_Enter);   // the keypad one too
        QVERIFY(!d.table()->isVisibleTo(&d));
        QVERIFY(d.isVisible());
        QCOMPARE(finished.count(), 0);
    }

    // With the focus on no button Enter means "Cancel", never "Delete".
    void enterElsewhereCancels()
    {
        DeleteFilesDialog d(DeleteFilesDialog::Kind::ToTrash, m_paths, 0);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        QPushButton *cancel = button(d, QStringLiteral("Отмена"));
        QVERIFY(cancel && cancel->isDefault());
        QVERIFY(!button(d, QStringLiteral("Удалить"))->isDefault());
        QSignalSpy finished(&d, &QDialog::finished);
        QTest::keyClick(&d, Qt::Key_Return);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().toInt(), int(QDialog::Rejected));
    }

    // Without the list it is a fixed-size question; with it, it resizes
    // up to showing every row, not without end.
    void sizeLimits()
    {
        QStringList many;
        for (int i = 0; i < 30; ++i)
            many << QStringLiteral("/m/Artist %1/Album/%1 - Title.mp3").arg(i);
        DeleteFilesDialog d(DeleteFilesDialog::Kind::ToTrash, many, 3);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        QCOMPARE(d.minimumSize(), d.maximumSize());
        const QSize plain = d.size();
        noOverlaps(d);

        d.detailsButton()->click();   // grows to show the list, not squeezed into the old size
        QVERIFY(d.minimumHeight() > plain.height());
        QTRY_VERIFY(d.height() >= d.minimumHeight() && d.width() >= d.minimumWidth());
        QVERIFY(d.table()->height() >= d.table()->minimumHeight());
        QVERIFY(d.maximumHeight() > d.minimumHeight());   // room for more rows
        QVERIFY(d.maximumWidth() > d.minimumWidth());     // and for longer folders
        QVERIFY(d.maximumWidth() < 2000 && d.maximumHeight() < 2000);
        noOverlaps(d);
        d.resize(5000, 5000);
        QTRY_VERIFY(d.width() <= d.maximumWidth() && d.height() <= d.maximumHeight());
        d.resize(1, 1);   // as small as it goes, everything still readable
        QTRY_COMPARE(d.size(), d.minimumSize());
        noOverlaps(d);
        QVERIFY(d.table()->height() >= d.table()->minimumHeight());

        d.detailsButton()->click();   // list hidden again: back to the question
        QCOMPARE(d.minimumSize(), d.maximumSize());
        QCOMPARE(d.maximumSize(), plain);
    }

    // Every visible widget fits inside the dialog, wrapped texts get their
    // full height, and no two of them overlap.
    static void noOverlaps(QDialog &d)
    {
        QApplication::processEvents();
        QList<QWidget *> shown;
        for (QWidget *w : d.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!w->isVisibleTo(&d))
                continue;
            QVERIFY2(d.rect().contains(w->geometry()), w->metaObject()->className());
            if (w->hasHeightForWidth())
                QVERIFY2(w->height() >= w->heightForWidth(w->width()), w->metaObject()->className());
            for (QWidget *o : shown)
                QVERIFY2(!o->geometry().intersects(w->geometry()),
                         qPrintable(QStringLiteral("%1 / %2").arg(o->metaObject()->className(), w->metaObject()->className())));
            shown << w;
        }
        QVERIFY(shown.size() >= 5);
    }

    // Not a dialog GNOME attaches to its window (that one resizes from both
    // sides at once): a utility window, still above the main window.
    void resizesLikeAWindow()
    {
        DeleteFilesDialog d(DeleteFilesDialog::Kind::ToTrash, m_paths, 0);
        QCOMPARE(d.windowType(), Qt::Tool);
        QVERIFY(d.windowFlags() & Qt::WindowCloseButtonHint);
    }
};

QTEST_MAIN(TestDeleteDialog)
#include "test_deletedialog.moc"
