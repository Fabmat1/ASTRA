#include "PeriodogramRecord.h"
#include "utils/DataStore.h"
#include <QDataStream>
#include <QSet>
#include <QStringList>
#include <QIODevice>

bool PeriodogramRecord::saveDataToFile(const QString& filepath)
{
    QByteArray buf;
    {
        QDataStream s(&buf, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << static_cast<quint32>(1);                       // format version
        s << result.grid.f0 << result.grid.df;
        s << static_cast<quint32>(result.grid.Nf);
        s << static_cast<qint32>(result.nPoints);
        s << result.label;
        for (int i = 0; i < result.grid.Nf; ++i) s << result.power[i];
    }
    return DataStore::writeCompressed(filepath, DataStore::PeriodogramData, buf);
}

bool PeriodogramRecord::loadDataFromFile(const QString& filepath)
{
    QByteArray buf;
    if (!DataStore::readCompressed(filepath, DataStore::PeriodogramData, buf))
        return false;

    QDataStream s(&buf, QIODevice::ReadOnly);
    s.setVersion(QDataStream::Qt_6_0);
    quint32 version; s >> version;
    if (version != 1) return false;

    quint32 nf;  qint32 npts;
    s >> result.grid.f0 >> result.grid.df >> nf >> npts >> result.label;
    result.grid.Nf = static_cast<int>(nf);
    result.nPoints = npts;
    result.power.resize(nf);
    result.frequency.resize(nf);
    for (quint32 i = 0; i < nf; ++i) {
        s >> result.power[i];
        result.frequency[i] = result.grid.f0 + i * result.grid.df;
    }
    return s.status() == QDataStream::Ok;
}

namespace PeriodogramUtils {

Periodogram::Result combineForSource(
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
    const QString& source)
{
    QList<Periodogram::Result> parts;
    for (const auto& r : records)
        if (r && r->source == source && r->result.isValid())
            parts.append(r->result);
    return Periodogram::weightedSum(parts, source);
}

Periodogram::Result combineForStar(
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records)
{
    QStringList sources;
    QSet<QString> seen;
    for (const auto& r : records) {
        if (!r || !r->result.isValid()) continue;
        if (!seen.contains(r->source)) { seen.insert(r->source); sources << r->source; }
    }
    QList<Periodogram::Result> srcResults;
    for (const QString& s : sources)
        srcResults.append(combineForSource(records, s));
    return Periodogram::multiplied(srcResults, "Combined");
}

}