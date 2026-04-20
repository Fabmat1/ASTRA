#pragma once

#include "DetailPanel.h"
#include <QMap>
#include <QVector>

class QPushButton;
class QVBoxLayout;
class QCustomPlot;
class QFrame;

class LCPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit LCPanel(const Context& ctx, QWidget* parent = nullptr);

    void refresh() override;
    void refreshTheme() override;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onToggleFolded();

private:
    void setupUi();
    void populate();
    void replotData();

    struct LCSeries {
        QString source;
        QString filter;
        QVector<double> px, py, pe;
    };

    QPushButton* _toggleButton  = nullptr;
    QWidget*     _content       = nullptr;
    QVBoxLayout* _contentLayout = nullptr;
    bool         _folded        = false;

    QCustomPlot*    _plot = nullptr;
    QList<LCSeries> _seriesCache;
    QFrame*         _burgerMenu = nullptr;

    QMap<QString, int>  _binsUnfolded;
    QMap<QString, int>  _binsFolded;
    QMap<QString, bool> _normalize;
    QMap<QString, bool> _binEnabled;
};