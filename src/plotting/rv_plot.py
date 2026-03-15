#!/usr/bin/env python3
"""
Radial velocity plot with broken-axis support, error bars, and fit overlay.

Expected JSON payload:
{
    "times": [...],
    "rvs": [...],
    "errors": [...],
    "t0_offset": <float>,         // times already offset by this
    "folded": <bool>,
    "fit": {
        "period": <float>,
        "phi": <float>,
        "K": <float>,
        "gamma": <float>,
        "eccentricity": <float>,  // optional
        "omega": <float>,         // optional
        "fit_times": [...],       // pre-computed x values for fit curve
        "fit_rvs": [...]          // pre-computed y values for fit curve
    }  // or null
}
"""

import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
from matplotlib.gridspec import GridSpec

# Add parent to path so we can import plot_base
sys.path.insert(0, __import__('os').path.dirname(__file__))
from plot_base import parse_args, load_payload, setup_style, output_figure


# ── Gap detection (mirrors C++ findGapIndices) ──────────────────────

def find_gaps(times: np.ndarray, factor: float = 5.0) -> list:
    if len(times) < 3:
        return []
    diffs = np.diff(times)
    median = np.median(diffs)
    threshold = max(median * factor, 1.0)
    return list(np.where(diffs > threshold)[0] + 1)


def split_at(arr, indices):
    return np.split(arr, indices)


# ── Main ─────────────────────────────────────────────────────────────

def main():
    args = parse_args()
    payload = load_payload(args.input)
    rp = setup_style(payload)

    times  = np.array(payload['times'],  dtype=float)
    rvs    = np.array(payload['rvs'],    dtype=float)
    errors = np.array(payload['errors'], dtype=float)
    folded = payload.get('folded', False)
    fit    = payload.get('fit', None)

    # Colors
    pt_color  = '#569cd6'
    err_color = '#c87878'
    fit_color = '#dc3232'

    if folded and fit and fit.get('period', 0) > 0:
        # ── FOLDED VIEW ──
        P   = fit['period']
        phi = fit['phi']

        phases = np.mod((times - phi) / P, 1.0)

        fig, ax = plt.subplots(figsize=(rp['width'], rp['height']))

        ax.errorbar(phases, rvs, yerr=errors, fmt='o', ms=4,
                     color=pt_color, ecolor=err_color, elinewidth=1,
                     capsize=2, capthick=1, zorder=3)

        if 'fit_times' in fit and 'fit_rvs' in fit:
            ax.plot(fit['fit_times'], fit['fit_rvs'],
                    '-', color=fit_color, lw=2, zorder=2)

        ax.set_xlabel('Phase')
        ax.set_ylabel('RV [km/s]')
        ax.set_xlim(-0.05, 1.05)
        ax.margins(y=0.1)
        ax.grid(True, alpha=0.3)

        fig.tight_layout()
        output_figure(fig, payload, args)

    else:
        # ── TIMELINE VIEW (with broken axis if gaps found) ──

        gap_indices = find_gaps(times)
        n_segments = len(gap_indices) + 1

        if n_segments == 1:
            # Simple single-panel plot
            fig, ax = plt.subplots(figsize=(rp['width'], rp['height']))

            ax.errorbar(times, rvs, yerr=errors, fmt='o', ms=4,
                         color=pt_color, ecolor=err_color, elinewidth=1,
                         capsize=2, capthick=1, zorder=3)

            if fit and 'fit_times' in fit and 'fit_rvs' in fit:
                ax.plot(fit['fit_times'], fit['fit_rvs'],
                        '-', color=fit_color, lw=2, zorder=2)

            ax.set_xlabel('Days from first observation')
            ax.set_ylabel('RV [km/s]')
            ax.margins(x=0.05, y=0.1)
            ax.grid(True, alpha=0.3)

            fig.tight_layout()
            output_figure(fig, payload, args)

        else:
            # ── Broken-axis plot ──
            seg_times  = split_at(times,  gap_indices)
            seg_rvs    = split_at(rvs,    gap_indices)
            seg_errors = split_at(errors, gap_indices)

            # Compute proportional widths
            widths = []
            for seg in seg_times:
                w = seg[-1] - seg[0] if len(seg) > 1 else 1.0
                widths.append(max(w, 0.05 * max(s[-1] - s[0] for s in seg_times if len(s) > 1)))

            fig, axes = plt.subplots(
                1, n_segments,
                figsize=(rp['width'], rp['height']),
                sharey=True,
                gridspec_kw={'width_ratios': widths, 'wspace': 0.04}
            )

            if n_segments == 1:
                axes = [axes]

            # Global Y range
            all_lo = rvs - errors
            all_hi = rvs + errors
            y_min, y_max = np.nanmin(all_lo), np.nanmax(all_hi)
            y_margin = max((y_max - y_min) * 0.1, 1.0)

            for i, ax in enumerate(axes):
                st = seg_times[i]
                sr = seg_rvs[i]
                se = seg_errors[i]

                ax.errorbar(st, sr, yerr=se, fmt='o', ms=4,
                             color=pt_color, ecolor=err_color, elinewidth=1,
                             capsize=2, capthick=1, zorder=3)

                if fit and 'fit_times' in fit and 'fit_rvs' in fit:
                    ft = np.array(fit['fit_times'])
                    fr = np.array(fit['fit_rvs'])
                    mask = (ft >= st[0] - 1) & (ft <= st[-1] + 1)
                    if mask.any():
                        ax.plot(ft[mask], fr[mask], '-', color=fit_color, lw=2, zorder=2)

                span = st[-1] - st[0] if len(st) > 1 else 1.0
                ax.set_xlim(st[0] - span * 0.08, st[-1] + span * 0.08)
                ax.set_ylim(y_min - y_margin, y_max + y_margin)
                ax.grid(True, alpha=0.3)

                # ── Broken-axis marks ──
                if i > 0:
                    ax.spines['left'].set_visible(False)
                    ax.tick_params(left=False)
                if i < n_segments - 1:
                    ax.spines['right'].set_visible(False)

                # Draw diagonal break marks
                d = 0.015
                kwargs = dict(transform=ax.transAxes, color='gray',
                              clip_on=False, lw=1)
                if i > 0:
                    ax.plot((-d, +d), (1 - d, 1 + d), **kwargs)
                    ax.plot((-d, +d), (-d, +d), **kwargs)
                if i < n_segments - 1:
                    ax.plot((1 - d, 1 + d), (1 - d, 1 + d), **kwargs)
                    ax.plot((1 - d, 1 + d), (-d, +d), **kwargs)

            axes[0].set_ylabel('RV [km/s]')

            # Shared x-label
            fig.text(0.5, 0.01, 'Days from first observation',
                     ha='center', fontsize=10)

            fig.tight_layout(rect=[0, 0.04, 1, 1])
            output_figure(fig, payload, args)


if __name__ == '__main__':
    main()