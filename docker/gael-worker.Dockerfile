# Build environment for the self-contained GAEL worker bundle that ASTRA
# uploads to remote hosts.
#
# GAEL requires Python3 development files at configure time even with its
# report target off, so they are installed although the bundle never links
# against libpython.
#
# Ubuntu 22.04 pins glibc at 2.35, which is older than every target we
# support (Debian 12 has 2.36, the SUSE analysis machines 2.38), so a binary
# built here runs on all of them. Everything else the worker needs is shipped
# inside the bundle, so the remote host needs nothing preinstalled.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build gfortran git ca-certificates \
        libeigen3-dev libopenblas-dev libboost-filesystem-dev \
        nlohmann-json3-dev libtbb-dev libccfits-dev libcfitsio-dev \
        libcxxopts-dev patchelf zstd file \
        python3-dev python3-numpy \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# Debian and Ubuntu ship OpenBLAS without the CMake package config that
# find_package(OpenBLAS) needs (they provide pkg-config instead), so supply a
# minimal one pointing at the system library. Purely a packaging gap: the
# library itself is the one GAEL wants.
RUN mkdir -p /usr/lib/x86_64-linux-gnu/cmake/OpenBLAS && \
    printf '%s\n' \
      'if(NOT TARGET OpenBLAS::OpenBLAS)' \
      '  add_library(OpenBLAS::OpenBLAS UNKNOWN IMPORTED)' \
      '  set_target_properties(OpenBLAS::OpenBLAS PROPERTIES' \
      '    IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libopenblas.so"' \
      '    INTERFACE_INCLUDE_DIRECTORIES "/usr/include/x86_64-linux-gnu")' \
      'endif()' \
      'set(OpenBLAS_LIBRARIES OpenBLAS::OpenBLAS)' \
      'set(OpenBLAS_FOUND TRUE)' \
      > /usr/lib/x86_64-linux-gnu/cmake/OpenBLAS/OpenBLASConfig.cmake

# ankerl::unordered_dense is header-only and not packaged for Ubuntu 22.04;
# GAEL looks for the header on the include path.
ARG UNORDERED_DENSE_VERSION=v4.4.0
RUN git clone --depth 1 --branch ${UNORDERED_DENSE_VERSION}         https://github.com/martinus/unordered_dense.git /tmp/ud &&     cp /tmp/ud/include/ankerl/unordered_dense.h /usr/include/ &&     mkdir -p /usr/include/ankerl &&     cp /tmp/ud/include/ankerl/unordered_dense.h /usr/include/ankerl/ &&     rm -rf /tmp/ud
