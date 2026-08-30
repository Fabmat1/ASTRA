// ─────────────────────────────────────────────────────────────────────────────
// Instrument shape-matcher regression test.
//
// Runs matchSpectrumToInstrument against the shipped default instrument set
// with synthetic wavelength grids modeled on real archive products. The two
// misdetection cases are the ones from the field: an oversampled ESO Phase-3
// UVES stack and a resampled LAMOST LRS export, both of which used to lose
// against WiFeS because the resolution score assumed 2.5 px per resolution
// element.
//
// It also covers the archive-provenance path (InstrumentPrior): a fetched
// spectrum is not a guess, the archive states the instrument and often its
// resolving power, and those must beat whatever the wavelength shape says.
// ─────────────────────────────────────────────────────────────────────────────
#include "models/Instrument.h"
#include "utils/matchSpectraToInstrument.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

std::vector<std::shared_ptr<Instrument>> loadDefaults() {
    std::vector<std::shared_ptr<Instrument>> out;
    QFile file(QStringLiteral(ASTRA_SOURCE_DIR
                              "/resources/data/default_instruments.json"));
    if (!file.open(QIODevice::ReadOnly)) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonArray    arr = doc.object().value("instruments").toArray();
    for (const auto& v : arr) {
        auto inst = std::make_shared<Instrument>(
            Instrument::fromJson(v.toObject()));
        // default_instruments.json carries no ids; the repository stamps a
        // deterministic one from the name on load. The restricted match keys
        // on that id, so the test has to stand one in or it would silently
        // test the unrestricted path instead.
        inst->setId(inst->getName());
        out.push_back(inst);
    }
    return out;
}

std::vector<double> linearGrid(double lo, double hi, double step) {
    std::vector<double> wl;
    for (double w = lo; w <= hi; w += step) wl.push_back(w);
    return wl;
}

std::vector<double> logGrid(double lo, double hi, double logStep) {
    std::vector<double> wl;
    for (double lw = std::log10(lo); lw <= std::log10(hi); lw += logStep)
        wl.push_back(std::pow(10.0, lw));
    return wl;
}

void expectMatch(const std::vector<std::shared_ptr<Instrument>>& instruments,
                 const QString& hint, const std::vector<double>& wl,
                 const QString& expectedName, const std::string& label) {
    const InstrumentMatch m = matchSpectrumToInstrument(instruments, hint, wl);
    const QString got = m.instrument ? m.instrument->getName() : QString();
    check(got == expectedName,
          label + ": matched '" + got.toStdString() + "' (conf " +
              std::to_string(m.confidence) + "), expected '" +
              expectedName.toStdString() + "'");
    check(m.confidence >= 0.25, label + ": confidence above import gate");
}

std::shared_ptr<Instrument> byName(
    const std::vector<std::shared_ptr<Instrument>>& instruments,
    const QString& name) {
    for (const auto& i : instruments)
        if (i && i->getName() == name) return i;
    return nullptr;
}

}   // namespace

int main() {
    const auto instruments = loadDefaults();
    check(!instruments.empty(), "default instruments loaded");
    if (instruments.empty()) return 1;

    // ESO Phase-3 UVES red arm, 564 nm setting: two chips with a gap around
    // the arm center, sampled at ~8.6 px per resolution element.
    {
        std::vector<double> wl = linearGrid(4583.0, 5635.0, 0.014);
        const std::vector<double> upper = linearGrid(5675.0, 6686.0, 0.014);
        wl.insert(wl.end(), upper.begin(), upper.end());
        expectMatch(instruments, QStringLiteral("UVES"), wl,
                    QStringLiteral("UVES"), "oversampled UVES 564 stack");
    }

    // LAMOST LRS resampled to twice the native density (a processed export):
    // used to be claimed by WiFeS via the resolution term.
    {
        const std::vector<double> wl = logGrid(3547.0, 9080.0, 5.14e-5);
        expectMatch(instruments, QString(), wl, QStringLiteral("LAMOST"),
                    "resampled LAMOST LRS, no hint");
        expectMatch(instruments, QStringLiteral("LAMOST/LRS"), wl,
                    QStringLiteral("LAMOST"), "resampled LAMOST LRS, hint");
    }

    // Native LAMOST LRS coadd grid, straight from the archive.
    {
        const std::vector<double> wl = logGrid(3700.0, 9099.0, 1.0e-4);
        expectMatch(instruments, QStringLiteral("LAMOST LRS"), wl,
                    QStringLiteral("LAMOST"), "native LAMOST LRS");
    }

    // A spectrum that really is WiFeS B3000 must still match WiFeS.
    {
        const std::vector<double> wl = linearGrid(3200.0, 5900.0, 0.73);
        expectMatch(instruments, QStringLiteral("WiFeS"), wl,
                    QStringLiteral("WiFeS"), "genuine WiFeS B3000");
    }

    // The bidirectional hint: "LAMOST/LRS" must count as a hint match for
    // the instrument named "LAMOST" (name contained in the hint).
    {
        const std::vector<double> wlA = logGrid(3700.0, 9099.0, 1.0e-4);
        const InstrumentMatch noHint =
            matchSpectrumToInstrument(instruments, QString(), wlA);
        const InstrumentMatch hinted = matchSpectrumToInstrument(
            instruments, QStringLiteral("LAMOST/LRS"), wlA);
        check(hinted.confidence > noHint.confidence,
              "hint 'LAMOST/LRS' raises the LAMOST score");
    }

    // ── name keys ───────────────────────────────────────────────────────────
    // What lets ESO's "XSHOOTER" reach the entry named "X-Shooter". It must
    // bridge punctuation and nothing else: collapsing two genuinely different
    // instruments onto one key would silently mistag every one of their
    // spectra.
    check(instrumentNameKey(QStringLiteral("XSHOOTER")) ==
              instrumentNameKey(QStringLiteral("X-Shooter")),
          "'XSHOOTER' and 'X-Shooter' share a name key");
    check(instrumentNameKey(QStringLiteral("FLAMES_UVES")) ==
              instrumentNameKey(QStringLiteral("FLAMES-UVES")),
          "underscore and hyphen spellings share a name key");
    check(instrumentNameKey(QStringLiteral("UVES")) !=
              instrumentNameKey(QStringLiteral("ESPRESSO")),
          "different instruments keep different name keys");
    {
        std::vector<QString> names;
        int collisions = 0;
        for (const auto& i : instruments) {
            const QString k = instrumentNameKey(i->getName());
            for (const QString& seen : names)
                if (seen == k) ++collisions;
            names.push_back(k);
        }
        check(collisions == 0,
              "no two shipped instruments collide on a name key (" +
                  std::to_string(collisions) + " collision(s))");
    }

    // ── archive provenance beats shape ──────────────────────────────────────
    // These are the real misdetections, measured against the shipped defaults
    // on 2026-08-30. Every one of them scored well above the 0.25 import gate,
    // so they were not near misses: an ESO run silently filed its X-Shooter
    // UVB and VIS arms under UVES, and GIRAFFE under UVES or SOAR. Wavelength
    // coverage simply cannot separate spectrographs that observe the same
    // band, and the archive was telling us the answer the whole time.
    {
        struct Case {
            const char* hint;        // what the archive calls it
            const char* want;        // the configured instrument it belongs to
            const char* wantMode;
            double      lo, hi, step, reportedR;
            const char* looseGets;   // what shape alone claimed, "" if right
            const char* label;
        };
        static const Case kCases[] = {
            {"XSHOOTER", "X-Shooter", "UVB", 3000, 5560, 0.02, 5400.0,
             "UVES", "X-Shooter UVB arm"},
            {"XSHOOTER", "X-Shooter", "VIS", 5600, 10200, 0.02, 18400.0,
             "UVES", "X-Shooter VIS arm"},
            {"XSHOOTER", "X-Shooter", "NIR", 10250, 24700, 0.06, 5600.0,
             "", "X-Shooter NIR arm"},
            {"GIRAFFE", "GIRAFFE", "HR", 4500, 5080, 0.05, 19600.0,
             "UVES", "GIRAFFE HR window"},
            {"GIRAFFE", "GIRAFFE", "LR", 3700, 5150, 0.20, 6000.0,
             "SOAR", "GIRAFFE LR window"},
            {"FEROS", "FEROS", "echelle", 3530, 9210, 0.03, 48000.0,
             "", "FEROS echelle"},
            {"HARPS", "HARPS", "echelle", 3780, 6910, 0.01, 115000.0,
             "", "HARPS echelle"},
            {"ESPRESSO", "ESPRESSO", "singleHR", 3780, 7880, 0.01, 140000.0,
             "", "ESPRESSO singleHR"},
            {"UVES", "UVES", "blue", 3260, 4540, 0.02, 40000.0,
             "", "UVES blue 390"},
            {"UVES", "UVES", "red", 4780, 6810, 0.02, 40000.0,
             "", "UVES red 580"},
        };

        for (const Case& c : kCases) {
            const auto inst = byName(instruments, QString::fromLatin1(c.want));
            check(inst != nullptr,
                  std::string(c.label) + ": '" + c.want + "' is configured");
            if (!inst) continue;

            const std::vector<double> wl = linearGrid(c.lo, c.hi, c.step);

            InstrumentPrior prior;
            prior.hint               = QString::fromLatin1(c.hint);
            prior.knownInstrumentId  = inst->getId();
            prior.reportedResolution = c.reportedR;
            const InstrumentMatch m =
                matchSpectrumToInstrument(instruments, prior, wl);

            const QString gotName =
                m.instrument ? m.instrument->getName() : QString();
            check(gotName == QString::fromLatin1(c.want),
                  std::string(c.label) + ": tagged '" + gotName.toStdString() +
                      "', expected '" + c.want + "'");
            check(m.modeKey == QString::fromLatin1(c.wantMode),
                  std::string(c.label) + ": mode '" +
                      m.modeKey.toStdString() + "', expected '" + c.wantMode +
                      "'");

            // And the half of the story that makes the restriction worth
            // having: without it, shape alone reached for another instrument
            // and did so confidently enough to be written to the database.
            if (c.looseGets[0] != '\0') {
                const InstrumentMatch loose = matchSpectrumToInstrument(
                    instruments, QString::fromLatin1(c.hint), wl);
                const QString looseName =
                    loose.instrument ? loose.instrument->getName() : QString();
                check(looseName == QString::fromLatin1(c.looseGets) &&
                          loose.confidence >= 0.25,
                      std::string(c.label) +
                          ": shape alone still misreads it as '" +
                          looseName.toStdString() + "' (conf " +
                          std::to_string(loose.confidence) +
                          "), which is what the prior is for");
            }
        }
    }

    // A joined X-Shooter product: three arms spliced into one spectrum, which
    // is what Options::joinArms produces. No single mode describes it, and the
    // arms disagree on resolution, so the mode is left empty - but the
    // instrument must still be the one the archive named, and the reported
    // resolving power (one arm's) must not be used to score three arms.
    {
        const auto xsh = byName(instruments, QStringLiteral("X-Shooter"));
        if (xsh) {
            std::vector<double> wl = linearGrid(3000.0, 5560.0, 0.02);
            for (double w : linearGrid(5600.0, 10200.0, 0.02)) wl.push_back(w);
            for (double w : linearGrid(10250.0, 24700.0, 0.06)) wl.push_back(w);

            InstrumentPrior prior;
            prior.hint               = QStringLiteral("XSHOOTER");
            prior.knownInstrumentId  = xsh->getId();
            prior.reportedResolution = 5400.0;   // the UVB arm's, carried over
            const InstrumentMatch m =
                matchSpectrumToInstrument(instruments, prior, wl);
            check(m.instrument == xsh.get(),
                  "a joined three-arm X-Shooter product stays X-Shooter");
        }
    }

    // The reported resolving power is what separates two modes that overlap
    // in wavelength: UVES blue and red both reach across 4200-5000 A.
    {
        const auto uves = byName(instruments, QStringLiteral("UVES"));
        check(uves != nullptr, "UVES is in the shipped defaults");
        if (uves) {
            const std::vector<double> wl = linearGrid(3300.0, 4500.0, 0.02);
            InstrumentPrior prior;
            prior.hint              = QStringLiteral("UVES");
            prior.knownInstrumentId = uves->getId();
            const InstrumentMatch m =
                matchSpectrumToInstrument(instruments, prior, wl);
            check(m.instrument == uves.get(),
                  "a UVES product stays UVES under restriction");
            check(m.modeKey == QStringLiteral("blue"),
                  "the blue arm is picked for a 3300-4500 A product (got '" +
                      m.modeKey.toStdString() + "')");
        }
    }

    // A mode the source named outright is not re-derived from the shape.
    {
        const auto lamost = byName(instruments, QStringLiteral("LAMOST"));
        if (lamost) {
            const std::vector<double> wl = logGrid(3700.0, 9099.0, 1.0e-4);
            InstrumentPrior prior;
            prior.hint              = QStringLiteral("LAMOST/MRS_red");
            prior.knownInstrumentId = lamost->getId();
            prior.knownModeKey      = QStringLiteral("MRS_red");
            const InstrumentMatch m =
                matchSpectrumToInstrument(instruments, prior, wl);
            check(m.modeKey == QStringLiteral("MRS_red"),
                  "a mode the archive named is used as-is (got '" +
                      m.modeKey.toStdString() + "')");
        }
    }

    // Restriction to an instrument whose modes cannot explain the data still
    // names that instrument: the archive knows better than the shape, and a
    // missing mode is a smaller error than the wrong spectrograph.
    {
        const auto fuse = byName(instruments, QStringLiteral("FUSE"));
        if (fuse) {
            const std::vector<double> wl = linearGrid(6000.0, 7000.0, 0.5);
            InstrumentPrior prior;
            prior.hint              = QStringLiteral("FUSE");
            prior.knownInstrumentId = fuse->getId();
            const InstrumentMatch m =
                matchSpectrumToInstrument(instruments, prior, wl);
            check(m.instrument == fuse.get(),
                  "restriction holds even when no mode covers the data");
        }
    }

    // With no prior, nothing about the old behaviour may change.
    {
        const std::vector<double> wl = linearGrid(3200.0, 5900.0, 0.73);
        const InstrumentMatch viaString =
            matchSpectrumToInstrument(instruments, QStringLiteral("WiFeS"), wl);
        InstrumentPrior prior;
        prior.hint = QStringLiteral("WiFeS");
        const InstrumentMatch viaPrior =
            matchSpectrumToInstrument(instruments, prior, wl);
        check(viaString.instrument == viaPrior.instrument &&
                  viaString.modeKey == viaPrior.modeKey &&
                  std::abs(viaString.confidence - viaPrior.confidence) < 1e-12,
              "an empty prior is exactly the old hint-only behaviour");
    }

    std::printf("%s\n", gFailures ? "FAILED" : "PASSED");
    return gFailures ? 1 : 0;
}
