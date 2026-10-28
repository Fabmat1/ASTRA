#include "ai/ui/ActivityDialog.h"

#include "ai/AuditLog.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace astra::ai::ui {

ActivityDialog::ActivityDialog(AuditLog* log, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("AI activity"));
    resize(980, 560);
    auto* lay = new QVBoxLayout(this);
    _tree = new QTreeWidget;
    _tree->setHeaderLabels({tr("Time"), tr("Agent"), tr("Tool"), tr("What"), tr("Permitted by"), tr("Result")});
    _tree->setRootIsDecorated(true);
    _tree->setAlternatingRowColors(true);
    _tree->setUniformRowHeights(true);
    _tree->header()->setStretchLastSection(true);
    lay->addWidget(_tree, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* folder = buttons->addButton(tr("Open log folder"), QDialogButtonBox::ActionRole);
    connect(folder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AuditLog::directory()));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    const QJsonArray recent = log->recent(500);
    for (int i = recent.size() - 1; i >= 0; --i) addEntry(recent[i].toObject(), false);
    connect(log, &AuditLog::recorded, this, [this](const QJsonObject& o) { addEntry(o, true); });
    for (int c = 0; c < 5; ++c) _tree->resizeColumnToContents(c);
}

void ActivityDialog::addEntry(const QJsonObject& o, bool atTop)
{
    const QDateTime t = QDateTime::fromString(o.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs).toLocalTime();
    const bool ok = o.value(QStringLiteral("ok")).toBool();
    const QString decision = o.value(QStringLiteral("decision")).toString();
    QString result = decision == QLatin1String("deny") ? tr("refused") : ok ? tr("ok") : tr("failed");
    auto* item = new QTreeWidgetItem({t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                      o.value(QStringLiteral("caller")).toString(),
                                      o.value(QStringLiteral("tool")).toString(),
                                      o.value(QStringLiteral("summary")).toString().left(200),
                                      o.value(QStringLiteral("decisionSource")).toString(),
                                      result});
    auto* detail = new QTreeWidgetItem(item);
    detail->setText(3, QString::fromUtf8(QJsonDocument(o.value(QStringLiteral("args")).toObject()).toJson(QJsonDocument::Indented)) +
                           QStringLiteral("\n") + o.value(QStringLiteral("result")).toString());
    detail->setFirstColumnSpanned(false);
    if (atTop) _tree->insertTopLevelItem(0, item);
    else _tree->addTopLevelItem(item);
}

} // namespace astra::ai::ui
