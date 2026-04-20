#pragma once

#include "DetailPanel.h"

class QScrollArea;
class CrossRefResolver;
class QFrame;
class QLabel;

class SummaryPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit SummaryPanel(const Context& ctx, QWidget* parent = nullptr);

    void refresh() override;
    void refreshTheme() override;

private:
    void setupUi();
    void rebuild();

    // All moved from StarDetailView verbatim:
    QWidget* buildDashboard();
    QWidget* createNameHeader();
    QWidget* createMetricCardsRow();
    QWidget* createMetricCard(const QString& value, const QString& label,
                              const QString& subtitle, const QColor& accentColor);
    QWidget* createPropertiesSection();
    QWidget* createOrbitalFitSection();
    QWidget* createDataInventorySection();
    QWidget* createReferencesSection();
    QFrame*  createSectionFrame(const QString& title, QWidget* content);

    QColor logPColor(double logP) const;
    QColor deltaRVColor(double deltaRV) const;
    QColor specClassColor(const QString& specClass) const;
    QColor accentTextColor(const QColor& accent) const;

    QScrollArea*      _scroll    = nullptr;
    CrossRefResolver* _refResolver = nullptr;
};