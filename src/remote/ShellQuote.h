#pragma once

#include <QString>
#include <QStringList>

namespace astra::remote {

// True when the argument passes through every POSIX shell (and tcsh, which
// executes the command line of every `ssh host <cmd>`) without any quoting:
// letters, digits, and  _ - + = / . , : @ % ^  only, non-empty.
bool isPlainArg(const QString& arg);

// POSIX single-quote escaping: wraps in '...' with embedded ' as '\''.
// Only safe INSIDE an already-running `sh` (e.g. a script fed via `sh -s`),
// never on an ssh command line where the remote login shell might be tcsh.
QString shQuote(const QString& arg);

// shQuote applied to every element, space-joined.
QString shJoin(const QStringList& args);

} // namespace astra::remote
