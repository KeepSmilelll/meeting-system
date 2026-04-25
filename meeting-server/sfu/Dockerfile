# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build

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
COPY . .

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPENSSL_ROOT_HINT=/usr \
        -DLIBSRTP_ROOT_HINT=/usr \
    && cmake --build /build --target meeting_sfu

FROM ubuntu:24.04

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

