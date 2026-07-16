#include "KinematicContours.h"

#include <QFile>
#include <QHash>
#include <QTextStream>

namespace GalKin {

namespace {

ContourCurve loadCsv(const QString& path)
{
    ContourCurve c;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return c;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const auto p = line.split(',');
        if (p.size() < 2)
            continue;
        bool okX = false, okY = false;
        const double x = p[0].toDouble(&okX);
        const double y = p[1].toDouble(&okY);
        if (okX && okY) {
            c.x.append(x);
            c.y.append(y);
        }
    }
    return c;
}

QString fileFor(Diagram d, int pop)
{
    static const char* popName[] = {"thin", "thick", "halo"};
    if (pop < 0 || pop > 2)
        return {};
    switch (d) {
    case Diagram::Toomre:
        return QString(":/data/kinematics/toomre_%1.csv").arg(popName[pop]);
    case Diagram::UV:
        return QString(":/data/kinematics/uv_%1.csv").arg(popName[pop]);
    case Diagram::WV:
        return QString(":/data/kinematics/wv_%1.csv").arg(popName[pop]);
    case Diagram::UW:
        return QString(":/data/kinematics/uw_%1.csv").arg(popName[pop]);
    case Diagram::JzE:
        return pop == 1 ? QStringLiteral(
                              ":/data/kinematics/jze_parallelogram.csv")
                        : QString();
    }
    return {};
}

} // namespace

const ContourCurve& kinematicContour(Diagram d, int pop)
{
    static QHash<int, ContourCurve> cache;
    const int key = int(d) * 4 + pop;
    auto it = cache.find(key);
    if (it == cache.end())
        it = cache.insert(key, loadCsv(fileFor(d, pop)));
    return *it;
}

} // namespace GalKin
