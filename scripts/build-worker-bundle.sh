#!/usr/bin/env bash
# Builds the self-contained GAEL worker bundle ASTRA installs on remote hosts:
#   dist/gael-worker-linux-x86_64.tar.zst
#
#   scripts/build-worker-bundle.sh [GAEL source dir]   (default: external/GAEL)
#
# The build runs in the container from docker/gael-worker.Dockerfile, whose
# glibc (2.35) is older than every target host, and everything else the worker
# links against is copied into the bundle's lib/.  Bundle layout:
#   bin/GAEL          the CLI, RUNPATH $ORIGIN/../lib
#   bin/gael-worker   entry point ASTRA invokes
#   lib/              shared libraries except glibc's own
#   manifest.json
#
# ASTRA compares an installed worker with this file's hash, so any rebuild is
# rolled out to the hosts on their next remote fit.
#
# GAEL built on its own compiles with -march=native, which on a recent
# desktop emits instructions (AVX-512) that older cluster nodes lack and die
# on with "Illegal instruction".  It is therefore built as a subproject, which
# skips that, for a portable target instead: WORKER_MARCH, by default
# x86-64-v3 (AVX2 + FMA, Haswell / Zen 1 and newer).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
gael=$(cd "${1:-$here/external/GAEL}" && pwd)
image=astra-gael-worker-build
out=$here/dist
cache=$here/.build-worker
march=${WORKER_MARCH:-x86-64-v3}
# GAEL stamps its version from git; a submodule keeps its repository in the
# superproject's .git/modules, so that is mounted too, both at their host
# paths so the checkout's relative gitdir link resolves.
gitdir=$(git -C "$gael" rev-parse --absolute-git-dir)

docker build -t "$image" -f "$here/docker/gael-worker.Dockerfile" "$here/docker"
mkdir -p "$out" "$cache"

# Runs as the calling user so the build tree and bundle are not root-owned,
# and so git accepts the mounted checkout for the version stamp.
docker run --rm \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$gael":"$gael":ro -v "$gitdir":"$gitdir":ro \
    -v "$cache":/build -v "$out":/out \
    -e GAEL_SRC="$gael" -e MARCH="$march" \
    "$image" bash -euo pipefail -c '
mkdir -p /tmp/wrap
cat > /tmp/wrap/CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.18)
project(gael_worker LANGUAGES C CXX Fortran)
add_compile_options(-march=$MARCH -funroll-loops -ftree-vectorize)
add_subdirectory($GAEL_SRC gael)
EOF
cmake -S /tmp/wrap -B "/build/$MARCH" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGAEL_BUILD_REPORT=OFF >/dev/null
cmake --build "/build/$MARCH" --target GAEL

stage=$(mktemp -d)
chmod 755 "$stage"      # mktemp makes it 0700, and the archive root keeps that
mkdir -p "$stage/bin" "$stage/lib"
cp "/build/$MARCH/gael/GAEL" "$stage/bin/GAEL"
strip --strip-unneeded "$stage/bin/GAEL"

# Every library the binary pulls in, transitively, except glibc itself,
# which the host provides and the glibc floor above covers.
ldd "$stage/bin/GAEL" | awk "/=> \\// {print \$3}" | while read -r lib; do
    case $(basename "$lib") in
        libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*|\
        libresolv.so*|libutil.so*|libnsl.so*|libanl.so*) continue ;;
    esac
    cp -L "$lib" "$stage/lib/"
done
patchelf --set-rpath "\$ORIGIN/../lib" "$stage/bin/GAEL"
for l in "$stage"/lib/*; do patchelf --set-rpath "\$ORIGIN" "$l"; done

cat > "$stage/bin/gael-worker" <<EOF
#!/bin/sh
# Entry point ASTRA invokes on the remote host.
here=\$(dirname "\$0")
GAEL_PROGRESS=1
export GAEL_PROGRESS
exec "\$here/GAEL" "\$@"
EOF
chmod +x "$stage/bin/gael-worker"

version=$("$stage/bin/gael-worker" --version)
glibc=$(ldd --version | awk "NR == 1 {print \$NF}")
cat > "$stage/manifest.json" <<EOF
{
  "bundle_format": 1,
  "gael_version": "$version",
  "built_glibc": "$glibc",
  "march": "$MARCH",
  "platform": "linux-x86_64"
}
EOF

tar -C "$stage" --zstd -cf /out/gael-worker-linux-x86_64.tar.zst.part .
mv /out/gael-worker-linux-x86_64.tar.zst.part /out/gael-worker-linux-x86_64.tar.zst
echo "Built $version"
'
ls -l "$out/gael-worker-linux-x86_64.tar.zst"
