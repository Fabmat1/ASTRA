#pragma once

#include <QDialog>
#include <QString>
#include <memory>

class Spectrum;
class SpectralFit;
class DatabaseManager;
class QVBoxLayout;

// ─────────────────────────────────────────────────────────────────────────────
// Everything ASTRA stores about one spectrum, or about one of its spectral
// fits, laid out as labelled property grids.
//
// The plots elsewhere show the few parameters that fit on a toolbar; this is
// the full record - every column of the spectra / spectral_fits tables that
// carries information, the archive provenance blob, the solver bookkeeping and
// both components' element abundances. Values are rendered with QuantityLabel,
// so they stack their asymmetric error sides and copy in the configured
// notation exactly like the summary panel's property rows do.
//
// The dialog is a snapshot: it reads the objects once and does not follow later
// edits, which is what "show me what is stored" means. Open it again after a
// re-fit.
// ─────────────────────────────────────────────────────────────────────────────
class SpectrumDetailsDialog : public QDialog
{
    Q_OBJECT
public:
    /// Non-modal window with the spectrum's metadata. `dbm` may be null; it is
    /// only used to resolve the instrument/mode the spectrum is attributed to.
    static void showSpectrum(const std::shared_ptr<Spectrum>& spec,
                             DatabaseManager* dbm, QWidget* parent);

    /// Non-modal window with every fitted parameter of `fit`. `spec` is the
    /// spectrum it belongs to and only names it in the header.
    static void showFit(const std::shared_ptr<Spectrum>& spec,
                        const std::shared_ptr<SpectralFit>& fit,
                        QWidget* parent);

private:
    SpectrumDetailsDialog(const QString& title, const QString& subtitle,
                          QWidget* parent);

    /// Body layout new sections are appended to.
    QVBoxLayout* _body = nullptr;
    /// Plain-text dump built alongside the widgets, for the "Copy all" button.
    QString _plainDump;

    void buildSpectrum(const std::shared_ptr<Spectrum>& spec,
                       DatabaseManager* dbm);
    void buildFit(const std::shared_ptr<SpectralFit>& fit);
    void finish(const QString& header);
};
