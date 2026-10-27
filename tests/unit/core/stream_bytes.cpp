// The data files' fast codec (core/StreamBytes.h) against QDataStream.
//
// Spectra, fits and lightcurves used to be written and read one value at a
// time through a QDataStream. They now go through StreamBytes, which has to
// produce exactly the stream's bytes and read exactly what the stream wrote:
// every file already on disk was written the old way, and an older ASTRA
// reads the files this one writes. The reference here is always a
// QDataStream set up the way the files' code sets it up.

#include <doctest.h>

#include "app/DataStore.h"
#include "core/Instrument.h"
#include "core/StreamBytes.h"
#include "core/Time.h"
#include "lightcurve/Photometry.h"
#include "spectra/Spectrum.h"

#include <QByteArray>
#include <QDataStream>
#include <QTemporaryDir>

#include <memory>
#include <random>
#include <vector>

namespace {

QByteArray streamed(const std::function<void(QDataStream&)>& write)
{
    QByteArray b;
    QDataStream s(&b, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    write(s);
    return b;
}

/// Times covering every optional field, the exposure and the auto-convert
/// coordinates.
std::vector<Time> sampleTimes()
{
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(50000.0, 61000.0);
    auto inst = std::make_shared<Instrument>("Test", 28.76, -17.88, 2396.0);
    std::vector<Time> t;
    t.push_back(Time());
    for (int i = 0; i < 64; ++i) {
        Time x(u(rng), i % 2 ? TimeScale::BJD : TimeScale::MJD);
        if (i & 2) x.setMJD(u(rng));
        if (i & 4) x.setBJD(u(rng));
        if (i & 8) x.setHJD(u(rng));
        if (i & 16) x.setExposureTime(300.0 + i);
        if (i & 32) x.setAutoConvertInfo(inst, 123.4 + i, -45.6);
        t.push_back(x);
    }
    return t;
}

QByteArray payloadOf(const QString& file, DataStore::DataType type)
{
    QByteArray p;
    REQUIRE(DataStore::readCompressed(file, type, p));
    return p;
}

}   // namespace

TEST_SUITE("core")
{

TEST_CASE("StreamBytes: a Time is the bytes QDataStream writes, both ways")
{
    for (const Time& t : sampleTimes()) {
        const QByteArray ref = streamed([&](QDataStream& s) { s << t; });
        QByteArray mine;
        t.appendStreamBytes(mine);
        CHECK(mine == ref);

        StreamBytes::Reader r(ref);
        Time back;
        back.readStreamBytes(r);
        CHECK(r.ok);
        CHECK(r.p == r.end);
        // Re-encoded through the stream, the decoded Time is the same Time.
        CHECK(streamed([&](QDataStream& s) { s << back; }) ==
              streamed([&](QDataStream& s) {
                  Time viaStream;
                  QByteArray copy = ref;
                  QDataStream in(&copy, QIODevice::ReadOnly);
                  in.setVersion(QDataStream::Qt_6_0);
                  in >> viaStream;
                  s << viaStream;
              }));
    }
}

TEST_CASE("StreamBytes: strings, null and empty kept apart")
{
    const QString cases[] = {QString(), QStringLiteral(""), QStringLiteral("TESS"),
                             QStringLiteral("Hα 6563"), QString::fromUtf8("\xf0\x9f\x8c\x9f")};
    for (const QString& str : cases) {
        const QByteArray ref = streamed([&](QDataStream& s) { s << str; });
        QByteArray mine;
        StreamBytes::putString(mine, str);
        CHECK(mine == ref);

        StreamBytes::Reader r(ref);
        const QString back = r.string();
        CHECK(r.ok);
        CHECK(back == str);
        CHECK(back.isNull() == str.isNull());
    }
}

TEST_CASE("StreamBytes: a short buffer fails instead of reading past it")
{
    const QByteArray ref = streamed([](QDataStream& s) {
        s << quint32(7) << 1.5 << QStringLiteral("abc");
    });
    StreamBytes::Reader r(ref.left(ref.size() - 1));
    CHECK(r.u32() == 7);
    CHECK(r.f64() == 1.5);
    CHECK(r.string().isNull());
    CHECK_FALSE(r.ok);
}

TEST_CASE("Lightcurve files: written and read as before")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    std::vector<LightcurvePoint> pts;
    const auto times = sampleTimes();
    const QString filters[] = {QStringLiteral("TESS"), QString(), QStringLiteral(""),
                               QStringLiteral("zg")};
    for (size_t i = 0; i < times.size(); ++i) {
        LightcurvePoint p;
        p.time        = times[i];
        p.flux        = 1.0 + 0.001 * double(i);
        p.fluxError   = 0.01;
        p.filter      = filters[i % 4];
        p.userFlagged = (i % 5) == 0;
        pts.push_back(p);
    }

    // The payload the stream-based writer produced.
    const QByteArray ref = streamed([&](QDataStream& s) {
        s << quint32(0xFFFFFFFFu) << quint32(2) << quint32(pts.size());
        for (const auto& p : pts) {
            s << p.time << p.flux << p.fluxError << p.filter;
            s << quint8(p.userFlagged ? 1 : 0);
        }
    });

    Photometry ph;
    ph.addLightcurve("TESS", pts);
    const QString mine = tmp.filePath("new.asd");
    REQUIRE(ph.saveLightcurveToFile("TESS", mine));
    CHECK(payloadOf(mine, DataStore::LightcurveData) == ref);

    // A file written the old way loads into the same points.
    const QString old = tmp.filePath("old.asd");
    REQUIRE(DataStore::writeCompressed(old, DataStore::LightcurveData, ref));
    Photometry back;
    REQUIRE(back.loadLightcurveFromFile("TESS", old));
    const auto got = back.getLightcurve("TESS");
    REQUIRE(got.size() == pts.size());
    // What the stream itself reads back from those bytes: the auto-convert
    // instrument is not in a file (its owner re-links it), so a decoded Time
    // is compared with a decoded Time, not with the one that was written.
    std::vector<Time> viaStream(pts.size());
    {
        QByteArray copy = ref;
        QDataStream in(&copy, QIODevice::ReadOnly);
        in.setVersion(QDataStream::Qt_6_0);
        quint32 a, b, n;
        in >> a >> b >> n;
        for (size_t i = 0; i < pts.size(); ++i) {
            double f, e; QString flt; quint8 fl;
            in >> viaStream[i] >> f >> e >> flt >> fl;
        }
        REQUIRE(in.status() == QDataStream::Ok);
    }
    for (size_t i = 0; i < pts.size(); ++i) {
        INFO("point " << i);
        CHECK(got[i].flux == pts[i].flux);
        CHECK(got[i].filter == pts[i].filter);
        CHECK(got[i].filter.isNull() == pts[i].filter.isNull());
        CHECK(got[i].userFlagged == pts[i].userFlagged);
        CHECK(streamed([&](QDataStream& s) { s << got[i].time; }) ==
              streamed([&](QDataStream& s) { s << viaStream[i]; }));
    }

    // A truncated payload is reported, not read as zeros.
    const QString cut = tmp.filePath("cut.asd");
    REQUIRE(DataStore::writeCompressed(cut, DataStore::LightcurveData, ref.left(ref.size() - 3)));
    Photometry bad;
    CHECK_FALSE(bad.loadLightcurveFromFile("TESS", cut));
}

TEST_CASE("Spectrum, fit and LC fit files round-trip; LC fits keep the stream's bytes")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    std::mt19937 rng(5);
    std::normal_distribution<double> g(1.0, 0.2);
    auto arr = [&](size_t n) {
        std::vector<double> v(n);
        for (auto& x : v) x = g(rng);
        return v;
    };

    // Spectrum
    const auto wl = arr(1000), fl = arr(1000), er = arr(1000);
    Spectrum sp;
    sp.setData(wl, fl, er);
    const QString spFile = tmp.filePath("sp.asd");
    REQUIRE(sp.saveDataToFile(spFile));
    // Spectra and fits are written in the coded layout of format version 2
    // (tests/unit/app/array_codec.cpp); the classic one is read back below.
    QByteArray coded;
    bool isCoded = false;
    REQUIRE(DataStore::readCompressed(spFile, DataStore::SpectrumData, coded, &isCoded));
    CHECK(isCoded);
    Spectrum spBack;
    REQUIRE(spBack.loadDataFromFile(spFile));
    CHECK(spBack.getWavelengths() == wl);
    CHECK(spBack.getFluxErrors() == er);

    // Spectral fit, extension block included
    SpectralFit fit;
    fit.modelWavelengths     = arr(500);
    fit.modelFluxes          = arr(500);
    fit.rebinnedFluxes       = arr(400);
    fit.rebinnedSigmas       = arr(400);
    fit.modelSplines         = arr(400);
    fit.modelIgnore          = std::vector<uint8_t>(400, 1);
    fit.modelFluxesComp1     = arr(500);
    fit.telluricTransmission = arr(500);
    fit.lightFractionComp1   = arr(500);
    const QString fitFile = tmp.filePath("fit.asd");
    REQUIRE(fit.saveDataToFile(fitFile));
    SpectralFit fitBack;
    REQUIRE(fitBack.loadDataFromFile(fitFile));
    CHECK(fitBack.modelWavelengths == fit.modelWavelengths);
    CHECK(fitBack.modelSplines == fit.modelSplines);
    CHECK(fitBack.modelIgnore == fit.modelIgnore);
    CHECK(fitBack.modelFluxesComp1 == fit.modelFluxesComp1);
    CHECK(fitBack.modelFluxesComp2.empty());
    CHECK(fitBack.telluricTransmission == fit.telluricTransmission);
    CHECK(fitBack.lightFractionComp1 == fit.lightFractionComp1);

    // LC fit
    LCFit lcf;
    for (int i = 0; i < 300; ++i) {
        LCFitDataPoint p{g(rng), g(rng), g(rng), g(rng), g(rng), g(rng)};
        lcf.inputPoints.push_back(p);
        if (i % 2) lcf.modelPoints.push_back(p);
    }
    const QString lcfFile = tmp.filePath("lcf.asd");
    REQUIRE(lcf.saveDataToFile(lcfFile));
    CHECK(payloadOf(lcfFile, DataStore::LCFitData) == streamed([&](QDataStream& s) {
              s << quint16(2);
              for (const auto* v : {&lcf.inputPoints, &lcf.modelPoints}) {
                  s << quint32(v->size());
                  for (const auto& p : *v)
                      s << p.phase << p.dPhase << p.flux << p.fluxError << p.weight << p.factor;
              }
          }));
    LCFit lcfBack;
    REQUIRE(lcfBack.loadDataFromFile(lcfFile));
    REQUIRE(lcfBack.modelPoints.size() == lcf.modelPoints.size());
    CHECK(lcfBack.modelPoints.back().factor == lcf.modelPoints.back().factor);
    CHECK(lcfBack.inputPoints[17].dPhase == lcf.inputPoints[17].dPhase);
}

}   // TEST_SUITE("core")
