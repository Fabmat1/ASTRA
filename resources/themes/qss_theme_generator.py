#!/usr/bin/env python3
"""
QSS Theme Generator - color-role remapping.

The template is `rose_pine_dawn.qss`, which is the only fully-themed stylesheet
(every widget/subcontrol styled + the `@icons` header). Every other theme is
produced by remapping the *real* Rosé Pine Dawn colors used in the rules onto a
target palette.

The dict keys below are the actual hex values that appear in rose_pine_dawn.qss
rules (verified with grep). Each palette maps every role to the target hex.

Substitution is done case-insensitively against the ORIGINAL string into a fresh
output (single regex pass, longest key first) so that an already-substituted
value can never be re-substituted by a later mapping.
"""

import re
import argparse
from pathlib import Path


# =============================================================================
# CANONICAL COLOR ROLES (keys = real Rosé Pine Dawn hexes used in the rules)
# =============================================================================
# Roles, in order:
#   base      #faf4ed  Base / window background
#   elevated  #fffaf3  Elevated / input background (one step lighter)
#   crust     #f2e9e1  Crust / disabled background (one step darker)
#   surface0  #f4ede8  Surface0 / hover & alternate-row background
#   surface1  #dfdad9  Surface1 / default border
#   surface2  #cecacd  Surface2 / strong border, pressed
#   overlay   #9893a5  Overlay / muted & disabled text
#   subtext   #797593  Subtext
#   muted     #6e6a86  Muted text variant
#   text      #575279  Primary text
#   text_hi   #3b3650  High-contrast / darkest text
#   accent    #286983  Accent
#   accent_h  #3b7c96  Accent hover
#   accent_p  #1f5870  Accent pressed
#   iris      #907aa9  Secondary accent (lavender)
#   love      #b4637a  Danger (red)
#   love_h    #c4738a  Red hover
#   love_p    #a4536a  Red pressed
#   gold      #ea9d34  Warning (yellow)
#   foam      #56949f  Success (green-teal)
#   sel       #d7e4ea  Selection background (accent-tinted)
#   sel_str   #cadbe3  Selection background, stronger

ROLE_ORDER = [
    "base", "elevated", "crust", "surface0", "surface1", "surface2",
    "overlay", "subtext", "muted", "text", "text_hi",
    "accent", "accent_h", "accent_p", "iris",
    "love", "love_h", "love_p", "gold", "foam",
    "sel", "sel_str",
]

SOURCE = {
    "base":     "#faf4ed",
    "elevated": "#fffaf3",
    "crust":    "#f2e9e1",
    "surface0": "#f4ede8",
    "surface1": "#dfdad9",
    "surface2": "#cecacd",
    "overlay":  "#9893a5",
    "subtext":  "#797593",
    "muted":    "#6e6a86",
    "text":     "#575279",
    "text_hi":  "#3b3650",
    "accent":   "#286983",
    "accent_h": "#3b7c96",
    "accent_p": "#1f5870",
    "iris":     "#907aa9",
    "love":     "#b4637a",
    "love_h":   "#c4738a",
    "love_p":   "#a4536a",
    "gold":     "#ea9d34",
    "foam":     "#56949f",
    "sel":      "#d7e4ea",
    "sel_str":  "#cadbe3",
}


def palette(name, display, is_dark, **roles):
    """Build a palette entry; every role must be supplied."""
    # Optional explicit icon overrides (default: check=base, arrow=text).
    check_override = roles.pop("_check", None)
    arrow_override = roles.pop("_arrow", None)
    missing = [r for r in ROLE_ORDER if r not in roles]
    if missing:
        raise ValueError(f"{name}: missing roles {missing}")
    extra = [r for r in roles if r not in ROLE_ORDER]
    if extra:
        raise ValueError(f"{name}: unknown roles {extra}")
    return {
        "display": display,
        "is_dark": is_dark,
        # icons default to base (check, on accent box) and text (arrow, on bg).
        "check": check_override or roles["base"],
        "arrow": arrow_override or roles["text"],
        "roles": {role: roles[role] for role in ROLE_ORDER},
    }


# =============================================================================
# LIGHT THEMES
# =============================================================================

CATPPUCCIN_LATTE = palette(
    "catppuccin_latte", "Catppuccin Latte", False,
    base="#eff1f5", elevated="#e6e9ef", crust="#dce0e8",
    surface0="#ccd0da", surface1="#bcc0cc", surface2="#acb0be",
    overlay="#9ca0b0", subtext="#6c6f85", muted="#5c5f77",
    text="#4c4f69", text_hi="#3a3c52",
    accent="#1e66f5", accent_h="#3a7af7", accent_p="#0b53d6",
    iris="#7287fd",
    love="#d20f39", love_h="#e02850", love_p="#b00b30",
    gold="#df8e1d", foam="#179299",
    sel="#cfe0fb", sel_str="#bcd4fa",
)

GITHUB_LIGHT = palette(
    "github_light", "GitHub Light", False,
    base="#ffffff", elevated="#f6f8fa", crust="#eaeef2",
    surface0="#f0f3f6", surface1="#d0d7de", surface2="#afb8c1",
    overlay="#8c959f", subtext="#57606a", muted="#6e7781",
    text="#1f2328", text_hi="#0b0f14",
    accent="#0969da", accent_h="#1f7ae8", accent_p="#0550ae",
    iris="#8250df",
    love="#cf222e", love_h="#e0414c", love_p="#a40e19",
    gold="#9a6700", foam="#1a7f37",
    sel="#cfe5ff", sel_str="#b6d7ff",
)

SOLARIZED_LIGHT = palette(
    "solarized_light", "Solarized Light", False,
    base="#fdf6e3", elevated="#fefbf0", crust="#eee8d5",
    surface0="#f3eddb", surface1="#e3dcc4", surface2="#d4ccb0",
    overlay="#93a1a1", subtext="#657b83", muted="#586e75",
    text="#586e75", text_hi="#073642",
    accent="#268bd2", accent_h="#3a9ce0", accent_p="#1a6fb0",
    iris="#6c71c4",
    love="#dc322f", love_h="#e84a47", love_p="#bc2522",
    gold="#b58900", foam="#2aa198",
    sel="#cfe6f5", sel_str="#b6d9ef",
)

GRUVBOX_LIGHT = palette(
    "gruvbox_light", "Gruvbox Light", False,
    base="#fbf1c7", elevated="#fcf6da", crust="#f2e5bc",
    surface0="#f4ecc8", surface1="#ebdbb2", surface2="#d5c4a1",
    overlay="#a89984", subtext="#665c54", muted="#7c6f64",
    text="#3c3836", text_hi="#282828",
    accent="#458588", accent_h="#5a9a9d", accent_p="#356a6c",
    iris="#8f3f71",
    love="#cc241d", love_h="#e03a33", love_p="#a91b16",
    gold="#d79921", foam="#689d6a",
    sel="#cfe5e0", sel_str="#b8d6cf",
)

NORD_LIGHT = palette(
    "nord_light", "Nord Light", False,
    base="#eceff4", elevated="#f4f6fa", crust="#e5e9f0",
    surface0="#e1e7f0", surface1="#d8dee9", surface2="#c2cbdb",
    overlay="#9aa5b8", subtext="#4c566a", muted="#5b667d",
    text="#3b4252", text_hi="#2e3440",
    accent="#5e81ac", accent_h="#7193bd", accent_p="#4c6f9a",
    iris="#b48ead",
    love="#bf616a", love_h="#cf737c", love_p="#a64f58",
    gold="#d08770", foam="#8fbcbb",
    sel="#d3e0f0", sel_str="#c0d2e8",
)

ONE_LIGHT = palette(
    "one_light", "One Light", False,
    base="#fafafa", elevated="#ffffff", crust="#f0f0f0",
    surface0="#f2f2f2", surface1="#e5e5e6", surface2="#c8c8c9",
    overlay="#a0a1a7", subtext="#696c77", muted="#8b8b8d",
    text="#383a42", text_hi="#202227",
    accent="#4078f2", accent_h="#588cf5", accent_p="#2a62d6",
    iris="#a626a4",
    love="#e45649", love_h="#ed6c60", love_p="#c63d31",
    gold="#c18401", foam="#50a14f",
    sel="#d7e4fd", sel_str="#c2d6fb",
)

# =============================================================================
# DARK THEMES
# =============================================================================
# Background roles map to DARK colors, text roles to LIGHT colors.
# Ordering preserved: base < elevated (elevated is slightly lighter/elevated),
# crust slightly darker; surface1/surface2 are progressively lighter borders.
# text_hi is the highest-contrast (lightest) text.

CATPPUCCIN_MOCHA = palette(
    "catppuccin_mocha", "Catppuccin Mocha", True,
    base="#1e1e2e", elevated="#181825", crust="#11111b",
    surface0="#313244", surface1="#45475a", surface2="#585b70",
    overlay="#6c7086", subtext="#a6adc8", muted="#9399b2",
    text="#cdd6f4", text_hi="#f5f5fa",
    accent="#89b4fa", accent_h="#9cc0fb", accent_p="#6fa0f0",
    iris="#b4befe",
    love="#f38ba8", love_h="#f59cb5", love_p="#e07594",
    gold="#f9e2af", foam="#94e2d5",
    sel="#3a4a6b", sel_str="#4a5d83",
    _check="#1e1e2e", _arrow="#cdd6f4",
)

DRACULA = palette(
    "dracula", "Dracula", True,
    base="#282a36", elevated="#2e303e", crust="#21222c",
    surface0="#343746", surface1="#44475a", surface2="#565a73",
    overlay="#6272a4", subtext="#bcc2de", muted="#9aa3cf",
    text="#f8f8f2", text_hi="#ffffff",
    accent="#bd93f9", accent_h="#cba6fb", accent_p="#a87ef0",
    iris="#ff79c6",
    love="#ff5555", love_h="#ff6e6e", love_p="#e03e3e",
    gold="#f1fa8c", foam="#50fa7b",
    sel="#44475a", sel_str="#565a8c",
    _check="#282a36", _arrow="#f8f8f2",
)

NORD = palette(
    "nord", "Nord", True,
    base="#2e3440", elevated="#343c4a", crust="#272c36",
    surface0="#3b4252", surface1="#434c5e", surface2="#4c566a",
    overlay="#6b7689", subtext="#d8dee9", muted="#aab2c0",
    text="#e5e9f0", text_hi="#eceff4",
    accent="#88c0d0", accent_h="#99cdda", accent_p="#6fa8ba",
    iris="#b48ead",
    love="#bf616a", love_h="#cf737c", love_p="#a64f58",
    gold="#ebcb8b", foam="#a3be8c",
    sel="#3b4a5a", sel_str="#48586c",
    _check="#2e3440", _arrow="#e5e9f0",
)

GRUVBOX_DARK = palette(
    "gruvbox_dark", "Gruvbox Dark", True,
    base="#282828", elevated="#32302f", crust="#1d2021",
    surface0="#3c3836", surface1="#504945", surface2="#665c54",
    overlay="#928374", subtext="#d5c4a1", muted="#a89984",
    text="#ebdbb2", text_hi="#fbf1c7",
    accent="#83a598", accent_h="#94b4a7", accent_p="#6d8e82",
    iris="#d3869b",
    love="#fb4934", love_h="#fc5f4c", love_p="#e03a26",
    gold="#fabd2f", foam="#b8bb26",
    sel="#374b46", sel_str="#445d57",
    _check="#282828", _arrow="#ebdbb2",
)

TOKYO_NIGHT = palette(
    "tokyo_night", "Tokyo Night", True,
    base="#1a1b26", elevated="#1f2335", crust="#16161e",
    surface0="#24283b", surface1="#2f334d", surface2="#414868",
    overlay="#565f89", subtext="#a9b1d6", muted="#787c99",
    text="#c0caf5", text_hi="#d5dcff",
    accent="#7aa2f7", accent_h="#8db3f9", accent_p="#5e8ae8",
    iris="#bb9af7",
    love="#f7768e", love_h="#f98aa0", love_p="#e05e78",
    gold="#e0af68", foam="#9ece6a",
    sel="#2c3457", sel_str="#384270",
    _check="#1a1b26", _arrow="#c0caf5",
)

SOLARIZED_DARK = palette(
    "solarized_dark", "Solarized Dark", True,
    base="#002b36", elevated="#073642", crust="#00212b",
    surface0="#0a3d49", surface1="#0f4956", surface2="#1a5a68",
    overlay="#586e75", subtext="#93a1a1", muted="#657b83",
    text="#839496", text_hi="#eee8d5",
    accent="#268bd2", accent_h="#3a9ce0", accent_p="#1a6fb0",
    iris="#6c71c4",
    love="#dc322f", love_h="#e84a47", love_p="#bc2522",
    gold="#b58900", foam="#2aa198",
    sel="#0d4a5a", sel_str="#155d70",
    _check="#002b36", _arrow="#839496",
)

ONE_DARK = palette(
    "one_dark", "One Dark", True,
    base="#282c34", elevated="#2f343f", crust="#21252b",
    surface0="#31363f", surface1="#3b4048", surface2="#4b5263",
    overlay="#5c6370", subtext="#abb2bf", muted="#828997",
    text="#abb2bf", text_hi="#dcdfe4",
    accent="#61afef", accent_h="#76bcf2", accent_p="#4a98da",
    iris="#c678dd",
    love="#e06c75", love_h="#e87f88", love_p="#c85660",
    gold="#e5c07b", foam="#98c379",
    sel="#3a4a5e", sel_str="#465a72",
    _check="#282c34", _arrow="#abb2bf",
)


# =============================================================================
# PALETTE REGISTRY
# =============================================================================

PALETTES = {
    # Light
    "catppuccin_latte": CATPPUCCIN_LATTE,
    "github_light":     GITHUB_LIGHT,
    "solarized_light":  SOLARIZED_LIGHT,
    "gruvbox_light":    GRUVBOX_LIGHT,
    "nord_light":       NORD_LIGHT,
    "one_light":        ONE_LIGHT,
    # Dark
    "catppuccin_mocha": CATPPUCCIN_MOCHA,
    "dracula":          DRACULA,
    "nord":             NORD,
    "gruvbox_dark":     GRUVBOX_DARK,
    "tokyo_night":      TOKYO_NIGHT,
    "solarized_dark":   SOLARIZED_DARK,
    "one_dark":         ONE_DARK,
}


def replace_colors(qss_content: str, color_map: dict) -> str:
    """Replace all source hexes with target hexes in a single pass.

    A single alternation regex is matched against the ORIGINAL string and each
    match is substituted from the map. Because we never feed an output value
    back through the matcher, an already-substituted target hex can never be
    re-substituted by a later mapping (no double-substitution / chaining).
    Matching is case-insensitive; longest keys are listed first defensively.
    """
    # All source keys are distinct 7-char hexes, but sort longest-first anyway.
    keys = sorted(color_map.keys(), key=len, reverse=True)
    pattern = re.compile("|".join(re.escape(k) for k in keys), re.IGNORECASE)

    # Lower-cased lookup so case-insensitive matches resolve correctly.
    lut = {k.lower(): v for k, v in color_map.items()}

    def _sub(m):
        return lut[m.group(0).lower()]

    return pattern.sub(_sub, qss_content)


def update_header(qss_content: str, theme_display: str, icons: dict) -> str:
    """Rewrite the leading title comment and the @icons block."""
    result = re.sub(
        r'ROSE PINE DAWN THEME',
        f'{theme_display.upper()} THEME',
        qss_content,
        count=1,
    )
    # Rewrite the @icons check/arrow lines to the palette's icon colors. These
    # are normally remapped automatically (check=base, arrow=text) but we set
    # them explicitly so dark themes can override for contrast.
    result = re.sub(
        r'(@icons[\s\S]*?check\s*:\s*)#[0-9a-fA-F]{3,8}',
        rf'\g<1>{icons["check"]}',
        result,
        count=1,
    )
    result = re.sub(
        r'(@icons[\s\S]*?arrow\s*:\s*)#[0-9a-fA-F]{3,8}',
        rf'\g<1>{icons["arrow"]}',
        result,
        count=1,
    )
    return result


def build_color_map(entry: dict) -> dict:
    """source-hex -> target-hex for every role."""
    return {SOURCE[role]: target for role, target in entry["roles"].items()}


def generate_theme(template_path: str, output_path: str, palette_name: str):
    if palette_name not in PALETTES:
        print(f"Error: Unknown palette '{palette_name}'")
        print(f"Available palettes: {', '.join(PALETTES.keys())}")
        return False

    template = Path(template_path)
    if not template.exists():
        print(f"Error: Template file not found: {template_path}")
        return False

    entry = PALETTES[palette_name]
    qss_content = template.read_text(encoding='utf-8')

    color_map = build_color_map(entry)
    new_content = replace_colors(qss_content, color_map)
    new_content = update_header(
        new_content,
        entry["display"],
        {"check": entry["check"], "arrow": entry["arrow"]},
    )

    Path(output_path).write_text(new_content, encoding='utf-8')
    print(f"Generated: {output_path}  ({len(color_map)} roles, "
          f"{'dark' if entry['is_dark'] else 'light'})")
    return True


def generate_all(template_path: str, out_dir: str):
    for name in PALETTES:
        generate_theme(template_path, str(Path(out_dir) / f"{name}.qss"), name)


def main():
    parser = argparse.ArgumentParser(description='Generate QSS themes from rose_pine_dawn.qss')
    parser.add_argument('template', help='Path to the template QSS file (rose_pine_dawn.qss)')
    parser.add_argument('output', nargs='?', help='Output QSS path (single-palette mode)')
    parser.add_argument('palette', nargs='?',
                        help=f'Palette name: {", ".join(PALETTES.keys())}')
    parser.add_argument('--all', metavar='OUT_DIR',
                        help='Generate every palette into OUT_DIR')
    args = parser.parse_args()

    if args.all:
        generate_all(args.template, args.all)
    elif args.output and args.palette:
        generate_theme(args.template, args.output, args.palette)
    else:
        parser.error("provide OUTPUT and PALETTE, or use --all OUT_DIR")


if __name__ == '__main__':
    main()
