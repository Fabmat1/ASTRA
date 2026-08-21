// ─────────────────────────────────────────────────────────────────────────────
// Instrument shape-matcher regression test.
//
// Runs matchSpectrumToInstrument against the shipped default instrument set
// with synthetic wavelength grids modeled on real archive products. The two
// misdetection cases are the ones from the field: an oversampled ESO Phase-3
// UVES stack and a resampled LAMOST LRS export, both of which used to lose
// against WiFeS because the resolution score assumed 2.5 px per resolution
// element.
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
    for (const auto& v : arr)
        out.push_back(
            std::make_shared<Instrument>(Instrument::fromJson(v.toObject())));
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

    std::printf("%s\n", gFailures ? "FAILED" : "PASSED");
    return gFailures ? 1 : 0;
}
