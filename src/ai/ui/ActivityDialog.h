#pragma once

/*  Everything AI agents did, newest first: when, who, which tool, what it was
 *  asked to do, how it was permitted and how it ended. Reads the audit log
 *  and follows new entries while open.                                     */

#include <QDialog>

class QTreeWidget;
class QJsonObject;

namespace astra::ai { class AuditLog; }

namespace astra::ai::ui {

class ActivityDialog : public QDialog {
    Q_OBJECT
public:
    ActivityDialog(AuditLog* log, QWidget* parent);

private:
    void addEntry(const QJsonObject& o, bool atTop);
    QTreeWidget* _tree;
};

} // namespace astra::ai::ui
