#include "MassFitManagerDialog.h"

#include "db/DatabaseManager.h"
#include "db/SpectrumRepository.h"
#include "models/Star.h"
#include "utils/UiIcons.h"
#include "utils/WheelGuard.h"
#include "utils/WindowSizing.h"
#include "views/tools/MassFitPlanDialog.h"
#include "views/tools/MassFitRuleEditor.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableView>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mf = astra::massfit;

namespace {

/// "~12 min remaining", or nothing when the service has no estimate yet.
QString formatEta(qint64 ms)
{
    if (ms < 0) return QString();
    const qint64 s = ms / 1000;
    if (s < 90)   return QObject::tr("~%1 s remaining").arg(std::max<qint64>(s, 1));
    if (s < 5400) return QObject::tr("~%1 min remaining").arg((s + 30) / 60);
    return QObject::tr("~%1 h remaining").arg((s + 1800) / 3600);
}

/// Blank for the unset sentinel. A campaign's results table is read by eye,
/// and a missing teff printed as "0" is indistinguishable from a fit that
/// really did run away to the bottom of the grid.
QString formatNumber(double v, int decimals)
{
    if (!AsymErr::isSet(v)) return QString();
    return QString::number(v, 'f', decimals);
}

QString formatTriState(int v)
{
    if (v < 0) return QString();
    return v ? QObject::tr("yes") : QObject::tr("no");
}

QString csvField(const QString& s)
{
    QString out = s;
    out.replace(QChar('"'), QStringLiteral("\"\""));
    if (out.contains(',') || out.contains('"') || out.contains('\n')
        || out.contains('\r'))
        return QChar('"') + out + QChar('"');
    return out;
}

QStringList nodeIdsFromJson(const QString& json)
{
    QStringList out;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    if (!d.isArray()) return out;
    for (const auto& v : d.array()) out << v.toString();
    return out;
}

QString starLabelFor(const std::shared_ptr<Star>& s)
{
    if (!s) return QString();
    if (!s->getAlias().isEmpty())    return s->getAlias();
    if (!s->getSourceId().isEmpty()) return s->getSourceId();
    return s->getId();
}

/// True when the two samples are the same stars in the same order. Used only
/// to work out which scope entry the plan editor was left on, so pointer
/// identity is exactly the right test: both vectors hold the project's own
/// Star instances.
bool sameSample(const std::vector<std::shared_ptr<Star>>& a,
                const std::vector<std::shared_ptr<Star>>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].get() != b[i].get()) return false;
    return true;
}

}   // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  MassFitResultsModel
// ═════════════════════════════════════════════════════════════════════════════

MassFitResultsModel::MassFitResultsModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void MassFitResultsModel::setRun(DatabaseManager* dbm, const QString& runId,
                                 const QHash<QString, QString>& starNames)
{
    _dbm       = dbm;
    _runId     = runId;
    _starNames = starNames;
    reload();
}

QString MassFitResultsModel::starIdAt(int row) const
{
    if (row < 0 || row >= int(_rows.size())) return QString();
    return _rows[size_t(row)].starId;
}

void MassFitResultsModel::reload()
{
    beginResetModel();
    _rows.clear();
    _plan = mf::MassFitPlan{};

    if (_dbm && !_runId.isEmpty()) {
        // The run's own snapshot, never the saved plan: a plan edited since
        // the run would rename the setups this run actually used.
        if (const auto run = _dbm->loadMassFitRun(_runId))
            _plan = mf::MassFitPlan::fromJsonString(run->planSnapshotJson);

        QHash<QString, QString> setupByNode;
        for (const auto& n : _plan.nodes) {
            QString name;
            if (const mf::FitSetup* s = _plan.setup(n.setupId)) name = s->name;
            setupByNode.insert(n.id, name.isEmpty() ? n.setupId : name);
        }

        QHash<QString, QVector<MassFitAttemptRow>> attemptsByStar;
        for (const auto& a : _dbm->loadMassFitAttempts(_runId))
            attemptsByStar[a.starId].append(a);

        for (const auto& sr : _dbm->loadMassFitRunStars(_runId)) {
            Row r;
            r.starId   = sr.starId;
            r.starName = _starNames.value(sr.starId, sr.starId);
            r.state    = sr.state;

            const QVector<MassFitAttemptRow>& attempts =
                attemptsByStar[sr.starId];
            r.attempts = attempts.size();

            // The recorded path is authoritative; a star interrupted before
            // its row was rewritten still has its attempts, so those stand in.
            QStringList nodeIds = nodeIdsFromJson(sr.pathJson);
            if (nodeIds.isEmpty())
                for (const auto& a : attempts) nodeIds << a.nodeId;
            QStringList names;
            names.reserve(nodeIds.size());
            for (const QString& id : nodeIds)
                names << setupByNode.value(id, id);
            r.path = names.join(QStringLiteral(" -> "));

            if (!sr.adoptedNodeId.isEmpty()) {
                r.adoptedSetup =
                    setupByNode.value(sr.adoptedNodeId, sr.adoptedNodeId);
                for (const auto& a : attempts) {
                    if (a.nodeId != sr.adoptedNodeId) continue;
                    r.chi2r      = a.chi2r;
                    r.teff       = a.teff;
                    r.logg       = a.logg;
                    r.he         = a.he;
                    r.converged  = a.converged ? 1 : 0;
                    r.atBoundary = a.atBoundary ? 1 : 0;
                }
            }

            r.error = sr.error;
            // Only a failed star borrows its last attempt's message: on a star
            // that finished, an early attempt's error is history, not a result.
            if (r.error.isEmpty() && !attempts.isEmpty()
                && sr.state == QStringLiteral("Failed"))
                r.error = attempts.back().error;

            _rows.push_back(std::move(r));
        }
    }

    endResetModel();
}

int MassFitResultsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(_rows.size());
}

int MassFitResultsModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant MassFitResultsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= int(_rows.size()))
        return {};

    const Row& r = _rows[size_t(index.row())];

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case ColAttempts:
        case ColChi2r:
        case ColTeff:
        case ColLogg:
        case ColHe:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    if (role == Qt::ToolTipRole) {
        switch (index.column()) {
        case ColPath:  return r.path;
        case ColError: return r.error;
        default:       break;
        }
        return {};
    }

    if (role == SortRole) {
        // Numeric columns sort by their number; the unset sentinel returns an
        // invalid variant so blanks group together instead of ranking as zero.
        switch (index.column()) {
        case ColAttempts: return r.attempts;
        case ColChi2r:    return AsymErr::isSet(r.chi2r) ? QVariant(r.chi2r) : QVariant();
        case ColTeff:     return AsymErr::isSet(r.teff)  ? QVariant(r.teff)  : QVariant();
        case ColLogg:     return AsymErr::isSet(r.logg)  ? QVariant(r.logg)  : QVariant();
        case ColHe:       return AsymErr::isSet(r.he)    ? QVariant(r.he)    : QVariant();
        case ColConverged:  return r.converged  < 0 ? QVariant() : QVariant(r.converged);
        case ColAtBoundary: return r.atBoundary < 0 ? QVariant() : QVariant(r.atBoundary);
        default: break;
        }
        role = Qt::DisplayRole;   // strings sort by what they show
    }

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
    case ColStar:         return r.starName;
    case ColState:        return r.state;
    case ColPath:         return r.path;
    case ColAttempts:     return r.attempts;
    case ColAdoptedSetup: return r.adoptedSetup;
    case ColChi2r:        return formatNumber(r.chi2r, 3);
    case ColTeff:         return formatNumber(r.teff, 0);
    case ColLogg:         return formatNumber(r.logg, 3);
    case ColHe:           return formatNumber(r.he, 3);
    case ColConverged:    return formatTriState(r.converged);
    case ColAtBoundary:   return formatTriState(r.atBoundary);
    case ColError:        return r.error;
    default:              return {};
    }
}

QVariant MassFitResultsModel::headerData(int section,
                                         Qt::Orientation orientation,
                                         int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
    case ColStar:         return tr("Star");
    case ColState:        return tr("State");
    case ColPath:         return tr("Path taken");
    case ColAttempts:     return tr("Attempts");
    case ColAdoptedSetup: return tr("Adopted setup");
    case ColChi2r:        return tr("chi2r");
    case ColTeff:         return tr("Teff");
    case ColLogg:         return tr("log g");
    case ColHe:           return tr("log n(He)");
    case ColConverged:    return tr("Converged");
    case ColAtBoundary:   return tr("At boundary");
    case ColError:        return tr("Error");
    default:              return {};
    }
}

QString MassFitResultsModel::toCsv() const
{
    QStringList lines;

    QStringList header;
    for (int c = 0; c < ColumnCount; ++c)
        header << csvField(headerData(c, Qt::Horizontal).toString());
    lines << header.join(QChar(','));

    for (int row = 0; row < int(_rows.size()); ++row) {
        QStringList cells;
        for (int c = 0; c < ColumnCount; ++c)
            cells << csvField(data(index(row, c), Qt::DisplayRole).toString());
        lines << cells.join(QChar(','));
    }

    return lines.join(QStringLiteral("\n")) + QStringLiteral("\n");
}

// ═════════════════════════════════════════════════════════════════════════════
//  MassFitManagerDialog
// ═════════════════════════════════════════════════════════════════════════════

MassFitManagerDialog::MassFitManagerDialog(
    MassFitService* service, DatabaseManager* dbm, const QString& projectId,
    std::vector<std::shared_ptr<Star>> allStars,
    std::vector<std::shared_ptr<Star>> filteredStars,
    std::vector<std::shared_ptr<Star>> selectedStars, QWidget* parent)
    : QDialog(parent)
    , _service(service)
    , _dbm(dbm)
    , _projectId(projectId)
    , _allStars(std::move(allStars))
    , _filteredStars(std::move(filteredStars))
    , _selectedStars(std::move(selectedStars))
{
    setWindowTitle(tr("Mass Spectrum Fitting"));

    auto* root = new QVBoxLayout(this);

    _tabs = new QTabWidget(this);
    _tabs->addTab(buildPlansTab(),   tr("Plans"));
    _tabs->addTab(buildRunsTab(),    tr("Runs"));
    _tabs->addTab(buildResultsTab(), tr("Results"));
    connect(_tabs, &QTabWidget::currentChanged, this, [this](int) {
        // Results are only re-read when they are actually on screen: a
        // campaign of several hundred stars is two queries per refresh.
        if (_tabs->currentIndex() == 2) reloadResults();
    });
    root->addWidget(_tabs, 1);

    auto* closeBtn = new QPushButton(tr("Close"), this);
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(closeBtn);
    root->addLayout(bottom);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    if (_service) {
        connect(_service, &MassFitService::runsChanged,
                this, &MassFitManagerDialog::rebuildRunList);
        connect(_service, &MassFitService::runLogUpdated,
                this, &MassFitManagerDialog::onRunLogUpdated);
        connect(_service, &MassFitService::progressChanged, this,
                [this](int, int, int) { refreshRunDetail(); });
        connect(_service, &MassFitService::starFinished, this,
                [this](const QString& runId, const QString&) {
                    refreshRunDetail();
                    if (_tabs->currentIndex() == 2
                        && _resultsModel->runId() == runId)
                        reloadResults();
                });
        connect(_service, &MassFitService::allFinished, this,
                [this](int, int) {
                    rebuildRunList();
                    if (_tabs->currentIndex() == 2) reloadResults();
                });
    }

    _ticker = new QTimer(this);
    _ticker->setInterval(1000);
    connect(_ticker, &QTimer::timeout, this,
            &MassFitManagerDialog::refreshRunDetail);
    _ticker->start();

    rebuildPlanList();
    rebuildRunList();

    resize(1080, 700);
    astra::blockWheelScrollingRecursive(this);
    WindowSizing::fitToScreen(this);
}

void MassFitManagerDialog::setStarSamples(
    std::vector<std::shared_ptr<Star>> allStars,
    std::vector<std::shared_ptr<Star>> filteredStars,
    std::vector<std::shared_ptr<Star>> selectedStars)
{
    _allStars      = std::move(allStars);
    _filteredStars = std::move(filteredStars);
    _selectedStars = std::move(selectedStars);
}

void MassFitManagerDialog::showRunsTab()
{
    if (_tabs) _tabs->setCurrentIndex(1);
}

// ── Tab 1: plans ─────────────────────────────────────────────────────────────

QWidget* MassFitManagerDialog::buildPlansTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    auto* hint = new QLabel(
        tr("A plan holds the fit regions per instrument mode, the named fit "
           "setups and the decision tree that chooses between them. Running a "
           "plan walks that tree once per star, unattended."),
        page);
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* split = new QSplitter(Qt::Horizontal, page);

    _planList = new QListWidget;
    _planList->setMinimumWidth(280);
    split->addWidget(_planList);

    _planDetail = new QPlainTextEdit;
    _planDetail->setReadOnly(true);
    _planDetail->setLineWrapMode(QPlainTextEdit::NoWrap);
    split->addWidget(_planDetail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    auto* row = new QHBoxLayout;
    _planNewBtn = new QPushButton(tr("New"));
    UiIcons::apply(_planNewBtn, UiIcons::Role::TransferAdd);
    _planEditBtn = new QPushButton(tr("Edit..."));
    UiIcons::apply(_planEditBtn, UiIcons::Role::Edit);
    _planDupBtn = new QPushButton(tr("Duplicate"));
    _planDelBtn = new QPushButton(tr("Delete"));
    UiIcons::apply(_planDelBtn, UiIcons::Role::Remove);
    _planRunBtn = new QPushButton(tr("Run..."));
    UiIcons::apply(_planRunBtn, UiIcons::Role::Run);

    row->addWidget(_planNewBtn);
    row->addWidget(_planEditBtn);
    row->addWidget(_planDupBtn);
    row->addWidget(_planDelBtn);
    row->addStretch(1);
    row->addWidget(_planRunBtn);
    v->addLayout(row);

    connect(_planNewBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onNewPlan);
    connect(_planEditBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onEditPlan);
    connect(_planDupBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onDuplicatePlan);
    connect(_planDelBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onDeletePlan);
    connect(_planRunBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onRunPlan);
    connect(_planList, &QListWidget::itemSelectionChanged, this,
            &MassFitManagerDialog::onPlanSelectionChanged);
    connect(_planList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { onEditPlan(); });

    return page;
}

QString MassFitManagerDialog::selectedPlanId() const
{
    auto* item = _planList ? _planList->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void MassFitManagerDialog::rebuildPlanList()
{
    if (!_planList) return;

    const QString keep = selectedPlanId();
    QSignalBlocker block(_planList);
    _planList->clear();

    if (_dbm) {
        for (const MassFitPlanRow& row : _dbm->loadMassFitPlans(_projectId)) {
            const mf::MassFitPlan p =
                mf::MassFitPlan::fromJsonString(row.configJson);
            const QString name =
                row.name.isEmpty() ? tr("(unnamed plan)") : row.name;
            auto* item = new QListWidgetItem(
                tr("%1  -  %2 setup(s), %3 node(s)")
                    .arg(name)
                    .arg(p.setups.size())
                    .arg(p.nodes.size()));
            item->setData(Qt::UserRole, row.id);
            _planList->addItem(item);
            if (row.id == keep) _planList->setCurrentItem(item);
        }
    }
    if (!_planList->currentItem() && _planList->count() > 0)
        _planList->setCurrentRow(0);

    block.unblock();
    onPlanSelectionChanged();
}

void MassFitManagerDialog::onPlanSelectionChanged()
{
    const QString id = selectedPlanId();
    const bool has = !id.isEmpty();

    _planEditBtn->setEnabled(has);
    _planDupBtn->setEnabled(has);
    _planDelBtn->setEnabled(has);
    _planRunBtn->setEnabled(has);

    if (!has) {
        _planDetail->clear();
        return;
    }

    mf::MassFitPlan plan;
    if (!loadPlan(id, &plan)) {
        _planDetail->setPlainText(tr("This plan could not be read back."));
        return;
    }

    QStringList text;
    text << tr("Join mode: %1")
                .arg(plan.joinMode == mf::JoinMode::Simultaneous
                         ? tr("simultaneous (one fit per star)")
                         : tr("individual (one fit per spectrum)"));
    text << tr("Adoption: %1").arg(mf::adoptionToString(plan.adoption));
    text << tr("Parallel stars: %1").arg(plan.parallelStars);
    text << tr("Enabled modes: %1")
                .arg(std::count_if(plan.modes.begin(), plan.modes.end(),
                                   [](const mf::ModeRegionConfig& m) {
                                       return m.enabled;
                                   }));
    text << QString();
    text << mf::describeTree(plan);

    const QStringList problems = MassFitService::validateForRun(plan);
    if (!problems.isEmpty()) {
        text << QString();
        text << tr("Problems that stop this plan from running:");
        for (const QString& p : problems) text << QStringLiteral("  - ") + p;
    }

    _planDetail->setPlainText(text.join(QChar('\n')));
}

bool MassFitManagerDialog::loadPlan(const QString& planId,
                                    mf::MassFitPlan* out,
                                    MassFitPlanRow* rowOut) const
{
    if (!_dbm || planId.isEmpty()) return false;
    for (const MassFitPlanRow& row : _dbm->loadMassFitPlans(_projectId)) {
        if (row.id != planId) continue;
        if (out) *out = mf::MassFitPlan::fromJsonString(row.configJson);
        if (rowOut) *rowOut = row;
        return true;
    }
    return false;
}

bool MassFitManagerDialog::storePlan(const mf::MassFitPlan& plan)
{
    if (!_dbm) return false;

    // INSERT OR REPLACE rewrites the whole row, so created_at has to be
    // carried over by hand or every save would look like a new plan.
    MassFitPlanRow existing;
    const bool known = loadPlan(plan.id, nullptr, &existing);

    MassFitPlanRow row;
    row.id         = plan.id;
    row.projectId  = _projectId;
    row.name       = plan.name;
    row.createdAt  = known && !existing.createdAt.isEmpty()
                         ? existing.createdAt
                         : QDateTime::currentDateTime().toString(Qt::ISODate);
    row.updatedAt  = QDateTime::currentDateTime().toString(Qt::ISODate);
    row.configJson = plan.toJsonString();
    return _dbm->saveMassFitPlan(row);
}

void MassFitManagerDialog::editPlan(mf::MassFitPlan plan, const QString& title)
{
    if (plan.projectId.isEmpty()) plan.projectId = _projectId;

    MassFitPlanDialog dlg(_allStars, _filteredStars, _selectedStars, _dbm,
                          _projectId, plan, this);
    dlg.setWindowTitle(title);
    if (dlg.exec() != QDialog::Accepted) return;

    const mf::MassFitPlan edited = dlg.plan();
    if (!storePlan(edited)) {
        QMessageBox::warning(this, tr("Mass Spectrum Fitting"),
                             tr("The plan could not be saved."));
        return;
    }

    // Remember the run-time choices the editor was left on, so pressing Run
    // straight afterwards offers back what was just configured rather than
    // the defaults. Neither is part of the plan itself.
    RunChoices choices;
    const auto& scope = dlg.scopeStars();
    choices.scope = sameSample(scope, _selectedStars) ? 2
                    : sameSample(scope, _filteredStars) ? 1
                                                        : 0;
    choices.existing      = dlg.existingFitPolicy();
    choices.poorQuality   = dlg.poorQualityRule();
    choices.parallelStars = std::max(1, edited.parallelStars);
    _lastChoices.insert(edited.id, choices);

    rebuildPlanList();
    for (int i = 0; i < _planList->count(); ++i) {
        if (_planList->item(i)->data(Qt::UserRole).toString() != edited.id)
            continue;
        _planList->setCurrentRow(i);
        break;
    }
}

void MassFitManagerDialog::onNewPlan()
{
    mf::MassFitPlan plan;
    plan.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    plan.projectId = _projectId;
    plan.name      = tr("New plan");
    editPlan(std::move(plan), tr("New mass fitting plan"));
}

void MassFitManagerDialog::onEditPlan()
{
    mf::MassFitPlan plan;
    if (!loadPlan(selectedPlanId(), &plan)) return;
    editPlan(std::move(plan), tr("Edit mass fitting plan"));
}

void MassFitManagerDialog::onDuplicatePlan()
{
    mf::MassFitPlan plan;
    if (!loadPlan(selectedPlanId(), &plan)) return;

    plan.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    plan.name = tr("%1 (copy)").arg(plan.name);
    if (!storePlan(plan)) {
        QMessageBox::warning(this, tr("Mass Spectrum Fitting"),
                             tr("The copy could not be saved."));
        return;
    }
    rebuildPlanList();
}

void MassFitManagerDialog::onDeletePlan()
{
    const QString id = selectedPlanId();
    mf::MassFitPlan plan;
    if (!loadPlan(id, &plan)) return;

    const auto answer = QMessageBox::question(
        this, tr("Delete Plan"),
        tr("Delete the plan \"%1\"?\n\nRuns already made from it keep their "
           "own snapshot of the plan and are not affected.")
            .arg(plan.name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    if (_dbm) _dbm->deleteMassFitPlan(id);
    _lastChoices.remove(id);
    rebuildPlanList();
}

void MassFitManagerDialog::onRunPlan()
{
    if (!_service) return;

    mf::MassFitPlan plan;
    if (!loadPlan(selectedPlanId(), &plan)) return;

    const QStringList problems = MassFitService::validateForRun(plan);
    if (!problems.isEmpty()) {
        QMessageBox::warning(
            this, tr("Cannot Run Plan"),
            tr("\"%1\" cannot be run yet:\n\n%2")
                .arg(plan.name,
                     QStringLiteral("  - ")
                         + problems.join(QStringLiteral("\n  - "))));
        return;
    }

    const RunChoices last = _lastChoices.value(plan.id, RunChoices{});
    MassFitRunConfirmDialog confirm(
        plan, _dbm, _allStars, _filteredStars, _selectedStars, last.scope,
        last.existing, last.poorQuality,
        _lastChoices.contains(plan.id) ? last.parallelStars
                                       : std::max(1, plan.parallelStars),
        this);
    if (confirm.exec() != QDialog::Accepted) return;

    const auto& stars = starsForScope(confirm.scope());
    if (stars.empty()) {
        QMessageBox::information(this, tr("Mass Spectrum Fitting"),
                                 tr("The chosen sample has no stars."));
        return;
    }

    RunChoices chosen;
    chosen.scope         = confirm.scope();
    chosen.existing      = confirm.existingFitPolicy();
    chosen.poorQuality   = confirm.poorQualityRule();
    chosen.parallelStars = confirm.parallelStars();
    _lastChoices.insert(plan.id, chosen);

    MassFitService::RunOptions options;
    options.existing      = chosen.existing;
    options.poorQuality   = chosen.poorQuality;
    options.parallelStars = chosen.parallelStars;

    // The plan's own parallelism is what the run honours downstream, so the
    // confirmation's value has to reach it too rather than only the options.
    plan.parallelStars = chosen.parallelStars;

    const QString runId = _service->startRun(stars, _projectId, plan, options);
    if (runId.isEmpty()) {
        QMessageBox::warning(this, tr("Mass Spectrum Fitting"),
                             tr("The run could not be started. See the "
                                "application log for details."));
        return;
    }

    rebuildRunList();
    for (int i = 0; i < _runList->count(); ++i) {
        if (_runList->item(i)->data(Qt::UserRole).toString() != runId) continue;
        _runList->setCurrentRow(i);
        break;
    }
    _tabs->setCurrentIndex(1);
}

// ── Tab 2: runs ──────────────────────────────────────────────────────────────

QWidget* MassFitManagerDialog::buildRunsTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    // Non-intrusive: a banner rather than a popup on open. A run left behind
    // by a previous session is information, not a question that has to be
    // answered before the window can be used.
    _resumeBanner = new QLabel(page);
    _resumeBanner->setWordWrap(true);
    _resumeBanner->setVisible(false);
    v->addWidget(_resumeBanner);

    auto* split = new QSplitter(Qt::Horizontal, page);

    _runList = new QListWidget;
    _runList->setMinimumWidth(300);
    split->addWidget(_runList);

    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);

    _runSummary = new QLabel(right);
    _runSummary->setWordWrap(true);
    rv->addWidget(_runSummary);

    _runProgress = new QProgressBar(right);
    _runProgress->setRange(0, 100);
    _runProgress->setValue(0);
    rv->addWidget(_runProgress);

    _runLog = new QPlainTextEdit(right);
    _runLog->setReadOnly(true);
    _runLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    rv->addWidget(_runLog, 1);

    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    auto* row = new QHBoxLayout;
    _pauseBtn  = new QPushButton(tr("Pause"));
    _resumeBtn = new QPushButton(tr("Resume"));
    UiIcons::apply(_resumeBtn, UiIcons::Role::Run);
    _cancelBtn    = new QPushButton(tr("Cancel"));
    UiIcons::apply(_cancelBtn, UiIcons::Role::Dismiss);
    _cancelAllBtn = new QPushButton(tr("Cancel All"));
    row->addWidget(_pauseBtn);
    row->addWidget(_resumeBtn);
    row->addStretch(1);
    row->addWidget(_cancelBtn);
    row->addWidget(_cancelAllBtn);
    v->addLayout(row);

    connect(_pauseBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onPauseRun);
    connect(_resumeBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onResumeRun);
    connect(_cancelBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onCancelRun);
    connect(_cancelAllBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onCancelAllRuns);
    connect(_runList, &QListWidget::itemSelectionChanged, this,
            &MassFitManagerDialog::onRunSelectionChanged);

    return page;
}

QString MassFitManagerDialog::selectedRunId() const
{
    auto* item = _runList ? _runList->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

bool MassFitManagerDialog::runEntry(const QString& id, RunEntry* out) const
{
    if (!out || !_service || id.isEmpty()) return false;

    bool live = false;
    const MassFitService::RunInfo info = _service->runInfo(id, &live);
    if (live) {
        RunEntry e;
        e.id         = id;
        e.planName   = info.planName.isEmpty() ? tr("(unnamed plan)")
                                               : info.planName;
        e.stateLabel = MassFitService::runStateLabel(info.state);
        e.total      = info.starTotal;
        e.done       = info.starDone;
        e.failed     = info.starFailed;
        e.running    = info.starRunning;
        e.etaMs      = info.etaMs;
        e.live       = true;
        e.active     = !MassFitService::isTerminal(info.state);
        // `resumable` means "a previous session left this behind", which a
        // live run never is. A paused one is picked back up through `paused`.
        e.paused     = info.state == MassFitService::RunState::Paused;
        *out = std::move(e);
        return true;
    }

    for (const RunEntry& e : collectRuns()) {
        if (e.id != id) continue;
        *out = e;
        return true;
    }
    return false;
}

std::vector<MassFitManagerDialog::RunEntry>
MassFitManagerDialog::collectRuns() const
{
    std::vector<RunEntry> out;
    if (!_dbm || !_service) return out;

    QSet<QString> resumable;
    for (const QString& id : _service->resumableRuns(_projectId))
        resumable.insert(id);

    // The database is the list; the service supplies live progress for the
    // runs it happens to be executing. That way a run from a previous session
    // appears in the same list as a running one, which is what makes Resume
    // reachable without a modal prompt on open.
    for (const MassFitRunRow& row : _dbm->loadMassFitRuns(_projectId)) {
        RunEntry e;
        e.id = row.id;

        bool live = false;
        const MassFitService::RunInfo info = _service->runInfo(row.id, &live);
        e.live = live;
        if (live) {
            e.planName   = info.planName;
            e.stateLabel = MassFitService::runStateLabel(info.state);
            e.total      = info.starTotal;
            e.done       = info.starDone;
            e.failed     = info.starFailed;
            e.running    = info.starRunning;
            e.etaMs      = info.etaMs;
            e.active     = !MassFitService::isTerminal(info.state);
            e.paused     = info.state == MassFitService::RunState::Paused;
        } else {
            e.stateLabel = row.state;
            e.total      = row.starTotal;
            e.done       = row.starDone;
            e.failed     = row.starFailed;
            e.active     = false;
            const mf::MassFitPlan snap =
                mf::MassFitPlan::fromJsonString(row.planSnapshotJson);
            e.planName = snap.name;
        }
        if (e.planName.isEmpty()) e.planName = tr("(unnamed plan)");
        e.resumable = resumable.contains(row.id);
        out.push_back(std::move(e));
    }
    return out;
}

void MassFitManagerDialog::rebuildRunList()
{
    if (!_runList) return;

    const QString keep = selectedRunId();
    const auto entries = collectRuns();

    QSignalBlocker block(_runList);
    _runList->clear();

    int nResumable = 0;
    for (const RunEntry& e : entries) {
        if (e.resumable) ++nResumable;
        auto* item = new QListWidgetItem(
            tr("%1  -  %2  -  %3/%4 stars%5")
                .arg(e.planName, e.stateLabel)
                .arg(e.done)
                .arg(e.total)
                .arg(e.failed > 0 ? tr(", %1 failed").arg(e.failed)
                                  : QString()));
        item->setData(Qt::UserRole, e.id);
        _runList->addItem(item);
        if (e.id == keep) _runList->setCurrentItem(item);
    }
    if (!_runList->currentItem() && _runList->count() > 0)
        _runList->setCurrentRow(0);

    if (nResumable > 0) {
        _resumeBanner->setText(
            tr("%1 run(s) were left unfinished by an earlier session. Select "
               "one and press Resume to carry on from the stars it did not "
               "reach.")
                .arg(nResumable));
        _resumeBanner->setVisible(true);
    } else {
        _resumeBanner->setVisible(false);
    }

    block.unblock();
    onRunSelectionChanged();
}

void MassFitManagerDialog::onRunSelectionChanged()
{
    const QString id = selectedRunId();
    if (id != _shownLogRunId) {
        _shownLogRunId = id;
        _runLog->setPlainText(
            id.isEmpty() ? QString()
                         : QString::fromUtf8(_service->runLog(id)));
        _runLog->moveCursor(QTextCursor::End);
    }
    refreshRunDetail();
    if (_tabs && _tabs->currentIndex() == 2) reloadResults();
}

void MassFitManagerDialog::onRunLogUpdated(const QString& runId)
{
    if (runId != selectedRunId()) return;
    _runLog->setPlainText(QString::fromUtf8(_service->runLog(runId)));
    _runLog->moveCursor(QTextCursor::End);
}

void MassFitManagerDialog::refreshRunDetail()
{
    const QString id = selectedRunId();
    if (id.isEmpty()) {
        _runSummary->clear();
        _runProgress->setRange(0, 100);
        _runProgress->setValue(0);
        _pauseBtn->setEnabled(false);
        _resumeBtn->setEnabled(false);
        _cancelBtn->setEnabled(false);
        _cancelAllBtn->setEnabled(_service && _service->hasActiveRuns());
        return;
    }

    RunEntry entry;
    if (!runEntry(id, &entry)) {
        _runSummary->setText(tr("This run is no longer available."));
        return;
    }

    QStringList parts;
    parts << tr("%1  -  %2").arg(entry.planName, entry.stateLabel);
    parts << tr("%1 of %2 stars done").arg(entry.done).arg(entry.total);
    if (entry.failed > 0) parts << tr("%1 failed").arg(entry.failed);
    if (entry.running > 0) parts << tr("%1 running").arg(entry.running);
    const QString eta = formatEta(entry.etaMs);
    if (!eta.isEmpty()) parts << eta;
    _runSummary->setText(parts.join(QStringLiteral("  -  ")));

    _runProgress->setRange(0, std::max(1, entry.total));
    _runProgress->setValue(entry.done);

    _pauseBtn->setEnabled(entry.active && !entry.paused);
    // Resume covers both halves of the same idea: unpausing a live run and
    // picking a persisted one back up.
    _resumeBtn->setEnabled(entry.resumable || entry.paused);
    _cancelBtn->setEnabled(entry.active);
    _cancelAllBtn->setEnabled(_service && _service->hasActiveRuns());
}

void MassFitManagerDialog::onPauseRun()
{
    const QString id = selectedRunId();
    if (id.isEmpty() || !_service) return;
    _service->pauseRun(id);
    rebuildRunList();
}

void MassFitManagerDialog::onResumeRun()
{
    const QString id = selectedRunId();
    if (id.isEmpty() || !_service) return;
    if (_service->resumeRun(id).isEmpty()) {
        QMessageBox::information(
            this, tr("Resume Run"),
            tr("This run could not be resumed. It may already be finished, or "
               "its stars may no longer be in the project."));
    }
    rebuildRunList();
}

void MassFitManagerDialog::onCancelRun()
{
    const QString id = selectedRunId();
    if (id.isEmpty() || !_service) return;

    const auto answer = QMessageBox::question(
        this, tr("Cancel Run"),
        tr("Cancel this run?\n\nStars that have not started are dropped and "
           "the fits in flight are asked to stop. Everything already fitted "
           "is kept."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    _service->cancelRun(id);
    rebuildRunList();
}

void MassFitManagerDialog::onCancelAllRuns()
{
    if (!_service || !_service->hasActiveRuns()) return;

    const auto answer = QMessageBox::question(
        this, tr("Cancel All Runs"),
        tr("Cancel every active run?\n\nEverything already fitted is kept."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    _service->cancelAll();
    rebuildRunList();
}

// ── Tab 3: results ───────────────────────────────────────────────────────────

QWidget* MassFitManagerDialog::buildResultsTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    _resultsHeader = new QLabel(
        tr("Select a run on the Runs tab to see its per-star results."), page);
    _resultsHeader->setWordWrap(true);
    v->addWidget(_resultsHeader);

    _resultsModel = new MassFitResultsModel(this);
    _resultsProxy = new QSortFilterProxyModel(this);
    _resultsProxy->setSourceModel(_resultsModel);
    _resultsProxy->setSortRole(MassFitResultsModel::SortRole);
    _resultsProxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    _resultsTable = new QTableView(page);
    _resultsTable->setModel(_resultsProxy);
    _resultsTable->setSortingEnabled(true);
    _resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    _resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _resultsTable->setAlternatingRowColors(true);
    _resultsTable->verticalHeader()->setVisible(false);
    _resultsTable->horizontalHeader()->setStretchLastSection(true);
    v->addWidget(_resultsTable, 1);

    auto* row = new QHBoxLayout;
    auto* hint = new QLabel(
        tr("Double-click a star to open its spectra and fits, where the "
           "adopted fit can be inspected or overridden."),
        page);
    hint->setWordWrap(true);
    row->addWidget(hint, 1);
    _resultsExportBtn = new QPushButton(tr("Export Results as CSV..."), page);
    _resultsExportBtn->setEnabled(false);
    row->addWidget(_resultsExportBtn);
    v->addLayout(row);

    connect(_resultsTable, &QTableView::doubleClicked, this,
            &MassFitManagerDialog::onResultDoubleClicked);
    connect(_resultsExportBtn, &QPushButton::clicked, this,
            &MassFitManagerDialog::onExportResults);

    return page;
}

QHash<QString, QString> MassFitManagerDialog::starNames() const
{
    QHash<QString, QString> names;
    const auto add = [&names](const std::vector<std::shared_ptr<Star>>& v) {
        for (const auto& s : v) {
            if (!s || s->getId().isEmpty()) continue;
            names.insert(s->getId(), starLabelFor(s));
        }
    };
    add(_allStars);
    add(_filteredStars);
    add(_selectedStars);
    return names;
}

void MassFitManagerDialog::reloadResults()
{
    const QString id = selectedRunId();
    if (id.isEmpty()) {
        _resultsModel->setRun(_dbm, QString(), {});
        _resultsHeader->setText(
            tr("Select a run on the Runs tab to see its per-star results."));
        _resultsExportBtn->setEnabled(false);
        return;
    }

    _resultsModel->setRun(_dbm, id, starNames());
    _resultsExportBtn->setEnabled(_resultsModel->rowCount() > 0);

    RunEntry entry;
    const QString planName = runEntry(id, &entry) ? entry.planName : QString();
    _resultsHeader->setText(tr("Results for \"%1\": %2 star(s).")
                                .arg(planName)
                                .arg(_resultsModel->rowCount()));
    _resultsTable->resizeColumnsToContents();
}

void MassFitManagerDialog::onResultDoubleClicked(const QModelIndex& proxyIndex)
{
    if (!proxyIndex.isValid()) return;
    const QModelIndex src = _resultsProxy->mapToSource(proxyIndex);
    const QString starId = _resultsModel->starIdAt(src.row());
    if (!starId.isEmpty()) emit starDrillDownRequested(starId);
}

void MassFitManagerDialog::onExportResults()
{
    if (_resultsModel->rowCount() == 0) return;

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Results"),
        QStringLiteral("mass_fit_results.csv"),
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export Results"),
                             tr("Could not write to %1.").arg(path));
        return;
    }
    QTextStream out(&file);
    out << _resultsModel->toCsv();
    out.flush();
    if (!file.commit()) {
        QMessageBox::warning(this, tr("Export Results"),
                             tr("Could not write to %1.").arg(path));
        return;
    }
    QMessageBox::information(this, tr("Export Results"),
                             tr("Wrote %1 row(s) to %2.")
                                 .arg(_resultsModel->rowCount())
                                 .arg(path));
}

const std::vector<std::shared_ptr<Star>>&
MassFitManagerDialog::starsForScope(int scope) const
{
    switch (scope) {
    case 1: if (!_filteredStars.empty()) return _filteredStars; break;
    case 2: if (!_selectedStars.empty()) return _selectedStars; break;
    default: break;
    }
    return _allStars;
}

// ═════════════════════════════════════════════════════════════════════════════
//  MassFitRunConfirmDialog
// ═════════════════════════════════════════════════════════════════════════════

MassFitRunConfirmDialog::MassFitRunConfirmDialog(
    const mf::MassFitPlan& plan, DatabaseManager* dbm,
    const std::vector<std::shared_ptr<Star>>& allStars,
    const std::vector<std::shared_ptr<Star>>& filteredStars,
    const std::vector<std::shared_ptr<Star>>& selectedStars, int initialScope,
    mf::ExistingFitPolicy initialPolicy, mf::RuleGroup poorQuality,
    int initialParallel, QWidget* parent)
    : QDialog(parent)
    , _plan(plan)
    , _dbm(dbm)
    , _allStars(allStars)
    , _filteredStars(filteredStars)
    , _selectedStars(selectedStars)
    , _poorQuality(std::move(poorQuality))
{
    setWindowTitle(tr("Run Mass Fitting Plan"));
    setModal(true);

    auto* v = new QVBoxLayout(this);

    auto* title = new QLabel(tr("About to run \"%1\".").arg(plan.name), this);
    title->setWordWrap(true);
    v->addWidget(title);

    auto* form = new QFormLayout;

    _scopeCombo = new QComboBox(this);
    _scopeCombo->addItem(tr("All project stars (%1)").arg(allStars.size()), 0);
    _scopeCombo->addItem(tr("Filtered stars (%1)").arg(filteredStars.size()), 1);
    _scopeCombo->addItem(tr("Selected stars (%1)").arg(selectedStars.size()), 2);
    if (auto* model = qobject_cast<QStandardItemModel*>(_scopeCombo->model())) {
        if (filteredStars.empty()) model->item(1)->setEnabled(false);
        if (selectedStars.empty()) model->item(2)->setEnabled(false);
    }
    int scope = initialScope;
    if (scope == 1 && filteredStars.empty()) scope = 0;
    if (scope == 2 && selectedStars.empty()) scope = 0;
    _scopeCombo->setCurrentIndex(scope);
    form->addRow(tr("Stars:"), _scopeCombo);

    _existingCombo = new QComboBox(this);
    _existingCombo->addItem(tr("Add new fits (keep what is there)"),
                            int(mf::ExistingFitPolicy::AddNew));
    _existingCombo->addItem(tr("Skip stars that already have a spectral fit"),
                            int(mf::ExistingFitPolicy::SkipFitted));
    _existingCombo->addItem(tr("Refit only where the current best fit is poor"),
                            int(mf::ExistingFitPolicy::RefitPoor));
    _existingCombo->setCurrentIndex(
        _existingCombo->findData(int(initialPolicy)));
    form->addRow(tr("Existing fits:"), _existingCombo);

    _poorQualityBtn = new QPushButton(tr("Edit \"poor fit\" rule..."), this);
    UiIcons::apply(_poorQualityBtn, UiIcons::Role::Edit);
    _poorQualityBtn->setEnabled(initialPolicy
                                == mf::ExistingFitPolicy::RefitPoor);
    connect(_poorQualityBtn, &QPushButton::clicked, this, [this] {
        MassFitRuleEditor dlg(_poorQuality, tr("Poor fit rule"), this);
        if (dlg.exec() == QDialog::Accepted) _poorQuality = dlg.rule();
    });
    connect(_existingCombo, &QComboBox::currentIndexChanged, this, [this] {
        _poorQualityBtn->setEnabled(
            _existingCombo->currentData().toInt()
            == int(mf::ExistingFitPolicy::RefitPoor));
    });
    form->addRow(QString(), _poorQualityBtn);

    _parallelSpin = new QSpinBox(this);
    _parallelSpin->setRange(1, 64);
    _parallelSpin->setValue(std::max(1, initialParallel));
    _parallelSpin->setToolTip(
        tr("How many stars are fitted at the same time. Each running fit holds "
           "its own model grid in memory, so this multiplies peak memory as "
           "well as speed."));
    form->addRow(tr("Parallel stars:"), _parallelSpin);

    v->addLayout(form);

    _summaryLabel = new QLabel(this);
    _summaryLabel->setWordWrap(true);
    v->addWidget(_summaryLabel);

    auto* bb = new QDialogButtonBox(this);
    auto* runBtn = bb->addButton(tr("Start Run"), QDialogButtonBox::AcceptRole);
    UiIcons::apply(runBtn, UiIcons::Role::Run);
    bb->addButton(QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    UiIcons::applyDialogButtons(bb);
    v->addWidget(bb);

    connect(_scopeCombo, &QComboBox::currentIndexChanged, this,
            [this] { refreshScopeSummary(); });

    refreshScopeSummary();
    astra::blockWheelScrollingRecursive(this);
    WindowSizing::fitToScreen(this);
}

const std::vector<std::shared_ptr<Star>>&
MassFitRunConfirmDialog::scopeStars() const
{
    switch (scope()) {
    case 1: if (!_filteredStars.empty()) return _filteredStars; break;
    case 2: if (!_selectedStars.empty()) return _selectedStars; break;
    default: break;
    }
    return _allStars;
}

int MassFitRunConfirmDialog::scope() const
{
    return _scopeCombo ? _scopeCombo->currentData().toInt() : 0;
}

mf::ExistingFitPolicy MassFitRunConfirmDialog::existingFitPolicy() const
{
    if (!_existingCombo) return mf::ExistingFitPolicy::AddNew;
    return mf::ExistingFitPolicy(_existingCombo->currentData().toInt());
}

int MassFitRunConfirmDialog::parallelStars() const
{
    return _parallelSpin ? _parallelSpin->value() : 1;
}

void MassFitRunConfirmDialog::refreshScopeSummary()
{
    const auto& stars = scopeStars();

    QStringList starIds;
    starIds.reserve(int(stars.size()));
    for (const auto& s : stars)
        if (s && !s->getId().isEmpty()) starIds << s->getId();

    // One grouped query, not a spectrum load: the point of the line is to say
    // how big the campaign is before it starts, not to read any data.
    int spectra = 0, modes = 0;
    if (_dbm) {
        for (const ModeSpectrumStat& st : _dbm->spectraModeStats(starIds)) {
            if (st.instrumentId.isEmpty()) continue;   // unlinked, never fitted
            const mf::ModeRegionConfig* m =
                _plan.mode(st.instrumentId, st.modeKey);
            if (!m || !m->enabled) continue;
            spectra += st.count;
            ++modes;
        }
    }

    _summaryLabel->setText(
        tr("%1 star(s), %2 spectra, %3 enabled mode(s) will be fitted.")
            .arg(starIds.size())
            .arg(spectra)
            .arg(modes));
}
