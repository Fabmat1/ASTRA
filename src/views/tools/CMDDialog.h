#pragma once

#include <QDialog>
#include <memory>

class Star;

class CMDDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CMDDialog(std::shared_ptr<Star> star, QWidget* parent = nullptr);
    ~CMDDialog() override;

private:
    void setupUi();

    std::shared_ptr<Star> _star;
};