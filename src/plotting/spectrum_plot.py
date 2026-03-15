#!/usr/bin/env python3
"""
Spectrum plot with optional model overlay and residual sub-panel.

Expected JSON payload:
{
    "wavelengths": [...],
    "fluxes": [...],
    "errors": [...],          // optional
    "model": {                // optional
        "wavelengths": [...],
        "fluxes": [...]
    },
    "renormalize": <bool>,
    "show_residuals": <bool>,
    "info_text": "..."
}
"""

import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec

sys.path.insert(0, __import__('os').path.dirname(__file__))
from plot_base import parse_args, load_payload, setup_style, output_figure


def interpolate_model(model_wl, model_flux, target_wl):
    return np.interp(target_wl, model_wl, model_flux,
                     left=np.nan, right=np.nan)


def renorm_factor(data, model):
    mask = np.isfinite(data) & np.isfinite(model)
    d, m = data[mask], model[mask]
    denom = np.dot(m, m)
    return np.dot(d, m) / denom if denom > 0 else 1.0


def main():
    args = parse_args()
    payload = load_payload(args.input)
    rp = setup_style(payload)

    wl   = np.array(payload['wavelengths'], dtype=float)
    flux = np.array(payload['fluxes'], dtype=float)
    errors = np.array(payload.get('errors', []), dtype=float) if payload.get('errors') else None

    model_data = payload.get('model', None)
    do_renorm  = payload.get('renormalize', False)
    show_resid = payload.get('show_residuals', False) and model_data is not None

    data_color = '#1e1e1e' if not rp['dark'] else '#cccccc'
    model_color = '#dc3232'

    if show_resid:
        fig = plt.figure(figsize=(rp['width'], rp['height']))
        gs = GridSpec(3, 1, height_ratios=[3, 1, 0], hspace=0.05, figure=fig)
        ax_main = fig.add_subplot(gs[0])
        ax_resid = fig.add_subplot(gs[1], sharex=ax_main)
    else:
        fig, ax_main = plt.subplots(figsize=(rp['width'], rp['height']))
        ax_resid = None

    # ── Error band ──
    if errors is not None and len(errors) == len(wl):
        ax_main.fill_between(wl, flux - errors, flux + errors,
                              color='gray', alpha=0.15, zorder=1)

    # ── Observed spectrum ──
    ax_main.plot(wl, flux, '-', color=data_color, lw=0.8, zorder=2,
                 label='Observed')

    # ── Model overlay ──
    residual_wl = None
    residual_val = None

    if model_data and 'wavelengths' in model_data:
        m_wl   = np.array(model_data['wavelengths'], dtype=float)
        m_flux = np.array(model_data['fluxes'], dtype=float)

        model_on_data = interpolate_model(m_wl, m_flux, wl)

        c = 1.0
        if do_renorm:
            c = renorm_factor(flux, model_on_data)

        ax_main.plot(m_wl, m_flux * c, '-', color=model_color, lw=1.2,
                     alpha=0.85, zorder=3, label='Model')

        if show_resid:
            mask = np.isfinite(model_on_data)
            residual_wl = wl[mask]
            residual_val = flux[mask] - model_on_data[mask] * c

    ax_main.set_ylabel('Normalized Flux')
    ax_main.grid(True, alpha=0.2)

    if model_data:
        ax_main.legend(loc='best', framealpha=0.7)

    # ── Residual panel ──
    if ax_resid is not None and residual_wl is not None:
        ax_resid.plot(residual_wl, residual_val, '-', color=data_color, lw=0.7)
        ax_resid.axhline(0, color='gray', ls='--', lw=0.8)
        ax_resid.set_ylabel('Residual')
        ax_resid.set_xlabel('Wavelength [Å]')
        ax_resid.grid(True, alpha=0.2)
        plt.setp(ax_main.get_xticklabels(), visible=False)
    else:
        ax_main.set_xlabel('Wavelength [Å]')

    fig.tight_layout()
    output_figure(fig, payload, args)


if __name__ == '__main__':
    main()