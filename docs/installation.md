# Installation

ASTRA runs on Linux and macOS. Most users should download a prebuilt package
from the [latest release](https://github.com/Fabmat1/ASTRA/releases/latest);
building from source is also supported.

## Linux AppImage (recommended)

Download the `astra-<version>-x86_64.AppImage` from the
[latest release](https://github.com/Fabmat1/ASTRA/releases/latest).

The AppImage bundles Qt and most libraries, but a few system packages must be
installed separately:

=== "Debian / Ubuntu (22.04+)"

    ```bash
    sudo apt install libfuse2 libopengl0 libegl1 libxkbcommon0 libgl1 \
                     libdbus-1-3 libfontconfig1 \
                     python3 python3-numpy gnuplot
    ```

=== "Arch Linux"

    ```bash
    sudo pacman -S fuse2 libglvnd libxkbcommon dbus fontconfig \
                   python python-numpy gnuplot
    ```

Then make it executable and run it (adjust the version number to match your
download):

```bash
chmod +x ./astra-0.6.0-x86_64.AppImage
./astra-0.6.0-x86_64.AppImage
```

!!! tip "FUSE problems"
    If the AppImage fails to start because FUSE is unavailable on your system,
    run it with `--appimage-extract-and-run`:
    ```bash
    ./astra-0.6.0-x86_64.AppImage --appimage-extract-and-run
    ```

## macOS (Apple Silicon)

Download the `astra-<version>-arm64.dmg` from the
[latest release](https://github.com/Fabmat1/ASTRA/releases/latest), open it,
and drag ASTRA into your Applications folder.

!!! note "Unsigned app"
    The app is not notarized with Apple. If macOS refuses to open it, either
    right-click the app and choose **Open**, or allow it under
    **System Settings → Privacy & Security**. If it still will not start, remove
    the quarantine flag:
    ```bash
    xattr -dr com.apple.quarantine /Applications/ASTRA.app
    ```

## Building from source

ASTRA is a CMake project requiring **Qt ≥ 6.10**, a Fortran compiler, and
several scientific libraries.

### Automated installer (Ubuntu / Debian / Arch)

`install-linux.sh` detects your distribution, installs the packages, downloads
Qt via `aqtinstall` when your distro's Qt is too old, fetches the git
submodules, builds the optional `lcurve` fitting binaries, and finally builds
and installs ASTRA:

```bash
git clone https://github.com/Fabmat1/ASTRA.git
cd ASTRA
./install-linux.sh
```

Useful options: `--prefix ~/.local` (per-user install, no root needed),
`--build-only`, `--no-lcurve`, `--skip-deps`, `--no-cuda`, `-j N`, `-v`.
Run `./install-linux.sh --help` for the full list.

### Manual build

#### 1. Install build dependencies

=== "Ubuntu 22.04+ / Debian 12+"

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

    Ubuntu's packaged Qt is older than 6.10, so install Qt with `aqtinstall`:

    ```bash
    pip install aqtinstall
    aqt install-qt linux desktop 6.11.1 linux_gcc_64 -O ~/Qt

    # Make CMake aware of it
    export CMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64:$CMAKE_PREFIX_PATH
    export PATH=$HOME/Qt/6.11.1/gcc_64/bin:$PATH
    ```

    Add the two `export` lines to your `~/.bashrc` to make them persistent.

=== "Arch Linux"

    ```bash
    sudo pacman -S base-devel cmake git wget pkgconf gcc-fortran \
                   qt6-base qt6-tools qt6-svg qt6-declarative \
                   vulkan-headers vulkan-icd-loader \
                   eigen openblas lapack boost nlohmann-json \
                   fftw onetbb cxxopts \
                   ccfits cfitsio \
                   python python-numpy gnuplot
    ```

#### 2. Header-only dependencies

`unordered_dense` is not packaged on most distros, so install it manually:

```bash
sudo mkdir -p /usr/local/include/ankerl
sudo wget -O /usr/local/include/ankerl/unordered_dense.h \
  https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h
sudo ln -sf /usr/local/include/ankerl/unordered_dense.h /usr/local/include/unordered_dense.h
```

#### 3. Clone and build

```bash
git clone --recurse-submodules https://github.com/Fabmat1/ASTRA.git
cd ASTRA

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAEL_ENABLE_CUDA=OFF   # set ON if you have CUDA installed

cmake --build build -j$(nproc)
```

The compiled binary is at `build/ASTRA`:

```bash
./build/ASTRA
```

#### 4. (Optional) system-wide install

Installing system-wide also registers desktop files so ASTRA appears in your
application menu:

```bash
sudo cmake --install build --prefix /usr/local
```

After installing, launch it from your application menu or run `ASTRA` from any
terminal.

!!! info "SED and light-curve fitting backends"
    SED fitting uses [ISIS](https://www.sternwarte.uni-erlangen.de/isis/) and
    light-curve fitting uses an adaptation of `lcurve`. These backends are
    bundled with the AppImage and DMG releases. For source builds,
    `install-linux.sh` builds `lcurve` for you (skip with `--no-lcurve`).
