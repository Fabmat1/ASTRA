#include "RVInspectorDialog.h"
#include "models/Star.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

RVInspectorDialog::RVInspectorDialog(std::shared_ptr<Star> star, QWidget* parent)
    : QDialog(parent)
    , _star(star)
{
    setupUi();
}

RVInspectorDialog::~RVInspectorDialog() = default;

void RVInspectorDialog::setupUi()
{
    setWindowTitle(QString("RV Inspector — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(900, 600);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* placeholder = new QLabel(
        "🚧  RV Inspector\n\n"
        "This tool will allow you to:\n"
        "  • View and edit individual RV points\n"
        "  • Add/remove measurements\n"
        "  • Apply heliocentric corrections\n"
        "  • Fit orbital solutions\n"
        "  • Adjust zero-point offsets between instruments\n\n"
        "Not yet implemented.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("color: gray; font-size: 14px;");
    layout->addWidget(placeholder, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    LOG_INFO("Tools", QString("RV Inspector opened for star %1").arg(_star->getSourceId()));
}