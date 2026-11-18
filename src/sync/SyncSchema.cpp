#include "sync/SyncSchema.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSqlQuery>
#include <QVariant>

namespace astra::sync {

namespace {

// Separates the parts of a composite key. A unit separator cannot occur in
// the ids and names that make up the keys.
const QChar kPkSeparator(0x1f);

// Bump when the generated triggers or shadow tables change shape, so
// SyncCapture::ensure() rebuilds them in databases that already have them.
const int kGeneratorVersion = 1;

}   // namespace

const QList<TablePolicy>& syncedTables()
{
    static const QList<TablePolicy> tables = {
        {QStringLiteral("projects"), {QStringLiteral("modified_date")}, {QStringLiteral("image_path")}},
        {QStringLiteral("instruments"), {}, {}},
        {QStringLiteral("instrument_modes"), {}, {}},
        {QStringLiteral("applied_default_instruments"), {}, {}},
        {QStringLiteral("applied_default_setups"), {}, {}},
        {QStringLiteral("stars"), {}, {}},
        {QStringLiteral("photometry"), {},
         {QStringLiteral("photometric_points_file"), QStringLiteral("sed_points_file")}},
        {QStringLiteral("photometric_points"), {}, {}},
        {QStringLiteral("lightcurves"), {}, {QStringLiteral("data_file")}},
        {QStringLiteral("sed_models"), {}, {QStringLiteral("model_data_file")}},
        {QStringLiteral("lc_fits"), {}, {QStringLiteral("data_file")}},
        // spectra.file is where the spectrum was imported from on the machine
        // that imported it; created_at is filled in by the database and reset
        // by every INSERT OR REPLACE.
        {QStringLiteral("spectra"), {QStringLiteral("created_at"), QStringLiteral("file")},
         {QStringLiteral("data_file")}},
        {QStringLiteral("spectral_fits"), {}, {QStringLiteral("model_data_file")}},
        {QStringLiteral("rv_curves"), {QStringLiteral("created_at")}, {}},
        {QStringLiteral("rv_points"), {QStringLiteral("created_at")}, {}},
        {QStringLiteral("rv_fits"), {QStringLiteral("created_at")}, {}},
        {QStringLiteral("mass_fit_plans"), {}, {}},
    };
    return tables;
}

const TablePolicy* policyFor(const QString& table)
{
    for (const auto& p : syncedTables())
        if (p.name == table) return &p;
    return nullptr;
}

Owner ownerOf(const QString& table)
{
    static const QHash<QString, Owner> owners = {
        {QStringLiteral("stars"), {QStringLiteral("project_id"), QStringLiteral("projects")}},
        {QStringLiteral("instrument_modes"), {QStringLiteral("instrument_id"), QStringLiteral("instruments")}},
        {QStringLiteral("photometry"), {QStringLiteral("star_id"), QStringLiteral("stars")}},
        {QStringLiteral("photometric_points"), {QStringLiteral("photometry_id"), QStringLiteral("photometry")}},
        {QStringLiteral("lightcurves"), {QStringLiteral("photometry_id"), QStringLiteral("photometry")}},
        {QStringLiteral("sed_models"), {QStringLiteral("photometry_id"), QStringLiteral("photometry")}},
        {QStringLiteral("lc_fits"), {QStringLiteral("lightcurve_id"), QStringLiteral("lightcurves")}},
        {QStringLiteral("spectra"), {QStringLiteral("star_id"), QStringLiteral("stars")}},
        {QStringLiteral("spectral_fits"), {QStringLiteral("spectrum_id"), QStringLiteral("spectra")}},
        {QStringLiteral("rv_curves"), {QStringLiteral("star_id"), QStringLiteral("stars")}},
        {QStringLiteral("rv_points"), {QStringLiteral("curve_id"), QStringLiteral("rv_curves")}},
        {QStringLiteral("rv_fits"), {QStringLiteral("curve_id"), QStringLiteral("rv_curves")}},
    };
    return owners.value(table);
}

QStringList bestFitGroup(const QString& table)
{
    static const QHash<QString, QStringList> groups = {
        {QStringLiteral("spectral_fits"), {QStringLiteral("spectrum_id")}},
        {QStringLiteral("rv_fits"), {QStringLiteral("curve_id")}},
        {QStringLiteral("sed_models"), {QStringLiteral("photometry_id")}},
        {QStringLiteral("lc_fits"), {QStringLiteral("lightcurve_id"), QStringLiteral("filter")}},
    };
    return groups.value(table);
}

QList<Reference> referencesTo(const QString& table)
{
    QList<Reference> refs;
    for (const TablePolicy& policy : syncedTables())
        if (const Owner owner = ownerOf(policy.name); owner.table == table)
            refs.append({policy.name, owner.column});

    // Ids held outside the ownership chain.
    if (table == QLatin1String("instruments")) {
        refs.append({QStringLiteral("spectra"), QStringLiteral("instrument_id")});
        refs.append({QStringLiteral("lightcurves"), QStringLiteral("instrument_id")});
        refs.append({QStringLiteral("photometric_points"), QStringLiteral("instrument_id")});
    } else if (table == QLatin1String("spectra")) {
        refs.append({QStringLiteral("rv_points"), QStringLiteral("spectrum_id")});
    } else if (table == QLatin1String("spectral_fits")) {
        refs.append({QStringLiteral("rv_points"), QStringLiteral("spectral_fit_id")});
    }
    return refs;
}

QString quoted(const QString& identifier)
{
    QString s = identifier;
    s.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

TableInfo inspect(const QSqlDatabase& db, const QString& table)
{
    TableInfo info;
    info.name = table;

    QList<QPair<int, QString>> pk;   // position in key, column
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(quoted(table)))) return info;
    while (q.next()) {
        const QString column = q.value(1).toString();
        info.columns << column;
        info.types << q.value(2).toString();
        if (const int pos = q.value(5).toInt(); pos > 0) pk.append({pos, column});
    }
    std::sort(pk.begin(), pk.end());
    for (const auto& [pos, column] : pk) info.pk << column;

    if (info.pk.isEmpty()) {          // nothing to identify a row by
        info.columns.clear();
        info.types.clear();
        return info;
    }

    QStringList uniqueIndexes;
    if (q.exec(QStringLiteral("PRAGMA index_list(%1)").arg(quoted(table))))
        while (q.next())
            // origin 'u' is a UNIQUE constraint, 'c' a CREATE UNIQUE INDEX; the
            // key's own index is 'pk'.
            if (q.value(2).toInt() == 1 && q.value(3).toString() != QLatin1String("pk"))
                uniqueIndexes << q.value(1).toString();
    uniqueIndexes.sort();
    for (const QString& index : uniqueIndexes) {
        QStringList columns;
        if (q.exec(QStringLiteral("PRAGMA index_info(%1)").arg(quoted(index))))
            while (q.next()) {
                const QString column = q.value(2).toString();
                if (column.isEmpty()) { columns.clear(); break; }   // an expression index
                columns << column;
            }
        if (!columns.isEmpty()) info.uniqueKeys << columns;
    }
    return info;
}

QString schemaFingerprint(const QSqlDatabase& db)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const TablePolicy& policy : syncedTables()) {
        const TableInfo t = inspect(db, policy.name);
        h.addData(policy.name.toUtf8());
        QStringList columns = t.columns;
        columns.sort();
        for (const QString& c : columns) {
            h.addData(QByteArrayView("\x1f", 1));
            h.addData(c.toUtf8());
        }
        h.addData(QByteArrayView("\x1e", 1));
    }
    return QString::fromLatin1(h.result().toHex().left(16));
}

QString TableInfo::fingerprint() const
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    auto add = [&h](const QString& s) {
        h.addData(s.toUtf8());
        h.addData(QByteArrayView("\x1e", 1));
    };
    add(QString::number(kGeneratorVersion));
    add(name);
    for (int i = 0; i < columns.size(); ++i) {
        add(columns.at(i));
        add(types.value(i));
    }
    add(QStringLiteral("pk"));
    for (const QString& c : pk) add(c);
    for (const QStringList& key : uniqueKeys) {
        add(QStringLiteral("unique"));
        for (const QString& c : key) add(c);
    }
    if (const TablePolicy* policy = policyFor(name))
        for (const QString& c : policy->frozen) add(QStringLiteral("frozen:") + c);
    return QString::fromLatin1(h.result().toHex());
}

namespace {

// Blocks of the stars row that one computation writes as a whole.
QString starBlock(const QString& column)
{
    static const QList<QPair<QString, QString>> blocks = {
        {QStringLiteral("rv_"), QStringLiteral("@rv")},
        {QStringLiteral("sed_"), QStringLiteral("@sed")},
        {QStringLiteral("phot_"), QStringLiteral("@phot")},
        {QStringLiteral("gal_"), QStringLiteral("@gal")},
        {QStringLiteral("comp_"), QStringLiteral("@comp")},
    };
    const QString c = column.startsWith(QLatin1String("e_")) ? column.mid(2) : column;
    for (const auto& [prefix, cell] : blocks)
        if (c.startsWith(prefix)) return cell;
    return {};
}

// Columns whose cell does not follow from their name.
QString explicitCell(const QString& table, const QString& column)
{
    static const QHash<QString, QHash<QString, QString>> map = {
        {QStringLiteral("rv_points"),
         {{QStringLiteral("rv_error"), QStringLiteral("radial_velocity")},
          {QStringLiteral("rv_error_formal"), QStringLiteral("radial_velocity")},
          {QStringLiteral("rv_error_systematic"), QStringLiteral("radial_velocity")}}},
        // Statistics of the curve's points, written together on every save.
        {QStringLiteral("rv_curves"),
         {{QStringLiteral("num_points"), QStringLiteral("@stats")},
          {QStringLiteral("mean_rv"), QStringLiteral("@stats")},
          {QStringLiteral("std_rv"), QStringLiteral("@stats")},
          {QStringLiteral("min_rv"), QStringLiteral("@stats")},
          {QStringLiteral("max_rv"), QStringLiteral("@stats")},
          {QStringLiteral("time_baseline"), QStringLiteral("@stats")}}},
    };
    return map.value(table).value(column);
}

// The column a value's error or limit column belongs to: "e_teff_up" and
// "teff_error_down" both give "teff". Empty when the name has no such form.
QString baseOfErrorColumn(const QString& column)
{
    static const QStringList suffixes = {
        QStringLiteral("_error_systematic"), QStringLiteral("_error_formal"),
        QStringLiteral("_error_down"),       QStringLiteral("_error_up"),
        QStringLiteral("_error"),            QStringLiteral("_limit"),
    };
    if (column.startsWith(QLatin1String("e_"))) {
        QString base = column.mid(2);
        if (base.endsWith(QLatin1String("_down"))) base.chop(5);
        else if (base.endsWith(QLatin1String("_up"))) base.chop(3);
        return base;
    }
    for (const QString& suffix : suffixes)
        if (column.endsWith(suffix) && column.size() > suffix.size())
            return column.left(column.size() - suffix.size());
    return {};
}

}   // namespace

QString cellFor(const TableInfo& table, const QString& column)
{
    if (table.pk.contains(column)) return {};
    if (const TablePolicy* policy = policyFor(table.name))
        if (policy->frozen.contains(column)) return {};

    if (const QString cell = explicitCell(table.name, column); !cell.isEmpty()) return cell;

    if (table.name == QLatin1String("stars"))
        if (const QString block = starBlock(column); !block.isEmpty()) return block;

    // An error or limit column shares the cell of the value it describes,
    // when there is such a column.
    const QString base = baseOfErrorColumn(column);
    if (!base.isEmpty() && base != column && table.columns.contains(base) &&
        !table.pk.contains(base))
        return cellFor(table, base);

    return column;
}

QString encodePk(const QStringList& values)
{
    return values.join(kPkSeparator);
}

QStringList decodePk(const QString& encoded)
{
    return encoded.split(kPkSeparator);
}

}   // namespace astra::sync
