#include "ui/Theme.h"

#include <QApplication>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

namespace Theme {

namespace {

Kind g_current = Kind::System;
QString g_systemStyleName;   // the platform style picked by Qt at startup

const QColor kAccent(0xFF, 0x8A, 0x00);

// App-wide tweaks on top of the base style:
//  * clicking a slider jumps to that position (seek bar, volume).
class AppStyle : public QProxyStyle
{
public:
    explicit AppStyle(const QString &base)
        : QProxyStyle(QStyleFactory::create(base))
    {
    }

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *ret) const override
    {
        if (hint == QStyle::SH_Slider_AbsoluteSetButtons)
            return Qt::LeftButton;
        return QProxyStyle::styleHint(hint, option, widget, ret);
    }
};

QPalette darkPalette()
{
    const QColor window(0x12, 0x12, 0x12);
    const QColor base(0x17, 0x17, 0x17);
    const QColor alt(0x1C, 0x1C, 0x1C);
    const QColor button(0x1E, 0x1E, 0x1E);
    const QColor text(0xE6, 0xE6, 0xE6);
    const QColor dim(0x8C, 0x8C, 0x8C);
    const QColor disabled(0x5A, 0x5A, 0x5A);
    const QColor onAccent(0x12, 0x12, 0x12);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::ToolTipBase, QColor(0x24, 0x24, 0x24));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, kAccent);
    p.setColor(QPalette::Light, QColor(0x2E, 0x2E, 0x2E));
    p.setColor(QPalette::Midlight, QColor(0x26, 0x26, 0x26));
    p.setColor(QPalette::Mid, QColor(0x1F, 0x1F, 0x1F));
    p.setColor(QPalette::Dark, QColor(0x0A, 0x0A, 0x0A));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, onAccent);
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::LinkVisited, kAccent.darker(120));

    p.setColor(QPalette::Inactive, QPalette::Highlight, kAccent.darker(135));
    p.setColor(QPalette::Inactive, QPalette::HighlightedText, onAccent);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x3A, 0x3A, 0x3A));
    return p;
}

const char *kDarkStyleSheet = R"(
QToolTip { border: 1px solid #333; padding: 4px; }

QSplitter::handle { background: #121212; }

QMenuBar { background: #121212; }
QMenuBar::item:selected { background: #262626; }
QMenu { background: #1a1a1a; border: 1px solid #2c2c2c; padding: 4px 0; }
QMenu::item { padding: 5px 24px 5px 24px; }
QMenu::item:selected { background: #ff8a00; color: #121212; }
QMenu::item:disabled { color: #5a5a5a; }
QMenu::separator { height: 1px; background: #2c2c2c; margin: 4px 8px; }

QLineEdit, QComboBox, QSpinBox {
    background: #1a1a1a; border: 1px solid #2e2e2e; border-radius: 6px; padding: 4px 8px;
    selection-background-color: #ff8a00; selection-color: #121212;
}
QLineEdit:focus, QComboBox:focus { border-color: #ff8a00; }
QKeySequenceEdit QLineEdit { padding: 1px 6px; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #1a1a1a; border: 1px solid #2e2e2e; outline: 0; }

QPushButton {
    background: #222; border: 1px solid #333; border-radius: 6px; padding: 5px 14px;
}
QPushButton:hover { border-color: #ff8a00; }
QPushButton:pressed { background: #2c2c2c; }
QPushButton:default, QPushButton:focus { border-color: #ff8a00; }

/* Keyboard focus shows on the frame (buttons) or the box (check/radio), not
   as a second rectangle drawn around the text. */
QPushButton, QToolButton, QCheckBox, QRadioButton, QSlider, QTabBar, QTabBar::tab { outline: none; }
QTabBar::tab:focus { color: #ffffff; }

QToolButton { border: none; border-radius: 6px; padding: 4px; }
QToolButton:hover { background: #242424; }
QToolButton:pressed { background: #2e2e2e; }

QSlider::groove:horizontal { height: 4px; background: #333; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #ff8a00; border-radius: 2px; }
QSlider::add-page:horizontal { background: #333; border-radius: 2px; }
QSlider::handle:horizontal {
    background: #ff8a00; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: #ffa640; }
QSlider::sub-page:horizontal:disabled { background: #3a3a3a; }
QSlider::handle:horizontal:disabled { background: #3a3a3a; }

QTabWidget::pane { border: none; }
QTabBar::tab {
    background: transparent; color: #9a9a9a; padding: 6px 12px; margin-right: 2px;
    border: none; border-bottom: 2px solid transparent;
}
QTabBar::tab:hover { color: #e6e6e6; }
QTabBar::tab:selected { color: #ffffff; border-bottom: 2px solid #ff8a00; }

QTableView {
    background: #151515; alternate-background-color: #1a1a1a; border: none;
    gridline-color: #222; outline: 0;
}
QTableView::item { padding: 0 4px; border: none; }
QHeaderView::section {
    background: #121212; color: #8c8c8c; border: none; border-bottom: 1px solid #262626;
    padding: 4px 6px;
}
QHeaderView::section:hover { color: #e6e6e6; }
QTableCornerButton::section { background: #121212; border: none; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #3a3a3a; min-height: 30px; border-radius: 4px; margin: 1px; }
QScrollBar::handle:horizontal { background: #3a3a3a; min-width: 30px; border-radius: 4px; margin: 1px; }
QScrollBar::handle:hover { background: #ff8a00; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QStatusBar { color: #8c8c8c; }
QStatusBar::item { border: none; }

QGroupBox { border: 1px solid #2a2a2a; border-radius: 8px; margin-top: 14px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; color: #ff8a00; }

QCheckBox::indicator:checked, QRadioButton::indicator:checked { background: #ff8a00; border: 1px solid #ff8a00; }
QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid #555; background: #1a1a1a; }
QRadioButton::indicator { border-radius: 7px; }
QCheckBox::indicator { border-radius: 3px; }
QCheckBox::indicator:focus, QRadioButton::indicator:focus { border: 1px solid #ff8a00; }

#NowPlayingTitle { color: #ffffff; }
#NowPlayingArtist { color: #ff8a00; }
#NowPlayingDetails { color: #8c8c8c; }
#TransportBar { background: #0e0e0e; border-top: 1px solid #222; }
#TimeLabel { color: #b0b0b0; }
)";

} // namespace

void apply(Kind kind)
{
    if (g_systemStyleName.isEmpty())
        g_systemStyleName = QApplication::style()->name();

    g_current = kind;
    if (kind == Kind::Dark) {
        QApplication::setStyle(new AppStyle(QStringLiteral("Fusion")));
        QApplication::setPalette(darkPalette());
        qApp->setStyleSheet(QString::fromLatin1(kDarkStyleSheet));
    } else {
        auto *style = new AppStyle(g_systemStyleName);
        QApplication::setStyle(style);
        QApplication::setPalette(style->standardPalette());
        qApp->setStyleSheet(QString());
    }
}

Kind current()
{
    return g_current;
}

QString toString(Kind kind)
{
    return kind == Kind::Dark ? QStringLiteral("dark") : QStringLiteral("system");
}

Kind fromString(const QString &s)
{
    return s == QLatin1String("system") ? Kind::System : Kind::Dark;
}

QColor accent()
{
    return kAccent;
}

QColor iconColor()
{
    return g_current == Kind::Dark ? QColor(0xD8, 0xD8, 0xD8) : QApplication::palette().color(QPalette::WindowText);
}

QColor dimText()
{
    return QApplication::palette().color(QPalette::PlaceholderText);
}

} // namespace Theme
