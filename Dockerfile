# =============================================================================
# Dockerfile — build and run Virtual Camera Engine on any Linux machine that
# has Docker, without installing compilers or FFmpeg on the host.
#
#   docker build -t virtual-camera-engine .
#   docker run --rm -it --device /dev/video10 -v "$HOME/Videos":/media:ro \
#       virtual-camera-engine --input /media/clip.mp4
#
# The camera device itself comes from the HOST's kernel: load v4l2loopback on
# the host first (sudo tools/setup_loopback.sh). Containers share the host
# kernel, so they cannot load kernel modules themselves.
# =============================================================================

# ---- Stage 1: build ----------------------------------------------------------
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Tests are run in CI; the image only needs the program.
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DVCAM_BUILD_TESTS=OFF \
    && cmake --build /build -j"$(nproc)" \
    && cmake --install /build --prefix /opt/vcam

# ---- Stage 2: small runtime image (only the FFmpeg shared libraries) ---------
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libavformat60 libavcodec60 libavutil58 libswscale7 v4l-utils \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/vcam /opt/vcam
ENV PATH="/opt/vcam/bin:${PATH}"
ENTRYPOINT ["virtual-camera"]
CMD ["--help"]
