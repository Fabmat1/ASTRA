#pragma once

#include "DetailPanel.h"
#include <limits>

class QScrollArea;
class CrossRefResolver;
class QFrame;
class QLabel;

class SummaryPanel : public DetailPanel {
    Q_OBJECT
  public:
    explicit SummaryPanel(const Context &ctx, QWidget *parent = nullptr);

    void refresh() override;
    void refreshTheme() override;

  private:
    void setupUi();
    void rebuild();

    QWidget *buildDashboard();
    QWidget *createNameHeader();
    QWidget *createMetricCardsRow();
    QWidget *createMetricCard(const QString &value, const QString &label,
                              const QString &subtitle,
                              const QColor  &accentColor);
    QWidget *createPropertiesSection();
    QWidget *createOrbitalFitSection();
    QWidget *createCompanionSection(); // ← was createCompanionMassBanner
    QWidget *createDataInventorySection();
    QWidget *createReferencesSection();
    QFrame  *createSectionFrame(const QString &title, QWidget *content);
    QFrame  *createExpandableSectionFrame(const QString &title,
                                          QWidget       *compactContent,
                                          QWidget       *expandedContent);

    // ── Companion-mass derivation (cached, analytical error propagation) ──
    struct MassResult {
        double value = std::numeric_limits<double>::quiet_NaN();
        double error = std::numeric_limits<double>::quiet_NaN();
        bool   valid() const { return std::isfinite(value) && value > 0.0; }
    };
    struct MassInputs {
        double P = 0, eP = 0, K = 0, eK = 0;
        double M1 = 0, eM1 = 0, e = 0, ee = 0;
        double sini = 0, esini = 0;
        bool   hasIncl = false;
        bool   valid   = false;
        bool   sameAs(const MassInputs &o) const noexcept;
    };
    void ensureCompanionMasses();

    MassInputs _cachedMassInputs;
    MassResult _cachedMassMin;
    MassResult _cachedMassTrue;
    bool       _hasMassCache = false;

    QColor logPColor(double logP) const;
    QColor deltaRVColor(double deltaRV) const;
    QColor specClassColor(const QString &specClass) const;
    QColor accentTextColor(const QColor &accent) const;

    QScrollArea      *_scroll      = nullptr;
    CrossRefResolver *_refResolver = nullptr;
};