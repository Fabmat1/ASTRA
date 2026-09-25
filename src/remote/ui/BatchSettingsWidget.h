#pragma once

#include "remote/BatchSettings.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace astra::remote {

struct SlurmLimits;

/*  Editor for one host's BatchSettings: how the mass fitter packs fits into
 *  tasks on that host, the per-tier memory and wall time, and the scheduler
 *  ceilings.  Every value here is site-specific, which is the point of the
 *  widget; the Detect button (Slurm hosts only) asks the page to read the
 *  ceilings off the cluster, and showDetected() takes the answer.           */
class BatchSettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit BatchSettingsWidget(QWidget* parent = nullptr);

    void          setSettings(const BatchSettings& s);
    BatchSettings settings() const;

    /*  Slurm-only rows (array size, submit ceiling, Detect) are hidden for
     *  a plain host, where "concurrent tasks" means parallel runners.       */
    void setSlurm(bool slurm);

    /*  Applies detected limits to the fields and shows what was found.     */
    void showDetected(const SlurmLimits& limits);
    void showDetectError(const QString& message);
    void setDetectBusy(bool busy);

signals:
    void detectRequested();

private:
    void addTierRow(const ResourceTier& t);
    void refreshValidation();

    QCheckBox*    _enabled = nullptr;
    QSpinBox*     _fitsPerTask = nullptr;
    QSpinBox*     _cpusPerTask = nullptr;
    QSpinBox*     _maxArray = nullptr;
    QSpinBox*     _maxConcurrent = nullptr;
    QSpinBox*     _maxQueued = nullptr;
    QSpinBox*     _timeGuard = nullptr;
    QSpinBox*     _retries = nullptr;
    QCheckBox*    _learn = nullptr;
    QTableWidget* _tiers = nullptr;
    QPushButton*  _detectBtn = nullptr;
    QLabel*       _detectResult = nullptr;
    QLabel*       _validation = nullptr;
    QList<QWidget*> _slurmOnly;
};

} // namespace astra::remote
