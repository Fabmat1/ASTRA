#include "evolution/ui/TracksSettingsPage.h"

#include "evolution/EvolutionSettings.h"
#include "evolution/ui/TrackFolderWidget.h"

#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace astra::evolution::ui {

TracksSettingsPage::TracksSettingsPage(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);

    auto* box = new QGroupBox(tr("Evolutionary tracks folder (this computer)"));
    auto* boxLay = new QVBoxLayout(box);
    _folder = new TrackFolderWidget;
    _folder->setFolder(EvolutionSettings::tracksFolder());
    boxLay->addWidget(_folder);
    outer->addWidget(box, 1);
}

void TracksSettingsPage::apply()
{
    EvolutionSettings::instance().setTracksFolder(_folder->folder());
}

}  // namespace astra::evolution::ui
