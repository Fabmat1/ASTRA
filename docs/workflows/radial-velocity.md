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

## Importing an RV table

The wizard's **From a single RV table** mode reads one table holding all
epochs for many stars and attaches each row to a star already in the project.
Columns (identifier, timestamp, RV, RV error, systematic error) are detected
from the header and can be overridden.

Rows are matched to stars in one of three ways, chosen with the drop-down next
to the identifier column:

- **Gaia Source ID** - exact match, with or without the `Gaia DR3` prefix.
- **Alias/Name** - matched after normalisation, so `* alf Lac`, `Alf Lac` and
  `HD   1185` find the same star as `alf Lac` and `HD 1185`.
- **Coordinates (RA + Dec)** - a cone search around each row's position, using
  a separate RA and Dec column (decimal degrees) and a configurable match
  radius. The default 3 arcsec covers tables whose coordinates are rounded to
  whole seconds of time, which can sit ~2 arcsec from the Gaia position of the
  same star.

Coordinate matching is the most reliable option for observing logs, where the
same star often appears under several spellings, or none at all. Rows that
resolve to the same star are merged into one RV curve regardless of the label
they carry, and epochs already present from an earlier import of the same
table are not duplicated. Rows that match no star are skipped and listed in
the preview under **Unmatched rows**, together with the identifiers that
failed - usually a sign that the star table has not been imported yet or that
the match radius is too tight.

### One row per epoch, or one row per star

Both layouts work. A cell may hold a whole series instead of a single value,
written the way pandas writes a list of floats:

```
main_id,ra,dec,bjd_list,vrad_list,vrad_err_list,systematic_rv_err,component
10AQR,315.136083,-5.477333,"[2460922.4842, 2460927.3369]","[3.0365, 1.6404]","[0.26, 0.26]",0,1
```

Each element becomes its own RV point. The timestamp and RV series must hold
the same number of values; a column holding a single value (the systematic
error and component above) applies to every epoch of that row. Values may also
be separated by semicolons or spaces, with or without brackets.

Only the timestamp and the RV are required. An unset or empty error column
means 0, an unset component means the primary, and neither drops the row.

Matched rows that still produce no point are listed in the preview under
**Matched rows without a new point**, with the reason (an epoch already
imported, an unreadable cell, or a series length mismatch) and examples naming
the offending cell.

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
