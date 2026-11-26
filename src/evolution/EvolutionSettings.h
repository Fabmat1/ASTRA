#pragma once

// Settings of evolutionary track fitting: this machine's tracks folder.
//
// Read from and written to QSettings on every call, never cached: AppSettings::save()
// rewrites every key it knows from an instance's cached copy, so a long-lived copy of
// these values would silently revert a newer one (see the user fit defaults).  The
// keys live outside AppSettings' group for the same reason.
//
// The tracks folder is per machine: track sets are often unpublished and are never
// bundled, so every installation points at its own copy.  Nothing here is part of the
// database or of ASTRA's built-in sync.

#include <QObject>
#include <QString>

namespace astra::evolution {

class EvolutionSettings : public QObject {
    Q_OBJECT
public:
    /// The process-wide instance (lives on the GUI thread; the getters may be called
    /// from any thread, they only read QSettings).
    static EvolutionSettings& instance();

    /// Folder with the track sets (*.sdtracks.fits.gz, Han FITS); empty = none.
    static QString tracksFolder();
    void setTracksFolder(const QString& folder);

signals:
    void tracksFolderChanged(const QString& folder);

private:
    EvolutionSettings() = default;
};

}  // namespace astra::evolution
