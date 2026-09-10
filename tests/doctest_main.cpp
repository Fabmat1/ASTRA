// Entry point for the doctest-based unit tests.
//
// DOCTEST_CONFIG_IMPLEMENT (rather than ..._WITH_MAIN) so a QCoreApplication
// exists before any test runs: Core classes reached from these tests create
// QTimer, QThreadPool and QNetworkAccessManager objects, which need an
// application object and an event loop to attach to.
//
// Tests are grouped with TEST_SUITE("<domain>"); run one domain with
//     ./astra_tests -ts=rv
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <QCoreApplication>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
