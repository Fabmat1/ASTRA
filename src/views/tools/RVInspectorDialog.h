#pragma once

#include <QDialog>
#include <memory>

class Star;

class RVInspectorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RVInspectorDialog(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~RVInspectorDialog() override;

private:
    void setupUi();

    std::shared_ptr<Star> _star;
};