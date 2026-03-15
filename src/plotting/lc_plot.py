#!/usr/bin/env python3
"""
Light curve plot — multiple sources overlaid with error bars.

Expected JSON payload:
{
    "sources": [
        {
            "name": "TESS",
            "bjd": [...],
            "flux": [...],
            "flux_error": [...]
        },
        ...
    ],
    "folded": <bool>,
    "fold_period": <float>,
    "fold_t0": <float>
}
"""

import sys
import numpy as np
import matplotlib.pyplot as plt

sys.path.insert(0, __import__('os').path.dirname(__file__))
from plot_base import parse_args, load_payload, setup_style, output_figure


COLORS = ['#569cd6', '#d69d56', '#56d678', '#d656ba',
          '#d6d656', '#56d6d6', '#b482d6']


def main():
    args = parse_args()
    payload = load_payload(args.input)
    rp = setup_style(payload)

    sources = payload.get('sources', [])
    folded = payload.get('folded', False)
    fold_period = payload.get('fold_period', 0)
    fold_t0 = payload.get('fold_t0', 0)

    fig, ax = plt.subplots(figsize=(rp['width'], rp['height']))

    for i, src in enumerate(sources):
        bjd = np.array(src['bjd'], dtype=float)
        flux = np.array(src['flux'], dtype=float)
        flux_err = np.array(src.get('flux_error', [0] * len(bjd)), dtype=float)
        color = COLORS[i % len(COLORS)]

        if folded and fold_period > 0:
            x = np.mod((bjd - fold_t0) / fold_period, 1.0)
        else:
            x = bjd

        ax.errorbar(x, flux, yerr=flux_err, fmt='.', ms=3,
                     color=color, ecolor=color, alpha=0.7,
                     elinewidth=0.5, capsize=0, label=src.get('name', ''),
                     zorder=2 + i)

    if folded and fold_period > 0:
        ax.set_xlabel('Phase')
        ax.set_xlim(-0.05, 1.05)
    else:
        ax.set_xlabel('BJD')

    ax.set_ylabel('Flux')
    ax.margins(y=0.08)
    ax.grid(True, alpha=0.3)

    if len(sources) > 1:
        ax.legend(loc='best', framealpha=0.8)

    fig.tight_layout()
    output_figure(fig, payload, args)


if __name__ == '__main__':
    main()