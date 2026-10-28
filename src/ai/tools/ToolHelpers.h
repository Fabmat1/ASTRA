#pragma once

/*  Shared pieces of the tool implementations: argument resolution (projects
 *  by id or name, stars by id), compact JSON for stars, and downsampling of
 *  long arrays for tool results.                                            */

#include "ai/AiHost.h"
#include "ai/Tool.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <memory>
#include <vector>

class Project;
class Star;

namespace astra::ai::tools {

/// The project named by args["project"] (id or name), or the open one.
std::shared_ptr<Project> resolveProject(AiHost& host, const ToolCall& call,
                                        const QString& key = QStringLiteral("project"));

/// One star from args[key]; throws ToolError when it does not exist.
std::shared_ptr<Star> resolveStar(AiHost& host, const ToolCall& call, QString* projectId = nullptr,
                                  const QString& key = QStringLiteral("star_id"));

/// args["star_ids"] (array) or args["star_id"]; throws when empty.
QStringList starIdArg(const ToolCall& call);

/// id, alias, source id, J-name, ra, dec: enough to tell stars apart.
QJsonObject starBrief(const Star& s);

/// Selected fields (all non-null when `fields` is empty).
QJsonObject starFields(const Star& s, const QStringList& fields = {});

/// Every n-th sample so at most maxPoints remain (keeps first and last).
std::vector<size_t> decimationIndices(size_t n, size_t maxPoints);
QJsonArray toJsonArray(const std::vector<double>& v, const std::vector<size_t>& idx);

/// NaN-safe number for JSON (NaN and inf become null).
QJsonValue num(double v);

} // namespace astra::ai::tools
