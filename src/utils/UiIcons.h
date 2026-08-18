#ifndef UIICONS_H
#define UIICONS_H

#include <QIcon>

class QAbstractButton;

// Themed, semantic UI icons.
//
// Direction indicators used to be literal unicode triangles baked into button
// text ("▾ Advanced Options", "◀  Previous", ...). That spread four different
// glyph pairs across one concept, overloaded "▶" for three unrelated meanings,
// left the glyphs unable to follow the theme's arrow colour, and baked
// direction into translatable strings so RTL locales could not mirror it.
//
// Instead, ask for a *role* here. Roles map onto the monochrome CURRENTCOLOR
// SVG templates in :/icons/ that ThemeManager already recolours for the QSS
// subcontrols, so these icons match the combo-box, spin-box, header and tree
// branch arrows the active theme draws.
namespace UiIcons {

enum class Role {
    // Expand/collapse a section. Deliberately shares the tree-branch art so
    // inline disclosure toggles match QTreeView's branch indicators.
    DisclosureCollapsed,
    DisclosureExpanded,

    // Reorder the selected row within a list.
    ReorderUp,
    ReorderDown,

    // Step through a sequence (previous/next page). Mirrored under RTL.
    NavigatePrev,
    NavigateNext,

    // Start a fit/job, and the "currently running" state marker. A distinct,
    // larger triangle so "run" never reads as "next".
    Run,

    // Move an entry between two lists. Mirrored under RTL.
    TransferAdd,
    TransferRemove,
    TransferAddAll,
    TransferRemoveAll,

    // Exchange two things (e.g. swap plot axes).
    Swap,
};

// Themed icon for `role`, rendered at `px` logical pixels.
// Results are cached per (role, colour, size); the cache is cleared by refresh().
QIcon icon(Role role, int px = 16);

// Set `role`'s icon on `button` and keep it correct across theme changes.
// Call again with a different role to change state (e.g. collapsed -> expanded);
// the button stays registered either way.
void apply(QAbstractButton* button, Role role, int px = 16);

// As apply(), but places the icon on the button's *trailing* edge, for the
// "Next >" shape where the arrow follows the label. Qt only ever lays an icon
// out on the leading edge, so this mirrors the button's own layout direction;
// combined with the RTL-aware roles the arrow still points along the reading
// direction and still trails the text under RTL.
void applyTrailing(QAbstractButton* button, Role role, int px = 16);

// Drop the icon cache and re-apply icons to every button passed to apply().
// ThemeManager calls this after a new stylesheet is installed.
void refresh();

} // namespace UiIcons

#endif // UIICONS_H
