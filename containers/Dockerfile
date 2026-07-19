# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: GPL-3.0-only

FROM ubuntu:22.04@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG HTSLIB_VERSION=1.18
ARG HTSLIB_SHA256=f1ab53a593a2320a1bfadf4ef915dae784006c5b5c922c8a8174d7530a9af18f

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential=12.9ubuntu3 \
        ca-certificates=20240203~22.04.1 \
        cmake=3.22.1-1ubuntu1.22.04.2 \
        curl=7.81.0-1ubuntu1.24 \
        libbz2-dev=1.0.8-5build1 \
        libdeflate-dev=1.10-2 \
        libjansson-dev=2.13.1-1.1build3 \
        liblzma-dev=5.2.5-2ubuntu1 \
        libssl-dev=3.0.2-0ubuntu1.23 \
        jq=1.6-2.1ubuntu3.2 \
        pkg-config=0.29.2-1ubuntu3 \
        python3-jsonschema=3.2.0-0ubuntu2 \
        zlib1g-dev=1:1.2.11.dfsg-2ubuntu9.2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/dependencies
RUN curl -fsSLo "htslib-${HTSLIB_VERSION}.tar.bz2" \
        "https://github.com/samtools/htslib/releases/download/${HTSLIB_VERSION}/htslib-${HTSLIB_VERSION}.tar.bz2" \
    && echo "${HTSLIB_SHA256}  htslib-${HTSLIB_VERSION}.tar.bz2" | sha256sum -c - \
    && tar -xf "htslib-${HTSLIB_VERSION}.tar.bz2" \
    && cd "htslib-${HTSLIB_VERSION}" \
    && ./configure \
        --prefix=/opt/longlineage-deps \
        --disable-libcurl \
        --disable-plugins \
        --with-libdeflate \
    && make -j"$(nproc)" \
    && make install

WORKDIR /src
COPY . /src
ENV PKG_CONFIG_PATH=/opt/longlineage-deps/lib/pkgconfig
RUN cmake -S /src -B /tmp/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/longlineage \
        -DLONGLINEAGE_BUILD_TESTS=ON \
        -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON \
        -DLONGLINEAGE_WARNINGS_AS_ERRORS=ON \
    && cmake --build /tmp/build --parallel 4
RUN --network=none LONGLINEAGE_NETWORK_ISOLATED=1 \
    /src/scripts/ci/run_no_network_tests.sh /tmp/build
RUN cmake --install /tmp/build

FROM ubuntu:22.04@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates=20240203~22.04.1 \
        libbz2-1.0=1.0.8-5build1 \
        libdeflate0=1.10-2 \
        libjansson4=2.13.1-1.1build3 \
        liblzma5=5.2.5-2ubuntu1 \
        libssl3=3.0.2-0ubuntu1.23 \
        zlib1g=1:1.2.11.dfsg-2ubuntu9.2 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 65532 longlineage \
    && useradd --system --uid 65532 --gid 65532 --no-create-home longlineage

COPY --from=builder /opt/longlineage-deps /opt/longlineage-deps
COPY --from=builder /opt/longlineage /opt/longlineage

ENV PATH=/opt/longlineage/bin:$PATH \
    LD_LIBRARY_PATH=/opt/longlineage-deps/lib \
    REF_PATH="" \
    HTS_PATH=""

USER 65532:65532
WORKDIR /work
ENTRYPOINT ["longlineage"]
CMD ["--help"]
