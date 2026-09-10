#!/usr/bin/env bash
# Resolve the two ISIS script libraries (isisscripts, stellar_isisscripts) into
# a work directory, tolerating an unreachable upstream.
#
# Both libraries live on one university server and, unlike everything else in
# the bundles, they are re-fetched at HEAD on every build by design. That makes
# a single server the hard dependency of every release: on 2026-09-10 its GitLab
# started answering the ref advertisement (nginx, 200) but rejecting the
# git-upload-pack POST (Apache, 403), which killed the AppImage outright and
# quietly shipped a .dmg with no script libraries at all.
#
# So the fetch is a chain, freshest first:
#
#   1. clone upstream HEAD (3 attempts, backing off)
#   2. the newest of
#        - the snapshot cache: a tarball of the last clone that worked, kept in
#          a GitHub Actions cache on the default branch (see
#          .github/workflows/isis-scripts-cache.yml) or in ~/.cache locally
#        - a local ISIS install, e.g. the src/ dir of an ISIS_install tree
#   3. nothing, and the caller drops into its non-fatal path
#
# Steps 2 and 3 announce themselves loudly, with a GitHub annotation on a
# runner: a release built off a months-old snapshot is fine, a release that
# silently isn't is not.
#
# Usage:  source scripts/isis-scripts.sh
#         isis_scripts_fetch <workdir>       # populates <workdir>/<name> for both
#
# Env:
#   ASTRA_ISIS_SCRIPTS_CACHE    snapshot dir (default ~/.cache/astra-isis-scripts)
#   ASTRA_ISIS_SCRIPTS_DIR      local install holding both clones side by side;
#                               auto-probed if unset (see below)
#   ASTRA_ISIS_SCRIPTS_OFFLINE  1 skips the clone, to exercise the fallbacks

# name=url, in the order the callers build them. isisscripts (Remeis) moved off
# the old /git.public gitweb (dumb HTTP, now 404) to the Remeis GitLab, which
# speaks smart HTTP, so --depth 1 works for both.
ISIS_SCRIPTS_REPOS=(
  "isisscripts=https://www.sternwarte.uni-erlangen.de/gitlab/remeis/isisscripts.git"
  "stellar_isisscripts=http://www.sternwarte.uni-erlangen.de/gitlab/irrgang/stellar.git"
)

ISIS_SCRIPTS_CACHE_DIR="${ASTRA_ISIS_SCRIPTS_CACHE:-${HOME}/.cache/astra-isis-scripts}"

# Where a hand-maintained ISIS install keeps its two clones. Probed only when
# ASTRA_ISIS_SCRIPTS_DIR is unset, so CI (where none of these exist) is unaffected.
_isis_scripts_local_dir() {
  if [[ -n "${ASTRA_ISIS_SCRIPTS_DIR:-}" ]]; then
    [[ -d "${ASTRA_ISIS_SCRIPTS_DIR}" ]] && printf '%s' "${ASTRA_ISIS_SCRIPTS_DIR}"
    return 0
  fi
  local d
  for d in "${HOME}/Projects/ISIS_install/src" "${HOME}/ISIS_install/src" \
           "${HOME}/isis_install/src"; do
    if [[ -d "${d}" ]]; then printf '%s' "${d}"; return 0; fi
  done
  return 0
}

# Commit timestamp of a checkout, 0 when it is not a repo. Used to pick the
# freshest fallback rather than assuming the cache beats the local install.
_isis_scripts_ts() {
  local ts
  ts="$(git -C "$1" log -1 --format=%ct 2>/dev/null)" || ts=""
  printf '%s' "${ts:-0}"
}

# GNU date takes -d @ts, BSD date (macOS) takes -r ts.
_isis_scripts_date() {
  date -d "@$1" '+%Y-%m-%d' 2>/dev/null || date -r "$1" '+%Y-%m-%d' 2>/dev/null || printf '%s' "$1"
}

_isis_scripts_note() {   # <level> <message>
  local level="$1" msg="$2"
  echo "${msg}"
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "::${level} title=ISIS script libraries::${msg}"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
      echo "- ${msg}" >> "${GITHUB_STEP_SUMMARY}"
    fi
  fi
}

# Pack a checkout into the snapshot cache.
#
# `.git` is kept but its objects are left out. Both Makefiles stamp a version
# string from `git rev-parse HEAD` and fail outright when that command fails, so
# something has to answer it; an object-less .git still resolves a detached HEAD
# to its sha, and that is all they ask for. Keeping the pack would double the
# entry, since stellar_isisscripts is 144 MB of refdata that does not compress.
# Commit date and sha are read out first and stored alongside, because `git log`
# does need the objects.
_isis_scripts_snapshot_save() {   # <workdir> <name>
  local workdir="$1" name="$2" tmp ts sha
  mkdir -p "${ISIS_SCRIPTS_CACHE_DIR}" || return 0
  ts="$(_isis_scripts_ts "${workdir}/${name}")"
  sha="$(git -C "${workdir}/${name}" rev-parse HEAD 2>/dev/null || echo unknown)"
  tmp="${ISIS_SCRIPTS_CACHE_DIR}/${name}.tar.gz.part"
  if tar --exclude="${name}/.git/objects/*" --exclude="${name}/.git/logs" \
         -czf "${tmp}" -C "${workdir}" "${name}" 2>/dev/null; then
    mv -f "${tmp}" "${ISIS_SCRIPTS_CACHE_DIR}/${name}.tar.gz"
    printf '%s %s\n' "${ts}" "${sha}" > "${ISIS_SCRIPTS_CACHE_DIR}/${name}.rev"
  else
    rm -f "${tmp}"
    echo "!!! could not snapshot ${name} into ${ISIS_SCRIPTS_CACHE_DIR} (continuing)"
  fi
  return 0
}

_isis_scripts_clone() {   # <workdir> <name> <url>
  local workdir="$1" name="$2" url="$3" n
  [[ "${ASTRA_ISIS_SCRIPTS_OFFLINE:-0}" == "1" ]] && return 1
  for n in 1 2 3; do
    if git clone --depth 1 "${url}" "${workdir}/${name}"; then return 0; fi
    echo "!!! clone of ${name} failed (attempt ${n}/3)"
    rm -rf "${workdir:?}/${name}"
    if (( n < 3 )); then sleep $((n * 10)); fi
  done
  return 1
}

# Copy a local checkout the way a clone would: tracked files only, .git intact.
# `share/*.sl` and `slirp/*-module.so` are build products that upstream does not
# track, so a plain cp would ship whatever the local machine happened to build,
# for whatever platform it was built on.
_isis_scripts_from_local() {   # <workdir> <name> <srcdir>
  local workdir="$1" name="$2" src="$3"
  if [[ -d "${src}/.git" ]] && git clone --depth 1 "file://${src}" "${workdir}/${name}"; then
    return 0
  fi
  rm -rf "${workdir:?}/${name}"
  cp -a "${src}" "${workdir}/${name}" || return 1
  rm -f "${workdir}/${name}"/share/*.sl "${workdir}/${name}"/share/*.txt \
        "${workdir}/${name}"/share/*.md "${workdir}/${name}"/share/*.html \
        "${workdir}/${name}"/slirp/*.so "${workdir}/${name}"/slirp/*.o 2>/dev/null
  return 0
}

_isis_scripts_one() {   # <workdir> <name> <url>
  local workdir="$1" name="$2" url="$3"
  local snap="${ISIS_SCRIPTS_CACHE_DIR}/${name}.tar.gz"
  local local_dir="" snap_ts=0 local_ts=0 rev=""

  rm -rf "${workdir:?}/${name}"

  if _isis_scripts_clone "${workdir}" "${name}" "${url}"; then
    echo ">>> ${name}: upstream HEAD $(git -C "${workdir}/${name}" rev-parse --short HEAD 2>/dev/null || echo '?')"
    _isis_scripts_snapshot_save "${workdir}" "${name}"
    return 0
  fi
  echo "!!! ${url} unreachable after 3 attempts, looking for a local copy"

  if [[ -f "${snap}" && -f "${ISIS_SCRIPTS_CACHE_DIR}/${name}.rev" ]]; then
    read -r snap_ts rev < "${ISIS_SCRIPTS_CACHE_DIR}/${name}.rev" || true
    [[ "${snap_ts}" =~ ^[0-9]+$ ]] || snap_ts=0
  fi
  local base; base="$(_isis_scripts_local_dir)"
  if [[ -n "${base}" && -d "${base}/${name}" ]]; then
    local_dir="${base}/${name}"
    local_ts="$(_isis_scripts_ts "${local_dir}")"
  fi

  if [[ ! -f "${snap}" && -z "${local_dir}" ]]; then
    _isis_scripts_note error \
      "${name}: upstream is unreachable and there is no snapshot or local copy to fall back on."
    return 1
  fi

  # Freshest wins. A snapshot with no readable date still beats nothing, and a
  # local install that is genuinely newer beats a stale snapshot.
  if [[ -n "${local_dir}" ]] && (( local_ts > snap_ts )); then
    if _isis_scripts_from_local "${workdir}" "${name}" "${local_dir}"; then
      _isis_scripts_note warning \
        "${name}: upstream unreachable, built from the local install ${local_dir} (HEAD of $(_isis_scripts_date "${local_ts}"))."
      return 0
    fi
    echo "!!! ${local_dir} could not be used, trying the snapshot cache"
  fi

  rm -rf "${workdir:?}/${name}"
  if [[ -f "${snap}" ]] && tar -xzf "${snap}" -C "${workdir}"; then
    _isis_scripts_note warning \
      "${name}: upstream unreachable, built from the cached snapshot ${rev:-unknown} (HEAD of $(_isis_scripts_date "${snap_ts}"))."
    return 0
  fi

  # Snapshot missing or corrupt, and the local install was older or absent.
  if [[ -n "${local_dir}" ]] && _isis_scripts_from_local "${workdir}" "${name}" "${local_dir}"; then
    _isis_scripts_note warning \
      "${name}: upstream unreachable, built from the local install ${local_dir}."
    return 0
  fi

  _isis_scripts_note error \
    "${name}: upstream is unreachable and no fallback could be materialised."
  return 1
}

# Snapshot whatever is in <workdir> into the cache, whichever source it came
# from. The clone path does this on its own; a seeding run that had to fall back
# to a local install calls this explicitly, which is how a machine with an ISIS
# tree can prime the cache for everyone else.
isis_scripts_snapshot() {
  local workdir="$1" entry name
  for entry in "${ISIS_SCRIPTS_REPOS[@]}"; do
    name="${entry%%=*}"
    [[ -d "${workdir}/${name}" ]] || continue
    _isis_scripts_snapshot_save "${workdir}" "${name}"
  done
  return 0
}

# Populate <workdir>/isisscripts and <workdir>/stellar_isisscripts. Returns
# non-zero if either could not be resolved; the caller decides how fatal that is.
isis_scripts_fetch() {
  local workdir="$1" entry name url
  mkdir -p "${workdir}"
  for entry in "${ISIS_SCRIPTS_REPOS[@]}"; do
    name="${entry%%=*}"
    url="${entry#*=}"
    _isis_scripts_one "${workdir}" "${name}" "${url}" || return 1
  done
  return 0
}
