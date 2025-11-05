# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ASTRA is a modern Qt6 application (Wayland native and compatible with Windows/Mac/Linux) for managing and analyzing stellar astrophysics data. The application handles large catalogs of stars (~17k+ objects), spectroscopic data (~76k+ spectra), and associated analysis results.

## Technical Stack

- **Framework**: Qt6 (targeting Qt 6.2+)
- **Language**: C++17 or later
- **Build System**: CMake
- **Database**: SQLite for local project storage
- **Plotting**: Qt Charts or QCustomPlot for interactive visualizations
- **Themes**: Catppuccin Light and Dark variants
- **Platform Support**: Linux (Wayland/X11), Windows, macOS

## Project Structure

```
ASTRA/
├── src/              # Source files
│   ├── models/       # Data models (Star, Photometry, Spectrum, etc.)
│   ├── views/        # UI components
│   ├── controllers/  # Business logic
│   └── utils/        # Helper functions
├── tests/            # Unit tests
├── resources/        # Icons, themes, UI files
└── data/             # Example data files
```

## Architecture Principles

- **Model-View-Controller**: Separate data models from UI presentation
- **Asynchronous Loading**: Load expensive operations (plots, large datasets) asynchronously to maintain UI responsiveness
- **Performance**: Optimize for handling 17k+ catalog entries and 76k+ spectral files
- **Error Handling**: Gracefully handle missing data, corrupt files, and I/O errors with clear UI feedback
- **Version Control**: Use Git for all development

### Landing/Project Selection Page

There is a nice landing page giving an overview of available Projects in a grid layout - each project represents at its core a table of stars with different parameters. When none is present yet there should be the option to create one. Projects should also have the option to be modified in their Title, description or image to represent them here, as well as having the option to be removed. Clicking on a Project opens the main view, being the table of stars, and their parameters. 

#### Project Objects

Projects are represented in the backend as objects with a project name and metadata as well as member values and methods pointing to the data the Project contains.

### Project Page 

Clicking on a Project opens this main and most important view, with the table representing the stars int the project. The first columns are the name of the star, as well as the Gaia DR3 source ID for the object. After that follow relevant columns, which can represent many different parameters about these stars like temperature, position or parameters related to radial velocity, and the User should be able to control on a per-Project basis which columns are shown here.
There is a Menu bar at the top of the page with which the User can control and perform operations on the stars in the project, such as adding stars individually or with table imports, and also modifying them or removing them, or creating plots. Double Clicking on any row or right clicking on a row and selecting "View Detail Window" should take the user to a new window for the selected star, containing detailed information. This window is described further below.

#### Star Object Backend

Each star should internally be represented as an object. It should always possess all fields that I specify here, though they do not all need to be filled for every star:

Identifying fields:
  - `alias` Common Name
  - `source_id` Gaia DR3 source id  
  - `tic` TESS input catalog id
  - `jname` J-Name (e.g. J011927.36+031328.8)

Astrometric Fields:
  - `ra` RA (J2000)
  - `dec` DEC (J2000)
  - `pmra` Proper Motion RA component
  - `pmdec` Proper Motion DEC component
  - `e_pmra` Proper Motion RA component uncertainty
  - `e_pmdec` Proper Motion DEC component uncertainty
  - `plx` Parallax (mas)
  - `e_plx` Parallax uncertainty (mas)
  - `pmra_pmdec_corr` PM RA - PM DEC correlation
  - `plx_pmdec_corr` Parallax - PM DEC correlation
  - `plx_pmra_corr` Parallax - PM RA correlation

Photometric Fields:
  **There are many different sources for photometry. All photometry should be stored in a "photometry" object, containing a list of photometric measurements for the star, their uncertainties and identifiers. This should also later store SEDs fitted to that photometry (The contents of the SED objects will be discussed later). The user should be able to expose any of them to the table view. This is an example of commonly available photometry from gaia for all stars:**
  - `gmag` Gaia G band magnitude
  - `e_gmag` Gaia G band magnitude uncertainty
  - `bp` Gaia BP band magnitude 
  - `e_bp` Gaia BP band magnitude uncertainty
  - `rp` Gaia RP band magnitude 
  - `e_rp` Gaia RP band magnitude uncertainty
  - `bp_rp` Gaia BP-RP band magnitude difference (color)

Spectroscopic Fields:
  **Each star can have none, one, or many spectroscopic epochs from different instruments associated with it. As with the photometry, the info about these spectra should be contained in an object containing the list of spectra, which instruments they are from and some metadata that will be discussed later. Like in the photometry, spectra can also have models fitted to them so each spectral object should also point to the model fits available for it. The star object should point to some fields from the spectroscopy so they can be exposed in the table:**
  - `spec_class` Spectral classification of the star
  - `teff` Effective Temperature
  - `e_teff` Effective Temperature uncertainty
  - `logg` Surface Gravity
  - `e_logg` Surface Gravity uncertainty
  - `he` Log Helium content / Suns
  - `e_he` Log Helium content / Suns uncertainty

Radial Velocity Fields:
  **Radial Velocity information is also available often for stars with spectral epochs. The individual radial velocities should be stored with the model fits in the spectral model objects associated with the spectrum objects. In the table, radial velocity metadata should be displayed, e.g.:**
  - `logp` $\log p$, logarithm of the false-detection probability signalling radial velocity variability (e.g. logp = -4 -> 0.1% false-detection probability)
  - `deltaRV` $\delta \rm{RV}_\max$, The peak-to-peak radial velocity amplitude (if multiple epochs are available)
  - `e_deltaRV` The delta RV uncertainty
  - `RV_avg` Average Radial Velocity across Epochs
  - `e_RV_avg` Its uncertainty
  - `RV_med` Median Radial Velocity across Epochs
  - `e_RV_avg` Its uncertainty

Metadata/Various:
  - `bibcodes` should not be displayed in the table, but each star should have info of all the bibcodes of papers it is mentioned in

#### Photometry object backend.

The photometry for any star should be stored in this object. This contains a) single photometric measurements from different instruments. These have a magnitude with an uncertainty and a flux with an uncertainty. b) Lightcurve objects. These come from TESS, ATLAS, Gaia, ZTF or BlackGEM and hold many hundreds to many thousand individual points with a flux, a timepoint, a flux uncertainty and optionally a filter identifier string. The individual photometry points should also have SED objects associated with them, which are fits to the photometry curve. These objects contain a model flux and wavelengths, and fit parameters such as the stars angular size, radius etc. THe time-series photometry also can have lightcurve fits with model fluxes and timepoints and also stellar parameters. These fits, wheter SED or time-series model should have a creation date and a flag where i can set them to be the best fit for this star or not so the star can source their fit parameters. I have external scripts that handle these data, and will explain later how to implement them with ASTRA, for now just know these structures.

#### Spectrum object backend.

Each star also holds multiple spectrum objects. Each spectrum object points to a `file` containing the actual spectrum, but also holds wavelength (in angstrom), flux and flux error values as well as an MJD and BJD of when the spectrum was taken (mid exposure) and the exposure time. There is one or multiple spectral fit objects associated with each spectrum, each containing a creation date, model identifier, model flux and model wavelengths, effective temperature, logg, he, vsini, radial velocity all with uncertainties and finally a flag selecting it as the preferred model fit for this particular spectrum or not.

### Detail View

The detail view for each star looks like this: Prominently in the upper left edge is a plot of the measured radial velocity datapoints for that star, with the BJD on the x axis. Since the epochs can sometimes be spaced apart very far in time, the plot has to have a broken time axis to still be able to show all the detail. When we get to coding this, please ask about my previous code i had written for this purpose to see how to implement this. Below the RV curve is a list of the bibcodes associated with the star as well as a nice looking small table containing copyable information about the ra dex spec_class, basically everything that is also in the row in the project page view. The bibcodes should be clickable and open the default browser to the associated NASA/ADS page (e.g. https://ui.adsabs.harvard.edu/abs/2022A%26A...662A..40C/abstract). Up top to the right of the rv curve plot should be a configurable plot showing the observability of the object where the user can specify a night/telescope to see when the star is observable. I have a previous code for this too, ask for it when we get to coding this. Below this plot, so in the bottom right corner should be the final plot, a plot showing all spectra for the star stacked above each other (i.e. normalized spectra each with an offset of .5 to each other) and also showing the best fit model, if any. All plots should be interactive on this page and be loaded asynchronously since they can get expensive to create if a star has many different spectra. Finally, on the right side of the page is a stack of buttons. The first one, "Show in SIMBAD" opens the users browser to the SIMBAD website for the star based on its gaia id (e.g. https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+1095358811015024384&submit=submit+id). The second one "View Spectra" Takes the user to a view where they can see and manipulate the spectra available for this star. The third "View SED" takes the user to a view for the non time-series photometry for the star, where they can also fit SEDs. The next one "Fit RV curve" takes the user to a view where they can fit the radial velocity information. The last one "View CMD" shows the stars position in the color-magnitude diagram compared to all other stars in the project.

All these functionalities need to be implemented as skeletons on your first run, we fully implement them one-by-one later.


## Example Data Structure

I provide some example tables and spectral fits I did externally in the project folder. We will need some functionality to import those.

### Primary Data Files

**object_catalogue.csv** (~17,682 entries)
- Contains stellar object metadata from Gaia and SDSS surveys
- Key columns: `name`, `source_id`, `ra`, `dec`, `file`, `SPEC_CLASS`, `bp_rp`, `gmag`, `nspec`, proper motions, parallax
- Each row represents a spectrum observation (objects can have multiple spectra)
- Spectral classifications include: sdB (subdwarf B), He-sdO, sdOB, sdB+MS (binary systems), and unknown

**result_parameters.csv** (~8,009 entries)
- Contains derived RV analysis results for objects with multiple spectra
- Key columns: `source_id`, `alias`, `ra`, `dec`, `logp`, `deltaRV`, `RVavg`, `Nspec`, `bibcodes`, `associated_files`, `observability`, `known_category`, `flags`, `spatial_vels`
- `logp=-500.0` indicates significant RV variations
- `flags` column includes markers like 'rvv-detection' for RV variable candidates
- Objects in this file have undergone RV variability analysis

### Spectral Data Directory

**spectra_processed/** (symlink to `/home/fabian/RVVD/spectra_processed/`)
- Contains ~76,012 processed spectrum files
- File format: Pairs of `.txt` and `_mjd.txt` files for each spectrum
- Spectrum `.txt` files: Three-column ASCII format (wavelength, flux, uncertainty)
  - Column 1: Wavelength in Ångströms
  - Column 2: Flux values
  - Column 3: Flux uncertainties
- MJD `.txt` files: Single value containing Modified Julian Date of observation

## Data Relationships

- `object_catalogue.csv` entries link to spectrum files via the `file` column
- `result_parameters.csv` entries reference multiple spectra via `associated_files` (semicolon-separated)
- Join between catalogues using `source_id` (Gaia DR3 source identifier)
- Objects with `Nspec > 1` in result_parameters have multi-epoch observations suitable for RV variability analysis

## General Notes

- Please create a git repository and use version control in this project.