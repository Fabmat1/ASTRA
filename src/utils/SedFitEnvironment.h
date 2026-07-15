#pragma once

#include <QProcessEnvironment>
#include <QString>

/// Locates the bundled/installed `sedfit` executable (SEDplusplus) and its
/// filter reference data, and prepares the process environment to run it.
/// Resolution order mirrors the lcurve helper binaries: user setting >
/// next-to-app (AppImage) > installed libexec bundle > dev build tree > PATH.
class SedFitEnvironment
{
public:
    /// Absolute path of the sedfit binary to run, or "" if none was found.
    static QString resolveBinary();

    /// Directory holding filter_passbands.fits[.gz] (and optionally
    /// zeropoint_offsets.txt), or "" if not found.
    static QString refdataDir();

    /// True when `binaryPath` is the copy shipped with ASTRA (as opposed to a
    /// user-provided one).
    static bool isBundled(const QString& binaryPath);

    /// Environment for launching `binaryPath`. For the bundled copy the
    /// AppImage lib dir is prepended to LD_LIBRARY_PATH; a user-provided
    /// binary gets the unmodified system environment.
    static QProcessEnvironment environmentFor(const QString& binaryPath);
};
