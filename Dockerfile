# syntax=docker/dockerfile:1.7
FROM ubuntu:22.04 AS builder
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
        apt-get install -y --no-install-recommends build-essential cmake clang ccache gperf wget git \
        ninja-build pkg-config autoconf automake libtool \
        libjemalloc-dev lsb-release software-properties-common gnupg && \
        rm -rf /var/lib/apt/lists/*

RUN wget https://apt.llvm.org/llvm.sh && \
    chmod +x llvm.sh && \
    ./llvm.sh 22 all && \
    rm -rf /var/lib/apt/lists/*

ENV CC=/usr/bin/clang-22
ENV CXX=/usr/bin/clang++-22
ENV CCACHE_DIR=/root/.cache/ccache
ENV CCACHE_BASEDIR=/ton
ENV CCACHE_NOHASHDIR=true
ENV CCACHE_MAXSIZE=20G
ENV CCACHE_COMPILERCHECK=content

WORKDIR /
RUN mkdir ton
WORKDIR /ton

COPY ./ ./

# Build-only arguments are intentionally declared after the expensive toolchain
# layers. Docker includes in-scope ARG values in subsequent RUN cache keys, so
# a new source revision or architecture must not invalidate apt/LLVM setup.
ARG NINJA_JOBS=2
ARG PORTABLE=1
ARG TON_ARCH=
ARG TON_BUILD_TARGETS="storage-daemon storage-daemon-cli tonlibjson fift func validator-engine validator-engine-console generate-random-id dht-server lite-client native-load-generator tolk rldp-http-proxy dht-server proxy-liteserver create-state blockchain-explorer emulator tonlibjson http-proxy dht-ping-servers dht-resolve"
ARG VCS_REF=unknown
ARG VCS_DATE=unknown
RUN --mount=type=cache,id=corton-ton-ccache,target=/root/.cache/ccache,sharing=locked \
    export GIT_REVISION="${VCS_REF}" GIT_REVISION_DATE="${VCS_DATE}" && \
        ccache --zero-stats && \
        mkdir build && \
        cd build && \
        cmake -GNinja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -DPORTABLE="${PORTABLE}" -DTON_ARCH="${TON_ARCH}" -DTON_USE_JEMALLOC=ON .. && \
        ninja -j "${NINJA_JOBS}" ${TON_BUILD_TARGETS} && \
        ccache --show-stats

# Both architecture publishers use this builder. Native correctness must pass
# in the same portable toolchain before any final image can be assembled/pushed.
# There is intentionally no build argument that disables the publication gate.
RUN bash /ton/docker/run-native-publication-tests.sh --build-dir /ton/build --jobs "${NINJA_JOBS}"

FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y wget curl libatomic1 openssl libsodium-dev libmicrohttpd-dev liblz4-dev libjemalloc-dev htop \
    net-tools netcat iptraf-ng jq tcpdump pv plzip && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir -p /var/ton-work/db /var/ton-work/scripts /usr/share/ton/smartcont/auto /usr/lib/fift/

COPY --from=builder /ton/build/storage/storage-daemon/storage-daemon /usr/local/bin/
COPY --from=builder /ton/build/storage/storage-daemon/storage-daemon-cli /usr/local/bin/
COPY --from=builder /ton/build/lite-client/lite-client /usr/local/bin/
COPY --from=builder /ton/build/lite-client/native-load-generator /usr/local/bin/
COPY --from=builder /ton/build/validator-engine/validator-engine /usr/local/bin/
COPY --from=builder /ton/build/validator-engine-console/validator-engine-console /usr/local/bin/
COPY --from=builder /ton/build/utils/generate-random-id /usr/local/bin/
COPY --from=builder /ton/build/blockchain-explorer/blockchain-explorer /usr/local/bin/
COPY --from=builder /ton/build/crypto/create-state /usr/local/bin/
COPY --from=builder /ton/build/utils/proxy-liteserver /usr/local/bin/
COPY --from=builder /ton/build/dht-server/dht-server /usr/local/bin/
COPY --from=builder /ton/build/dht/dht-ping-servers /usr/local/bin/
COPY --from=builder /ton/build/dht/dht-resolve /usr/local/bin/
COPY --from=builder /ton/build/rldp-http-proxy/rldp-http-proxy /usr/local/bin/
COPY --from=builder /ton/build/http/http-proxy  /usr/local/bin/
COPY --from=builder /ton/build/tonlib/libtonlibjson.so /usr/local/bin/
COPY --from=builder /ton/build/emulator/libemulator.so /usr/local/bin/
COPY --from=builder /ton/build/tolk/tolk /usr/local/bin/
COPY --from=builder /ton/build/crypto/fift /usr/local/bin/
COPY --from=builder /ton/build/crypto/func /usr/local/bin/
COPY --from=builder /ton/crypto/smartcont/* /usr/share/ton/smartcont/
COPY --from=builder /ton/crypto/smartcont/auto/* /usr/share/ton/smartcont/auto/
COPY --from=builder /ton/crypto/fift/lib/* /usr/lib/fift/

WORKDIR /var/ton-work/db
COPY ./docker/init.sh /var/ton-work/scripts/
RUN chmod +x /var/ton-work/scripts/init.sh

ENTRYPOINT ["/var/ton-work/scripts/init.sh"]

# Apply provenance last for the same reason as in the builder stage: changing
# labels must not invalidate package installation or binary-copy layers.
ARG VCS_REF=unknown
ARG BUILD_DATE=unknown
LABEL org.opencontainers.image.revision=$VCS_REF \
      org.opencontainers.image.created=$BUILD_DATE
