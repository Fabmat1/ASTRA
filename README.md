# ASTRA - Stellar Astrophysics Data Manager

ASTRA is a modern Qt6 application for managing and analyzing stellar astrophysics data. It provides comprehensive tools for handling large catalogs of stars, spectroscopic data, photometry, radial velocities, and SEDs.

## Installation

ASTRA can be installed by downloading the [latest AppImage release](https://github.com/Fabmat1/ASTRA/releases/latest) (recommended for most users), or built from source.

### Runtime Dependencies (AppImage users)

The AppImage bundles Qt and most libraries, but a few system packages must be installed separately:

**Debian / Ubuntu (22.04+):**
```bash
sudo apt install libfuse2 libopengl0 libegl1 libxkbcommon0 libgl1 \
                 libdbus-1-3 libfontconfig1 \
                 python3 python3-numpy gnuplot
```

**Arch Linux:**
```bash
sudo pacman -S fuse2 libglvnd libxkbcommon dbus fontconfig \
               python python-numpy gnuplot
```

Then:
```bash
chmod +x astra-0.1.0-x86_64.AppImage
./astra-0.1.0-x86_64.AppImage
```

---

### Building from Source

ASTRA is a CMake project requiring **Qt ≥ 6.10**, a Fortran compiler, and several scientific libraries.

#### 1. Install Build Dependencies

**Ubuntu 22.04+ / Debian 12+:**
```bash
sudo apt install build-essential cmake git wget pkg-config gfortran \
                 qt6-base-dev qt6-tools-dev libqt6opengl6-dev \
                 libqt6svg6-dev qt6-declarative-dev \
                 libgl1-mesa-dev libxkbcommon-dev libvulkan-dev \
                 libeigen3-dev libopenblas-dev liblapack-dev \
                 libboost-all-dev nlohmann-json3-dev \
                 libfftw3-dev libtbb-dev libcxxopts-dev \
                 libccfits-dev libcfitsio-dev \
                 python3-dev python3-numpy gnuplot
```

> **Installing Qt 6.10+ on Ubuntu 22.04 / 24.04:**
> ```bash
> pip install aqtinstall
> aqt install-qt linux desktop 6.11.1 linux_gcc_64 -O ~/Qt
>
> # Make CMake aware of it
> export CMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64:$CMAKE_PREFIX_PATH
> export PATH=$HOME/Qt/6.11.1/gcc_64/bin:$PATH
> ```
> Add the two `export` lines to your `~/.bashrc` if you want them persistent.

**Arch Linux:**
```bash
sudo pacman -S base-devel cmake git wget pkgconf gcc-fortran \
               qt6-base qt6-tools qt6-svg qt6-declarative \
               vulkan-headers vulkan-icd-loader \
               eigen openblas lapack boost nlohmann-json \
               fftw onetbb cxxopts \
               ccfits cfitsio \
               python python-numpy gnuplot
```

#### 2. Header-only Dependencies

`unordered_dense` is not packaged on most distros — install it manually:
```bash
sudo mkdir -p /usr/local/include/ankerl
sudo wget -O /usr/local/include/ankerl/unordered_dense.h \
  https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h
sudo ln -sf /usr/local/include/ankerl/unordered_dense.h /usr/local/include/unordered_dense.h
```

#### 3. Clone and Build

```bash
git clone --recurse-submodules https://github.com/Fabmat1/ASTRA.git
cd ASTRA

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DDIGGA_ENABLE_CUDA=OFF   # set ON if you have CUDA installed

cmake --build build -j$(nproc)
```

The compiled binary will be at `build/ASTRA`. Run it directly:
```bash
./build/ASTRA
```

#### 4. (Optional) System-wide Install

To install ASTRA system-wide — which also registers desktop files so it appears in your application menu:

```bash
sudo cmake --install build --prefix /usr/local
```

After install, launch it from your application menu or run `ASTRA` from any terminal.

## License

MIT License

## AI Declaration

ASTRA was made using AI assistance, but without agentic or autonomous coding. All code was carefully reviewed and pushed manually.

Further instructions will be added to this README in the future - hang tight!
