ARG UBUNTU_IMAGE=ubuntu:24.04

FROM ${UBUNTU_IMAGE} AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libprotobuf-dev \
        libsrtp2-dev \
        libssl-dev \
        ninja-build \
        pkg-config \
        protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY proto /src/proto
COPY sfu /src/sfu

RUN rm -rf /src/sfu/proto/pb \
    && mkdir -p /src/sfu/proto/pb \
    && protoc --proto_path=/src/proto \
        --cpp_out=/src/sfu/proto/pb \
        /src/proto/signaling.proto \
        /src/proto/sfu_rpc.proto

RUN cmake -S /src/sfu -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPENSSL_ROOT_HINT=/usr \
        -DLIBSRTP_ROOT_HINT=/usr \
    && cmake --build /build --target meeting_sfu

FROM ${UBUNTU_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libprotobuf-dev \
        libsrtp2-1 \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --uid 10001 --create-home --home-dir /app appuser

WORKDIR /app
COPY --from=build /build/meeting_sfu /app/meeting_sfu

USER appuser
EXPOSE 9000/tcp 10000/udp

ENTRYPOINT ["/app/meeting_sfu"]

