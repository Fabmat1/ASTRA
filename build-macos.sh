#!/usr/bin/env bash
# Build a self-contained ASTRA .app (and .dmg) for Apple Silicon macOS.
#
# Usage:   ./build-macos.sh [VERSION]
# Env overrides:
#   QT_FROM        = brew (default) | aqt        # where Qt6 comes from
#   OPENBLAS_VERSION  = 0.3.27 (only for source fallback)
#   JOBS              = number of parallel build jobs (default: all cores)
#   ASTRA_VERSION_OVERRIDE = stamp this exact version into the binary instead of
#                     resolving it from git. Release pipelines only (the tag
#                     build in .github/workflows/build-macos.yml sets it): a
#                     build that self-reports a release version it isn't will
#                     also refuse to auto-update to that release. VERSION alone
#                     only names the .dmg — it does not stamp the binary.
#
# CCfits is REQUIRED (both ASTRA and DIGGA link it); it comes from the
# Homebrew bottle, with a one-time source build as fallback. The build
# aborts if neither works — there is no FITS-less fallback.
#
# Unlike the Linux AppImage build there is NO container: macOS apps must be
# built natively on macOS. Run this on the target Mac (Apple Silicon).
# First run may do a one-time OpenBLAS/CCfits source build (~5-8 min, cached);
# later runs reuse the cache.
set -euo pipefail

VERSION="${1:-0.5.1}"
QT_FROM="${QT_FROM:-brew}"
OPENBLAS_VERSION="${OPENBLAS_VERSION:-0.3.27}"
# Bundled helper programs (mirrors what build-appimage.sh ships).
ASTRA_BUNDLE_LCURVE="${ASTRA_BUNDLE_LCURVE:-1}"   # lcurve_re fitting binaries
ASTRA_BUNDLE_LCQUERY="${ASTRA_BUNDLE_LCQUERY:-1}" # lightcurvequery Python sources
# ISIS/S-Lang. Provisioned by build-macos-isis.sh (kept separate so its very
# slow, very stable source build caches independently of this script) and
# staged into the .app below. Unlike the AppImage's PGPLOT this one has no X11
# drivers — see the header of build-macos-isis.sh for why. Failure to build it
# is non-fatal by default: the .dmg still ships, just without the bundled ISIS.
# Set ASTRA_REQUIRE_ISIS=1 (the release workflow does) to make it fatal instead,
# so a tagged .dmg can never quietly go out without ISIS.
ASTRA_BUNDLE_ISIS="${ASTRA_BUNDLE_ISIS:-1}"
ASTRA_REQUIRE_ISIS="${ASTRA_REQUIRE_ISIS:-0}"
LCURVE_REPO="${LCURVE_REPO:-https://github.com/Fabmat1/lcurve_re.git}"
LCURVE_REF="${LCURVE_REF:-main}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="${HOME}/.cache/astra-build-macos"
BUILD_DIR="${SRC_DIR}/build-macos"

# ── 0. Sanity checks ────────────────────────────────────────────────────────
[[ "$(uname -s)" == "Darwin" ]] || { echo "This script must run on macOS."; exit 1; }
[[ -f "${SRC_DIR}/CMakeLists.txt" ]] || { echo "Run from the ASTRA repo root."; exit 1; }
if [[ "$(uname -m)" != "arm64" ]]; then
  echo "WARNING: host arch is $(uname -m), not arm64 — this will build an Intel app."
fi

command -v brew >/dev/null 2>&1 || {
  echo "Homebrew not found. Install it from https://brew.sh then re-run."
  exit 1
}
BREW_PREFIX="$(brew --prefix)"
mkdir -p "${CACHE}"

# We pin the C/C++ compiler to Apple Clang from the Command Line Tools. If CMake
# instead picks up Homebrew's LLVM clang (often on PATH), object files compile
# against LLVM's libc++ but link against the system libc++ -> undefined
# __hash_memory. /usr/bin/clang++ guarantees compile and link use one libc++.
[[ -x /usr/bin/clang++ ]] || {
  echo "/usr/bin/clang++ not found. Install Xcode Command Line Tools: xcode-select --install"
  exit 1
}

echo ">>> ASTRA ${VERSION}  |  $(uname -m)  |  $(sw_vers -productName) $(sw_vers -productVersion)"
echo ">>> Compiler: $(/usr/bin/clang++ --version | head -1)"

# ── 1. Ensure submodules ────────────────────────────────────────────────────
git -C "${SRC_DIR}" submodule update --init --recursive

# ── 2. Homebrew dependencies ────────────────────────────────────────────────
# gcc -> gfortran (DIGGA declares `LANGUAGES ... Fortran`, so CMake needs a
#                  Fortran compiler at configure time even with no .f sources)
# libomp -> OpenMP for Apple Clang (DIGGA + rv_mcmc require it)
# gsl/curl/zlib -> SEDplusplus (sedfit): its CMake uses pkg_check_modules(...
#   REQUIRED) for all three, and macOS ships no .pc files for curl/zlib — so
#   they must come from brew (keg-only; their prefixes are added to
#   CMAKE_PREFIX_PATH below, which CMake forwards to PKG_CONFIG_PATH).
# cxxopts -> DIGGA (its brew formula ships the CMake config DIGGA looks for)
BREW_PKGS=(cmake pkg-config wget gcc libomp eigen boost fftw nlohmann-json tbb cfitsio ccfits gsl curl zlib cxxopts openblas python numpy)
# dylibbundler self-contains the non-Qt helper binaries' dylibs (sedfit always;
# lcurve when bundled) — macdeployqt only handles the main Qt app, not
# arbitrary executables.
BREW_PKGS+=(dylibbundler)
# cpanminus installs the Perl File::Slurp that stellar_isisscripts' makestatic
# needs; only used when ISIS is bundled, but it is a tiny, harmless formula.
[[ "${ASTRA_BUNDLE_ISIS}" == "1" ]] && BREW_PKGS+=(cpanminus)
[[ "${QT_FROM}" == "brew" ]] && BREW_PKGS+=(qt)

echo ">>> Installing/updating Homebrew packages: ${BREW_PKGS[*]}"
brew install "${BREW_PKGS[@]}" 2>&1 | grep -vE '^(Warning: .* already installed|==> )' || true

LIBOMP="$(brew --prefix libomp)"
# gfortran lives in the gcc keg; pick whatever version brew installed.
FORTRAN_COMPILER="$(ls "${BREW_PREFIX}/bin/gfortran"* 2>/dev/null | sort -V | tail -1 || true)"
[[ -n "${FORTRAN_COMPILER}" ]] || { echo "gfortran not found after 'brew install gcc'."; exit 1; }
echo ">>> Using Fortran compiler: ${FORTRAN_COMPILER}"

# ── 3. Qt6 ──────────────────────────────────────────────────────────────────
if [[ "${QT_FROM}" == "brew" ]]; then
  QT_PREFIX="$(brew --prefix qt)"
else
  # aqtinstall fallback (Qt for macOS, universal/arm64).
  python3 -m pip install --quiet --user aqtinstall
  QT_VERSION="${QT_VERSION:-6.11.1}"
  python3 -m aqt install-qt mac desktop "${QT_VERSION}" clang_64 -O "${CACHE}/Qt"
  QT_PREFIX="${CACHE}/Qt/${QT_VERSION}/macos"
fi
MACDEPLOYQT="${QT_PREFIX}/bin/macdeployqt"
[[ -x "${MACDEPLOYQT}" ]] || { echo "macdeployqt not found at ${MACDEPLOYQT}"; exit 1; }
echo ">>> Qt6 prefix: ${QT_PREFIX}"

# ── 4. OpenBLAS (DIGGA needs the OpenBLAS::OpenBLAS CMake target) ────────────
# Prefer Homebrew's openblas IF it ships a CMake package config; otherwise
# build from source with CMake (guarantees the config + LAPACKE headers,
# matching the known-good Linux build).
OPENBLAS_BREW="$(brew --prefix openblas 2>/dev/null || true)"
if [[ -z "${OPENBLAS_BREW}" ]]; then
  brew install openblas >/dev/null 2>&1 || true
  OPENBLAS_BREW="$(brew --prefix openblas 2>/dev/null || true)"
fi
OPENBLAS_PREFIX=""
if [[ -n "${OPENBLAS_BREW}" ]] && find "${OPENBLAS_BREW}" -name 'OpenBLASConfig.cmake' 2>/dev/null | grep -q .; then
  OPENBLAS_PREFIX="${OPENBLAS_BREW}"
  echo ">>> Using Homebrew OpenBLAS (CMake config found): ${OPENBLAS_PREFIX}"
else
  OPENBLAS_PREFIX="${CACHE}/openblas/install"
  STAMP="${CACHE}/openblas/${OPENBLAS_VERSION}.stamp"
  if [[ -f "${STAMP}" ]]; then
    echo ">>> Using cached source-built OpenBLAS ${OPENBLAS_VERSION}"
  else
    echo ">>> Building OpenBLAS ${OPENBLAS_VERSION} from source (one-time, ~5 min)"
    rm -rf "${CACHE}/openblas/src" "${CACHE}/openblas/build"
    mkdir -p "${CACHE}/openblas/src"
    wget -q -O "${CACHE}/openblas/src.tgz" \
      "https://github.com/OpenMathLib/OpenBLAS/releases/download/v${OPENBLAS_VERSION}/OpenBLAS-${OPENBLAS_VERSION}.tar.gz"
    tar xf "${CACHE}/openblas/src.tgz" -C "${CACHE}/openblas/src" --strip-components=1
    # USE_OPENMP=0 on purpose: with OpenMP on, OpenBLAS's C objects pull in
    # Apple Clang's libomp (__kmpc_*) but the shared lib is linked by gfortran
    # (libgomp) -> unresolved symbols. Without it OpenBLAS uses its own pthread
    # threading, which is self-contained and the standard macOS recommendation.
    # DIGGA/rv_mcmc still get their own OpenMP (libomp) independently.
    # CMAKE_POLICY_VERSION_MINIMUM works around CMake 4.x dropping <3.5 compat.
    cmake -S "${CACHE}/openblas/src" -B "${CACHE}/openblas/build" \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${OPENBLAS_PREFIX}" \
      -DBUILD_SHARED_LIBS=ON -DUSE_OPENMP=0 -DUSE_THREAD=1 -DDYNAMIC_ARCH=ON
    cmake --build "${CACHE}/openblas/build" -j"${JOBS}"
    cmake --install "${CACHE}/openblas/build"
    touch "${STAMP}"
  fi
fi

# ── 4b. FindOpenBLAS shim ───────────────────────────────────────────────────
# DIGGA does `find_package(OpenBLAS REQUIRED)` then links OpenBLAS::OpenBLAS,
# but OpenBLAS's various CMake configs don't reliably define that namespaced
# target. find_package tries Module mode first, so a FindOpenBLAS.cmake on
# CMAKE_MODULE_PATH wins and lets us define the target deterministically,
# pointing at the OpenBLAS we resolved above (libopenblas bundles LAPACK[E]).
CMAKE_MODULES="${CACHE}/cmake-modules"
mkdir -p "${CMAKE_MODULES}"
cat > "${CMAKE_MODULES}/FindOpenBLAS.cmake" <<EOF
# Auto-generated by build-macos.sh — defines OpenBLAS::OpenBLAS for DIGGA.
if(NOT TARGET OpenBLAS::OpenBLAS)
  find_path(OpenBLAS_INCLUDE_DIR NAMES openblas_config.h cblas.h
    HINTS "${OPENBLAS_PREFIX}/include" "${OPENBLAS_PREFIX}/include/openblas")
  find_library(OpenBLAS_LIBRARY NAMES openblas
    HINTS "${OPENBLAS_PREFIX}/lib")
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(OpenBLAS
    REQUIRED_VARS OpenBLAS_LIBRARY OpenBLAS_INCLUDE_DIR)
  if(OpenBLAS_FOUND)
    add_library(OpenBLAS::OpenBLAS UNKNOWN IMPORTED)
    set_target_properties(OpenBLAS::OpenBLAS PROPERTIES
      IMPORTED_LOCATION "\${OpenBLAS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "\${OpenBLAS_INCLUDE_DIR}")
  endif()
endif()
EOF

# rv_mcmc does `find_package(FFTW3 REQUIRED)` then links a BARE `fftw3` name,
# which CMake turns into a plain -lfftw3 with no include/lib path. Provide a
# real `fftw3` imported target (Module mode wins over brew's config) so FFTW's
# header and full library path propagate — no global -I/-L needed, which keeps
# Homebrew's lib dir off the link line (it can shadow the system libc++ and
# cause undefined __hash_memory).
FFTW_PREFIX="$(brew --prefix fftw)"
cat > "${CMAKE_MODULES}/FindFFTW3.cmake" <<EOF
# Auto-generated by build-macos.sh — provides the bare \`fftw3\` target rv_mcmc
# links (plus FFTW3::fftw3), with include/lib resolved under Homebrew.
if(NOT TARGET fftw3)
  find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h HINTS "${FFTW_PREFIX}/include")
  find_library(FFTW3_LIBRARY  NAMES fftw3   HINTS "${FFTW_PREFIX}/lib")
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(FFTW3
    REQUIRED_VARS FFTW3_LIBRARY FFTW3_INCLUDE_DIR)
  add_library(fftw3 UNKNOWN IMPORTED)
  set_target_properties(fftw3 PROPERTIES
    IMPORTED_LOCATION "\${FFTW3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "\${FFTW3_INCLUDE_DIR}")
  if(NOT TARGET FFTW3::fftw3)
    add_library(FFTW3::fftw3 ALIAS fftw3)
  endif()
endif()
EOF

# ── 5. CCfits (REQUIRED — ASTRA and DIGGA both link it) ─────────────────────
# DIGGA does `find_library(CCfits ... REQUIRED)`, so a missing CCfits doesn't
# degrade gracefully — configure hard-fails. Resolve one CCfits here and feed
# the exact paths to every consumer.
#
# Detection is by files, NOT `pkg-config --exists ccfits`: the Homebrew bottle
# is an autotools build that ships no ccfits.pc, so pkg-config reports it
# missing even when it's perfectly usable (that false negative is what pushed
# fresh machines into the source-build path before).
CFITSIO_PREFIX="$(brew --prefix cfitsio)"
find_ccfits_in() {  # sets CCFITS_INCDIR/CCFITS_LIBFILE if $1 holds a usable CCfits
  local p="$1"
  [[ -n "${p}" && -f "${p}/include/CCfits/CCfits.h" ]] || return 1
  local lib
  lib="$(ls "${p}"/lib/libCCfits*.dylib "${p}"/lib/libCCfits*.a 2>/dev/null | head -1)"
  [[ -n "${lib}" ]] || return 1
  CCFITS_INCDIR="${p}/include"
  CCFITS_LIBFILE="${lib}"
  return 0
}

CCFITS_PREFIX=""
CCFITS_BREW="$(brew --prefix ccfits 2>/dev/null || true)"
if find_ccfits_in "${CCFITS_BREW}"; then
  CCFITS_PREFIX="${CCFITS_BREW}"
  echo ">>> Using Homebrew CCfits: ${CCFITS_PREFIX}"
else
  # Source-build fallback (e.g. no bottle for this macOS version).
  CCFITS_PREFIX="${CACHE}/ccfits/install"
  CCFITS_STAMP="${CACHE}/ccfits/2.6.stamp"
  # Trust the stamp only if the install it points at still exists.
  if [[ -f "${CCFITS_STAMP}" ]] && find_ccfits_in "${CCFITS_PREFIX}"; then
    echo ">>> Using cached source-built CCfits: ${CCFITS_PREFIX}"
  else
    rm -f "${CCFITS_STAMP}"
    echo ">>> Building CCfits 2.6 from source (one-time)"
    (
      set -e
      rm -rf "${CACHE}/ccfits/src" "${CCFITS_PREFIX}"
      mkdir -p "${CACHE}/ccfits/src"
      wget -q -O "${CACHE}/ccfits/src.tgz" \
        "https://heasarc.gsfc.nasa.gov/fitsio/CCfits/CCfits-2.6.tar.gz"
      # Don't assume the tarball layout — extract, then locate the build system.
      tar xf "${CACHE}/ccfits/src.tgz" -C "${CACHE}/ccfits/src"
      CONFIGURE="$(find "${CACHE}/ccfits/src" -maxdepth 3 -name configure -type f -print -quit)"
      CMAKELISTS="$(find "${CACHE}/ccfits/src" -maxdepth 3 -name CMakeLists.txt -type f -print -quit)"
      if [[ -n "${CONFIGURE}" ]]; then
        cd "$(dirname "${CONFIGURE}")"
        # Pin the compiler here too — same libc++ mismatch hazard as the main build.
        CC=/usr/bin/clang CXX=/usr/bin/clang++ \
          ./configure --prefix="${CCFITS_PREFIX}" --with-cfitsio="${CFITSIO_PREFIX}"
        make -j"${JOBS}"
        make install
      elif [[ -n "${CMAKELISTS}" ]]; then
        SD="$(dirname "${CMAKELISTS}")"
        cmake -S "${SD}" -B "${SD}/_build" \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=/usr/bin/clang \
          -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
          -DCMAKE_INSTALL_PREFIX="${CCFITS_PREFIX}" \
          -DCMAKE_PREFIX_PATH="${CFITSIO_PREFIX}"
        cmake --build "${SD}/_build" -j"${JOBS}"
        cmake --install "${SD}/_build"
      else
        echo "No configure or CMakeLists.txt found in extracted CCfits source"; exit 1
      fi
    ) && find_ccfits_in "${CCFITS_PREFIX}" && touch "${CCFITS_STAMP}" || {
      echo "!!! CCfits could not be provisioned (brew bottle absent, source build failed)."
      echo "!!! CCfits is required — try 'brew install ccfits' manually, then re-run."
      exit 1
    }
  fi
fi
CFITSIO_LIBFILE="$(ls "${CFITSIO_PREFIX}"/lib/libcfitsio*.dylib 2>/dev/null | head -1)"
[[ -n "${CFITSIO_LIBFILE}" ]] || { echo "libcfitsio not found under ${CFITSIO_PREFIX}."; exit 1; }
echo ">>> CCfits: ${CCFITS_LIBFILE}  |  cfitsio: ${CFITSIO_LIBFILE}"

# ── 6. Header-only dep: ankerl/unordered_dense (DIGGA) ──────────────────────
DEPS_INC="${CACHE}/include"
mkdir -p "${DEPS_INC}/ankerl"
if [[ ! -f "${DEPS_INC}/ankerl/unordered_dense.h" ]]; then
  wget -q -O "${DEPS_INC}/ankerl/unordered_dense.h" \
    https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h
fi
ln -sf "${DEPS_INC}/ankerl/unordered_dense.h" "${DEPS_INC}/unordered_dense.h"

# ── 7. Idempotent source patches (no-op if already fixed/committed) ──────────
# BSD-safe in-place edits via perl. Mirrors build-appimage.sh's self-healing.
RV="${SRC_DIR}/external/rv_mcmc/src/vector_operations.cpp"
# (a) Replace the GNU-only `2j` imaginary literal — Apple Clang rejects it.
if grep -q '2j \* M_PI' "${RV}"; then
  echo ">>> Patching rv_mcmc: 2j -> std::complex<double>(0.0, 2.0)"
  perl -i -pe 's/\b2j\b/std::complex<double>(0.0, 2.0)/g' "${RV}"
fi
# (b) Missing standard includes some toolchains need.
PHOTO="${SRC_DIR}/src/models/Photometry.cpp"
grep -q '#include <unordered_set>' "${PHOTO}" || \
  perl -0pi -e 's/(#include <QJsonObject>\n)/$1#include <unordered_set>\n/' "${PHOTO}" || true
SFIP="${SRC_DIR}/src/importWizard/SpectralFitImportPage.cpp"
grep -q '#include <charconv>' "${SFIP}" || \
  perl -0pi -e 's/(#include )/#include <charconv>\n$1/' "${SFIP}" || true
# (c) Strip the x86-only baseline-ISA flags from the top-level CMakeLists —
#     Apple Clang on arm64 rejects `-march=x86-64-v3`. Current trees guard those
#     flags with a CMAKE_SYSTEM_PROCESSOR check, so this is a no-op there; it
#     stays only for older checkouts. Patching them out would modify a *tracked*
#     file, which makes GitVersion.cmake stamp the build "git-<hash>-dirty" and
#     silently disables the updater — hence the guard is the preferred fix.
CML="${SRC_DIR}/CMakeLists.txt"
if grep -qE -- '^\s*-march=x86-64-v3' "${CML}" \
   && ! grep -q 'CMAKE_SYSTEM_PROCESSOR MATCHES' "${CML}"; then
  echo ">>> Removing x86-only -march/-mtune flags from CMakeLists.txt (ARM host)"
  echo "!!! This dirties the working tree; the build will self-report as a"
  echo "!!! development version unless ASTRA_VERSION_OVERRIDE is set."
  perl -i -ne 'print unless /^\s*-mtune=generic\s*$/ || /^\s*-march=x86-64-v3\b/' "${CML}"
fi
# (d) DIGGA: std::sqrt/std::log are constexpr only as a libstdc++ (GCC) extension;
#     Apple Clang's libc++ rejects the constexpr initializer. Use const instead.
DIGGA_RES="${SRC_DIR}/external/DIGGA/src/Resolution.cpp"
if grep -q 'constexpr double SIGMA_FROM_FWHM' "${DIGGA_RES}"; then
  echo ">>> Patching DIGGA Resolution.cpp: constexpr -> const (SIGMA_FROM_FWHM)"
  perl -i -pe 's/constexpr(\s+double\s+SIGMA_FROM_FWHM\b)/const$1/' "${DIGGA_RES}"
fi

# ── 7b. Ensure Info.plist.in exists (CMakeLists references it for the bundle) ─
# Written here as a fallback so the build works even if the file wasn't
# committed/transferred. CMake substitutes the ${MACOSX_BUNDLE_*} placeholders,
# so this heredoc is single-quoted to keep them literal.
if [[ ! -f "${SRC_DIR}/Info.plist.in" ]]; then
  echo ">>> Info.plist.in missing — writing a default one"
  cat > "${SRC_DIR}/Info.plist.in" <<'PLIST_EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>${MACOSX_BUNDLE_EXECUTABLE_NAME}</string>
	<key>CFBundleName</key>
	<string>${MACOSX_BUNDLE_BUNDLE_NAME}</string>
	<key>CFBundleDisplayName</key>
	<string>${MACOSX_BUNDLE_BUNDLE_NAME}</string>
	<key>CFBundleIdentifier</key>
	<string>${MACOSX_BUNDLE_GUI_IDENTIFIER}</string>
	<key>CFBundleVersion</key>
	<string>${MACOSX_BUNDLE_BUNDLE_VERSION}</string>
	<key>CFBundleShortVersionString</key>
	<string>${MACOSX_BUNDLE_SHORT_VERSION_STRING}</string>
	<key>CFBundleIconFile</key>
	<string>${MACOSX_BUNDLE_ICON_FILE}</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>NSHumanReadableCopyright</key>
	<string>${MACOSX_BUNDLE_COPYRIGHT}</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
	<key>LSMinimumSystemVersion</key>
	<string>12.0</string>
</dict>
</plist>
PLIST_EOF
fi

# ── 8. Configure & build ASTRA ──────────────────────────────────────────────
PREFIX_PATH="${QT_PREFIX};${BREW_PREFIX};${OPENBLAS_PREFIX};${LIBOMP};${CCFITS_PREFIX};${CFITSIO_PREFIX}"
# curl/zlib are keg-only in Homebrew: their .pc files are NOT linked under
# ${BREW_PREFIX}/lib/pkgconfig, and SEDplusplus hard-requires both via
# pkg_check_modules(... REQUIRED). Adding the keg prefixes to CMAKE_PREFIX_PATH
# is enough — FindPkgConfig appends <prefix>/lib/pkgconfig to pkg-config's
# search path (PKG_CONFIG_USE_CMAKE_PREFIX_PATH is ON by default).
PREFIX_PATH="${PREFIX_PATH};$(brew --prefix curl);$(brew --prefix zlib)"
# The deps cache dir must be a CMake prefix too: DIGGA locates unordered_dense.h
# via find_path, which searches <prefix>/include on CMAKE_PREFIX_PATH — the
# -isystem compile flag below is invisible to find_path.
PREFIX_PATH="${PREFIX_PATH};${CACHE}"

rm -rf "${BUILD_DIR}"
# Narrow extra include dirs only (no blanket -L/opt/homebrew/lib — that can put
# a Homebrew libc++ ahead of the system one and break the final link). fftw and
# the other deps resolve through proper CMake targets; cfitsio's header is the
# one thing pulled in by a bare include (via CCfits), so add just that dir.
EXTRA_INC="-isystem ${DEPS_INC} -isystem ${CCFITS_INCDIR} -isystem ${CFITSIO_PREFIX}/include"
# ASTRA's CMakeLists links a *bare* -lCCfits but never feeds the linker the
# library search dir. On Linux libCCfits sits in /usr/lib so the default search
# finds it; on macOS it's in a Homebrew keg / cache path the linker doesn't
# scan. Add the -L explicitly.
EXTRA_LINK="-L$(dirname "${CCFITS_LIBFILE}") -L${CFITSIO_PREFIX}/lib"
# Seed every CCfits/cfitsio cache variable the project tree consults, so both
# ASTRA's manual find_path/find_library fallback (pkg-config never succeeds on
# macOS — the brew CCfits ships no .pc) and DIGGA's `find_library(CCFITS_LIB
# CCfits REQUIRED)` resolve to the ONE CCfits provisioned above, on every
# machine, without depending on search-path luck.
CCFITS_ARGS=(
  -DCCFITS_INCLUDE_DIR="${CCFITS_INCDIR}"
  -DCCFITS_LIBRARY="${CCFITS_LIBFILE}"
  -DCFITSIO_LIBRARY="${CFITSIO_LIBFILE}"
  -DCCFITS_LIB="${CCFITS_LIBFILE}"
  -DCFITSIO_LIB="${CFITSIO_LIBFILE}"
)
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTRA_VERSION_OVERRIDE="${ASTRA_VERSION_OVERRIDE:-}" \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_PREFIX_PATH="${PREFIX_PATH}" \
  -DCMAKE_MODULE_PATH="${CMAKE_MODULES}" \
  -DCMAKE_Fortran_COMPILER="${FORTRAN_COMPILER}" \
  -DCMAKE_CXX_FLAGS="${EXTRA_INC}" \
  -DCMAKE_EXE_LINKER_FLAGS="${EXTRA_LINK}" \
  "${CCFITS_ARGS[@]}" \
  -DDIGGA_ENABLE_CUDA=OFF \
  -DBUILD_TESTING=OFF \
  -DOpenMP_ROOT="${LIBOMP}" \
  -DOpenMP_C_FLAGS="-Xpreprocessor -fopenmp -I${LIBOMP}/include" \
  -DOpenMP_C_LIB_NAMES="omp" \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I${LIBOMP}/include" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="${LIBOMP}/lib/libomp.dylib"

cmake --build "${BUILD_DIR}" -j"${JOBS}"

# ── 9. Locate the produced .app ─────────────────────────────────────────────
APP="$(find "${BUILD_DIR}" -maxdepth 2 -name 'ASTRA.app' -type d | head -1)"
[[ -n "${APP}" ]] || { echo "Build finished but ASTRA.app not found."; exit 1; }
echo ">>> Built bundle: ${APP}"

# ── 10. App icon (.icns) from the SVG/PNG ───────────────────────────────────
ICON_SRC=""
for cand in resources/linux/icons/astra.png resources/icons/astra.png resources/linux/icons/astra.svg; do
  [[ -f "${SRC_DIR}/${cand}" ]] && { ICON_SRC="${SRC_DIR}/${cand}"; break; }
done
if [[ -n "${ICON_SRC}" ]]; then
  echo ">>> Generating astra.icns from ${ICON_SRC##*/}"
  ICONSET="${BUILD_DIR}/astra.iconset"
  rm -rf "${ICONSET}"; mkdir -p "${ICONSET}"
  # Rasterise to a big square PNG first (sips can read PNG; SVG needs rsvg/qlmanage).
  BASE_PNG="${BUILD_DIR}/astra_1024.png"
  if [[ "${ICON_SRC}" == *.svg ]]; then
    if command -v rsvg-convert >/dev/null 2>&1; then
      rsvg-convert -w 1024 -h 1024 "${ICON_SRC}" -o "${BASE_PNG}"
    else
      # qlmanage ships with macOS; fall back to it for SVG -> PNG.
      qlmanage -t -s 1024 -o "${BUILD_DIR}" "${ICON_SRC}" >/dev/null 2>&1 || true
      mv "${BUILD_DIR}/$(basename "${ICON_SRC}").png" "${BASE_PNG}" 2>/dev/null || cp "${ICON_SRC}" "${BASE_PNG}"
    fi
  else
    sips -s format png -z 1024 1024 "${ICON_SRC}" --out "${BASE_PNG}" >/dev/null
  fi
  # iconutil only accepts the canonical iconset names; an unexpected file (e.g.
  # icon_64x64.png) makes it reject the whole set. 64px is covered by 32@2x.
  for sz in 16 32 128 256 512; do
    sips -z "${sz}" "${sz}"           "${BASE_PNG}" --out "${ICONSET}/icon_${sz}x${sz}.png"     >/dev/null
    sips -z "$((sz*2))" "$((sz*2))"   "${BASE_PNG}" --out "${ICONSET}/icon_${sz}x${sz}@2x.png"  >/dev/null
  done
  iconutil -c icns "${ICONSET}" -o "${APP}/Contents/Resources/astra.icns" || \
    echo "!!! iconutil failed — bundle will use a default icon."
else
  echo "!!! No icon source found — bundle will use a default icon."
fi

# ── 11. Bundle Qt + dylibs, sign, and package ───────────────────────────────
# Homebrew installs Qt6 as split kegs (qtbase, qtsvg, …). Some frameworks ASTRA
# pulls in transitively — QtPdf (QPrinter PDF export via qcustomplot) and the
# QtVirtualKeyboard input-context plugin — live in keg lib dirs that aren't in
# the rpath list macdeployqt derives from the binary, so it can't resolve them
# ("Cannot resolve rpath @rpath/QtPdf.framework"). Add the Qt lib/Frameworks
# dirs as temporary rpaths so macdeployqt finds & bundles them, then strip the
# absolute paths afterward to keep the .app relocatable.
EXE="${APP}/Contents/MacOS/ASTRA"
ADDED_RPATHS=()
for d in "${QT_PREFIX}/lib" "${QT_PREFIX}/Frameworks" \
         "${BREW_PREFIX}"/opt/qt*/lib "${BREW_PREFIX}"/opt/qt*/Frameworks; do
  [[ -d "${d}" ]] || continue
  if install_name_tool -add_rpath "${d}" "${EXE}" 2>/dev/null; then
    ADDED_RPATHS+=("${d}")
  fi
done

echo ">>> Running macdeployqt (bundling Qt frameworks + plugins)"
"${MACDEPLOYQT}" "${APP}" -always-overwrite

for d in ${ADDED_RPATHS[@]+"${ADDED_RPATHS[@]}"}; do
  install_name_tool -delete_rpath "${d}" "${EXE}" 2>/dev/null || true
done

# ── 11b. Bundle external helper programs (lcurve, lightcurvequery) ───────────
# ASTRA resolves these at runtime relative to the executable (Contents/MacOS):
#   lcurve         -> Contents/libexec/astra/lcurve   (ASTRA_LCURVE_BUNDLE_RELDIR)
#   lightcurvequery-> Contents/share/astra/lightcurvequery (ASTRA_LCQUERY_BUNDLE_RELDIR)
# These mirror what build-appimage.sh ships (steps 7b / install rules).

if [[ "${ASTRA_BUNDLE_LCURVE}" == "1" ]]; then
  # Non-fatal: lcurve_re is a separately-maintained native build. If it fails we
  # still ship a working .app — lcurve just falls back to Settings/PATH at run-
  # time. Re-run with ASTRA_BUNDLE_LCURVE=0 to skip it outright.
  if (
    set -e
    # lcurve_re is the forward model behind the fit dialog's model preview;
    # the solvers alone leave that preview broken (build-appimage.sh ships all four).
    LCURVE_BINS=(lcurve_levmarq lcurve_mcmc lcurve_simplex lcurve_re)
    LCURVE_SRC="${CACHE}/lcurve_re"
    LCURVE_BUILD="${LCURVE_SRC}/build"
    echo ">>> Building lcurve fitting binaries (${LCURVE_REF})"
    if [[ ! -d "${LCURVE_SRC}/.git" ]]; then
      rm -rf "${LCURVE_SRC}"
      git clone --depth 1 --branch "${LCURVE_REF}" "${LCURVE_REPO}" "${LCURVE_SRC}"
    else
      git -C "${LCURVE_SRC}" fetch --depth 1 origin "${LCURVE_REF}" && \
        git -C "${LCURVE_SRC}" checkout -q FETCH_HEAD || true
    fi
    # gnuplot-iostream is a header-only dep lcurve_re needs at build time (same as
    # the AppImage build). Fetch it once and point lcurve's CMake at it.
    GP_INC="${CACHE}/include"
    mkdir -p "${GP_INC}"
    [[ -f "${GP_INC}/gnuplot-iostream.h" ]] || \
      wget -q -O "${GP_INC}/gnuplot-iostream.h" \
        https://raw.githubusercontent.com/dstahlke/gnuplot-iostream/master/gnuplot-iostream.h
    # Build with the same Apple-Clang + libomp toolchain as ASTRA so the bundled
    # binaries share one libc++/libomp ABI. CUDA off (no NVIDIA on macOS).
    rm -rf "${LCURVE_BUILD}"
    cmake -S "${LCURVE_SRC}" -B "${LCURVE_BUILD}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/usr/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DCMAKE_PREFIX_PATH="${BREW_PREFIX};${LIBOMP}" \
      -DGNUPLOT_IOSTREAM_INCLUDE_DIR="${GP_INC}" \
      -DCMAKE_CXX_FLAGS="-isystem ${GP_INC}" \
      -DLCURVE_ENABLE_CUDA=OFF \
      -DOpenMP_ROOT="${LIBOMP}" \
      -DOpenMP_C_FLAGS="-Xpreprocessor -fopenmp -I${LIBOMP}/include" \
      -DOpenMP_C_LIB_NAMES="omp" \
      -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I${LIBOMP}/include" \
      -DOpenMP_CXX_LIB_NAMES="omp" \
      -DOpenMP_omp_LIBRARY="${LIBOMP}/lib/libomp.dylib"
    cmake --build "${LCURVE_BUILD}" --target "${LCURVE_BINS[@]}" -j"${JOBS}"

    LCURVE_DEST="${APP}/Contents/libexec/astra/lcurve"
    mkdir -p "${LCURVE_DEST}"
    for b in "${LCURVE_BINS[@]}"; do
      found="$(find "${LCURVE_BUILD}" -name "${b}" -type f -perm -u+x | head -1)"
      [[ -n "${found}" ]] || { echo "lcurve build missing ${b}"; exit 1; }
      cp -f "${found}" "${LCURVE_DEST}/"
      # Copy each binary's non-system dylib deps next to it and rewrite the load
      # paths to @loader_path/libs so the bundle is self-contained & relocatable.
      dylibbundler -cd -of -b -x "${LCURVE_DEST}/${b}" \
        -d "${LCURVE_DEST}/libs" -p "@loader_path/libs/" >/dev/null
    done
    echo ">>> lcurve bundled at Contents/libexec/astra/lcurve ($(ls "${LCURVE_DEST}" | tr '\n' ' '))"
  ); then
    :
  else
    echo "!!! lcurve build/bundle failed — shipping .app WITHOUT bundled lcurve."
    echo "!!! lcurve will be resolved from the Settings dialog / PATH at runtime."
  fi
fi

# sedfit (SEDplusplus) is built by the main cmake build; the Linux package gets
# it via `cmake --install`, but this script assembles the .app by hand, so copy
# it (plus its filter refdata) to the paths SedFitEnvironment.cpp resolves:
#   Contents/libexec/astra/sedfit/sedfit   (ASTRA_SEDFIT_BUNDLE_RELDIR)
#   Contents/share/astra/sedfit/refdata    (ASTRA_SEDFIT_REFDATA_RELDIR)
SEDFIT_BIN="$(find "${BUILD_DIR}/external/SEDplusplus" -name sedfit -type f -perm -u+x 2>/dev/null | head -1)"
if [[ -n "${SEDFIT_BIN}" ]]; then
  echo ">>> Bundling sedfit"
  SEDFIT_DEST="${APP}/Contents/libexec/astra/sedfit"
  mkdir -p "${SEDFIT_DEST}"
  cp -f "${SEDFIT_BIN}" "${SEDFIT_DEST}/"
  dylibbundler -cd -of -b -x "${SEDFIT_DEST}/sedfit" \
    -d "${SEDFIT_DEST}/libs" -p "@loader_path/libs/" >/dev/null
  REFDATA_SRC="${SRC_DIR}/resources/sedfit/refdata"
  if [[ -d "${REFDATA_SRC}" ]]; then
    REFDATA_DEST="${APP}/Contents/share/astra/sedfit/refdata"
    rm -rf "${REFDATA_DEST}"; mkdir -p "${REFDATA_DEST}"
    cp -R "${REFDATA_SRC}/." "${REFDATA_DEST}/"
  fi
else
  echo "!!! sedfit binary not found in build tree — SED fitting will need a user-installed sedfit."
fi

if [[ "${ASTRA_BUNDLE_LCQUERY}" == "1" ]]; then
  LCQ_SRC="${SRC_DIR}/external/lightcurvequery"
  if [[ -f "${LCQ_SRC}/lightcurvequery.py" ]]; then
    echo ">>> Bundling lightcurvequery Python sources"
    LCQ_DEST="${APP}/Contents/share/astra/lightcurvequery"
    rm -rf "${LCQ_DEST}"; mkdir -p "${LCQ_DEST}"
    # Mirror the install(DIRECTORY ... EXCLUDE) filters from CMakeLists.txt so we
    # ship clean read-only sources (no VCS, venv, caches or per-run outputs).
    rsync -a \
      --exclude '.git*' --exclude '.venv' --exclude '.idea' \
      --exclude '__pycache__' --exclude 'lightcurves' --exclude 'mastDownload' \
      --exclude 'lcplots' --exclude 'pgramplots' --exclude 'rvplots' \
      "${LCQ_SRC}/" "${LCQ_DEST}/"
  else
    echo "!!! lightcurvequery submodule missing — skipping (run: git submodule update --init)"
  fi
fi

# ── 11c. Stage ISIS into the bundle ─────────────────────────────────────────
# Mirrors step 3c of build-appimage.sh. The ISIS core comes from the cached
# prefix build-macos-isis.sh maintains; the script libraries (isisscripts,
# stellar_isisscripts) change often, so they are cloned at HEAD and built on
# every run, exactly as the AppImage does.
#
# ISIS only *reads* its tree at runtime, so it ships as data under
# Contents/share/astra/isis and runs in place. ASTRA points it there via
# ISIS_SRCDIR / SLSH_PATH / SLANG_MODULE_PATH / PGPLOT_DIR and a private
# .isisrc (src/utils/IsisEnvironment.cpp) — the reldir CMake bakes in resolves
# to Contents/share/astra/isis from Contents/MacOS, and the isis binary is
# found via IsisEnvironment's <root>/bin/isis fallback.
if [[ "${ASTRA_BUNDLE_ISIS}" == "1" ]]; then
  # The subshell below must be a *standalone* command with errexit disabled
  # around it. Bash propagates "errexit is ignored here" into a subshell used as
  # an `if` condition or as an operand of &&/||, so the older
  # `if ( set -e; ... ); then` form silently ran on past every failure and took
  # its exit status from the last echo — that is how a .dmg with no isis binary,
  # no isisscripts and no stellar_isisscripts shipped as a green build.
  set +e
  (
    set -e
    ISIS_PREFIX="${HOME}/.cache/astra-build-macos-isis/prefix"
    [[ -x "${SRC_DIR}/build-macos-isis.sh" ]] \
      || { echo "build-macos-isis.sh missing or not executable"; exit 1; }
    "${SRC_DIR}/build-macos-isis.sh" "${ISIS_PREFIX}"

    # --- Script libraries: always latest HEAD, built fresh each run ---
    ISIS_SCRIPTS_SRC="${BUILD_DIR}/isis_scripts"
    rm -rf "${ISIS_SCRIPTS_SRC}"; mkdir -p "${ISIS_SCRIPTS_SRC}"
    export PATH="${ISIS_PREFIX}/bin:${PATH}"

    # isisscripts (Remeis) moved off the old /git.public gitweb (dumb HTTP, now
    # 404) to the Remeis GitLab, which speaks smart HTTP — so --depth 1 works.
    ( cd "${ISIS_SCRIPTS_SRC}"
      git clone --depth 1 \
        https://www.sternwarte.uni-erlangen.de/gitlab/remeis/isisscripts.git isisscripts
      ( cd isisscripts && make )
      # stellar's `make` runs bin/makestatic, which needs Perl's File::Slurp.
      # It is not core, and macOS runners don't ship it — without it the make
      # dies at share/stellar_isisscripts.sl.
      perl -MFile::Slurp -e1 >/dev/null 2>&1 || cpanm --notest File::Slurp
      git clone --depth 1 \
        http://www.sternwarte.uni-erlangen.de/gitlab/irrgang/stellar.git stellar_isisscripts
      ( cd stellar_isisscripts && make )
      if [[ -f stellar_isisscripts/slirp/c_functions.h ]]; then
        ( cd stellar_isisscripts/slirp
          "${ISIS_PREFIX}/bin/slirp" -make -lm -lgsl -lgslcblas -lpthread \
            c_functions.h c_functions.o
          [[ -f Makefile ]] && make ) || echo "WARN: stellar c_functions build failed (continuing)"
      fi )

    ISIS_STAGE="${APP}/Contents/share/astra/isis"
    rm -rf "${ISIS_STAGE}"
    mkdir -p "${ISIS_STAGE}/srcdir" "${ISIS_STAGE}/bin"
    cp -a "${ISIS_PREFIX}"/isis/*/. "${ISIS_STAGE}/srcdir/"  # ISIS_SRCDIR
    cp -a "${ISIS_PREFIX}/lib"      "${ISIS_STAGE}/"         # libslang + slang modules
    cp -a "${ISIS_PREFIX}/share"    "${ISIS_STAGE}/"         # share/slsh
    cp -a "${ISIS_PREFIX}/pgplot"   "${ISIS_STAGE}/"         # grfont.dat, rgb.txt
    cp -a "${ISIS_PREFIX}/bin/isis" "${ISIS_STAGE}/bin/"
    mkdir -p "${ISIS_STAGE}/share/slsh/local-packages"
    for s in isisscripts stellar_isisscripts; do
      cp -a "${ISIS_SCRIPTS_SRC}/${s}" "${ISIS_STAGE}/${s}"
      rm -rf "${ISIS_STAGE}/${s}/.git"
    done
    # Build leftovers: object files, and the static PGPLOT archives that were
    # only ever needed to link the pgplot module.
    find "${ISIS_STAGE}" \( -name '*.o' -o -name '*.a' \) -delete 2>/dev/null || true

    # --- Self-contain the Mach-O files ---------------------------------------
    # The AppImage gets this free from linuxdeploy; macOS has no equivalent
    # that walks a directory tree, so run dylibbundler over every binary and
    # dlopened module. They sit at different depths, so each one needs its own
    # @loader_path prefix pointing back at the single shared libs/ dir.
    ISIS_LIBS="${ISIS_STAGE}/libs"
    mkdir -p "${ISIS_LIBS}"
    while IFS= read -r f; do
      # Skip anything that isn't actually Mach-O (scripts, data, .sl files).
      file -b "${f}" | grep -q 'Mach-O' || continue
      rel="$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))' \
             "${ISIS_LIBS}" "${f}")"
      dylibbundler -cd -of -b -x "${f}" -d "${ISIS_LIBS}" -p "@loader_path/${rel}/" >/dev/null
    done < <(find "${ISIS_STAGE}" -type f \( -name '*.so' -o -name '*.dylib' -o -perm -u+x \) \
                  -not -path "${ISIS_LIBS}/*")

    echo ">>> ISIS staged at Contents/share/astra/isis ($(du -sh "${ISIS_STAGE}" | cut -f1))"
  )
  ISIS_STATUS=$?
  set -e
  if (( ISIS_STATUS != 0 )); then
    echo "!!! ISIS build/bundle failed (exit ${ISIS_STATUS}) — the .app has NO bundled ISIS."
    echo "!!! ISIS-backed fitting would fall back to a user-installed isis on PATH."
    rm -rf "${APP}/Contents/share/astra/isis"
    # Release builds must not ship a .dmg that quietly lacks ISIS; local builds
    # keep the soft fallback (ASTRA_REQUIRE_ISIS=0).
    if [[ "${ASTRA_REQUIRE_ISIS}" == "1" ]]; then
      echo "!!! ASTRA_REQUIRE_ISIS=1 — refusing to package a .dmg without ISIS."
      exit 1
    fi
  fi
fi

# Ad-hoc (self-)signature so Gatekeeper reports "unidentified developer"
# (right-click → Open works) instead of "damaged and can't be opened".
echo ">>> Ad-hoc code-signing the bundle"
codesign --force --deep --sign - "${APP}" || echo "!!! codesign failed (non-fatal)"

# ── 12. Build the .dmg with hdiutil (built into macOS, no extra deps) ───────
OUT_DMG="${SRC_DIR}/astra-${VERSION}-arm64.dmg"
STAGE="${BUILD_DIR}/dmg-stage"
rm -rf "${STAGE}" "${OUT_DMG}"; mkdir -p "${STAGE}"
cp -R "${APP}" "${STAGE}/"
ln -s /Applications "${STAGE}/Applications"     # drag-to-install affordance
echo ">>> Creating ${OUT_DMG##*/}"
hdiutil create -volname "ASTRA ${VERSION}" -srcfolder "${STAGE}" \
  -ov -format UDZO "${OUT_DMG}" >/dev/null
# Bare file name in the checksum file (not the build machine's absolute path),
# so `shasum -c` works next to a downloaded .dmg.
(cd "$(dirname "${OUT_DMG}")" && shasum -a 256 "$(basename "${OUT_DMG}")") \
  > "${OUT_DMG}.sha256"

echo
echo "=== Build complete ==="
ls -lh "${OUT_DMG}" "${OUT_DMG}.sha256"
echo
echo "Test locally:    open \"${APP}\""
echo "Ship the .dmg:   astra-${VERSION}-arm64.dmg"
echo
echo "Note: the .dmg is ad-hoc signed, not notarized. On first launch the"
echo "recipient must right-click ASTRA.app → Open (once) to bypass Gatekeeper,"
echo "or run:  xattr -dr com.apple.quarantine /Applications/ASTRA.app"
