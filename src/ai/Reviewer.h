#pragma once

/*  The "checking" model of auto mode: before a dangerous action (deleting
 *  data, running code, contacting remote hosts) runs without asking the user,
 *  a second model the user configured judges whether it is what the user
 *  asked for and safe, and may refuse it with a reason.                     */

#include "ai/PermissionGate.h"

#include <QObject>

namespace astra::ai {

class AiSettings;

class ModelReviewer : public QObject, public ActionReviewer {
    Q_OBJECT
public:
    explicit ModelReviewer(AiSettings* settings, QObject* parent = nullptr);

    bool available() const override;
    void review(const ToolDef& tool, const ToolCall& call, const QString& summary,
                DecisionFn done) override;

private:
    AiSettings* _settings;
};

} // namespace astra::ai
