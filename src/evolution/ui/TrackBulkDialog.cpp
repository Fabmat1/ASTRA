#include "evolution/ui/TrackBulkDialog.h"

#include "app/ui/UiIcons.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "evolution/ui/TrackRunSetupWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTextBrowser>
#include <QThread>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace astra::evolution::ui {

namespace {

QString starLabel(const Star& s)
{
    return s.getAlias().isEmpty() ? s.getSourceId() : s.getAlias();
}

}  // namespace

TrackBulkDialog::TrackBulkDialog(DatabaseManager* dbm, std::vector<std::shared_ptr<Star>> all,
                                 std::vector<std::shared_ptr<Star>> filtered,
                                 std::vector<std::shared_ptr<Star>> selected, QWidget* parent)
    : QDialog(parent), _dbm(dbm), _all(std::move(all)), _filtered(std::move(filtered)), _selected(std::move(selected))
{
    setWindowTitle(tr("Fit Evolutionary Tracks"));
    auto* lay = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Stars:")));
    _scope = new QComboBox;
    _scope->addItem(tr("All project stars (%1)").arg(_all.size()), int(Scope::All));
    _scope->addItem(tr("Filtered stars (%1)").arg(_filtered.size()), int(Scope::Filtered));
    _scope->addItem(tr("Selected stars (%1)").arg(_selected.size()), int(Scope::Selected));
    if (auto* m = qobject_cast<QStandardItemModel*>(_scope->model())) {
        if (_filtered.empty()) m->item(1)->setEnabled(false);
        if (_selected.empty()) m->item(2)->setEnabled(false);
    }
    _scope->setCurrentIndex(!_selected.empty() ? 2 : !_filtered.empty() ? 1 : 0);
    top->addWidget(_scope, 1);
    top->addSpacing(12);
    top->addWidget(new QLabel(tr("Fit plane:")));
    _plane = new QComboBox;
    _plane->addItem(tr("Automatic"), QStringLiteral("auto"));
    _plane->addItem(tr("Teff + L (HRD)"), QStringLiteral("hrd"));
    _plane->addItem(tr("Teff + log g (Kiel)"), QStringLiteral("kiel"));
    top->addWidget(_plane);
    lay->addLayout(top);

    _skipCurrent = new QCheckBox(tr("Skip stars with an up-to-date fit"));
    _skipCurrent->setChecked(true);
    _adopt = new QCheckBox(tr("Adopt new fits"));
    _adopt->setChecked(true);
    lay->addWidget(_skipCurrent);
    lay->addWidget(_adopt);

    _setup = new TrackRunSetupWidget;
    lay->addWidget(_setup);

    _count = new QLabel;
    _count->setWordWrap(true);
    lay->addWidget(_count);

    auto* runRow = new QHBoxLayout;
    _progress = new QProgressBar;
    _progress->setTextVisible(true);
    _progress->hide();
    _status = new QLabel;
    _status->setWordWrap(true);
    _runButton = new QPushButton(tr("Run"));
    _runButton->setDefault(true);
    UiIcons::apply(_runButton, UiIcons::Role::Run);
    _cancelButton = new QPushButton(tr("Cancel"));
    _cancelButton->setAutoDefault(false);
    UiIcons::apply(_cancelButton, UiIcons::Role::Stop);
    _cancelButton->hide();
    _closeButton = new QPushButton(tr("Close"));
    _closeButton->setAutoDefault(false);
    UiIcons::apply(_closeButton, UiIcons::Role::Dismiss);
    runRow->addWidget(_progress, 1);
    runRow->addWidget(_runButton);
    runRow->addWidget(_cancelButton);
    runRow->addWidget(_closeButton);
    lay->addLayout(runRow);
    lay->addWidget(_status);

    _report = new QTextBrowser;
    _report->setOpenLinks(false);
    _report->setMinimumHeight(140);
    _report->hide();
    lay->addWidget(_report, 1);

    connect(_scope, &QComboBox::currentIndexChanged, this, &TrackBulkDialog::updateCount);
    connect(_plane, &QComboBox::currentIndexChanged, this, &TrackBulkDialog::updateCount);
    connect(_skipCurrent, &QCheckBox::toggled, this, &TrackBulkDialog::updateCount);
    connect(_setup, &TrackRunSetupWidget::changed, this, &TrackBulkDialog::updateCount);
    connect(_runButton, &QPushButton::clicked, this, &TrackBulkDialog::run);
    connect(_cancelButton, &QPushButton::clicked, this, [this] {
        if (_cancel) _cancel->store(true);
        _status->setText(tr("Cancelling"));
    });
    connect(_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    updateCount();
    resize(640, 640);
}

TrackBulkDialog::~TrackBulkDialog()
{
    if (_cancel) _cancel->store(true);
}

void TrackBulkDialog::reject()
{
    if (_running) {
        if (_cancel) _cancel->store(true);
        _status->setText(tr("Cancelling"));
        return;  // closes once the run has stopped
    }
    _setup->saveChoices();
    QDialog::reject();
}

const std::vector<std::shared_ptr<Star>>& TrackBulkDialog::sample() const
{
    switch (Scope(_scope->currentData().toInt())) {
    case Scope::Filtered: return _filtered;
    case Scope::Selected: return _selected;
    default: return _all;
    }
}

TrackBulkDialog::Plan TrackBulkDialog::planRun(const std::vector<std::shared_ptr<Star>>& stars, const QString& plane,
                                               bool skipCurrent)
{
    Plan p;
    for (const auto& s : stars) {
        if (!s) continue;
        const StarTrackSummary& t = s->getTrackSummary();
        if (skipCurrent && t.hasFit() && !t.stale) {
            p.skipped.push_back({s->getId(), starLabel(*s), QString(), QObject::tr("adopted fit is up to date")});
            continue;
        }
        TrackStarJob job{s->getId(), starLabel(*s), trackInputsFromStar(*s), plane};
        QString why;
        if (resolvePlane(job.inputs, plane, &why).isEmpty()) {
            p.skipped.push_back({s->getId(), job.label, QString(), why});
            continue;
        }
        p.jobs.push_back(std::move(job));
    }
    return p;
}

void TrackBulkDialog::updateCount()
{
    if (_running) return;
    // A cheap count: stars with what a fit needs, without reading photometry.
    int n = 0, current = 0;
    const QString plane = _plane->currentData().toString();
    for (const auto& s : sample()) {
        if (!s) continue;
        if (_skipCurrent->isChecked() && s->getTrackSummary().hasFit() && !s->getTrackSummary().stale) {
            ++current;
            continue;
        }
        const bool teff = s->getTeff() > 0 && s->getETeff() > 0 && s->getTeffLimit() == 0;
        const bool lum = s->getSedLum1() > 0 || s->getSedLum2() > 0;
        const bool logg = s->getLogg() > 0 && s->getELogg() > 0 && s->getLoggLimit() == 0;
        const bool ok = plane == QLatin1String("hrd") ? teff && lum
                      : plane == QLatin1String("kiel") ? teff && logg
                                                       : teff && (lum || logg);
        if (ok) ++n;
    }
    const int fams = _setup->families().size();
    QString text = tr("%n star(s) to fit", "", n);
    if (current > 0) text += tr(", %1 up to date").arg(current);
    _count->setText(text);
    _runButton->setEnabled(n > 0 && fams > 0);
}

void TrackBulkDialog::setRunning(bool on)
{
    _running = on;
    _runButton->setVisible(!on);
    _cancelButton->setVisible(on);
    _closeButton->setEnabled(!on);
    _progress->setVisible(on);
    for (QWidget* w : std::initializer_list<QWidget*>{_scope, _plane, _skipCurrent, _adopt, _setup})
        w->setEnabled(!on);
}

void TrackBulkDialog::run()
{
    if (_running || !_dbm) return;
    _setup->saveChoices();
    _report->hide();
    setRunning(true);
    _status->setText(tr("Reading stars"));
    QApplication::processEvents();

    // Inputs are read here, on the thread that owns the stars.
    Plan plan = planRun(sample(), _plane->currentData().toString(), _skipCurrent->isChecked());
    TrackRunRequest req;
    req.families = _setup->families();
    req.subsets = _setup->subsets();
    req.model = _setup->modelSettings();
    req.stars = std::move(plan.jobs);
    req.threads = std::max(1, QThread::idealThreadCount() - 1);
    if (req.stars.empty()) {
        setRunning(false);
        onFinished(TrackRunOutcome{}, std::move(plan.skipped));
        return;
    }

    _cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = _cancel;
    QPointer<TrackBulkDialog> self(this);
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    auto progress = [self, timer](int done, int total, const QString& what) {
        QMetaObject::invokeMethod(
            qApp,
            [self, done, total, what, timer] {
                if (!self) return;
                self->_progress->setMaximum(std::max(1, total));
                self->_progress->setValue(done);
                QString eta;
                if (done > 0 && done < total) {
                    const double s = timer->elapsed() / 1000.0 * (total - done) / done;
                    eta = s < 90 ? tr(", %1 s left").arg(int(s)) : tr(", %1 min left").arg(int(s / 60 + 0.5));
                }
                self->_status->setText(tr("%1: %2 of %3%4").arg(what).arg(done).arg(total).arg(eta));
            },
            Qt::QueuedConnection);
    };
    auto planSkips = std::make_shared<std::vector<TrackSkip>>(std::move(plan.skipped));
    auto* watcher = new QFutureWatcher<TrackRunOutcome>(this);
    connect(watcher, &QFutureWatcher<TrackRunOutcome>::finished, this, [this, watcher, planSkips] {
        const TrackRunOutcome out = watcher->result();
        watcher->deleteLater();
        setRunning(false);
        onFinished(out, *planSkips);
    });
    watcher->setFuture(QtConcurrent::run([req, progress, cancel] {
        return runTrackFits(TrackSetLibrary::shared(), req, progress, cancel.get());
    }));
}

void TrackBulkDialog::onFinished(const TrackRunOutcome& out, std::vector<TrackSkip> planSkips)
{
    QString head;
    if (out.cancelled) {
        head = tr("Cancelled");
    } else if (!out.fits.empty()) {
        QHash<QString, std::shared_ptr<Star>> byId;
        for (const auto& s : sample())
            if (s) byId.insert(s->getId(), s);
        QString err;
        if (storeTrackFits(*_dbm, out, _adopt->isChecked(), byId, &err)) {
            QSet<QString> starsFitted;
            for (const auto& f : out.fits) starsFitted.insert(f.starId);
            head = tr("%n fit(s) stored", "", int(out.fits.size())) +
                   tr(" for %n star(s).", "", int(starsFitted.size()));
            emit trackFitsStored();
        } else {
            head = err;
        }
    } else {
        head = tr("Nothing fitted");
    }
    _status->setText(head);

    // Skips grouped by reason (and family).
    std::vector<TrackSkip> all = std::move(planSkips);
    all.insert(all.end(), out.skipped.begin(), out.skipped.end());
    if (all.empty()) {
        _report->hide();
        return;
    }
    QMap<QString, QStringList> groups;
    for (const TrackSkip& s : all) {
        const QString key = s.family.isEmpty() ? s.reason
                                               : TrackSetLibrary::displayName(s.family) + QStringLiteral(": ") + s.reason;
        groups[key] << (s.label.isEmpty() ? tr("(whole family)") : s.label);
    }
    QString html = QStringLiteral("<b>%1</b><ul>").arg(tr("Not fitted").toHtmlEscaped());
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        QStringList names = it.value();
        const int n = names.size();
        if (names.size() > 25) {
            names = names.mid(0, 25);
            names << tr("and %1 more").arg(n - 25);
        }
        html += QStringLiteral("<li><b>%1</b> (%2): %3</li>")
                    .arg(it.key().toHtmlEscaped())
                    .arg(n)
                    .arg(names.join(QStringLiteral(", ")).toHtmlEscaped());
    }
    html += QStringLiteral("</ul>");
    _report->setHtml(html);
    _report->show();
    updateCount();
}

}  // namespace astra::evolution::ui
