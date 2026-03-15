#include "SpectraFitDialog.h"
#include "models/Star.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

SpectraFitDialog::SpectraFitDialog(std::shared_ptr<Star> star, QWidget* parent)
    : QDialog(parent)
    , _star(star)
{
    setupUi();
}

SpectraFitDialog::~SpectraFitDialog() = default;

void SpectraFitDialog::setupUi()
{
    setWindowTitle(QString("Spectral Analysis — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1000, 700);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* placeholder = new QLabel(
        "🚧  Spectral Analysis\n\n"
        "This tool will allow you to:\n"
        "  • Fit model atmospheres to observed spectra\n"
        "  • Determine Teff, log g, abundances\n"
        "  • Measure radial velocities via cross-correlation\n"
        "  • Compare multiple model grids\n"
        "  • Normalize and co-add spectra\n\n"
        "Not yet implemented.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: gray; font-size: 14px;");
    layout->addWidget(placeholder, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    LOG_INFO("Tools", QString("Spectra Fit dialog opened for star %1").arg(_star->getSourceId()));
}