#include <QtTest/QTest>
#include <QTemporaryDir>
#include "../src/utils/DataStore.h"
#include "../src/models/Spectrum.h"
#include "../src/models/Photometry.h"

class TestDataStore : public QObject
{
    Q_OBJECT

private slots:

    // ── Low-level compressed round-trip ─────────────────────────

    void testWriteReadCompressed()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString path = tmpDir.path() + "/sub/dir/test.asd";

        QByteArray original("Hello ASTRA compressed world! "
                            "Some filler to make it worth compressing... "
                            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

        QVERIFY(DataStore::writeCompressed(path, DataStore::SpectrumData, original));
        QVERIFY(QFile::exists(path));

        // File on disk should be smaller than raw payload
        QFileInfo fi(path);
        QVERIFY(fi.size() < original.size() + 20);   // header overhead

        QByteArray readBack;
        QVERIFY(DataStore::readCompressed(path, DataStore::SpectrumData, readBack));
        QCOMPARE(readBack, original);
    }

    void testTypeMismatchRejected()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString path = tmpDir.path() + "/typed.asd";

        QByteArray data("payload");
        QVERIFY(DataStore::writeCompressed(path, DataStore::SpectrumData, data));

        QByteArray readBack;
        // Request wrong type → should fail
        QVERIFY(!DataStore::readCompressed(path, DataStore::SEDModelData, readBack));
    }

    void testIsCompressedFormat()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Compressed file
        QString asdPath = tmpDir.path() + "/compressed.asd";
        QVERIFY(DataStore::writeCompressed(asdPath, DataStore::SpectrumData,
                                           QByteArray("test")));
        QVERIFY(DataStore::isCompressedFormat(asdPath));

        // Legacy / arbitrary file
        QString legacyPath = tmpDir.path() + "/legacy.dat";
        QFile legacyFile(legacyPath);
        QVERIFY(legacyFile.open(QIODevice::WriteOnly));
        QDataStream s(&legacyFile);
        s << quint32(0) << quint32(0) << quint32(0);
        legacyFile.close();
        QVERIFY(!DataStore::isCompressedFormat(legacyPath));
    }

    void testEmptyPayloadRoundTrip()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString path = tmpDir.path() + "/empty.asd";

        QByteArray empty;
        QVERIFY(DataStore::writeCompressed(path, DataStore::LightcurveData, empty));

        QByteArray readBack;
        QVERIFY(DataStore::readCompressed(path, DataStore::LightcurveData, readBack));
        QVERIFY(readBack.isEmpty());
    }

    // ── Path helpers ────────────────────────────────────────────

    void testPathHelpers()
    {
        QString base = "/data/astra";
        QString starId = "abc-123";
        QString specId = "spec-456";
        QString fitId  = "fit-789";
        QString photId = "phot-000";

        QCOMPARE(DataStore::starDir(base, starId),
                 QString("/data/astra/stars/abc-123"));

        QCOMPARE(DataStore::spectrumPath(base, starId, specId),
                 QString("/data/astra/stars/abc-123/spectra/spec-456.asd"));

        QCOMPARE(DataStore::spectralFitPath(base, starId, specId, fitId),
                 QString("/data/astra/stars/abc-123/spectra/fit_spec-456_fit-789.asd"));

        QCOMPARE(DataStore::photometricPointsPath(base, starId, photId),
                 QString("/data/astra/stars/abc-123/photometry/points_phot-000.asd"));

        QCOMPARE(DataStore::sedModelPath(base, starId, photId, "m1"),
                 QString("/data/astra/stars/abc-123/photometry/sed_phot-000_m1.asd"));
    }

    void testLightcurvePathSanitizesSource()
    {
        QString path = DataStore::lightcurvePath("/base", "star1", "phot1",
                                                 "TESS Sector 42");
        // spaces and special chars replaced by '_'
        QVERIFY(path.contains("TESS_Sector_42"));
        QVERIFY(!path.contains(' '));
    }

    // ── Spectrum compressed round-trip ──────────────────────────

    void testSpectrumSaveLoadCompressed()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        Spectrum original;
        std::vector<double> wl, fl, er;
        for (int i = 0; i < 5000; ++i) {
            wl.push_back(3800.0 + i * 0.5);
            fl.push_back(1.0 + 0.01 * (i % 100));
            er.push_back(0.001);
        }
        original.setData(wl, fl, er);

        QString path = tmpDir.path() + "/spectrum.asd";
        QVERIFY(original.saveDataToFile(path));
        QVERIFY(DataStore::isCompressedFormat(path));

        // Check compression actually saved space
        // 5000 points × 3 arrays × 8 bytes = 120000 raw bytes
        QFileInfo fi(path);
        QVERIFY2(fi.size() < 100000,
                 qPrintable(QString("Expected compression, got %1 bytes").arg(fi.size())));

        Spectrum loaded;
        QVERIFY(loaded.loadDataFromFile(path));
        QCOMPARE(loaded.getWavelengths().size(), size_t(5000));
        QCOMPARE(loaded.getFluxes().size(), size_t(5000));
        QCOMPARE(loaded.getFluxErrors().size(), size_t(5000));

        // Verify values survive round-trip exactly (doubles, no lossy compression)
        QCOMPARE(loaded.getWavelengths().front(), 3800.0);
        QCOMPARE(loaded.getWavelengths().back(), 3800.0 + 4999 * 0.5);
        QCOMPARE(loaded.getFluxErrors()[0], 0.001);
    }

    // ── Spectrum legacy fallback ────────────────────────────────

    void testSpectrumLegacyFallback()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Write a legacy (uncompressed) file the old way
        QString path = tmpDir.path() + "/legacy_spectrum.dat";
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QDataStream stream(&file);
            stream.setVersion(QDataStream::Qt_5_0);

            std::vector<double> wl = {4000.0, 4001.0, 4002.0};
            std::vector<double> fl = {1.0, 1.1, 0.9};
            std::vector<double> er = {0.01, 0.01, 0.01};

            stream << static_cast<quint32>(wl.size());
            stream << static_cast<quint32>(fl.size());
            stream << static_cast<quint32>(er.size());
            for (auto v : wl) stream << v;
            for (auto v : fl) stream << v;
            for (auto v : er) stream << v;
        }

        QVERIFY(!DataStore::isCompressedFormat(path));

        Spectrum loaded;
        QVERIFY(loaded.loadDataFromFile(path));
        QCOMPARE(loaded.getWavelengths().size(), size_t(3));
        QCOMPARE(loaded.getWavelengths()[1], 4001.0);
        QCOMPARE(loaded.getFluxes()[2], 0.9);
    }

    // ── SpectralFit compressed round-trip ───────────────────────

    void testSpectralFitSaveLoadCompressed()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SpectralFit original;
        for (int i = 0; i < 2000; ++i) {
            original.modelWavelengths.push_back(3800.0 + i * 1.0);
            original.modelFluxes.push_back(0.5 + 0.001 * i);
        }

        QString path = tmpDir.path() + "/fit.asd";
        QVERIFY(original.saveDataToFile(path));
        QVERIFY(DataStore::isCompressedFormat(path));

        SpectralFit loaded;
        QVERIFY(loaded.loadDataFromFile(path));
        QCOMPARE(loaded.modelWavelengths.size(), size_t(2000));
        QCOMPARE(loaded.modelFluxes.size(), size_t(2000));
        QCOMPARE(loaded.modelWavelengths.back(), 3800.0 + 1999.0);
    }

    // ── SEDModel compressed round-trip ──────────────────────────

    void testSEDModelSaveLoadCompressed()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SEDModel original;
        for (int i = 0; i < 500; ++i) {
            original.modelWavelengths.push_back(1000.0 + i * 10.0);
            original.modelFluxes.push_back(1e-15 + i * 1e-18);
        }

        QString path = tmpDir.path() + "/sed.asd";
        QVERIFY(original.saveDataToFile(path));

        SEDModel loaded;
        QVERIFY(loaded.loadDataFromFile(path));
        QCOMPARE(loaded.modelWavelengths.size(), size_t(500));
        QCOMPARE(loaded.modelFluxes[0], 1e-15);
    }

    // ── Photometric points compressed round-trip ────────────────

    void testPhotometricPointsSaveLoadCompressed()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        Photometry original;
        for (int i = 0; i < 30; ++i) {
            PhotometricPoint p;
            p.instrument = "GALEX";
            p.filter = QString("F%1").arg(i);
            p.magnitude = 12.0 + i * 0.1;
            p.magnitudeError = 0.01;
            p.flux = 1e-14;
            p.fluxError = 1e-16;
            p.wavelength = 1500.0 + i * 100.0;
            original.addPhotometricPoint(p);
        }

        QString path = tmpDir.path() + "/points.asd";
        QVERIFY(original.savePhotometricPointsToFile(path));
        QVERIFY(DataStore::isCompressedFormat(path));

        Photometry loaded;
        QVERIFY(loaded.loadPhotometricPointsFromFile(path));
        auto points = loaded.getPhotometricPoints();
        QCOMPARE(points.size(), size_t(30));
        QCOMPARE(points[0].instrument, QString("GALEX"));
        QCOMPARE(points[5].filter, QString("F5"));
    }

    // ── Lightcurve compressed round-trip ────────────────────────

    void testLightcurveSaveLoadCompressed()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        Photometry original;
        std::vector<LightcurvePoint> lc;
        for (int i = 0; i < 10000; ++i) {
            LightcurvePoint p;
            p.bjd = 2459000.0 + i * 0.001;
            p.flux = 1.0 + 0.01 * std::sin(i * 0.1);
            p.fluxError = 0.001;
            p.filter = "TESS";
            lc.push_back(p);
        }
        original.addLightcurve("TESS", lc);

        QString path = tmpDir.path() + "/lightcurve.asd";
        QVERIFY(original.saveLightcurveToFile("TESS", path));
        QVERIFY(DataStore::isCompressedFormat(path));

        // 10000 points — verify compression works
        QFileInfo fi(path);
        // Raw: ~10000 * (8+8+8 + ~4 for string) ≈ 280 KB
        QVERIFY2(fi.size() < 200000,
                 qPrintable(QString("Expected compression, got %1 bytes").arg(fi.size())));

        Photometry loaded;
        QVERIFY(loaded.loadLightcurveFromFile("TESS", path));
        auto loadedLc = loaded.getLightcurve("TESS");
        QCOMPARE(loadedLc.size(), size_t(10000));
        QCOMPARE(loadedLc[0].bjd, 2459000.0);
        QCOMPARE(loadedLc[0].filter, QString("TESS"));
    }

    // ── removeStarData ──────────────────────────────────────────

    void testRemoveStarData()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString base   = tmpDir.path();
        QString starId = "deadbeef-1234";

        // Create some files under the star dir
        QString specPath = DataStore::spectrumPath(base, starId, "s1");
        QVERIFY(DataStore::writeCompressed(specPath, DataStore::SpectrumData,
                                           QByteArray("data")));
        QVERIFY(QFile::exists(specPath));

        QString photPath = DataStore::photometricPointsPath(base, starId, "p1");
        QVERIFY(DataStore::writeCompressed(photPath, DataStore::PhotometricPointsData,
                                           QByteArray("data")));

        // Nuke it
        QVERIFY(DataStore::removeStarData(base, starId));

        // Verify everything is gone
        QVERIFY(!QFile::exists(specPath));
        QVERIFY(!QFile::exists(photPath));
        QVERIFY(!QDir(DataStore::starDir(base, starId)).exists());
    }
};

QTEST_MAIN(TestDataStore)
#include "test_datastore.moc"