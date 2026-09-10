#pragma once

#include <QString>

// Small shared confirmation popup used by every copy-on-click surface, so the
// feedback looks the same wherever a value is copied from.
namespace CopyToast {

/// Put `text` on the clipboard and flash a confirmation near the cursor.
/// `note` names what was copied, e.g. "value" -> "✓ Copied value".
void copy(const QString &text, const QString &note = QString());

/// Flash the confirmation without touching the clipboard.
void flash(const QString &note = QString());

} // namespace CopyToast
