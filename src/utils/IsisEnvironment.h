#pragma once

#include <QProcessEnvironment>
#include <QString>

/**
 * Resolves and configures the ISIS spectral-fitting backend, including the
 * copy that may be bundled inside the AppImage.
 *
 * ISIS (S-Lang based) only *reads* its installation tree, so the bundled copy
 * can run in place from the read-only AppImage mount - no unpacking needed.
 * What it does require is a set of environment variables pointing at that tree
 * (which lives at a different absolute path every launch) plus a `.isisrc`
 * that puts the isisscripts / stellar_isisscripts directories on the ISIS load
 * path. We compute those at runtime from applicationDirPath() and write a
 * private `.isisrc` into a per-user HOME so the user's real ~/.isisrc and their
 * own ISIS install are never touched.
 *
 * All members are static and side-effect free except environmentFor(), which
 * (re)writes the private `.isisrc` when the bundled ISIS is in use.
 */
class IsisEnvironment
{
public:
    /// Root of the bundled ISIS tree (e.g. <AppImage>/usr/share/astra/isis),
    /// resolved relative to the ASTRA executable. Empty if not bundled.
    static QString bundleRoot();
    static bool    bundleAvailable();

    /// Absolute path to the bundled isis binary (placed next to the ASTRA
    /// executable by the AppImage build). Empty if not present / runnable.
    static QString bundledBinary();

    /// True if `binaryPath` is the bundled isis (next to ASTRA or under the
    /// bundle root) and therefore needs the bundled environment applied.
    static bool isBundled(const QString& binaryPath);

    /// Resolve the isis binary to use: an explicitly configured path wins,
    /// otherwise the bundled copy, otherwise PATH. Empty if none found.
    static QString resolveBinary();

    /// Process environment to launch `binaryPath` with. For the bundled isis
    /// this augments the system environment with ISIS_SRCDIR / LD_LIBRARY_PATH /
    /// SLSH_PATH / SLANG_MODULE_PATH / HOME and ensures the private `.isisrc`.
    /// For any other (user-provided) binary it returns the unmodified system
    /// environment, preserving the previous behaviour exactly.
    static QProcessEnvironment environmentFor(const QString& binaryPath);

private:
    static QString privateHome();           // <AppData>/isis-home
    static void    writeIsisrc(const QString& root);
};
