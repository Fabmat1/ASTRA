#include "SettingsDialog.h"
#include "utils/AppSettings.h"
#include "utils/LightcurveFetcher.h"

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
// DetailGridEditor — nested widget (file-private)
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
    
    _pages = new QStackedWidget;
    _pages->addWidget(createGeneralPage());
    _pages->addWidget(createStarDetailPage());
    _pages->addWidget(createGridPathsPage());
    _pages->addWidget(createLightcurveFetchPage());

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


QWidget* SettingsDialog::createGeneralPage()
{
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 16, 16, 16);

    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignRight);

    auto* pathRow = new QHBoxLayout;
    _isisEdit = new QLineEdit(_settings->isisBinaryPath());
    _isisEdit->setPlaceholderText(
    QStandardPaths::findExecutable("isis").isEmpty()
        ? "isis not found in PATH — set explicitly"
        : "Auto-detected from PATH");
    auto* browseBtn = new QPushButton("Browse…");
    pathRow->addWidget(_isisEdit, 1);
    pathRow->addWidget(browseBtn);

    connect(browseBtn, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, "Locate ISIS binary",
                                                 _isisEdit->text());
        if (!f.isEmpty()) _isisEdit->setText(f);
    });

    auto* resetBtn = new QPushButton("Use PATH");
    resetBtn->setToolTip("Auto-locate 'isis' on your PATH");
    pathRow->addWidget(resetBtn);
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        _isisEdit->setText(QStandardPaths::findExecutable("isis"));
    });

    form->addRow("ISIS binary:", pathRow);
    outer->addLayout(form);

    auto* hint = new QLabel(
        "<i>Path to the ISIS executable used e.g. for spectal and SED fitting</i>");
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
        "in each cell — empty cells collapse at view time.");
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
        "<i>lightcurvequery</i> Python tool "
        "(<code>external/lightcurvequery</code>). "
        "The selected Python interpreter must have its "
        "dependencies installed:<br>"
        "<code>python -m pip install -r external/lightcurvequery/requirements.txt</code>");
    intro->setWordWrap(true);
    outer->addWidget(intro);

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
        ? "python3 not found in PATH — set explicitly"
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
    QStringList paths;
    for (int i = 0; i < _gridPathsList->count(); ++i)
        paths << _gridPathsList->item(i)->text();
    _settings->setGridBasePaths(paths);
    _gridEditor->commit();

    // Lightcurve fetching
    _settings->setLcqueryPython (_lcqPythonEdit->text().trimmed());
    _settings->setLcqueryScript (_lcqScriptEdit->text().trimmed());
    _settings->setAtlasToken    (_atlasTokenEdit->text().trimmed());
    _settings->setBlackgemScript(_blackgemEdit->text().trimmed());
}