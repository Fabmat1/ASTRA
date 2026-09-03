#include "GridSelectorWidget.h"
#include "remote/RemoteHostRegistry.h"
#include "utils/Logger.h"
#include "utils/UiIcons.h"

#include "utils/Logger.h"
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QtConcurrent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <functional>
#include <utility>
#ifndef Q_OS_WIN
  #include <dirent.h>
  #include <sys/stat.h>
#endif

namespace {

// Process-wide cache of scan results, keyed by base paths + markers.
QHash<QString, QVector<DiscoveredGrid>> &gridScanCache() {
    static QHash<QString, QVector<DiscoveredGrid>> cache;
    return cache;
}

void clearGridScanCache() { gridScanCache().clear(); }

struct DirListing {
    QStringList subdirs;
    int         entryCount   = 0;
    int         unknownCount = 0;
    // markerFound removed: detection now done by dirHasMarker()
};

// Cheap marker probe: a few stat() calls, no directory enumeration.
//
// The POSIX spelling below is deliberate -- grid trees live on network mounts
// with tens of thousands of directories, and readdir()'s d_type answers "is
// this a directory?" without a stat() per entry. mingw-w64 has neither d_type
// nor lstat(), so Windows gets a Qt implementation of the same two functions.
// Same semantics either way: hidden entries skipped, symlinked directories not
// followed, entryCount over everything but "." and "..".
#ifdef Q_OS_WIN

bool dirHasMarker(const QString &path, const QStringList &markers) {
    for (const QString &m : markers)
        if (QFileInfo(path + "/" + m).isFile())
            return true;
    return false;
}

#else

bool dirHasMarker(const QString &path, const QStringList &markers) {
    for (const QString &m : markers) {
        struct stat      st;
        const QByteArray full = QFile::encodeName(path + "/" + m);
        if (::stat(full.constData(), &st) == 0 && S_ISREG(st.st_mode))
            return true;
    }
    return false;
}

#endif

// readdir pass to enumerate subdirectories only.
#ifdef Q_OS_WIN

DirListing listDir(const QString &path) {
    DirListing out;
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                 | QDir::Hidden | QDir::System);
    for (const QFileInfo &fi : entries) {
        ++out.entryCount;
        const QString name = fi.fileName();
        if (name.startsWith('.'))
            continue; // skip hidden
        if (fi.isDir() && !fi.isSymLink())
            out.subdirs << name;
    }
    return out;
}

#else

DirListing listDir(const QString &path) {
    DirListing out;
    DIR       *dir = ::opendir(QFile::encodeName(path).constData());
    if (!dir)
        return out;

    struct dirent *ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        const QString name = QString::fromLocal8Bit(ent->d_name);
        if (name == "." || name == "..")
            continue;
        ++out.entryCount;

        if (name.startsWith('.'))
            continue; // skip hidden

        bool isDir = false;
        switch (ent->d_type) {
        case DT_DIR:
            isDir = true;
            break;
        case DT_LNK:
            isDir = false;
            break;
        case DT_UNKNOWN: {
            ++out.unknownCount;
            struct stat      st;
            const QByteArray full = QFile::encodeName(path + "/" + name);
            if (::lstat(full.constData(), &st) == 0 && !S_ISLNK(st.st_mode))
                isDir = S_ISDIR(st.st_mode);
            break;
        }
        default:
            isDir = false;
            break;
        }
        if (isDir)
            out.subdirs << name;
    }
    ::closedir(dir);
    return out;
}

#endif

QString stripSlashes(QString s) {
    while (s.endsWith('/'))
        s.chop(1);
    while (s.startsWith('/'))
        s.remove(0, 1);
    return s;
}

// A plain endsWith() would let ".../my_sdB/processed" match the "sdB/processed"
// preset, so the match has to land on a path-component boundary.
bool pathEndsWith(const QString &canon, const QString &rawSuffix) {
    const QString suffix = stripSlashes(rawSuffix);
    if (suffix.isEmpty() || !canon.endsWith(suffix))
        return false;
    const int at = canon.size() - suffix.size();
    return at == 0 || canon.at(at - 1) == '/';
}

// Leaf directory name -> preset, for the leaf names that identify exactly one
// preset. A bare "processed" belongs to a dozen of them and would misclassify
// anything it touched, so ambiguous names are dropped.
QHash<QString, int> uniqueLeafIndex(const QVector<GridPreset> &presets) {
    QHash<QString, int> leaves;
    for (int pi = 0; pi < presets.size(); ++pi) {
        QStringList paths{presets[pi].path};
        paths += presets[pi].aliases;
        for (const QString &p : std::as_const(paths)) {
            const QString leaf = stripSlashes(p).section('/', -1);
            if (leaf.isEmpty())
                continue;
            auto it = leaves.find(leaf);
            if (it == leaves.end())
                leaves.insert(leaf, pi);
            else if (it.value() != pi)
                it.value() = -1; // claimed by more than one preset
        }
    }
    for (auto it = leaves.begin(); it != leaves.end();) {
        if (it.value() < 0)
            it = leaves.erase(it);
        else
            ++it;
    }
    return leaves;
}

// Preset for a discovered grid directory, or -1. Grids get re-rooted (a base
// path pointing straight at .../sdB drops the "sdB/" the presets carry) as
// often as they get renamed, so the full suffix is only the first attempt.
int matchPreset(const QString &canon, const QVector<GridPreset> &presets,
                const QHash<QString, int> &leaves) {
    for (int pi = 0; pi < presets.size(); ++pi) {
        if (pathEndsWith(canon, presets[pi].path))
            return pi;
        for (const QString &alt : presets[pi].aliases)
            if (pathEndsWith(canon, alt))
                return pi;
    }
    return leaves.value(canon.section('/', -1), -1);
}

} // namespace

const QVector<GridPreset>& GridSelectorWidget::defaultPresets()
{
    static const QVector<GridPreset> p = {
        {"sdB","sdB standard","sdB/processed/",15000,55000,4.6,6.6,-5.05,-0.041,-1.0,1.0},
        {"sdB","sdB extended","sdB/processed_sdB24/",15000,55000,4.6,7.0,-5.05,-0.041,-1.0,1.0},
        {"sdB","ELM / BHB","sdB/processed_ELM_BHB/",9000,20000,2.8,7.0,-5.05,-0.300,-1.0,1.0,
         {"sdB/processed_ELM/"}},
        {"sdB","BLAPS","sdB/processed_blaps/",15000,31000,3.6,7.0,-4.05,-0.300,-2.0,1.0},
        {"sdB","Hot sdO","sdB/processed_hot_sdO/",51000,75000,5.2,6.6,-5.05,-0.041,-1.0,1.0},
        {"sdB","He-sdO","sdB/processed_He_sdO/",25000,55000,5.0,6.6,-1.05,-0.001,-1.0,1.0},
        {"sdB","He-sdO Z=0 xl","sdB/processed_He_sdO_Z0.00_xl/",25000,55000,4.0,6.6,-1.05,-0.001,0.0,0.0},
        {"sdB","Very hot sdO","sdB/processed_vhot_sdO/",75000,99000,5.6,7.0,-5.05,-0.041,-1.0,0.0},
        {"sdB","Super-hot sdO","sdB/processed_shot_sdO/",75000,115000,5.8,7.0,-5.05,-0.041,-1.0,0.0},
        {"B stars","Late B (III–V)","B_V_III/processed_late/",10000,19000,3.0,4.6,-1.25,-0.85,-0.5,0.5},
        {"B stars","Mid B (III–V)","B_V_III/processed_mid/",18000,25000,3.0,4.6,-1.25,-0.85,-0.5,0.5},
        {"B stars","Early B (III–V)","B_V_III/processed_early/",25000,33000,3.4,4.6,-1.25,-0.85,-0.5,0.5},
        {"sdO (2020)","Standard","sdOstar2020_SED/processed/",26250,57500,4.25,6.75,-1.75,4.00,0.0,0.0},
        {"sdO (2020)","Hot","sdOstar2020_SED/processed_hot/",26250,65000,4.50,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)","Hot He-sdO","sdOstar2020_SED/processed_hot_HesdO/",26250,72500,4.625,6.75,-1.00,4.00,0.0,0.0},
        {"sdO (2020)","Low-He sdO","sdOstar2020_SED/processed_lHe-sdO/",26250,55000,4.00,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)","MS","sdOstar2020_SED/processed_MS/",26250,45000,3.625,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)","Cool","sdOstar2020_SED/processed_cool/",26250,47500,3.75,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)","EHe","sdOstar2020_SED/processed_EHe/",26250,35000,3.25,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)","EHe cool","sdOstar2020_SED/processed_EHe_cool/",26250,31250,3.00,6.75,-0.75,4.00,0.0,0.0},
        {"Steven","Grid 5 (hot)","steven/grid5/",38000,55000,4.6,6.6,-5.00,-0.25,-2.0,0.5},
        {"Steven","Grid 4","steven/grid4/",22000,40000,4.0,6.6,-5.00,-0.25,-2.0,0.5},
        {"Steven","Grid 3","steven/grid3/",15000,26000,3.0,6.4,-5.00,-0.25,-2.0,0.5},
        {"Steven","Grid 2","steven/grid2/",11000,17000,2.8,6.0,-5.00,-0.25,-2.0,0.5},
        {"Steven","Grid 1 (cool)","steven/grid1/",8000,12500,2.4,4.4,-5.00,-0.50,-2.0,0.5},
        {"Cool stars","Synthe","synthe/processed/",3800,12500,1.4,5.6,-1.0,-1.0,-2.5,0.5},
        {"Cool stars","Synthe high logg","synthe/processed_vhighlogg/",4600,14000,1.6,7.0,-1.0,-1.0,-2.0,0.5},
        {"Cool stars","Synthe low logg","synthe/processed_lowlogg/",3400,6200,0.0,4.0,-1.0,-1.0,-2.0,0.5},
        {"Cool stars","Synthe α+0.3","synthe_alpha+0.3/processed/",4000,8000,2.0,5.2,-1.0,-1.0,-2.0,0.5},
        {"Cool stars","Synthe α+0.4","synthe_alpha+0.4/processed/",4000,8000,2.0,5.2,-1.0,-1.0,-2.0,0.5},
        {"Cool stars","Phoenix","Phoenix_late_type_stars_photometry_v2.0/processed/",2300,15000,2.0,5.0,-1.05,-1.05,-2.0,0.0},
        {"WD","DAO (Nicole)","WD/Nicole/DAO/processed/",40000,180000,6.0,9.0,-5.0,0.0,0.0,0.0,
         {"DAO/processed/"}},
        {"WD","DO (Nicole)","WD/Nicole/DO/processed/",40000,180000,6.0,9.0,99.0,99.0,0.0,0.0},
        {"WD","DA (Nicole)","WD/Nicole/DA/processed/",20000,180000,6.0,9.0,-99.0,-99.0,0.0,0.0},
        {"WD","OH (Nicole)","WD/Nicole/OH/processed/",40000,140000,5.5,6.5,-1.553,-0.423,0.0,0.0},
        {"WD","DA NLTE (Tremblay)","WD/Tremblay/processed_DA1DNLTE/",2000,140000,6.5,9.5,-99,-99,0.0,0.0},
        {"WD","DA","WD/DA/processed",6000,100000,5.5,9.5,-100,-100,0.0,0.0},
        {"WD","DB","WD/DB/processed",10000,40000,7.0,9.0,0,0,0.0,0.0},
    };
    return p;
}

GridSelectorWidget::GridSelectorWidget(QWidget* parent) : QWidget(parent)
{
    _presets = defaultPresets();
    buildUi();
}

void GridSelectorWidget::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    _groupBox = new QGroupBox;
    _groupBox->setTitle(QString());
    auto* g = new QGridLayout(_groupBox);
    g->setColumnStretch(1, 1);
    int r = 0;

    g->addWidget(new QLabel("Category:"), r, 0);
    _catCombo = new QComboBox;
    g->addWidget(_catCombo, r++, 1, 1, 2);

    g->addWidget(new QLabel("Grid:"), r, 0);
    _gridCombo = new QComboBox;
    _gridCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _gridCombo->setMinimumContentsLength(20);
    g->addWidget(_gridCombo, r++, 1, 1, 2);

    g->addWidget(new QLabel("Override:"), r, 0);
    _overrideEdit = new QLineEdit;
    _overrideEdit->setPlaceholderText(
        "Custom grid relative path (leave empty to use combo)");
    g->addWidget(_overrideEdit, r++, 1, 1, 2);

    auto* bar = new QHBoxLayout;
    _statusLabel = new QLabel;
    _statusLabel->setStyleSheet("color: gray; font-style: italic;");
    bar->addWidget(_statusLabel, 1);

    _spinner = new QProgressBar;
    _spinner->setRange(0, 0); // indeterminate "busy" animation
    _spinner->setMaximumWidth(90);
    _spinner->setTextVisible(false);
    _spinner->setVisible(false);
    bar->addWidget(_spinner);

    _refreshBtn = new QPushButton;
    UiIcons::apply(_refreshBtn, UiIcons::Role::Refresh);
    _refreshBtn->setToolTip("Rescan grid base paths");
    _refreshBtn->setMaximumWidth(30);
    bar->addWidget(_refreshBtn);

    _configBtn = new QPushButton("Paths…");
    _configBtn->setToolTip("Configure grid base paths in preferences");
    _configBtn->setVisible(false);
    bar->addWidget(_configBtn);

    g->addLayout(bar, r, 0, 1, 3);
    outer->addWidget(_groupBox);

    connect(_catCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ _hasExplicitSelection = true;
                               populateGridCombo(); emit selectionChanged(); });
    connect(_gridCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ _hasExplicitSelection = true;
                               emit selectionChanged(); });
    connect(_overrideEdit, &QLineEdit::editingFinished,
            this, [this]{ _hasExplicitSelection = true; emit selectionChanged(); });
    connect(_refreshBtn, &QPushButton::clicked, this, [this] {
        clearGridScanCache();
        refresh();
    });
    connect(_configBtn, &QPushButton::clicked,
            this, &GridSelectorWidget::configurePathsRequested);
}

void GridSelectorWidget::setBasePaths(const QStringList& paths)
{ _basePaths = paths; refresh(); }

void GridSelectorWidget::setGridMarkers(const QStringList& markers)
{ _markers = markers.isEmpty() ? QStringList{"grid.fits"} : markers; refresh(); }

void GridSelectorWidget::setPresets(const QVector<GridPreset>& presets)
{ _presets = presets; refresh(); }

void GridSelectorWidget::setTitle(const QString& title)
{ _groupBox->setTitle(title); }

void GridSelectorWidget::setShowConfigureButton(bool show)
{ _configBtn->setVisible(show); }

void GridSelectorWidget::refresh() { startScan(); }

void GridSelectorWidget::startScan() {
    const QString cacheKey = _basePaths.join('|') + "##" + _markers.join('|') +
                             "##" + _skipTokens.join('|');

    // Cache hit -> synchronous, instant.
    auto it = gridScanCache().constFind(cacheKey);
    if (it != gridScanCache().constEnd()) {
        _discovered = it.value();
        LOG_DEBUG(
            "GridScan",
            QString("Using cached scan (%1 grids)").arg(_discovered.size()));
        finishPopulate();
        return;
    }

    if (_scanWatcher && _scanWatcher->isRunning())
        return; // a scan is already in flight

    setLoading(true);

    auto *watcher = new QFutureWatcher<QVector<DiscoveredGrid>>(this);
    _scanWatcher  = watcher;
    connect(watcher, &QFutureWatcher<QVector<DiscoveredGrid>>::finished, this,
            &GridSelectorWidget::onScanFinished);

    // Snapshot inputs by value so the worker touches nothing on `this`.
    const QStringList         basePaths = _basePaths;
    const QStringList         markers   = _markers;
    const QStringList         skip      = _skipTokens;
    const QVector<GridPreset> presets   = _presets;

    watcher->setFuture(QtConcurrent::run([basePaths, markers, skip, presets] {
        return performScan(basePaths, markers, skip, presets);
    }));
}

void GridSelectorWidget::onScanFinished() {
    if (!_scanWatcher)
        return;
    _discovered = _scanWatcher->result();

    const QString cacheKey = _basePaths.join('|') + "##" + _markers.join('|') +
                             "##" + _skipTokens.join('|');
    gridScanCache().insert(cacheKey,
                           _discovered); // main thread: QHash safe here

    _scanWatcher->deleteLater();
    _scanWatcher = nullptr;

    setLoading(false);
    finishPopulate();
}

void GridSelectorWidget::setLoading(bool on) {
    _spinner->setVisible(on);
    _catCombo->setEnabled(!on);
    _gridCombo->setEnabled(!on);
    _overrideEdit->setEnabled(!on);
    _refreshBtn->setEnabled(!on);
    if (on)
        _statusLabel->setText("Scanning grids…");
}

void GridSelectorWidget::finishPopulate() {
    populateCategoryCombo();
    populateGridCombo();

    if (_basePaths.isEmpty())
        _statusLabel->setText("No base paths configured");
    else if (_discovered.isEmpty())
        _statusLabel->setText(QString("No grids found under %1 path%2")
                                  .arg(_basePaths.size())
                                  .arg(_basePaths.size() == 1 ? "" : "s"));
    else
        _statusLabel->setText(QString("%1 grid%2 found")
                                  .arg(_discovered.size())
                                  .arg(_discovered.size() == 1 ? "" : "s"));

    // Apply any selection requested before the scan completed.
    if (_scanPending) {
        _scanPending = false;
        applySelection(_pendingCat, _pendingSel);
    } else if (!_hasExplicitSelection) {
        applyDefaultSelection();
    }
}

void GridSelectorWidget::applyDefaultSelection()
{
    // Prefer the sdB / "sdB standard" grid when it was discovered. This is the
    // most common starting point; the user can still change it freely.
    for (const auto& dg : _discovered) {
        if (dg.category == "sdB" && dg.displayName == "sdB standard") {
            _catCombo->blockSignals(true);
            int ci = _catCombo->findData(dg.category);
            if (ci >= 0) _catCombo->setCurrentIndex(ci);
            _catCombo->blockSignals(false);

            populateGridCombo();

            _gridCombo->blockSignals(true);
            int gi = _gridCombo->findData(dg.relativePath);
            if (gi >= 0) _gridCombo->setCurrentIndex(gi);
            _gridCombo->blockSignals(false);

            emit selectionChanged();
            return;
        }
    }
}

QVector<DiscoveredGrid> GridSelectorWidget::performScan(
    const QStringList &basePaths, const QStringList &markers,
    const QStringList &skipTokens, const QVector<GridPreset> &presets) {
    QElapsedTimer timer;
    timer.start();
    int  dirsVisited = 0, dirsPruned = 0, dirsSkipped = 0;
    long unknownTotal = 0;

    QVector<DiscoveredGrid> discovered;
    QSet<QString>           seen;

    auto shouldSkipDir = [&](const QString &name) {
        for (const QString &tok : skipTokens)
            if (name.contains(tok, Qt::CaseInsensitive))
                return true;
        return false;
    };

    const QHash<QString, int> leaves = uniqueLeafIndex(presets);

    for (const QString &raw : basePaths) {
        QString base = raw.trimmed();
        if (base.isEmpty())
            continue;

        // Remote base paths ("ssh://<host>/<dir>") are listed over the host's
        // streaming channel instead of being walked locally: one round trip
        // for the whole tree rather than a directory listing per level.
        if (astra::remote::isRemoteGridUrl(base)) {
            QString hostName, remoteDir;
            if (!astra::remote::parseRemoteGridUrl(base, &hostName, &remoteDir))
                continue;
            auto *chan =
                astra::remote::RemoteHostRegistry::instance().channelByName(
                    hostName);
            if (!chan) {
                LOG_DEBUG("GridScan",
                          QString("Remote host gone: %1").arg(hostName));
                continue;
            }
            QStringList markerPaths;
            QString err;
            if (!chan->list(remoteDir, 6, &markerPaths, &err)) {
                LOG_INFO("GridScan", QString("Remote listing of %1 failed: %2")
                                         .arg(base, err));
                continue;
            }
            for (const QString &marker : markerPaths) {
                // ".../<grid dir>/grid.fits" -> the grid directory
                const int slash = marker.lastIndexOf(QLatin1Char('/'));
                if (slash <= 0)
                    continue;
                const QString gridDir = marker.left(slash);
                QString rel = gridDir.mid(remoteDir.size());
                while (rel.startsWith(QLatin1Char('/')))
                    rel.remove(0, 1);
                if (rel.isEmpty())
                    rel = QStringLiteral(".");
                if (!rel.endsWith(QLatin1Char('/')))
                    rel += QLatin1Char('/');

                bool skip = false;
                for (const QString &tok : skipTokens)
                    if (rel.contains(tok, Qt::CaseInsensitive))
                        skip = true;
                if (skip)
                    continue;

                const QString canon = base + gridDir.mid(remoteDir.size());
                if (seen.contains(canon))
                    continue;
                seen.insert(canon);

                DiscoveredGrid dg;
                dg.fullPath     = canon;
                dg.basePath     = base;
                dg.relativePath = rel;
                const int pi = matchPreset(gridDir, presets, leaves);
                if (pi >= 0) {
                    dg.presetIndex = pi;
                    dg.category    = presets[pi].category;
                    dg.displayName = presets[pi].name;
                    dg.teffMin     = presets[pi].teffMin;
                    dg.teffMax     = presets[pi].teffMax;
                    dg.loggMin     = presets[pi].loggMin;
                    dg.loggMax     = presets[pi].loggMax;
                    dg.heMin       = presets[pi].heMin;
                    dg.heMax       = presets[pi].heMax;
                    dg.zMin        = presets[pi].zMin;
                    dg.zMax        = presets[pi].zMax;
                } else {
                    dg.category    = QStringLiteral("Discovered");
                    dg.displayName = rel;
                }
                // The host has to be visible in the combo: the same grid may
                // exist locally and on several hosts, and they are not
                // interchangeable (one costs a download).
                dg.displayName += QStringLiteral(" @ ") + hostName;
                discovered.append(std::move(dg));
            }
            continue;
        }

        QDir baseDir(base);
        if (!baseDir.exists()) {
            LOG_DEBUG("GridScan", QString("Base does not exist: %1").arg(base));
            continue;
        }
        QString baseCan = baseDir.canonicalPath();
        if (baseCan.isEmpty())
            baseCan = QDir::cleanPath(baseDir.absolutePath());

        QElapsedTimer baseTimer;
        baseTimer.start();
        const int before = discovered.size();

        auto addGrid = [&](const QString &canon) {
            if (seen.contains(canon))
                return;
            seen.insert(canon);

            DiscoveredGrid dg;
            dg.fullPath     = canon;
            dg.basePath     = baseCan;
            dg.relativePath = QDir(baseCan).relativeFilePath(canon);
            if (!dg.relativePath.endsWith('/'))
                dg.relativePath += '/';

            const int pi = matchPreset(canon, presets, leaves);
            if (pi >= 0) {
                dg.presetIndex = pi;
                dg.category    = presets[pi].category;
                dg.displayName = presets[pi].name;
                dg.teffMin     = presets[pi].teffMin;
                dg.teffMax     = presets[pi].teffMax;
                dg.loggMin     = presets[pi].loggMin;
                dg.loggMax     = presets[pi].loggMax;
                dg.heMin       = presets[pi].heMin;
                dg.heMax       = presets[pi].heMax;
                dg.zMin        = presets[pi].zMin;
                dg.zMax        = presets[pi].zMax;
            } else {
                dg.category = "Discovered";
                // "./" (the base path is the grid) names nothing the user can
                // recognise in a combo box, so fall back to the folder name.
                dg.displayName = (dg.relativePath == "./")
                                     ? QFileInfo(canon).fileName()
                                     : dg.relativePath;
            }
            LOG_DEBUG("GridScan", QString("  + grid: %1 [%2]")
                                      .arg(dg.relativePath, dg.displayName));
            discovered.append(std::move(dg));
        };

        // A marker in the base directory itself is nearly always a leftover
        // sitting on top of a whole collection of real grids, and pruning there
        // would hide every one of them. So the root is scanned through and only
        // offered as a grid if nothing turns up beneath it.
        QString rootCandidate;

        std::function<void(const QString &, int)> scan = [&](const QString &dir,
                                                             int depth) {
            ++dirsVisited;

            if (dirHasMarker(dir, markers)) {
                if (depth > 0) {
                    ++dirsPruned;
                    addGrid(QDir::cleanPath(dir));
                    return;
                }
                rootCandidate = QDir::cleanPath(dir);
            }

            if (depth >= 5)
                return;

            QElapsedTimer t;
            t.start();
            const DirListing listing = listDir(dir);
            const qint64     ms      = t.elapsed();
            unknownTotal += listing.unknownCount;
            if (ms > 200)
                LOG_DEBUG("GridScan",
                          QString("SLOW %1 ms (entries=%2, unknownType=%3): %4")
                              .arg(ms)
                              .arg(listing.entryCount)
                              .arg(listing.unknownCount)
                              .arg(dir));

            for (const QString &sub : listing.subdirs) {
                if (shouldSkipDir(sub)) {
                    ++dirsSkipped;
                    continue;
                }
                scan(dir + "/" + sub, depth + 1);
            }
        };

        scan(baseCan, 0);
        if (!rootCandidate.isEmpty()) {
            // Counted over everything discovered, not just this base's own
            // additions, so a grid already claimed by an earlier base path still
            // disqualifies the root.
            bool anyBelow = false;
            for (const QString &s : std::as_const(seen))
                if (s.size() > baseCan.size() && s.startsWith(baseCan + '/')) {
                    anyBelow = true;
                    break;
                }
            if (!anyBelow)
                addGrid(rootCandidate);
            else
                LOG_DEBUG("GridScan",
                          QString("  - ignoring marker at base root %1 "
                                  "(real grids found below it)")
                              .arg(rootCandidate));
        }
        LOG_DEBUG("GridScan", QString("Base %1 -> %2 grids in %3 ms")
                                  .arg(base)
                                  .arg(discovered.size() - before)
                                  .arg(baseTimer.elapsed()));
    }

    LOG_DEBUG("GridScan",
              QString("DONE: visited %1 dirs, pruned %2, skipped %3, found %4 "
                      "grids in %5 ms (lstat-from-UNKNOWN=%6)")
                  .arg(dirsVisited)
                  .arg(dirsPruned)
                  .arg(dirsSkipped)
                  .arg(discovered.size())
                  .arg(timer.elapsed())
                  .arg(unknownTotal));

    return discovered;
}

void GridSelectorWidget::populateCategoryCombo()
{
    QString prev = _catCombo->currentData().toString();
    _catCombo->blockSignals(true);
    _catCombo->clear();

    QStringList cats;
    for (const auto& dg : _discovered)
        if (!cats.contains(dg.category)) cats << dg.category;
    for (const auto& p : _presets)
        if (!cats.contains(p.category)) cats << p.category;

    for (const auto& c : cats) {
        int n = 0;
        for (const auto& dg : _discovered) if (dg.category == c) ++n;
        _catCombo->addItem(
            n > 0 ? QString("%1  (%2 found)").arg(c).arg(n)
                  : QString("%1  (none found)").arg(c),
            c);
    }
    int idx = _catCombo->findData(prev);
    if (idx >= 0) _catCombo->setCurrentIndex(idx);
    _catCombo->blockSignals(false);
}

void GridSelectorWidget::populateGridCombo()
{
    QString cat  = _catCombo->currentData().toString();
    QString prev = _gridCombo->currentData().toString();
    _gridCombo->blockSignals(true);
    _gridCombo->clear();

    for (const auto& dg : _discovered) {
        if (dg.category != cat) continue;
        QString label = dg.presetIndex >= 0
            ? QString("%1  (%2–%3 kK, logg %4–%5)")
                .arg(dg.displayName)
                .arg(dg.teffMin / 1000.0, 0, 'f', 0)
                .arg(dg.teffMax / 1000.0, 0, 'f', 0)
                .arg(dg.loggMin, 0, 'f', 1)
                .arg(dg.loggMax, 0, 'f', 1)
            : dg.displayName;
        _gridCombo->addItem(label, dg.relativePath);
    }
    int idx = _gridCombo->findData(prev);
    if (idx >= 0) _gridCombo->setCurrentIndex(idx);
    _gridCombo->blockSignals(false);
}

bool GridSelectorWidget::hasSelection() const
{ return !selectedRelativePath().isEmpty(); }

QString GridSelectorWidget::selectedRelativePath() const
{
    QString ov = _overrideEdit->text().trimmed();
    if (!ov.isEmpty()) return ov;
    return _gridCombo->currentData().toString();
}

QString GridSelectorWidget::selectedBasePath() const
{
    if (!_overrideEdit->text().trimmed().isEmpty()) return {};
    QString rel = _gridCombo->currentData().toString();
    for (const auto& dg : _discovered)
        if (dg.relativePath == rel) return dg.basePath;
    return {};
}

QString GridSelectorWidget::selectedFullPath() const
{
    auto g = selectedGrid();
    if (g) return g->fullPath;
    return _overrideEdit->text().trimmed();
}

std::optional<DiscoveredGrid> GridSelectorWidget::selectedGrid() const
{
    if (!_overrideEdit->text().trimmed().isEmpty()) return std::nullopt;
    QString rel = _gridCombo->currentData().toString();
    for (const auto& dg : _discovered)
        if (dg.relativePath == rel) return dg;
    return std::nullopt;
}

void GridSelectorWidget::setSelection(const QString &category,
                                      const QString &relativePathOrOverride) {
    if (relativePathOrOverride.isEmpty())
        return;

    if (_scanWatcher && _scanWatcher->isRunning()) {
        _pendingCat  = category;
        _pendingSel  = relativePathOrOverride;
        _scanPending = true;
        return; // applied in finishPopulate()
    }
    applySelection(category, relativePathOrOverride);
}

void GridSelectorWidget::applySelection(const QString &category,
                                        const QString &relativePathOrOverride)
{
    if (relativePathOrOverride.isEmpty()) return;
    _hasExplicitSelection = true;

    for (const auto& dg : _discovered) {
        if (dg.relativePath == relativePathOrOverride ||
            dg.fullPath     == relativePathOrOverride)
        {
            int ci = _catCombo->findData(dg.category);
            if (ci >= 0) _catCombo->setCurrentIndex(ci);
            populateGridCombo();
            int gi = _gridCombo->findData(dg.relativePath);
            if (gi >= 0) _gridCombo->setCurrentIndex(gi);
            _overrideEdit->clear();
            return;
        }
    }
    if (!category.isEmpty()) {
        int ci = _catCombo->findData(category);
        if (ci >= 0) _catCombo->setCurrentIndex(ci);
    }
    _overrideEdit->setText(relativePathOrOverride);
}