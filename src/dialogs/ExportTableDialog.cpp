#include "ExportTableDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

ExportTableDialog::ExportTableDialog(bool hasSelection, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Export Table"));
    setMinimumWidth(440);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 14);
    root->setSpacing(14);

    // ── What to export ───────────────────────────────────────────────────
    auto* scopeBox = new QGroupBox(tr("What to export"));
    auto* scopeLay = new QVBoxLayout(scopeBox);
    _scopeShown     = new QRadioButton(tr("Currently shown columns and rows"));
    _scopeAll       = new QRadioButton(tr("All columns and rows"));
    _scopeSelection = new QRadioButton(tr("Current selection (selected rows && columns)"));
    _scopeShown->setChecked(true);
    _scopeSelection->setEnabled(hasSelection);
    if (!hasSelection)
        _scopeSelection->setToolTip(tr("Select rows in the table first."));
    scopeLay->addWidget(_scopeShown);
    scopeLay->addWidget(_scopeAll);
    scopeLay->addWidget(_scopeSelection);
    root->addWidget(scopeBox);

    // ── Destination ──────────────────────────────────────────────────────
    auto* fmtBox = new QGroupBox(tr("Destination"));
    auto* fmtLay = new QVBoxLayout(fmtBox);
    _fmtFits      = new QRadioButton(tr("FITS binary table (.fits)"));
    _fmtCsv       = new QRadioButton(tr("CSV / text file"));
    _fmtClipboard = new QRadioButton(tr("Clipboard"));
    _fmtCsv->setChecked(true);
#ifndef HAVE_CCFITS
    _fmtFits->setEnabled(false);
    _fmtFits->setToolTip(tr("This build was compiled without CCfits support."));
#endif
    fmtLay->addWidget(_fmtFits);
    fmtLay->addWidget(_fmtCsv);
    fmtLay->addWidget(_fmtClipboard);

    auto* sepRow = new QHBoxLayout;
    _sepLabel = new QLabel(tr("Separator:"));
    _sepEdit  = new QLineEdit(QStringLiteral(","));
    _sepEdit->setMaximumWidth(80);
    _sepEdit->setToolTip(tr("Field separator for CSV / clipboard output. "
                            "Use \\t for a tab."));
    _headerCheck = new QCheckBox(tr("Include header row"));
    _headerCheck->setChecked(true);
    sepRow->addWidget(_sepLabel);
    sepRow->addWidget(_sepEdit);
    sepRow->addSpacing(16);
    sepRow->addWidget(_headerCheck);
    sepRow->addStretch();
    fmtLay->addLayout(sepRow);
    root->addWidget(fmtBox);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);

    for (auto* rb : { _fmtFits, _fmtCsv, _fmtClipboard })
        connect(rb, &QRadioButton::toggled, this,
                &ExportTableDialog::updateEnabledState);
    updateEnabledState();
}

void ExportTableDialog::updateEnabledState()
{
    // Separator / header only matter for the text-based outputs.
    const bool textual = _fmtCsv->isChecked() || _fmtClipboard->isChecked();
    _sepLabel->setEnabled(textual);
    _sepEdit->setEnabled(textual);
    _headerCheck->setEnabled(textual);
}

ExportTableDialog::Scope ExportTableDialog::scope() const
{
    if (_scopeAll->isChecked())       return Scope::All;
    if (_scopeSelection->isChecked()) return Scope::Selection;
    return Scope::Shown;
}

ExportTableDialog::Format ExportTableDialog::format() const
{
    if (_fmtFits->isChecked())      return Format::Fits;
    if (_fmtClipboard->isChecked()) return Format::Clipboard;
    return Format::Csv;
}

QString ExportTableDialog::separator() const
{
    QString s = _sepEdit->text();
    if (s.isEmpty()) return QStringLiteral(",");
    // Allow the common escape for tab.
    s.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
    return s;
}

bool ExportTableDialog::includeHeader() const
{
    return _headerCheck->isChecked();
}
