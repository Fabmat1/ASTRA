#pragma once
#include <QDialog>
#include <memory>

class Star;
class DatabaseManager;
class RadialVelocityPoint;

class QDoubleSpinBox;
class QComboBox;
class QDialogButtonBox;

class RVAddPointDialog : public QDialog
{
    Q_OBJECT
public:
    RVAddPointDialog(std::shared_ptr<Star> star,
                     DatabaseManager* dbm,
                     QWidget* parent = nullptr);

    std::shared_ptr<RadialVelocityPoint> result() const { return _result; }

private slots:
    void onAccept();

private:
    std::shared_ptr<Star>                  _star;
    DatabaseManager*                        _dbm = nullptr;
    std::shared_ptr<RadialVelocityPoint>    _result;

    QDoubleSpinBox*   _mjdSpin   = nullptr;
    QDoubleSpinBox*   _rvSpin    = nullptr;
    QDoubleSpinBox*   _errFormal = nullptr;
    QDoubleSpinBox*   _errSyst   = nullptr;
    QComboBox*        _instCombo = nullptr;
    QDialogButtonBox* _buttons   = nullptr;
};