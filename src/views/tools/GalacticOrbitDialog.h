#pragma once

#include <QDialog>
#include <memory>

class Star;

class GalacticOrbitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GalacticOrbitDialog(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~GalacticOrbitDialog() override;

private:
    void setupUi();

    std::shared_ptr<Star> _star;
};