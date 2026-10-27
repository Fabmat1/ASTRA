// Data file format version 2: ArrayCodec, the coded spectrum and fit layouts,
// the fits' shared base files, and reading version 1 files.
//
// Everything here must be lossless to the bit: a migrated store is checked
// file by file against these same decoders, and a fit that came back with one
// changed bit would be a different fit. Version 1 files stay on disk for as
// long as nobody migrates them, so they must keep reading.

#include <doctest.h>

#include "app/ArrayCodec.h"
#include "app/DataStore.h"
#include "spectra/Spectrum.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {

bool bitEqual(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * 8) == 0);
}

std::vector<double> roundTrip(const std::vector<double>& v, qsizetype* bytes = nullptr)
{
    QByteArray b;
    ArrayCodec::putF64s(b, v);
    if (bytes) *bytes = b.size();
    StreamBytes::Reader r(b);
    std::vector<double> back;
    ArrayCodec::getF64s(r, back);
    REQUIRE(r.ok);
    CHECK(r.p == r.end);
    return back;
}

double fromBits(quint64 u)
{
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

std::vector<double> logGrid(size_t n, double w0 = 3600.0, double step = 1e-5)
{
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = w0 * std::exp(step * double(i));
    return v;
}

/// A file as format version 1 wrote it: zlib (qCompress) of the classic payload.
void writeVersion1(const QString& path, DataStore::DataType type, const QByteArray& payload)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("ASTR", 4);
    QDataStream out(&f);
    out.setVersion(QDataStream::Qt_6_0);
    out << quint16(1) << quint16(type) << qCompress(payload, 1);
}

SpectralFit sampleFit(std::mt19937& rng, size_t n, bool twoComponents, bool telluric)
{
    std::normal_distribution<double> g(1.0, 0.05);
    SpectralFit f;
    f.modelWavelengths = logGrid(n);
    for (size_t i = 0; i < n; ++i) {
        f.modelFluxes.push_back(g(rng));
        f.rebinnedFluxes.push_back(g(rng));
        f.rebinnedSigmas.push_back(0.01 * g(rng));
        f.modelSplines.push_back(1.0 + 1e-6 * double(i));
        f.modelIgnore.push_back(i % 17 == 0 ? 1 : 0);
    }
    if (twoComponents) {
        for (size_t i = 0; i < n; ++i) {
            f.modelFluxesComp1.push_back(g(rng));
            f.modelFluxesComp2.push_back(g(rng));
            f.lightFractionComp1.push_back(0.6 + 0.01 * g(rng));
        }
    } else {
        f.modelFluxesComp1 = f.modelFluxes;
    }
    if (telluric)
        for (size_t i = 0; i < n; ++i) f.telluricTransmission.push_back(0.9 + 0.1 * g(rng));
    return f;
}

void checkSameFit(const SpectralFit& a, const SpectralFit& b)
{
    CHECK(bitEqual(a.modelWavelengths, b.modelWavelengths));
    CHECK(bitEqual(a.modelFluxes, b.modelFluxes));
    CHECK(bitEqual(a.rebinnedFluxes, b.rebinnedFluxes));
    CHECK(bitEqual(a.rebinnedSigmas, b.rebinnedSigmas));
    CHECK(bitEqual(a.modelSplines, b.modelSplines));
    CHECK(a.modelIgnore == b.modelIgnore);
    CHECK(bitEqual(a.modelFluxesComp1, b.modelFluxesComp1));
    CHECK(bitEqual(a.modelFluxesComp2, b.modelFluxesComp2));
    CHECK(bitEqual(a.telluricTransmission, b.telluricTransmission));
    CHECK(bitEqual(a.lightFractionComp1, b.lightFractionComp1));
}

QStringList bases(const QString& dir)
{
    return QDir(dir).entryList({"fitbase_*.asd"}, QDir::Files);
}

void age(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadWrite));
    REQUIRE(f.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                          QFileDevice::FileModificationTime));
}

}   // namespace

TEST_SUITE("app")
{

TEST_CASE("ArrayCodec: random doubles, special values and edge lengths round-trip bit-exact")
{
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<quint64> bits;

    std::vector<double> random(5000);
    for (auto& x : random) x = fromBits(bits(rng));   // includes NaNs with payloads
    CHECK(bitEqual(roundTrip(random), random));

    const std::vector<double> special = {
        0.0, -0.0, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(), fromBits(0x7ff0000000000001ull),
        fromBits(0xfff8dead0000beefull), std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(), 1.0, -1.0};
    CHECK(bitEqual(roundTrip(special), special));

    CHECK(roundTrip({}).empty());
    CHECK(bitEqual(roundTrip({42.125}), {42.125}));
    CHECK(bitEqual(roundTrip({0.1}), {0.1}));
}

TEST_CASE("ArrayCodec: float32 data and log grids shrink, and stay exact")
{
    std::mt19937 rng(3);
    std::normal_distribution<float> g(1.0f, 0.1f);
    std::vector<double> f32(20000);
    for (auto& x : f32) x = double(g(rng));
    qsizetype bytes = 0;
    CHECK(bitEqual(roundTrip(f32, &bytes), f32));
    CHECK(bytes < qsizetype(f32.size() * 4 + 64));    // 4-byte lanes at most

    // A float32 NaN widened to double is exact; one with a double-only
    // payload is not and must take the 8-byte path.
    std::vector<double> mixed = f32;
    mixed[7] = fromBits(0x7ff8000000000123ull);
    CHECK(bitEqual(roundTrip(mixed), mixed));

    const auto grid = logGrid(40000);
    CHECK(bitEqual(roundTrip(grid, &bytes), grid));
    CHECK(bytes < qsizetype(grid.size() * 8 / 2));

    // Above the trial limit the transform is chosen on a prefix.
    const auto big = logGrid(600000, 1000.0, 1e-7);
    CHECK(bitEqual(roundTrip(big), big));
}

TEST_CASE("ArrayCodec: masks, and damaged records")
{
    std::vector<uint8_t> flags(1001);
    for (size_t i = 0; i < flags.size(); ++i) flags[i] = (i * 7) % 3 == 0;
    std::vector<uint8_t> bytes = {0, 1, 2, 255, 1};
    for (const auto* m : {&flags, &bytes}) {
        QByteArray b;
        ArrayCodec::putMask(b, *m);
        StreamBytes::Reader r(b);
        std::vector<uint8_t> back;
        ArrayCodec::getMask(r, back);
        CHECK(r.ok);
        CHECK(back == *m);
    }

    QByteArray b;
    ArrayCodec::putF64s(b, logGrid(100));
    for (qsizetype cut : {qsizetype(0), qsizetype(5), b.size() - 1}) {
        StreamBytes::Reader r(b.left(cut));
        std::vector<double> back;
        ArrayCodec::getF64s(r, back);
        CHECK_FALSE(r.ok);
        CHECK(back.empty());
    }
    // A flipped bit in the compressed bytes must fail or decode, never crash.
    QByteArray bad = b;
    bad[bad.size() - 3] = char(bad[bad.size() - 3] ^ 0x5a);
    StreamBytes::Reader r(bad);
    std::vector<double> back;
    ArrayCodec::getF64s(r, back);
    CHECK((r.ok ? back.size() == 100 : back.empty()));
}

TEST_CASE("DataStore: version 2 zstd payloads, version 1 files, newer versions")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QByteArray payload = QByteArray("lightcurve payload ").repeated(200);

    const QString v2 = tmp.filePath("v2.asd");
    REQUIRE(DataStore::writeCompressed(v2, DataStore::LightcurveData, payload));
    quint16 version = 0, type = 0;
    REQUIRE(DataStore::readHeader(v2, version, type));
    CHECK(version == 2);
    CHECK(type == DataStore::LightcurveData);
    QByteArray back;
    bool coded = true;
    REQUIRE(DataStore::readCompressed(v2, DataStore::LightcurveData, back, &coded));
    CHECK_FALSE(coded);
    CHECK(back == payload);
    REQUIRE(DataStore::writeCompressed(v2, DataStore::LightcurveData, {}));
    REQUIRE(DataStore::readCompressed(v2, DataStore::LightcurveData, back));
    CHECK(back.isEmpty());

    const QString v1 = tmp.filePath("v1.asd");
    writeVersion1(v1, DataStore::LightcurveData, payload);
    REQUIRE(DataStore::readCompressed(v1, DataStore::LightcurveData, back));
    CHECK(back == payload);

    // A coded payload is refused by a reader that only knows the classic layout.
    const QString c = tmp.filePath("coded.asd");
    REQUIRE(DataStore::writeCoded(c, DataStore::SpectrumData, payload));
    CHECK_FALSE(DataStore::readCompressed(c, DataStore::SpectrumData, back));
    REQUIRE(DataStore::readCompressed(c, DataStore::SpectrumData, back, &coded));
    CHECK(coded);

    const QString v3 = tmp.filePath("v3.asd");
    {
        QFile f(v3);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("ASTR", 4);
        QDataStream out(&f);
        out.setVersion(QDataStream::Qt_6_0);
        out << quint16(3) << quint16(DataStore::LightcurveData) << quint8(1) << QByteArray("x");
    }
    CHECK_FALSE(DataStore::readCompressed(v3, DataStore::LightcurveData, back, &coded));
}

TEST_CASE("Spectra: coded files and version 1 files load bit-exact")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    std::mt19937 rng(9);
    std::normal_distribution<float> g(1.0f, 0.1f);
    std::vector<double> wl = logGrid(3000), fl, er;
    for (size_t i = 0; i < wl.size(); ++i) {
        fl.push_back(double(g(rng)));
        er.push_back(0.01 * double(g(rng)) + 1e-9 * double(i));
    }
    Spectrum sp;
    sp.setData(wl, fl, er);
    const QString file = tmp.filePath("stars/S/spectra/SP.asd");
    REQUIRE(sp.saveDataToFile(file));
    Spectrum back;
    REQUIRE(back.loadDataFromFile(file));
    CHECK(bitEqual(back.getWavelengths(), wl));
    CHECK(bitEqual(back.getFluxes(), fl));
    CHECK(bitEqual(back.getFluxErrors(), er));

    QByteArray classic;
    {
        QDataStream s(&classic, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << quint32(wl.size()) << quint32(fl.size()) << quint32(er.size());
        for (const auto* v : {&wl, &fl, &er})
            for (double x : *v) s << x;
    }
    const QString old = tmp.filePath("old.asd");
    writeVersion1(old, DataStore::SpectrumData, classic);
    Spectrum fromOld;
    REQUIRE(fromOld.loadDataFromFile(old));
    CHECK(bitEqual(fromOld.getWavelengths(), wl));
    CHECK(bitEqual(fromOld.getFluxErrors(), er));
}

TEST_CASE("Fits: one and two components, telluric, base files shared and verified")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.filePath("stars/S/spectra");
    std::mt19937 rng(21);

    SpectralFit one = sampleFit(rng, 2000, false, false);
    SpectralFit two = sampleFit(rng, 2000, true, true);
    two.modelWavelengths = one.modelWavelengths;
    two.rebinnedFluxes   = one.rebinnedFluxes;      // same spectrum: same base
    SpectralFit other = sampleFit(rng, 1500, false, true);   // another spectrum

    REQUIRE(one.saveDataToFile(dir + "/fit_A_1.asd"));
    REQUIRE(two.saveDataToFile(dir + "/fit_A_2.asd"));
    REQUIRE(other.saveDataToFile(dir + "/fit_B_3.asd"));
    CHECK(bases(dir).size() == 2);
    CHECK(SpectralFit::basePathOf(dir + "/fit_A_1.asd") ==
          SpectralFit::basePathOf(dir + "/fit_A_2.asd"));

    for (auto [file, ref] : {std::pair{"fit_A_1.asd", &one}, std::pair{"fit_A_2.asd", &two},
                             std::pair{"fit_B_3.asd", &other}}) {
        SpectralFit back;
        REQUIRE(back.loadDataFromFile(dir + "/" + file));
        checkSameFit(back, *ref);
    }

    // The coded file is far smaller than the doubles it holds.
    CHECK(QFileInfo(dir + "/fit_A_1.asd").size() < qint64(2000 * 8 * 3));

    // A missing base fails the load loudly instead of returning half a fit.
    const QString base = SpectralFit::basePathOf(dir + "/fit_B_3.asd");
    REQUIRE(QFile::rename(base, base + ".gone"));
    SpectralFit missing;
    CHECK_FALSE(missing.loadDataFromFile(dir + "/fit_B_3.asd"));
    CHECK(missing.modelWavelengths.empty());
    REQUIRE(QFile::rename(base + ".gone", base));

    // A base under the right name with other contents is never reused.
    {
        QFile::remove(base);
        SpectralFit decoy = other;
        decoy.rebinnedFluxes[0] += 1.0;
        REQUIRE(decoy.saveDataToFile(dir + "/fit_C_9.asd"));
        REQUIRE(QFile::rename(SpectralFit::basePathOf(dir + "/fit_C_9.asd"), base));
        QFile::remove(dir + "/fit_C_9.asd");
    }
    REQUIRE(other.saveDataToFile(dir + "/fit_B_4.asd"));
    CHECK(SpectralFit::basePathOf(dir + "/fit_B_4.asd").isEmpty());   // kept its own arrays
    SpectralFit own;
    REQUIRE(own.loadDataFromFile(dir + "/fit_B_4.asd"));
    checkSameFit(own, other);
}

TEST_CASE("Fits: version 1 files load with the new reader")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    std::mt19937 rng(4);
    const SpectralFit ref = sampleFit(rng, 800, true, true);

    QByteArray classic;
    {
        QDataStream s(&classic, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << quint32(ref.modelWavelengths.size()) << quint32(ref.modelFluxes.size())
          << quint32(ref.rebinnedFluxes.size()) << quint32(ref.rebinnedSigmas.size())
          << quint32(ref.modelSplines.size()) << quint32(ref.modelIgnore.size());
        for (const auto* v : {&ref.modelWavelengths, &ref.modelFluxes, &ref.rebinnedFluxes,
                              &ref.rebinnedSigmas, &ref.modelSplines})
            for (double x : *v) s << x;
        for (auto m : ref.modelIgnore) s << quint8(m);
        s << quint32(0xA57EA001) << quint32(2);
        for (const auto* v : {&ref.modelFluxesComp1, &ref.modelFluxesComp2,
                              &ref.telluricTransmission, &ref.lightFractionComp1}) {
            s << quint32(v->size());
            for (double x : *v) s << x;
        }
    }
    const QString file = tmp.filePath("fit_old.asd");
    writeVersion1(file, DataStore::SpectralFitData, classic);
    SpectralFit back;
    REQUIRE(back.loadDataFromFile(file));
    checkSameFit(back, ref);
    CHECK(SpectralFit::basePathOf(file).isEmpty());
}

TEST_CASE("pruneFitBases keeps referenced and recent bases, drops the rest")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.filePath("spectra");
    std::mt19937 rng(8);
    SpectralFit a = sampleFit(rng, 300, false, false);
    SpectralFit b = sampleFit(rng, 300, false, false);
    REQUIRE(a.saveDataToFile(dir + "/fit_A_1.asd"));
    REQUIRE(a.saveDataToFile(dir + "/fit_A_2.asd"));
    REQUIRE(b.saveDataToFile(dir + "/fit_B_1.asd"));
    const QString baseA = SpectralFit::basePathOf(dir + "/fit_A_1.asd");
    const QString baseB = SpectralFit::basePathOf(dir + "/fit_B_1.asd");
    for (const QString& f : bases(dir)) age(dir + "/" + f);

    CHECK(SpectralFit::pruneFitBases(dir) == 0);
    QFile::remove(dir + "/fit_A_1.asd");
    CHECK(SpectralFit::pruneFitBases(dir) == 0);     // fit_A_2 still uses it
    QFile::remove(dir + "/fit_B_1.asd");
    CHECK(SpectralFit::pruneFitBases(dir) == 1);
    CHECK(QFile::exists(baseA));
    CHECK_FALSE(QFile::exists(baseB));

    // A base written moments ago may belong to a fit still being saved.
    SpectralFit c = sampleFit(rng, 300, false, false);
    REQUIRE(c.saveDataToFile(dir + "/fit_C_1.asd"));
    QFile::remove(dir + "/fit_C_1.asd");
    CHECK(SpectralFit::pruneFitBases(dir) == 0);
    CHECK(bases(dir).size() == 2);
}

}   // TEST_SUITE("app")
