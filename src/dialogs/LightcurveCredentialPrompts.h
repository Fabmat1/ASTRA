#pragma once

#include <QString>
#include <QStringList>

class QWidget;
class AppSettings;

/**
 * Pre-flight credential checks for lightcurve fetching.
 *
 * ZTF queries go through ztfquery, which reads an IRSA login from
 * ~/.ztfquery; without it the child process would block on an interactive
 * prompt it can never answer. ATLAS forced photometry needs the API token
 * from Settings (passed as ATLASFORCED_SECRET_KEY).
 *
 * ensureCredentials() checks the requested sources and pops up a login
 * prompt for anything missing. Declined sources are removed from the list
 * so the fetch continues without them; callers should uncheck the matching
 * source checkboxes for the returned names.
 */
namespace LightcurveCredentialPrompts
{
    /// True if ~/.ztfquery holds an [irsa] username/password pair.
    bool hasZtfIrsaLogin();

    /// Write the IRSA login to ~/.ztfquery in ztfquery's own format
    /// (INI section, password base64-encoded as a python bytes literal).
    /// Preserves any other sections already in the file.
    bool saveZtfIrsaLogin(const QString& username, const QString& password);

    /// Prompt for missing ZTF / ATLAS credentials needed by `sources`.
    /// Sources whose prompt was declined (or whose credentials could not be
    /// saved) are removed from `sources`. Returns the removed source names.
    QStringList ensureCredentials(QWidget*     parent,
                                  QStringList& sources,
                                  AppSettings* settings);
}
