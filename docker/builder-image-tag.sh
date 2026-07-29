#!/usr/bin/env bash
# Print the tag identifying the AppImage builder image for the current
# Dockerfile + build args.
#
# Shared by build-appimage.sh and the two AppImage workflows so a local build
# and CI always agree on which image a given Dockerfile corresponds to: change
# the Dockerfile (or bump a version below) and both sides independently derive
# the new tag, so nothing is ever built against a stale environment.
#
# Env overrides must match those honoured by build-appimage.sh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKERFILE="${HERE}/appimage-builder.Dockerfile"

QT_VERSION="${QT_VERSION:-6.11.1}"
OPENBLAS_VERSION="${OPENBLAS_VERSION:-0.3.27}"
ASTRA_BUNDLE_ISIS="${ASTRA_BUNDLE_ISIS:-1}"

[[ -f "${DOCKERFILE}" ]] || { echo "Missing ${DOCKERFILE}" >&2; exit 1; }

(cat "${DOCKERFILE}"; echo "${QT_VERSION} ${OPENBLAS_VERSION} ${ASTRA_BUNDLE_ISIS}") \
  | sha256sum | cut -c1-12
