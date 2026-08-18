# Tutorial: Complex Plots

ASTRA's plot tool (**Analysis → Create Plot…**) builds publication-ready
scatter plots, histograms, and sky maps over any numeric fields of your
catalog. This tutorial builds three example plots of increasing complexity.

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: building the Kiel diagram from this tutorial</div>
  <div class="mp-file">assets/videos/tutorial-plotting.webm</div>
</div>

## Example 1: A Kiel diagram (Teff vs. log g)

1. Open **Analysis → Create Plot…** and choose **Scatter plot**.
2. In the **Data** panel, pick your source: all stars, the currently filtered
   set, or the current selection. Enable *Open star details on click* so each
   plotted point links back to its star.
3. In the **Axes** panel:
     - X axis: `Teff`, enable **log** and **invert** (hot stars on the left,
       as convention demands).
     - Y axis: `log g`, enable **invert**.
     - Set **X error** and **Y error** to the corresponding error columns to
       draw error bars.
4. In the **Style** panel, adjust marker shape and size, tick density, fonts,
   and give the plot a title.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the finished Kiel diagram</div>
  <div class="mp-file">assets/images/tutorial-kiel.png</div>
</div>

## Example 2: Adding dimensions with color and size

A scatter plot can encode two more quantities:

1. Keep the Kiel diagram from Example 1.
2. In the **Axes** panel, set **Color by** to `BP-RP` so each point is
   colored by Gaia color.
3. Set **Size by** to `G mag` so brighter stars draw bigger markers.
4. For dense samples, the **Style** panel offers *Cull dense points* for fast
   drafts; switch back to *Draw all points* for the final export.
5. Add a **Running median** trend line in the Axes panel if you want the
   population's ridge line.

## Example 3: A sky map

1. Choose **Sky map** as the plot type.
2. Pick the sample as before; the stars are drawn on a projected sky grid.
3. Use **Color by** to encode a quantity, for example the radial velocity or
   parallax, across the sky.
4. Limit the plotted sample beforehand with table filters; the plot's
   *Filtered stars* source honors whatever filter set is active, including
   the observability filter.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: a sky map colored by radial velocity</div>
  <div class="mp-file">assets/images/tutorial-skymap.png</div>
</div>

## Histograms

Choose **Histogram** to look at distributions, for example orbital periods or
effective temperatures of your sample. Log axes work here too, which is
usually what you want for periods.

## Saving and exporting

- **Presets…** stores a complete plot configuration under a name, so
  recurring plots (the group's standard Kiel diagram) are one click away.
- **Export…** writes PDF, SVG, or PNG; an options dialog controls the output
  size. Vector formats (PDF/SVG) are the right choice for papers.

!!! tip "Per-star plots"
    Plots of a single star's data, such as folded RV curves or periodograms,
    live in the star detail window and its dialogs instead. The
    [Python scripts](../reference/scripts.md) can reproduce some of them
    outside the app for custom styling.
