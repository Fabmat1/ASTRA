// Resolving an instrument string to an instrument and mode.
//
// ASTRA shows a spectrum's instrument as "Name (Mode)", e.g.
// "GTC (OSIRIS R2000B)", and agents pass that string straight back to
// import_spectrum. The resolver split it on separators into the name "GTC" and
// the mode "(OSIRIS_R2000B)", matched the instrument, matched no mode, and
// returned the instrument alone: the spectrum was stored without a mode and
// fitted with the instrument's fallback range and resolution, with no warning.

#include <doctest.h>

#include "core/Instrument.h"
#include "core/InstrumentMode.h"
#include "db/DatabaseManager.h"

#include <QTemporaryDir>

#include <memory>

namespace {

std::shared_ptr<Instrument> makeGtc()
{
    auto inst = std::make_shared<Instrument>(QStringLiteral("GTC"), 28.7567, -17.8917, 2267.0);
    inst->addMode(InstrumentMode(QStringLiteral("OSIRIS_R1000B"), QStringLiteral("OSIRIS R1000B"),
                                 InstrumentMode::Spectroscopy));
    inst->addMode(InstrumentMode(QStringLiteral("OSIRIS_R2000B"), QStringLiteral("OSIRIS R2000B"),
                                 InstrumentMode::Spectroscopy));
    return inst;
}

/// The mode key the resolver picks for input, or "<no instrument>".
QString resolvedMode(const DatabaseManager& dbm, const QString& input, const QString& wantInstrument)
{
    QString mode;
    const auto inst = dbm.resolveInstrumentString(input, &mode);
    if (!inst) return QStringLiteral("<no instrument>");
    if (inst->getName() != wantInstrument) return QStringLiteral("<wrong instrument: %1>").arg(inst->getName());
    return mode;
}

}   // namespace

TEST_SUITE("db")
{

TEST_CASE("resolveInstrumentString keeps the mode of every spelling")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(dbm.saveInstrument(makeGtc()));

    const QString want = QStringLiteral("OSIRIS_R2000B");

    // The display form, as list_spectra and the spectra table show it.
    CHECK(resolvedMode(dbm, "GTC (OSIRIS R2000B)", "GTC") == want);
    CHECK(resolvedMode(dbm, "  GTC(OSIRIS R2000B) ", "GTC") == want);
    CHECK(resolvedMode(dbm, "gtc (osiris r2000b)", "GTC") == want);
    CHECK(resolvedMode(dbm, "GTC (OSIRIS_R2000B)", "GTC") == want);

    // The spellings that already worked must keep working.
    CHECK(resolvedMode(dbm, "GTC OSIRIS_R2000B", "GTC") == want);
    CHECK(resolvedMode(dbm, "GTC/OSIRIS_R2000B", "GTC") == want);
    CHECK(resolvedMode(dbm, "GTC OSIRIS R2000B", "GTC") == want);
    CHECK(resolvedMode(dbm, "GTC", "GTC") == QString());

    // The other mode is not confused with this one.
    CHECK(resolvedMode(dbm, "GTC (OSIRIS R1000B)", "GTC") == QStringLiteral("OSIRIS_R1000B"));
}

TEST_CASE("an instrument whose own name has brackets still matches by name")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    DatabaseManager dbm;
    REQUIRE(dbm.openDatabase(tmp.filePath("astra_test.db")));
    REQUIRE(dbm.saveInstrument(makeGtc()));
    auto odd = std::make_shared<Instrument>(QStringLiteral("GTC (old camera)"), 28.7567, -17.8917, 2267.0);
    REQUIRE(dbm.saveInstrument(odd));

    QString mode;
    const auto inst = dbm.resolveInstrumentString(QStringLiteral("GTC (old camera)"), &mode);
    REQUIRE(inst);
    CHECK(inst->getName() == QStringLiteral("GTC (old camera)"));
    CHECK(mode.isEmpty());
}

}
