# Tutorial: One Star, Start to Finish

This tutorial walks through the full life of a single star in ASTRA: adding
it, attaching spectra, fitting them, fetching photometry, solving the orbit,
and fitting the SED. At the end you have a fully characterized system.

You need an open project (see [Getting Started](../getting-started.md)) and,
for the fitting steps, configured model grids and backends (see the setup
sections of the [workflow guides](../workflows/sed-spectral.md)).

<div class="media-placeholder">
  <div class="mp-icon">🎬</div>
  <div class="mp-caption">Video: the complete pipeline of this tutorial, condensed</div>
  <div class="mp-file">assets/videos/tutorial-single-star.webm</div>
</div>

## Step 1: Add the star

1. Open **Stars → Add Star…**
2. Enter any identifier you have: an alias, a Gaia DR3 source ID, a TIC
   number, a J-name, or RA/Dec coordinates.
3. Click **Resolve from Gaia / SIMBAD**. ASTRA fills in the astrometry and
   Gaia photometry, and can also query SIMBAD for bibliography codes.
4. Confirm. The star appears in the star table.

Double-click the new row to open its **Star Detail** window. The Summary
panel already shows the catalog data; everything else is still empty.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the freshly added star's detail window</div>
  <div class="mp-file">assets/images/tutorial-star-fresh.png</div>
</div>

## Step 2: Attach spectra

1. In the detail window sidebar, click **View / Fit Spectra** to open the
   **Spectral Analysis** window.
2. Click **Add Spectra…** and select your spectrum files. ASTRA detects the
   instrument and mode automatically; review the table (file, instrument,
   mode, scale, time) and confirm.
3. The spectra appear in the tree on the left and can be inspected in the
   **Browse** tab.

!!! tip
    If an instrument is not recognized, define it first under
    **Data → Instruments…**, then use **Re-detect instruments/modes**.

## Step 3: Fit the spectra

1. Switch to the **Fit Setup** tab.
2. Under *Stellar components*, click **+ Add component** and set the starting
   parameters; freeze anything you want held fixed.
3. Select the elements to include, tick the spectra to fit, and set the
   wavelength range and any ignore regions.
4. Leave **Fit one spectrum at a time** unticked to fit all the marked
   spectra together, or tick it to fit them one after another, each as its
   own independent fit. For a sequence, **Start each fit from the previous
   fit's result** usually converges faster, since the spectra of one star
   share a solution. Coming back later with new spectra? Tick **Skip spectra
   that already have a best fit** so the run only covers the ones still
   missing one.
5. Click **▶ Run Fit** and watch the progress dialog. When it finishes, the
   fit shows up in the tree; mark the good one as **Best**.

Each fitted spectrum contributes a radial-velocity point, so after this step
the **Radial Velocity** panel starts filling in.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: a finished spectral fit in the Browse tab</div>
  <div class="mp-file">assets/images/tutorial-spectral-fit.png</div>
</div>

## Step 4: Fetch light curves

1. In the detail window sidebar, click **Fetch / Fit LC** and open the
   **Fetch** tab.
2. Leave the default sources (**TESS**, **ZTF**, **ATLAS**, **Gaia**) enabled
   and click **Fetch**. Progress streams into the dialog; ZTF asks for an
   IRSA login and ATLAS needs a token, and declined sources are simply
   skipped.
3. When the fetch finishes, inspect the data in the **Viewer** tab.

## Step 5: Find the photometric period

1. In the same dialog, open the **Periodogram** tab and compute
   periodograms for the fetched light curves.
2. Pick the physical peak, then use **Set as Best Period** in the Viewer tab.
   The Light Curves panel of the detail window now folds on that period.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: a folded light curve after setting the best period</div>
  <div class="mp-file">assets/images/tutorial-folded-lc.png</div>
</div>

## Step 6: Solve the spectroscopic orbit

1. Back in the detail window, click **View / Adjust RV** to open the
   **RV Inspector**. Check the RV Points table; flag any bad epochs.
2. Click **➕ Add Fit**. For a first attempt, the **RV-MCMC** tab is the most
   robust: set a sensible period search range, enable the
   *Light-curve periodogram prior* if Step 5 found a period, and click
   **Run MCMC…**
3. In the **RV-MCMC results** dialog, inspect the candidate solutions and add
   the best one.
4. Back in the Inspector, select the solution and click **Set as Best**. The
   folded RV curve appears in the Radial Velocity panel.

If the MCMC struggles, try the **RV Periodogram** or **From Photometry** tabs
instead; see [Radial Velocities](../workflows/radial-velocity.md).

## Step 7: Fit the SED

1. Click **View / Fit SED** in the sidebar.
2. Review the photometry under *Photometry Points*.
3. In *New Fit Configuration*, pick your model grid (enable a second
   component grid for composite SEDs), decide whether to fix the distance to
   the Gaia parallax, choose fit parameters, and click **▶ Run Fit**.
4. Promote the result with **★ Set as Best Fit**. Mass, radius, and
   luminosity now appear in the Summary panel and the SED table columns.

## Step 8: Optional extras

- **Fit the light curve**: the **Fit** tab of the light-curve dialog starts
  the [Light-Curve Fit wizard](../workflows/light-curves.md) to constrain the
  inclination and component parameters.
- **Kinematics**: **Galactic Orbit** integrates the star's orbit in the
  Galactic potential and classifies its population; **Show CMD** places it on
  a Gaia color-magnitude diagram.
- **Observability**: plan follow-up observations for your instruments.

## Step 9: Review and share

The Summary panel now aggregates everything: atmospheric parameters, the
orbital solution, SED radii and masses, companion constraints, and
kinematics. Click **Share Star** to export the whole thing as a `.astra`
package a colleague can drop into their own ASTRA.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the fully populated Summary panel at the end of the tutorial</div>
  <div class="mp-file">assets/images/tutorial-summary-final.png</div>
</div>
