#pragma once

#include <QDialog>
#include <memory>

class Star;

class SpectraFitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SpectraFitDialog(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~SpectraFitDialog() override;

private:
    void setupUi();

    std::shared_ptr<Star> _star;
};