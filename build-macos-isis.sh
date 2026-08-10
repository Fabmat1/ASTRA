#!/usr/bin/env bash
# Provision the ISIS / S-Lang stack for the macOS bundle into a cache prefix.
#
# Usage:   ./build-macos-isis.sh [PREFIX]
#          PREFIX defaults to ~/.cache/astra-build-macos-isis/prefix
#
# Split out of build-macos.sh on purpose: this stack is expensive (PGPLOT +
# S-Lang + ISIS + 3 modules + jed, all from source, ~15-25 min cold) and almost
# never changes, while build-macos.sh changes often. Keeping it in its own file
# lets CI cache it under a key derived from THIS script's hash, so editing the
# main build script no longer throws the ISIS build away.
#
# Env overrides: JOBS, SLANG_VERSION, PGPLOT_VERSION, ISIS_REPO, ISIS_REF
#                ASTRA_ISIS_FORCE=1  (ignore the stamp and rebuild)
#
# ── Deliberate divergences from the Linux/AppImage build ────────────────────
# The AppImage gets PGPLOT from Ubuntu's `pgplot5` package, which is a shared,
# X11-enabled build. There is no equivalent on macOS (Homebrew dropped pgplot
# over its licence), so it is built here from source with two simplifications:
#
#   1. NO X11. The /XWINDOW, /XSERVE and pgdisp drivers are left out, so the
#      bundled ISIS plots to files (/PS, /VPS, /CPS, /VCPS, /GIF, /VGIF,
#      /LATEX) and /NULL but has no interactive plot window. This avoids
#      shipping libX11 + libxcb + libXau + libXdmcp inside the .app and, more
#      importantly, avoids requiring the *user* to install XQuartz for a
#      feature that would only ever half-work in a signed bundle.
#      Consequence: ISIS's own X11 hardcodes have to be patched out — see
#      patch_isis_no_x11 below. `--without-x` alone is not sufficient.
#
#   2. Static libpgplot.a / libcpgplot.a rather than dylibs. ISIS's pgplot
#      module links `$(PGPLOT_LIB) $(FCLIBS) ...`, and autoconf's FCLIBS
#      already carries the gfortran runtime, so the Fortran objects resolve
#      without a shared PGPLOT. Static also means one fewer dylib to relocate
#      when build-macos.sh runs dylibbundler over the staged tree, and it lets
#      us skip the MacPorts makemake patch (its only load-bearing parts are
#      shared-library plumbing we no longer need).
#
# The PNG/TPNG drivers are also skipped: enabling them makes ISIS's configure
# append a bare `-lpng` to the module link line with no matching -L, which then
# fails to resolve against the Homebrew keg. GIF covers raster output.
set -euo pipefail

PREFIX="${1:-${HOME}/.cache/astra-build-macos-isis/prefix}"
WORK="$(dirname "${PREFIX}")/src"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
SLANG_VERSION="${SLANG_VERSION:-2.3.3}"
PGPLOT_VERSION="${PGPLOT_VERSION:-5.2.2}"
ISIS_REPO="${ISIS_REPO:-https://github.com/houckj/isis.git}"
ISIS_REF="${ISIS_REF:-master}"
SELF="${BASH_SOURCE[0]}"

[[ "$(uname -s)" == "Darwin" ]] || { echo "This script must run on macOS."; exit 1; }
command -v brew >/dev/null 2>&1 || { echo "Homebrew not found."; exit 1; }
BREW_PREFIX="$(brew --prefix)"

# The stamp records the hash of THIS script, so any change to the recipe below
# invalidates a cached prefix even when CI's cache key happens to survive.
STAMP="${PREFIX}/.astra-isis-stamp"
SELF_HASH="$(shasum -a 256 "${SELF}" | cut -d' ' -f1)"
if [[ "${ASTRA_ISIS_FORCE:-0}" != "1" && -f "${STAMP}" ]] \
   && [[ "$(cat "${STAMP}")" == "${SELF_HASH}" ]] \
   && [[ -x "${PREFIX}/bin/isis" ]]; then
  echo ">>> Reusing cached ISIS prefix: ${PREFIX}"
  exit 0
fi

echo ">>> Building the ISIS stack from source into ${PREFIX}"
echo ">>> (this is the slow one: PGPLOT + S-Lang + ISIS + modules)"
rm -rf "${PREFIX}" "${WORK}"
mkdir -p "${PREFIX}" "${WORK}"

# gcc supplies gfortran (PGPLOT is Fortran); gsl is needed by slgsl and by the
# stellar_isisscripts slirp module build-macos.sh compiles later; cfitsio is a
# hard requirement of ISIS's configure (see --with-cfitsio below).
brew install gcc gsl cfitsio wget gawk >/dev/null 2>&1 || true
FC_BIN="$(ls "${BREW_PREFIX}/bin/gfortran"* 2>/dev/null | sort -V | tail -1 || true)"
[[ -n "${FC_BIN}" ]] || { echo "gfortran not found (brew install gcc)."; exit 1; }

export PATH="${PREFIX}/bin:${PATH}"
# ISIS's configure and the module builds dlopen against the slang we just
# built; without this they can pick up a Homebrew s-lang if one is installed.
export DYLD_LIBRARY_PATH="${PREFIX}/lib:${DYLD_LIBRARY_PATH:-}"

# ── 1. PGPLOT (static, no X11, no libpng) ───────────────────────────────────
echo ">>> PGPLOT ${PGPLOT_VERSION}"
PG_TGZ="${WORK}/pgplot.tar.gz"
PG_SRC="${WORK}/pgplot"
PG_SHA="a5799ff719a510d84d26df4ae7409ae61fe66477e3f1e8820422a9a4727a5be4"
# Caltech's own FTP is unreliable; these are the mirrors the (removed) Homebrew
# formula used. The checksum is what pins the contents, not the host.
for url in \
  "https://distfiles.macports.org/pgplot/pgplot522.tar.gz" \
  "https://gentoo.osuosl.org/distfiles/pgplot522.tar.gz" ; do
  wget -q -O "${PG_TGZ}" "${url}" && break || true
done
[[ -s "${PG_TGZ}" ]] || { echo "Could not download PGPLOT ${PGPLOT_VERSION}."; exit 1; }
echo "${PG_SHA}  ${PG_TGZ}" | shasum -a 256 -c - >/dev/null \
  || { echo "PGPLOT tarball checksum mismatch — refusing to build it."; exit 1; }
mkdir -p "${PG_SRC}"
tar xzf "${PG_TGZ}" -C "${PG_SRC}" --strip-components=1

# PGPLOT is 1990s C: these two files call read/write/strlen/close with no
# prototype in scope. Implicit function declarations have been a hard error
# since clang 16 (Xcode 15), so without these includes the build dies. Same
# fix MacPorts carries; inlined here rather than fetched so the build doesn't
# depend on a third-party patch repo staying reachable.
perl -0pi -e 's{(#include <termios\.h>\n)}{$1#include <string.h>\n#include <fcntl.h>\n#include <unistd.h>\n}' \
  "${PG_SRC}/sys/grtermio.c"
perl -0pi -e 's{(#include <sys/types\.h>\n)}{$1#include <string.h>\n#include <unistd.h>\n}' \
  "${PG_SRC}/sys/grfileio.c"
# PGPLOT hardcodes its install dir into grgfil.f for runtime font lookup. ASTRA
# overrides it with PGPLOT_DIR at launch (IsisEnvironment), but point it at the
# staged location anyway so a stray direct invocation still finds grfont.dat.
perl -0pi -e "s{/usr/local/pgplot}{${PREFIX}/pgplot}g" "${PG_SRC}/src/grgfil.f"

# PGPLOT ships no sys_darwin (the distribution predates OS X); makemake only
# requires the directory to exist and to hold the named .conf — with no
# system-specific source overrides in it, every routine falls back to the
# generic sys/ and fonts/ copies, which is what we want.
mkdir -p "${PG_SRC}/sys_darwin"

# makemake reads this as shell. -fPIC everywhere: these static archives get
# linked into ISIS's shared pgplot module.
#
# -std=gnu17 is load-bearing. pgbind generates C wrappers declaring the Fortran
# entry points with empty parameter lists (`extern void pgvect_();`) and then
# calls them with arguments. Under C23 — the default for clang 16+ / gcc 14+ —
# `()` means "no parameters" rather than "unspecified", so every one of those
# ~150 wrappers becomes a hard error and libcpgplot.a fails to build. Pinning
# the older dialect restores the pre-C23 meaning.
cat > "${PG_SRC}/sys_darwin/astra.conf" <<CONF
# Generated by build-macos-isis.sh — no-X11, static-only PGPLOT for ASTRA.
   XINCL=""
   MOTIF_INCL=""
   ATHENA_INCL=""
   TK_INCL=""
   RV_INCL=""
   FCOMPL="${FC_BIN}"
   FFLAGC="-fPIC -O2 -ffixed-line-length-none -fallow-argument-mismatch"
   FFLAGD="-fno-backslash"
   CCOMPL="/usr/bin/clang"
   CFLAGC="-fPIC -O2 -std=gnu17 -DPG_PPU"
   CFLAGD="-O2 -std=gnu17"
   PGBIND_FLAGS="bsd"
   LIBS=""
   MOTIF_LIBS=""
   ATHENA_LIBS=""
   TK_LIBS=""
   RANLIB="ranlib"
   SHARED_LIB=""
   SHARED_LD=""
   SHARED_LIB_LIBS=""
   MCOMPL=""
   MFLAGC=""
   SYSDIR="\$SYSDIR"
CONF

PG_BUILD="${WORK}/pgplot-build"
mkdir -p "${PG_BUILD}"
cp "${PG_SRC}/drivers.list" "${PG_BUILD}/drivers.list"
# drivers.list ships every driver commented out with a leading "! ". Uncomment
# only the file-output ones. Deliberately absent: XWINDOW/XSERVE (X11) and
# PNG/TPNG (see header comment).
for drv in NULL PS VPS CPS VCPS LATEX GIF VGIF; do
  perl -i -pe "s{^! (.*/${drv}\s.*)}{  \$1}" "${PG_BUILD}/drivers.list"
done
echo ">>> PGPLOT drivers enabled:"
grep -E "^\s+\w+\s+\d+\s+/" "${PG_BUILD}/drivers.list" | sed 's/^/      /' || true

( cd "${PG_BUILD}"
  ../pgplot/makemake ../pgplot darwin astra
  # Only the pieces ISIS links against. Notably NOT `make` (builds the demo
  # programs) or `make cpg` (builds cpgdemo) — those pull in link steps we have
  # no use for. `libcpgplot.a` and `cpgplot.h` share one rule, so asking for
  # the archive produces the header too; naming both would run pgbind twice.
  make -j"${JOBS}" libpgplot.a
  make grfont.dat
  make libcpgplot.a )

mkdir -p "${PREFIX}/lib" "${PREFIX}/include" "${PREFIX}/pgplot"
cp "${PG_BUILD}/libpgplot.a" "${PG_BUILD}/libcpgplot.a" "${PREFIX}/lib/"
cp "${PG_BUILD}/cpgplot.h" "${PREFIX}/include/"
cp "${PG_BUILD}/grfont.dat" "${PREFIX}/pgplot/"
# rgb.txt is plain data shipped in the source tree, not a build product.
[[ -f "${PG_SRC}/rgb.txt" ]] && cp "${PG_SRC}/rgb.txt" "${PREFIX}/pgplot/"
echo ">>> PGPLOT installed (static, no X11)"

# ── 2. S-Lang ───────────────────────────────────────────────────────────────
# Release tarball rather than git: jedsoft's git daemon speaks the bare git://
# protocol, which is the least reliable thing in this whole script.
echo ">>> S-Lang ${SLANG_VERSION}"
wget -q -O "${WORK}/slang.tar.bz2" \
  "https://www.jedsoft.org/releases/slang/slang-${SLANG_VERSION}.tar.bz2"
mkdir -p "${WORK}/slang"
tar xjf "${WORK}/slang.tar.bz2" -C "${WORK}/slang" --strip-components=1
( cd "${WORK}/slang"
  ./configure --prefix="${PREFIX}"
  make -j"${JOBS}"
  make install )

# jedsoft ships these only via git. Prefer https (works through proxies and
# doesn't need port 9418 open) and fall back to the git protocol.
clone_jedsoft() {
  local dest="$1" repo="$2"
  git clone --depth 1 "https://git.jedsoft.org/git/${repo}" "${dest}" 2>/dev/null \
    || git clone --depth 1 "git://git.jedsoft.org/git/${repo}" "${dest}"
}

build_slang_pkg() {
  local name="$1" repo="$2"
  echo ">>> S-Lang package: ${name}"
  clone_jedsoft "${WORK}/${name}" "${repo}"
  ( cd "${WORK}/${name}"
    ./configure --prefix="${PREFIX}" --with-slang="${PREFIX}"
    make -j"${JOBS}"
    make install )
}

# ── 3. ISIS ─────────────────────────────────────────────────────────────────
echo ">>> ISIS (${ISIS_REF})"
git clone --depth 1 --branch "${ISIS_REF}" "${ISIS_REPO}" "${WORK}/isis"

# `--without-x` clears the @X_LIBS@ substitution but three Makefile.in files
# append a literal `-lX11` *after* it, so the link line still demands X11 even
# in a no-X build. Strip those. (xspec isn't in MODULE_LIST, but patch it too
# so a future --with-xspec doesn't quietly reintroduce the dependency.)
patch_isis_no_x11() {
  local f
  for f in src/Makefile.in modules/pgplot/src/Makefile.in modules/xspec/src/Makefile.in; do
    [[ -f "${f}" ]] || continue
    perl -i -pe 's{^(X_LIBS\s*=\s*\@X_LIBS\@)\s*-lX11\s*$}{$1\n}' "${f}"
  done
  ! grep -q -- "-lX11" src/Makefile.in modules/pgplot/src/Makefile.in \
    || { echo "ISIS still references -lX11 after patching — layout changed upstream."; return 1; }
}

# cfitsio is mandatory for ISIS and its configure only searches the usual system
# prefixes, which on Apple Silicon Homebrew (/opt/homebrew) it never finds — the
# build then dies with "unable to find the cfitsio library and header file
# fitsio.h". Point it at the keg explicitly (Linux gets this free from
# libcfitsio-dev in /usr).
CFITSIO_PREFIX="$(brew --prefix cfitsio)"
[[ -f "${CFITSIO_PREFIX}/include/fitsio.h" ]] \
  || { echo "cfitsio headers not found under ${CFITSIO_PREFIX} (brew install cfitsio)."; exit 1; }

( cd "${WORK}/isis"
  patch_isis_no_x11
  ./configure --prefix="${PREFIX}" --without-x \
    --with-slang="${PREFIX}" \
    --with-cfitsio="${CFITSIO_PREFIX}" \
    --with-pgplotinc="${PREFIX}/include" \
    --with-pgplotlib="${PREFIX}/lib"
  make -j"${JOBS}"
  make install )

# ── 4. S-Lang modules the isisscripts pull in ───────────────────────────────
build_slang_pkg slgsl  slgsl.git
build_slang_pkg slxfig slxfig.git
build_slang_pkg slirp  slirp.git

# ── 5. jed (build-only) ─────────────────────────────────────────────────────
# Not shipped: its `jed-script` drives tmexpand, the isisscripts doc/help
# generator that build-macos.sh runs when it builds the script libraries.
echo ">>> jed (build tooling for the isisscripts docs)"
clone_jedsoft "${WORK}/jed" "jed.git"
( cd "${WORK}/jed"
  ./configure --prefix="${PREFIX}" --with-slang="${PREFIX}"
  make -j"${JOBS}"
  make install )

# ── 6. Verify + stamp ───────────────────────────────────────────────────────
[[ -x "${PREFIX}/bin/isis" ]] || { echo "ISIS built but ${PREFIX}/bin/isis is missing."; exit 1; }
[[ -d "$(echo "${PREFIX}"/isis/*/etc | cut -d' ' -f1)" ]] \
  || { echo "ISIS_SRCDIR tree (${PREFIX}/isis/<ver>/etc) missing."; exit 1; }

# Smoke test: the pgplot module is the one most likely to have silently not
# built (it is the only piece touching Fortran + the X11 patches).
if "${PREFIX}/bin/isis" -e 'require("pgplot"); exit(0);' >/dev/null 2>&1; then
  echo ">>> pgplot module loads"
else
  echo "!!! WARNING: the isis pgplot module did not load — plotting will be unavailable."
fi

rm -rf "${WORK}"
printf '%s' "${SELF_HASH}" > "${STAMP}"
echo ">>> ISIS stack ready at ${PREFIX} ($(du -sh "${PREFIX}" | cut -f1))"
