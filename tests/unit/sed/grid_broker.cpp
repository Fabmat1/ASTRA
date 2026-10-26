// Unit tests for sed/SedGridBroker.
//
// sedfit asks its parent for streamed (ssh://) grid files over stdout/stdin.
// The real counterpart is SEDplusplus' gridio.cpp; here a shell script plays
// sedfit, and a fake provider stands in for SshGridProvider, so the protocol
// and the stdout split are checked without a network or a sedfit binary.

#include <doctest.h>

#include "sed/SedGridBroker.h"

#include <specfit/GridDataProvider.hpp>

#include <QEventLoop>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <stdexcept>

namespace {

/*  Serves "fake://<name>" as "/local/<name>"; "fake://missing" does not
 *  exist and cannot be fetched.                                           */
class FakeProvider : public specfit::GridDataProvider {
public:
    bool handles(const std::string& p) const override
    { return p.rfind("fake://", 0) == 0; }
    bool exists(const std::string& p) override
    { return p != "fake://missing"; }
    std::string localize(const std::string& p) override
    {
        if (p == "fake://missing") throw std::runtime_error("no such file");
        return "/local/" + p.substr(7);
    }
    void prefetch(const std::vector<std::string>& ps) override
    { prefetched += int(ps.size()); }
    int prefetched = 0;
};

/*  Runs `script` under the broker; returns everything it passed on.       */
QStringList runScript(const QString& script)
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    SedGridBroker::enableIn(env);
    proc.setProcessEnvironment(env);
    SedGridBroker broker(&proc);
    QStringList out;
    QObject::connect(&broker, &SedGridBroker::output,
                     [&out](const QString& t) { out << t; });

    QEventLoop loop;
    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    proc.start(QStringLiteral("sh"), {QStringLiteral("-c"), script});
    loop.exec();
    broker.flush();
    return out;
}

} // namespace

TEST_SUITE("sed")
{

TEST_CASE("SedGridBroker: answers requests and passes log lines through")
{
    auto provider = std::make_shared<FakeProvider>();
    specfit::set_grid_data_provider(provider);

    const QStringList out = runScript(QStringLiteral(
        "[ \"$SEDFIT_GRID_BROKER\" = stdio ] || echo 'broker not enabled'\n"
        "echo 'first log line'\n"
        "printf '@@SEDFIT_GRID\\tEXISTS\\tfake://a/grid.fits\\n'; read r; echo \"exists:$r\"\n"
        "printf '@@SEDFIT_GRID\\tEXISTS\\tfake://missing\\n'; read r; echo \"missing:$r\"\n"
        "printf '@@SEDFIT_GRID\\tGET\\tfake://a/T1.fits\\tfake://a/T2.fits\\n'; read r; echo \"get:$r\"\n"
        "printf '@@SEDFIT_GRID\\tGET\\tfake://missing\\n'; read r; echo \"bad:${r%%\t*}\"\n"
        "printf 'no newline at the end'\n"));
    const QString all = out.join(QLatin1Char('\n'));

    CHECK_FALSE(all.contains("broker not enabled"));
    CHECK(all.contains("first log line"));
    CHECK(all.contains("exists:1"));
    CHECK(all.contains("missing:0"));
    CHECK(all.contains("get:OK\t/local/a/T1.fits\t/local/a/T2.fits"));
    CHECK(all.contains("bad:ERR"));
    CHECK(all.contains("no newline at the end"));
    // requests never reach the log
    CHECK_FALSE(all.contains("@@SEDFIT_GRID"));
    // every GET is announced as one batch first, the failing one included
    CHECK(provider->prefetched == 3);
    CHECK(all.contains("Streamed grid files used: 2"));

    specfit::set_grid_data_provider(nullptr);
}

TEST_CASE("SedGridBroker: a local path is refused, not localized")
{
    specfit::set_grid_data_provider(std::make_shared<FakeProvider>());
    const QStringList out = runScript(QStringLiteral(
        "printf '@@SEDFIT_GRID\\tGET\\t/etc/passwd\\n'; read r; echo \"reply:${r%%\t*}\"\n"));
    CHECK(out.join(QLatin1Char('\n')).contains("reply:ERR"));
    specfit::set_grid_data_provider(nullptr);
}

}
