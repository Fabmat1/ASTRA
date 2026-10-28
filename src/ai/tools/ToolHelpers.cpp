#include "ai/tools/ToolHelpers.h"

#include "core/Project.h"
#include "core/Star.h"
#include "core/StarFieldRegistry.h"

#include <cmath>

namespace astra::ai::tools {

std::shared_ptr<Project> resolveProject(AiHost& host, const ToolCall& call, const QString& key)
{
    const QString arg = call.str(key);
    if (arg.isEmpty()) {
        if (auto cur = host.currentProject()) return cur;
        throw ToolError(QStringLiteral("No project given and none is open; pass \"%1\" "
                                       "(a project id or name, see list_projects).").arg(key));
    }
    auto p = host.project(arg);
    if (!p) throw ToolError(QStringLiteral("No project \"%1\"; see list_projects.").arg(arg));
    return p;
}

std::shared_ptr<Star> resolveStar(AiHost& host, const ToolCall& call, QString* projectId,
                                  const QString& key)
{
    const QString id = call.requireStr(key);
    auto s = host.star(id, projectId);
    if (!s)
        throw ToolError(QStringLiteral("No star with id \"%1\". Star tools take ASTRA star ids; "
                                       "use find_stars to look one up by name or position.").arg(id));
    return s;
}

QStringList starIdArg(const ToolCall& call)
{
    QStringList ids = call.strList(QStringLiteral("star_ids"));
    if (ids.isEmpty()) ids = call.strList(QStringLiteral("star_id"));
    if (ids.isEmpty()) throw ToolError(QStringLiteral("Pass star_ids (a list of star ids)."));
    ids.removeDuplicates();
    return ids;
}

QJsonValue num(double v)
{
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

QJsonObject starBrief(const Star& s)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), s.getId());
    if (!s.getAlias().isEmpty())    o.insert(QStringLiteral("alias"), s.getAlias());
    if (!s.getSourceId().isEmpty()) o.insert(QStringLiteral("source_id"), s.getSourceId());
    if (!s.getJName().isEmpty())    o.insert(QStringLiteral("jname"), s.getJName());
    o.insert(QStringLiteral("ra"), num(s.getRa()));
    o.insert(QStringLiteral("dec"), num(s.getDec()));
    return o;
}

QJsonObject starFields(const Star& s, const QStringList& fields)
{
    const auto& reg = StarFieldRegistry::instance();
    QJsonObject o;
    o.insert(QStringLiteral("id"), s.getId());
    if (fields.isEmpty()) {
        for (const auto& f : reg.all()) {
            const QJsonValue v = reg.valueJson(s, f);
            if (!v.isNull() && !v.isUndefined()) o.insert(f.key, v);
        }
        return o;
    }
    for (const QString& k : fields) {
        const QJsonValue v = reg.valueJson(s, k);
        if (v.isUndefined()) continue;
        o.insert(k, v);
    }
    return o;
}

std::vector<size_t> decimationIndices(size_t n, size_t maxPoints)
{
    std::vector<size_t> idx;
    if (n == 0) return idx;
    if (maxPoints < 2 || n <= maxPoints) {
        idx.resize(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        return idx;
    }
    idx.reserve(maxPoints);
    const double step = double(n - 1) / double(maxPoints - 1);
    for (size_t i = 0; i < maxPoints; ++i) idx.push_back(size_t(std::llround(double(i) * step)));
    return idx;
}

QJsonArray toJsonArray(const std::vector<double>& v, const std::vector<size_t>& idx)
{
    QJsonArray a;
    for (size_t i : idx) a.append(i < v.size() ? num(v[i]) : QJsonValue());
    return a;
}

} // namespace astra::ai::tools
