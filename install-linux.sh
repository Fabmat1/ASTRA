#!/usr/bin/env bash
#
# ASTRA — Linux installer for Ubuntu/Debian and Arch Linux.
#
# Takes a machine from "fresh checkout" to "ASTRA in the application menu":
#   1. detect the distribution and the available Qt
#   2. install every system package ASTRA and its submodules need
#   3. fill in the dependencies the distro packages don't provide
#      (unordered_dense header, FFTW3/OpenBLAS CMake configs, ...)
#   4. install Qt >= 6.10 via aqtinstall when the distro is too old
#   5. fetch the git submodules (DIGGA, rv_mcmc, lightcurvequery, SEDplusplus)
#   6. build the lcurve light-curve fitting binaries (optional, bundled)
#   7. configure, build and install ASTRA
#
# Usage:  ./install-linux.sh [options]          (see --help)
#
set -euo pipefail

# ───────────────────────────── configuration ────────────────────────────────
REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${REPO_DIR}/build"
LOG_FILE="${BUILD_DIR}/install-linux.log"

PREFIX="/usr/local"
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
QT_MIN="6.10"                       # what CMakeLists.txt demands
QT_DOWNLOAD_VERSION="${QT_VERSION:-6.11.1}"
QT_DOWNLOAD_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/astra/Qt"
LCURVE_REPO="${LCURVE_REPO:-https://github.com/Fabmat1/lcurve_re.git}"
LCURVE_REF="${LCURVE_REF:-main}"
UNORDERED_DENSE_URL="https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h"
CXXOPTS_URL="https://raw.githubusercontent.com/jarro2783/cxxopts/v3.2.0/include/cxxopts.hpp"
GNUPLOT_IOSTREAM_URL="https://raw.githubusercontent.com/dstahlke/gnuplot-iostream/master/gnuplot-iostream.h"

VERBOSE=0
ASSUME_YES=0
SKIP_DEPS=0
DO_INSTALL=1
WITH_LCURVE=1
CUDA_MODE="auto"                    # auto | on | off
FORCE_QT_DOWNLOAD=0

# ───────────────────────────── pretty output ────────────────────────────────
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m';  C_DIM=$'\033[2m'
    C_RED=$'\033[31m';  C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'; C_CYAN=$'\033[36m'
    IS_TTY=1
else
    C_RESET=''; C_BOLD=''; C_DIM=''; C_RED=''; C_GREEN=''; C_YELLOW=''
    C_BLUE=''; C_CYAN=''
    IS_TTY=0
fi

STEP_NO=0
STEP_TOTAL=9
SPIN_FRAMES=('⠋' '⠙' '⠹' '⠸' '⠼' '⠴' '⠦' '⠧' '⠇' '⠏')

log()      { printf '%s\n' "$*" >>"${LOG_FILE}" 2>/dev/null || true; }
say()      { printf '%s\n' "$*"; log "$*"; }
info()     { say "    ${C_DIM}$*${C_RESET}"; }
ok()       { say "    ${C_GREEN}✔${C_RESET} $*"; }
warn()     { say "    ${C_YELLOW}!${C_RESET} $*"; WARNINGS+=("$*"); }
note()     { say "    ${C_BLUE}i${C_RESET} $*"; }

banner() {
    printf '\n'
    say "${C_BOLD}${C_CYAN}    ___    _______________  ___${C_RESET}"
    say "${C_BOLD}${C_CYAN}   /   |  / ___/_  __/ __ \\/   |${C_RESET}   ${C_DIM}Stellar Astrophysics Data Manager${C_RESET}"
    say "${C_BOLD}${C_CYAN}  / /| |  \\__ \\ / / / /_/ / /| |${C_RESET}   ${C_DIM}Linux installer${C_RESET}"
    say "${C_BOLD}${C_CYAN} / ___ | ___/ // / / _, _/ ___ |${C_RESET}"
    say "${C_BOLD}${C_CYAN}/_/  |_|/____//_/ /_/ |_/_/  |_|${C_RESET}"
    printf '\n'
}

step() {
    STEP_NO=$((STEP_NO + 1))
    printf '\n'
    say "${C_BOLD}${C_BLUE}[${STEP_NO}/${STEP_TOTAL}]${C_RESET} ${C_BOLD}$*${C_RESET}"
}

die() {
    printf '\n'
    say "${C_RED}${C_BOLD}✘ $*${C_RESET}"
    if [[ -s "${LOG_FILE}" ]]; then
        printf '\n'
        say "${C_DIM}── last 25 log lines (${LOG_FILE}) ─────────────────────${C_RESET}"
        tail -n 25 "${LOG_FILE}" | sed "s/^/${C_DIM}│${C_RESET} /"
        say "${C_DIM}────────────────────────────────────────────────────────${C_RESET}"
    fi
    exit 1
}

# run_step "<description>" <command...>
# Streams to the log file; shows a spinner (and build progress, when the
# command emits "[ 42%]" / "[12/345]" markers) on an interactive terminal.
run_step() {
    local desc="$1"; shift
    log ""; log "### ${desc}"; log "### \$ $*"

    if [[ ${VERBOSE} -eq 1 ]]; then
        say "    ${C_DIM}▸ ${desc}${C_RESET}"
        if "$@" 2>&1 | tee -a "${LOG_FILE}"; then ok "${desc}"; return 0; fi
        say "    ${C_RED}✘${C_RESET} ${desc}"
        return 1
    fi
    if [[ ${IS_TTY} -eq 0 ]]; then
        say "    ${C_DIM}▸ ${desc}${C_RESET}"
        if "$@" >>"${LOG_FILE}" 2>&1; then ok "${desc}"; return 0; fi
        say "    ${C_RED}✘${C_RESET} ${desc}"
        return 1
    fi

    printf '\033[?25l'                       # hide the cursor while spinning
    "$@" >>"${LOG_FILE}" 2>&1 &
    local pid=$! i=0 start=${SECONDS} progress='' elapsed
    while kill -0 "${pid}" 2>/dev/null; do
        elapsed=$((SECONDS - start))
        if (( i % 10 == 0 )); then
            progress="$(tail -n 60 "${LOG_FILE}" 2>/dev/null \
                        | grep -oE '\[ *[0-9]+%\]|\[[0-9]+/[0-9]+\]' | tail -n 1 || true)"
        fi
        printf '\r    %s %s %s' \
            "${C_CYAN}${SPIN_FRAMES[$((i % 10))]}${C_RESET}" \
            "${desc}" \
            "${C_DIM}${progress} ${elapsed}s${C_RESET}   "
        i=$((i + 1))
        sleep 0.1
    done
    local rc=0
    wait "${pid}" || rc=$?
    elapsed=$((SECONDS - start))
    printf '\r\033[K\033[?25h'
    if [[ ${rc} -eq 0 ]]; then
        ok "${desc} ${C_DIM}(${elapsed}s)${C_RESET}"
    else
        say "    ${C_RED}✘${C_RESET} ${desc}"
    fi
    return ${rc}
}

confirm() {
    [[ ${ASSUME_YES} -eq 1 ]] && return 0
    local reply
    printf '\n    %s [Y/n] ' "${C_BOLD}$1${C_RESET}"
    read -r reply || reply=""
    [[ -z "${reply}" || "${reply}" =~ ^[Yy] ]]
}

# ───────────────────────────── small helpers ────────────────────────────────
have() { command -v "$1" >/dev/null 2>&1; }

# version_ge A B  →  true when A is a valid version and A >= B
version_ge() {
    [[ "$1" =~ ^[0-9]+(\.[0-9]+)*$ ]] || return 1
    [[ "$1" == "$2" ]] && return 0
    [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" == "$2" ]]
}

fetch_to() {   # fetch_to <url> <destination file>  (uses sudo when needed)
    local url="$1" dest="$2" tmp
    tmp="$(mktemp)"
    if have curl; then
        curl -fsSL "${url}" -o "${tmp}" || die "Download failed: ${url}"
    elif have wget; then
        wget -q -O "${tmp}" "${url}" || die "Download failed: ${url}"
    else
        die "Neither curl nor wget is available to download ${url}"
    fi
    ${SUDO} install -Dm644 "${tmp}" "${dest}"
    rm -f "${tmp}"
}

find_first() { local f; for f in "$@"; do [[ -e "${f}" ]] && { printf '%s\n' "${f}"; return 0; }; done; return 1; }

usage() {
    cat <<EOF
${C_BOLD}ASTRA Linux installer${C_RESET}

  ./install-linux.sh [options]

${C_BOLD}Options${C_RESET}
  -p, --prefix DIR      install prefix (default: ${PREFIX})
  -j, --jobs N          parallel compile jobs (default: ${JOBS})
  -t, --build-type T    Debug|Release|RelWithDebInfo|MinSizeRel (default: ${BUILD_TYPE})
      --qt-version V    Qt version to download when the distro is too old
                        (default: ${QT_DOWNLOAD_VERSION}, needs >= ${QT_MIN})
      --download-qt     always download Qt, even if the system Qt is new enough
      --cuda / --no-cuda  force the DIGGA CUDA back-end on/off (default: auto)
      --no-lcurve       skip building the bundled lcurve fitting binaries
      --skip-deps       don't touch system packages (assume they are present)
      --build-only      configure and build, but do not install
  -y, --yes             don't ask for confirmation
  -v, --verbose         stream all build output instead of a progress line
  -h, --help            show this help

${C_BOLD}Examples${C_RESET}
  ./install-linux.sh                        # system-wide install into /usr/local
  ./install-linux.sh -p "\$HOME/.local"      # per-user install, no root needed
  ./install-linux.sh --build-only -v        # just build, full output
EOF
}

# ───────────────────────────── argument parsing ─────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--prefix)     PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        -j|--jobs)       JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        -t|--build-type) BUILD_TYPE="${2:?--build-type needs a value}"; shift 2 ;;
        --qt-version)    QT_DOWNLOAD_VERSION="${2:?--qt-version needs a value}"; shift 2 ;;
        --download-qt)   FORCE_QT_DOWNLOAD=1; shift ;;
        --cuda)          CUDA_MODE="on"; shift ;;
        --no-cuda)       CUDA_MODE="off"; shift ;;
        --no-lcurve)     WITH_LCURVE=0; shift ;;
        --skip-deps)     SKIP_DEPS=1; shift ;;
        --build-only)    DO_INSTALL=0; shift ;;
        -y|--yes)        ASSUME_YES=1; shift ;;
        -v|--verbose)    VERBOSE=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               printf '%s\n' "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

case "${BUILD_TYPE}" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) printf 'Invalid --build-type "%s"\n' "${BUILD_TYPE}" >&2; exit 2 ;;
esac

WARNINGS=()
mkdir -p "${BUILD_DIR}"
: >"${LOG_FILE}"

# Package-installation helpers run inside a background subshell (for the
# spinner), so they report failures through a file rather than a variable.
MISSING_PKG_FILE="${BUILD_DIR}/.install-missing-packages"
: >"${MISSING_PKG_FILE}"

SUDO_KEEPALIVE_PID=""
cleanup() {
    [[ -n "${SUDO_KEEPALIVE_PID}" ]] && kill "${SUDO_KEEPALIVE_PID}" 2>/dev/null || true
    [[ ${IS_TTY} -eq 1 ]] && printf '\033[?25h'   # restore cursor
    return 0
}
trap cleanup EXIT

banner
say "    ${C_DIM}log: ${LOG_FILE}${C_RESET}"

# ════════════════════════ 1. detect the system ══════════════════════════════
step "Detecting your system"

[[ -f "${REPO_DIR}/CMakeLists.txt" ]] || die "install-linux.sh must live in the ASTRA repository root."
[[ "$(uname -s)" == "Linux" ]] || die "This installer is for Linux. On macOS use the build-macos.sh script."

ARCH="$(uname -m)"
[[ "${ARCH}" == "x86_64" ]] || warn "ASTRA is compiled with -march=x86-64-v3; on ${ARCH} the build will likely fail."

DISTRO_ID="unknown"; DISTRO_NAME="unknown"; DISTRO_LIKE=""
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_ID="${ID:-unknown}"
    DISTRO_NAME="${PRETTY_NAME:-${NAME:-unknown}}"
    DISTRO_LIKE="${ID_LIKE:-}"
fi

PKG_MGR=""
case "${DISTRO_ID}" in
    ubuntu|debian|linuxmint|pop|elementary|zorin|neon|raspbian|kali) PKG_MGR="apt" ;;
    arch|archarm|manjaro|endeavouros|garuda|cachyos|artix|arcolinux) PKG_MGR="pacman" ;;
    *)
        case " ${DISTRO_LIKE} " in
            *" debian "*|*" ubuntu "*) PKG_MGR="apt" ;;
            *" arch "*)                PKG_MGR="pacman" ;;
        esac
        ;;
esac

if [[ -z "${PKG_MGR}" ]]; then
    if   have apt-get; then PKG_MGR="apt"
    elif have pacman;  then PKG_MGR="pacman"
    fi
    [[ -n "${PKG_MGR}" ]] && warn "Unrecognised distribution '${DISTRO_ID}' — falling back to ${PKG_MGR}."
fi
[[ -n "${PKG_MGR}" ]] || die "Unsupported distribution '${DISTRO_ID}'. This script handles Ubuntu/Debian and Arch. Use --skip-deps to install the dependencies yourself."

ok "${DISTRO_NAME} (${ARCH}) — package manager: ${PKG_MGR}"

# root / sudo
SUDO=""
if [[ ${EUID} -ne 0 ]]; then
    have sudo || die "Not running as root and 'sudo' is not installed."
    SUDO="sudo"
fi

# Does the install prefix need root?
NEED_ROOT_INSTALL=1
if [[ -w "${PREFIX}" ]] || { [[ ! -e "${PREFIX}" ]] && [[ -w "$(dirname "${PREFIX}")" ]]; }; then
    NEED_ROOT_INSTALL=0
fi

# ════════════════════════ 2. work out the Qt situation ══════════════════════
step "Checking for Qt >= ${QT_MIN}"

system_qt_version() {
    local f v
    for f in /usr/lib/cmake/Qt6/Qt6ConfigVersion.cmake \
             /usr/lib64/cmake/Qt6/Qt6ConfigVersion.cmake \
             /usr/lib/*/cmake/Qt6/Qt6ConfigVersion.cmake; do
        [[ -f "${f}" ]] || continue
        v="$(sed -n 's/^ *set *( *PACKAGE_VERSION *"\([0-9][0-9.]*\)".*/\1/p' "${f}" | head -n 1)"
        if [[ -n "${v}" ]]; then printf '%s\n' "${v}"; return 0; fi
    done
    for f in qmake6 qmake-qt6; do
        if have "${f}"; then
            v="$("${f}" -query QT_VERSION 2>/dev/null || true)"
            if [[ -n "${v}" ]]; then printf '%s\n' "${v}"; return 0; fi
        fi
    done
    return 1
}

repo_qt_version() {
    case "${PKG_MGR}" in
        apt)    apt-cache policy qt6-base-dev 2>/dev/null \
                    | awk '/Candidate:/{print $2}' | sed 's/[+-].*//;s/^[0-9]*://' ;;
        pacman) pacman -Si qt6-base 2>/dev/null | awk '/^Version/{print $3}' | sed 's/-[0-9]*$//' ;;
    esac
}

QT_SOURCE="system"        # system | distro-package | download
QT_PREFIX=""              # non-empty only for a downloaded Qt

INSTALLED_QT="$(system_qt_version 2>/dev/null || true)"
if [[ ${FORCE_QT_DOWNLOAD} -eq 1 ]]; then
    QT_SOURCE="download"
    info "--download-qt given; ignoring any system Qt."
elif [[ -n "${INSTALLED_QT}" ]] && version_ge "${INSTALLED_QT}" "${QT_MIN}"; then
    QT_SOURCE="system"
    ok "Qt ${INSTALLED_QT} is already installed and new enough."
else
    [[ -n "${INSTALLED_QT}" ]] && info "System Qt ${INSTALLED_QT} is older than ${QT_MIN}."
    REPO_QT="$(repo_qt_version || true)"
    if [[ -n "${REPO_QT}" ]] && version_ge "${REPO_QT}" "${QT_MIN}"; then
        QT_SOURCE="distro-package"
        ok "Your distribution ships Qt ${REPO_QT} — installing it from the repositories."
    else
        QT_SOURCE="download"
        [[ -n "${REPO_QT}" ]] && info "Distribution only offers Qt ${REPO_QT}."
        note "Qt ${QT_DOWNLOAD_VERSION} will be downloaded via aqtinstall into ${QT_DOWNLOAD_ROOT}."
    fi
fi
[[ ${SKIP_DEPS} -eq 1 && "${QT_SOURCE}" == "distro-package" ]] && QT_SOURCE="system"

if [[ "${QT_SOURCE}" == "download" ]]; then
    version_ge "${QT_DOWNLOAD_VERSION}" "${QT_MIN}" \
        || die "--qt-version ${QT_DOWNLOAD_VERSION} is below the required ${QT_MIN}."
    QT_PREFIX="${QT_DOWNLOAD_ROOT}/${QT_DOWNLOAD_VERSION}/gcc_64"
fi

# CUDA
if [[ "${CUDA_MODE}" == "auto" ]]; then
    if have nvcc; then CUDA_MODE="on"; else CUDA_MODE="off"; fi
fi
[[ "${CUDA_MODE}" == "on" ]] && info "CUDA toolkit found — DIGGA's CUDA back-end will be built."

# ════════════════════════ the plan ══════════════════════════════════════════
printf '\n'
say "${C_BOLD}Plan${C_RESET}"
say "    repository      ${REPO_DIR}"
say "    build directory ${BUILD_DIR}"
say "    install prefix  ${PREFIX}$( [[ ${DO_INSTALL} -eq 0 ]] && printf ' (build only)' )"
say "    build type      ${BUILD_TYPE}, ${JOBS} parallel jobs"
say "    Qt              ${QT_SOURCE}$( [[ -n "${QT_PREFIX}" ]] && printf ' (%s)' "${QT_DOWNLOAD_VERSION}" )"
say "    CUDA            ${CUDA_MODE}"
say "    lcurve binaries $( [[ ${WITH_LCURVE} -eq 1 ]] && echo 'build and bundle' || echo 'skip' )"
say "    system packages $( [[ ${SKIP_DEPS} -eq 1 ]] && echo 'skipped (--skip-deps)' || echo "installed with ${PKG_MGR}" )"
if [[ ${SKIP_DEPS} -eq 0 || ( ${DO_INSTALL} -eq 1 && ${NEED_ROOT_INSTALL} -eq 1 ) ]]; then
    say "    ${C_DIM}root access is needed for package installation and/or ${PREFIX}${C_RESET}"
fi

confirm "Continue?" || { say "    Aborted."; exit 0; }

if [[ -n "${SUDO}" ]] && { [[ ${SKIP_DEPS} -eq 0 ]] || { [[ ${DO_INSTALL} -eq 1 ]] && [[ ${NEED_ROOT_INSTALL} -eq 1 ]]; }; }; then
    printf '\n'
    sudo -v || die "Could not obtain root privileges."
    # keep the sudo timestamp fresh while long steps run in the background
    ( while true; do sudo -n true 2>/dev/null; sleep 45; kill -0 "$$" 2>/dev/null || exit 0; done ) &
    SUDO_KEEPALIVE_PID=$!
fi

# ════════════════════════ 3. system packages ════════════════════════════════
step "Installing system packages"

apt_install() {
    if ${SUDO} DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@"; then
        return 0
    fi
    # One bad package name shouldn't sink the whole run — retry individually.
    local p
    for p in "$@"; do
        ${SUDO} DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${p}" \
            || printf '%s\n' "${p}" >>"${MISSING_PKG_FILE}"
    done
    return 0
}

pacman_install() {
    if ${SUDO} pacman -S --needed --noconfirm "$@"; then
        return 0
    fi
    local p
    for p in "$@"; do
        ${SUDO} pacman -S --needed --noconfirm "${p}" \
            || printf '%s\n' "${p}" >>"${MISSING_PKG_FILE}"
    done
    return 0
}

# First of the given package names that the distribution actually offers.
apt_first_available() {
    local p
    for p in "$@"; do
        if apt-cache policy "${p}" 2>/dev/null | grep -qE 'Candidate: [^(]'; then
            printf '%s\n' "${p}"; return 0
        fi
    done
    return 1
}

if [[ ${SKIP_DEPS} -eq 1 ]]; then
    info "Skipping package installation (--skip-deps)."
elif [[ "${PKG_MGR}" == "apt" ]]; then
    run_step "Refreshing the package index" ${SUDO} apt-get update \
        || warn "apt-get update failed — continuing with the cached index."
    APT_PKGS=(
        # toolchain
        build-essential cmake ninja-build git curl wget ca-certificates
        pkg-config gfortran ccache patchelf
        # python (DIGGA links against it; lightcurvequery needs a venv)
        python3 python3-dev python3-pip python3-venv python3-numpy
        # maths / science
        libeigen3-dev libopenblas-dev liblapacke-dev
        libboost-dev libboost-filesystem-dev libboost-iostreams-dev
        libboost-program-options-dev
        nlohmann-json3-dev libfftw3-dev libtbb-dev libcxxopts-dev libgsl-dev
        # FITS + networking + compression
        libccfits-dev libcfitsio-dev libcurl4-openssl-dev zlib1g-dev libssl-dev
        # desktop integration + runtime helpers
        desktop-file-utils shared-mime-info gnuplot-nox
    )
    if [[ "${QT_SOURCE}" == "distro-package" ]]; then
        APT_PKGS+=(qt6-base-dev qt6-base-dev-tools qt6-wayland)
        # Package names for the Svg module and the SQLite driver have moved
        # around between releases — take whichever this distribution has.
        for _alt in "libqt6svg6-dev qt6-svg-dev" \
                    "libqt6sql6-sqlite qt6-base-dev" ; do
            # shellcheck disable=SC2086
            _pick="$(apt_first_available ${_alt} || true)"
            [[ -n "${_pick}" ]] && APT_PKGS+=("${_pick}")
        done
    elif [[ "${QT_SOURCE}" == "download" ]]; then
        # Everything a self-downloaded Qt needs to build and run against.
        APT_PKGS+=(
            libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev libvulkan-dev
            libdbus-1-dev libfontconfig1-dev libfreetype-dev libcups2-dev
            libxcb-cursor-dev libxcb-icccm4-dev libxcb-image0-dev
            libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render-util0-dev
            libxcb-shape0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-xkb-dev
            libxcb-util-dev libx11-dev libxext-dev libpng-dev
        )
    fi
    run_step "Installing ${#APT_PKGS[@]} packages (this can take a while)" \
        apt_install "${APT_PKGS[@]}" \
        || die "Package installation failed. See the log for details."
else
    PACMAN_PKGS=(
        base-devel cmake ninja git curl wget pkgconf gcc-fortran ccache patchelf
        python python-numpy python-pip
        eigen openblas boost nlohmann-json fftw onetbb cxxopts gsl
        ccfits cfitsio zlib openssl
        desktop-file-utils shared-mime-info gnuplot
    )
    [[ "${QT_SOURCE}" == "distro-package" ]] && PACMAN_PKGS+=(qt6-base qt6-svg qt6-wayland)
    # A full -Syu rather than a bare -Sy: installing new packages against a
    # freshly synced database without upgrading is a partial upgrade, which
    # Arch explicitly does not support.
    run_step "Synchronising and upgrading the system (pacman -Syu)" \
        ${SUDO} pacman -Syu --noconfirm \
        || warn "pacman -Syu failed — continuing with the currently installed packages."
    run_step "Installing ${#PACMAN_PKGS[@]} packages (this can take a while)" \
        pacman_install "${PACMAN_PKGS[@]}" \
        || die "Package installation failed. See the log for details."
fi

if [[ -s "${MISSING_PKG_FILE}" ]]; then
    warn "These packages could not be installed: $(tr '\n' ' ' <"${MISSING_PKG_FILE}")"
    info "The build may still succeed — the next step fills in common gaps."
fi

# ════════════════════════ 4. dependency gap filling ═════════════════════════
step "Filling in dependencies the distribution doesn't package"

LIBDIR_GLOBS=(/usr/lib /usr/lib64 /usr/local/lib /usr/lib/*-linux-gnu)

# 4a. ankerl/unordered_dense — DIGGA includes <ankerl/unordered_dense.h> but its
#     fallback path looks for a bare unordered_dense.h on the include path.
ensure_unordered_dense() {
    if find_first /usr/lib/cmake/unordered_dense /usr/lib64/cmake/unordered_dense \
                  /usr/lib/*/cmake/unordered_dense /usr/local/lib/cmake/unordered_dense >/dev/null; then
        info "unordered_dense: CMake package present."
        return 0
    fi
    if find_first /usr/include/unordered_dense.h /usr/local/include/unordered_dense.h >/dev/null; then
        info "unordered_dense: header present."
        return 0
    fi
    local existing
    if existing="$(find_first /usr/include/ankerl/unordered_dense.h /usr/local/include/ankerl/unordered_dense.h)"; then
        ${SUDO} ln -sf "${existing}" /usr/local/include/unordered_dense.h
        ok "unordered_dense: linked ${existing} onto the include path."
        return 0
    fi
    fetch_to "${UNORDERED_DENSE_URL}" /usr/local/include/ankerl/unordered_dense.h
    ${SUDO} ln -sf /usr/local/include/ankerl/unordered_dense.h /usr/local/include/unordered_dense.h
    ok "unordered_dense: header installed into /usr/local/include."
}

# 4b. cxxopts (header-only) — packaged on both distros, but be defensive.
ensure_cxxopts() {
    if find_first /usr/include/cxxopts.hpp /usr/local/include/cxxopts.hpp \
                  /usr/lib/cmake/cxxopts /usr/lib/*/cmake/cxxopts >/dev/null; then
        info "cxxopts: present."
        return 0
    fi
    fetch_to "${CXXOPTS_URL}" /usr/local/include/cxxopts.hpp
    ok "cxxopts: header installed into /usr/local/include."
}

# 4c. FFTW3Config.cmake — rv_mcmc does find_package(FFTW3 REQUIRED), which
#     Debian/Ubuntu's libfftw3-dev does not ship.
ensure_fftw3_config() {
    local d
    for d in "${LIBDIR_GLOBS[@]}"; do
        [[ -f "${d}/cmake/fftw3/FFTW3Config.cmake" ]] && { info "FFTW3: CMake package present."; return 0; }
    done
    local lib
    lib="$(find_first /usr/lib/x86_64-linux-gnu/libfftw3.so /usr/lib/libfftw3.so \
                      /usr/lib64/libfftw3.so /usr/local/lib/libfftw3.so)" \
        || { warn "FFTW3: libfftw3.so not found — rv_mcmc may fail to configure."; return 0; }
    local tmp; tmp="$(mktemp)"
    cat >"${tmp}" <<EOF
# Generated by ASTRA's install-linux.sh: minimal FFTW3 CMake package for
# distributions whose fftw development package ships no config file.
set(FFTW3_INCLUDE_DIRS /usr/include)
set(FFTW3_LIBRARIES    ${lib})
if(NOT TARGET FFTW3::fftw3)
  add_library(FFTW3::fftw3 UNKNOWN IMPORTED)
  set_target_properties(FFTW3::fftw3 PROPERTIES
    IMPORTED_LOCATION "\${FFTW3_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "\${FFTW3_INCLUDE_DIRS}")
endif()
if(NOT TARGET fftw3)
  add_library(fftw3 ALIAS FFTW3::fftw3)
endif()
set(FFTW3_FOUND TRUE)
EOF
    ${SUDO} install -Dm644 "${tmp}" /usr/local/lib/cmake/fftw3/FFTW3Config.cmake
    rm -f "${tmp}"
    ok "FFTW3: CMake package written to /usr/local/lib/cmake/fftw3."
}

# 4d. OpenBLASConfig.cmake — DIGGA links the OpenBLAS::OpenBLAS target.
ensure_openblas_config() {
    local d f
    for d in "${LIBDIR_GLOBS[@]}"; do
        for f in "${d}"/cmake/OpenBLAS/OpenBLASConfig.cmake "${d}"/cmake/openblas/OpenBLASConfig.cmake \
                 "${d}"/cmake/openblas/openblas-config.cmake; do
            [[ -f "${f}" ]] && { info "OpenBLAS: CMake package present."; return 0; }
        done
    done
    local lib inc
    lib="$(find_first /usr/lib/x86_64-linux-gnu/libopenblas.so /usr/lib/libopenblas.so \
                      /usr/lib64/libopenblas.so /usr/local/lib/libopenblas.so)" \
        || { warn "OpenBLAS: libopenblas.so not found — DIGGA will fail to configure."; return 0; }
    inc="$(find_first /usr/include/openblas /usr/include/x86_64-linux-gnu/openblas-pthread \
                      /usr/include/x86_64-linux-gnu/openblas-openmp /usr/include)" || inc=/usr/include
    local tmp; tmp="$(mktemp)"
    cat >"${tmp}" <<EOF
# Generated by ASTRA's install-linux.sh: minimal OpenBLAS CMake package for
# distributions whose openblas development package ships no config file.
set(OpenBLAS_INCLUDE_DIRS ${inc})
set(OpenBLAS_INCLUDE_DIR  ${inc})
set(OpenBLAS_LIBRARIES    ${lib})
set(OpenBLAS_LIBRARY      ${lib})
if(NOT TARGET OpenBLAS::OpenBLAS)
  add_library(OpenBLAS::OpenBLAS UNKNOWN IMPORTED)
  set_target_properties(OpenBLAS::OpenBLAS PROPERTIES
    IMPORTED_LOCATION "\${OpenBLAS_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "\${OpenBLAS_INCLUDE_DIRS}")
endif()
set(OpenBLAS_FOUND TRUE)
EOF
    ${SUDO} install -Dm644 "${tmp}" /usr/local/lib/cmake/OpenBLAS/OpenBLASConfig.cmake
    rm -f "${tmp}"
    ok "OpenBLAS: CMake package written to /usr/local/lib/cmake/OpenBLAS."
}

# 4e. gnuplot-iostream.h — only needed for the optional lcurve binaries.
ensure_gnuplot_iostream() {
    if find_first /usr/include/gnuplot-iostream.h /usr/local/include/gnuplot-iostream.h \
                  /usr/include/gnuplot-iostream/gnuplot-iostream.h >/dev/null; then
        info "gnuplot-iostream: present."
        return 0
    fi
    fetch_to "${GNUPLOT_IOSTREAM_URL}" /usr/local/include/gnuplot-iostream.h
    ok "gnuplot-iostream: header installed into /usr/local/include."
}

if [[ ${SKIP_DEPS} -eq 1 ]]; then
    info "Skipping dependency gap filling (--skip-deps)."
else
    ${SUDO} mkdir -p /usr/local/include /usr/local/lib/cmake
    ensure_unordered_dense
    ensure_cxxopts
    ensure_fftw3_config
    ensure_openblas_config
    [[ ${WITH_LCURVE} -eq 1 ]] && ensure_gnuplot_iostream
fi

# ════════════════════════ 5. Qt ═════════════════════════════════════════════
step "Preparing Qt"

if [[ "${QT_SOURCE}" == "download" ]]; then
    if [[ -x "${QT_PREFIX}/bin/qmake" || -d "${QT_PREFIX}/lib/cmake/Qt6" ]]; then
        ok "Qt ${QT_DOWNLOAD_VERSION} is already present in ${QT_PREFIX}."
    else
        AQT_VENV="${QT_DOWNLOAD_ROOT}/.aqt-venv"
        if [[ ! -x "${AQT_VENV}/bin/aqt" ]]; then
            run_step "Creating a virtualenv for aqtinstall" \
                python3 -m venv "${AQT_VENV}" \
                || die "Could not create a Python virtualenv (install python3-venv)."
            run_step "Installing aqtinstall" \
                "${AQT_VENV}/bin/pip" install --quiet --upgrade pip aqtinstall \
                || die "Could not install aqtinstall."
        fi
        run_step "Downloading Qt ${QT_DOWNLOAD_VERSION} (~1.5 GB, be patient)" \
            "${AQT_VENV}/bin/aqt" install-qt linux desktop "${QT_DOWNLOAD_VERSION}" \
                linux_gcc_64 -O "${QT_DOWNLOAD_ROOT}" \
            || die "Qt download failed. Try a different --qt-version, or install Qt >= ${QT_MIN} yourself and re-run with --skip-deps."
        [[ -d "${QT_PREFIX}/lib/cmake/Qt6" ]] \
            || die "Qt was downloaded but ${QT_PREFIX} does not look like a Qt prefix."
        ok "Qt ${QT_DOWNLOAD_VERSION} installed into ${QT_PREFIX}."
    fi
else
    FINAL_QT="$(system_qt_version 2>/dev/null || true)"
    [[ -n "${FINAL_QT}" ]] || die "Qt6 still not found after installing packages."
    version_ge "${FINAL_QT}" "${QT_MIN}" \
        || die "Qt ${FINAL_QT} is installed but ASTRA needs >= ${QT_MIN}. Re-run with --download-qt."
    ok "Using the system Qt ${FINAL_QT}."
fi

# ════════════════════════ 6. submodules ═════════════════════════════════════
step "Fetching git submodules"

if [[ -d "${REPO_DIR}/.git" ]]; then
    # rv_mcmc is registered with an SSH URL; rewrite it to HTTPS on the fly so
    # the checkout works without a GitHub SSH key. The .gitmodules file itself
    # is left untouched.
    run_step "Updating submodules (DIGGA, rv_mcmc, lightcurvequery, SEDplusplus)" \
        git -C "${REPO_DIR}" \
            -c "url.https://github.com/.insteadOf=git@github.com:" \
            -c "url.https://github.com/.insteadOf=ssh://git@github.com/" \
            submodule update --init --recursive \
        || die "Submodule checkout failed. Check your network connection and the log."
    for sm in DIGGA rv_mcmc lightcurvequery SEDplusplus; do
        if [[ -n "$(ls -A "${REPO_DIR}/external/${sm}" 2>/dev/null)" ]]; then
            info "external/${sm} ✔"
        else
            warn "external/${sm} is empty — the matching feature will be unavailable."
        fi
    done
else
    warn "Not a git checkout — assuming external/ is already populated."
fi

# ════════════════════════ 7. lcurve (optional) ══════════════════════════════
step "Building the lcurve light-curve fitting binaries"

LCURVE_BIN_DIR=""
if [[ ${WITH_LCURVE} -eq 0 ]]; then
    info "Skipped (--no-lcurve); light-curve fitting will use whatever lcurve is on PATH."
else
    LCURVE_SRC="${BUILD_DIR}/lcurve_re"
    LCURVE_BIN_DIR="${BUILD_DIR}/lcurve-bin"
    lcurve_failed=0
    if [[ -d "${LCURVE_SRC}/.git" ]]; then
        run_step "Updating lcurve sources" \
            git -C "${LCURVE_SRC}" pull --ff-only || lcurve_failed=1
    else
        rm -rf "${LCURVE_SRC}"
        run_step "Cloning lcurve" \
            git clone --depth 1 --branch "${LCURVE_REF}" "${LCURVE_REPO}" "${LCURVE_SRC}" \
            || lcurve_failed=1
    fi

    # lcurve targets a newer CMake than Ubuntu 22.04 ships and sets policies
    # 3.22.1 doesn't know. Both are no-ops for this build, so relax them.
    if [[ ${lcurve_failed} -eq 0 ]] && ! version_ge "$(cmake --version | head -n1 | awk '{print $3}')" "3.22.3"; then
        info "Relaxing lcurve's CMake requirements for your older cmake."
        sed -i 's/VERSION 3\.22\.3/VERSION 3.22.1/' "${LCURVE_SRC}/CMakeLists.txt"
        sed -i '/cmake_policy(SET CMP0146 NEW)/d; /cmake_policy(SET CMP0167 NEW)/d' \
            "${LCURVE_SRC}/CMakeLists.txt"
    fi

    if [[ ${lcurve_failed} -eq 0 ]]; then
        run_step "Configuring lcurve" \
            cmake -S "${LCURVE_SRC}" -B "${LCURVE_SRC}/build" \
                -DCMAKE_BUILD_TYPE=Release \
                -DLCURVE_ENABLE_CUDA="$( [[ "${CUDA_MODE}" == "on" ]] && echo ON || echo OFF )" \
            || lcurve_failed=1
    fi
    if [[ ${lcurve_failed} -eq 0 ]]; then
        run_step "Compiling lcurve_levmarq / lcurve_mcmc / lcurve_simplex" \
            cmake --build "${LCURVE_SRC}/build" \
                --target lcurve_levmarq lcurve_mcmc lcurve_simplex -j "${JOBS}" \
            || lcurve_failed=1
    fi

    if [[ ${lcurve_failed} -eq 0 ]]; then
        mkdir -p "${LCURVE_BIN_DIR}"
        for b in lcurve_levmarq lcurve_mcmc lcurve_simplex; do
            if [[ -x "${LCURVE_SRC}/build/${b}" ]]; then
                cp -f "${LCURVE_SRC}/build/${b}" "${LCURVE_BIN_DIR}/"
            else
                lcurve_failed=1
            fi
        done
    fi

    if [[ ${lcurve_failed} -eq 0 ]]; then
        ok "lcurve binaries ready — they will be bundled into the installation."
    else
        LCURVE_BIN_DIR=""
        warn "lcurve could not be built; ASTRA will be installed without it."
        info "Light-curve fitting then needs an lcurve binary configured in ASTRA's settings."
    fi
fi

# ════════════════════════ 8. configure & build ══════════════════════════════
step "Configuring and building ASTRA"

CMAKE_ARGS=(
    -S "${REPO_DIR}" -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_INSTALL_PREFIX="${PREFIX}"
    -DDIGGA_ENABLE_CUDA="$( [[ "${CUDA_MODE}" == "on" ]] && echo ON || echo OFF )"
)
# Only pick a generator for a fresh build tree — CMake refuses to switch
# generators in an existing one.
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]] && have ninja; then
    CMAKE_ARGS+=(-G Ninja)
fi
[[ -n "${QT_PREFIX}" ]] && CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="${QT_PREFIX}")
[[ -n "${LCURVE_BIN_DIR}" ]] && CMAKE_ARGS+=(-DASTRA_LCURVE_DIR="${LCURVE_BIN_DIR}")
if have ccache; then
    CMAKE_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

run_step "Running CMake configure" cmake "${CMAKE_ARGS[@]}" \
    || die "CMake configuration failed. The log lists the missing dependency."

run_step "Compiling ASTRA with ${JOBS} jobs (grab a coffee)" \
    cmake --build "${BUILD_DIR}" -j "${JOBS}" \
    || die "The build failed. See ${LOG_FILE}."

[[ -x "${BUILD_DIR}/ASTRA" ]] && ok "Binary built: ${BUILD_DIR}/ASTRA"

# ════════════════════════ 9. install ════════════════════════════════════════
step "Installing ASTRA"

if [[ ${DO_INSTALL} -eq 0 ]]; then
    info "Skipped (--build-only)."
else
    INSTALL_SUDO=""
    [[ ${NEED_ROOT_INSTALL} -eq 1 ]] && INSTALL_SUDO="${SUDO}"
    run_step "Installing into ${PREFIX}" \
        ${INSTALL_SUDO} cmake --install "${BUILD_DIR}" \
        || die "Installation failed."
fi

# ════════════════════════ summary ═══════════════════════════════════════════
printf '\n'
say "${C_GREEN}${C_BOLD}  ✔ ASTRA is ready.${C_RESET}"
printf '\n'
if [[ ${DO_INSTALL} -eq 1 ]]; then
    say "    Start it from your application menu, or run:  ${C_BOLD}${PREFIX}/bin/ASTRA${C_RESET}"
    case ":${PATH}:" in
        *":${PREFIX}/bin:"*) ;;
        *) note "${PREFIX}/bin is not on your PATH — add it to run 'ASTRA' by name." ;;
    esac
else
    say "    Run it from the build tree:  ${C_BOLD}${BUILD_DIR}/ASTRA${C_RESET}"
fi
[[ -n "${QT_PREFIX}" ]] && \
    note "Qt lives in ${QT_PREFIX} — keep that directory, the binary links against it."
if [[ -n "${LCURVE_BIN_DIR}" ]]; then
    if [[ ${DO_INSTALL} -eq 1 ]]; then
        info "lcurve binaries were bundled into ${PREFIX}/libexec/astra/lcurve."
    else
        info "lcurve binaries are in ${LCURVE_BIN_DIR}."
    fi
fi

if [[ ${#WARNINGS[@]} -gt 0 ]]; then
    printf '\n'
    say "${C_YELLOW}${C_BOLD}  ${#WARNINGS[@]} warning(s):${C_RESET}"
    for w in "${WARNINGS[@]}"; do say "    ${C_YELLOW}!${C_RESET} ${w}"; done
fi
printf '\n'
say "    ${C_DIM}Full log: ${LOG_FILE}${C_RESET}"
printf '\n'
