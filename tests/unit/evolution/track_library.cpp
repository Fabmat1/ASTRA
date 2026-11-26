// TrackSetLibrary and EvolutionSettings: scanning a tracks folder, subsets as sdfit
// selects them, the Han interpolation choice, plot curves, and the folder setting.
//
// Every track file here is synthetic (synthetic_tracks.h); the real track sets are
// unpublished and never part of the repository.

#include <doctest.h>

#include "evolution/EvolutionSettings.h"
#include "evolution/TrackSetLibrary.h"
#include "synthetic_tracks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>

using namespace astra::evolution;

namespace {

struct ScratchSettings {
    QString org = QCoreApplication::organizationName();
    QString app = QCoreApplication::applicationName();
    ScratchSettings()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("ASTRA-unittest"));
        QCoreApplication::setApplicationName(QStringLiteral("evolution_settings"));
        QSettings().clear();
    }
    ~ScratchSettings()
    {
        const QString file = QSettings().fileName();
        QFile::remove(file);
        QDir().rmdir(QFileInfo(file).absolutePath());
        QCoreApplication::setOrganizationName(org);
        QCoreApplication::setApplicationName(app);
    }
};

#ifdef HAVE_CCFITS

// Dorman-like: 5 compositions x 5 envelopes; z22 and z42 lie only 0.003 Msun apart in
// core mass (numerical scatter in sdfit's terms).
sdage::TrackGrid dormanLike()
{
    struct G {
        const char* name;
        double feh, core;
    };
    const G groups[] = {{"yz62", 0.71, 0.434}, {"z42", 0.39, 0.466}, {"z22", 0.0, 0.469},
                        {"j63", -0.47, 0.475}, {"g14", -2.26, 0.495}};
    std::vector<double> core, env;
    std::vector<std::string> label;
    std::vector<double> feh;
    for (const G& g : groups)
        for (int k = 0; k < 5; ++k) {
            core.push_back(g.core + 1e-4 * k);
            env.push_back(0.002 + 0.003 * k * k + 0.0001 * g.feh);
            label.push_back(g.name);
            feh.push_back(g.feh);
        }
    sdage::TrackGrid gr = synth::scatterAt(core, env);
    gr.set_node_groups(label, feh);
    return gr;
}

// Nicolas-like: irregular nodes.
sdage::TrackGrid nicolasLike()
{
    const double cores[] = {0.40, 0.42, 0.45, 0.47, 0.50, 0.53, 0.55};
    std::vector<double> core, env;
    for (int i = 0; i < 7; ++i)
        for (int k = 0; k < 4; ++k) {
            core.push_back(cores[i]);
            env.push_back(0.0005 * (i % 3) + 0.002 * k);
        }
    return synth::scatterAt(core, env);
}

struct Folder {
    QTemporaryDir tmp;
    QString path;
    Folder()
    {
        REQUIRE(tmp.isValid());
        path = tmp.path();
        const std::string d = path.toStdString() + "/";
        synth::writeHanStyle(d + "han_tracks_Z0_extended_925.fits.gz", synth::rectGrid());
        synth::writeHanTriangulation(d + "han_tracks_Z0_extended_925.scipy_tri.txt");
        synth::writeTrackSet(d + "han.sdtracks.fits.gz", "han", synth::scatterGrid());  // shadowed
        synth::writeTrackSet(d + "nicolas.sdtracks.fits.gz", "nicolas", nicolasLike());
        synth::writeTrackSet(d + "dorman.sdtracks.fits.gz", "dorman", dormanLike());
        synth::writeTrackSet(d + "driebe.sdtracks.fits.gz", "driebe", synth::lineGrid());
        QFile junk(path + "/broken.sdtracks.fits.gz");
        REQUIRE(junk.open(QIODevice::WriteOnly));
        junk.write("not a FITS file");
    }
};

#endif

}  // namespace

TEST_SUITE("evolution")
{

TEST_CASE("EvolutionSettings: the tracks folder")
{
    ScratchSettings scratch;
    EvolutionSettings& s = EvolutionSettings::instance();
    CHECK(EvolutionSettings::tracksFolder().isEmpty());

    int folderSignals = 0;
    const auto c = QObject::connect(&s, &EvolutionSettings::tracksFolderChanged, [&] { ++folderSignals; });
    s.setTracksFolder(QStringLiteral("  /some/where//tracks/ "));
    CHECK(EvolutionSettings::tracksFolder() == QStringLiteral("/some/where/tracks"));
    CHECK(folderSignals == 1);
    s.setTracksFolder(QStringLiteral("/some/where/tracks"));  // no change, no signal
    CHECK(folderSignals == 1);
    // stored outside AppSettings' group, so AppSettings::save() cannot revert it
    CHECK(QSettings().contains(QStringLiteral("evolution/tracksFolder")));
    s.setTracksFolder(QString());
    CHECK(EvolutionSettings::tracksFolder().isEmpty());
    QObject::disconnect(c);
}

TEST_CASE("TrackSubset and TrackModelSettings round-trip through JSON")
{
    TrackSubset s;
    s.cores = {0.45, 0.5};
    CHECK(TrackSubset::fromJson(s.toJson(SubsetKind::Cores)) == s);

    TrackSubset r;
    r.massLo = 0.42;
    r.massHi = 0.5;
    CHECK(TrackSubset::fromJson(r.toJson(SubsetKind::MassRange)) == r);
    TrackSubset open;
    CHECK(TrackSubset::fromJson(open.toJson(SubsetKind::MassRange)) == open);

    TrackSubset g;
    g.groups = {"z22", "g14"};
    g.envelopes = 0;
    CHECK(TrackSubset::fromJson(g.toJson(SubsetKind::Groups)) == g);

    TrackModelSettings m;
    m.eepPrior = EepPriorChoice::SdO;
    m.massPrior = true;
    m.walkers = 200;
    m.burn = 50;
    m.steps = 70;
    m.seed = 0xFEDCBA9876543210ULL;  // needs all 64 bits
    m.hanScipyInterpolation = true;
    const TrackModelSettings back = TrackModelSettings::fromJson(m.toJson());
    CHECK(back.eepPrior == m.eepPrior);
    CHECK(back.massPrior);
    CHECK(back.walkers == 200);
    CHECK(back.burn == 50);
    CHECK(back.steps == 70);
    CHECK(back.seed == m.seed);
    CHECK(back.hanScipyInterpolation);

    const sdage::FitConfig fc = m.toFitConfig();
    CHECK(fc.model.eep_prior == sdage::EepPrior::Legacy);
    CHECK(fc.model.eep_shape == sdage::EepPriorShape::Fast);
    CHECK(fc.model.mass_prior);
    CHECK(fc.sampler.n_walkers == 200);
    CHECK(TrackModelSettings().toFitConfig().sampler.n_walkers == 1000);  // sdfit's default
}

#ifdef HAVE_CCFITS

TEST_CASE("TrackSetLibrary: scanning a tracks folder")
{
    Folder folder;
    TrackSetLibrary lib(folder.path);
    const QVector<TrackFamilyInfo> fams = lib.families();
    QStringList keys;
    for (const auto& f : fams) keys << f.key;
    CHECK(keys == QStringList({"han", "nicolas", "dorman", "driebe", "broken"}));

    const TrackFamilyInfo han = lib.family("han");
    CHECK(han.usable());
    CHECK(han.rectangular);  // the FITS file wins over han.sdtracks.fits.gz
    CHECK(han.fileName == QStringLiteral("han_tracks_Z0_extended_925.fits.gz"));
    CHECK_FALSE(han.triangulationPath.isEmpty());
    CHECK(han.subsetKind == SubsetKind::Cores);
    CHECK(han.nTracks == 12);
    CHECK(han.coreMasses.size() == 4);
    CHECK(han.sha1.size() == 40);

    const TrackFamilyInfo dor = lib.family("dorman");
    CHECK(dor.subsetKind == SubsetKind::Groups);
    CHECK(dor.groups == QStringList({"g14", "j63", "z22", "z42", "yz62"}));  // by [Fe/H]
    CHECK(dor.groupFeh.first() == doctest::Approx(-2.26));
    CHECK(dor.maxEnvelopesPerGroup == 5);

    CHECK(lib.family("nicolas").subsetKind == SubsetKind::MassRange);
    const TrackFamilyInfo dri = lib.family("driebe");
    CHECK(dri.ndim == 1);
    CHECK(dri.subsetKind == SubsetKind::MassRange);

    const TrackFamilyInfo broken = lib.family("broken");
    CHECK_FALSE(broken.usable());
    CHECK_FALSE(broken.summary().isEmpty());

    CHECK(lib.family("nonexistent").key.isEmpty());
    CHECK(TrackSetLibrary(QString()).families().isEmpty());
    CHECK(TrackSetLibrary(folder.path + "/missing").families().isEmpty());
}

TEST_CASE("TrackSetLibrary: a changed file is re-read")
{
    Folder folder;
    TrackSetLibrary lib(folder.path);
    const QString before = lib.family("driebe").sha1;
    // rewrite with one track fewer (different size, so the cache notices for sure)
    sdage::TrackGrid line = synth::lineGrid();
    line = line.select_nodes({true, true, true, false});
    synth::writeTrackSet((folder.path + "/driebe.sdtracks.fits.gz").toStdString(), "driebe", line);
    const TrackFamilyInfo after = lib.family("driebe");
    CHECK(after.sha1 != before);
    CHECK(after.nTracks == 3);
}

TEST_CASE("TrackSetLibrary: subsets as sdfit selects them")
{
    Folder folder;
    TrackSetLibrary lib(folder.path);
    TrackModelSettings model;
    QString why;

    SUBCASE("Han: core masses, bilinear or scipy's triangles")
    {
        const TrackFamilyInfo han = lib.family("han");
        auto p = lib.prepare("han", TrackSetLibrary::defaultSubset(han), model, &why);
        REQUIRE(p);
        CHECK(p->grid == p->full);
        CHECK(p->interpolation == QStringLiteral("bilinear"));
        CHECK(p->grid->mode() == sdage::InterpMode::Bilinear);

        model.hanScipyInterpolation = true;
        p = lib.prepare("han", TrackSetLibrary::defaultSubset(han), model, &why);
        REQUIRE(p);
        CHECK(p->grid->mode() == sdage::InterpMode::Triangulated);

        TrackSubset s;
        s.cores = {0.55, 0.45};
        p = lib.prepare("han", s, model, &why);
        REQUIRE(p);
        CHECK(p->grid->n_core() == 2);
        CHECK(p->subset.cores == QVector<double>({0.45, 0.55}));
        CHECK(p->interpolation.startsWith(QStringLiteral("bilinear")));  // subset: no scipy triangles
        CHECK(std::count(p->keep.begin(), p->keep.end(), true) == 6);

        s.cores = {0.47, 0.5};
        CHECK_FALSE(lib.prepare("han", s, model, &why));
        CHECK(why.contains(QStringLiteral("0.47")));
        s.cores = {0.5};
        CHECK_FALSE(lib.prepare("han", s, model, &why));
        CHECK(why.contains(QStringLiteral("two core masses")));
    }

    SUBCASE("Nicolas: mass range snapped to the tracks, with a widening hint")
    {
        TrackSubset s;
        s.massLo = 0.41;
        s.massHi = 0.52;
        auto p = lib.prepare("nicolas", s, model, &why);
        REQUIRE(p);
        CHECK(p->subset.massLo == doctest::Approx(0.42));
        CHECK(p->subset.massHi == doctest::Approx(0.50));
        CHECK(p->grid->n_nodes() == 16);  // cores 0.42, 0.45, 0.47, 0.50 x 4 envelopes
        CHECK(p->interpolation == QStringLiteral("Delaunay"));

        s.massLo = 0.465;
        s.massHi = 0.48;  // a single core mass: one line, cannot be triangulated
        CHECK_FALSE(lib.prepare("nicolas", s, model, &why));
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("Widen it to ([0-9.]+)-([0-9.]+) Msun")).match(why);
        REQUIRE_MESSAGE(m.hasMatch(), why.toStdString());
        s.massLo = m.captured(1).toDouble();
        s.massHi = m.captured(2).toDouble();
        CHECK(lib.prepare("nicolas", s, model, &why));  // the suggestion works

        s.massLo = 0.6;
        s.massHi = 0.5;
        CHECK_FALSE(lib.prepare("nicolas", s, model, &why));
    }

    SUBCASE("Dorman: compositions and the thinnest envelopes")
    {
        const TrackFamilyInfo dor = lib.family("dorman");
        auto p = lib.prepare("dorman", TrackSetLibrary::defaultSubset(dor), model, &why);
        REQUIRE(p);
        CHECK(p->grid->n_nodes() == 15);  // 5 compositions x 3 envelopes, as sdfit
        CHECK(p->summary.contains(QStringLiteral("3 thinnest envelopes")));
        // the three thinnest of each composition
        for (int n = 0; n < p->grid->n_nodes(); ++n) CHECK(p->grid->node_env(n) < 0.002 + 0.003 * 4 + 0.001);

        TrackSubset s;
        s.groups = {"yz62", "g14"};
        s.envelopes = 0;
        p = lib.prepare("dorman", s, model, &why);
        REQUIRE(p);
        CHECK(p->grid->n_nodes() == 10);
        CHECK(p->subset.groups == QStringList({"g14", "yz62"}));  // [Fe/H] order

        s.groups = {"z22", "z42"};  // 0.003 Msun apart: no core-mass axis
        CHECK_FALSE(lib.prepare("dorman", s, model, &why));
        CHECK(why.contains(QStringLiteral("scatter")));
        s.groups = {"z22"};
        CHECK_FALSE(lib.prepare("dorman", s, model, &why));
        s.groups = {"z22", "x99"};
        CHECK_FALSE(lib.prepare("dorman", s, model, &why));
    }

    SUBCASE("Driebe: a 1-D range needs two tracks")
    {
        TrackSubset s;
        s.massLo = 0.24;
        s.massHi = 0.26;
        CHECK_FALSE(lib.prepare("driebe", s, model, &why));
        CHECK(why.contains(QStringLiteral("Widen it to")));
        s.massHi = 0.30;
        auto p = lib.prepare("driebe", s, model, &why);
        REQUIRE(p);
        CHECK(p->grid->n_nodes() == 2);
    }

    SUBCASE("missing and broken families")
    {
        CHECK_FALSE(lib.prepare("nonexistent", TrackSubset(), model, &why));
        CHECK(why.contains(QStringLiteral("not in the evolutionary tracks folder")));
        CHECK_FALSE(lib.prepare("broken", TrackSubset(), model, &why));
    }
}

TEST_CASE("TrackSetLibrary: plot curves and a fit on a prepared subset")
{
    Folder folder;
    TrackSetLibrary lib(folder.path);
    TrackModelSettings model;
    model.walkers = 100;
    model.burn = 150;
    model.steps = 200;
    TrackSubset s;
    s.massLo = 0.42;
    s.massHi = 0.53;
    QString why;
    const auto p = lib.prepare("nicolas", s, model, &why);
    REQUIRE(p);

    const TrackCurves hrd = TrackSetLibrary::curves(*p, CurvePlane::HRD);
    CHECK(hrd.lines.size() == p->full->n_nodes());
    CHECK(std::count(hrd.inSubset.begin(), hrd.inSubset.end(), true) == p->grid->n_nodes());
    const QPointF first = hrd.lines[0].first();
    CHECK(first.x() == doctest::Approx(synth::fn(sdage::Q_LOGTEFF, hrd.core[0], hrd.env[0], 0.0)));
    CHECK(first.y() == doctest::Approx(synth::fn(sdage::Q_LOGL, hrd.core[0], hrd.env[0], 0.0)));
    const TrackCurves kiel = TrackSetLibrary::curves(*p, CurvePlane::Kiel);
    CHECK(kiel.lines[0].first().y() == doctest::Approx(synth::fn(sdage::Q_LOGG, hrd.core[0], hrd.env[0], 0.0)));

    // a synthetic star on a track inside the subset is recovered
    const sdage::Fitter fitter(*p->grid, model.toFitConfig());
    const double truth[3] = {0.47, 0.003, 25.0};
    sdage::Derived td;
    REQUIRE(sdage::Posterior(*p->grid, fitter.config().model, sdage::Observation()).derive(truth, td));
    sdage::Observation ob;
    ob.use[sdage::OBS_LOGTEFF] = ob.use[sdage::OBS_LOGL] = true;
    ob.value[sdage::OBS_LOGTEFF] = td.log_teff;
    ob.value[sdage::OBS_LOGL] = td.log_l;
    ob.sigma[sdage::OBS_LOGTEFF] = 0.005;
    ob.sigma[sdage::OBS_LOGL] = 0.02;
    const sdage::FitResult r = fitter.fit(ob, sdage::mix_seed(model.seed, 0));
    REQUIRE(r.ok);
    CHECK(r.in_region == 1);
    CHECK(std::fabs(r.log_teff.median - td.log_teff) < 0.005);
}

#endif  // HAVE_CCFITS

}  // TEST_SUITE
