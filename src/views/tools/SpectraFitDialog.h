#pragma once

#include <QDialog>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class DatabaseManager;
class QTabBar;
class QCustomPlot;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;

class SpectraFitDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SpectraFitDialog(std::shared_ptr<Star> star,
                              DatabaseManager* dbm = nullptr,
                              QWidget* parent = nullptr);
    ~SpectraFitDialog() override;

private slots:
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    void onTreeItemSelected();
    void onTabChanged(int index);

private:
    void setupUi();
    void rebuildTree();
    void refreshTreeStyling();
    void displaySpectrum(int index);
    QString formatSpectrumTabLabel(const std::shared_ptr<Spectrum>& s, int i) const;
    QString formatSpectrumInfo(const std::shared_ptr<Spectrum>& s) const;

    // Keep tree and tab selection in sync
    void syncTreeToCurrentSpectrum();

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;

    // Sorted spectra (mirrors the tab order)
    std::vector<std::shared_ptr<Spectrum>> _spectra;

    // UI
    QSplitter*    _splitter     = nullptr;
    QTabBar*      _tabBar       = nullptr;
    QCustomPlot*  _plot         = nullptr;
    QLabel*       _infoLabel    = nullptr;
    QTreeWidget*  _tree         = nullptr;

    int _currentSpectrumIndex = -1;
    bool _updatingTree = false;   // reentrancy guard
};