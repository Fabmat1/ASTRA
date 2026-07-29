# syntax=docker/dockerfile:1
# Builder image for the ASTRA AppImage. Everything that does NOT depend on the
# ASTRA source tree lives here: apt packages, Qt, OpenBLAS, header-only deps,
# the linuxdeploy/appimagetool tooling, and the full ISIS + S-Lang stack.
#
# Built automatically by ./build-appimage.sh when missing. The image tag
# encodes QT_VERSION / OPENBLAS_VERSION / BUNDLE_ISIS and a hash of this file,
# so editing anything here (or bumping a version) triggers a fresh build.
# The script libraries (isisscripts, stellar_isisscripts) and lcurve_re are
# NOT baked in — build-appimage.sh clones their latest HEAD on every build.
# Use --rebuild-image only to refresh baked-in tooling (linuxdeploy continuous,
# ISIS/S-Lang git HEADs).

FROM ubuntu:22.04

ARG QT_VERSION=6.11.1
ARG OPENBLAS_VERSION=0.3.27
ARG BUNDLE_ISIS=1

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# ---------- 1. System packages ----------
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git wget file ca-certificates pkg-config \
      gfortran patchelf desktop-file-utils libfuse2 ccache \
      imagemagick librsvg2-bin \
      libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev libvulkan-dev \
      libdbus-1-dev libfontconfig1-dev libfreetype-dev libcups2-dev \
      libxcb-cursor-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev \
      libxcb-randr0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-sync-dev \
      libxcb-xfixes0-dev libxcb-xkb-dev libxcb-util-dev \
      libeigen3-dev liblapack-dev libboost-all-dev nlohmann-json3-dev \
      libfftw3-dev libtbb-dev libcxxopts-dev \
      libccfits-dev libcfitsio-dev libssl-dev \
      python3 python3-pip python3-dev python3-numpy \
      libreadline-dev libncurses-dev libgsl-dev \
      libx11-dev libxext-dev libpng-dev libcurl4-openssl-dev \
      zlib1g-dev libpcre3-dev libonig-dev fig2dev perl curl \
      pgplot5 libfile-slurp-perl \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory '*'

# ---------- 2. Qt via aqtinstall ----------
RUN pip3 install --quiet aqtinstall \
    && aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 -O /opt/Qt

ENV QTROOT=/opt/Qt/${QT_VERSION}/gcc_64
ENV Qt6_DIR=${QTROOT}/lib/cmake/Qt6 \
    PATH=${QTROOT}/bin:/opt/isis/bin:${PATH} \
    LD_LIBRARY_PATH=${QTROOT}/lib:/opt/isis/lib:/usr/local/lib

# ---------- 3. OpenBLAS ----------
RUN cd /tmp \
    && wget -q "https://github.com/OpenMathLib/OpenBLAS/releases/download/v${OPENBLAS_VERSION}/OpenBLAS-${OPENBLAS_VERSION}.tar.gz" \
    && tar xf "OpenBLAS-${OPENBLAS_VERSION}.tar.gz" \
    && cd "OpenBLAS-${OPENBLAS_VERSION}" \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/usr/local \
         -DBUILD_SHARED_LIBS=ON -DUSE_OPENMP=1 -DDYNAMIC_ARCH=ON \
    && cmake --build build -j"$(nproc)" \
    && cmake --install build \
    && rm -rf /tmp/OpenBLAS-*

# ---------- 4. Header-only deps + FFTW3 CMake config stub ----------
RUN <<'EOF'
set -euo pipefail
mkdir -p /usr/local/include/ankerl
wget -q -O /usr/local/include/ankerl/unordered_dense.h \
  https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h
ln -sf /usr/local/include/ankerl/unordered_dense.h /usr/local/include/unordered_dense.h

# gnuplot-iostream (header-only) — required by the lcurve_re build
wget -q -O /usr/local/include/gnuplot-iostream.h \
  https://raw.githubusercontent.com/dstahlke/gnuplot-iostream/master/gnuplot-iostream.h

# Ubuntu's fftw3 apt package ships no CMake config — provide a stub
if ! find /usr -name 'FFTW3Config.cmake' 2>/dev/null | grep -q .; then
  mkdir -p /usr/local/lib/cmake/fftw3
  cat > /usr/local/lib/cmake/fftw3/FFTW3Config.cmake <<'FFTW_EOF'
set(FFTW3_INCLUDE_DIRS /usr/include)
set(FFTW3_LIBRARIES    /usr/lib/x86_64-linux-gnu/libfftw3.so)
if(NOT TARGET FFTW3::fftw3)
  add_library(FFTW3::fftw3 UNKNOWN IMPORTED)
  set_target_properties(FFTW3::fftw3 PROPERTIES
    IMPORTED_LOCATION "${FFTW3_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFTW3_INCLUDE_DIRS}")
endif()
if(NOT TARGET fftw3)
  add_library(fftw3 ALIAS FFTW3::fftw3)
endif()
set(FFTW3_FOUND TRUE)
FFTW_EOF
fi
EOF

# ---------- 5. linuxdeploy + plugins + appimagetool ----------
RUN <<'EOF'
set -euo pipefail
mkdir -p /opt/appimage-tools && cd /opt/appimage-tools
for img in linuxdeploy linuxdeploy-plugin-qt linuxdeploy-plugin-appimage; do
  wget -q "https://github.com/linuxdeploy/${img}/releases/download/continuous/${img}-x86_64.AppImage"
  chmod +x "${img}-x86_64.AppImage"
  ./"${img}-x86_64.AppImage" --appimage-extract >/dev/null
  mv squashfs-root "${img}.extracted"
  cp "${img}.extracted/usr/bin/${img}" /usr/local/bin/
  chmod +x "/usr/local/bin/${img}"
  rm -f "${img}-x86_64.AppImage"
done

# appimagetool (separate project — linuxdeploy-plugin-appimage shells out to it)
wget -q https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x appimagetool-x86_64.AppImage
./appimagetool-x86_64.AppImage --appimage-extract >/dev/null
mv squashfs-root appimagetool.extracted
cat > /usr/local/bin/appimagetool <<'WRAP'
#!/bin/sh
exec /opt/appimage-tools/appimagetool.extracted/AppRun "$@"
WRAP
chmod +x /usr/local/bin/appimagetool
rm -f appimagetool-x86_64.AppImage
EOF

# ---------- 6. ISIS + S-Lang stack (slowest part — the reason this image exists)
# Built entirely here; build-appimage.sh only *stages* /opt/isis + the script
# clones in /opt/isis_src into the AppDir at package time.
RUN <<'EOF'
set -euo pipefail
if [[ "${BUNDLE_ISIS}" != "1" ]]; then
  echo ">>> BUNDLE_ISIS=0 — skipping ISIS build"
  exit 0
fi

ISIS_PREFIX=/opt/isis
ISIS_SRC=/opt/isis_src
export PATH="${ISIS_PREFIX}/bin:${PATH}"
export LD_LIBRARY_PATH="${ISIS_PREFIX}/lib:${LD_LIBRARY_PATH:-}"
mkdir -p "${ISIS_PREFIX}" "${ISIS_SRC}"

# Discover PGPLOT (pgplot5 package) — paths differ across Debian/Ubuntu
PGPLOT_INC="$(dirname "$(dpkg -L pgplot5 | grep -m1 '/cpgplot\.h$')")"
PGPLOT_LIB="$(dirname "$(dpkg -L pgplot5 | grep -m1 '/libcpgplot\.so')")"
PGPLOT_DATA="$(dirname "$(dpkg -L pgplot5 | grep -m1 '/grfont\.dat$')")"
echo ">>> PGPLOT inc=${PGPLOT_INC} lib=${PGPLOT_LIB} data=${PGPLOT_DATA}"
[[ -f "${PGPLOT_INC}/cpgplot.h" && -f "${PGPLOT_DATA}/grfont.dat" ]] \
  || { echo "pgplot5 layout not as expected"; exit 1; }

# --- S-Lang (release tarball: stable, avoids the jedsoft git protocol) ---
cd "${ISIS_SRC}"
wget -q https://www.jedsoft.org/releases/slang/slang-2.3.3.tar.bz2
tar xjf slang-2.3.3.tar.bz2
( cd slang-2.3.3
  ./configure --prefix="${ISIS_PREFIX}"
  make -j"$(nproc)"
  make install )

# jedsoft-style configure/make/install against our freshly built slang
build_slang_pkg() {
  local name="$1" url="$2"
  echo ">>> ISIS dep: ${name}"
  cd "${ISIS_SRC}"
  git clone --depth 1 "${url}" "${name}"
  ( cd "${name}"
    ./configure --prefix="${ISIS_PREFIX}" --with-slang="${ISIS_PREFIX}"
    make -j"$(nproc)"
    make install )
}

# --- ISIS itself (with PGPLOT so plotting-aware isisscripts load) ---
cd "${ISIS_SRC}"
git clone --depth 1 https://github.com/houckj/isis.git isis
( cd isis
  ./configure --prefix="${ISIS_PREFIX}" --with-slang="${ISIS_PREFIX}" \
    --with-pgplotinc="${PGPLOT_INC}" --with-pgplotlib="${PGPLOT_LIB}"
  make -j"$(nproc)"
  make install )

# --- S-Lang modules / helpers the isisscripts pull in ---
build_slang_pkg slgsl  git://git.jedsoft.org/git/slgsl.git
build_slang_pkg slxfig git://git.jedsoft.org/git/slxfig.git
build_slang_pkg slirp  git://git.jedsoft.org/git/slirp.git

# --- jed (build-only): its `jed-script` drives the isisscripts doc/help
#     generator (tmexpand), which build-appimage.sh runs per build. ---
cd "${ISIS_SRC}"
git clone --depth 1 git://git.jedsoft.org/git/jed.git jed
( cd jed
  ./configure --prefix="${ISIS_PREFIX}" --with-slang="${ISIS_PREFIX}"
  make -j"$(nproc)"
  make install )

# isisscripts / stellar_isisscripts are intentionally NOT built here:
# build-appimage.sh clones their latest HEAD on every build (they change often
# and per-build `make` is cheap — the toolchain they need lives in this image).

# Slim the sources — only the installed prefix is needed from here on
rm -rf "${ISIS_SRC}"/*/.git "${ISIS_SRC}"/slang-2.3.3.tar.bz2
echo ">>> ISIS built into ${ISIS_PREFIX} ($(du -sh ${ISIS_PREFIX} | cut -f1))"
EOF
