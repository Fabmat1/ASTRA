#!/usr/bin/env bash
# Build a self-contained ASTRA installer (.exe) for 64-bit Windows.
#
# Usage:   ./build-windows.sh [VERSION]
#          Run it from an MSYS2 **UCRT64** shell, not from msys2/mingw32/clang64.
#
# Env overrides:
#   JOBS                   = number of parallel build jobs (default: all cores)
#   ASTRA_SKIP_PACMAN=1    = trust the toolchain already installed, skip pacman
#   ASTRA_WIN_CACHE        = cache root for ccache + the CCfits source build
#                            (default: ~/.cache/astra-build-windows)
#   ASTRA_SKIP_SUBMODULE_UPDATE=1
#                          = do not touch external/* (CI checks them out itself)
#   ASTRA_VERSION_OVERRIDE = stamp this exact version into the binary instead of
#                            resolving it from git. Release pipelines only (the
#                            tag build in .github/workflows/build-windows.yml
#                            sets it): a build that self-reports a release
#                            version it isn't will also refuse to auto-update to
#                            that release. VERSION alone only names the
#                            installer — it does not stamp the binary.
#   ISCC                   = path to Inno Setup's ISCC.exe, if it is not on PATH
#                            and not in the default install location.
#
# Toolchain: MSYS2 UCRT64 (mingw-w64 GCC). Not MSVC — GAEL declares
# `LANGUAGES C CXX Fortran` and there is no Fortran compiler in the MSVC
# toolchain, so a Visual Studio build cannot configure at all. UCRT64 also has
# packaged builds of every other dependency (Qt 6.11, OpenBLAS, Boost, TBB,
# FFTW, cfitsio, GSL) which is the whole reason this is not a vcpkg build.
#
# CCfits is REQUIRED (both ASTRA and GAEL link it) and is the one dependency
# MSYS2 does not package, so it gets a one-time source build here, cached.
#
# What is NOT in the Windows package, and why:
#   • lcurve — the light-curve fitting binaries are a Unix Fortran/C++ program
#     with no Windows build. Light-curve *fitting* is therefore unavailable;
#     everything else (fetching, periodograms, plotting) works. ASTRA already
#     resolves lcurve from Settings/PATH when it is not bundled, so a user who
#     builds it themselves can still point at it.
#   • ISIS — Unix-only, and SED fitting no longer needs it: the bundled
#     `sedfit` (SEDplusplus) replaced it and does build here.
set -euo pipefail

VERSION="${1:-0.0.0}"
JOBS="${JOBS:-$(nproc)}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# One cache root for the ccache tree and the CCfits source build. Overridable
# so CI can put it somewhere actions/cache can address (see build-windows.yml).
CACHE="${ASTRA_WIN_CACHE:-${HOME}/.cache/astra-build-windows}"
BUILD_DIR="${SRC_DIR}/build-windows"
STAGE_DIR="${SRC_DIR}/stage-windows"
CCFITS_PREFIX="${CACHE}/ccfits/install"
CCFITS_STAMP="${CACHE}/ccfits/2.7.stamp"
CCFITS_URL="https://heasarc.gsfc.nasa.gov/FTP/software/fitsio/ccfits/CCfits.tar.gz"
OUT_BASENAME="astra-${VERSION}-x86_64-setup"

# ── 0. Sanity checks ────────────────────────────────────────────────────────
[[ -f "${SRC_DIR}/CMakeLists.txt" ]] || { echo "Run from the ASTRA repo root."; exit 1; }
if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
  echo "MSYSTEM is '${MSYSTEM:-unset}', expected UCRT64."
  echo "Start the 'MSYS2 UCRT64' shell (not MSYS / MINGW64 / CLANG64) and re-run."
  exit 1
fi

echo ">>> ASTRA ${VERSION}  |  ${MSYSTEM}  |  $(uname -s) $(uname -r)"

# ── 1. Toolchain and dependencies ───────────────────────────────────────────
# One list, used both here and (via the same script) by CI, so what ships is
# what a developer building locally gets.
PACMAN_PKGS=(
  mingw-w64-ucrt-x86_64-gcc
  mingw-w64-ucrt-x86_64-gcc-fortran     # GAEL declares `LANGUAGES ... Fortran`
  mingw-w64-ucrt-x86_64-cmake
  mingw-w64-ucrt-x86_64-ninja
  mingw-w64-ucrt-x86_64-ccache
  mingw-w64-ucrt-x86_64-pkgconf
  mingw-w64-ucrt-x86_64-qt6-base        # also ships windeployqt
  mingw-w64-ucrt-x86_64-qt6-svg
  mingw-w64-ucrt-x86_64-eigen3
  mingw-w64-ucrt-x86_64-openblas        # BLAS + LAPACKE for EIGEN_USE_BLAS
  mingw-w64-ucrt-x86_64-boost
  mingw-w64-ucrt-x86_64-tbb
  mingw-w64-ucrt-x86_64-fftw            # src/fitting/Periodogram.cpp
  mingw-w64-ucrt-x86_64-cfitsio
  mingw-w64-ucrt-x86_64-nlohmann-json
  mingw-w64-ucrt-x86_64-cxxopts
  mingw-w64-ucrt-x86_64-unordered_dense
  mingw-w64-ucrt-x86_64-gsl             # SEDplusplus (sedfit)
  mingw-w64-ucrt-x86_64-curl            #   "
  mingw-w64-ucrt-x86_64-zlib            #   "
  mingw-w64-ucrt-x86_64-python          # GAEL: find_package(Python3 ... REQUIRED)
  mingw-w64-ucrt-x86_64-python-numpy    #   "
)
if [[ "${ASTRA_SKIP_PACMAN:-0}" != "1" ]]; then
  echo ">>> Installing MSYS2 packages"
  pacman -S --needed --noconfirm "${PACMAN_PKGS[@]}"
fi

mkdir -p "${CACHE}"

# ── 2. Submodules ───────────────────────────────────────────────────────────
if [[ "${ASTRA_SKIP_SUBMODULE_UPDATE:-0}" != "1" ]]; then
  git -C "${SRC_DIR}" submodule update --init --recursive
fi

# ── 3. CCfits (REQUIRED — ASTRA and GAEL both link it) ──────────────────────
# GAEL does `find_library(CCfits ... REQUIRED)`, so a missing CCfits hard-fails
# configure rather than degrading. MSYS2 has no CCfits package, so build it.
# Static on purpose: CCfits' own CMakeLists only exports symbols for MSVC
# shared builds, and a static archive sidesteps one more DLL to deploy.
find_ccfits() { [[ -f "${CCFITS_PREFIX}/include/CCfits/CCfits.h" ]] \
                && compgen -G "${CCFITS_PREFIX}/lib/libCCfits.*" >/dev/null; }

if [[ -f "${CCFITS_STAMP}" ]] && find_ccfits; then
  echo ">>> Using cached CCfits: ${CCFITS_PREFIX}"
else
  echo ">>> Building CCfits from source (one-time)"
  rm -f "${CCFITS_STAMP}"
  rm -rf "${CACHE}/ccfits/src" "${CCFITS_PREFIX}"
  mkdir -p "${CACHE}/ccfits/src"
  curl -fsSL -o "${CACHE}/ccfits/src.tgz" "${CCFITS_URL}"
  tar xzf "${CACHE}/ccfits/src.tgz" -C "${CACHE}/ccfits/src"
  CCFITS_SD="$(find "${CACHE}/ccfits/src" -maxdepth 2 -name CMakeLists.txt -printf '%h\n' -quit)"
  [[ -n "${CCFITS_SD}" ]] || { echo "CCfits tarball has no CMakeLists.txt"; exit 1; }
  # CCfits declares cmake_minimum_required(3.8); CMake 4 refuses that outright.
  cmake -S "${CCFITS_SD}" -B "${CCFITS_SD}/_build" -G Ninja \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_INSTALL_PREFIX="${CCFITS_PREFIX}"
  cmake --build "${CCFITS_SD}/_build" -j "${JOBS}"
  cmake --install "${CCFITS_SD}/_build"
  find_ccfits || { echo "CCfits build produced no usable library"; exit 1; }
  touch "${CCFITS_STAMP}"
fi

# ── 4. Configure ────────────────────────────────────────────────────────────
if [[ -z "${ASTRA_VERSION_OVERRIDE:-}" ]]; then
  echo "!!! ASTRA_VERSION_OVERRIDE is unset — the binary will self-report a"
  echo "!!! development version and the in-app updater will treat it as such."
fi

export CCACHE_DIR="${CACHE}/ccache"
mkdir -p "${CCACHE_DIR}"

# GAEL wants Python3 with the Development and NumPy components. On a GitHub
# runner there is a second Python in C:/hostedtoolcache that CMake finds first,
# and it has neither -- so point FindPython3 at MSYS2's explicitly rather than
# letting it pick. Native (mixed) paths: this CMake is a Windows binary and does
# not understand /ucrt64.
UCRT_PREFIX="$(cygpath -m /ucrt64)"

cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_VERSION_OVERRIDE="${ASTRA_VERSION_OVERRIDE:-}" \
  -DCMAKE_PREFIX_PATH="${CCFITS_PREFIX}" \
  -DPython3_ROOT_DIR="${UCRT_PREFIX}" \
  -DPython3_EXECUTABLE="${UCRT_PREFIX}/bin/python3.exe" \
  -DPython3_FIND_STRATEGY=LOCATION \
  -DPython3_FIND_REGISTRY=NEVER \
  -DCMAKE_INSTALL_PREFIX="${STAGE_DIR}" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DBUILD_TESTING=OFF

# ── 5. Build and stage ──────────────────────────────────────────────────────
# -k 0 keeps going after the first error. A Windows build breaking is almost
# always a batch of related portability errors, and one CI round that reports
# all of them beats N rounds that report one each.
cmake --build "${BUILD_DIR}" -j "${JOBS}" -- -k 0

rm -rf "${STAGE_DIR}"
cmake --install "${BUILD_DIR}"

BIN_DIR="${STAGE_DIR}/bin"
[[ -f "${BIN_DIR}/ASTRA.exe" ]] || { echo "No ASTRA.exe in ${BIN_DIR}"; exit 1; }

# ── 6. Qt deployment ────────────────────────────────────────────────────────
# Copies the Qt DLLs plus the platform/imageformat/sqldrivers plugins ASTRA
# needs. The SQLite driver in particular is not a link-time dependency — the
# app loads it by name at runtime — so nothing else would pull it in.
echo ">>> windeployqt"
windeployqt.exe \
  --release \
  --no-translations \
  --no-system-d3d-compiler \
  --no-opengl-sw \
  --no-compiler-runtime \
  "$(cygpath -w "${BIN_DIR}/ASTRA.exe")"

[[ -f "${BIN_DIR}/sqldrivers/qsqlite.dll" ]] || {
  echo "::warning::qsqlite.dll was not deployed — ASTRA cannot open its database."
  mkdir -p "${BIN_DIR}/sqldrivers"
  cp "/ucrt64/share/qt6/plugins/sqldrivers/qsqlite.dll" "${BIN_DIR}/sqldrivers/"
}

# ── 7. Non-Qt DLL closure ───────────────────────────────────────────────────
# windeployqt only knows about Qt. OpenBLAS, gfortran's runtime, Boost, TBB,
# FFTW, cfitsio, GSL and libstdc++/libgcc/libwinpthread all still have to come
# along or the app dies at load time on a machine without MSYS2.
#
# objdump reads the PE import table directly, which is exact; `ldd` on a mingw
# binary depends on the loader emulation and has been known to miss delay-loaded
# entries. Anything that does not resolve inside /ucrt64/bin is a system DLL
# (kernel32, user32, ...) and is deliberately left behind.
imports_of() {  # $1 = PE file -> one imported DLL name per line
  objdump -p "$1" 2>/dev/null | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p'
}

deploy_dll_closure() {  # $1 = directory holding the .exe(s) to satisfy
  local dir="$1" target dll key src
  local -a pending
  local -A seen=()
  # Seed with everything windeployqt already put here, plugins included. A Qt
  # plugin is loaded into the process, so its imports resolve against the
  # executable's directory, not the plugin's -- which is why the whole subtree
  # seeds one closure rooted at ${dir} rather than one closure per folder.
  mapfile -t pending < <(find "${dir}" \( -name '*.exe' -o -name '*.dll' \))
  while ((${#pending[@]})); do
    target="${pending[0]}"
    pending=("${pending[@]:1}")
    while read -r dll; do
      [[ -n "${dll}" ]] || continue
      # Case-insensitive dedupe: import tables spell DLL names inconsistently
      # (KERNEL32.dll vs kernel32.dll), and the filesystem does not care.
      key="${dll,,}"
      [[ -n "${seen[${key}]:-}" ]] && continue
      seen[${key}]=1
      src="/ucrt64/bin/${dll}"
      [[ -f "${src}" ]] || continue          # system DLL — ships with Windows
      [[ -f "${dir}/${dll}" ]] || cp "${src}" "${dir}/"
      pending+=("${dir}/${dll}")
    done < <(imports_of "${target}")
  done
}

echo ">>> Resolving DLL dependencies"
# Every directory that holds an executable needs its own closure: Windows
# resolves imports next to the .exe, and the helper binaries live in their own
# libexec subdirectories, not beside ASTRA.exe.
while read -r exe_dir; do
  echo "    ${exe_dir#${STAGE_DIR}/}"
  deploy_dll_closure "${exe_dir}"
done < <(find "${STAGE_DIR}" -name '*.exe' -printf '%h\n' | sort -u)

# A missing import here means the app would fail to start with an unhelpful
# "0xc0000135" dialog, or a plugin that silently never loads, so check rather
# than trust. Each PE is checked against the directory of the executable it
# belongs to, since that is where Windows will look.
missing=0
while read -r exe_dir; do
  while read -r pe; do
    while read -r dll; do
      [[ -n "${dll}" ]] || continue
      [[ -f "${exe_dir}/${dll}" ]] && continue
      [[ -f "/ucrt64/bin/${dll}" ]] || continue   # system DLL
      echo "::error::${pe#${STAGE_DIR}/} imports ${dll}, which was not deployed"
      missing=1
    done < <(imports_of "${pe}")
  done < <(find "${exe_dir}" \( -name '*.exe' -o -name '*.dll' \))
done < <(find "${STAGE_DIR}" -name '*.exe' -printf '%h\n' | sort -u)
((missing == 0)) || exit 1

echo ">>> Staged tree: $(du -sh "${STAGE_DIR}" | cut -f1)"

# ── 8. Installer ────────────────────────────────────────────────────────────
find_iscc() {
  if command -v iscc >/dev/null 2>&1; then command -v iscc; return; fi
  if [[ -n "${ISCC:-}" && -f "${ISCC}" ]]; then echo "${ISCC}"; return; fi
  local c
  for c in "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
           "/c/Program Files/Inno Setup 6/ISCC.exe"; do
    [[ -f "${c}" ]] && { echo "${c}"; return; }
  done
  return 1
}

ISCC_BIN="$(find_iscc)" || {
  echo "Inno Setup (ISCC.exe) not found. Install it from https://jrsoftware.org/isdl.php"
  echo "or set ISCC=/path/to/ISCC.exe. On GitHub's windows runners it is preinstalled."
  exit 1
}
echo ">>> Inno Setup: ${ISCC_BIN}"

# Windows paths are full of backslashes, and a backslash in a sed replacement
# is an escape -- "C:\Users\..." would silently become "C:Users...", and a \U
# would upper-case the rest of the line. Double them before substituting.
sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

ISS="${BUILD_DIR}/astra.iss"
sed -e "s|@VERSION@|$(sed_escape "${VERSION}")|g" \
    -e "s|@STAGE_DIR@|$(sed_escape "$(cygpath -w "${STAGE_DIR}")")|g" \
    -e "s|@ICON@|$(sed_escape "$(cygpath -w "${SRC_DIR}/resources/windows/astra.ico")")|g" \
    -e "s|@OUT_DIR@|$(sed_escape "$(cygpath -w "${SRC_DIR}")")|g" \
    -e "s|@OUT_BASENAME@|$(sed_escape "${OUT_BASENAME}")|g" \
    "${SRC_DIR}/resources/windows/astra.iss.in" > "${ISS}"

"${ISCC_BIN}" "$(cygpath -w "${ISS}")"

OUT_EXE="${SRC_DIR}/${OUT_BASENAME}.exe"
[[ -f "${OUT_EXE}" ]] || { echo "Inno Setup produced no ${OUT_BASENAME}.exe"; exit 1; }

# ── 9. Checksum ─────────────────────────────────────────────────────────────
# Same "<hex>  <filename>" shape the AppImage and .dmg publish, because
# UpdateManager parses all three with one reader.
( cd "${SRC_DIR}" && sha256sum "${OUT_BASENAME}.exe" > "${OUT_BASENAME}.exe.sha256" )

echo
echo ">>> Done: ${OUT_EXE}  ($(du -h "${OUT_EXE}" | cut -f1))"
cat "${SRC_DIR}/${OUT_BASENAME}.exe.sha256"
