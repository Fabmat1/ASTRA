#!/usr/bin/env python3
"""
Plot the phase-folded RV curve of an ASTRA star — and, if available,
its lightcurves folded and synchronized to the same ephemeris.

Usage:
    plot_curve.py <star name / alias / Gaia ID / J-name / TIC> [options]

RV points, the best orbital fit and all lightcurves are loaded from the
ASTRA database (default: ~/data/ASTRA/astra.db). Fit parameters can be
overridden on the command line.
"""

import argparse
import os
import sys

import matplotlib as mpl
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.gridspec import GridSpec
from scipy.stats import binned_statistic

import astra_data as ad

mpl.rcParams["font.family"] = "sans-serif"
mpl.rcParams["font.sans-serif"] = ["Arial"]

PAUL_TOL_COLORS = {
    "bright":  {"blue": "#4477AA", "cyan": "#66CCEE", "green": "#228833",
                "yellow": "#CCBB44", "red": "#EE6677", "purple": "#AA3377",
                "grey": "#BBBBBB"},
    "vibrant": {"blue": "#0077BB", "cyan": "#33BBEE", "teal": "#009988",
                "orange": "#EE7733", "red": "#CC3311", "magenta": "#EE3377",
                "grey": "#BBBBBB"},
    "muted":   {"rose": "#CC6677", "indigo": "#332288", "sand": "#DDCC77",
                "green": "#117733", "cyan": "#88CCEE", "wine": "#882255",
                "teal": "#44AA99", "olive": "#999933", "purple": "#AA4499"},
}

INSTRUMENT_COLOR_CYCLE = ["rose", "indigo", "green", "wine",
                          "cyan", "teal", "olive", "purple"]


def get_filter_color(tel, filt):
    p = PAUL_TOL_COLORS["muted"]
    cm = {"GAIA": {"BP": p["indigo"], "G": p["sand"], "RP": p["rose"]},
          "ZTF":  {"zg": p["green"], "zr": p["rose"], "zi": p["wine"]},
          "ATLAS": {"c": p["cyan"], "o": PAUL_TOL_COLORS["vibrant"]["orange"]},
          "TESS": PAUL_TOL_COLORS["bright"]["blue"],
          "BLACKGEM": p["olive"]}
    if tel in cm:
        return cm[tel].get(filt, p["purple"]) if isinstance(cm[tel], dict) else cm[tel]
    return "#808080"


def bin_data(x, y, yerr=None, nbins=None):
    if nbins is None:
        nbins = int(np.cbrt(len(x)))
    si = np.argsort(x)
    xs, ys = x[si], y[si]
    be = np.linspace(xs.min(), xs.max(), nbins + 1)
    bc = 0.5 * (be[:-1] + be[1:])
    yb, _, _ = binned_statistic(xs, ys, statistic="mean", bins=be)
    ct, _, _ = binned_statistic(xs, ys, statistic="count", bins=be)
    if yerr is not None:
        es = yerr[si]
        eq, _, _ = binned_statistic(xs, es**2, statistic="sum", bins=be)
        eb = np.sqrt(eq) / ct
    else:
        eb = None
    m = ~np.isnan(yb)
    return bc[m], yb[m], eb[m] if eb is not None else None


def parse_lc_fit_shift(s):
    """
    Parse --lc-fit-shift: either a single phase shift applied to all LC
    fit models ('0.02') or per-survey values ('TESS:0.02,ZTF:-0.01').
    Returns dict survey -> shift, with '*' as the global key.
    """
    if s is None:
        return {}
    try:
        return {"*": float(s)}
    except ValueError:
        pass
    cfg = {}
    for item in s.split(","):
        item = item.strip()
        if ":" in item:
            t, v = item.split(":", 1)
            try:
                cfg[t.strip().upper()] = float(v)
            except ValueError:
                sys.exit(f"--lc-fit-shift: cannot parse '{item}'")
    return cfg


def parse_binning_config(s):
    if s is None:
        return {}
    cfg = {}
    for item in s.split(","):
        item = item.strip()
        if ":" in item:
            t, v = item.split(":", 1)
            t = t.strip().upper()
            v = v.strip().lower()
            if v in ("none", "no"):
                cfg[t] = None
            elif v == "auto":
                cfg[t] = "auto"
            else:
                try:
                    cfg[t] = int(v)
                except ValueError:
                    cfg[t] = "auto"
    return cfg


# ====================================================================
#  Main plot
# ====================================================================

def plot_rv_and_lightcurves(
        star, conn, fit_overrides=None,
        show_lightcurves=True, selected_lightcurves=None,
        show_lc_fits=True, lc_fit_shift=None,
        lc_bins=None, binning_config=None,
        show_rv_legend=True, show_lc_legend=True,
        legend_loc="upper right", figsize=None, fontsize=None,
        output_path=None, output_dir="plots", dpi=300, show=True):

    label = ad.star_label(star)

    # --- RV data + fit ---
    curve_id, rv = ad.load_rv_curve(conn, star["id"])
    if rv is None:
        print(f"No RV points stored for {label}.")
        return None

    fit = ad.load_best_fit(conn, curve_id)
    if fit is None:
        print(f"No RV fit stored for {label}; "
              f"provide --period/--k/--gamma to plot anyway.")
        fit = dict(k=0.0, gamma=float(np.mean(rv["rv"])), period=0.0,
                   phi=0.0, t0=0.0, eccentricity=0.0, omega=0.0,
                   fit_method="(none)", chi2=0.0)
    if fit_overrides:
        fit.update(fit_overrides)
    if fit["period"] <= 0:
        print("Fit has no valid period — cannot phase-fold.")
        return None

    period = fit["period"]
    t_ref = rv["t"].min()                       # earliest point, as in ASTRA
    t0 = ad.fit_t0(fit, t_ref)

    print(f"Star: {label}   (project: {star['project_name']})")
    print(f"Fit:  {fit.get('fit_method') or '-'}")
    print(f"  {'period':<14}: {period:.6f} d")
    print(f"  {'K':<14}: {fit['k']:.4f} km/s")
    print(f"  {'gamma':<14}: {fit['gamma']:.4f} km/s")
    print(f"  {'T0':<14}: {t0:.6f} JD")
    if ad.fit_is_eccentric(fit):
        print(f"  {'eccentricity':<14}: {fit['eccentricity']:.4f}")
        print(f"  {'omega':<14}: {fit.get('omega', 0.0):.4f} deg")

    # --- phase-fold & model ---
    rv_ph = ad.phase_fold(rv["t"], period, t0)
    pg = np.linspace(-1, 1, 1000)
    model_rv = ad.rv_at_phase(fit, pg)
    model_at_obs = ad.rv_at_phase(fit, np.mod(rv_ph, 1.0))

    res = rv["rv"] - model_at_obs
    with np.errstate(divide="ignore", invalid="ignore"):
        chi = np.where(rv["err"] > 0, res / rv["err"], np.nan)
    n_par = 6 if ad.fit_is_eccentric(fit) else 4
    dof = len(res) - n_par
    chi2r = np.nansum(chi**2) / dof if dof > 0 else float("inf")
    print(f"  {'chi2/dof':<14}: {chi2r:.2f}")

    # --- lightcurves ---
    lightcurves = {}
    if show_lightcurves:
        lightcurves = ad.load_lightcurves(conn, star["id"])
        if selected_lightcurves is not None:
            sel = {s.upper() for s in selected_lightcurves}
            lightcurves = {k: v for k, v in lightcurves.items() if k in sel}
    npan = len(lightcurves)
    if npan:
        print(f"  lightcurves   : {', '.join(lightcurves)}")

    lc_fits = (ad.load_lc_fits(conn, star["id"])
               if show_lc_fits and lightcurves else [])
    fits_by_source = {}
    for lf in lc_fits:
        if lf["source"] in lightcurves:
            print(f"  LC fit        : {lf['source']} "
                  f"P={lf['period']:.6f} d  ({lf['label']})")
            fits_by_source.setdefault(lf["source"], []).append(lf)
    nchi = len(fits_by_source)

    # --- figure ---
    dfs = {"general": 8, "labels": 8, "legend": 6, "ticks": 7}
    if fontsize:
        dfs.update(fontsize)
    if figsize is None:
        figsize = (7, 3 + 1.5 * npan + 0.5 * nchi)
    plt.rcParams.update({
        "figure.figsize": figsize, "font.size": dfs["general"],
        "axes.labelsize": dfs["labels"], "legend.fontsize": dfs["legend"],
        "xtick.labelsize": dfs["ticks"], "ytick.labelsize": dfs["ticks"],
        "xtick.direction": "in", "ytick.direction": "in",
        "xtick.top": True, "ytick.right": True,
        "axes.linewidth": 0.8, "lines.linewidth": 1.0, "patch.linewidth": 0.8,
    })
    fig = plt.figure(figsize=figsize)
    hr = [2, 0.7]
    for tel in lightcurves:
        hr.append(2)
        if tel in fits_by_source:
            hr.append(0.7)                      # χ_LC residual panel
    gs = GridSpec(len(hr), 1, height_ratios=hr, hspace=0)
    ax_rv = fig.add_subplot(gs[0])
    ax_res = fig.add_subplot(gs[1], sharex=ax_rv)

    muted = PAUL_TOL_COLORS["muted"]
    instruments = sorted(set(rv["instrument"]))
    icol = {inst: muted[INSTRUMENT_COLOR_CYCLE[i % len(INSTRUMENT_COLOR_CYCLE)]]
            for i, inst in enumerate(instruments)}

    for inst in instruments:
        m = rv["instrument"] == inst
        for sh in (-1, 0, 1):
            ax_rv.errorbar(rv_ph[m] + sh, rv["rv"][m], yerr=rv["err"][m],
                           fmt=".", color=icol[inst], markersize=10,
                           elinewidth=1.2, label=inst if sh == 0 else "",
                           zorder=5, markeredgecolor="white",
                           markeredgewidth=0.4)
    ax_rv.plot(pg, model_rv, "k-", lw=1.2, label="Model", zorder=3)
    ax_rv.set_ylabel("RV (km/s)")
    if show_rv_legend:
        ax_rv.legend(loc=legend_loc, framealpha=0.9).set_zorder(99)
    plt.setp(ax_rv.get_xticklabels(), visible=False)

    for inst in instruments:
        m = rv["instrument"] == inst
        for sh in (-1, 0, 1):
            ax_res.errorbar(rv_ph[m] + sh, chi[m], yerr=1, fmt=".",
                            color=icol[inst], markersize=8, elinewidth=1.2,
                            zorder=5, markeredgecolor="white",
                            markeredgewidth=0.4)
    ax_res.axhline(0, color="grey", ls="--", lw=0.8, zorder=3)
    ax_res.set_ylabel(r"$\chi_{\mathrm{RV}}$")
    ax_res.set_ylim(-4, 4)
    if npan == 0:
        ax_res.set_xlabel("Phase")
    else:
        plt.setp(ax_res.get_xticklabels(), visible=False)

    if binning_config is None:
        binning_config = {}

    if lc_fit_shift is None:
        lc_fit_shift = {}

    def model_shift(tel):
        return lc_fit_shift.get(tel, lc_fit_shift.get("*", 0.0))

    def fit_for_filter(tel, fn):
        cands = fits_by_source.get(tel, [])
        for lf in cands:
            if lf["filter"] and lf["filter"].upper() == fn.upper():
                return lf
        for lf in cands:
            if not lf["filter"] or lf["filter"].upper() == tel:
                return lf
        return cands[0] if cands else None

    gi = 2
    for ii, (tel, fd) in enumerate(lightcurves.items()):
        ax_lc = fig.add_subplot(gs[gi], sharex=ax_rv)
        gi += 1
        ax_chi = None
        if tel in fits_by_source:
            ax_chi = fig.add_subplot(gs[gi], sharex=ax_rv)
            gi += 1
        for fn, (tt, ff, ee) in fd.items():
            lph = ad.phase_fold(tt, period, t0)
            fm = np.median(ff)
            fn_ = ff / fm
            en = ee / fm if ee is not None else None
            col_ = get_filter_color(tel, fn)
            lab = f"{tel}-{fn}" if fn != "default" else tel
            if tel in binning_config:
                bs = binning_config[tel]
                nb = (None if bs is None
                      else (50 if bs == "auto" and len(tt) > 250
                            else (None if bs == "auto" else bs)))
            elif lc_bins is not None:
                nb = lc_bins
            elif len(tt) > 250:
                nb = 50
            else:
                nb = None
            if nb:
                pb, fb, eb = bin_data(lph, fn_, en, nbins=nb)
                for sh in (-1, 0, 1):
                    ax_lc.errorbar(pb + sh, fb, yerr=eb, fmt=".", color=col_,
                                   markersize=6, elinewidth=0.5,
                                   label=f"{lab} (n={nb})" if sh == 0 else "",
                                   zorder=5, markeredgecolor="white",
                                   markeredgewidth=0.4)
                pc, fc, ec_ = pb, fb, eb
            else:
                for sh in (-1, 0, 1):
                    ax_lc.errorbar(lph + sh, fn_, yerr=en, fmt=".", color=col_,
                                   markersize=6, elinewidth=0.5,
                                   label=lab if sh == 0 else "", zorder=5,
                                   markeredgecolor="white",
                                   markeredgewidth=0.4)
                pc, fc, ec_ = lph, fn_, en

            # χ_LC residuals of the plotted points against the LC fit model
            lf = fit_for_filter(tel, fn)
            if ax_chi is not None and lf is not None and ec_ is not None:
                mflux = ad.lc_fit_model_flux(
                    lf, t0 + (pc - model_shift(tel)) * period)
                with np.errstate(divide="ignore", invalid="ignore"):
                    ch = np.where(ec_ > 0, (fc - mflux) / ec_, np.nan)
                for sh in (-1, 0, 1):
                    ax_chi.errorbar(pc + sh, ch, yerr=1, fmt=".", color=col_,
                                    markersize=5, elinewidth=0.5, zorder=5,
                                    markeredgecolor="white",
                                    markeredgewidth=0.4)
        # overlay stored LC fit models, remapped onto the RV-fold phase axis
        for lf in fits_by_source.get(tel, []):
            mflux = ad.lc_fit_model_flux(
                lf, t0 + (pg - model_shift(tel)) * period)
            mlab = "Model"
            if lf["filter"] and lf["filter"].upper() != tel:
                mlab += f" ({lf['filter']})"
            ax_lc.plot(pg, mflux, "k-", lw=1.2, label=mlab, zorder=3)
        ax_lc.set_ylabel("Rel. Flux")
        if show_lc_legend:
            ax_lc.legend(loc=legend_loc, ncol=2, framealpha=0.9).set_zorder(99)
        ax_lc.axhline(1, color="gray", ls=":", lw=0.8)
        if ax_chi is not None:
            ax_chi.axhline(0, color="grey", ls="--", lw=0.8, zorder=3)
            ax_chi.set_ylabel(r"$\chi_{\mathrm{LC}}$")
            ax_chi.set_ylim(-4, 4)
        bottom_ax = ax_chi if ax_chi is not None else ax_lc
        if ii == npan - 1:
            if ax_chi is not None:
                plt.setp(ax_lc.get_xticklabels(), visible=False)
            bottom_ax.set_xlabel("Phase")
        else:
            plt.setp(ax_lc.get_xticklabels(), visible=False)
            if ax_chi is not None:
                plt.setp(ax_chi.get_xticklabels(), visible=False)

    ax_rv.set_xlim(-1, 1)
    plt.tight_layout(pad=0.0, h_pad=0.0)
    os.makedirs(output_dir, exist_ok=True)
    if output_path is None:
        safe = "".join(c if c.isalnum() or c in "+-._" else "_" for c in label)
        output_path = os.path.join(output_dir, f"{safe}_rvcurve.pdf")
    plt.savefig(output_path, bbox_inches="tight", dpi=dpi, pad_inches=0)
    print(f"Figure saved to: {output_path}")
    if show:
        plt.show()
    return fig


# ====================================================================
if __name__ == "__main__":
    p = argparse.ArgumentParser(
        description="Plot phase-folded RV curve and synchronized lightcurves "
                    "from an ASTRA database.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("star", type=str,
                   help="Star name/alias, Gaia source id, J-name or TIC")
    p.add_argument("--db", type=str, default=None,
                   help=f"Path to ASTRA database (default: {ad.DEFAULT_DB})")
    # fit parameter overrides
    p.add_argument("--period", type=float, help="Override period [d]")
    p.add_argument("--k", "--amplitude", dest="k", type=float,
                   help="Override RV semi-amplitude K [km/s]")
    p.add_argument("--gamma", "--offset", dest="gamma", type=float,
                   help="Override systemic velocity [km/s]")
    p.add_argument("--phi", type=float,
                   help="Override phase at reference epoch")
    p.add_argument("--ecc", type=float, help="Override eccentricity")
    p.add_argument("--omega", type=float,
                   help="Override argument of periapsis [deg]")
    # lightcurve options
    p.add_argument("--lightcurves", type=str, default=None,
                   help="Comma-separated subset, e.g. TESS,ZTF")
    p.add_argument("--no-lightcurves", action="store_true")
    p.add_argument("--no-lc-fits", action="store_true",
                   help="Do not overlay stored lightcurve fit models")
    p.add_argument("--lc-fit-shift", type=str, default=None,
                   help="Phase shift applied to LC fit models, global "
                        "('0.02') or per-survey ('TESS:0.02,ZTF:-0.01')")
    p.add_argument("--lc_bins", type=int, default=None)
    p.add_argument("--binning", type=str, default=None,
                   help="Per-survey binning, e.g. 'TESS:100,ZTF:none'")
    # appearance
    p.add_argument("--no-rv-legend", action="store_true")
    p.add_argument("--no-lc-legend", action="store_true")
    p.add_argument("--legend-loc", type=str, default="upper right")
    p.add_argument("--figsize", type=str, default=None)
    p.add_argument("--fontsize", type=float, default=None)
    p.add_argument("--label-fontsize", type=float, default=None)
    p.add_argument("--legend-fontsize", type=float, default=None)
    p.add_argument("--tick-fontsize", type=float, default=None)
    # output
    p.add_argument("--output", "-o", type=str, default=None)
    p.add_argument("--output-dir", type=str, default="plots")
    p.add_argument("--dpi", type=int, default=300)
    p.add_argument("--no-show", action="store_true")
    args = p.parse_args()

    overrides = {k: v for k, v in dict(
        period=args.period, k=args.k, gamma=args.gamma, phi=args.phi,
        eccentricity=args.ecc, omega=args.omega).items() if v is not None}

    sel_lc = ([s.strip() for s in args.lightcurves.split(",")]
              if args.lightcurves else None)

    fs = None
    if args.figsize:
        try:
            w, h = args.figsize.split(",")
            fs = (float(w), float(h))
        except ValueError:
            sys.exit("--figsize expects 'width,height'")
    fsd = None
    if any([args.fontsize, args.label_fontsize,
            args.legend_fontsize, args.tick_fontsize]):
        fsd = {}
        if args.fontsize:
            fsd["general"] = args.fontsize
        if args.label_fontsize:
            fsd["labels"] = args.label_fontsize
        if args.legend_fontsize:
            fsd["legend"] = args.legend_fontsize
        if args.tick_fontsize:
            fsd["ticks"] = args.tick_fontsize

    conn = ad.connect(args.db)
    star = ad.find_star(conn, args.star)
    plot_rv_and_lightcurves(
        star, conn, fit_overrides=overrides,
        show_lightcurves=not args.no_lightcurves,
        selected_lightcurves=sel_lc,
        show_lc_fits=not args.no_lc_fits,
        lc_fit_shift=parse_lc_fit_shift(args.lc_fit_shift),
        lc_bins=args.lc_bins,
        binning_config=parse_binning_config(args.binning),
        show_rv_legend=not args.no_rv_legend,
        show_lc_legend=not args.no_lc_legend,
        legend_loc=args.legend_loc, figsize=fs, fontsize=fsd,
        output_path=args.output, output_dir=args.output_dir,
        dpi=args.dpi, show=not args.no_show)
