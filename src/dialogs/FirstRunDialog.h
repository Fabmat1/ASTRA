#pragma once

#include <QDialog>

class AppSettings;
class QLineEdit;

/**
 * One-time welcome / onboarding dialog shown on the first launch of ASTRA.
 *
 * Prompts for the (optional) external-service tokens that aren't discoverable
 * automatically — the NASA/ADS API token and the ATLAS forced-photometry token
 * — so the literature-lookup and ATLAS lightcurve features work out of the box.
 * Both are optional and can be changed later in Settings. Whether or not the
 * user fills anything in, the dialog records that onboarding has run so it does
 * not reappear.
 */
class FirstRunDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FirstRunDialog(AppSettings* settings, QWidget* parent = nullptr);

    /// True if the onboarding dialog has not been shown before.
    static bool shouldShow();
    /// Record that onboarding has run (so it won't be shown again).
    static void markShown();

private:
    void saveAndAccept();

    AppSettings* _settings   = nullptr;
    QLineEdit*   _adsEdit    = nullptr;
    QLineEdit*   _atlasEdit  = nullptr;
};
