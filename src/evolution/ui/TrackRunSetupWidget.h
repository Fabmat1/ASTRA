#pragma once

// What a track-fit run uses, shared by the per-star dialog and the bulk dialog:
// the families (a reorderable checklist; the first ticked family is the one a run
// adopts), the subset of the selected family, and the advanced sampler settings.
// The choices are remembered per machine (QSettings "evolution/run/...").

#include "evolution/TrackSetLibrary.h"

#include <QHash>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class FlowLayout;

namespace astra::evolution::ui {

/// Edits the subset of one family (core-mass ticks, a mass range, or compositions
/// with a number of envelopes) and says what the subset gives.
class TrackSubsetWidget : public QWidget {
    Q_OBJECT
public:
    explicit TrackSubsetWidget(QWidget* parent = nullptr);

    void setFamily(const TrackFamilyInfo& family, const TrackSubset& subset);
    TrackSubset subset() const;
    QString familyKey() const { return _family.key; }
    /// Re-check the subset against the tracks (prepare) and show the outcome.
    void setModelSettings(const TrackModelSettings& model);

signals:
    void changed();

private:
    void refreshStatus();
    void clearChecks();

    TrackFamilyInfo _family;
    TrackModelSettings _model;
    bool _updating = false;

    QStackedWidget* _stack = nullptr;
    // Cores
    QWidget* _coresPage = nullptr;
    FlowLayout* _coresFlow = nullptr;
    QList<QCheckBox*> _coreChecks;
    // MassRange
    QDoubleSpinBox* _lo = nullptr;
    QDoubleSpinBox* _hi = nullptr;
    // Groups
    QWidget* _groupsPage = nullptr;
    FlowLayout* _groupsFlow = nullptr;
    QList<QCheckBox*> _groupChecks;
    QSpinBox* _envelopes = nullptr;

    QPushButton* _resetButton = nullptr;
    QLabel* _status = nullptr;
};

class TrackRunSetupWidget : public QWidget {
    Q_OBJECT
public:
    explicit TrackRunSetupWidget(QWidget* parent = nullptr);

    /// Re-read the families of the tracks folder (keeps choices where possible).
    void reloadFamilies();

    /// Ticked families, in the list's order.
    QStringList families() const;
    QHash<QString, TrackSubset> subsets() const;
    TrackModelSettings modelSettings() const;
    /// The family whose subset is shown (for previews); empty if none.
    QString currentFamily() const;
    bool hasUsableFamilies() const { return _usable > 0; }

    /// Remember the current choices for the next run (either dialog).
    void saveChoices() const;

signals:
    void changed();
    void currentFamilyChanged(const QString& key);

private:
    void restoreChoices();
    void onCurrentRowChanged();
    void moveCurrent(int delta);

    QVector<TrackFamilyInfo> _infos;
    QHash<QString, TrackSubset> _subsets;
    int _usable = 0;

    QListWidget* _list = nullptr;
    QToolButton* _up = nullptr;
    QToolButton* _down = nullptr;
    TrackSubsetWidget* _subset = nullptr;
    QLabel* _empty = nullptr;

    QToolButton* _advToggle = nullptr;
    QWidget* _advanced = nullptr;
    QComboBox* _eepPrior = nullptr;
    QCheckBox* _massPrior = nullptr;
    QSpinBox* _walkers = nullptr;
    QSpinBox* _burn = nullptr;
    QSpinBox* _steps = nullptr;
    QLineEdit* _seed = nullptr;
    QComboBox* _hanInterp = nullptr;
};

}  // namespace astra::evolution::ui
