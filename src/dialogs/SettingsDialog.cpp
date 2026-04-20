#include "SettingsDialog.h"
#include "utils/AppSettings.h"

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

    _pages = new QStackedWidget;
    _pages->addWidget(createGeneralPage());
    _pages->addWidget(createStarDetailPage());

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

void SettingsDialog::apply()
{
    _settings->setIsisBinaryPath(_isisEdit->text().trimmed());
    _gridEditor->commit();
}