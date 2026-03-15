#include "GalacticOrbitDialog.h"
#include "models/Star.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

GalacticOrbitDialog::GalacticOrbitDialog(std::shared_ptr<Star> star, QWidget* parent)
    : QDialog(parent)
    , _star(star)
{
    setupUi();
}

GalacticOrbitDialog::~GalacticOrbitDialog() = default;

void GalacticOrbitDialog::setupUi()
{
    setWindowTitle(QString("Galactic Orbit — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(800, 700);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* placeholder = new QLabel(
        "🚧  Galactic Orbit Integration\n\n"
        "This tool will allow you to:\n"
        "  • Integrate stellar orbit in a Milky Way potential\n"
        "  • Compute U, V, W space velocities\n"
        "  • Determine Galactic population membership\n"
        "  • Visualize orbit in R-z and X-Y planes\n"
        "  • Estimate flight time from Galactic plane\n\n"
        "Not yet implemented.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: gray; font-size: 14px;");
    layout->addWidget(placeholder, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    LOG_INFO("Tools", QString("Galactic orbit dialog opened for star %1").arg(_star->getSourceId()));
}