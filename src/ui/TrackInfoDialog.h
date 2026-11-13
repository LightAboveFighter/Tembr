#pragma once

#include "core/Track.h"

#include <QDialog>

// Tags, technical info, cover art and the raw tag list of one track.
class TrackInfoDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TrackInfoDialog(const Track &track, QWidget *parent = nullptr);
};
