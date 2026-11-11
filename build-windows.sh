#!/usr/bin/env bash
# Build a self-contained ASTRA installer (.exe) for 64-bit Windows.
#
# One installer serves every machine: ASTRA is compiled once per x86-64 level
# in ASTRA_WIN_VARIANTS, the installer carries all of them (files that come out
# identical are stored once), and on the target it runs a small CPU probe and
# copies the fastest build that CPU can execute. On ARM it installs the lowest
# level, which runs under Windows 11's x64 emulation.
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
#   ASTRA_WIN_VARIANTS     = x86-64 levels to build and pack, space separated
#                            (default: "x86-64-v3 x86-64"). Each is one full
#                            compile of ASTRA, GAEL, sedfit and lcurve with
#                            that -march. x86-64-v3 is AVX2/FMA; x86-64 is the
#                            plain baseline every 64-bit CPU runs. Keep x86-64
#                            in the list, or the installer has to refuse the
#                            machines below the lowest level it carries.
#   ASTRA_BUNDLE_LCURVE=0  = do not build/bundle the lcurve fitting binaries
#   LCURVE_REPO / LCURVE_REF
#                          = source and ref for that build (defaults: the
#                            public repo, main). Its -march follows the variant.
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
#   • ISIS — Unix-only, and SED fitting no longer needs it: the bundled
#     `sedfit` (SEDplusplus) replaced it and does build here.
#   • CUDA: lcurve is built with LCURVE_ENABLE_CUDA=OFF. The runners have no
#     NVIDIA toolkit, and the fit dialog's GPU switch degrades to CPU on its
#     own when the binaries carry no kernels.
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
# Pinned by hash, so a mirror is as good as the original. HEASARC served a
# certificate for the wrong host name in 2026-10, which failed every cold
# build; Fedora's source cache keeps the identical file (SHA-256 matches
# Homebrew's ccfits formula).
CCFITS_SHA256="f63546d2feecbf732cc08aaaa80a2eb5334ada37fb2530181b7363a5dbdeb01a"
CCFITS_URLS=(
  "https://heasarc.gsfc.nasa.gov/FTP/software/fitsio/ccfits/v2.7/CCfits-2.7.tar.gz"
  "https://src.fedoraproject.org/repo/pkgs/CCfits/CCfits-2.7.tar.gz/sha512/5cb802f41cf0695d0e49924ee163151ee657b93158246766d04c192518c7bed30383405d87b5fb312f5f44af26d5ede3104fab90d93cc232e950f8ae66050fde/CCfits-2.7.tar.gz"
)
OUT_BASENAME="astra-${VERSION}-x86_64-setup"

# The payloads to build. Names are -march values and x86-64 psABI levels at the
# same time, which is what lets the installer compare them against the level
# its CPU probe reports (resources/windows/cpulevel.c).
read -r -a VARIANTS <<< "${ASTRA_WIN_VARIANTS:-x86-64-v3 x86-64}"
variant_level() {  # x86-64 -> 1, x86-64-vN -> N
  case "$1" in
    x86-64)        echo 1 ;;
    x86-64-v[234]) echo "${1##*-v}" ;;
    *)             return 1 ;;
  esac
}

# ── 0. Sanity checks ────────────────────────────────────────────────────────
[[ -f "${SRC_DIR}/CMakeLists.txt" ]] || { echo "Run from the ASTRA repo root."; exit 1; }
if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
  echo "MSYSTEM is '${MSYSTEM:-unset}', expected UCRT64."
  echo "Start the 'MSYS2 UCRT64' shell (not MSYS / MINGW64 / CLANG64) and re-run."
  exit 1
fi

((${#VARIANTS[@]})) || { echo "ASTRA_WIN_VARIANTS is empty."; exit 1; }
declare -A _seen_variant=()
for _v in "${VARIANTS[@]}"; do
  variant_level "${_v}" >/dev/null || {
    echo "ASTRA_WIN_VARIANTS: '${_v}' is not one of x86-64, x86-64-v2, x86-64-v3, x86-64-v4."
    exit 1
  }
  [[ -z "${_seen_variant[${_v}]:-}" ]] || { echo "ASTRA_WIN_VARIANTS lists '${_v}' twice."; exit 1; }
  _seen_variant[${_v}]=1
done
[[ -n "${_seen_variant[x86-64]:-}" ]] || \
  echo "!!! ASTRA_WIN_VARIANTS has no x86-64 baseline: the installer will refuse older CPUs."

echo ">>> ASTRA ${VERSION}  |  ${MSYSTEM}  |  $(uname -s) $(uname -r)  |  ${VARIANTS[*]}"

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
  mingw-w64-ucrt-x86_64-fftw            # src/rv/Periodogram.cpp
  mingw-w64-ucrt-x86_64-cfitsio
  mingw-w64-ucrt-x86_64-nlohmann-json
  mingw-w64-ucrt-x86_64-cxxopts
  mingw-w64-ucrt-x86_64-unordered_dense
  mingw-w64-ucrt-x86_64-gsl             # SEDplusplus (sedfit)
  mingw-w64-ucrt-x86_64-curl            #   "
  mingw-w64-ucrt-x86_64-zlib            #   "
  mingw-w64-ucrt-x86_64-zstd            # src/app/ArrayCodec.cpp (data files)
  mingw-w64-ucrt-x86_64-python          # GAEL: find_package(Python3 ... REQUIRED)
  mingw-w64-ucrt-x86_64-python-numpy    #   "
)
if [[ "${ASTRA_SKIP_PACMAN:-0}" != "1" ]]; then
  echo ">>> Installing MSYS2 packages"
  # MSYS2 mirrors rotate packages out from under a local database, which
  # surfaces as a 404 on a file pacman was just told exists, and they stall
  # often enough to matter. Refresh the database and retry: losing a build to a
  # transient download is worse than the twenty seconds this costs.
  for _attempt in 1 2 3; do
    pacman -Sy --noconfirm >/dev/null 2>&1 || true
    if pacman -S --needed --noconfirm "${PACMAN_PKGS[@]}"; then
      break
    fi
    if ((_attempt == 3)); then
      echo "::error::pacman could not install the toolchain after 3 attempts"
      exit 1
    fi
    echo ">>> pacman attempt ${_attempt} failed; refreshing and retrying"
    sleep 15
  done
fi

mkdir -p "${CACHE}"

# ── 1b. Native-path pkg-config metadata ─────────────────────────────────────
# Some MSYS2 .pc files hard-code POSIX prefixes (cfitsio: includedir=
# "/ucrt64/include"), which resolve only when a command line passes through the
# MSYS2 runtime's argument conversion. CMake and Ninja spawn the compiler
# natively, so nothing converts them, and pkg_check_modules(... IMPORTED_TARGET)
# fails outright with "includes non-existent path /ucrt64/include". Others
# (gsl) derive their prefix from ${pcfiledir} and already come out native, so
# PKG_CONFIG_SYSROOT_DIR is the wrong tool -- it prefixes blindly and turns
# those into D:/.../D:/.../ucrt64/include.
#
# So rewrite the metadata instead: copy every .pc into a private directory with
# a leading /ucrt64 replaced by the native prefix. The anchor is what keeps the
# already-native files untouched -- their "/ucrt64" is preceded by "msys64",
# not by a delimiter or an -I/-L.
PC_DIR="${BUILD_DIR}/pkgconfig-native"
UCRT_NATIVE="$(cygpath -m /ucrt64)"
rm -rf "${PC_DIR}"
mkdir -p "${PC_DIR}"
for _pc in /ucrt64/lib/pkgconfig/*.pc /ucrt64/share/pkgconfig/*.pc; do
  [[ -f "${_pc}" ]] || continue
  sed -E "s#(^|[=\" ]|-I|-L)/ucrt64#\1${UCRT_NATIVE}#g" \
      "${_pc}" > "${PC_DIR}/$(basename "${_pc}")"
done
# LIBDIR (not just PATH) so the unpatched originals are out of the search path
# entirely and cannot be reached through a Requires: line.
export PKG_CONFIG_LIBDIR="${PC_DIR}"
export PKG_CONFIG_PATH="${PC_DIR}"

# Fail here rather than in CMake's generate step, where the message names a
# target instead of the cause.
for _mod in cfitsio gsl libcurl zlib libzstd nlohmann_json fftw3; do
  _flags="$(pkg-config --cflags --libs "${_mod}" 2>&1)" || {
    echo "::error::pkg-config cannot resolve ${_mod}: ${_flags}"; exit 1; }
  case "${_flags}" in
    *-I/*|*-L/*)
      echo "::error::${_mod}.pc still reports POSIX paths: ${_flags}"; exit 1 ;;
    *"${UCRT_NATIVE}${UCRT_NATIVE}"*|*"$(cygpath -m /)${UCRT_NATIVE}"*)
      echo "::error::${_mod}.pc paths were double-prefixed: ${_flags}"; exit 1 ;;
  esac
done
echo ">>> pkg-config metadata rewritten to native paths in ${PC_DIR}"

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
  ccfits_ok=0
  for url in "${CCFITS_URLS[@]}"; do
    if curl -fsSL -o "${CACHE}/ccfits/src.tgz" "${url}" &&
       echo "${CCFITS_SHA256}  ${CACHE}/ccfits/src.tgz" | sha256sum -c --status; then
      ccfits_ok=1
      break
    fi
    echo "!!! CCfits download failed or did not match its checksum: ${url}"
  done
  [[ "${ccfits_ok}" == "1" ]] || { echo "No CCfits source could be fetched"; exit 1; }
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

# ── 4. Shared setup for the per-variant builds ──────────────────────────────
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

# Every compile command of a variant has to carry that variant's -march and no
# other, and below x86-64-v3 nothing may switch a newer instruction set back on
# for a single file. Nobody builds these on the old CPU they are meant for, so
# a stray flag (a submodule's -march=native, a per-file -mavx2) would ship
# unnoticed and surface as an illegal-instruction crash on a user's machine.
# Checked against the compile database, where it is exact.
assert_isa_flags() {  # $1 = compile_commands.json, $2 = variant
  local cc="$1" variant="$2" found
  [[ -f "${cc}" ]] || { echo "::error::no compile database at ${cc}"; exit 1; }
  found="$({ grep -oE -- '-m(arch|cpu)=[A-Za-z0-9_.+-]+' "${cc}" || true; } | sort -u | tr '\n' ' ')"
  [[ "${found}" == "-march=${variant} " ]] || {
    echo "::error::${cc}: expected only -march=${variant}, found: ${found:-none}"; exit 1; }
  if (($(variant_level "${variant}") < 3)); then
    found="$({ grep -oE -- '-m(avx|fma|bmi|f16c|lzcnt|movbe|xsave)[A-Za-z0-9_.]*' "${cc}" || true; } | sort -u | tr '\n' ' ')"
    [[ -z "${found}" ]] || {
      echo "::error::${cc}: ${variant} build compiles files with ${found}"; exit 1; }
  fi
}

# ── 4b. lcurve sources (built per variant in step 5b) ───────────────────────
# ASTRA shells out to lcurve_levmarq / lcurve_mcmc / lcurve_simplex to fit a
# light curve, and to lcurve_re (the forward model) for the fit dialog's model
# preview. Built from source here, exactly as build-appimage.sh and
# build-macos.sh do, so every release picks up the current lcurve_re HEAD.
if [[ "${ASTRA_BUNDLE_LCURVE:-1}" == "1" ]]; then
  LCURVE_REPO="${LCURVE_REPO:-https://github.com/Fabmat1/lcurve_re.git}"
  LCURVE_REF="${LCURVE_REF:-main}"
  LCURVE_SRC="${CACHE}/lcurve_re"
  LCURVE_BINS=(lcurve_levmarq lcurve_mcmc lcurve_simplex lcurve_re)

  echo ">>> Fetching lcurve (${LCURVE_REF})"
  if [[ -d "${LCURVE_SRC}/.git" ]]; then
    # reset --hard, not checkout: the cached clone still carries the previous
    # run's -march rewrite in its working tree, and a plain checkout refuses to
    # move past a locally modified file the new commit also touches.
    git -C "${LCURVE_SRC}" fetch --depth 1 origin "${LCURVE_REF}"
    git -C "${LCURVE_SRC}" reset -q --hard FETCH_HEAD
  else
    rm -rf "${LCURVE_SRC}"
    git clone --depth 1 --branch "${LCURVE_REF}" "${LCURVE_REPO}" "${LCURVE_SRC}"
  fi
  # Build tree of the single-variant layout this script used to have; it sits
  # in the CI cache and would only take up room there.
  rm -rf "${LCURVE_SRC}/build-windows"

  # gnuplot-iostream is a header-only build dependency (new_helpers.h includes
  # it unconditionally). MSYS2 has no package for it, and ASTRA never lets
  # lcurve start a gnuplot process anyway - it drives the "stream" plot device,
  # which writes JSON frames to stdout. Same fetch the macOS build does.
  GP_INC="${CACHE}/include"
  mkdir -p "${GP_INC}"
  [[ -f "${GP_INC}/gnuplot-iostream.h" ]] || \
    curl -fsSL -o "${GP_INC}/gnuplot-iostream.h" \
      https://raw.githubusercontent.com/dstahlke/gnuplot-iostream/master/gnuplot-iostream.h
  GP_INC_NATIVE="$(cygpath -m "${GP_INC}")"
else
  echo ">>> ASTRA_BUNDLE_LCURVE=0 - light-curve fitting will need a user-installed lcurve"
fi

# ── 4c. DLL closure helpers (used per variant in step 7) ────────────────────
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

# ── 5-7. Build, stage and deploy one complete tree per variant ──────────────
# Each variant gets its own build directory and its own staged tree, and each
# tree is finished and checked as if it were the only one: Qt deployed, DLL
# closure resolved, imports verified. Step 8 then folds what the trees have in
# common back together.
rm -rf "${STAGE_DIR}"

for VARIANT in "${VARIANTS[@]}"; do
  V_BUILD="${BUILD_DIR}/${VARIANT}"
  V_STAGE="${STAGE_DIR}/${VARIANT}"
  BIN_DIR="${V_STAGE}/bin"
  echo
  echo ">>> ── Variant ${VARIANT} ──────────────────────────────────────────"

  # ── 5. Configure, build, install ──────────────────────────────────────────
  cmake -S "${SRC_DIR}" -B "${V_BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DASTRA_X86_MARCH="${VARIANT}" \
    -DASTRA_VERSION_OVERRIDE="${ASTRA_VERSION_OVERRIDE:-}" \
    -DCMAKE_PREFIX_PATH="${CCFITS_PREFIX}" \
    -DPython3_ROOT_DIR="${UCRT_PREFIX}" \
    -DPython3_EXECUTABLE="${UCRT_PREFIX}/bin/python3.exe" \
    -DPython3_FIND_STRATEGY=LOCATION \
    -DPython3_FIND_REGISTRY=NEVER \
    -DCMAKE_INSTALL_PREFIX="${V_STAGE}" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DBUILD_TESTING=OFF
  assert_isa_flags "${V_BUILD}/compile_commands.json" "${VARIANT}"

  # -k 0 keeps going after the first error. A Windows build breaking is almost
  # always a batch of related portability errors, and one CI round that reports
  # all of them beats N rounds that report one each.
  cmake --build "${V_BUILD}" -j "${JOBS}" -- -k 0
  cmake --install "${V_BUILD}"

  [[ -f "${BIN_DIR}/ASTRA.exe" ]] || { echo "No ASTRA.exe in ${BIN_DIR}"; exit 1; }

  # ── 5b. lcurve fitting binaries ───────────────────────────────────────────
  # They land in libexec/astra/lcurve, which is where ASTRA looks: the runtime
  # path comes from ASTRA_LCURVE_BUNDLE_RELDIR ("../libexec/astra/lcurve"),
  # computed from the install layout in CMakeLists.txt. Step 7 gives the
  # directory its own DLL closure, since Windows resolves imports next to the
  # .exe and these do not sit beside ASTRA.exe.
  if [[ "${ASTRA_BUNDLE_LCURVE:-1}" == "1" ]]; then
    LCURVE_BUILD="${LCURVE_SRC}/build-windows-${VARIANT}"
    echo ">>> Building lcurve (${LCURVE_REF}, ${VARIANT})"

    # lcurve_re's Release flags hardcode -march=native, which would tune the
    # binaries to whatever CPU the runner happened to have. Pin this variant's
    # baseline instead. The checkout first undoes the previous variant's
    # rewrite, which otherwise leaves no -march=native for sed to find.
    git -C "${LCURVE_SRC}" checkout -q -- CMakeLists.txt
    sed -i "s/-march=native/-march=${VARIANT}/" "${LCURVE_SRC}/CMakeLists.txt"
    grep -q -- "-march=native" "${LCURVE_SRC}/CMakeLists.txt" \
      && { echo "::error::lcurve_re still requests -march=native"; exit 1; }

    cmake -S "${LCURVE_SRC}" -B "${LCURVE_BUILD}" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DLCURVE_ENABLE_CUDA=OFF \
      -DGNUPLOT_IOSTREAM_INCLUDE_DIR="${GP_INC_NATIVE}" \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    assert_isa_flags "${LCURVE_BUILD}/compile_commands.json" "${VARIANT}"
    cmake --build "${LCURVE_BUILD}" -j "${JOBS}" --target "${LCURVE_BINS[@]}"

    LCURVE_DEST="${V_STAGE}/libexec/astra/lcurve"
    mkdir -p "${LCURVE_DEST}"
    for b in "${LCURVE_BINS[@]}"; do
      [[ -f "${LCURVE_BUILD}/${b}.exe" ]] \
        || { echo "::error::lcurve build produced no ${b}.exe"; exit 1; }
      cp -f "${LCURVE_BUILD}/${b}.exe" "${LCURVE_DEST}/"
    done
    echo ">>> lcurve staged at libexec/astra/lcurve ($(ls "${LCURVE_DEST}" | tr '\n' ' '))"
  fi

  # ── 6. Qt deployment ──────────────────────────────────────────────────────
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

  # ── 7. Non-Qt DLL closure ─────────────────────────────────────────────────
  echo ">>> Resolving DLL dependencies"
  # Every directory that holds an executable needs its own closure: Windows
  # resolves imports next to the .exe, and the helper binaries live in their own
  # libexec subdirectories, not beside ASTRA.exe.
  while read -r exe_dir; do
    echo "    ${exe_dir#${V_STAGE}/}"
    deploy_dll_closure "${exe_dir}"
  done < <(find "${V_STAGE}" -name '*.exe' -printf '%h\n' | sort -u)

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
  done < <(find "${V_STAGE}" -name '*.exe' -printf '%h\n' | sort -u)
  ((missing == 0)) || exit 1

  echo ">>> Staged tree (${VARIANT}): $(du -sh "${V_STAGE}" | cut -f1)"
done

# ── 8. Fold the variants together ───────────────────────────────────────────
# The trees differ only in what was compiled here (ASTRA.exe, sedfit.exe, the
# lcurve binaries). Qt, the MSYS2 DLLs and all the data are byte-identical
# copies, so packing every tree whole would roughly double the download for
# nothing. Every file that is the same in all variants moves to common/, which
# the installer copies unconditionally; what stays behind in a variant's
# directory is exactly what that variant overrides. Compared by content, not by
# a list of names, so nothing here needs to know which files a build produces.
#
# After this step the per-variant directories are no longer runnable trees:
# install common/ and one variant over each other to get one, as setup does.
COMMON_DIR="${STAGE_DIR}/common"
fold_common_files() {
  local first="${VARIANTS[0]}" rel v same
  mkdir -p "${COMMON_DIR}"
  # Keep the directory skeleton, so a directory that is empty by design still
  # gets created on install once the pruning below has removed it per variant.
  while IFS= read -r -d '' rel; do
    mkdir -p "${COMMON_DIR}/${rel}"
  done < <(cd "${STAGE_DIR}/${first}" && find . -mindepth 1 -type d -printf '%P\0')
  while IFS= read -r -d '' rel; do
    same=1
    for v in "${VARIANTS[@]:1}"; do
      cmp -s "${STAGE_DIR}/${first}/${rel}" "${STAGE_DIR}/${v}/${rel}" || { same=0; break; }
    done
    ((same)) || continue
    mv "${STAGE_DIR}/${first}/${rel}" "${COMMON_DIR}/${rel}"
    for v in "${VARIANTS[@]:1}"; do
      rm -f "${STAGE_DIR}/${v}/${rel}"
    done
  done < <(cd "${STAGE_DIR}/${first}" && find . -type f -printf '%P\0')
  for v in "${VARIANTS[@]}"; do
    find "${STAGE_DIR}/${v}" -mindepth 1 -depth -type d -empty -delete
  done
}

echo
echo ">>> Folding files shared by all variants into common/"
fold_common_files
echo "    common: $(du -sh "${COMMON_DIR}" | cut -f1)"
for _v in "${VARIANTS[@]}"; do
  echo "    ${_v}: $(du -sh "${STAGE_DIR}/${_v}" | cut -f1), $(find "${STAGE_DIR}/${_v}" -type f | wc -l) file(s)"
done
# With more than one variant, each must still own its ASTRA.exe: if the
# executables compared equal, the -march never reached the compiler.
if ((${#VARIANTS[@]} > 1)); then
  for _v in "${VARIANTS[@]}"; do
    [[ -f "${STAGE_DIR}/${_v}/bin/ASTRA.exe" ]] || {
      echo "::error::variant ${_v} has no ASTRA.exe of its own after folding"; exit 1; }
  done
fi

# ── 9. CPU probe ────────────────────────────────────────────────────────────
# What the installer runs to decide which variant to copy. Always compiled for
# the plain baseline and linked statically, since it has to run on the very
# machines the choice exists for, before anything is installed.
PROBE="${BUILD_DIR}/astra-cpulevel.exe"
gcc -Os -Wall -Wextra -march=x86-64 -mtune=generic -static -s \
    -o "${PROBE}" "${SRC_DIR}/resources/windows/cpulevel.c"
while read -r dll; do
  [[ -f "/ucrt64/bin/${dll}" ]] && {
    echo "::error::astra-cpulevel.exe imports ${dll}; it must depend on system DLLs only"; exit 1; }
done < <(imports_of "${PROBE}")
probe_rc=0
"${PROBE}" || probe_rc=$?
((probe_rc >= 11 && probe_rc <= 14)) || {
  echo "::error::astra-cpulevel.exe exited with ${probe_rc}, expected 11 to 14"; exit 1; }
echo ">>> CPU probe built; this machine is x86-64 level $((probe_rc - 10))"

# ── 10. Installer ───────────────────────────────────────────────────────────
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

# One [Files] line per variant that kept files of its own, each guarded by the
# installer's IsVariant() check. Written to a file the script #includes rather
# than substituted into it, because it is a variable number of lines.
VARIANT_FILES_ISS="${BUILD_DIR}/astra-variant-files.iss"
: > "${VARIANT_FILES_ISS}"
for _v in "${VARIANTS[@]}"; do
  [[ -n "$(find "${STAGE_DIR}/${_v}" -type f -print -quit)" ]] || continue
  printf 'Source: "%s\\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion; Check: IsVariant('"'%s'"')\n' \
    "$(cygpath -w "${STAGE_DIR}/${_v}")" "${_v}" >> "${VARIANT_FILES_ISS}"
done

# Windows paths are full of backslashes, and a backslash in a sed replacement
# is an escape -- "C:\Users\..." would silently become "C:Users...", and a \U
# would upper-case the rest of the line. Double them before substituting.
sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

ISS="${BUILD_DIR}/astra.iss"
sed -e "s|@VERSION@|$(sed_escape "${VERSION}")|g" \
    -e "s|@COMMON_DIR@|$(sed_escape "$(cygpath -w "${COMMON_DIR}")")|g" \
    -e "s|@VARIANTS@|$(sed_escape "${VARIANTS[*]}")|g" \
    -e "s|@VARIANT_FILES@|$(sed_escape "$(cygpath -w "${VARIANT_FILES_ISS}")")|g" \
    -e "s|@PROBE@|$(sed_escape "$(cygpath -w "${PROBE}")")|g" \
    -e "s|@ICON@|$(sed_escape "$(cygpath -w "${SRC_DIR}/resources/windows/astra.ico")")|g" \
    -e "s|@OUT_DIR@|$(sed_escape "$(cygpath -w "${SRC_DIR}")")|g" \
    -e "s|@OUT_BASENAME@|$(sed_escape "${OUT_BASENAME}")|g" \
    "${SRC_DIR}/resources/windows/astra.iss.in" > "${ISS}"
if grep -n '@[A-Z_]*@' "${ISS}"; then
  echo "::error::unsubstituted token(s) left in ${ISS}"; exit 1
fi

"${ISCC_BIN}" "$(cygpath -w "${ISS}")"

OUT_EXE="${SRC_DIR}/${OUT_BASENAME}.exe"
[[ -f "${OUT_EXE}" ]] || { echo "Inno Setup produced no ${OUT_BASENAME}.exe"; exit 1; }

# ── 11. Checksum ────────────────────────────────────────────────────────────
# Same "<hex>  <filename>" shape the AppImage and .dmg publish, because
# UpdateManager parses all three with one reader.
( cd "${SRC_DIR}" && sha256sum "${OUT_BASENAME}.exe" > "${OUT_BASENAME}.exe.sha256" )

echo
echo ">>> Done: ${OUT_EXE}  ($(du -h "${OUT_EXE}" | cut -f1))"
echo ">>> Variants: ${VARIANTS[*]}"
cat "${SRC_DIR}/${OUT_BASENAME}.exe.sha256"
