#include "StarDetailView.h"
#include "models/Star.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QGroupBox>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QChart>
#include <QtCharts/QValueAxis>

StarDetailView::StarDetailView(std::shared_ptr<Star> star, QWidget *parent)
    : QWidget(parent)
    , m_star(star)
{
    setupUi();
    loadStarData();
}

StarDetailView::~StarDetailView()
{
}

void StarDetailView::setupUi()
{
    setWindowTitle("Star Details");
    resize(1200, 800);

    QHBoxLayout* mainLayout = new QHBoxLayout(this);

    // Left side layout
    QVBoxLayout* leftLayout = new QVBoxLayout;

    // RV curve plot
    QGroupBox* rvGroup = new QGroupBox("Radial Velocity Curve");
    QVBoxLayout* rvLayout = new QVBoxLayout(rvGroup);
    m_rvPlot = new QChartView;
    m_rvPlot->setMinimumHeight(300);
    rvLayout->addWidget(m_rvPlot);
    leftLayout->addWidget(rvGroup);

    // Bibcodes and info table
    QHBoxLayout* infoLayout = new QHBoxLayout;

    // Bibcodes list
    QGroupBox* bibGroup = new QGroupBox("Bibcodes");
    QVBoxLayout* bibLayout = new QVBoxLayout(bibGroup);
    m_bibcodesList = new QListWidget;
    bibLayout->addWidget(m_bibcodesList);
    infoLayout->addWidget(bibGroup);

    // Info table
    QGroupBox* infoGroup = new QGroupBox("Star Information");
    QVBoxLayout* infoGroupLayout = new QVBoxLayout(infoGroup);
    m_infoTable = new QTableWidget;
    m_infoTable->setColumnCount(2);
    m_infoTable->setHorizontalHeaderLabels({"Property", "Value"});
    m_infoTable->horizontalHeader()->setStretchLastSection(true);
    infoGroupLayout->addWidget(m_infoTable);
    infoLayout->addWidget(infoGroup);

    leftLayout->addLayout(infoLayout);
    mainLayout->addLayout(leftLayout, 2);

    // Right side layout
    QVBoxLayout* rightLayout = new QVBoxLayout;

    // Observability plot
    QGroupBox* obsGroup = new QGroupBox("Observability");
    QVBoxLayout* obsLayout = new QVBoxLayout(obsGroup);
    m_observabilityPlot = new QChartView;
    m_observabilityPlot->setMinimumHeight(250);
    obsLayout->addWidget(m_observabilityPlot);
    rightLayout->addWidget(obsGroup);

    // Spectra plot
    QGroupBox* specGroup = new QGroupBox("Spectra");
    QVBoxLayout* specLayout = new QVBoxLayout(specGroup);
    m_spectraPlot = new QChartView;
    m_spectraPlot->setMinimumHeight(250);
    specLayout->addWidget(m_spectraPlot);
    rightLayout->addWidget(specGroup);

    // Action buttons
    QGroupBox* actionsGroup = new QGroupBox("Actions");
    QVBoxLayout* actionsLayout = new QVBoxLayout(actionsGroup);

    m_simbadButton = new QPushButton("Show in SIMBAD");
    connect(m_simbadButton, &QPushButton::clicked, this, &StarDetailView::onShowInSimbad);
    actionsLayout->addWidget(m_simbadButton);

    m_spectraButton = new QPushButton("View Spectra");
    connect(m_spectraButton, &QPushButton::clicked, this, &StarDetailView::onViewSpectra);
    actionsLayout->addWidget(m_spectraButton);

    m_sedButton = new QPushButton("View SED");
    connect(m_sedButton, &QPushButton::clicked, this, &StarDetailView::onViewSED);
    actionsLayout->addWidget(m_sedButton);

    m_rvFitButton = new QPushButton("Fit RV Curve");
    connect(m_rvFitButton, &QPushButton::clicked, this, &StarDetailView::onFitRVCurve);
    actionsLayout->addWidget(m_rvFitButton);

    m_cmdButton = new QPushButton("View CMD");
    connect(m_cmdButton, &QPushButton::clicked, this, &StarDetailView::onViewCMD);
    actionsLayout->addWidget(m_cmdButton);

    rightLayout->addWidget(actionsGroup);
    rightLayout->addStretch();

    mainLayout->addLayout(rightLayout, 1);
}

void StarDetailView::loadStarData()
{
    if (!m_star) return;

    // Load bibcodes
    for (const auto& bibcode : m_star->getBibcodes()) {
        m_bibcodesList->addItem(bibcode);
    }
    connect(m_bibcodesList, &QListWidget::itemDoubleClicked,
            [](QListWidgetItem* item) {
                QString url = QString("https://ui.adsabs.harvard.edu/abs/%1/abstract")
                                .arg(item->text());
                QDesktopServices::openUrl(QUrl(url));
            });

    // Load info table
    int row = 0;
    auto addRow = [this, &row](const QString& property, const QString& value) {
        m_infoTable->insertRow(row);
        m_infoTable->setItem(row, 0, new QTableWidgetItem(property));
        m_infoTable->setItem(row, 1, new QTableWidgetItem(value));
        row++;
    };

    addRow("Name", m_star->getAlias());
    addRow("Source ID", m_star->getSourceId());
    addRow("RA", QString::number(m_star->getRa()));
    addRow("DEC", QString::number(m_star->getDec()));
    addRow("Spectral Class", m_star->getSpecClass());
    addRow("Teff", QString::number(m_star->getTeff()));
    addRow("log g", QString::number(m_star->getLogg()));
    addRow("G mag", QString::number(m_star->getGmag()));
    addRow("BP-RP", QString::number(m_star->getBpRp()));

    // Create plots
    createRVPlot();
    createObservabilityPlot();
    createSpectraPlot();
}

void StarDetailView::createRVPlot()
{
    // TODO: Implement actual RV curve plotting
    // For now, create placeholder chart
    QChart* chart = new QChart;
    chart->setTitle("Radial Velocity vs BJD");

    QLineSeries* series = new QLineSeries;
    series->append(0, 0);
    series->append(1, 1);
    chart->addSeries(series);

    chart->createDefaultAxes();
    m_rvPlot->setChart(chart);
}

void StarDetailView::createObservabilityPlot()
{
    // TODO: Implement observability plot
    QChart* chart = new QChart;
    chart->setTitle("Observability");
    m_observabilityPlot->setChart(chart);
}

void StarDetailView::createSpectraPlot()
{
    // TODO: Implement spectra stacking plot
    QChart* chart = new QChart;
    chart->setTitle("Stacked Spectra");
    m_spectraPlot->setChart(chart);
}

void StarDetailView::onShowInSimbad()
{
    if (!m_star) return;

    QString url = QString("https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+%1&submit=submit+id")
                    .arg(m_star->getSourceId());
    QDesktopServices::openUrl(QUrl(url));
}

void StarDetailView::onViewSpectra()
{
    // TODO: Implement spectra viewer
    QMessageBox::information(this, "View Spectra", "Spectra viewer to be implemented");
}

void StarDetailView::onViewSED()
{
    // TODO: Implement SED viewer
    QMessageBox::information(this, "View SED", "SED viewer to be implemented");
}

void StarDetailView::onFitRVCurve()
{
    // TODO: Implement RV curve fitting
    QMessageBox::information(this, "Fit RV Curve", "RV curve fitting to be implemented");
}

void StarDetailView::onViewCMD()
{
    // TODO: Implement CMD viewer
    QMessageBox::information(this, "View CMD", "CMD viewer to be implemented");
}