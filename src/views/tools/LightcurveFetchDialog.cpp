#include "LightcurveFetchDialog.h"
#include "models/Star.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

LightcurveFetchDialog::LightcurveFetchDialog(std::shared_ptr<Star> star, QWidget* parent)
    : QDialog(parent)
    , _star(star)
{
    setupUi();
}

LightcurveFetchDialog::~LightcurveFetchDialog() = default;

void LightcurveFetchDialog::setupUi()
{
    setWindowTitle(QString("Light Curves — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(900, 600);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* placeholder = new QLabel(
        "🚧  Light Curve Fetch & Fit\n\n"
        "This tool will allow you to:\n"
        "  • Query TESS, ZTF, ASAS-SN for photometry\n"
        "  • Download and display light curves\n"
        "  • Fit periodic models\n"
        "  • Phase-fold on RV or LC periods\n"
        "  • Detect eclipses and transits\n\n"
        "Not yet implemented.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: gray; font-size: 14px;");
    layout->addWidget(placeholder, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    LOG_INFO("Tools", QString("Lightcurve dialog opened for star %1").arg(_star->getSourceId()));
}