# ASTRA - Stellar Astrophysics Data Manager

ASTRA is a modern Qt6 application for managing and analyzing stellar astrophysics data. It provides comprehensive tools for handling large catalogs of stars, spectroscopic data, photometry, and radial velocity analysis.

## Features

- **Project Management**: Organize stellar data into projects with customizable metadata
- **Large Dataset Support**: Efficiently handle 17,000+ stellar objects and 76,000+ spectra
- **Multi-epoch Spectroscopy**: Manage and analyze time-series spectroscopic observations
- **Photometry Analysis**: Support for single measurements and time-series lightcurves
- **Radial Velocity Analysis**: Track and analyze RV variations across multiple epochs
- **Interactive Visualizations**: Qt Charts-based plots for RV curves, spectra, and SEDs
- **Theme Support**: Catppuccin Light and Dark themes for comfortable viewing
- **Cross-platform**: Native support for Linux (Wayland/X11), Windows, and macOS

## Data Capabilities

### Stellar Parameters
- **Identification**: Common names, Gaia DR3 source IDs, TESS IDs, J-names
- **Astrometry**: RA/DEC, proper motions, parallax with uncertainties
- **Photometry**: Multi-band measurements (Gaia, SDSS, etc.)
- **Spectroscopy**: Teff, log g, helium abundance, spectral classification
- **Radial Velocities**: Multi-epoch RV measurements and variability analysis

### Supported Data Sources
- Gaia DR3 catalog data
- SDSS spectroscopic observations
- Custom CSV imports
- FITS spectral files
- Time-series photometry from TESS, ATLAS, Gaia, ZTF, BlackGEM

## Building from Source

### Prerequisites

- Qt6 (6.2 or later) with modules: Core, Widgets, Charts, SQL
- CMake 3.16 or later
- C++17 compatible compiler
- SQLite3

### Build Instructions

```bash
# Clone the repository
git clone https://github.com/yourusername/ASTRA.git
cd ASTRA

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build
make -j$(nproc)

# Run the application
./ASTRA
```

### Windows Build

```cmd
# Using Qt Creator or Visual Studio with CMake support
# Or from command line with appropriate Qt and compiler paths:
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

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
├── data/             # Example data files
└── CLAUDE.md         # AI assistant instructions
```

## Usage

### Creating a Project

1. Launch ASTRA
2. Click "Create New Project" on the landing page
3. Enter project name and description
4. Begin adding stars through individual entry or CSV import

### Importing Data

ASTRA supports importing stellar catalogs from CSV files with flexible column mapping:
- Navigate to Stars → Import Stars
- Select your CSV file
- Map columns to ASTRA fields
- Review and confirm import

### Viewing Star Details

Double-click any star in the project table to open the detailed view featuring:
- Radial velocity curve with broken time axis
- Observability plots
- Stacked spectra visualization
- Bibliography with clickable ADS links
- Quick access to SIMBAD

## Data Format Examples

### Object Catalog CSV
```csv
name,source_id,ra,dec,gmag,bp_rp,spec_class,teff
HD 12345,1234567890,123.456,45.678,10.5,0.65,sdB,28000
```

### Spectrum Files
ASTRA expects spectrum files in three-column ASCII format:
```
wavelength(Å)  flux  flux_error
3000.0        1.234  0.012
3001.0        1.245  0.013
...
```

## Development

### Running Tests
```bash
cd build
ctest
# or
./tests/astra_tests
```

### Contributing

We welcome contributions! Please:
1. Fork the repository
2. Create a feature branch
3. Commit your changes
4. Push to the branch
5. Open a Pull Request

## License

[To be determined]

## Acknowledgments

This application is designed for the analysis of stellar astrophysics data, particularly focusing on hot subdwarf stars and radial velocity variable detection.

## Contact

[Your contact information]

## References

When using ASTRA for scientific work, please cite:
[Citation information to be added]