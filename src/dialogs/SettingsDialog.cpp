#include "SettingsDialog.h"
#include "utils/AppSettings.h"
#include "utils/IsisEnvironment.h"
#include "utils/SedFitEnvironment.h"
#include "utils/LcqueryEnvironment.h"
#include "utils/LightcurveFetcher.h"
#include "utils/UpdateManager.h"
#include "views/tools/LcquerySetupDialog.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QListWidget>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QStandardPaths>
#include <QListWidget>

// =====================================================================
// DetailGridEditor - nested widget (file-private)
// =====================================================================
class DetailGridEditor : public QWidget
{
public:
    explicit DetailGridEditor(AppSettings* settings, QWidget* parent = nullptr)
        : QWidget(parent), _settings(settings)
    {
        _rows  = settings->rows();
        _cols  = settings->cols();
        _state = settings->detailGrid();

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);

        // ── Dimension controls ────────────────────────────────────────
        auto* dimRow = new QHBoxLayout;
        _rowsSpin = new QSpinBox;
        _rowsSpin->setRange(AppSettings::kMinGridDim, AppSettings::kMaxGridDim);
        _rowsSpin->setValue(_rows);
        _colsSpin = new QSpinBox;
        _colsSpin->setRange(AppSettings::kMinGridDim, AppSettings::kMaxGridDim);
        _colsSpin->setValue(_cols);

        dimRow->addWidget(new QLabel("Rows:"));
        dimRow->addWidget(_rowsSpin);
        dimRow->addSpacing(16);
        dimRow->addWidget(new QLabel("Columns:"));
        dimRow->addWidget(_colsSpin);
        dimRow->addStretch();
        root->addLayout(dimRow);

        // ── Grid container ────────────────────────────────────────────
        auto* gridBox = new QGroupBox("Panel layout");
        auto* gridOuter = new QVBoxLayout(gridBox);
        _gridHost = new QWidget;
        _gridLayout = new QGridLayout(_gridHost);
        _gridLayout->setSpacing(6);
        gridOuter->addWidget(_gridHost);
        root->addWidget(gridBox, 1);

        connect(_rowsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int r) {
            _state.resize(r);
            for (auto& row : _state)
                if (row.size() != _cols)
                    row.resize(_cols, AppSettings::DetailPanel::None);
            _rows = r;
            rebuild();
        });
        connect(_colsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int c) {
            for (auto& row : _state) row.resize(c, AppSettings::DetailPanel::None);
            _cols = c;
            rebuild();
        });

        rebuild();
    }

    void commit() { _settings->setDetailGrid(_rows, _cols, _state); }

private:
    void rebuild()
    {
        // Clear existing widgets
        QLayoutItem* item;
        while ((item = _gridLayout->takeAt(0)) != nullptr) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }

        const auto panels = AppSettings::allPanels();

        for (int r = 0; r < _rows; ++r) {
            for (int c = 0; c < _cols; ++c) {
                auto* cell = new QFrame;
                cell->setFrameShape(QFrame::StyledPanel);
                cell->setFrameShadow(QFrame::Sunken);
                cell->setMinimumSize(110, 60);

                auto* lay = new QVBoxLayout(cell);
                lay->setContentsMargins(6, 4, 6, 4);
                lay->setSpacing(2);

                auto* posLabel = new QLabel(QString("[%1,%2]").arg(r + 1).arg(c + 1));
                posLabel->setStyleSheet("color: gray; font-size: 10px;");
                lay->addWidget(posLabel, 0, Qt::AlignLeft);

                auto* cb = new QComboBox;
                for (auto p : panels)
                    cb->addItem(AppSettings::panelName(p), static_cast<int>(p));
                int idx = panels.indexOf(_state[r][c]);
                if (idx < 0) idx = 0;
                cb->setCurrentIndex(idx);
                connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, r, c, cb](int) {
                    _state[r][c] = static_cast<AppSettings::DetailPanel>(
                        cb->currentData().toInt());
                });
                lay->addWidget(cb);

                _gridLayout->addWidget(cell, r, c);
            }
        }
        // Make cells share space equally
        for (int r = 0; r < _rows; ++r) _gridLayout->setRowStretch(r, 1);
        for (int c = 0; c < _cols; ++c) _gridLayout->setColumnStretch(c, 1);
    }

    AppSettings* _settings;
    QSpinBox*    _rowsSpin = nullptr;
    QSpinBox*    _colsSpin = nullptr;
    QWidget*     _gridHost = nullptr;
    QGridLayout* _gridLayout = nullptr;

    int _rows = 2, _cols = 2;
    QVector<QVector<AppSettings::DetailPanel>> _state;
};

// =====================================================================
// SettingsDialog
// =====================================================================

SettingsDialog::SettingsDialog(AppSettings* settings, QWidget* parent)
    : QDialog(parent), _settings(settings)
{
    setupUi();
}

void SettingsDialog::setupUi()
{
    setWindowTitle("Settings");
    resize(820, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);

    // ── Body: topics | pages ─────────────────────────────────────────
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    _topicList = new QListWidget;
    _topicList->setFixedWidth(180);
    _topicList->setFrameShape(QFrame::NoFrame);
    _topicList->addItem("General");
    _topicList->addItem("Star Detail View");
    _topicList->addItem("Grid Paths");
    _topicList->addItem("Lightcurve Fetching");
    _topicList->addItem("Lightcurve Fitting");
    _topicList->addItem("Updates");

    _pages = new QStackedWidget;
    _pages->addWidget(createGeneralPage());
    _pages->addWidget(createStarDetailPage());
    _pages->addWidget(createGridPathsPage());
    _pages->addWidget(createLightcurveFetchPage());
    _pages->addWidget(createLightcurveFitPage());
    _pages->addWidget(createUpdatesPage());

    connect(_topicList, &QListWidget::currentRowChanged,
            _pages, &QStackedWidget::setCurrentIndex);
    _topicList->setCurrentRow(0);

    body->addWidget(_topicList);
    body->addWidget(_pages, 1);
    root->addLayout(body, 1);

    // ── Buttons ──────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    auto* wrap = new QHBoxLayout;
    wrap->setContentsMargins(12, 6, 12, 0);
    wrap->addWidget(buttons);
    root->addLayout(wrap);

    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::apply);
}


QWidget* SettingsDialog::createGridPathsPage()
{
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 16, 16, 16);

    auto* intro = new QLabel(
        "Base directories searched recursively for stellar model grids. "
        "Both the SED fit (ISIS) and spectral fit (DIGGA) tools scan these "
        "paths for <code>grid.fits</code> markers.");
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* box = new QGroupBox("Grid base paths");
    auto* v = new QVBoxLayout(box);

    _gridPathsList = new QListWidget;
    _gridPathsList->addItems(_settings->gridBasePaths());
    _gridPathsList->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(_gridPathsList, 1);

    auto* row = new QHBoxLayout;
    auto* add  = new QPushButton("Add…");
    auto* rem  = new QPushButton("Remove");
    auto* up   = new QPushButton(QString::fromUtf8("\xE2\x86\x91"));
    auto* down = new QPushButton(QString::fromUtf8("\xE2\x86\x93"));
    up->setMaximumWidth(30); down->setMaximumWidth(30);
    row->addWidget(add); row->addWidget(rem);
    row->addWidget(up);  row->addWidget(down);
    row->addStretch();
    v->addLayout(row);

    connect(add, &QPushButton::clicked, this, [this]{
        QString d = QFileDialog::getExistingDirectory(this, "Add grid base path");
        if (!d.isEmpty()) _gridPathsList->addItem(d);
    });
    connect(rem, &QPushButton::clicked, this, [this]{
        int r = _gridPathsList->currentRow();
        if (r >= 0) delete _gridPathsList->takeItem(r);
    });
    connect(up, &QPushButton::clicked, this, [this]{
        int r = _gridPathsList->currentRow();
        if (r > 0) {
            auto* it = _gridPathsList->takeItem(r);
            _gridPathsList->insertItem(r - 1, it);
            _gridPathsList->setCurrentRow(r - 1);
        }
    });
    connect(down, &QPushButton::clicked, this, [this]{
        int r = _gridPathsList->currentRow();
        if (r >= 0 && r < _gridPathsList->count() - 1) {
            auto* it = _gridPathsList->takeItem(r);
            _gridPathsList->insertItem(r + 1, it);
            _gridPathsList->setCurrentRow(r + 1);
        }
    });

    outer->addWidget(box, 1);
    return page;
}

QWidget *SettingsDialog::createGeneralPage() {
    auto *page  = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 16, 16, 16);

    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignRight);

    // ── ISIS binary ───────────────────────────────────────────────────────
    auto *pathRow = new QHBoxLayout;
    _isisEdit     = new QLineEdit(_settings->isisBinaryPath());
    _isisEdit->setPlaceholderText(
        !IsisEnvironment::bundledBinary().isEmpty()
            ? "Leave blank to use the bundled ISIS"
            : QStandardPaths::findExecutable("isis").isEmpty()
                ? "isis not found in PATH - set explicitly"
                : "Auto-detected from PATH");
    _isisEdit->setToolTip(
        "Path to an ISIS binary. Leave blank to use the copy bundled with "
        "ASTRA (when present), otherwise ISIS is searched for on PATH.");
    auto *browseBtn = new QPushButton("Browse…");
    pathRow->addWidget(_isisEdit, 1);
    pathRow->addWidget(browseBtn);
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, "Locate ISIS binary",
                                                 _isisEdit->text());
        if (!f.isEmpty())
            _isisEdit->setText(f);
    });
    auto *resetBtn = new QPushButton("Use PATH");
    resetBtn->setToolTip("Auto-locate 'isis' on your PATH");
    pathRow->addWidget(resetBtn);
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        _isisEdit->setText(QStandardPaths::findExecutable("isis"));
    });
    form->addRow("ISIS binary:", pathRow);

    // ── sedfit binary (SEDplusplus SED fitting) ───────────────────────────
    auto *sedRow = new QHBoxLayout;
    _sedFitEdit  = new QLineEdit(_settings->sedFitBinaryPath());
    _sedFitEdit->setPlaceholderText(
        !SedFitEnvironment::resolveBinary().isEmpty()
            ? "Leave blank to use the bundled sedfit"
            : "sedfit not found - build ASTRA with the SEDplusplus submodule "
              "or set explicitly");
    _sedFitEdit->setToolTip(
        "Path to a sedfit binary (SEDplusplus). Leave blank to use the copy "
        "built and bundled with ASTRA, otherwise sedfit is searched for on "
        "PATH.");
    auto *sedBrowseBtn = new QPushButton("Browse…");
    sedRow->addWidget(_sedFitEdit, 1);
    sedRow->addWidget(sedBrowseBtn);
    connect(sedBrowseBtn, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, "Locate sedfit binary",
                                                 _sedFitEdit->text());
        if (!f.isEmpty())
            _sedFitEdit->setText(f);
    });
    form->addRow("sedfit binary:", sedRow);

    // ── ADS API token ─────────────────────────────────────────────────────
    _adsTokenEdit = new QLineEdit(_settings->adsApiToken());
    _adsTokenEdit->setEchoMode(QLineEdit::Password);
    _adsTokenEdit->setPlaceholderText("ADS API token (optional)");
    auto *showAdsToken = new QCheckBox("Show");
    connect(showAdsToken, &QCheckBox::toggled, this, [this](bool on) {
        _adsTokenEdit->setEchoMode(on ? QLineEdit::Normal
                                      : QLineEdit::Password);
    });
    auto *adsLink = new QLabel(
        "<a href=\"https://ui.adsabs.harvard.edu/user/settings/token\">"
        "Get token</a>");
    adsLink->setOpenExternalLinks(true);
    {
        auto *row = new QHBoxLayout;
        row->addWidget(_adsTokenEdit, 1);
        row->addWidget(showAdsToken);
        row->addWidget(adsLink);
        form->addRow("ADS API token:", row);
    }

    outer->addLayout(form);

    auto *hint = new QLabel("<i>ISIS is used for spectral fitting; sedfit "
                            "(SEDplusplus) performs the photometric SED "
                            "fitting</i>");
    hint->setStyleSheet("color: gray;");
    outer->addWidget(hint);

    outer->addStretch();
    return page;
}

QWidget* SettingsDialog::createStarDetailPage()
{
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 16, 16, 16);

    auto* intro = new QLabel(
        "Configure the default layout of panels in the Star Detail view. "
        "You can have between 1 and 4 rows and columns. Choose what to display "
        "in each cell - empty cells collapse at view time.");
    intro->setWordWrap(true);
    outer->addWidget(intro);

    _gridEditor = new DetailGridEditor(_settings);
    outer->addWidget(_gridEditor, 1);

    return page;
}

QWidget* SettingsDialog::createLightcurveFetchPage()
{
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 16, 16, 16);

    auto* intro = new QLabel(
        "The Lightcurve Fetch dialog uses the bundled "
        "<i>lightcurvequery</i> Python tool. The easiest way to get going is "
        "<b>Set up bundled environment</b> below, which unpacks the scripts and "
        "builds a self-contained Python environment with all required packages. "
        "Alternatively, point the fields here at your own interpreter and "
        "<code>lightcurvequery.py</code>.");
    intro->setWordWrap(true);
    outer->addWidget(intro);

    // ── One-click bundled setup ─────────────────────────────────────────
    {
        auto* setupRow = new QHBoxLayout;
        auto* setupBtn = new QPushButton(
            LcqueryEnvironment::isProvisioned()
                ? tr("Reinstall bundled environment…")
                : tr("Set up bundled environment…"));
        setupBtn->setEnabled(LcqueryEnvironment::bundleAvailable());
        if (!LcqueryEnvironment::bundleAvailable())
            setupBtn->setToolTip(tr("This build does not ship the lightcurvequery "
                                    "sources."));
        connect(setupBtn, &QPushButton::clicked, this, [this, setupBtn] {
            LcquerySetupDialog dlg(_settings, this);
            dlg.exec();
            // Reflect any newly written paths in the editable fields.
            _lcqPythonEdit->setText(_settings->lcqueryPython());
            _lcqScriptEdit->setText(_settings->lcqueryScript());
            setupBtn->setText(LcqueryEnvironment::isProvisioned()
                                  ? tr("Reinstall bundled environment…")
                                  : tr("Set up bundled environment…"));
        });
        setupRow->addWidget(setupBtn);
        setupRow->addStretch();
        outer->addLayout(setupRow);
    }

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);

    auto makePathRow = [&](QLineEdit*& edit,
                           const QString& current,
                           const QString& placeholder,
                           bool   pickFile,
                           const QString& filter = {}) -> QHBoxLayout*
    {
        auto* row = new QHBoxLayout;
        edit = new QLineEdit(current);
        edit->setPlaceholderText(placeholder);
        auto* browse = new QPushButton("Browse…");
        row->addWidget(edit, 1);
        row->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, edit, pickFile, filter] {
            QString start = edit->text().isEmpty()
                ? QDir::homePath() : edit->text();
            QString f = pickFile
                ? QFileDialog::getOpenFileName(this, "Locate file", start, filter)
                : QFileDialog::getExistingDirectory(this, "Locate directory", start);
            if (!f.isEmpty()) edit->setText(f);
        });
        return row;
    };

    // Python interpreter
    QString pyHint = QStandardPaths::findExecutable("python3").isEmpty()
        ? "python3 not found in PATH - set explicitly"
        : "Auto-detected from PATH";
    form->addRow("Python:",
        makePathRow(_lcqPythonEdit, _settings->lcqueryPython(),
                    pyHint, true,
                    "Executables (python python3 *.exe);;All files (*)"));

    // lightcurvequery script
    form->addRow("lightcurvequery.py:",
        makePathRow(_lcqScriptEdit, _settings->lcqueryScript(),
                    "Path to bundled lightcurvequery.py", true,
                    "Python scripts (*.py);;All files (*)"));

    // ATLAS token (password-style)
    _atlasTokenEdit = new QLineEdit(_settings->atlasToken());
    _atlasTokenEdit->setEchoMode(QLineEdit::Password);
    _atlasTokenEdit->setPlaceholderText("ATLAS forced-photometry token (optional)");
    auto* showToken = new QCheckBox("Show");
    connect(showToken, &QCheckBox::toggled, this, [this](bool on){
        _atlasTokenEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    {
        auto* row = new QHBoxLayout;
        row->addWidget(_atlasTokenEdit, 1);
        row->addWidget(showToken);
        form->addRow("ATLAS token:", row);
    }

    // BlackGEM script
    form->addRow("BlackGEM script:",
        makePathRow(_blackgemEdit, _settings->blackgemScript(),
                    "Path to query_fullsource.py (leave blank to disable)", true,
                    "Python scripts (*.py);;All files (*)"));

    outer->addLayout(form);

    // ── Test row ────────────────────────────────────────────────────
    auto* testRow = new QHBoxLayout;
    auto* testBtn = new QPushButton("Test setup");
    testBtn->setToolTip("Probe the configured Python and verify "
                        "all required packages are importable.");
    testRow->addWidget(testBtn);
    testRow->addStretch();
    outer->addLayout(testRow);

    _lcqTestResult = new QLabel;
    _lcqTestResult->setWordWrap(true);
    _lcqTestResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(_lcqTestResult);

    connect(testBtn, &QPushButton::clicked, this, [this, testBtn] {
        _lcqTestResult->setStyleSheet("color: gray;");
        _lcqTestResult->setText("Testing…");
        testBtn->setEnabled(false);

        auto* f = new LightcurveFetcher(this);
        f->setPython(_lcqPythonEdit->text().trimmed());
        f->setScript(_lcqScriptEdit->text().trimmed());

        connect(f, &LightcurveFetcher::availabilityChecked,
                this, [this, f, testBtn](bool ok, const QString& msg) {
            if (ok) {
                _lcqTestResult->setStyleSheet("color: #7dbd5e;");
                _lcqTestResult->setText("✓ All checks passed.");
            } else {
                _lcqTestResult->setStyleSheet("color: #c46060;");
                _lcqTestResult->setText("⚠ " + msg);
            }
            testBtn->setEnabled(true);
            f->deleteLater();
        });

        f->checkAvailableAsync();
    });

    auto* hint = new QLabel(
        "<i>ATLAS token and BlackGEM script are passed to the child process as "
        "environment variables (<code>ATLASFORCED_SECRET_KEY</code>, "
        "<code>BLACKGEM_QUERYSCRIPT_LOCATION</code>).</i>");
    hint->setStyleSheet("color: gray;");
    hint->setWordWrap(true);
    outer->addWidget(hint);

    outer->addStretch();
    return page;
}

void SettingsDialog::apply()
{
    _settings->setIsisBinaryPath(_isisEdit->text().trimmed());
    _settings->setSedFitBinaryPath(_sedFitEdit->text().trimmed());
    QStringList paths;
    for (int i = 0; i < _gridPathsList->count(); ++i)
        paths << _gridPathsList->item(i)->text();
    _settings->setGridBasePaths(paths);
    _gridEditor->commit();

    // Lightcurve fetching
    _settings->setLcqueryPython (_lcqPythonEdit->text().trimmed());
    _settings->setLcqueryScript (_lcqScriptEdit->text().trimmed());
    _settings->setAdsApiToken   (_adsTokenEdit->text().trimmed());
    _settings->setAtlasToken    (_atlasTokenEdit->text().trimmed());
    _settings->setBlackgemScript(_blackgemEdit->text().trimmed());
    _settings->setLcurveDir(_lcurveDirEdit->text().trimmed());

    if (_updateOnStartup)
        _settings->setCheckUpdatesOnStartup(_updateOnStartup->isChecked());
}

QWidget *SettingsDialog::createLightcurveFitPage() {
  auto *page = new QWidget;
  auto *outer = new QVBoxLayout(page);
  outer->setContentsMargins(16, 16, 16, 16);

  auto *intro = new QLabel(
      "Path to the directory containing the <code>lcurve_levmarq</code>, "
      "<code>lcurve_mcmc</code> and <code>lcurve_simplex</code> binaries. "
      "Leave blank to search <code>PATH</code> automatically.");
  intro->setWordWrap(true);
  outer->addWidget(intro);

  auto *form = new QFormLayout;
  form->setLabelAlignment(Qt::AlignRight);

  auto *pathRow = new QHBoxLayout;
  _lcurveDirEdit = new QLineEdit(_settings->lcurveDir());
  _lcurveDirEdit->setPlaceholderText(
      QStandardPaths::findExecutable("lcurve_levmarq").isEmpty()
          ? "lcurve binaries not found in PATH - set explicitly"
          : "Auto-detected from PATH");
  auto *browse = new QPushButton("Browse…");
  auto *reset = new QPushButton("Use PATH");
  pathRow->addWidget(_lcurveDirEdit, 1);
  pathRow->addWidget(browse);
  pathRow->addWidget(reset);
  form->addRow("Install dir:", pathRow);

  connect(browse, &QPushButton::clicked, this, [this] {
    QString start = _lcurveDirEdit->text().isEmpty() ? QDir::homePath()
                                                     : _lcurveDirEdit->text();
    QString d = QFileDialog::getExistingDirectory(
        this, "Locate lcurve install directory", start);
    if (!d.isEmpty()) {
      _lcurveDirEdit->setText(d);
    }
  });
  connect(reset, &QPushButton::clicked, this,
          [this] { _lcurveDirEdit->clear(); });

  outer->addLayout(form);

  auto *testBtn = new QPushButton("Test");
  testBtn->setToolTip("Check which lcurve binaries can be found "
                      "in the configured directory or PATH.");
  auto *row = new QHBoxLayout;
  row->addWidget(testBtn);
  row->addStretch();
  outer->addLayout(row);

  _lcurveStatusLbl = new QLabel;
  _lcurveStatusLbl->setTextFormat(Qt::RichText);
  _lcurveStatusLbl->setWordWrap(true);
  _lcurveStatusLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(_lcurveStatusLbl);

  auto refreshStatus = [this] {
    const QString dir = _lcurveDirEdit->text().trimmed();
    AppSettings probe;
    probe.setLcurveDir(dir); // does not touch persisted settings of _settings
    const QStringList bins = {"lcurve_levmarq", "lcurve_mcmc",
                              "lcurve_simplex"};
    QStringList rows;
    int found = 0;
    for (const QString &b : bins) {
      const QString p = probe.lcurveBinary(b);
      if (p.isEmpty()) {
        rows << QString("<span style='color:#c46060;'>✗ %1</span> - not found")
                    .arg(b);
      } else {
        rows << QString("<span style='color:#7dbd5e;'>✓ %1</span> - %2")
                    .arg(b, p.toHtmlEscaped());
        ++found;
      }
    }
    QString colour = (found == bins.size()) ? "#7dbd5e"
                     : (found > 0)          ? "#dca84d"
                                            : "#c46060";
    _lcurveStatusLbl->setText(
        QString("<b style='color:%1;'>%2 / %3 binaries available</b><br>%4")
            .arg(colour)
            .arg(found)
            .arg(bins.size())
            .arg(rows.join("<br>")));
  };

  connect(testBtn, &QPushButton::clicked, this, refreshStatus);
  connect(_lcurveDirEdit, &QLineEdit::textChanged, this, refreshStatus);
  refreshStatus();

  auto *hint = new QLabel(
      "<i>The dialog launched from the Light Curves view runs one of "
      "<code>lcurve_levmarq</code> (fast point fit), "
      "<code>lcurve_simplex</code> (robust minimiser) or "
      "<code>lcurve_mcmc</code> (full posterior) against a generated "
      "<code>config.json</code> in a temporary directory.</i>");
  hint->setWordWrap(true);
  hint->setStyleSheet("color: gray;");
  outer->addWidget(hint);

  outer->addStretch();
  return page;
}

// =====================================================================
// Updates page
// =====================================================================

QWidget* SettingsDialog::createUpdatesPage()
{
    auto* page  = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 16, 16, 16);

    auto* intro = new QLabel(
        "ASTRA can check GitHub for new releases. When you run the official "
        "<code>.AppImage</code>, available updates can be downloaded and "
        "installed in place.");
    intro->setWordWrap(true);
    outer->addWidget(intro);

    // Current version / channel.
    auto* verLbl = new QLabel(
        QString("Current version: <b>%1</b>%2")
            .arg(UpdateManager::currentVersion().toHtmlEscaped(),
                 UpdateManager::isAppImage() ? "  (AppImage)" : ""));
    verLbl->setTextFormat(Qt::RichText);
    outer->addWidget(verLbl);

    _updateOnStartup = new QCheckBox("Check for updates automatically on startup");
    _updateOnStartup->setChecked(_settings->checkUpdatesOnStartup());
    outer->addWidget(_updateOnStartup);

    if (!UpdateManager::isReleaseBuild()) {
        auto* devHint = new QLabel(
            "<i>This is a development build; automatic update prompts are "
            "disabled, but you can still check what the latest release is.</i>");
        devHint->setWordWrap(true);
        devHint->setStyleSheet("color: gray;");
        outer->addWidget(devHint);
    }

    auto* btnRow = new QHBoxLayout;
    _updateCheckBtn = new QPushButton("Check now");
    _updateInstallBtn = new QPushButton("Download && Install");
    _updateInstallBtn->setVisible(false);
    btnRow->addWidget(_updateCheckBtn);
    btnRow->addWidget(_updateInstallBtn);
    btnRow->addStretch();
    outer->addLayout(btnRow);

    _updateStatus = new QLabel;
    _updateStatus->setWordWrap(true);
    _updateStatus->setTextFormat(Qt::RichText);
    _updateStatus->setOpenExternalLinks(true);
    _updateStatus->setTextInteractionFlags(Qt::TextBrowserInteraction);
    outer->addWidget(_updateStatus);

    _updater = new UpdateManager(this);

    connect(_updater, &UpdateManager::checkStarted, this, [this] {
        _updateCheckBtn->setEnabled(false);
        _updateInstallBtn->setVisible(false);
        _updateStatus->setStyleSheet("color: gray;");
        _updateStatus->setText("Checking for updates…");
    });
    connect(_updater, &UpdateManager::upToDate, this,
            [this](const QString& current) {
        _updateCheckBtn->setEnabled(true);
        const UpdateInfo& latest = _updater->latestInfo();
        if (!UpdateManager::isReleaseBuild() && !latest.tagName.isEmpty()) {
            _updateStatus->setStyleSheet("color: gray;");
            _updateStatus->setText(
                QString("Latest release is <b>%1</b>. You are on development "
                        "build %2.")
                    .arg(latest.tagName.toHtmlEscaped(), current.toHtmlEscaped()));
        } else {
            _updateStatus->setStyleSheet("color: #7dbd5e;");
            _updateStatus->setText("✓ You are running the latest version.");
        }
    });
    connect(_updater, &UpdateManager::checkFailed, this,
            [this](const QString& err) {
        _updateCheckBtn->setEnabled(true);
        _updateStatus->setStyleSheet("color: #c46060;");
        _updateStatus->setText("⚠ Update check failed: " + err.toHtmlEscaped());
    });
    connect(_updater, &UpdateManager::updateAvailable, this,
            [this](const UpdateInfo& info) {
        _updateCheckBtn->setEnabled(true);
        _updateStatus->setStyleSheet("color: #dca84d;");
        _updateStatus->setText(
            QString("A new version <b>%1</b> is available "
                    "(<a href=\"%2\">release notes</a>).")
                .arg(info.version.toHtmlEscaped(), info.htmlUrl.toHtmlEscaped()));
        if (UpdateManager::isAppImage() && info.hasAppImage()) {
            _updateInstallBtn->setVisible(true);
            _updateInstallBtn->setEnabled(true);
            disconnect(_updateInstallBtn, &QPushButton::clicked, nullptr, nullptr);
            connect(_updateInstallBtn, &QPushButton::clicked, this,
                    [this, info] { startUpdateInstall(info); });
        } else {
            _updateInstallBtn->setVisible(false);
        }
    });

    connect(_updateCheckBtn, &QPushButton::clicked, this, [this] {
        // Manual check ignores any skipped-version marker.
        _updater->checkForUpdates(/*respectSkip=*/false);
    });

    outer->addStretch();
    return page;
}

void SettingsDialog::startUpdateInstall(const UpdateInfo& info)
{
    auto* progress = new QProgressDialog(
        QString("Downloading ASTRA %1…").arg(info.version),
        "Cancel", 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);

    connect(_updater, &UpdateManager::downloadProgress, progress,
            [progress](qint64 received, qint64 total) {
        if (total > 0) {
            progress->setMaximum(100);
            progress->setValue(int(received * 100 / total));
        } else {
            progress->setMaximum(0);  // indeterminate
        }
    });
    connect(progress, &QProgressDialog::canceled, _updater,
            &UpdateManager::cancelDownload);

    connect(_updater, &UpdateManager::installFailed, progress,
            [this, progress](const QString& err) {
        progress->close();
        progress->deleteLater();
        QMessageBox::warning(this, "Update failed", err);
    });
    connect(_updater, &UpdateManager::installFinished, progress,
            [this, progress, info](const QString&) {
        progress->close();
        progress->deleteLater();
        const auto btn = QMessageBox::information(
            this, "Update installed",
            QString("ASTRA %1 has been installed.\n\n"
                    "Restart now to use the new version?").arg(info.version),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (btn == QMessageBox::Yes)
            UpdateManager::relaunch();
    });

    _updateInstallBtn->setEnabled(false);
    _updater->downloadAndInstall(info);
}