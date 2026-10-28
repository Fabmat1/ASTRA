#pragma once

/*  AiHost inside the window: projects and stars through the
 *  ApplicationController, so the open project's stars are the very objects
 *  the table and the detail windows show.                                  */

#include "ai/AiHost.h"

class ApplicationController;
class MainWindow;

namespace astra::ai::ui {

class GuiAiHost : public AiHost {
public:
    GuiAiHost(ApplicationController* controller, MainWindow* window);

    ServiceContext* services() const override;
    bool hasWindow() const override { return true; }
    QJsonObject windowState() const override;

    std::vector<std::shared_ptr<Project>> projects() const override;
    std::shared_ptr<Project> currentProject() const override;
    std::shared_ptr<Project> createProject(const QString& name, const QString& description) override;
    bool deleteProject(const QString& projectId) override;
    bool saveProject(const std::shared_ptr<Project>& p) override;

    SpectrumFetchService*   spectrumFetchService() override;
    LightcurveFetchService* lightcurveFetchService() override;
    MassFitService*         massFitService() override;

    bool addStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars) override;
    bool deleteStars(const QString& projectId, const QStringList& starIds) override;

    ApplicationController* controller() const { return _controller; }
    MainWindow* window() const { return _window; }

private:
    ApplicationController* _controller;
    MainWindow*            _window;
};

} // namespace astra::ai::ui
