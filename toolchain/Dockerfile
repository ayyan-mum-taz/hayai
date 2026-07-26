# Build environment for hayai (Nintendo Switch homebrew).
# Based on devkitPro's published toolchain image; we add only what we need.
FROM devkitpro/devkita64:latest

RUN apt-get update && \
    apt-get -y install --no-install-recommends \
        cmake ninja-build protobuf-compiler python3-protobuf python3-setuptools \
        git ca-certificates xxd && \
    rm -rf /var/lib/apt/lists/*

# Switch libraries. No SDL2 on purpose: the stream path talks to hid/audout directly.
RUN ln -sf /proc/mounts /etc/mtab && \
    dkp-pacman -Syu --noconfirm && \
    dkp-pacman -S --noconfirm --needed \
        devkitA64 libnx switch-tools switch-cmake deko3d uam \
        switch-ffmpeg switch-dav1d switch-mbedtls switch-libopus switch-opusfile \
        switch-glm switch-zlib && \
    rm -rf /opt/devkitpro/pacman/var/cache/pacman/pkg/*

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    DEVKITPPC=/opt/devkitpro/devkitPPC \
    PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH

WORKDIR /src
