#include "CMDDialog.h"
#include "models/Star.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

CMDDialog::CMDDialog(std::shared_ptr<Star> star, QWidget* parent)
    : QDialog(parent)
    , _star(star)
{
    setupUi();
}

CMDDialog::~CMDDialog() = default;

void CMDDialog::setupUi()
{
    setWindowTitle(QString("Colour-Magnitude Diagram — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(700, 700);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* placeholder = new QLabel(
        "🚧  Colour-Magnitude Diagram\n\n"
        "This tool will allow you to:\n"
        "  • Plot the star on a Gaia CMD\n"
        "  • Overlay theoretical isochrones\n"
        "  • Compare with project sample\n"
        "  • Toggle absolute / apparent magnitudes\n\n"
        "Not yet implemented.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: gray; font-size: 14px;");
    layout->addWidget(placeholder, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    LOG_INFO("Tools", QString("CMD dialog opened for star %1").arg(_star->getSourceId()));
}