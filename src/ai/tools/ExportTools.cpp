// Exporting data to files for scripts.

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/ToolRegistry.h"
#include "ai/tools/ToolHelpers.h"
#include "app/AppPaths.h"
#include "core/FilterExpression.h"
#include "core/Instrument.h"
#include "core/Project.h"
#include "core/Star.h"
#include "core/StarFieldRegistry.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "lightcurve/Photometry.h"
#include "rv/PeriodogramRecord.h"
#include "rv/RadialVelocity.h"
#include "spectra/Spectrum.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace astra::ai {

using namespace tools;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const QStringList kKinds = {QStringLiteral("stars"), QStringLiteral("spectra"),
                            QStringLiteral("spectral_fit"), QStringLiteral("rv_curve"),
                            QStringLiteral("lightcurves"), QStringLiteral("periodograms")};

enum class Format { Csv, Json, Npy };

struct Column {
    QString name;
    QString unit;
    bool isString = false;
    std::vector<double> num;
    QStringList str;

    size_t size() const { return isString ? size_t(str.size()) : num.size(); }
};

struct ExportObject {
    QString name;
    QString starId, starAlias, project;
    QJsonObject meta;
    std::vector<Column> cols;

    Column& add(const QString& n, const QString& unit, std::vector<double> v)
    {
        cols.push_back(Column{n, unit, false, std::move(v), {}});
        return cols.back();
    }
    Column& addStr(const QString& n, QStringList v)
    {
        cols.push_back(Column{n, QString(), true, {}, std::move(v)});
        return cols.back();
    }
};

QString starName(const Star& s)
{
    if (!s.getAlias().isEmpty()) return s.getAlias();
    if (!s.getSourceId().isEmpty()) return s.getSourceId();
    return s.getId();
}

QString safeName(const QString& s)
{
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9_.+-]+"));
    QString out = s;
    out.replace(bad, QStringLiteral("_"));
    return out.left(80);
}

QJsonValue optEpoch(double v) { return std::isfinite(v) && v > 0 ? QJsonValue(v) : QJsonValue(); }

/// Fills the star and project fields of an export object.
void attachStar(AiHost& host, ExportObject& o, const Star& s, const QString& projectId)
{
    o.starId = s.getId();
    o.starAlias = starName(s);
    if (auto p = host.project(projectId)) o.project = p->getName();
    else o.project = projectId;
}

// ── writers ────────────────────────────────────────────────────────────────

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data) == data.size();
}

/// A 1-D little-endian float64 .npy file, format version 1.0.
QByteArray npyBytes(const std::vector<double>& v)
{
    QByteArray header = QStringLiteral("{'descr': '<f8', 'fortran_order': False, 'shape': (%1,), }")
                            .arg(v.size()).toLatin1();
    const int preamble = 10;   // magic (6) + version (2) + header length (2)
    int total = preamble + header.size() + 1;
    const int pad = (64 - total % 64) % 64;
    header.append(QByteArray(pad, ' '));
    header.append('\n');
    QByteArray out;
    out.reserve(preamble + header.size() + int(v.size()) * 8);
    out.append("\x93NUMPY", 6);
    out.append(char(1));
    out.append(char(0));
    const quint16 hl = qToLittleEndian(quint16(header.size()));
    out.append(reinterpret_cast<const char*>(&hl), 2);
    out.append(header);
    for (double d : v) {
        quint64 bits;
        std::memcpy(&bits, &d, 8);
        bits = qToLittleEndian(bits);
        out.append(reinterpret_cast<const char*>(&bits), 8);
    }
    return out;
}

QString csvField(const QString& s)
{
    if (!s.contains(QLatin1Char(',')) && !s.contains(QLatin1Char('"')) && !s.contains(QLatin1Char('\n')))
        return s;
    QString q = s;
    q.replace(QLatin1String("\""), QLatin1String("\"\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

QString csvNumber(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 17) : QStringLiteral("nan");
}

/// Writes one object; returns the manifest entry and appends the written
/// files (relative to dir) to `files`.
QJsonObject writeObject(const QDir& dir, const ExportObject& o, Format fmt, QStringList* files)
{
    const size_t n = o.cols.empty() ? 0 : o.cols.front().size();
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), o.name);
    if (!o.starId.isEmpty()) {
        entry.insert(QStringLiteral("star_id"), o.starId);
        entry.insert(QStringLiteral("star_alias"), o.starAlias);
    }
    if (!o.project.isEmpty()) entry.insert(QStringLiteral("project"), o.project);
    entry.insert(QStringLiteral("n_rows"), double(n));
    entry.insert(QStringLiteral("metadata"), o.meta);

    QJsonArray cols;
    for (const auto& c : o.cols) {
        QJsonObject co{{QStringLiteral("name"), c.name},
                       {QStringLiteral("dtype"), c.isString ? QStringLiteral("string") : QStringLiteral("float64")}};
        co.insert(QStringLiteral("unit"), c.unit.isEmpty() ? QJsonValue() : QJsonValue(c.unit));
        cols.append(co);
    }

    if (fmt == Format::Csv) {
        const QString file = o.name + QStringLiteral(".csv");
        QByteArray buf;
        QStringList head;
        for (const auto& c : o.cols) head << csvField(c.name);
        buf += head.join(QLatin1Char(',')).toUtf8() + '\n';
        for (size_t i = 0; i < n; ++i) {
            QStringList row;
            for (const auto& c : o.cols)
                row << (c.isString ? csvField(c.str.value(int(i))) : csvNumber(c.num[i]));
            buf += row.join(QLatin1Char(',')).toUtf8() + '\n';
        }
        if (!writeFile(dir.filePath(file), buf))
            throw ToolError(QStringLiteral("Could not write %1.").arg(dir.filePath(file)));
        files->append(file);
        entry.insert(QStringLiteral("file"), file);
    } else if (fmt == Format::Json) {
        const QString file = o.name + QStringLiteral(".json");
        QJsonObject data;
        for (const auto& c : o.cols) {
            QJsonArray a;
            if (c.isString) for (const auto& s : c.str) a.append(s);
            else for (double v : c.num) a.append(num(v));
            data.insert(c.name, a);
        }
        QJsonObject doc{{QStringLiteral("name"), o.name},
                        {QStringLiteral("metadata"), o.meta},
                        {QStringLiteral("columns"), data}};
        if (!writeFile(dir.filePath(file), QJsonDocument(doc).toJson(QJsonDocument::Compact)))
            throw ToolError(QStringLiteral("Could not write %1.").arg(dir.filePath(file)));
        files->append(file);
        entry.insert(QStringLiteral("file"), file);
    } else {
        if (!dir.mkpath(o.name))
            throw ToolError(QStringLiteral("Could not create %1.").arg(dir.filePath(o.name)));
        for (int k = 0; k < int(o.cols.size()); ++k) {
            const auto& c = o.cols[size_t(k)];
            const QString file = o.name + QLatin1Char('/') + safeName(c.name) +
                                 (c.isString ? QStringLiteral(".json") : QStringLiteral(".npy"));
            const QByteArray bytes = c.isString ? QJsonDocument(QJsonArray::fromStringList(c.str)).toJson(QJsonDocument::Compact)
                                                : npyBytes(c.num);
            if (!writeFile(dir.filePath(file), bytes))
                throw ToolError(QStringLiteral("Could not write %1.").arg(dir.filePath(file)));
            files->append(file);
            QJsonObject co = cols[k].toObject();
            co.insert(QStringLiteral("file"), file);
            cols[k] = co;
        }
        entry.insert(QStringLiteral("dir"), o.name);
    }
    entry.insert(QStringLiteral("columns"), cols);
    return entry;
}

QString pythonSnippet(const QString& dir, Format fmt)
{
    QString load;
    switch (fmt) {
    case Format::Csv:
        load = QStringLiteral(
            "    # structured array; columns by name, e.g. t[\"flux\"]\n"
            "    t = np.genfromtxt(d / obj[\"file\"], delimiter=\",\", names=True, dtype=None,\n"
            "                      encoding=\"utf-8\", deletechars=\"\")\n"
            "    data[obj[\"name\"]] = t\n");
        break;
    case Format::Json:
        load = QStringLiteral(
            "    cols = json.loads((d / obj[\"file\"]).read_text())[\"columns\"]\n"
            "    kinds = {c[\"name\"]: c[\"dtype\"] for c in obj[\"columns\"]}\n"
            "    data[obj[\"name\"]] = {k: np.asarray(v, dtype=float) if kinds[k] == \"float64\" else v\n"
            "                         for k, v in cols.items()}  # null becomes nan\n");
        break;
    case Format::Npy:
        load = QStringLiteral(
            "    data[obj[\"name\"]] = {\n"
            "        c[\"name\"]: np.load(d / c[\"file\"]) if c[\"dtype\"] == \"float64\"\n"
            "        else json.loads((d / c[\"file\"]).read_text())\n"
            "        for c in obj[\"columns\"]}\n");
        break;
    }
    return QStringLiteral(
               "import json, pathlib\n"
               "import numpy as np\n\n"
               "d = pathlib.Path(r\"%1\")\n"
               "manifest = json.loads((d / \"manifest.json\").read_text())\n"
               "data = {}\n"
               "for obj in manifest[\"objects\"]:\n")
               .arg(dir) +
           load +
           QStringLiteral("\n# Or with the ASTRA Python package:\n"
                          "# import astra; data = astra.read_export(r\"%1\")\n").arg(dir);
}

// ── data collection ────────────────────────────────────────────────────────

struct StarRef {
    std::shared_ptr<Star> star;
    QString projectId;
};

std::vector<StarRef> starsArg(AiHost& host, const ToolCall& c, bool required)
{
    QStringList ids = c.strList(QStringLiteral("star_ids"));
    if (ids.isEmpty()) ids = c.strList(QStringLiteral("star_id"));
    if (ids.isEmpty() && required) throw ToolError(QStringLiteral("Pass star_ids (a list of star ids)."));
    std::vector<StarRef> out;
    for (const QString& id : ids) {
        StarRef r;
        r.star = host.star(id, &r.projectId);
        if (!r.star)
            throw ToolError(QStringLiteral("No star with id \"%1\"; use find_stars to look one up.").arg(id));
        out.push_back(r);
    }
    return out;
}

struct SpecRef {
    StarRef at;
    std::shared_ptr<Spectrum> spectrum;
};

/// Spectra of args star_ids (all of them) plus args spectrum_ids.
std::vector<SpecRef> spectraArg(AiHost& host, const ToolCall& c)
{
    std::vector<SpecRef> out;
    QSet<QString> seen;
    const auto stars = starsArg(host, c, false);
    for (const auto& r : stars)
        for (const auto& sp : r.star->getSpectra())
            if (sp && !seen.contains(sp->getId())) { seen.insert(sp->getId()); out.push_back({r, sp}); }

    const QStringList specIds = c.strList(QStringLiteral("spectrum_ids"));
    if (stars.empty() && specIds.isEmpty())
        throw ToolError(QStringLiteral("Pass star_ids and/or spectrum_ids."));
    QHash<QString, QString> starOf;
    for (const QString& id : specIds) {
        if (seen.contains(id)) continue;
        if (starOf.isEmpty())
            for (const auto& p : host.projects())
                for (const auto& row : host.db()->loadSpectraIndex(p->getId()))
                    starOf.insert(row.spectrumId, row.starId);
        const QString sid = starOf.value(id);
        if (sid.isEmpty()) throw ToolError(QStringLiteral("No spectrum with id \"%1\".").arg(id));
        StarRef r;
        r.star = host.star(sid, &r.projectId);
        if (!r.star) throw ToolError(QStringLiteral("The star of spectrum %1 could not be loaded.").arg(id));
        std::shared_ptr<Spectrum> found;
        for (const auto& sp : r.star->getSpectra())
            if (sp && sp->getId() == id) { found = sp; break; }
        if (!found) throw ToolError(QStringLiteral("No spectrum with id \"%1\".").arg(id));
        seen.insert(id);
        out.push_back({r, found});
    }
    return out;
}

bool spectrumArrays(const Spectrum& sp, std::vector<double>* wl, std::vector<double>* fl,
                    std::vector<double>* er)
{
    if (sp.hasData()) {
        *wl = sp.getWavelengths();
        *fl = sp.getFluxes();
        *er = sp.getFluxErrors();
        return true;
    }
    if (sp.getDataFile().isEmpty() || !QFileInfo::exists(sp.getDataFile())) return false;
    Spectrum tmp;
    if (!tmp.loadDataFromFile(sp.getDataFile())) return false;
    *wl = tmp.getWavelengths();
    *fl = tmp.getFluxes();
    *er = tmp.getFluxErrors();
    return !wl->empty();
}

QJsonObject spectrumMeta(const Spectrum& sp)
{
    QJsonObject m;
    m.insert(QStringLiteral("spectrum_id"), sp.getId());
    m.insert(QStringLiteral("instrument"), sp.getInstrument());
    m.insert(QStringLiteral("mjd_utc"), optEpoch(sp.getMJD()));
    m.insert(QStringLiteral("bjd_tdb"), optEpoch(sp.getBJD()));
    m.insert(QStringLiteral("exposure_time_s"), sp.time().hasExposureTime() ? num(sp.getExposureTime()) : QJsonValue());
    m.insert(QStringLiteral("barycentric_corrected"), sp.isBarycentricallyCorrected());
    m.insert(QStringLiteral("flagged"), sp.isFlagged());
    if (!sp.getOrigin().isEmpty()) m.insert(QStringLiteral("origin"), sp.getOrigin());
    if (auto best = sp.getBestFit()) m.insert(QStringLiteral("best_fit_id"), best->getId());
    return m;
}

std::vector<ExportObject> collectStars(AiHost& host, const ToolCall& c)
{
    auto p = resolveProject(host, c);
    const auto& freg = StarFieldRegistry::instance();
    QStringList cols = c.strList(QStringLiteral("columns"));
    if (cols.isEmpty())
        cols = {QStringLiteral("alias"), QStringLiteral("source_id"), QStringLiteral("ra"), QStringLiteral("dec"),
                QStringLiteral("gmag"), QStringLiteral("teff"), QStringLiteral("logg")};
    cols.removeAll(QStringLiteral("id"));
    std::vector<const StarField*> fields;
    for (const QString& k : cols) {
        const StarField* f = freg.find(k);
        if (!f) throw ToolError(QStringLiteral("Unknown column \"%1\"; see list_star_fields.").arg(k));
        fields.push_back(f);
    }

    QStringList exprs = c.strList(QStringLiteral("filters"));
    if (!c.str(QStringLiteral("filter")).isEmpty()) exprs.prepend(c.str(QStringLiteral("filter")));
    std::vector<std::shared_ptr<const FilterExpression>> compiled;
    for (const QString& e : exprs) {
        QString err;
        auto fx = FilterExpression::compile(e, &err);
        if (!fx) throw ToolError(QStringLiteral("Filter \"%1\": %2").arg(e, err));
        if (!fx->hasComparison())
            throw ToolError(QStringLiteral("Filter \"%1\" has no comparison (>, <, ==, ...).").arg(e));
        compiled.push_back(fx);
    }

    std::vector<std::shared_ptr<Star>> rows;
    for (const auto& s : host.stars(p->getId())) {
        bool pass = true;
        for (const auto& fx : compiled) {
            bool ok = false;
            const Star& ref = *s;
            if (!fx->evaluateBool([&ref](const QString& key) { return ref.getFieldValue(key); }, &ok) || !ok) {
                pass = false;
                break;
            }
        }
        if (pass) rows.push_back(s);
    }

    ExportObject o;
    o.name = QStringLiteral("stars");
    o.project = p->getName();
    o.meta = QJsonObject{{QStringLiteral("project_id"), p->getId()},
                         {QStringLiteral("filters"), QJsonArray::fromStringList(exprs)}};
    QStringList ids;
    for (const auto& s : rows) ids << s->getId();
    o.addStr(QStringLiteral("id"), ids);
    for (const StarField* f : fields) {
        if (f->type == StarField::Type::String) {
            QStringList v;
            for (const auto& s : rows) v << freg.valueJson(*s, *f).toString();
            o.addStr(f->key, v);
        } else {
            std::vector<double> v;
            v.reserve(rows.size());
            for (const auto& s : rows) {
                const QJsonValue j = freg.valueJson(*s, *f);
                v.push_back(j.isBool() ? (j.toBool() ? 1.0 : 0.0) : j.isDouble() ? j.toDouble() : kNaN);
            }
            o.add(f->key, f->unit, std::move(v));
        }
    }
    return {o};
}

std::vector<ExportObject> collectSpectra(AiHost& host, const ToolCall& c, QJsonArray* skipped)
{
    std::vector<ExportObject> out;
    for (const auto& r : spectraArg(host, c)) {
        std::vector<double> wl, fl, er;
        if (!spectrumArrays(*r.spectrum, &wl, &fl, &er)) {
            skipped->append(QStringLiteral("spectrum %1: data file unreadable").arg(r.spectrum->getId()));
            continue;
        }
        ExportObject o;
        attachStar(host, o, *r.at.star, r.at.projectId);
        o.name = safeName(o.starAlias + QLatin1Char('_') + r.spectrum->getId().left(8));
        o.meta = spectrumMeta(*r.spectrum);
        o.meta.insert(QStringLiteral("flux_unit_note"),
                      QStringLiteral("flux and flux_error are in the units the file was stored in "
                                     "(often arbitrary or normalised)"));
        const size_t n = wl.size();
        o.add(QStringLiteral("wavelength"), QStringLiteral("Angstrom"), std::move(wl));
        o.add(QStringLiteral("flux"), QStringLiteral("as stored"), std::move(fl));
        if (er.size() == n) o.add(QStringLiteral("flux_error"), QStringLiteral("as stored"), std::move(er));
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<ExportObject> collectSpectralFits(AiHost& host, const ToolCall& c, QJsonArray* skipped)
{
    const QStringList fitIds = c.strList(QStringLiteral("fit_ids"));
    const bool allFits = c.flag(QStringLiteral("all_fits"));
    std::vector<ExportObject> out;
    QSet<QString> found;
    for (const auto& r : spectraArg(host, c)) {
        std::vector<std::shared_ptr<SpectralFit>> fits;
        for (const auto& f : r.spectrum->getSpectralFits()) {
            if (!f) continue;
            if (!fitIds.isEmpty() ? fitIds.contains(f->getId()) : (allFits || f->isBestFit)) fits.push_back(f);
        }
        for (const auto& f : fits) {
            found.insert(f->getId());
            const SpectralFit* data = f.get();
            SpectralFit tmp;
            if (!f->hasData()) {
                const QString file = f->getModelDataFile();
                if (file.isEmpty() || !QFileInfo::exists(file) || !tmp.loadDataFromFile(file)) {
                    skipped->append(QStringLiteral("fit %1: model data unreadable").arg(f->getId()));
                    continue;
                }
                data = &tmp;
            }
            const size_t n = data->modelWavelengths.size();
            ExportObject o;
            attachStar(host, o, *r.at.star, r.at.projectId);
            o.name = safeName(o.starAlias + QLatin1Char('_') + r.spectrum->getId().left(8) +
                              QStringLiteral("_fit_") + f->getId().left(8));
            o.meta = spectrumMeta(*r.spectrum);
            o.meta.insert(QStringLiteral("fit_id"), f->getId());
            o.meta.insert(QStringLiteral("model"), f->modelId);
            o.meta.insert(QStringLiteral("is_best"), f->isBestFit);
            o.meta.insert(QStringLiteral("n_components"), f->nComponents);
            o.meta.insert(QStringLiteral("teff_k"), num(f->teff));
            o.meta.insert(QStringLiteral("logg_cgs"), num(f->logg));
            o.meta.insert(QStringLiteral("log_n_he"), num(f->he));
            o.meta.insert(QStringLiteral("vsini_kms"), num(f->vsini));
            o.meta.insert(QStringLiteral("rv_kms"), num(f->radialVelocity));
            o.meta.insert(QStringLiteral("chi2"), num(f->chi2));
            o.meta.insert(QStringLiteral("note"),
                          QStringLiteral("All arrays are on the model wavelength grid. observed_* is the "
                                         "spectrum rebinned to it; divide flux and model by continuum to "
                                         "normalise; used = 1 where the point entered the fit; comp*_flux "
                                         "are the undiluted single-star models."));
            o.add(QStringLiteral("wavelength"), QStringLiteral("Angstrom"), data->modelWavelengths);
            const auto addIf = [&](const char* name, const std::vector<double>& v, const QString& unit) {
                if (n > 0 && v.size() == n) o.add(QLatin1String(name), unit, v);
            };
            const QString fluxUnit = QStringLiteral("observed flux units");
            addIf("model_flux", data->modelFluxes, fluxUnit);
            addIf("observed_flux", data->rebinnedFluxes, fluxUnit);
            addIf("observed_error", data->rebinnedSigmas, fluxUnit);
            addIf("continuum", data->modelSplines, fluxUnit);
            if (n > 0 && data->modelIgnore.size() == n) {
                std::vector<double> used(n);
                for (size_t i = 0; i < n; ++i) used[i] = double(data->modelIgnore[i]);
                o.add(QStringLiteral("used"), QString(), std::move(used));
            }
            addIf("comp1_flux", data->modelFluxesComp1, fluxUnit);
            addIf("comp2_flux", data->modelFluxesComp2, fluxUnit);
            addIf("light_fraction_comp1", data->lightFractionComp1, QString());
            addIf("telluric_transmission", data->telluricTransmission, QString());
            out.push_back(std::move(o));
        }
    }
    for (const QString& id : fitIds)
        if (!found.contains(id)) skipped->append(QStringLiteral("fit %1: not found on the given stars/spectra").arg(id));
    return out;
}

std::vector<ExportObject> collectRv(AiHost& host, const ToolCall& c)
{
    std::vector<ExportObject> out;
    for (const auto& r : starsArg(host, c, true)) {
        auto curve = r.star->getRVCurve();
        if (!curve) continue;
        auto pts = curve->getRVPoints();
        pts.erase(std::remove(pts.begin(), pts.end(), nullptr), pts.end());
        std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) {
            return a->time().sortValue() < b->time().sortValue();
        });
        std::vector<double> mjd, bjd, rv, err, errF, errS, comp, active;
        QStringList inst, specIds, ids;
        for (const auto& p : pts) {
            const auto m = p->time().mjd();
            const auto b = p->time().bjd();
            mjd.push_back(m && *m > 0 ? *m : kNaN);
            bjd.push_back(b && *b > 0 ? *b : kNaN);
            rv.push_back(p->getRV());
            err.push_back(p->getRVError());
            errF.push_back(p->getRVErrorFormal());
            errS.push_back(p->getRVErrorSystematic());
            comp.push_back(p->getComponent());
            active.push_back(p->isFlagged() ? 0.0 : 1.0);
            QString in;
            if (auto i = p->getInstrument()) in = i->getName();
            else if (auto sp = p->getSourceSpectrum().lock()) in = sp->getInstrument();
            inst << in;
            specIds << p->getSpectrumId();
            ids << p->getId();
        }
        ExportObject o;
        attachStar(host, o, *r.star, r.projectId);
        o.name = safeName(o.starAlias + QStringLiteral("_rv"));
        o.meta.insert(QStringLiteral("curve_id"), curve->getId());
        if (auto best = curve->getBestFit()) o.meta.insert(QStringLiteral("best_fit_id"), best->getId());
        o.meta.insert(QStringLiteral("note"),
                      QStringLiteral("component 1 = primary, 2 = secondary; active 0 = excluded from fits; "
                                     "bjd_tdb nan when it could not be computed"));
        o.addStr(QStringLiteral("point_id"), ids);
        o.add(QStringLiteral("mjd_utc"), QStringLiteral("d"), std::move(mjd));
        o.add(QStringLiteral("bjd_tdb"), QStringLiteral("d"), std::move(bjd));
        o.add(QStringLiteral("rv"), QStringLiteral("km/s"), std::move(rv));
        o.add(QStringLiteral("rv_err"), QStringLiteral("km/s"), std::move(err));
        o.add(QStringLiteral("rv_err_formal"), QStringLiteral("km/s"), std::move(errF));
        o.add(QStringLiteral("rv_err_sys"), QStringLiteral("km/s"), std::move(errS));
        o.add(QStringLiteral("component"), QString(), std::move(comp));
        o.add(QStringLiteral("active"), QString(), std::move(active));
        o.addStr(QStringLiteral("instrument"), inst);
        o.addStr(QStringLiteral("spectrum_id"), specIds);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<ExportObject> collectLightcurves(AiHost& host, const ToolCall& c)
{
    const QString source = c.str(QStringLiteral("source"));
    const QString filter = c.str(QStringLiteral("filter"));
    std::vector<ExportObject> out;
    for (const auto& r : starsArg(host, c, true)) {
        auto phot = r.star->getPhotometry();
        if (!phot) continue;
        for (const auto& src : phot->getLightcurveSources()) {
            if (!source.isEmpty() && src != source) continue;
            auto pts = phot->getLightcurve(src);
            if (!filter.isEmpty())
                pts.erase(std::remove_if(pts.begin(), pts.end(),
                                         [&](const LightcurvePoint& p) { return p.filter != filter; }),
                          pts.end());
            std::stable_sort(pts.begin(), pts.end(), [](const LightcurvePoint& a, const LightcurvePoint& b) {
                return a.time.sortValue() < b.time.sortValue();
            });
            std::vector<double> tb, tn, fl, fe, mg, me, uf, qf;
            QStringList filt;
            bool anyMag = false;
            for (const auto& p : pts) {
                const auto b = p.time.bjd();
                tb.push_back(b && *b > 0 ? *b : kNaN);
                tn.push_back(p.time.nativeValue());
                fl.push_back(p.flux);
                fe.push_back(p.fluxError);
                mg.push_back(p.magnitude);
                me.push_back(p.magnitudeError);
                if (std::isfinite(p.magnitude)) anyMag = true;
                uf.push_back(p.userFlagged ? 1.0 : 0.0);
                qf.push_back(p.qualityFlag);
                filt << p.filter;
            }
            ExportObject o;
            attachStar(host, o, *r.star, r.projectId);
            o.name = safeName(o.starAlias + QLatin1Char('_') + src + (filter.isEmpty() ? QString() : QLatin1Char('_') + filter));
            o.meta.insert(QStringLiteral("source"), src);
            if (!filter.isEmpty()) o.meta.insert(QStringLiteral("filter"), filter);
            o.meta.insert(QStringLiteral("native_time_scale"),
                          pts.empty() ? QString() : Time::scaleToString(pts.front().time.nativeScale()));
            o.meta.insert(QStringLiteral("n_lc_fits"), double(phot->getLCFits(src).size()));
            o.meta.insert(QStringLiteral("note"),
                          QStringLiteral("flux is the stored brightness value (usually relative or normalised "
                                         "flux, no absolute units); time_bjd is nan where no BJD could be "
                                         "computed, time_native holds the stored timestamp; user_flagged 1 = "
                                         "marked bad by the user"));
            o.add(QStringLiteral("time_bjd"), QStringLiteral("BJD_TDB (full JD)"), std::move(tb));
            o.add(QStringLiteral("time_native"), Time::scaleToString(pts.empty() ? TimeScale::Unknown
                                                                                 : pts.front().time.nativeScale()),
                  std::move(tn));
            o.add(QStringLiteral("flux"), QStringLiteral("as stored"), std::move(fl));
            o.add(QStringLiteral("flux_err"), QStringLiteral("as stored"), std::move(fe));
            if (anyMag) {
                o.add(QStringLiteral("magnitude"), QStringLiteral("mag"), std::move(mg));
                o.add(QStringLiteral("magnitude_err"), QStringLiteral("mag"), std::move(me));
            }
            o.add(QStringLiteral("user_flagged"), QString(), std::move(uf));
            o.add(QStringLiteral("quality_flag"), QString(), std::move(qf));
            o.addStr(QStringLiteral("filter"), filt);
            out.push_back(std::move(o));
        }
    }
    return out;
}

ExportObject periodogramObject(AiHost& host, const StarRef& r, const PeriodogramRecord& rec, bool rv)
{
    const auto& res = rec.result;
    ExportObject o;
    attachStar(host, o, *r.star, r.projectId);
    o.name = safeName(o.starAlias + (rv ? QStringLiteral("_rvpgram_") : QStringLiteral("_pgram_")) +
                      (rec.source.isEmpty() ? rec.getId().left(8) : rec.source) +
                      (rec.filter.isEmpty() ? QString() : QLatin1Char('_') + rec.filter));
    o.meta.insert(QStringLiteral("periodogram_id"), rec.getId());
    o.meta.insert(QStringLiteral("data"), rv ? QStringLiteral("radial_velocity") : QStringLiteral("light_curve"));
    o.meta.insert(QStringLiteral("source"), rec.source);
    o.meta.insert(QStringLiteral("filter"), rec.filter);
    if (!res.label.isEmpty()) o.meta.insert(QStringLiteral("label"), res.label);
    o.meta.insert(QStringLiteral("computed_at"), rec.computedAt.toString(Qt::ISODate));
    o.meta.insert(QStringLiteral("n_input_points"), res.nPoints);
    o.meta.insert(QStringLiteral("stored_as"), res.isSummary() ? QStringLiteral("summary") : QStringLiteral("full"));
    if (res.isSummary())
        o.meta.insert(QStringLiteral("note"),
                      QStringLiteral("summary record: power is exact only near the strongest peaks and an "
                                     "upper envelope (pooled maximum) elsewhere"));
    const int n = int(std::min(res.frequency.size(), res.power.size()));
    const size_t un = size_t(std::max(n, 0));
    std::vector<double> f(un), p(un), w(un);
    for (int i = 0; i < n; ++i) {
        f[size_t(i)] = res.frequency[i];
        p[size_t(i)] = res.frequency[i] > 0 ? 1.0 / res.frequency[i] : kNaN;
        w[size_t(i)] = res.power[i];
    }
    o.add(QStringLiteral("frequency"), QStringLiteral("1/d"), std::move(f));
    o.add(QStringLiteral("period"), QStringLiteral("d"), std::move(p));
    o.add(QStringLiteral("power"), QStringLiteral("periodogram power (unitless)"), std::move(w));
    return o;
}

std::vector<ExportObject> collectPeriodograms(AiHost& host, const ToolCall& c)
{
    const QString source = c.str(QStringLiteral("source"));
    const bool withRv = c.flag(QStringLiteral("include_rv"), true);
    std::vector<ExportObject> out;
    for (const auto& r : starsArg(host, c, true)) {
        for (const auto& rec : host.db()->loadStarPeriodograms(r.star->getId()))
            if (rec && (source.isEmpty() || rec->source == source))
                out.push_back(periodogramObject(host, r, *rec, false));
        if (!withRv) continue;
        if (auto curve = r.star->getRVCurve())
            for (const auto& rec : host.db()->loadCurveRVPeriodograms(curve->getId()))
                if (rec && (source.isEmpty() || rec->source == source))
                    out.push_back(periodogramObject(host, r, *rec, true));
    }
    return out;
}

} // namespace

void registerExportTools(AiRuntime& rt)
{
    ToolRegistry* reg = rt.tools();
    AiHost* host = rt.host();

    ToolDef t;
    t.name = QStringLiteral("export_data");
    t.title = QStringLiteral("Export data to files");
    t.group = QStringLiteral("export");
    t.description = QStringLiteral(
        "Writes ASTRA data at full resolution (never decimated) to files for your own scripts, "
        "into <data root>/ai/exports/<timestamp>_<kind>/ or into out_dir. kind: "
        "\"stars\" (one table of a project: id plus columns, with the query_stars filter syntax), "
        "\"spectra\" (one file per spectrum: wavelength [Angstrom], flux, flux_error as stored; "
        "from star_ids and/or spectrum_ids), "
        "\"spectral_fit\" (model arrays on the model grid: wavelength, model_flux, observed_flux, "
        "observed_error, continuum, used, component and telluric curves; the best fit of each "
        "spectrum, all_fits for every fit, or fit_ids), "
        "\"rv_curve\" (per star: mjd_utc, bjd_tdb, rv, errors in km/s, component, active, "
        "instrument, spectrum_id), "
        "\"lightcurves\" (per star and source: time_bjd, time_native, flux, flux_err, "
        "magnitudes when stored, flags, filter; optional source/filter), "
        "\"periodograms\" (stored light-curve and RV periodograms: frequency [1/d], period [d], "
        "power). format: csv (default, one file per object), json (one file per object, "
        "null for missing numbers) or npy (one directory per object, one float64 .npy per "
        "numeric column and a JSON list per text column). Missing numbers are nan in csv and "
        "npy. manifest.json lists every object with its file(s), columns, units, star id, "
        "alias, project and metadata (instrument, MJD/BJD, source, ...). Returns the "
        "directory, the files and a Python snippet that loads everything with numpy.");
    t.inputSchema = Schema()
        .enumStr(QStringLiteral("kind"), kKinds, QStringLiteral("What to export"), true)
        .enumStr(QStringLiteral("format"), {QStringLiteral("csv"), QStringLiteral("json"), QStringLiteral("npy")},
                 QStringLiteral("File format (default csv)"))
        .str(QStringLiteral("out_dir"), QStringLiteral("Directory to write into (created if needed); "
                                                       "default: a new directory under <data root>/ai/exports"))
        .strArray(QStringLiteral("star_ids"), QStringLiteral("Stars (spectra, spectral_fit, rv_curve, "
                                                             "lightcurves, periodograms)"))
        .strArray(QStringLiteral("spectrum_ids"), QStringLiteral("Spectra (spectra, spectral_fit)"))
        .strArray(QStringLiteral("fit_ids"), QStringLiteral("Spectral fits to export (spectral_fit; with "
                                                            "their star_ids or spectrum_ids)"))
        .boolean(QStringLiteral("all_fits"), QStringLiteral("spectral_fit: every fit, not only the best"))
        .str(QStringLiteral("source"), QStringLiteral("lightcurves/periodograms: only this source"))
        .str(QStringLiteral("filter"), QStringLiteral("stars: filter expression; lightcurves: only this band"))
        .strArray(QStringLiteral("filters"), QStringLiteral("stars: several filter expressions, all must hold"))
        .strArray(QStringLiteral("columns"), QStringLiteral("stars: field keys (default alias, source_id, ra, "
                                                            "dec, gmag, teff, logg)"))
        .str(QStringLiteral("project"), QStringLiteral("stars: project id or name; default: the open project"))
        .boolean(QStringLiteral("include_rv"), QStringLiteral("periodograms: also RV periodograms (default true)"));
    t.risk = Risk::Modify;
    t.summarize = [](const QJsonObject& a) {
        const QString kind = a.value(QStringLiteral("kind")).toString();
        const int n = a.value(QStringLiteral("star_ids")).toArray().size() +
                      a.value(QStringLiteral("spectrum_ids")).toArray().size();
        const QString where = a.value(QStringLiteral("out_dir")).toString();
        return QStringLiteral("Export %1%2 to %3")
            .arg(kind, n > 0 ? QStringLiteral(" (%1 item(s))").arg(n) : QString(),
                 where.isEmpty() ? QStringLiteral("a new folder under ai/exports") : where);
    };
    t.handler = ToolDef::sync([host](const ToolCall& c) {
        const QString kind = c.requireStr(QStringLiteral("kind"));
        if (!kKinds.contains(kind))
            throw ToolError(QStringLiteral("Unknown kind \"%1\"; use one of %2.").arg(kind, kKinds.join(QStringLiteral(", "))));
        const QString fmtArg = c.str(QStringLiteral("format"), QStringLiteral("csv")).toLower();
        Format fmt;
        if (fmtArg == QLatin1String("csv")) fmt = Format::Csv;
        else if (fmtArg == QLatin1String("json")) fmt = Format::Json;
        else if (fmtArg == QLatin1String("npy")) fmt = Format::Npy;
        else throw ToolError(QStringLiteral("Unknown format \"%1\"; use csv, json or npy.").arg(fmtArg));

        QJsonArray skipped;
        std::vector<ExportObject> objs;
        if (kind == QLatin1String("stars")) objs = collectStars(*host, c);
        else if (kind == QLatin1String("spectra")) objs = collectSpectra(*host, c, &skipped);
        else if (kind == QLatin1String("spectral_fit")) objs = collectSpectralFits(*host, c, &skipped);
        else if (kind == QLatin1String("rv_curve")) objs = collectRv(*host, c);
        else if (kind == QLatin1String("lightcurves")) objs = collectLightcurves(*host, c);
        else objs = collectPeriodograms(*host, c);
        if (objs.empty())
            throw ToolError(skipped.isEmpty()
                                ? QStringLiteral("Nothing to export: the selection holds no %1 data.").arg(kind)
                                : QStringLiteral("Nothing could be exported: %1")
                                      .arg(QString::fromUtf8(QJsonDocument(skipped).toJson(QJsonDocument::Compact))));

        // Unique object names.
        QHash<QString, int> used;
        for (auto& o : objs) {
            const int k = used.value(o.name, 0);
            used.insert(o.name, k + 1);
            if (k > 0) o.name += QStringLiteral("_%1").arg(k + 1);
        }

        QString path = c.str(QStringLiteral("out_dir")).trimmed();
        if (path.isEmpty()) {
            const QString base = AppPaths::root() + QStringLiteral("/ai/exports/") +
                                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                                 QLatin1Char('_') + kind;
            path = base;
            for (int i = 2; QFileInfo::exists(path); ++i) path = base + QStringLiteral("_%1").arg(i);
        }
        path = QDir::cleanPath(QDir(path).absolutePath());
        QDir dir(path);
        if (!dir.mkpath(QStringLiteral(".")))
            throw ToolError(QStringLiteral("Could not create %1.").arg(path));

        QStringList files;
        QJsonArray entries;
        for (const auto& o : objs) entries.append(writeObject(dir, o, fmt, &files));

        const QString snippet = pythonSnippet(path, fmt);
        QJsonObject manifest{{QStringLiteral("kind"), kind},
                             {QStringLiteral("format"), fmtArg},
                             {QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                             {QStringLiteral("directory"), path},
                             {QStringLiteral("objects"), entries},
                             {QStringLiteral("python"), snippet}};
        if (!skipped.isEmpty()) manifest.insert(QStringLiteral("skipped"), skipped);
        if (!writeFile(dir.filePath(QStringLiteral("manifest.json")),
                       QJsonDocument(manifest).toJson(QJsonDocument::Indented)))
            throw ToolError(QStringLiteral("Could not write the manifest in %1.").arg(path));
        files.prepend(QStringLiteral("manifest.json"));

        QJsonObject out{{QStringLiteral("directory"), path},
                        {QStringLiteral("n_objects"), int(objs.size())},
                        {QStringLiteral("files"), QJsonArray::fromStringList(files)},
                        {QStringLiteral("python"), snippet}};
        if (!skipped.isEmpty()) out.insert(QStringLiteral("skipped"), skipped);
        return ToolResult::json(out);
    });
    reg->add(std::move(t));
}

} // namespace astra::ai
