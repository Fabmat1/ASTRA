#!/usr/bin/env bash
# Build a portable ASTRA AppImage inside a prebuilt Ubuntu 22.04 builder image.
#
# The expensive, source-independent environment (apt packages, Qt, OpenBLAS,
# ISIS/S-Lang stack, linuxdeploy tooling) lives in a Docker image built from
# docker/appimage-builder.Dockerfile. It is built once and reused; the tag
# encodes the Dockerfile hash + version args, so changing either rebuilds it
# automatically. Per-release builds then only compile ASTRA itself.
#
# Usage:  ./build-appimage.sh [VERSION]
#         ./build-appimage.sh --rebuild-image [VERSION]   # force image rebuild
#                                                         # (refreshes git HEADs
#                                                         # baked into the image)
# Env overrides: QT_VERSION, OPENBLAS_VERSION, ASTRA_BUNDLE_ISIS,
#                ASTRA_REQUIRE_ISIS=1 (fail instead of shipping without ISIS),
#                ASTRA_ISIS_SCRIPTS_CACHE / ASTRA_ISIS_SCRIPTS_DIR (snapshot and
#                  local-install fallbacks for the ISIS script libraries, see
#                  isis-scripts.sh),
#                LCURVE_REPO, LCURVE_REF, LCURVE_ARCH (-march for the bundled
#                  lcurve solvers; default x86-64-v3, same baseline as ASTRA),
#                ASTRA_BUILDER_IMAGE (use a prebuilt/published image, e.g. from
#                  GHCR, instead of building one locally — see
#                  .github/workflows/appimage-builder-image.yml),
#                ASTRA_SKIP_SUBMODULE_UPDATE=1 (caller already checked them out)
set -euo pipefail

REBUILD_IMAGE=0
if [[ "${1:-}" == "--rebuild-image" ]]; then
  REBUILD_IMAGE=1
  shift
fi

VERSION="${1:-0.1.0}"
# Exported so docker/builder-image-tag.sh (a subprocess) hashes the same values
# this build actually uses, rather than falling back to its own defaults.
export QT_VERSION="${QT_VERSION:-6.11.1}"
export OPENBLAS_VERSION="${OPENBLAS_VERSION:-0.3.27}"
export ASTRA_BUNDLE_ISIS="${ASTRA_BUNDLE_ISIS:-1}"
# A failed ISIS bundle only warns and ships an AppImage that falls back to a
# user-installed isis on PATH; set this to 1 to make it fatal instead.
export ASTRA_REQUIRE_ISIS="${ASTRA_REQUIRE_ISIS:-0}"
SRC_DIR="$(pwd)"
DOCKERFILE="${SRC_DIR}/docker/appimage-builder.Dockerfile"

[[ -f "${SRC_DIR}/CMakeLists.txt" ]] || { echo "Run from ASTRA repo root."; exit 1; }
[[ -f "${DOCKERFILE}" ]] || { echo "Missing ${DOCKERFILE}"; exit 1; }

# Ensure submodules are present. CI checks them out itself (actions/checkout
# handles the SSH-URL submodule); re-running it there is redundant.
if [[ "${ASTRA_SKIP_SUBMODULE_UPDATE:-0}" != "1" ]]; then
  git submodule update --init --recursive
fi

# ---------- Builder image (built once, reused across releases) ----------
# Tag = hash over the Dockerfile + everything passed as build args, so any
# change to the environment definition yields a new image instead of a stale one.
# docker/builder-image-tag.sh owns that hash so CI derives the same tag.
IMAGE_HASH="$("${SRC_DIR}/docker/builder-image-tag.sh")"
IMAGE="${ASTRA_BUILDER_IMAGE:-astra-appimage-builder:${IMAGE_HASH}}"

if [[ -n "${ASTRA_BUILDER_IMAGE:-}" ]]; then
  # A published image was named (CI): pull it rather than spend an hour
  # rebuilding Qt + OpenBLAS + ISIS on a runner with no image cache.
  echo ">>> Using prebuilt builder image ${IMAGE}"
  docker image inspect "${IMAGE}" >/dev/null 2>&1 || docker pull "${IMAGE}"
elif [[ "${REBUILD_IMAGE}" == "1" ]] || ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
  echo ">>> Building builder image ${IMAGE} (one-time, slow: Qt + OpenBLAS + ISIS)"
  docker build \
    $( [[ "${REBUILD_IMAGE}" == "1" ]] && echo --no-cache ) \
    --build-arg QT_VERSION="${QT_VERSION}" \
    --build-arg OPENBLAS_VERSION="${OPENBLAS_VERSION}" \
    --build-arg BUNDLE_ISIS="${ASTRA_BUNDLE_ISIS}" \
    -t "${IMAGE}" \
    -f "${DOCKERFILE}" \
    "${SRC_DIR}/docker"
else
  echo ">>> Reusing builder image ${IMAGE}"
fi

# ccache cache shared across builds (compiler cache for ASTRA + lcurve)
CCACHE_DIR_HOST="${HOME}/.cache/astra-build/ccache"
mkdir -p "${CCACHE_DIR_HOST}"

# Snapshot of the last isisscripts/stellar_isisscripts clone that worked, plus
# an optional local ISIS install to fall back on when their server is down (see
# isis-scripts.sh). Both are mounted rather than fetched inside the
# container, so a build with no network for that host still produces a complete
# AppImage. On a runner the snapshot dir is filled by actions/cache.
ISIS_SCRIPTS_CACHE_HOST="${ASTRA_ISIS_SCRIPTS_CACHE:-${HOME}/.cache/astra-isis-scripts}"
mkdir -p "${ISIS_SCRIPTS_CACHE_HOST}"
ISIS_SCRIPTS_LOCAL_MOUNT=()
ISIS_SCRIPTS_LOCAL_HOST="${ASTRA_ISIS_SCRIPTS_DIR:-}"
if [[ -z "${ISIS_SCRIPTS_LOCAL_HOST}" ]]; then
  for d in "${HOME}/Projects/ISIS_install/src" "${HOME}/ISIS_install/src" \
           "${HOME}/isis_install/src"; do
    [[ -d "${d}" ]] && { ISIS_SCRIPTS_LOCAL_HOST="${d}"; break; }
  done
fi
if [[ -n "${ISIS_SCRIPTS_LOCAL_HOST}" && -d "${ISIS_SCRIPTS_LOCAL_HOST}" ]]; then
  echo ">>> Local ISIS scripts available as a fallback: ${ISIS_SCRIPTS_LOCAL_HOST}"
  ISIS_SCRIPTS_LOCAL_MOUNT=(-v "${ISIS_SCRIPTS_LOCAL_HOST}:/isis-scripts-local:ro"
                            -e ASTRA_ISIS_SCRIPTS_DIR=/isis-scripts-local)
fi

docker run --rm -i \
  -v "${SRC_DIR}:/src" \
  -v "${CCACHE_DIR_HOST}:/root/.ccache" \
  -v "${ISIS_SCRIPTS_CACHE_HOST}:/isis-scripts-cache" \
  "${ISIS_SCRIPTS_LOCAL_MOUNT[@]}" \
  -w /src \
  -e VERSION="${VERSION}" \
  -e ASTRA_BUNDLE_ISIS="${ASTRA_BUNDLE_ISIS}" \
  -e ASTRA_REQUIRE_ISIS="${ASTRA_REQUIRE_ISIS}" \
  -e ASTRA_ISIS_SCRIPTS_CACHE=/isis-scripts-cache \
  -e ASTRA_ISIS_SCRIPTS_OFFLINE="${ASTRA_ISIS_SCRIPTS_OFFLINE:-0}" \
  -e LCURVE_REPO="${LCURVE_REPO:-}" \
  -e LCURVE_REF="${LCURVE_REF:-}" \
  -e LCURVE_ARCH="${LCURVE_ARCH:-}" \
  "${IMAGE}" bash <<'DOCKER_EOF'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
# QTROOT, Qt6_DIR, PATH, LD_LIBRARY_PATH come from the image's ENV.

# ---------- 1. Idempotent source patches (no-op if already committed) ----------
cd /src
grep -q '#include <unordered_set>' src/lightcurve/Photometry.cpp || \
  sed -i '/^#include <QJsonObject>/a #include <unordered_set>' src/lightcurve/Photometry.cpp
grep -q '#include <charconv>' src/importwizard/SpectralFitImportPage.cpp || \
  sed -i '0,/^#include /{s|^#include |#include <charconv>\n#include |}' src/importwizard/SpectralFitImportPage.cpp

# ---------- 2. Configure & build ASTRA ----------
rm -rf build AppDir
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="${QTROOT};/usr/local" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DGAEL_ENABLE_CUDA=OFF
cmake --build build -j"$(nproc)"

# ---------- 3. Install into AppDir ----------
cmake --install build --prefix AppDir/usr
if [[ ! -x AppDir/usr/bin/astra ]] && [[ -x build/ASTRA ]]; then
  mkdir -p AppDir/usr/bin
  cp build/ASTRA AppDir/usr/bin/astra
fi

# ---------- 3a. sedfit (SEDplusplus SED-fitting helper) ----------
# Built by the main cmake run via add_subdirectory(external/SEDplusplus).
# ASTRA resolves it next to its own executable first (SedFitEnvironment), so
# hand the binary to linuxdeploy via -e: it lands in usr/bin and its shared
# libs (cfitsio, gsl, curl, ...) get bundled + RPATH-patched. The refdata is
# already installed by cmake to usr/share/astra/sedfit/refdata, which the
# ../share relative lookup from usr/bin resolves at runtime. Drop the
# duplicate libexec copy cmake installed — its libs would go undeployed.
SEDFIT_BIN="build/external/SEDplusplus/sedfit"
[[ -x "${SEDFIT_BIN}" ]] || { echo "sedfit binary missing at ${SEDFIT_BIN}"; exit 1; }
rm -rf AppDir/usr/libexec/astra/sedfit
[[ -d AppDir/usr/share/astra/sedfit/refdata ]] \
  || { echo "sedfit refdata missing in AppDir"; exit 1; }

# ---------- 3b. Build lcurve fitting binaries (bundled into the AppImage) ----
# ASTRA shells out to lcurve_levmarq / lcurve_mcmc / lcurve_simplex for light-
# curve fitting, and to lcurve_re (forward model) for the model preview in the
# fit dialog. Built here (not baked into the image) so each release picks up
# the current lcurve_re HEAD; the compile is small and ccache-accelerated.
LCURVE_REPO="${LCURVE_REPO:-https://github.com/Fabmat1/lcurve_re.git}"
LCURVE_REF="${LCURVE_REF:-main}"
rm -rf /tmp/lcurve_re
git clone --depth 1 --branch "${LCURVE_REF}" "${LCURVE_REPO}" /tmp/lcurve_re
cd /tmp/lcurve_re
# Compatibility shims for Ubuntu 22.04's cmake 3.22.1: lcurve_re targets a newer
# cmake (min 3.22.3) and SETs policies CMP0146/CMP0167 that 3.22.1 doesn't know.
# CUDA is disabled and Boost resolves fine here, so dropping them is a no-op.
sed -i 's/VERSION 3\.22\.3/VERSION 3.22.1/' CMakeLists.txt
sed -i '/cmake_policy(SET CMP0146 NEW)/d; /cmake_policy(SET CMP0167 NEW)/d' CMakeLists.txt
# lcurve_re's Release flags hardcode -march=native, which tunes the binaries to
# whatever CPU the release runner happened to use (GitHub's Ice Lake Xeons emit
# AVX-512). On a user machine without those instructions the solver dies with
# SIGILL the moment it enters a vectorized kernel. Pin the same baseline ASTRA
# itself is compiled against (CMakeLists.txt: -march=x86-64-v3, AVX2/FMA).
LCURVE_ARCH="${LCURVE_ARCH:-x86-64-v3}"
sed -i "s/-march=native/-march=${LCURVE_ARCH}/" CMakeLists.txt
grep -q -- "-march=native" CMakeLists.txt \
  && { echo "lcurve_re still requests -march=native; refusing to ship a host-tuned build"; exit 1; }
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLCURVE_ENABLE_CUDA=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build --target lcurve_levmarq lcurve_mcmc lcurve_simplex \
  lcurve_re -j"$(nproc)"
LCURVE_BUILD=/tmp/lcurve_re/build
for b in lcurve_levmarq lcurve_mcmc lcurve_simplex lcurve_re; do
  [[ -x "${LCURVE_BUILD}/${b}" ]] || { echo "lcurve build missing ${b}"; exit 1; }
done
cd /src

# ---------- 3c. Stage ISIS (core prebuilt in the image at /opt/isis) ----------
# The S-Lang/ISIS core (slang, isis, modules, jed, slirp) is compiled at
# image-build time. The script libraries (isisscripts, stellar_isisscripts)
# change often, so they are fetched at the freshest HEAD available and built
# here on every run (their `make` is cheap); isis-scripts.sh handles the
# fetch and its fallbacks. ISIS only *reads* its tree at runtime, so it is
# shipped as data under usr/share/astra/isis and runs in place; ASTRA points it
# there via ISIS_SRCDIR / SLSH_PATH / SLANG_MODULE_PATH and a private .isisrc
# (see src/fitting/IsisEnvironment.cpp). Set ASTRA_BUNDLE_ISIS=0 to skip.
ISIS_BIN=""
ISIS_EXTRA_LIBS=()
ISIS_STAGE=/src/AppDir/usr/share/astra/isis
if [[ "${ASTRA_BUNDLE_ISIS:-1}" == "1" ]]; then
  ISIS_PREFIX=/opt/isis
  [[ -d "${ISIS_PREFIX}/isis" ]] \
    || { echo "Image lacks prebuilt ISIS (${ISIS_PREFIX}); rebuild with BUNDLE_ISIS=1"; exit 1; }

  # PGPLOT runtime data + libs (pgplot5 package, baked into the image)
  PGPLOT_LIB="$(dirname "$(dpkg -L pgplot5 | grep -m1 '/libcpgplot\.so')")"
  PGPLOT_DATA="$(dirname "$(dpkg -L pgplot5 | grep -m1 '/grfont\.dat$')")"

  # Everything below is non-fatal, exactly as in build-macos.sh step 11c: the
  # script libraries come off one university server, and when it answered every
  # clone with HTTP 403 on 2026-09-10 a bare `git clone` under errexit took the
  # whole 0.8.0 AppImage down with it. An AppImage without ISIS still runs (it
  # falls back to a user-installed isis on PATH), so warn loudly and ship.
  #
  # The subshell must be a *standalone* command with errexit disabled around it:
  # bash propagates "errexit is ignored here" into a subshell used as an `if`
  # condition or as an operand of &&/||, which would silently run on past every
  # failure inside it.
  set +e
  (
    set -e
    # --- Script libraries: freshest HEAD obtainable, built fresh each run ---
    # isis-scripts.sh tries upstream first and falls back to a cached
    # snapshot or a local ISIS install; /src is the repo, mounted by the caller.
    ISIS_SCRIPTS_SRC=/tmp/isis_scripts
    rm -rf "${ISIS_SCRIPTS_SRC}"
    mkdir -p "${ISIS_SCRIPTS_SRC}"
    source /src/isis-scripts.sh
    isis_scripts_fetch "${ISIS_SCRIPTS_SRC}"
    cd "${ISIS_SCRIPTS_SRC}"

    # isisscripts (Remeis) - `make` builds share/ in the clone (not installed).
    ( cd isisscripts && make )

    # stellar_isisscripts (Irrgang) + its slirp C-function module
    ( cd stellar_isisscripts && make )
    if [[ -f stellar_isisscripts/slirp/c_functions.h ]]; then
      ( cd stellar_isisscripts/slirp
        "${ISIS_PREFIX}/bin/slirp" -make -lm -lgsl -lgslcblas -lpthread c_functions.h c_functions.o
        [[ -f Makefile ]] && make ) || echo "WARN: stellar c_functions build failed (continuing)"
    fi
    cd /src

    rm -rf "${ISIS_STAGE}"
    mkdir -p "${ISIS_STAGE}/srcdir"
    cp -a "${ISIS_PREFIX}"/isis/*/.  "${ISIS_STAGE}/srcdir/"   # ISIS_SRCDIR (etc, share, lib/modules)
    cp -a "${ISIS_PREFIX}/lib"       "${ISIS_STAGE}/"          # libslang + slang/v2/modules
    cp -a "${ISIS_PREFIX}/share"     "${ISIS_STAGE}/"          # share/slsh (+ local-packages)
    mkdir -p "${ISIS_STAGE}/share/slsh/local-packages"
    # Script libraries live in the clones, not the prefix - copy them whole
    # (minus VCS) so require("isisscripts") / require("stellar_isisscripts")
    # resolve.
    for s in isisscripts stellar_isisscripts; do
      cp -a "${ISIS_SCRIPTS_SRC}/${s}" "${ISIS_STAGE}/${s}"
      rm -rf "${ISIS_STAGE}/${s}/.git"
    done
    # PGPLOT runtime data (grfont.dat + rgb.txt); ISIS finds it via PGPLOT_DIR,
    # which ASTRA exports from IsisEnvironment when launching the bundled isis.
    mkdir -p "${ISIS_STAGE}/pgplot"
    cp -a "${PGPLOT_DATA}/grfont.dat" "${ISIS_STAGE}/pgplot/"
    [[ -f "${PGPLOT_DATA}/rgb.txt" ]] && cp -a "${PGPLOT_DATA}/rgb.txt" "${ISIS_STAGE}/pgplot/"
    find "${ISIS_STAGE}" -name '*.o' -delete 2>/dev/null || true
    # Both libraries must actually have built, or the stage is decoration.
    [[ -f "${ISIS_STAGE}/isisscripts/share/isisscripts.sl" ]] \
      || { echo "isisscripts did not build share/isisscripts.sl"; exit 1; }
    [[ -f "${ISIS_STAGE}/stellar_isisscripts/share/stellar_isisscripts.sl" ]] \
      || { echo "stellar_isisscripts did not build share/stellar_isisscripts.sl"; exit 1; }
  )
  ISIS_STATUS=$?
  set -e
  cd /src

  if (( ISIS_STATUS == 0 )); then
    # Binary (for linuxdeploy -e) + module-only libs (gsl) it can't discover.
    ISIS_BIN="${ISIS_PREFIX}/bin/isis"             # symlink -> isis/<ver>/bin/isis
    [[ -x "${ISIS_BIN}" ]] || { echo "image's ISIS has no isis binary"; exit 1; }
    for lib in libgsl libgslcblas; do
      for p in /usr/lib/x86_64-linux-gnu/${lib}.so.*; do
        [[ -f "${p}" ]] && ISIS_EXTRA_LIBS+=("-l" "${p}")
      done
    done
    # PGPLOT libs are dlopened via the isis pgplot module, so linuxdeploy can't
    # discover them from the isis binary - hand them over explicitly (their own
    # deps, e.g. libpng, get pulled in transitively).
    for p in "${PGPLOT_LIB}"/libpgplot.so* "${PGPLOT_LIB}"/libcpgplot.so*; do
      [[ -f "${p}" ]] && ISIS_EXTRA_LIBS+=("-l" "${p}")
    done
    echo ">>> ISIS staged at ${ISIS_STAGE} ($(du -sh ${ISIS_STAGE} | cut -f1))"
  else
    echo "!!! ISIS build/bundle failed (exit ${ISIS_STATUS}) - the AppImage has NO bundled ISIS."
    echo "!!! ISIS-backed fitting would fall back to a user-installed isis on PATH."
    rm -rf "${ISIS_STAGE}"
    # The AppImage still ships, so the only thing standing between an ISIS-less
    # release and nobody noticing is this annotation.
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
      echo "::warning title=AppImage built without ISIS::The ISIS stack failed to build (exit ${ISIS_STATUS}); this .AppImage ships WITHOUT bundled ISIS and falls back to a user-installed isis on PATH."
      if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        echo "⚠️ **This \`.AppImage\` was packaged without bundled ISIS** (ISIS build exited ${ISIS_STATUS})." \
          >> "${GITHUB_STEP_SUMMARY}"
      fi
    fi
    # Opt-in hard failure: refuse to package at all rather than ship degraded.
    if [[ "${ASTRA_REQUIRE_ISIS:-0}" == "1" ]]; then
      echo "!!! ASTRA_REQUIRE_ISIS=1 - refusing to package an AppImage without ISIS."
      exit 1
    fi
  fi
else
  echo ">>> ASTRA_BUNDLE_ISIS=0 - skipping ISIS bundling"
fi

cd /src

# ---------- 4. Desktop file + icon ----------
mkdir -p AppDir/usr/share/applications AppDir/usr/share/icons/hicolor/256x256/apps
cat > AppDir/usr/share/applications/astra.desktop <<'DESK'
[Desktop Entry]
Name=ASTRA
GenericName=Stellar Astrophysics Toolkit
Exec=ASTRA
Icon=astra
Type=Application
Categories=Science;Astronomy;Education;
Comment=Stellar astrophysics analysis program
Terminal=false
DESK

ICON_TARGET=AppDir/usr/share/icons/hicolor/256x256/apps/astra.png
SVG_INSTALLED=AppDir/usr/share/icons/hicolor/scalable/apps/astra.svg

if   [[ -f resources/icons/astra.png ]]; then
  cp resources/icons/astra.png "$ICON_TARGET"
elif [[ -f resources/astra.png ]]; then
  cp resources/astra.png "$ICON_TARGET"
elif [[ -f "$SVG_INSTALLED" ]]; then
  rsvg-convert -w 256 -h 256 "$SVG_INSTALLED" -o "$ICON_TARGET"
else
  # Last-resort placeholder — no fonts needed
  convert -size 256x256 gradient:'#0a1a3a-#1a4a8a' \
    -fill white -draw 'circle 128,128 128,30' "$ICON_TARGET"
fi

# ---------- 5. Run linuxdeploy, bundling OpenSSL ----------
# linuxdeploy / plugins / appimagetool are preinstalled in the image.
export APPIMAGE_EXTRACT_AND_RUN=1
export VERSION
export OUTPUT="astra-${VERSION}-x86_64.AppImage"

EXTRA_LIBS=()
for lib in libssl.so.3 libcrypto.so.3; do
  path="/usr/lib/x86_64-linux-gnu/${lib}"
  [[ -f "${path}" ]] && EXTRA_LIBS+=("-l" "${path}")
done

find "${QTROOT}/plugins/sqldrivers" -type f -name 'libqsql*.so' \
  ! -name 'libqsqlite.so' -delete

# ISIS: bundle the isis binary (linuxdeploy pulls its libs + patches RPATH) and
# the gsl libs its dlopened S-Lang modules need but linuxdeploy can't discover.
ISIS_DEPLOY_ARGS=()
[[ -n "${ISIS_BIN}" ]] && ISIS_DEPLOY_ARGS+=("-e" "${ISIS_BIN}")
ISIS_DEPLOY_ARGS+=("${ISIS_EXTRA_LIBS[@]}")

linuxdeploy \
  --appdir AppDir \
  -e AppDir/usr/bin/astra \
  -e "${LCURVE_BUILD}/lcurve_levmarq" \
  -e "${LCURVE_BUILD}/lcurve_mcmc" \
  -e "${LCURVE_BUILD}/lcurve_simplex" \
  -e "${LCURVE_BUILD}/lcurve_re" \
  -e "${SEDFIT_BIN}" \
  "${ISIS_DEPLOY_ARGS[@]}" \
  -d AppDir/usr/share/applications/astra.desktop \
  -i AppDir/usr/share/icons/hicolor/256x256/apps/astra.png \
  "${EXTRA_LIBS[@]}" \
  --plugin qt \
  --output appimage

sha256sum "${OUTPUT}" > "${OUTPUT}.sha256"
ls -lh "${OUTPUT}" "${OUTPUT}.sha256"
DOCKER_EOF

echo
echo "=== Build complete ==="
ls -lh astra-${VERSION}-x86_64.AppImage*
echo
echo "Next steps:"
echo "  ./astra-${VERSION}-x86_64.AppImage --appimage-extract-and-run"
echo "  gh release upload v${VERSION} astra-${VERSION}-x86_64.AppImage astra-${VERSION}-x86_64.AppImage.sha256"
