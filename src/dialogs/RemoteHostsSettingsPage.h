#pragma once

#include "models/RemoteHost.h"

#include <QVector>
#include <QWidget>

class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QComboBox;

namespace astra::remote {

/*  Settings page: define remote hosts for grid streaming and remote fitting,
 *  plus the local grid-cache location and size cap.  Edits live on a working
 *  copy; apply() persists through RemoteHostRegistry / AppSettings.         */
class RemoteHostsSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit RemoteHostsSettingsPage(QWidget* parent = nullptr);

    void apply();

private:
    void reloadList(int selectRow = -1);
    void loadEditor(int row);
    void storeEditor();          // editor widgets -> _hosts[_editing]
    void setEditorEnabled(bool on);
    void onAddHost();
    void onRemoveHost();
    void onTestConnection();

    QVector<RemoteHost> _hosts;
    int                 _editing = -1;

    QListWidget*    _list = nullptr;
    QLineEdit*      _nameEdit = nullptr;
    QLineEdit*      _destEdit = nullptr;
    QComboBox*      _typeCombo = nullptr;
    QLineEdit*      _workDirEdit = nullptr;
    QPlainTextEdit* _gridPathsEdit = nullptr;
    QCheckBox*      _streamCheck = nullptr;
    QCheckBox*      _fitCheck = nullptr;
    QGroupBox*      _slurmBox = nullptr;
    QLineEdit*      _partitionEdit = nullptr;
    QLineEdit*      _accountEdit = nullptr;
    QLineEdit*      _timeLimitEdit = nullptr;
    QSpinBox*       _cpusSpin = nullptr;
    QLineEdit*      _memPerCpuEdit = nullptr;
    QPlainTextEdit* _extraSbatchEdit = nullptr;
    QPlainTextEdit* _envSetupEdit = nullptr;
    QLabel*         _bundleLabel = nullptr;
    QPushButton*    _testBtn = nullptr;
    QLabel*         _testResult = nullptr;

    QLineEdit* _cacheDirEdit = nullptr;
    QSpinBox*  _cacheCapSpin = nullptr;
};

} // namespace astra::remote
