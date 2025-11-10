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
    , _star(star)
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
    _rvPlot = new QChartView;
    _rvPlot->setMinimumHeight(300);
    rvLayout->addWidget(_rvPlot);
    leftLayout->addWidget(rvGroup);

    // Bibcodes and info table
    QHBoxLayout* infoLayout = new QHBoxLayout;

    // Bibcodes list
    QGroupBox* bibGroup = new QGroupBox("Bibcodes");
    QVBoxLayout* bibLayout = new QVBoxLayout(bibGroup);
    _bibcodesList = new QListWidget;
    bibLayout->addWidget(_bibcodesList);
    infoLayout->addWidget(bibGroup);

    // Info table
    QGroupBox* infoGroup = new QGroupBox("Star Information");
    QVBoxLayout* infoGroupLayout = new QVBoxLayout(infoGroup);
    _infoTable = new QTableWidget;
    _infoTable->setColumnCount(2);
    _infoTable->setHorizontalHeaderLabels({"Property", "Value"});
    _infoTable->horizontalHeader()->setStretchLastSection(true);
    infoGroupLayout->addWidget(_infoTable);
    infoLayout->addWidget(infoGroup);

    leftLayout->addLayout(infoLayout);
    mainLayout->addLayout(leftLayout, 2);

    // Right side layout
    QVBoxLayout* rightLayout = new QVBoxLayout;

    // Observability plot
    QGroupBox* obsGroup = new QGroupBox("Observability");
    QVBoxLayout* obsLayout = new QVBoxLayout(obsGroup);
    _observabilityPlot = new QChartView;
    _observabilityPlot->setMinimumHeight(250);
    obsLayout->addWidget(_observabilityPlot);
    rightLayout->addWidget(obsGroup);

    // Spectra plot
    QGroupBox* specGroup = new QGroupBox("Spectra");
    QVBoxLayout* specLayout = new QVBoxLayout(specGroup);
    _spectraPlot = new QChartView;
    _spectraPlot->setMinimumHeight(250);
    specLayout->addWidget(_spectraPlot);
    rightLayout->addWidget(specGroup);

    // Action buttons
    QGroupBox* actionsGroup = new QGroupBox("Actions");
    QVBoxLayout* actionsLayout = new QVBoxLayout(actionsGroup);

    _simbadButton = new QPushButton("Show in SIMBAD");
    connect(_simbadButton, &QPushButton::clicked, this, &StarDetailView::onShowInSimbad);
    actionsLayout->addWidget(_simbadButton);

    _spectraButton = new QPushButton("View Spectra");
    connect(_spectraButton, &QPushButton::clicked, this, &StarDetailView::onViewSpectra);
    actionsLayout->addWidget(_spectraButton);

    _sedButton = new QPushButton("View SED");
    connect(_sedButton, &QPushButton::clicked, this, &StarDetailView::onViewSED);
    actionsLayout->addWidget(_sedButton);

    _rvFitButton = new QPushButton("Fit RV Curve");
    connect(_rvFitButton, &QPushButton::clicked, this, &StarDetailView::onFitRVCurve);
    actionsLayout->addWidget(_rvFitButton);

    _cmdButton = new QPushButton("View CMD");
    connect(_cmdButton, &QPushButton::clicked, this, &StarDetailView::onViewCMD);
    actionsLayout->addWidget(_cmdButton);

    rightLayout->addWidget(actionsGroup);
    rightLayout->addStretch();

    mainLayout->addLayout(rightLayout, 1);
}

void StarDetailView::loadStarData()
{
    if (!_star) return;

    // Load bibcodes
    for (const auto& bibcode : _star->getBibcodes()) {
        _bibcodesList->addItem(bibcode);
    }
    connect(_bibcodesList, &QListWidget::itemDoubleClicked,
            [](QListWidgetItem* item) {
                QString url = QString("https://ui.adsabs.harvard.edu/abs/%1/abstract")
                                .arg(item->text());
                QDesktopServices::openUrl(QUrl(url));
            });

    // Load info table
    int row = 0;
    auto addRow = [this, &row](const QString& property, const QString& value) {
        _infoTable->insertRow(row);
        _infoTable->setItem(row, 0, new QTableWidgetItem(property));
        _infoTable->setItem(row, 1, new QTableWidgetItem(value));
        row++;
    };

    addRow("Name", _star->getAlias());
    addRow("Source ID", _star->getSourceId());
    addRow("RA", QString::number(_star->getRa()));
    addRow("DEC", QString::number(_star->getDec()));
    addRow("Spectral Class", _star->getSpecClass());
    addRow("Teff", QString::number(_star->getTeff()));
    addRow("log g", QString::number(_star->getLogg()));
    addRow("G mag", QString::number(_star->getGmag()));
    addRow("BP-RP", QString::number(_star->getBpRp()));

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
    _rvPlot->setChart(chart);
}

void StarDetailView::createObservabilityPlot()
{
    // TODO: Implement observability plot
    QChart* chart = new QChart;
    chart->setTitle("Observability");
    _observabilityPlot->setChart(chart);
}

void StarDetailView::createSpectraPlot()
{
    // TODO: Implement spectra stacking plot
    QChart* chart = new QChart;
    chart->setTitle("Stacked Spectra");
    _spectraPlot->setChart(chart);
}

void StarDetailView::onShowInSimbad()
{
    if (!_star) return;

    QString url = QString("https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+%1&submit=submit+id")
                    .arg(_star->getSourceId());
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