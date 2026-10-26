// Unit tests for fitting/UserFitDefaults.
//
// The saved defaults go through QSettings, so these run under a throwaway
// organisation name and leave the user's real settings alone.

#include <doctest.h>

#include "app/AppSettings.h"
#include "fitting/UserFitDefaults.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

using namespace astra::fitting;

namespace {

/*  Points QSettings at a scratch file for the lifetime of the guard.      */
struct ScratchSettings {
    QString org = QCoreApplication::organizationName();
    QString app = QCoreApplication::applicationName();
    ScratchSettings()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("ASTRA-unittest"));
        QCoreApplication::setApplicationName(QStringLiteral("user_fit_defaults"));
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

} // namespace

TEST_SUITE("fitting")
{

TEST_CASE("UserFitDefaults: nothing saved means built-in defaults")
{
    ScratchSettings scratch;
    CHECK(UserFitDefaults::load(AppSettings()).isEmpty());
}

TEST_CASE("UserFitDefaults: components round trip")
{
    ScratchSettings scratch;
    UserFitDefaults d;
    StellarComponent a;
    a.gridPath = "sdB/processed";
    a.teff = 31000.0;
    a.freezeVsini = false;
    StellarComponent b;
    b.gridPath = "MS/processed";
    b.surRatio = 4.0;
    d.components = {a, b};
    {
        AppSettings s;
        d.save(s);
    }

    const auto back = UserFitDefaults::load(AppSettings());
    REQUIRE(back.components.size() == 2);
    CHECK(back.components[0].gridPath == "sdB/processed");
    CHECK(back.components[0].teff == doctest::Approx(31000.0));
    CHECK_FALSE(back.components[0].freezeVsini);
    CHECK(back.components[1].surRatio == doctest::Approx(4.0));
}

TEST_CASE("UserFitDefaults: saving none clears the setting")
{
    ScratchSettings scratch;
    UserFitDefaults d;
    d.components = {StellarComponent{}};
    {
        AppSettings s;
        d.save(s);
        UserFitDefaults{}.save(s);
    }
    CHECK(AppSettings().spectralFitDefaultsJson().isEmpty());
}

TEST_CASE("UserFitDefaults: a long-lived AppSettings does not revert a save")
{
    ScratchSettings scratch;
    AppSettings longLived;          // loaded before the defaults exist
    UserFitDefaults d;
    d.components = {StellarComponent{}};
    {
        AppSettings s;
        d.save(s);
    }
    longLived.setSpecFetchLastOptions(QStringLiteral("{\"x\":1}"));   // save()
    CHECK_FALSE(UserFitDefaults::load(AppSettings()).isEmpty());
}

}
