#include <QApplication>
#include <QStyleFactory>
#include "views/MainWindow.h"
#include "controllers/ApplicationController.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Set application metadata
    QCoreApplication::setOrganizationName("ASTRA");
    QCoreApplication::setOrganizationDomain("astra.app");
    QCoreApplication::setApplicationName("ASTRA");
    QCoreApplication::setApplicationVersion("0.1.0");

    // Initialize application controller
    ApplicationController controller;

    // Create and show main window
    MainWindow window(&controller);
    window.show();

    return app.exec();
}