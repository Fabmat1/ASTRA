#include <QApplication>
#include "views/MainWindow.h"
#include "controllers/ApplicationController.h"
#include "utils/Logger.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ASTRA");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("ASTRA");
    
    // Initialize logging system
    Logger::initialize("ASTRA");
    LOG_INFO("Main", "ASTRA starting up");
    
    ApplicationController controller;
    MainWindow window(&controller);
    window.show();
    
    LOG_INFO("Main", "Main window displayed");
    
    int result = app.exec();
    
    LOG_INFO("Main", QString("ASTRA shutting down with exit code %1").arg(result));
    Logger::shutdown();
    
    return result;
}