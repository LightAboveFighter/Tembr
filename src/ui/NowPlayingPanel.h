#pragma once

#include <QImage>
#include <QWidget>

class QLabel;
struct Track;

// Square cover that scales with the panel width.
class CoverView : public QWidget
{
    Q_OBJECT
public:
    explicit CoverView(QWidget *parent = nullptr);
    void setImage(const QImage &image);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }
    QSize sizeHint() const override { return {280, 280}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QImage m_image;
    QPixmap m_scaled;
};

// Left column: big cover, title, artist, album, technical line.
class NowPlayingPanel : public QWidget
{
    Q_OBJECT
public:
    explicit NowPlayingPanel(QWidget *parent = nullptr);

    void showTrack(const Track &t, const QImage &cover);
    void clear();

private:
    CoverView *m_cover;
    QLabel *m_title;
    QLabel *m_artist;
    QLabel *m_album;
    QLabel *m_details;
};
