#!/usr/bin/env python3
"""
Plot the stored periodograms of an ASTRA star.

Usage:
    plot_periodogram.py <star name / alias / Gaia ID / J-name / TIC> [options]

Plots the RV periodogram and any photometric periodograms (one panel
each) from the ASTRA database, marking the strongest peak and, if a
best RV fit exists, its orbital period.
"""

import argparse
import os

import matplotlib.pyplot as plt
import numpy as np

import astra_data as ad


def plot_periodograms(star, conn, log_x=True, period_range=None,
                      output_path=None, output_dir="plots", dpi=300,
                      show=True):
    label = ad.star_label(star)
    curve_id, _ = ad.load_rv_curve(conn, star["id"])
    pgrams = ad.load_periodograms(conn, star["id"], curve_id)
    if not pgrams:
        print(f"No periodograms stored for {label}.")
        return None

    fit = ad.load_best_fit(conn, curve_id) if curve_id else None
    fit_period = fit["period"] if fit and fit["period"] > 0 else None

    n = len(pgrams)
    fig, axes = plt.subplots(n, 1, figsize=(8, 2.2 * n + 0.8),
                             sharex=True, squeeze=False)
    axes = axes.ravel()

    print(f"Star: {label}   ({n} periodogram{'s' if n > 1 else ''})")
    for ax, pg in zip(axes, pgrams):
        with np.errstate(divide="ignore"):
            period = 1.0 / pg["freq"]
        power = pg["power"]
        m = np.isfinite(period) & np.isfinite(power)
        if period_range:
            m &= (period >= period_range[0]) & (period <= period_range[1])
        period, power = period[m], power[m]
        if len(period) == 0:
            ax.set_ylabel(pg["label"])
            continue

        ax.plot(period, power, "-", color="#4477AA", lw=0.6)
        pk = np.argmax(power)
        ax.plot(period[pk], power[pk], "*", color="#EE6677", markersize=10,
                label=f"Peak: {period[pk]:.6f} d", zorder=5)
        if fit_period and period.min() <= fit_period <= period.max():
            ax.axvline(fit_period, color="#228833", ls="--", lw=0.8,
                       label=f"RV fit: {fit_period:.6f} d")
        if log_x:
            ax.set_xscale("log")
        ax.set_ylabel(f"{pg['label']}\npower")
        ax.legend(loc="upper right", fontsize=7)
        print(f"  {pg['label']:<12s}: peak at {period[pk]:.6f} d")

    axes[-1].set_xlabel("Period [d]")
    fig.suptitle(label)
    plt.tight_layout()

    os.makedirs(output_dir, exist_ok=True)
    if output_path is None:
        safe = "".join(c if c.isalnum() or c in "+-._" else "_" for c in label)
        output_path = os.path.join(output_dir, f"{safe}_periodogram.png")
    plt.savefig(output_path, dpi=dpi, bbox_inches="tight")
    print(f"Figure saved to: {output_path}")
    if show:
        plt.show()
    return fig


def main():
    p = argparse.ArgumentParser(
        description="Plot stored periodograms from an ASTRA database.")
    p.add_argument("star", type=str,
                   help="Star name/alias, Gaia source id, J-name or TIC")
    p.add_argument("--db", type=str, default=None,
                   help=f"Path to ASTRA database (default: {ad.DEFAULT_DB})")
    p.add_argument("--linear", action="store_true",
                   help="Linear instead of logarithmic period axis")
    p.add_argument("--pmin", type=float, default=None, help="Min period [d]")
    p.add_argument("--pmax", type=float, default=None, help="Max period [d]")
    p.add_argument("--output", "-o", type=str, default=None)
    p.add_argument("--output-dir", type=str, default="plots")
    p.add_argument("--dpi", type=int, default=300)
    p.add_argument("--no-show", action="store_true")
    args = p.parse_args()

    period_range = None
    if args.pmin is not None or args.pmax is not None:
        period_range = (args.pmin or 0.0, args.pmax or np.inf)

    conn = ad.connect(args.db)
    star = ad.find_star(conn, args.star)
    plot_periodograms(star, conn, log_x=not args.linear,
                      period_range=period_range, output_path=args.output,
                      output_dir=args.output_dir, dpi=args.dpi,
                      show=not args.no_show)


if __name__ == "__main__":
    main()
