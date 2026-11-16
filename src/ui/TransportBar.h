#pragma once

#include <QWidget>

class QAction;
class QLabel;
class QSlider;
class QToolButton;
class PlaybackController;

// The bar under the playlists: previous / play-pause / next / stop, the
// position with a seek slider, the volume. The buttons show the window's
// actions (also in the menus and the tray); the bar keeps the play action's
// label and icon in step with the player.
class TransportBar : public QWidget
{
    Q_OBJECT
public:
    struct Actions
    {
        QAction *previous = nullptr;
        QAction *play = nullptr;
        QAction *next = nullptr;
        QAction *stop = nullptr;
    };

    TransportBar(PlaybackController *playback, const Actions &actions, QWidget *parent = nullptr);

    // After a theme change.
    void refreshIcons();

    QToolButton *playButton() const { return m_playButton; }
    QSlider *seekSlider() const { return m_seek; }
    QSlider *volumeSlider() const { return m_volume; }
    QLabel *positionLabel() const { return m_position; }
    QLabel *durationLabel() const { return m_duration; }

private:
    void showVolume(int volume);

    PlaybackController *m_playback;
    Actions m_actions;
    QToolButton *m_playButton = nullptr;
    QSlider *m_seek = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_position = nullptr;
    QLabel *m_duration = nullptr;
    QLabel *m_volumeIcon = nullptr;
};
