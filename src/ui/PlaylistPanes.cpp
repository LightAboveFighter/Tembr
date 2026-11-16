#include "ui/PlaylistPanes.h"

#include "ui/Icons.h"
#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

// Names are shown as button and menu text, where '&' marks a shortcut.
QString menuText(const QString &name)
{
    QString out = name;
    return out.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

QWidget *makeHeader()
{
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("PaneHeader"));
    header->setAttribute(Qt::WA_StyledBackground);   // the style sheet draws its underline
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(6);
    return header;
}

} // namespace

PlaylistPicker::PlaylistPicker(QWidget *parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("PlaylistPicker"));
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setFocusPolicy(Qt::TabFocus);
    setContextMenuPolicy(Qt::CustomContextMenu);
}

void PlaylistPicker::wheelEvent(QWheelEvent *e)
{
    // Taken and dropped: not passed on to the parents either.
    e->accept();
}

PlaylistPanes::PlaylistPanes(QWidget *parent)
    : QWidget(parent)
{
    // --- Left: Local files
    m_localPane = new QWidget;
    auto *localLayout = new QVBoxLayout(m_localPane);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(0);
    m_localHeader = makeHeader();
    auto *localIcon = new QLabel;
    localIcon->setObjectName(QStringLiteral("PaneIcon"));
    m_localTitle = new QLabel(QStringLiteral("Локальные файлы"));
    m_localHeader->setToolTip(QStringLiteral("Вся музыка из папок фонотеки. Изменить список нельзя — "
                                             "только папки в настройках локальных файлов."));
    m_localHeader->layout()->addWidget(localIcon);
    m_localHeader->layout()->addWidget(m_localTitle);
    static_cast<QHBoxLayout *>(m_localHeader->layout())->addStretch();
    localLayout->addWidget(m_localHeader);

    // --- Right: one of the user's playlists
    m_rightPane = new QWidget;
    auto *rightLayout = new QVBoxLayout(m_rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    m_rightHeader = makeHeader();
    m_picker = new PlaylistPicker;
    m_picker->setToolTip(QStringLiteral("Выбрать плейлист"));
    m_menu = new QMenu(m_picker);
    connect(m_picker, &QToolButton::clicked, this, [this] {
        // Filled when opened: names and the playlist shown are read then.
        fillMenu();
        m_menu->popup(m_picker->mapToGlobal(QPoint(0, m_picker->height())));
    });
    connect(m_picker, &QWidget::customContextMenuRequested, this, &PlaylistPanes::showPickerContextMenu);
    m_rightHeader->layout()->addWidget(m_picker);
    static_cast<QHBoxLayout *>(m_rightHeader->layout())->addStretch();
    rightLayout->addWidget(m_rightHeader);

    // No playlists: the pane stays free, with a way to make one.
    m_placeholder = new QWidget;
    auto *ph = new QVBoxLayout(m_placeholder);
    ph->addStretch(2);
    auto *text = new QLabel(QStringLiteral("Плейлистов пока нет"));
    text->setAlignment(Qt::AlignCenter);
    text->setForegroundRole(QPalette::PlaceholderText);
    auto *hint = new QLabel(QStringLiteral("Добавленные файлы попадут в новый плейлист."));
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    auto *create = new QPushButton(QStringLiteral("Новый плейлист"));
    auto *open = new QPushButton(QStringLiteral("Открыть плейлисты…"));
    connect(create, &QPushButton::clicked, this, [this] {
        if (m_create)
            m_create->trigger();
    });
    connect(open, &QPushButton::clicked, this, [this] {
        if (m_open)
            m_open->trigger();
    });
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(create);
    buttons->addWidget(open);
    buttons->addStretch();
    ph->addWidget(text);
    ph->addWidget(hint);
    ph->addSpacing(8);
    ph->addLayout(buttons);
    ph->addStretch(3);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_placeholder);
    rightLayout->addWidget(m_stack, 1);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setChildrenCollapsible(false);   // the active list never vanishes
    m_splitter->addWidget(m_localPane);
    m_splitter->addWidget(m_rightPane);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({450, 450});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    // Both headers as tall as the picker, so the lists start level.
    const int height = m_picker->sizeHint().height() + 2;
    m_localHeader->setFixedHeight(height);
    m_rightHeader->setFixedHeight(height);

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) { focusChanged(now); });
    refreshNames();
    refreshActiveMark();
}

void PlaylistPanes::setPlaylistActions(QAction *create, QAction *open, QAction *rename, QAction *close)
{
    m_create = create;
    m_open = open;
    m_rename = rename;
    m_close = close;
}

void PlaylistPanes::setLocalPage(PlaylistPage *page)
{
    m_local = page;
    static_cast<QVBoxLayout *>(m_localPane->layout())->addWidget(page, 1);
}

void PlaylistPanes::addPlaylist(PlaylistPage *page)
{
    m_playlists.append(page);
    m_stack->addWidget(page);
    if (!m_shown)
        showPlaylist(page);
}

void PlaylistPanes::removePlaylist(PlaylistPage *page)
{
    const int index = m_playlists.indexOf(page);
    if (index < 0)
        return;
    m_playlists.removeAt(index);
    if (page == m_shown) {
        // The one that took its place in the list, else the one before.
        PlaylistPage *next = m_playlists.value(index, m_playlists.value(index - 1));
        const bool wasActive = m_active == Pane::Playlist;
        m_shown = nullptr;
        showPlaylist(next);
        if (wasActive && next)
            activate(Pane::Playlist);
    }
    m_stack->removeWidget(page);
    refreshNames();
}

void PlaylistPanes::movePlaylist(int from, int to)
{
    if (from < 0 || from >= m_playlists.size() || to < 0 || to >= m_playlists.size() || from == to)
        return;
    m_playlists.move(from, to);
    emit orderChanged();
}

void PlaylistPanes::showPlaylist(PlaylistPage *page)
{
    if (page && !m_playlists.contains(page))
        return;
    if (page == m_shown && m_stack->currentWidget() == (page ? static_cast<QWidget *>(page) : m_placeholder))
        return;
    m_shown = page;
    m_stack->setCurrentWidget(page ? static_cast<QWidget *>(page) : m_placeholder);
    refreshNames();
    if (!page && m_active == Pane::Playlist)
        setActivePane(Pane::Local);
    emit shownChanged(page);
    if (m_active == Pane::Playlist)
        emit activeChanged(activePage());   // the same pane, another list
}

void PlaylistPanes::refreshNames()
{
    if (!m_shown) {
        m_picker->setText(QStringLiteral("Плейлисты  ▾"));
        m_picker->setToolTip(QStringLiteral("Плейлистов нет — создайте или откройте плейлист"));
        return;
    }
    const QString name = m_shown->model()->name();
    // A long name must not make the pane wider than the user left it.
    const QString shown = m_picker->fontMetrics().elidedText(name, Qt::ElideRight, 360);
    m_picker->setText(menuText(shown) + QStringLiteral("  ▾"));
    m_picker->setToolTip(QStringLiteral("«%1» — щёлкните, чтобы выбрать другой плейлист").arg(name));
}

void PlaylistPanes::fillMenu()
{
    m_menu->clear();
    for (PlaylistPage *page : std::as_const(m_playlists)) {
        QAction *a = m_menu->addAction(menuText(page->model()->name()));
        a->setCheckable(true);
        a->setChecked(page == m_shown);
        a->setData(QVariant::fromValue<QObject *>(page));
        connect(a, &QAction::triggered, this, [this, page = QPointer<PlaylistPage>(page)] {
            if (!page)
                return;
            showPlaylist(page);
            activate(Pane::Playlist);
        });
    }
    if (!m_playlists.isEmpty())
        m_menu->addSeparator();
    if (m_create)
        m_menu->addAction(m_create);
    if (m_shown) {
        if (m_rename)
            m_menu->addAction(m_rename);
        if (m_close)
            m_menu->addAction(m_close);
    }
}

void PlaylistPanes::showPickerContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    if (m_create)
        menu.addAction(m_create);
    if (m_shown) {
        if (m_rename)
            menu.addAction(m_rename);
        if (m_close)
            menu.addAction(m_close);
        // In place of dragging tabs: the order is the one of the list above
        // (and of the playlists on disk).
        const int index = m_playlists.indexOf(m_shown);
        menu.addSeparator();
        QAction *up = menu.addAction(QStringLiteral("Выше в списке"), this, [this, index] { movePlaylist(index, index - 1); });
        up->setEnabled(index > 0);
        QAction *down = menu.addAction(QStringLiteral("Ниже в списке"), this, [this, index] { movePlaylist(index, index + 1); });
        down->setEnabled(index + 1 < m_playlists.size());
    }
    menu.exec(m_picker->mapToGlobal(pos));
}

PlaylistPanes::Pane PlaylistPanes::activePane() const
{
    return m_active == Pane::Playlist && m_shown ? Pane::Playlist : Pane::Local;
}

PlaylistPage *PlaylistPanes::activePage() const
{
    return activePane() == Pane::Playlist ? m_shown : m_local;
}

PlaylistPage *PlaylistPanes::otherPage() const
{
    return activePane() == Pane::Playlist ? m_local : m_shown;
}

void PlaylistPanes::activate(Pane pane, bool focus)
{
    if (pane == Pane::Playlist && !m_shown)
        pane = Pane::Local;   // no playlist: Local files are the only list
    // Set here, not only through the focus: a window that is not active gets
    // no focus change until it is.
    setActivePane(pane);
    if (focus) {
        if (PlaylistPage *page = activePage())
            page->view()->setFocus();
    }
}

void PlaylistPanes::activatePage(PlaylistPage *page, bool focus)
{
    if (page && page == m_local)
        activate(Pane::Local, focus);
    else if (page && page == m_shown)
        activate(Pane::Playlist, focus);
}

void PlaylistPanes::setActivePane(Pane pane)
{
    if (pane == Pane::Playlist && !m_shown)
        pane = Pane::Local;
    if (pane == m_active)
        return;
    m_active = pane;
    refreshActiveMark();
    emit activeChanged(activePage());
}

void PlaylistPanes::focusChanged(QWidget *now)
{
    // The list itself, not the header: opening the picker is not working in
    // the list (choosing from it activates the pane explicitly).
    if (!now)
        return;
    if (m_local && (now == m_local || m_local->isAncestorOf(now)))
        setActivePane(Pane::Local);
    else if (m_shown && (now == m_shown || m_shown->isAncestorOf(now)))
        setActivePane(Pane::Playlist);
}

void PlaylistPanes::refreshActiveMark()
{
    // The active pane's header: normal text and, in the dark theme, the
    // accent underline the selected tab had (see Theme.cpp). The other one is
    // dimmed. Style sheets do not follow a property change by themselves.
    const QPalette app = QApplication::palette();
    const bool localActive = activePane() == Pane::Local;
    for (QWidget *header : {m_localHeader, m_rightHeader}) {
        const bool on = (header == m_localHeader) == localActive;
        QPalette pal = app;
        if (!on) {
            pal.setColor(QPalette::WindowText, app.color(QPalette::PlaceholderText));
            pal.setColor(QPalette::ButtonText, app.color(QPalette::PlaceholderText));
        }
        header->setPalette(pal);
        header->setProperty("active", on);
        QList<QWidget *> widgets = header->findChildren<QWidget *>();
        widgets << header;
        for (QWidget *w : std::as_const(widgets)) {
            w->style()->unpolish(w);
            w->style()->polish(w);
            w->update();
        }
    }
    if (auto *icon = m_localHeader->findChild<QLabel *>(QStringLiteral("PaneIcon"))) {
        const int size = m_localTitle->fontMetrics().height();
        icon->setPixmap(Icons::make(Icons::Kind::Note, localActive ? Theme::iconColor() : Theme::dimText())
                            .pixmap(size, size));
    }
}
