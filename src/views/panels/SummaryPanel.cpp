#include "SummaryPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "utils/CrossRefResolver.h"
#include "utils/AppPaths.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QTimer>
#include <QDesktopServices>
#include <QUrl>
#include <QSet>
#include <QMap>

#include <algorithm>
#include <cmath>

namespace {

class CopyEventFilter : public QObject
{
public:
    CopyEventFilter(const QString& text, QWidget* target, QObject* parent = nullptr)
        : QObject(parent), _text(text), _target(target) {}

protected:
    bool eventFilter(QObject*, QEvent* ev) override
    {
        if (ev->type() == QEvent::MouseButtonPress) {
            QApplication::clipboard()->setText(_text);
            auto* popup = new QLabel("\xe2\x9c\x93 Copied");
            popup->setAttribute(Qt::WA_DeleteOnClose);
            popup->setAttribute(Qt::WA_ShowWithoutActivating);
            popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
            popup->setStyleSheet(
                "QLabel { background: #4CAF50; color: white; font-weight: bold;"
                " padding: 4px 12px; border-radius: 4px; font-size: 12px; }");
            popup->adjustSize();
            popup->move(QCursor::pos() + QPoint(12, 12));
            popup->show();
            QTimer::singleShot(1000, popup, &QLabel::close);
            return true;
        }
        return false;
    }

private:
    QString  _text;
    QWidget* _target;
};

void makeCopyable(QLabel* label, const QString& textToCopy)
{
    label->setCursor(Qt::PointingHandCursor);
    label->installEventFilter(new CopyEventFilter(textToCopy, label, label));
}

QString bibcodeDisplayJournal(const QString& rawAbbrev)
{
    static const QMap<QString, QString> map = {
        {"Natur", "Nature"}, {"NatAs", "Nat. Astron."},
        {"Sci..", "Science"},
    };
    auto it = map.find(rawAbbrev);
    if (it != map.end()) return *it;
    QString c = rawAbbrev;
    c.remove('.');
    return c.trimmed();
}

QString formatBibcodeMeta(const QString& bib)
{
    if (bib.length() != 19) return QString();
    int year = bib.left(4).toInt();
    QString journal = bibcodeDisplayJournal(bib.mid(4, 5));
    QString volume = bib.mid(9, 4);
    volume.remove('.');
    volume = volume.trimmed();
    QChar section = bib.at(13);
    QString page = bib.mid(14, 4);
    page.remove('.');
    page = page.trimmed();
    if (section.isLetter()) page = QString(section) + page;

    QStringList parts;
    if (year > 0) parts << QString::number(year);
    if (!journal.isEmpty()) parts << journal;
    if (!volume.isEmpty()) {
        if (!page.isEmpty())
            parts << (volume + ", " + page);
        else
            parts << volume;
    }
    return parts.join(", ");
}

} // anonymous namespace

SummaryPanel::SummaryPanel(const Context& ctx, QWidget* parent)
    : DetailPanel(ctx, parent)
{
    setupUi();
    rebuild();
}

void SummaryPanel::setupUi()
{
    auto* box = new QGroupBox("Summary", this);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(box);

    auto* bl = new QVBoxLayout(box);
    _scroll = new QScrollArea;
    _scroll->setWidgetResizable(true);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bl->addWidget(_scroll);

    _refResolver = new CrossRefResolver(AppPaths::database(), this);
}

void SummaryPanel::rebuild()      { _scroll->setWidget(buildDashboard()); }
void SummaryPanel::refresh()      { rebuild(); }
void SummaryPanel::refreshTheme() { rebuild(); }

QWidget* SummaryPanel::buildDashboard()
{
    QWidget* container = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(10);

    layout->addWidget(createNameHeader());
    layout->addWidget(createMetricCardsRow());
    layout->addWidget(createPropertiesSection());

    // Orbital fit — only if we have one
    auto rvCurve = _ctx.star->getRVCurve();
    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();
    if (bestFit && bestFit->getPeriod() > 0)
        layout->addWidget(createOrbitalFitSection());

    layout->addWidget(createDataInventorySection());

    auto bibcodes = _ctx.star->getBibcodes();
    if (!bibcodes.empty())
        layout->addWidget(createReferencesSection());

    layout->addStretch();
    return container;
}

QWidget* SummaryPanel::createNameHeader()
{
    bool dark = PanelUtils::isDarkTheme();

    QWidget* header = new QWidget;
    QHBoxLayout* hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(4, 0, 4, 0);
    hLayout->setSpacing(12);

    // Left side: name + source ID
    QVBoxLayout* nameCol = new QVBoxLayout;
    nameCol->setSpacing(2);

    QString displayName = _ctx.star->getAlias().isEmpty() ? _ctx.star->getSourceId() : _ctx.star->getAlias();

    QLabel* nameLabel = new QLabel(displayName);
    nameLabel->setStyleSheet(QString("font-size: 20px; font-weight: 700; color: %1; background: transparent;")
        .arg(dark ? "white" : "#1a1a1a"));
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameCol->addWidget(nameLabel);

    // Gaia source ID line
    QString subText;
    if (!_ctx.star->getAlias().isEmpty() && !_ctx.star->getSourceId().isEmpty())
        subText = QString("Gaia DR3 %1").arg(_ctx.star->getSourceId());
    if (!_ctx.star->getTic().isEmpty()) {
        if (!subText.isEmpty()) subText += "  ·  ";
        subText += QString("TIC %1").arg(_ctx.star->getTic());
    }
    if (!_ctx.star->getJName().isEmpty()) {
        if (!subText.isEmpty()) subText += "  ·  ";
        subText += _ctx.star->getJName();
    }

    if (!subText.isEmpty()) {
        QLabel* subLabel = new QLabel(subText);
        subLabel->setStyleSheet(QString("font-size: 12px; color: %1; background: transparent;")
            .arg(dark ? "#999" : "#666"));
        subLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        nameCol->addWidget(subLabel);
    }

    hLayout->addLayout(nameCol, 1);

    // Right side: spectral class badge (if available)
    QString specClass = _ctx.star->getSpecClass();
    if (!specClass.isEmpty()) {
        QColor badgeColor = specClassColor(specClass);
        QColor badgeText = accentTextColor(badgeColor);

        QLabel* badge = new QLabel(specClass);
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedHeight(30);
        badge->setMinimumWidth(60);
        badge->setStyleSheet(QString(
            "font-size: 13px; font-weight: 700; color: %1; "
            "background: %2; border-radius: 6px; "
            "padding: 2px 12px; border: none;"
        ).arg(badgeText.name(), badgeColor.name()));
        hLayout->addWidget(badge, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    return header;
}

QWidget* SummaryPanel::createMetricCardsRow()
{
    QWidget* row = new QWidget;
    QHBoxLayout* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto has = [](double v) { return !std::isnan(v) && v != 0.0; };

    // ── log(p)
    {
        double logP = 0.0;
        int nSpectra = 0;
        QString subtitle;

        auto rvCurve = _ctx.star->getRVCurve();
        if (rvCurve && rvCurve->getNumPoints() >= 2) {
            logP = rvCurve->getLogP();
            nSpectra = static_cast<int>(rvCurve->getNumPoints());
            subtitle = QString("from %1 points").arg(nSpectra);
        } else if (has(_ctx.star->getLogP())) {
            logP = _ctx.star->getLogP();
            auto spectra = _ctx.star->getSpectra();
            nSpectra = static_cast<int>(spectra.size());
            if (nSpectra > 0)
                subtitle = QString("from %1 spectra").arg(nSpectra);
        }

        QString value = has(logP) ? QString::number(logP, 'f', 2) : "—";
        QColor accent = logPColor(logP);
        layout->addWidget(createMetricCard(value, "log(p)", subtitle, accent));
    }

    // ── ΔRV_max
    {
        double drv = 0.0;
        QString subtitle;

        auto rvCurve = _ctx.star->getRVCurve();
        if (rvCurve && rvCurve->getNumPoints() >= 2) {
            drv = rvCurve->getRVAmplitude();
        } else if (has(_ctx.star->getDeltaRV())) {
            drv = _ctx.star->getDeltaRV();
        }

        QString value;
        if (has(drv)) {
            value = QString::number(drv, 'f', 1);
            subtitle = "km/s";
            if (has(_ctx.star->getEDeltaRV()))
                subtitle = QString("± %1 km/s").arg(_ctx.star->getEDeltaRV(), 0, 'f', 1);
        } else {
            value = "—";
        }

        QColor accent = deltaRVColor(drv);
        layout->addWidget(createMetricCard(value, "ΔRV_max", subtitle, accent));
    }

    // ── N spectra
    {
        auto spectra = _ctx.star->getSpectra();
        int n = static_cast<int>(spectra.size());

        // Count instruments
        QSet<QString> instruments;
        for (auto& sp : spectra)
            if (!sp->getInstrument().isEmpty())
                instruments.insert(sp->getInstrument());

        QString subtitle;
        if (instruments.size() == 1)
            subtitle = *instruments.begin();
        else if (instruments.size() > 1)
            subtitle = QString("%1 instruments").arg(instruments.size());

        bool dark = PanelUtils::isDarkTheme();
        QColor accent = (n > 0) ? QColor(86, 156, 214) :
                         (dark ? QColor(100, 100, 100) : QColor(180, 180, 180));

        layout->addWidget(createMetricCard(
            QString::number(n), "Spectra", subtitle, accent));
    }

    // ── N RV points
    {
        auto rvCurve = _ctx.star->getRVCurve();
        int n = rvCurve ? static_cast<int>(rvCurve->getNumPoints()) : 0;

        QString subtitle;
        if (rvCurve && n > 0) {
            double span = rvCurve->getTimeSpan();
            if (span > 0)
                subtitle = QString("%1 d span").arg(span, 0, 'f', 0);
        }

        bool dark = PanelUtils::isDarkTheme();
        QColor accent = (n > 0) ? QColor(86, 180, 120) :
                         (dark ? QColor(100, 100, 100) : QColor(180, 180, 180));

        layout->addWidget(createMetricCard(
            QString::number(n), "RV Points", subtitle, accent));
    }

    return row;
}

QWidget* SummaryPanel::createMetricCard(const QString& value, const QString& label,
                                           const QString& subtitle, const QColor& accentColor)
{
    bool dark = PanelUtils::isDarkTheme();
    QColor cardBg   = dark ? QColor(50, 50, 55)   : QColor(248, 248, 250);
    QColor border   = dark ? QColor(70, 70, 75)    : QColor(210, 210, 215);
    QColor labelCol = dark ? QColor(160, 160, 165) : QColor(110, 110, 115);
    QColor subCol   = dark ? QColor(130, 130, 135) : QColor(140, 140, 145);

    QFrame* card = new QFrame;
    card->setObjectName("metricCard");
    card->setStyleSheet(QString(
        "QFrame#metricCard { background: %1; border: 1px solid %2; "
        "border-left: 4px solid %3; border-radius: 6px; }"
    ).arg(cardBg.name(), border.name(), accentColor.name()));

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(2);

    QLabel* valueLabel = new QLabel(value);
    valueLabel->setStyleSheet(QString(
        "font-size: 22px; font-weight: 700; color: %1; border: none; background: transparent;"
    ).arg(accentColor.name()));
    valueLabel->setAlignment(Qt::AlignLeft);
    if (value != "—")
        makeCopyable(valueLabel, value);
    layout->addWidget(valueLabel);

    QLabel* labelWidget = new QLabel(label);
    labelWidget->setStyleSheet(QString(
        "font-size: 11px; font-weight: 600; color: %1; border: none; background: transparent;"
    ).arg(labelCol.name()));
    if (value != "—")
        makeCopyable(labelWidget, value);
    layout->addWidget(labelWidget);

    if (!subtitle.isEmpty()) {
        QLabel* subLabel = new QLabel(subtitle);
        subLabel->setStyleSheet(QString(
            "font-size: 10px; color: %1; border: none; background: transparent;"
        ).arg(subCol.name()));
        layout->addWidget(subLabel);
    }

    layout->addStretch();
    card->setMinimumWidth(100);
    card->setMinimumHeight(80);
    return card;
}

QWidget* SummaryPanel::createPropertiesSection()
{
    auto has = [](double v) { return !std::isnan(v) && v != 0.0; };

    struct ValResult { QString display; QString copy; };
    auto valErr = [](double val, double err, int prec, const QString& unit = "") -> ValResult {
        QString num = QString::number(val, 'f', prec);
        QString s = num;
        if (!std::isnan(err) && err > 0.0)
            s += QString(" ± %1").arg(err, 0, 'f', prec);
        if (!unit.isEmpty())
            s += " " + unit;
        return {s, num};
    };

    bool dark = PanelUtils::isDarkTheme();
    QColor valCol   = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    QColor labelCol = dark ? QColor(140, 140, 145) : QColor(100, 100, 105);

    struct PropRow { QString label; QString value; QString copyValue; };
    std::vector<PropRow> astroRows, photoRows, atmosRows;

    // Astrometry
    if (has(_ctx.star->getRa()) && has(_ctx.star->getDec())) {
        QString raNum  = QString::number(_ctx.star->getRa(), 'f', 6);
        QString decNum = QString::number(_ctx.star->getDec(), 'f', 6);
        astroRows.push_back({"RA",  raNum + "°", raNum});
        astroRows.push_back({"Dec", decNum + "°", decNum});
    }
    if (has(_ctx.star->getPlx())) {
        auto [d, c] = valErr(_ctx.star->getPlx(), _ctx.star->getEPlx(), 3, "mas");
        astroRows.push_back({"Parallax", d, c});
    }
    if (has(_ctx.star->getPmra())) {
        auto [d, c] = valErr(_ctx.star->getPmra(), _ctx.star->getEPmra(), 3, "mas/yr");
        astroRows.push_back({"μ_RA", d, c});
    }
    if (has(_ctx.star->getPmdec())) {
        auto [d, c] = valErr(_ctx.star->getPmdec(), _ctx.star->getEPmdec(), 3, "mas/yr");
        astroRows.push_back({"μ_Dec", d, c});
    }

    // Photometry
    if (has(_ctx.star->getGmag())) {
        auto [d, c] = valErr(_ctx.star->getGmag(), _ctx.star->getEGmag(), 3, "mag");
        photoRows.push_back({"G", d, c});
    }
    if (has(_ctx.star->getBpRp())) {
        QString num = QString::number(_ctx.star->getBpRp(), 'f', 3);
        photoRows.push_back({"BP−RP", num + " mag", num});
    }
    if (has(_ctx.star->getBp())) {
        auto [d, c] = valErr(_ctx.star->getBp(), _ctx.star->getEBp(), 3, "mag");
        photoRows.push_back({"BP", d, c});
    }
    if (has(_ctx.star->getRp())) {
        auto [d, c] = valErr(_ctx.star->getRp(), _ctx.star->getERp(), 3, "mag");
        photoRows.push_back({"RP", d, c});
    }

    // Atmospheric
    if (has(_ctx.star->getTeff())) {
        auto [d, c] = valErr(_ctx.star->getTeff(), _ctx.star->getETeff(), 0, "K");
        atmosRows.push_back({"T_eff", d, c});
    }
    if (has(_ctx.star->getLogg())) {
        auto [d, c] = valErr(_ctx.star->getLogg(), _ctx.star->getELogg(), 2, "dex");
        atmosRows.push_back({"log g", d, c});
    }
    if (has(_ctx.star->getHe())) {
        auto [d, c] = valErr(_ctx.star->getHe(), _ctx.star->getEHe(), 2, "");
        atmosRows.push_back({"log(He/H)", d, c});
    }

    // RV summary (if no orbital fit)
    auto rvCurve = _ctx.star->getRVCurve();
    std::shared_ptr<RVFit> bestFit;
    if (rvCurve) bestFit = rvCurve->getBestFit();

    std::vector<PropRow> rvRows;
    if (!(bestFit && bestFit->getPeriod() > 0)) {
        if (has(_ctx.star->getRVMed())) {
            auto [d, c] = valErr(_ctx.star->getRVMed(), _ctx.star->getERVMed(), 2, "km/s");
            rvRows.push_back({"RV_med", d, c});
        } else if (has(_ctx.star->getRVAvg())) {
            auto [d, c] = valErr(_ctx.star->getRVAvg(), _ctx.star->getERVAvg(), 2, "km/s");
            rvRows.push_back({"RV_avg", d, c});
        }

        if (rvCurve && rvCurve->getNumPoints() > 0) {
            double minRV = rvCurve->getMinRV();
            double maxRV = rvCurve->getMaxRV();
            if (has(minRV) && has(maxRV)) {
                double mid = minRV + (maxRV - minRV) / 2.0;
                QString num = QString::number(mid, 'f', 2);
                rvRows.push_back({"RV_mid", num + " km/s", num});
            }
        }
    }

    // Build grid widget with click-to-copy
    auto buildGrid = [&](const std::vector<PropRow>& rows) -> QWidget* {
        if (rows.empty()) return nullptr;
        QWidget* grid = new QWidget;
        QGridLayout* gl = new QGridLayout(grid);
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setHorizontalSpacing(16);
        gl->setVerticalSpacing(4);

        int maxPerCol = static_cast<int>((rows.size() + 1) / 2);

        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            int col = (i < maxPerCol) ? 0 : 2;
            int row = (i < maxPerCol) ? i : i - maxPerCol;

            QString copyText = rows[i].copyValue.isEmpty()
                                   ? rows[i].value : rows[i].copyValue;

            QLabel* lbl = new QLabel(rows[i].label);
            lbl->setStyleSheet(QString(
                "font-size: 11px; font-weight: 600; color: %1; background: transparent; border: none;"
            ).arg(labelCol.name()));
            makeCopyable(lbl, copyText);

            QLabel* val = new QLabel(rows[i].value);
            val->setStyleSheet(QString(
                "font-size: 12px; color: %1; background: transparent; border: none;"
            ).arg(valCol.name()));
            makeCopyable(val, copyText);

            gl->addWidget(lbl, row, col);
            gl->addWidget(val, row, col + 1);
        }

        gl->setColumnStretch(1, 1);
        if (rows.size() > static_cast<size_t>(maxPerCol))
            gl->setColumnStretch(3, 1);

        return grid;
    };

    // Build combined properties widget
    QWidget* container = new QWidget;
    QVBoxLayout* vLayout = new QVBoxLayout(container);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(6);

    auto addSubSection = [&](const QString& title, const std::vector<PropRow>& rows) {
        if (rows.empty()) return;
        QWidget* grid = buildGrid(rows);
        if (grid)
            vLayout->addWidget(createSectionFrame(title, grid));
    };

    addSubSection("Astrometry", astroRows);
    addSubSection("Photometry", photoRows);
    addSubSection("Atmospheric Parameters", atmosRows);
    addSubSection("Radial Velocity", rvRows);

    if (vLayout->count() == 0) {
        QLabel* empty = new QLabel("No catalog data available yet.");
        empty->setStyleSheet("color: gray; font-style: italic; background: transparent;");
        vLayout->addWidget(empty);
    }

    return container;
}

QWidget* SummaryPanel::createOrbitalFitSection()
{
    bool dark = PanelUtils::isDarkTheme();
    QColor valCol   = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    QColor labelCol = dark ? QColor(140, 140, 145) : QColor(100, 100, 105);

    struct ValResult { QString display; QString copy; };
    auto valErr = [](double val, double err, int prec, const QString& unit = "") -> ValResult {
        QString num = QString::number(val, 'f', prec);
        QString s = num;
        if (!std::isnan(err) && err > 0.0)
            s += QString(" ± %1").arg(err, 0, 'f', prec);
        if (!unit.isEmpty())
            s += " " + unit;
        return {s, num};
    };
    auto has = [](double v) { return !std::isnan(v) && v != 0.0; };

    auto rvCurve = _ctx.star->getRVCurve();
    auto bestFit = rvCurve->getBestFit();

    struct Row { QString label; QString value; QString copyValue; };
    std::vector<Row> rows;

    {
        auto [d, c] = valErr(bestFit->getPeriod(), bestFit->getPeriodError(), 6, "d");
        rows.push_back({"Period", d, c});
    }
    {
        auto [d, c] = valErr(bestFit->getK(), bestFit->getKError(), 2, "km/s");
        rows.push_back({"K", d, c});
    }
    {
        auto [d, c] = valErr(bestFit->getGamma(), bestFit->getGammaError(), 2, "km/s");
        rows.push_back({"γ", d, c});
    }
    {
        auto [d, c] = valErr(bestFit->getPhi(), bestFit->getPhiError(), 4, "");
        rows.push_back({"T₀ (ϕ)", d, c});
    }

    if (bestFit->isEccentric()) {
        auto [d1, c1] = valErr(bestFit->getEccentricity(), bestFit->getEccentricityError(), 4, "");
        rows.push_back({"e", d1, c1});
        auto [d2, c2] = valErr(bestFit->getOmega(), bestFit->getOmegaError(), 1, "°");
        rows.push_back({"ω", d2, c2});
    }

    if (has(bestFit->getRms())) {
        QString num = QString::number(bestFit->getRms(), 'f', 2);
        rows.push_back({"RMS", num + " km/s", num});
    }
    if (!bestFit->getFitMethod().isEmpty()) {
        QString m = bestFit->getFitMethod();
        rows.push_back({"Method", m, m});
    }

    // Build grid
    QWidget* grid = new QWidget;
    QGridLayout* gl = new QGridLayout(grid);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setHorizontalSpacing(16);
    gl->setVerticalSpacing(4);

    int maxPerCol = static_cast<int>((rows.size() + 1) / 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        int col = (i < maxPerCol) ? 0 : 2;
        int row = (i < maxPerCol) ? i : i - maxPerCol;

        QString copyText = rows[i].copyValue.isEmpty()
                               ? rows[i].value : rows[i].copyValue;

        QLabel* lbl = new QLabel(rows[i].label);
        lbl->setStyleSheet(QString(
            "font-size: 11px; font-weight: 600; color: %1; background: transparent; border: none;"
        ).arg(labelCol.name()));
        makeCopyable(lbl, copyText);

        QLabel* val = new QLabel(rows[i].value);
        val->setStyleSheet(QString(
            "font-size: 12px; color: %1; background: transparent; border: none;"
        ).arg(valCol.name()));
        makeCopyable(val, copyText);

        gl->addWidget(lbl, row, col);
        gl->addWidget(val, row, col + 1);
    }
    gl->setColumnStretch(1, 1);
    if (rows.size() > static_cast<size_t>(maxPerCol))
        gl->setColumnStretch(3, 1);

    return createSectionFrame("Orbital Solution", grid);
}

QWidget* SummaryPanel::createDataInventorySection()
{
    bool dark = PanelUtils::isDarkTheme();
    QColor tagBg     = dark ? QColor(60, 65, 70)   : QColor(230, 235, 240);
    QColor tagBorder = dark ? QColor(80, 85, 90)    : QColor(200, 205, 210);
    QColor tagText   = dark ? QColor(195, 200, 205) : QColor(50, 55, 60);
    QColor checkCol  = QColor(80, 180, 100);
    QColor crossCol  = dark ? QColor(100, 100, 105) : QColor(170, 170, 175);

    auto spectra = _ctx.star->getSpectra();
    auto rvCurve = _ctx.star->getRVCurve();
    auto phot    = _ctx.star->getPhotometry();

    struct Inventory {
        QString label;
        bool available;
        QString detail;
    };

    std::vector<Inventory> items;

    // Spectra
    {
        int n = static_cast<int>(spectra.size());
        int nFitted = 0;
        QSet<QString> instruments;
        for (auto& sp : spectra) {
            if (sp->getBestFit()) nFitted++;
            if (!sp->getInstrument().isEmpty()) instruments.insert(sp->getInstrument());
        }
        QString detail;
        if (n > 0) {
            QStringList parts;
            parts << QString("%1 total").arg(n);
            if (nFitted > 0) parts << QString("%1 fitted").arg(nFitted);
            if (!instruments.isEmpty()) {
                QStringList instList(instruments.begin(), instruments.end());
                instList.sort();
                parts << instList.join(", ");
            }
            detail = parts.join(" · ");
        }
        items.push_back({"Spectra", n > 0, detail});
    }

    // RV curve
    {
        int n = rvCurve ? static_cast<int>(rvCurve->getNumPoints()) : 0;
        int nFits = rvCurve ? static_cast<int>(rvCurve->getNumFits()) : 0;
        QString detail;
        if (n > 0) {
            QStringList parts;
            parts << QString("%1 points").arg(n);
            if (nFits > 0) parts << QString("%1 fit(s)").arg(nFits);
            if (rvCurve->getTimeSpan() > 0)
                parts << QString("%1 d span").arg(rvCurve->getTimeSpan(), 0, 'f', 0);
            detail = parts.join(" · ");
        }
        items.push_back({"RV Curve", n > 0, detail});
    }

    // Light curves
    {
        bool hasLC = false;
        QString detail;
        if (phot) {
            auto sources = phot->getLightcurveSources();
            if (!sources.empty()) {
                hasLC = true;
                QStringList srcList;
                for (auto& s : sources) srcList.append(s);
                detail = srcList.join(", ");
            }
        }
        items.push_back({"Light Curves", hasLC, detail});
    }

    // SED
    {
        bool hasSED = false;
        QString detail;
        if (phot) {
            auto sed = phot->getBestSEDModel();
            if (sed) {
                hasSED = true;
                QStringList parts;
                parts << QString("%1-comp").arg(sed->numComponents);

                auto fmtAsym = [](double val, double up, double down, int prec) -> QString {
                    return QString("%1<sup><small>+%2</small></sup><sub><small>-%3</small></sub>")
                        .arg(val,  0, 'f', prec)
                        .arg(up,   0, 'f', prec)
                        .arg(down, 0, 'f', prec);
                };

                // Show primary component Teff, radius, mass
                if (!sed->components.empty()) {
                    const auto& c1 = sed->components[0];
                    if (c1.teff > 0)
                        parts << QString("T₁=%1 K").arg(c1.teff, 0, 'f', 0);
                    if (c1.radius.value > 0)
                        parts << QString("R₁=%1 R☉").arg(fmtAsym(c1.radius.value, c1.radius.errUp, c1.radius.errDown, 3));
                    if (c1.mass.value > 0)
                        parts << QString("M₁=%1 M☉").arg(fmtAsym(c1.mass.value, c1.mass.errUp, c1.mass.errDown, 3));
                }
                // Show companion Teff, radius, mass if 2-component
                if (sed->numComponents >= 2 && sed->components.size() >= 2) {
                    const auto& c2 = sed->components[1];
                    if (c2.teff > 0)
                        parts << QString("T₂=%1 K").arg(c2.teff, 0, 'f', 0);
                    if (c2.radius.value > 0)
                        parts << QString("R₂=%1 R☉").arg(fmtAsym(c2.radius.value, c2.radius.errUp, c2.radius.errDown, 3));
                    if (c2.mass.value > 0)
                        parts << QString("M₂=%1 M☉").arg(fmtAsym(c2.mass.value, c2.mass.errUp, c2.mass.errDown, 3));
                }
                if (sed->distanceMode > 0)
                    parts << QString("d=%1 pc").arg(sed->distanceMode, 0, 'f', 0);
                if (sed->chi2Reduced > 0)
                    parts << QString("χ²=%1").arg(sed->chi2Reduced, 0, 'f', 2);
                detail = parts.join(" · ");
            }
        }
        items.push_back({"SED Fit", hasSED, detail});
    }

    // Build tag strip
    QWidget* content = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    for (auto& item : items) {
        QHBoxLayout* row = new QHBoxLayout;
        row->setSpacing(8);

        // Status indicator
        QLabel* indicator = new QLabel(item.available ? "●" : "○");
        indicator->setFixedWidth(16);
        indicator->setAlignment(Qt::AlignCenter);
        indicator->setStyleSheet(QString(
            "font-size: 12px; color: %1; background: transparent; border: none;"
        ).arg(item.available ? checkCol.name() : crossCol.name()));
        row->addWidget(indicator);

        // Label
        QLabel* lbl = new QLabel(item.label);
        lbl->setFixedWidth(120);
        lbl->setStyleSheet(QString(
            "font-size: 12px; font-weight: 600; color: %1; background: transparent; border: none;"
        ).arg(tagText.name()));
        row->addWidget(lbl);

        // Detail
        if (!item.detail.isEmpty()) {
            QLabel* det = new QLabel(item.detail);
            det->setTextFormat(Qt::RichText); 
            det->setStyleSheet(QString(
                "font-size: 11px; color: %1; background: transparent; border: none;"
            ).arg(dark ? "#aaa" : "#777"));
            det->setTextInteractionFlags(Qt::TextSelectableByMouse);
            row->addWidget(det, 1);
        } else {
            row->addStretch();
        }

        layout->addLayout(row);
    }

    return createSectionFrame("Data Inventory", content);
}

QWidget* SummaryPanel::createReferencesSection()
{
    bool dark = PanelUtils::isDarkTheme();
    QColor accentColor = dark ? QColor(100, 130, 200) : QColor(60, 100, 180);
    QColor cardBg      = dark ? QColor(48, 50, 56)    : QColor(250, 250, 252);
    QColor cardBorder  = dark ? QColor(65, 68, 75)     : QColor(215, 218, 225);
    QColor titleColor  = dark ? QColor(225, 228, 235)  : QColor(25, 25, 30);
    QColor subtitleCol = dark ? QColor(145, 150, 160)  : QColor(100, 105, 115);
    QColor bodyCol     = dark ? QColor(190, 195, 205)  : QColor(55, 60, 70);
    QColor abstractBg  = dark ? QColor(42, 44, 50)     : QColor(243, 244, 248);
    QColor linkColor   = dark ? QColor(120, 160, 230)  : QColor(40, 90, 180);
    QColor loadingCol  = dark ? QColor(110, 115, 125)  : QColor(150, 155, 165);

    QWidget* content = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto bibcodes = _ctx.star->getBibcodes();
    std::sort(bibcodes.begin(), bibcodes.end(),
              [](const QString& a, const QString& b) { return a > b; });

    QStringList toResolve;

    QString cardStyle = QString(
        "QFrame#refCard { background: %1; border: 1px solid %2; "
        "border-left: 3px solid %3; border-radius: 5px; }"
    ).arg(cardBg.name(), cardBorder.name(), accentColor.name());

    QString linkBtnStyle = QString(
        "QPushButton { font-size: 10px; color: %1; border: none; "
        "background: transparent; padding: 0 4px; }"
        "QPushButton:hover { color: %2; }"
    ).arg(linkColor.name(), titleColor.name());

    for (const auto& bib : bibcodes) {
        QString adsUrl  = QString("https://ui.adsabs.harvard.edu/abs/%1/abstract").arg(bib);
        QString metaStr = formatBibcodeMeta(bib);

        QFrame* card = new QFrame;
        card->setObjectName("refCard");
        card->setStyleSheet(cardStyle);
        card->setToolTip(bib);

        QVBoxLayout* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(3);

        // Title — initially shows bibcode, replaced when resolved
        QLabel* titleLabel = new QLabel(bib);
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setWordWrap(true);
        titleLabel->setStyleSheet(QString(
            "font-size: 12px; font-weight: 600; color: %1; "
            "background: transparent; border: none;"
        ).arg(titleColor.name()));
        cardLayout->addWidget(titleLabel);

        // Subtitle — authors + metadata
        QLabel* subtitleLabel = new QLabel(metaStr);
        subtitleLabel->setTextFormat(Qt::PlainText);
        subtitleLabel->setWordWrap(true);
        subtitleLabel->setStyleSheet(QString(
            "font-size: 10px; color: %1; background: transparent; border: none;"
        ).arg(subtitleCol.name()));
        cardLayout->addWidget(subtitleLabel);

        // Button row
        QWidget* btnRow = new QWidget;
        QHBoxLayout* btnLayout = new QHBoxLayout(btnRow);
        btnLayout->setContentsMargins(0, 2, 0, 0);
        btnLayout->setSpacing(8);

        QPushButton* abstractBtn = new QPushButton("▸ Abstract");
        abstractBtn->setFlat(true);
        abstractBtn->setFixedHeight(20);
        abstractBtn->setCursor(Qt::PointingHandCursor);
        abstractBtn->setStyleSheet(linkBtnStyle);
        abstractBtn->setVisible(false);
        btnLayout->addWidget(abstractBtn);

        QPushButton* adsScrapeBtn = new QPushButton("↻ Fetch from NASA/ADS");
        adsScrapeBtn->setFlat(true);
        adsScrapeBtn->setFixedHeight(20);
        adsScrapeBtn->setCursor(Qt::PointingHandCursor);
        adsScrapeBtn->setToolTip("CrossRef has no record. Click to scrape the ADS page once.");
        adsScrapeBtn->setStyleSheet(linkBtnStyle);
        adsScrapeBtn->setVisible(false);
        btnLayout->addWidget(adsScrapeBtn);

        btnLayout->addStretch();

        QPushButton* adsBtn = new QPushButton("Open on ADS ↗");
        adsBtn->setFlat(true);
        adsBtn->setFixedHeight(20);
        adsBtn->setCursor(Qt::PointingHandCursor);
        adsBtn->setToolTip(adsUrl);
        adsBtn->setStyleSheet(linkBtnStyle);
        connect(adsBtn, &QPushButton::clicked, this, [adsUrl]() {
            QDesktopServices::openUrl(QUrl(adsUrl));
        });
        btnLayout->addWidget(adsBtn);

        cardLayout->addWidget(btnRow);

        // Loading label
        QLabel* loadingLabel = new QLabel("Resolving…");
        loadingLabel->setTextFormat(Qt::PlainText);
        loadingLabel->setStyleSheet(QString(
            "font-size: 10px; font-style: italic; color: %1; "
            "background: transparent; border: none;"
        ).arg(loadingCol.name()));
        loadingLabel->setVisible(false);
        cardLayout->addWidget(loadingLabel);

        // Abstract area (hidden until toggled)
        QLabel* abstractLabel = new QLabel;
        abstractLabel->setTextFormat(Qt::PlainText);
        abstractLabel->setWordWrap(true);
        abstractLabel->setStyleSheet(QString(
            "font-size: 11px; color: %1; background: %2; "
            "border: none; border-radius: 3px; padding: 6px 8px;"
        ).arg(bodyCol.name(), abstractBg.name()));
        abstractLabel->setVisible(false);
        cardLayout->addWidget(abstractLabel);

        connect(abstractBtn, &QPushButton::clicked, this,
                [abstractBtn, abstractLabel]() {
            bool show = !abstractLabel->isVisible();
            abstractLabel->setVisible(show);
            abstractBtn->setText(show ? "▾ Abstract" : "▸ Abstract");
        });

        layout->addWidget(card);

        // Populate callback
        auto populateCard = [titleLabel, subtitleLabel, loadingLabel,
                             abstractLabel, abstractBtn,
                             titleColor, subtitleCol, metaStr]
                            (const BibcodeInfo& info) {
            titleLabel->setText(info.title);
            titleLabel->setStyleSheet(QString(
                "font-size: 12px; font-weight: 600; color: %1; "
                "background: transparent; border: none;"
            ).arg(titleColor.name()));
            titleLabel->setCursor(Qt::PointingHandCursor);
            makeCopyable(titleLabel, info.title);

            QString meta;
            if (!info.authors.isEmpty())
                meta = info.authors + "  ·  " + metaStr;
            else
                meta = metaStr;
            subtitleLabel->setText(meta);

            if (!info.abstract.isEmpty()) {
                abstractLabel->setText(info.abstract);
                abstractBtn->setVisible(true);
            }

            loadingLabel->setVisible(false);
        };

        BibcodeInfo cached = _refResolver->lookupCache(bib);
        if (!cached.title.isEmpty()) {
            populateCard(cached);
        } else {
            loadingLabel->setVisible(true);
            toResolve << bib;

            connect(_refResolver, &CrossRefResolver::resolved,
                    card, [bib, populateCard, adsScrapeBtn]
                        (const QString& resolvedBib, const BibcodeInfo& info) {
                if (resolvedBib != bib) return;
                adsScrapeBtn->setVisible(false);
                populateCard(info);
            });

            connect(_refResolver, &CrossRefResolver::fetchFailed,
                    card, [bib, loadingLabel, adsScrapeBtn]
                        (const QString& failedBib) {
                if (failedBib != bib) return;
                loadingLabel->setVisible(false);
                loadingLabel->setText("Resolving…");
                adsScrapeBtn->setVisible(true);
                adsScrapeBtn->setEnabled(true);
            });

            connect(adsScrapeBtn, &QPushButton::clicked, this,
                    [this, bib, loadingLabel, adsScrapeBtn]() {
                adsScrapeBtn->setEnabled(false);
                adsScrapeBtn->setVisible(false);
                loadingLabel->setText("Fetching from NASA/ADS…");
                loadingLabel->setVisible(true);
                _refResolver->resolveViaADS(bib);
            });
        }
    }

    if (!toResolve.isEmpty())
        _refResolver->resolve(toResolve);

    return createSectionFrame("References", content);
}

QFrame* SummaryPanel::createSectionFrame(const QString& title, QWidget* content)
{
    bool dark = PanelUtils::isDarkTheme();
    QColor cardBg  = dark ? QColor(50, 50, 55) : QColor(248, 248, 250);
    QColor border  = dark ? QColor(70, 70, 75)  : QColor(210, 210, 215);
    QColor titleCol = dark ? QColor(180, 180, 185) : QColor(90, 90, 95);

    QFrame* frame = new QFrame;
    frame->setFrameShape(QFrame::NoFrame);
    frame->setStyleSheet(QString(
        "QFrame#sectionCard { background: %1; border: 1px solid %2; border-radius: 6px; }"
    ).arg(cardBg.name(), border.name()));
    frame->setObjectName("sectionCard");

    QVBoxLayout* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    if (!title.isEmpty()) {
        QLabel* titleLabel = new QLabel(title);
        titleLabel->setStyleSheet(QString(
            "font-size: 11px; font-weight: 600; color: %1; "
            "text-transform: uppercase; letter-spacing: 1px; "
            "padding-bottom: 4px; border: none; background: transparent;"
        ).arg(titleCol.name()));
        layout->addWidget(titleLabel);
    }

    layout->addWidget(content);
    return frame;
}

QColor SummaryPanel::logPColor(double logP) const
{
    // Very negative = highly variable = important
    if (std::isnan(logP) || logP == 0.0)
        return PanelUtils::isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (logP < -10.0)
        return QColor(220, 50, 50);    // Red — extremely significant
    if (logP < -5.0)
        return QColor(230, 150, 30);   // Orange — significant
    if (logP < -2.0)
        return QColor(200, 200, 50);   // Yellow — marginal
    return QColor(80, 180, 80);        // Green — consistent with constant
}

QColor SummaryPanel::deltaRVColor(double deltaRV) const
{
    if (std::isnan(deltaRV) || deltaRV == 0.0)
        return PanelUtils::isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (deltaRV > 100.0)
        return QColor(220, 50, 50);
    if (deltaRV > 30.0)
        return QColor(230, 150, 30);
    if (deltaRV > 10.0)
        return QColor(200, 200, 50);
    return QColor(80, 180, 80);
}

QColor SummaryPanel::specClassColor(const QString& specClass) const
{
    if (specClass.isEmpty()) return PanelUtils::isDarkTheme() ? QColor(140, 140, 140) : QColor(120, 120, 120);
    QChar first = specClass.at(0).toUpper();
    if (first == 'O') return QColor(100, 140, 255);
    if (first == 'B') return QColor(130, 170, 255);
    if (first == 'A') return QColor(180, 200, 255);
    if (first == 'F') return QColor(255, 255, 200);
    if (first == 'G') return QColor(255, 230, 140);
    if (first == 'K') return QColor(255, 180, 100);
    if (first == 'M') return QColor(255, 120, 80);
    // Subdwarf / white dwarf prefixes
    if (specClass.startsWith("sd", Qt::CaseInsensitive))
        return QColor(130, 170, 255);
    return PanelUtils::isDarkTheme() ? QColor(170, 170, 170) : QColor(100, 100, 100);
}

QColor SummaryPanel::accentTextColor(const QColor& accent) const
{
    // Return white or black text depending on accent luminance
    return (accent.lightnessF() > 0.55) ? QColor(20, 20, 20) : QColor(240, 240, 240);
}