#include "evolution/ui/TrackFolderWidget.h"

#include "app/ui/UiIcons.h"
#include "evolution/TrackSetLibrary.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QPushButton>
#include <QVBoxLayout>

namespace astra::evolution::ui {

TrackFolderWidget::TrackFolderWidget(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto* row = new QHBoxLayout;
    _edit = new QLineEdit;
    _edit->setPlaceholderText(tr("Tracks folder"));
    _edit->setClearButtonEnabled(true);
    _browse = new QPushButton(tr("Browse…"));
    row->addWidget(_edit, 1);
    row->addWidget(_browse);
    lay->addLayout(row);

    _status = new QLabel;
    _status->setWordWrap(true);
    _status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(_status);

    // plain wrapped labels: a list view's word wrap truncates next to an icon
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(120);
    auto* host = new QWidget;
    _rows = new QVBoxLayout(host);
    _rows->setContentsMargins(6, 6, 6, 6);
    _rows->setSpacing(8);
    _rows->addStretch(1);
    scroll->setWidget(host);
    lay->addWidget(scroll, 1);

    connect(_browse, &QPushButton::clicked, this, [this] {
        const QString start = _edit->text().isEmpty() ? QDir::homePath() : _edit->text();
        const QString d = QFileDialog::getExistingDirectory(this, tr("Evolutionary tracks folder"), start);
        if (d.isEmpty()) return;
        _edit->setText(QDir::toNativeSeparators(d));
        rescan();
    });
    connect(_edit, &QLineEdit::editingFinished, this, &TrackFolderWidget::rescan);
    connect(_edit, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.isEmpty()) rescan();  // the clear button
    });
    rescan();
}

QString TrackFolderWidget::folder() const
{
    const QString t = _edit->text().trimmed();
    return t.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(t));
}

void TrackFolderWidget::setFolder(const QString& folder)
{
    _edit->setText(QDir::toNativeSeparators(folder));
    rescan();
}

void TrackFolderWidget::rescan()
{
    const QString dir = folder();
    const bool changed = dir != _scanned;
    _scanned = dir;
    while (_rows->count() > 1) {  // keep the trailing stretch
        QLayoutItem* it = _rows->takeAt(0);
        delete it->widget();
        delete it;
    }
    if (dir.isEmpty()) {
        _status->setText(tr("No folder set."));
    } else if (!QFileInfo(dir).isDir()) {
        _status->setText(tr("This folder does not exist."));
    } else {
        TrackSetLibrary lib(dir);
        const QVector<TrackFamilyInfo> fams = lib.families();
        int usable = 0;
        for (const TrackFamilyInfo& f : fams) {
            auto* row = new QWidget;
            auto* rowLay = new QHBoxLayout(row);
            rowLay->setContentsMargins(0, 0, 0, 0);
            auto* icon = new QLabel;
            icon->setPixmap(UiIcons::icon(f.usable() ? UiIcons::Role::Accept : UiIcons::Role::Dismiss)
                                .pixmap(16, 16));
            icon->setAlignment(Qt::AlignTop);
            auto* text = new QLabel(f.summary());
            text->setWordWrap(true);
            text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            rowLay->addWidget(icon);
            rowLay->addWidget(text, 1);
            QString tip = QDir::toNativeSeparators(f.path);
            if (!f.description.isEmpty()) tip = f.description + "\n" + tip;
            if (!f.sha1.isEmpty()) tip += "\nSHA-1 " + f.sha1;
            if (f.key == QLatin1String("dorman") && f.groups.isEmpty() && f.usable())
                tip += tr("\nNo composition labels");
            row->setToolTip(tip);
            _rows->insertWidget(_rows->count() - 1, row);
            usable += f.usable();
        }
        _status->setText(fams.isEmpty() ? tr("No track sets found.")
                         : usable == 1      ? tr("1 track set")
                                            : tr("%1 track sets").arg(usable));
    }
    if (changed) emit folderChanged(dir);
}

}  // namespace astra::evolution::ui
