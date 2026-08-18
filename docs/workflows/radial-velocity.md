# Radial Velocities

Measuring, managing, and fitting radial velocities to solve spectroscopic
orbits.

!!! warning "Work in progress"
    This page is a structured outline; detailed steps, screenshots, and clips
    are still being added.

## Where RVs come from

RV points enter ASTRA in several ways:

- extracted from **spectral fits** (each fitted spectrum contributes an RV) -
  see [SED & Spectral Fitting](sed-spectral.md),
- imported via the [Star Import Wizard](catalog-management.md)'s RV page,
- imported per star from CSV (**Import from CSV…** in the RV Inspector, with
  column mapping and automatic barycentric time conversion), or
- entered manually (**Add manual point…**).

## The RV Inspector

Open a star's detail window and click **View / Adjust RV**. The
**RV Inspector** shows:

- the **RV Solutions** list - every orbital solution stored for the star,
  with **Set as Best** to choose the adopted one,
- **Manually Adjust Curve** - hand-tune period, amplitude, phase/T₀,
  eccentricity of a solution,
- the **RV Points** table - inspect, flag, add, import, or remove individual
  measurements.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the RV Inspector with a folded orbit solution</div>
  <div class="mp-file">assets/images/rv-inspector.png</div>
</div>

## Fitting an orbit - Add RV solution

Click **➕ Add Fit** in the RV Inspector. The **Add RV solution** dialog
offers five approaches, in separate tabs:

| Tab | Method |
|---|---|
| **χ² Landscape** | Brute-force χ² scan over period |
| **RV-MCMC** | Full MCMC orbit sampling with configurable search range, sampler settings, parameter bounds, and an optional light-curve periodogram prior. Results open in the **RV-MCMC results** dialog, where candidate solutions (or custom period regions) are added to the star. |
| **From Photometry** | Seed the orbit from a photometric period, including the ellipsoidal case (search at 2·P_phot) |
| **RV Periodogram** | Compute an RV periodogram (optionally multiplied with light-curve periodograms), detect peaks, and fit selected peaks with Levenberg–Marquardt |
| **Manual** | Enter orbital elements directly |

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: an RV-MCMC run and adopting a solution</div>
  <div class="mp-file">assets/videos/rv-mcmc.webm</div>
</div>
<!-- TODO: recommended settings and caveats for each method (sampler choice,
     when to use the LC prior, ellipsoidal ambiguity). -->

## Survey-level detectability

**Analysis → RV Detectability…** Monte-Carlos the SB1 detection probability
as a function of orbital period for all/filtered/selected stars, given a mass
model (fixed M₂ or mass ratio q). Results export to CSV or a plot.

## Related panels

The **Radial Velocity** panel in the star detail window shows the adopted
solution folded or as a timeline; the `scripts/plot_curve.py`
[helper script](../reference/scripts.md) reproduces the folded plot outside
the app.
