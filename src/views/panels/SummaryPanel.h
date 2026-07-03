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
    explicit SummaryPanel(const Context &ctx, QWidget *parent = nullptr,
                          bool deferPopulate = false);

    void refresh() override;
    void refreshTheme() override;

  private:
    void     addReferenceCards(QWidget *host, const QStringList &bibcodes);
    void     appendReferenceBatch(); // build the next page now
    void     onSummaryScrolled();    // scroll-to-bottom trigger
    QWidget *makeLoadingRow();       // the spinner row

    static constexpr int kRefBatchSize = 10;

    void setupUi();
    void populate() override { rebuild(); }
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

    // ── Companion-mass derivation (cached error propagation) ──
    // Symmetric inputs go through the analytical (linearised) path; as soon
    // as any input carries an asymmetric interval the errors come from
    // split-normal Monte-Carlo resampling instead (errUp/errDown set).
    struct MassResult {
        double value = std::numeric_limits<double>::quiet_NaN();
        double error = std::numeric_limits<double>::quiet_NaN();
        double errUp   = std::numeric_limits<double>::quiet_NaN();
        double errDown = std::numeric_limits<double>::quiet_NaN();
        bool   valid() const { return std::isfinite(value) && value > 0.0; }
    };
    struct MassInputs {
        double P = 0, eP = 0, K = 0, eK = 0;
        double M1 = 0, eM1 = 0, e = 0, ee = 0;
        double sini = 0, esini = 0;
        double q = 0, eQ = 0;
        // Asymmetric 1σ intervals (NaN = unset → symmetric error applies).
        // The inclination is carried in degrees for the MC resampling.
        static constexpr double kUnset = std::numeric_limits<double>::quiet_NaN();
        double ePUp = kUnset, ePDown = kUnset;
        double eKUp = kUnset, eKDown = kUnset;
        double eM1Up = kUnset, eM1Down = kUnset;
        double eeUp = kUnset, eeDown = kUnset;
        double eQUp = kUnset, eQDown = kUnset;
        double iDeg = 0, eIDeg = 0;
        double eIDegUp = kUnset, eIDegDown = kUnset;
        bool   hasIncl = false;
        bool   hasQ    = false;
        bool   valid   = false;
        bool   sameAs(const MassInputs &o) const noexcept;
    };
    void ensureCompanionMasses();

    MassInputs _cachedMassInputs;
    MassResult _cachedMassMin;
    MassResult _cachedMassTrue;
    // Set when the photometric mass ratio q cannot be reconciled with the RV
    // mass function (it would require sin i > 1). Drives a warning in the UI.
    bool       _cachedMassTrueInconsistent = false;
    bool       _hasMassCache = false;

    QColor logPColor(double logP) const;
    QColor deltaRVColor(double deltaRV) const;
    QColor specClassColor(const QString &specClass) const;
    QColor accentTextColor(const QColor &accent) const;

    QScrollArea      *_scroll      = nullptr;
    CrossRefResolver *_refResolver = nullptr;

    void buildReferenceCards(QWidget *host, const QStringList &bibcodes);
    bool _builtDark = false; // theme the current widget tree was built with

    QWidget    *_refCardHost = nullptr; // cards go here
    QWidget    *_refSpinner  = nullptr; // busy row, shown while more remain
    QStringList _pendingRefs;           // not-yet-shown bibcodes
    bool        _loadingMoreRefs = false;

    QWidget *createSpecClassBadge(); // editable badge
    void     commitSpecClass(const QString &newClass);

    bool _specEditing = false;
};