#pragma once

// Settings > Evolutionary Tracks: this machine's tracks folder and the track sets
// found in it.

#include <QWidget>

namespace astra::evolution::ui {

class TrackFolderWidget;

class TracksSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit TracksSettingsPage(QWidget* parent = nullptr);
    void apply();

private:
    TrackFolderWidget* _folder = nullptr;
};

}  // namespace astra::evolution::ui
