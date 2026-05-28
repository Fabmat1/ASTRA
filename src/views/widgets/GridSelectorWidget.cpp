#include "GridSelectorWidget.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <functional>

namespace {

// mountinfo escapes spaces/tabs as octal (\040 etc.) — undo that.
QString unescapeMountPath(const QByteArray &s) {
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            bool ok   = false;
            int  code = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out += QChar(code);
                i += 3;
                continue;
            }
        }
        out += QChar(s[i]);
    }
    return out;
}

// Returns the set of mount points whose filesystem type is a network/remote
// type. Reads /proc/self/mountinfo directly, which does NOT stat the (possibly
// dead) mounts, so it can never block.
QSet<QString> networkMountPoints() {
    QSet<QString> result;
#ifdef Q_OS_LINUX
    QFile f("/proc/self/mountinfo");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        static const QList<QByteArray> net = {
            "nfs",       "nfs4", "cifs",       "smbfs",           "smb2",
            "smb3",      "afs",  "ncpfs",      "afpfs",           "glusterfs",
            "ceph",      "9p",   "fuse.sshfs", "fuse.gvfsd-fuse", "fuse.rclone",
            "fuse.s3fs", "davfs"};
        while (!f.atEnd()) {
            const QByteArray line = f.readLine();
            const int        sep  = line.indexOf(" - ");
            if (sep < 0)
                continue;
            const QList<QByteArray> pre  = line.left(sep).split(' ');
            const QList<QByteArray> post = line.mid(sep + 3).split(' ');
            if (pre.size() < 5 || post.isEmpty())
                continue;

            const QByteArray fsType = post[0].toLower();
            bool             isNet  = false;
            for (const auto &n : net)
                if (fsType == n || fsType.startsWith(n)) {
                    isNet = true;
                    break;
                }
            if (!isNet)
                continue;

            result.insert(QDir::cleanPath(unescapeMountPath(pre[4])));
        }
    }
#endif
    return result;
}

} // namespace

const QVector<GridPreset>& GridSelectorWidget::defaultPresets()
{
    static const QVector<GridPreset> p = {
        {"sdB","sdB standard","sdB/processed/",15000,55000,4.6,6.6,-5.05,-0.041,-1.0,1.0},
        {"sdB","sdB extended","sdB/processed_sdB24/",15000,55000,4.6,7.0,-5.05,-0.041,-1.0,1.0},
        {"sdB","ELM / BHB","sdB/processed_ELM_BHB/",9000,20000,2.8,7.0,-5.05,-0.300,-1.0,1.0},
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
        {"WD","DAO (Nicole)","WD/Nicole/DAO/processed/",40000,180000,6.0,9.0,-5.0,0.0,0.0,0.0},
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

    _refreshBtn = new QPushButton(QString::fromUtf8("\xE2\x9F\xB3"));
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
            this, [this](int){ populateGridCombo(); emit selectionChanged(); });
    connect(_gridCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ emit selectionChanged(); });
    connect(_overrideEdit, &QLineEdit::editingFinished,
            this, [this]{ emit selectionChanged(); });
    connect(_refreshBtn, &QPushButton::clicked,
            this, &GridSelectorWidget::refresh);
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

void GridSelectorWidget::refresh()
{
    scanPaths();
    populateCategoryCombo();
    populateGridCombo();

    if (_basePaths.isEmpty())
        _statusLabel->setText("No base paths configured");
    else if (_discovered.isEmpty())
        _statusLabel->setText(QString("No grids found under %1 path%2")
            .arg(_basePaths.size()).arg(_basePaths.size() == 1 ? "" : "s"));
    else
        _statusLabel->setText(QString("%1 grid%2 found")
            .arg(_discovered.size()).arg(_discovered.size() == 1 ? "" : "s"));
}

void GridSelectorWidget::scanPaths() {
    _discovered.clear();
    QSet<QString> seen;

    const QSet<QString> netMounts = networkMountPoints();

    auto onNetworkMount = [&](const QString &path) {
        const QString clean = QDir::cleanPath(path);
        for (const QString &mp : netMounts)
            if (clean == mp || clean.startsWith(mp + "/"))
                return true;
        return false;
    };

    auto markerHit = [&](const QDir &d) {
        for (const auto &m : _markers)
            if (d.exists(m))
                return true;
        return false;
    };

    for (const QString &raw : _basePaths) {
        QString base = raw.trimmed();
        if (base.isEmpty())
            continue;

        QDir baseDir(base);
        if (onNetworkMount(baseDir.absolutePath()))
            continue; // never even touch it
        if (!baseDir.exists())
            continue;

        // Avoid canonicalPath() on the whole subtree; clean the base once.
        QString baseCan = baseDir.canonicalPath();
        if (baseCan.isEmpty())
            baseCan = QDir::cleanPath(baseDir.absolutePath());

        std::function<void(const QString &, int)> scan = [&](const QString &dir,
                                                             int depth) {
            if (depth > 5)
                return;
            QDir d(dir);

            if (markerHit(d)) {
                // NOTE: use cleanPath instead of canonicalPath() so we never
                // resolve a symlink into a (possibly dead) network location.
                const QString canon = QDir::cleanPath(d.absolutePath());
                if (!seen.contains(canon)) {
                    seen.insert(canon);
                    DiscoveredGrid dg;
                    dg.fullPath     = canon;
                    dg.basePath     = baseCan;
                    dg.relativePath = QDir(baseCan).relativeFilePath(canon);
                    if (!dg.relativePath.endsWith('/'))
                        dg.relativePath += '/';

                    for (int pi = 0; pi < _presets.size(); ++pi) {
                        QString suf = _presets[pi].path;
                        while (suf.endsWith('/'))
                            suf.chop(1);
                        if (canon.endsWith(suf)) {
                            dg.presetIndex = pi;
                            dg.category    = _presets[pi].category;
                            dg.displayName = _presets[pi].name;
                            dg.teffMin     = _presets[pi].teffMin;
                            dg.teffMax     = _presets[pi].teffMax;
                            dg.loggMin     = _presets[pi].loggMin;
                            dg.loggMax     = _presets[pi].loggMax;
                            dg.heMin       = _presets[pi].heMin;
                            dg.heMax       = _presets[pi].heMax;
                            dg.zMin        = _presets[pi].zMin;
                            dg.zMax        = _presets[pi].zMax;
                            break;
                        }
                    }
                    if (dg.presetIndex < 0) {
                        dg.category    = "Discovered";
                        dg.displayName = dg.relativePath;
                    }
                    _discovered.append(std::move(dg));
                }
            }

            // NoSymLinks: never follow a symlink (could point at a dead mount).
            const auto subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot |
                                          QDir::NoSymLinks);
            for (const auto &sub : subs) {
                if (sub.startsWith('.'))
                    continue;
                const QString child = dir + "/" + sub;
                if (onNetworkMount(child))
                    continue; // don't descend into network mounts
                scan(child, depth + 1);
            }
        };
        scan(baseCan, 0);
    }
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

void GridSelectorWidget::setSelection(const QString& category,
                                      const QString& relativePathOrOverride)
{
    if (relativePathOrOverride.isEmpty()) return;

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