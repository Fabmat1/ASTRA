#!/usr/bin/env python3
"""
QSS Theme Generator - Simple color replacement
"""

import re
import argparse
from pathlib import Path


# =============================================================================
# COLOR MAPPINGS: Catppuccin Latte -> New Theme
# =============================================================================

ROSE_PINE_DAWN = {
    # Base colors (backgrounds)
    "#eff1f5": "#faf4ed",  # Base
    "#e6e9ef": "#fffaf3",  # Mantle
    "#dce0e8": "#f2e9e1",  # Crust
    
    # Surface colors
    "#ccd0da": "#f4ede8",  # Surface0
    "#bcc0cc": "#dfdad9",  # Surface1
    "#acb0be": "#cecacd",  # Surface2
    
    # Overlay colors
    "#9ca0b0": "#9893a5",  # Overlay0
    "#8c8fa1": "#797593",  # Overlay1
    "#7c7f93": "#6e6a86",  # Overlay2
    
    # Text colors
    "#4c4f69": "#575279",  # Text
    "#5c5f77": "#797593",  # Subtext1
    "#6c6f85": "#9893a5",  # Subtext0
    
    # Accent colors
    "#1e66f5": "#286983",  # Blue (Pine)
    "#7287fd": "#907aa9",  # Lavender (Iris)
    "#d20f39": "#b4637a",  # Red (Love)
    "#df8e1d": "#ea9d34",  # Yellow (Gold)
    "#40a02b": "#56949f",  # Green (Foam)
    
    # Hover/pressed variants
    "#2a6ff7": "#3b7c96",  # Blue hover
    "#1a5ce0": "#1f5870",  # Blue pressed
    "#e01040": "#c4738a",  # Red hover
    "#b80d32": "#a4536a",  # Red pressed
}

NORD_LIGHT = {
    # Base colors (backgrounds)
    "#eff1f5": "#eceff4",  # Base (Nord Snow Storm 3)
    "#e6e9ef": "#e5e9f0",  # Mantle (Nord Snow Storm 2)
    "#dce0e8": "#d8dee9",  # Crust (Nord Snow Storm 1)
    
    # Surface colors
    "#ccd0da": "#d8dee9",  # Surface0
    "#bcc0cc": "#c9d1dc",  # Surface1
    "#acb0be": "#b8c4d0",  # Surface2
    
    # Overlay colors
    "#9ca0b0": "#9ba6b5",  # Overlay0
    "#8c8fa1": "#8892a2",  # Overlay1
    "#7c7f93": "#7b8392",  # Overlay2
    
    # Text colors
    "#4c4f69": "#2e3440",  # Text (Nord Polar Night 1)
    "#5c5f77": "#3b4252",  # Subtext1 (Nord Polar Night 2)
    "#6c6f85": "#4c566a",  # Subtext0 (Nord Polar Night 4)
    
    # Accent colors
    "#1e66f5": "#5e81ac",  # Blue (Nord Frost 4)
    "#7287fd": "#81a1c1",  # Lavender (Nord Frost 3)
    "#d20f39": "#bf616a",  # Red (Nord Aurora Red)
    "#df8e1d": "#d08770",  # Yellow (Nord Aurora Orange)
    "#40a02b": "#a3be8c",  # Green (Nord Aurora Green)
    
    # Hover/pressed variants
    "#2a6ff7": "#6e91bc",  # Blue hover
    "#1a5ce0": "#4e719c",  # Blue pressed
    "#e01040": "#cf717a",  # Red hover
    "#b80d32": "#af515a",  # Red pressed
}

GRUVBOX_LIGHT = {
    # Base colors (backgrounds)
    "#eff1f5": "#fbf1c7",  # Base (bg0)
    "#e6e9ef": "#f2e5bc",  # Mantle (bg1)
    "#dce0e8": "#ebdbb2",  # Crust (bg2)
    
    # Surface colors
    "#ccd0da": "#d5c4a1",  # Surface0 (bg3)
    "#bcc0cc": "#bdae93",  # Surface1 (bg4)
    "#acb0be": "#a89984",  # Surface2 (gray)
    
    # Overlay colors
    "#9ca0b0": "#928374",  # Overlay0
    "#8c8fa1": "#7c6f64",  # Overlay1
    "#7c7f93": "#665c54",  # Overlay2
    
    # Text colors
    "#4c4f69": "#3c3836",  # Text (fg0)
    "#5c5f77": "#504945",  # Subtext1 (fg1)
    "#6c6f85": "#665c54",  # Subtext0 (fg2)
    
    # Accent colors
    "#1e66f5": "#458588",  # Blue
    "#7287fd": "#83a598",  # Lavender (Aqua)
    "#d20f39": "#cc241d",  # Red
    "#df8e1d": "#d79921",  # Yellow
    "#40a02b": "#98971a",  # Green
    
    # Hover/pressed variants
    "#2a6ff7": "#559598",  # Blue hover
    "#1a5ce0": "#357578",  # Blue pressed
    "#e01040": "#dc342d",  # Red hover
    "#b80d32": "#bc140d",  # Red pressed
}

SOLARIZED_LIGHT = {
    # Base colors (backgrounds)
    "#eff1f5": "#fdf6e3",  # Base (base3)
    "#e6e9ef": "#eee8d5",  # Mantle (base2)
    "#dce0e8": "#e4ddc8",  # Crust
    
    # Surface colors
    "#ccd0da": "#d6d0bb",  # Surface0
    "#bcc0cc": "#c9c3ae",  # Surface1
    "#acb0be": "#b8b2a1",  # Surface2
    
    # Overlay colors
    "#9ca0b0": "#93a1a1",  # Overlay0 (base1)
    "#8c8fa1": "#839496",  # Overlay1 (base0)
    "#7c7f93": "#657b83",  # Overlay2 (base00)
    
    # Text colors
    "#4c4f69": "#657b83",  # Text (base00)
    "#5c5f77": "#586e75",  # Subtext1 (base01)
    "#6c6f85": "#93a1a1",  # Subtext0 (base1)
    
    # Accent colors
    "#1e66f5": "#268bd2",  # Blue
    "#7287fd": "#6c71c4",  # Lavender (Violet)
    "#d20f39": "#dc322f",  # Red
    "#df8e1d": "#b58900",  # Yellow
    "#40a02b": "#859900",  # Green
    
    # Hover/pressed variants
    "#2a6ff7": "#369be2",  # Blue hover
    "#1a5ce0": "#167bc2",  # Blue pressed
    "#e01040": "#ec423f",  # Red hover
    "#b80d32": "#cc221f",  # Red pressed
}

GITHUB_LIGHT = {
    # Base colors (backgrounds)
    "#eff1f5": "#ffffff",  # Base
    "#e6e9ef": "#f6f8fa",  # Mantle
    "#dce0e8": "#eaeef2",  # Crust
    
    # Surface colors
    "#ccd0da": "#d0d7de",  # Surface0
    "#bcc0cc": "#bdc4cb",  # Surface1
    "#acb0be": "#afb8c1",  # Surface2
    
    # Overlay colors
    "#9ca0b0": "#8c959f",  # Overlay0
    "#8c8fa1": "#6e7781",  # Overlay1
    "#7c7f93": "#57606a",  # Overlay2
    
    # Text colors
    "#4c4f69": "#1f2328",  # Text
    "#5c5f77": "#424a53",  # Subtext1
    "#6c6f85": "#57606a",  # Subtext0
    
    # Accent colors
    "#1e66f5": "#0969da",  # Blue
    "#7287fd": "#8250df",  # Lavender (Purple)
    "#d20f39": "#cf222e",  # Red
    "#df8e1d": "#9a6700",  # Yellow
    "#40a02b": "#1a7f37",  # Green
    
    # Hover/pressed variants
    "#2a6ff7": "#1979ea",  # Blue hover
    "#1a5ce0": "#0059ca",  # Blue pressed
    "#e01040": "#df323e",  # Red hover
    "#b80d32": "#bf121e",  # Red pressed
}

ONE_LIGHT = {
    # Base colors (backgrounds) - Atom One Light
    "#eff1f5": "#fafafa",  # Base
    "#e6e9ef": "#f0f0f0",  # Mantle
    "#dce0e8": "#e5e5e6",  # Crust
    
    # Surface colors
    "#ccd0da": "#d4d4d4",  # Surface0
    "#bcc0cc": "#c4c4c4",  # Surface1
    "#acb0be": "#a0a1a7",  # Surface2
    
    # Overlay colors
    "#9ca0b0": "#9d9d9f",  # Overlay0
    "#8c8fa1": "#8b8b8d",  # Overlay1
    "#7c7f93": "#696c77",  # Overlay2
    
    # Text colors
    "#4c4f69": "#383a42",  # Text
    "#5c5f77": "#4f525e",  # Subtext1
    "#6c6f85": "#696c77",  # Subtext0
    
    # Accent colors
    "#1e66f5": "#4078f2",  # Blue
    "#7287fd": "#a626a4",  # Lavender (Magenta)
    "#d20f39": "#e45649",  # Red
    "#df8e1d": "#c18401",  # Yellow
    "#40a02b": "#50a14f",  # Green
    
    # Hover/pressed variants
    "#2a6ff7": "#5088ff",  # Blue hover
    "#1a5ce0": "#3068e2",  # Blue pressed
    "#e01040": "#f46659",  # Red hover
    "#b80d32": "#d44639",  # Red pressed
}

TOKYO_NIGHT_LIGHT = {
    # Base colors (backgrounds)
    "#eff1f5": "#d5d6db",  # Base
    "#e6e9ef": "#cbccd1",  # Mantle
    "#dce0e8": "#c0c1c7",  # Crust
    
    # Surface colors
    "#ccd0da": "#b4b5bd",  # Surface0
    "#bcc0cc": "#9699a3",  # Surface1
    "#acb0be": "#8c8e98",  # Surface2
    
    # Overlay colors
    "#9ca0b0": "#6f7282",  # Overlay0
    "#8c8fa1": "#5a5c6b",  # Overlay1
    "#7c7f93": "#4c505e",  # Overlay2
    
    # Text colors
    "#4c4f69": "#343b59",  # Text
    "#5c5f77": "#4c505e",  # Subtext1
    "#6c6f85": "#5a5c6b",  # Subtext0
    
    # Accent colors
    "#1e66f5": "#34548a",  # Blue
    "#7287fd": "#5a4a78",  # Lavender (Purple)
    "#d20f39": "#8c4351",  # Red
    "#df8e1d": "#8f5e15",  # Yellow
    "#40a02b": "#33635c",  # Green
    
    # Hover/pressed variants
    "#2a6ff7": "#44649a",  # Blue hover
    "#1a5ce0": "#24447a",  # Blue pressed
    "#e01040": "#9c5361",  # Red hover
    "#b80d32": "#7c3341",  # Red pressed
}


# =============================================================================
# PALETTE REGISTRY - Add new palettes here
# =============================================================================

PALETTES = {
    "rose-pine-dawn": ROSE_PINE_DAWN,
    "nord-light": NORD_LIGHT,
    "gruvbox-light": GRUVBOX_LIGHT,
    "solarized-light": SOLARIZED_LIGHT,
    "github-light": GITHUB_LIGHT,
    "one-light": ONE_LIGHT,
    "tokyo-night-light": TOKYO_NIGHT_LIGHT,
}

def replace_colors(qss_content: str, color_map: dict) -> str:
    """Replace all color values in QSS content based on the color map."""
    result = qss_content
    
    # Sort by length (longest first) to avoid partial replacements
    for old_color, new_color in sorted(color_map.items(), key=lambda x: len(x[0]), reverse=True):
        # Case-insensitive replacement
        pattern = re.compile(re.escape(old_color), re.IGNORECASE)
        result = pattern.sub(new_color, result)
    
    return result


def update_header(qss_content: str, theme_name: str) -> str:
    """Update the theme name in the header comment."""
    # Replace the theme name in the header
    result = re.sub(
        r'CATPPUCCIN LATTE \(LIGHT\) THEME',
        f'{theme_name.upper()} THEME',
        qss_content
    )
    return result


def generate_theme(template_path: str, output_path: str, palette_name: str):
    """Generate a new theme file from template."""
    
    if palette_name not in PALETTES:
        print(f"Error: Unknown palette '{palette_name}'")
        print(f"Available palettes: {', '.join(PALETTES.keys())}")
        return False
    
    template = Path(template_path)
    if not template.exists():
        print(f"Error: Template file not found: {template_path}")
        return False
    
    # Read template
    qss_content = template.read_text(encoding='utf-8')
    
    # Replace colors
    color_map = PALETTES[palette_name]
    new_content = replace_colors(qss_content, color_map)
    
    # Update header
    theme_display_name = palette_name.replace('-', ' ').title()
    new_content = update_header(new_content, theme_display_name)
    
    # Write output
    output = Path(output_path)
    output.write_text(new_content, encoding='utf-8')
    
    print(f"Generated: {output_path}")
    print(f"Replaced {len(color_map)} colors")
    return True


def main():
    parser = argparse.ArgumentParser(description='Generate QSS themes from a template')
    parser.add_argument('template', help='Path to the template QSS file')
    parser.add_argument('output', help='Path for the output QSS file')
    parser.add_argument('palette', help=f'Palette name: {", ".join(PALETTES.keys())}')
    
    args = parser.parse_args()
    generate_theme(args.template, args.output, args.palette)


if __name__ == '__main__':
    main()