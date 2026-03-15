#!/usr/bin/env python3
"""
Base module for matplotlib plot scripts called from the Qt C++ application.

Usage from C++: python3 <script.py> --input <path.json> --output stdout
The script reads JSON configuration, renders a matplotlib figure, and writes
PNG bytes to stdout.
"""

import sys
import json
import argparse
import numpy as np
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend — no GUI needed
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
from matplotlib.backends.backend_agg import FigureCanvasAgg


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', required=True, help='Path to JSON input file')
    parser.add_argument('--output', default='stdout', help='"stdout" or file path')
    return parser.parse_args()


def load_payload(path: str) -> dict:
    with open(path, 'r') as f:
        return json.load(f)


def setup_style(payload: dict) -> dict:
    """Apply dark/light theme and return render params."""
    dark = payload.get('_dark_mode', False)
    dpi = payload.get('_render_dpi', 150)
    w_px = payload.get('_render_width', 800)
    h_px = payload.get('_render_height', 500)

    w_in = w_px / dpi
    h_in = h_px / dpi

    if dark:
        plt.style.use('dark_background')
        # Fine-tune for typical Qt dark themes
        plt.rcParams.update({
            'figure.facecolor': '#2b2b2b',
            'axes.facecolor': '#2b2b2b',
            'axes.edgecolor': '#888888',
            'text.color': '#cccccc',
            'xtick.color': '#aaaaaa',
            'ytick.color': '#aaaaaa',
            'axes.labelcolor': '#cccccc',
            'grid.color': '#444444',
            'legend.facecolor': '#333333',
            'legend.edgecolor': '#555555',
        })
    else:
        plt.style.use('default')
        plt.rcParams.update({
            'figure.facecolor': 'white',
            'axes.facecolor': 'white',
        })

    plt.rcParams.update({
        'font.size': 10,
        'axes.titlesize': 11,
        'axes.labelsize': 10,
        'legend.fontsize': 9,
        'xtick.labelsize': 9,
        'ytick.labelsize': 9,
        'figure.dpi': dpi,
    })

    return {'width': w_in, 'height': h_in, 'dpi': dpi, 'dark': dark}


def output_figure(fig: Figure, payload: dict, args):
    """Render figure to PNG and write to stdout or file."""
    dpi = payload.get('_render_dpi', 150)
    canvas = FigureCanvasAgg(fig)
    canvas.draw()

    import io
    buf = io.BytesIO()
    fig.savefig(buf, format='png', dpi=dpi, bbox_inches='tight',
                pad_inches=0.05, facecolor=fig.get_facecolor())
    buf.seek(0)
    png_bytes = buf.read()

    if args.output == 'stdout':
        sys.stdout.buffer.write(png_bytes)
        sys.stdout.buffer.flush()
    else:
        with open(args.output, 'wb') as f:
            f.write(png_bytes)

    plt.close(fig)