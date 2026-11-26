#pragma once

// Folder picker for the evolutionary track sets, with a live list of the families
// found there, for the Settings page.

#include <QWidget>

class QLabel;
class QLineEdit;
class QVBoxLayout;
class QPushButton;

namespace astra::evolution::ui {

class TrackFolderWidget : public QWidget {
    Q_OBJECT
public:
    explicit TrackFolderWidget(QWidget* parent = nullptr);

    QString folder() const;
    void setFolder(const QString& folder);

public slots:
    void rescan();

signals:
    void folderChanged(const QString& folder);

private:
    QLineEdit* _edit = nullptr;
    QPushButton* _browse = nullptr;
    QVBoxLayout* _rows = nullptr;  // one icon + wrapped label per family
    QLabel* _status = nullptr;
    QString _scanned;
};

}  // namespace astra::evolution::ui
