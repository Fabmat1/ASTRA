#pragma once

#include <QDialog>
#include <memory>

class Star;

class LightcurveFetchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LightcurveFetchDialog(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~LightcurveFetchDialog() override;

private:
    void setupUi();

    std::shared_ptr<Star> _star;
};