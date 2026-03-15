#pragma once

#include <QDialog>
#include <memory>

class Star;

class SEDFitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SEDFitDialog(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~SEDFitDialog() override;

private:
    void setupUi();

    std::shared_ptr<Star> _star;
};