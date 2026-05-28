#include <QApplication>
#include "views/MainWindow.h"
#include "controllers/ApplicationController.h"
#include "utils/Logger.h"
#include "utils/AppPaths.h"
#include "fitting/FitTypes.h"
#include "fitting/FitBackendRegistry.h"
#include <QDebug>
#include <QFontDatabase>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ASTRA");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("ASTRA");

    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Regular.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Medium.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Bold.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Light.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-Retina.ttf");
    QFontDatabase::addApplicationFont(":/fonts/FiraCode-SemiBold.ttf");

    // Initialize paths (uses compile-time ASTRA_DATA_DIR, or QStandardPaths if empty)
    AppPaths::initialize();

    // Initialize logging system
    Logger::initialize("ASTRA");
    LOG_INFO("Main", QString("Data root: %1").arg(AppPaths::root()));
    
    qRegisterMetaType<astra::fitting::SpectralFitResult>();
    qRegisterMetaType<astra::fitting::SpectralFitJob>();

    ApplicationController controller;
    MainWindow window(&controller);
    window.show();

    LOG_INFO("Main", "Main window displayed");

    int result = app.exec();

    LOG_INFO("Main", QString("ASTRA shutting down with exit code %1").arg(result));
    Logger::shutdown();

    return result;
}