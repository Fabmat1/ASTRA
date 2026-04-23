#pragma once

#include <QWidget>
#include <QStringList>
#include <QVector>

class QVBoxLayout;
class QLineEdit;

struct IsisMacroParam {
    QString label;
    QString defaultValue;
    int     widthPx = 70;
};

struct IsisMacroDef {
    QString                    label;
    QString                    tooltip;
    QVector<IsisMacroParam>    params;
    QStringList                commandTemplates;  // {0}, {1}, … substituted
};

class IsisMacroPanel : public QWidget
{
    Q_OBJECT
public:
    explicit IsisMacroPanel(QWidget* parent = nullptr);

signals:
    void runCommands(const QStringList& commands);

private:
    void addGroup(const QString& title);
    void addMacro(const IsisMacroDef& def);

    QVBoxLayout* _layout = nullptr;
};