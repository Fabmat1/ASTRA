// src/utils/RVPhotometryPages.cpp

#include "../importWizard/RVPhotometryPages.h"
#include "../importWizard/StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/RadialVelocity.h"
#include "../db/DatabaseManager.h"
#include "../utils/Logger.h"
#include "../utils/BackgroundTaskManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QButtonGroup>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTextStream>
#include <QApplication>
#include <QRegularExpression>
#include <QHeaderView>
#include <QScrollArea>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <algorithm>
#include <numeric>

// ════════════════════════════════════════════════════════════════
// PhotometryImportPage — keep existing implementation unchanged
// ════════════════════════════════════════════════════════════════

PhotometryImportPage::PhotometryImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Photometry");
    setSubTitle("Import photometric light curves (future feature)");

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel(
        "Photometry import is not yet implemented. Click Finish to complete.");
    label->setWordWrap(true);
    layout->addWidget(label);
}

int PhotometryImportPage::nextId() const
{
    return -1;
}