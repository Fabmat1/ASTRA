#include "SEDFitDialog.h"
#include "models/Star.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

SEDFitDialog::SEDFitDialog(std::shared_ptr<Star> star, QWidget* parent)
    : QDialog(parent)
    , _star(star)
{
    setupUi();
}

SEDFitDialog::~SEDFitDialog() = default;

void SEDFitDialog::setupUi()
{
    setWindowTitle(QString("SED Analysis — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(900, 600);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* placeholder = new QLabel(
        "🚧  SED Fit\n\n"
        "This tool will allow you to:\n"
        "  • Display broadband photometry as an SED\n"
        "  • Fit blackbody or model atmosphere SEDs\n"
        "  • Derive angular diameter and radius\n"
        "  • Estimate reddening E(B-V)\n"
        "  • Compare with Gaia parallax distance\n\n"
        "Not yet implemented.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: gray; font-size: 14px;");
    layout->addWidget(placeholder, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    LOG_INFO("Tools", QString("SED Fit dialog opened for star %1").arg(_star->getSourceId()));
}