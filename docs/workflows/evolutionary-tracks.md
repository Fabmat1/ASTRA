# Evolutionary Tracks

Masses and ages from evolutionary tracks: ASTRA places a star's Teff and
luminosity (or log g) on a set of tracks, interpolates between them and
samples which track, and which point along it, fit the star. The result is
whatever the track set provides: the mass (for two-parameter sets such as
the hot-subdwarf ones, core and envelope mass separately), the age since the
start of the track, the lifetime, and where the tracks carry the central
helium abundance, the age at core-helium exhaustion and the central helium
fraction. Every value comes with asymmetric errors.

The fitting is ASTRA's port of H. Dawson's *sdfit* (version 1.0.0): the
same interpolation and the same MCMC, so a fit in ASTRA gives sdfit's
numbers.

## The tracks

ASTRA does not ship track sets; the hot-subdwarf sets sdfit uses are
unpublished work of colleagues. Each user exports them once from a checkout
of sdfit into a folder of their choice and points ASTRA at that folder:

```bash
python3 scripts/export_tracks.py --sdfit /path/to/hot_subdwarf_track_interpolation \
                                 --outdir /path/to/tracks
```

Run it with the Python environment sdfit itself uses (numpy, scipy,
astropy). It writes one file per family (`han`, `nicolas`, `dorman`,
`driebe`), or only the families you name after the options. Han can also be
used from its original FITS file (`han_tracks_Z0_extended_925.fits.gz`);
put it into the same folder.

Then set the folder under **Settings / Evolutionary Tracks**. The page lists
the families it found and says which files it could not read. The folder is
a per-machine setting and is not synced.

ASTRA knows these four hot-subdwarf families and how sdfit selects their
tracks:

| Family | Selection of tracks | Mass axes |
|---|---|---|
| Han | tick core masses (at least two) | core + envelope |
| Nicolas | a mass range | core + envelope |
| Dorman | compositions and the number of thinnest envelopes per composition (default: all compositions, 3 envelopes, as sdfit) | core + envelope |
| Driebe | a mass range | total mass only |

A subset is triangulated afresh, exactly as sdfit triangulates the tracks it
loads. If a selection leaves too few tracks, ASTRA says so and suggests a
wider range.

## Fitting one star

Open a star and press **Evol. Tracks** in the sidebar.

- **Inputs.** The star's adopted spectroscopic Teff and log g, and the SED
  luminosity, radius and mass. For a composite SED, the hotter component is
  used; the dialog says so, and you can pick the other one. Without a
  spectroscopic Teff, the SED temperature is used. **Edit values** lets you
  type in other numbers; a fit made from typed-in values is never marked
  stale.
- **Fit plane.** *Automatic* fits Teff + L when the star has an SED
  luminosity and Teff + log g otherwise. Errors are converted to dex, using
  both sides of asymmetric errors.
- **Families.** Tick the families to fit and drag them into your order of
  preference. Select a family to edit its subset; the plot shows its tracks
  with the star.
- **Advanced.** The prior along the track (flat, uniform in age, or sdfit's
  sdB/sdO priors), a Gaussian prior on the core mass from the SED mass, the
  sampler size (default as sdfit: 1000 walkers, 800 burn-in, 1000 steps,
  about 0.4 s per star and core), the seed, and for Han bilinear or scipy's
  triangle interpolation.

**Run** fits every ticked family and stores one fit per family. Selecting a
stored fit draws its posterior: the tracks with the posterior samples and the
star, and a histogram per quantity with the median and the 68 % interval.
The samples are not stored; ASTRA re-runs the fit with its stored seed and
settings, which gives the same samples while the track file is unchanged
(and warns if it changed).

### Which fit counts

A star can have several fits; one is *adopted*, and its values are the
star's track values in the summary and the table.

- After a run, the fit of the first ticked family is adopted.
- Ticking **Adopted** on a fit makes it the star's fit until you untick it;
  later runs then add fits but leave your pick in place.
- Unticking hands the choice back to the latest run.

## Fitting many stars

**Analysis / Fit Evolutionary Tracks...** fits all, the filtered or the
selected stars of the project with the same settings. By default it skips
stars whose adopted fit is up to date and adopts the new fits (a fit you
picked by hand stays adopted). The report afterwards lists the stars that
were not fitted, grouped by reason (no Teff, Teff is a limit, no error, a
family not in the tracks folder, ...).

## Reading the results

The star summary shows an **Evolutionary Tracks** section with the masses,
the age, the lifetime, the age at core-helium exhaustion, the remaining time
(lifetime minus age) and the central helium fraction. Notes appear only when
they matter:

- **Stale:** Teff, log g, L or R changed since the fit. Run it again.
- **Outside the tracks:** the star lies outside the region the tracks cover,
  so the values are extrapolated.
- **Bounded:** a posterior runs into a limit (an edge of the tracks, zero
  age) instead of tailing off; its median and interval describe it badly.
- **Core-helium exhaustion:** a large part of the posterior lies beyond
  core-helium exhaustion, so solutions before and after it are both
  plausible (for hot subdwarfs: EHB or post-EHB).

All values are also table columns in the **Evolution** category (and the
*Evolution* column preset), and fields for plots and filters: `trk_core_mass`,
`trk_env_mass`, `trk_mass`, `trk_age`, `trk_age_max`, `trk_age_he0`,
`trk_he_c`, `trk_eep`, each with `trk_e_*` errors, plus `trk_family`,
`trk_plane`, `trk_stale`, `trk_in_region` and `trk_bounded`.

## AI agents

Agents have `run_track_fit` (a background job, like
`run_rv_mcmc`), `list_track_fits`, `set_adopted_track_fit` (undo with
`revert_change`) and `delete_track_fit`. See
[AI Agents (MCP)](../ai/agents.md).
