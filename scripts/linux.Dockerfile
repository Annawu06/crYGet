FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config dpkg-dev desktop-file-utils \
    libx11-dev libxft-dev libfontconfig1-dev libcurl4-openssl-dev libjpeg-dev \
    libavformat-dev libavcodec-dev libavutil-dev \
    libnode-dev libuv1-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/cryget
COPY Makefile ./
COPY src/ ./src/
COPY third_party/ ./third_party/
COPY assets/ ./assets/
COPY screenshots/linux-download-queue.png screenshots/linux-language-menu.png ./screenshots/
COPY scripts/build-linux.sh scripts/cryget.desktop scripts/cryget.metainfo.xml ./scripts/
ARG CRYGET_VERSION=2026.10.08
ENV CRYGET_VERSION=$CRYGET_VERSION
RUN bash scripts/build-linux.sh
