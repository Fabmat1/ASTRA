#include <QApplication>
#include "views/MainWindow.h"
#include "controllers/ApplicationController.h"
#include "utils/Logger.h"
#include "utils/AppPaths.h"
#include "db/DatabaseManager.h"
#include <QMessageBox>
#include "utils/UiIcons.h"
#include "utils/WindowSizing.h"
#include "fitting/FitTypes.h"
#include "fitting/FitBackendRegistry.h"
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include "astra_version.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ASTRA");
    app.setApplicationVersion(ASTRA_VERSION_STRING);
    app.setOrganizationName("ASTRA");

    // Keeps dialogs whose content grows after construction from pushing their
    // own window past the edge of the screen.
    WindowSizing::installScreenGuard(&app);

    // Dialog button boxes otherwise pick up Ok/Cancel/Close icons from the
    // desktop icon theme, which matches nothing else in ASTRA.
    UiIcons::installDialogButtonIcons(&app);

    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Regular.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Medium.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Bold.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Light.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Retina.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-SemiBold.ttf");

    // Symbol-rich fallback font so niche unicode glyphs (✓ ✗ ▲ ▼ → etc.)
    // used in labels/text always render, even when the resolved UI font lacks them.
    QFontDatabase::addApplicationFont(":/fonts/DejaVuSans.ttf");
    QFontDatabase::addApplicationFont(":/fonts/DejaVuSans-Bold.ttf");

    // Register DejaVu Sans as a fallback substitute for the UI font families
    // declared in the theme QSS, so glyphs missing from the primary font are
    // sourced from DejaVu Sans instead of rendering as a missing-glyph box.
    const QStringList uiFamilies = {
        "Segoe UI", "SF Pro Display", "Helvetica Neue", "Arial",
        "sans-serif", "FiraCode"
    };
    for (const QString& family : uiFamilies) {
        QFont::insertSubstitution(family, "DejaVu Sans");
    }

    // Initialize paths (uses compile-time ASTRA_DATA_DIR, or QStandardPaths if empty)
    AppPaths::initialize();

    // Initialize logging system
    Logger::initialize("ASTRA");
    LOG_INFO("Main", QString("Data root: %1").arg(AppPaths::root()));
    
    qRegisterMetaType<astra::fitting::SpectralFitResult>();
    qRegisterMetaType<astra::fitting::SpectralFitJob>();
    qRegisterMetaType<astra::fitting::FitProgressInfo>();

    ApplicationController controller;
    MainWindow            window(&controller);
    window.show();

    // A damaged database file opens fine and reads most rows, but silently
    // rolls back every write - say so up front instead of letting imports
    // fail one by one.
    if (auto* dbm = controller.databaseManager(); dbm && !dbm->isHealthy()) {
        QMessageBox::critical(
            &window, "Database Damaged",
            QString("The database at\n%1\nfailed its integrity check:\n\n%2\n\n"
                    "Reads may return partial data and nothing can be saved - "
                    "imports will roll back. Repair it with:\n\n"
                    "  sqlite3 astra.db \".recover\" | sqlite3 astra_fixed.db\n\n"
                    "then replace the file with the repaired copy. Keep the "
                    "damaged file until you have checked the result.")
                .arg(AppPaths::database(), dbm->integrityError()));
    }

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i].endsWith(".astra", Qt::CaseInsensitive)) {
            window.importStarPackage(args[i]);
            break;
        }
    }

    LOG_INFO("Main", "Main window displayed");

    int result = app.exec();

    LOG_INFO("Main", QString("ASTRA shutting down with exit code %1").arg(result));
    Logger::shutdown();

    return result;
}