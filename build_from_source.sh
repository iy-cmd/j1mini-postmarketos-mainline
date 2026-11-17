#!/bin/bash
# build_from_source.sh - 1-Click kernel build for Samsung Galaxy J1 Mini (Mainline Linux 7.3)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$ROOT/build"
SOURCE_DIR="$ROOT/source"
OUT_DIR="$ROOT/out_build"

mkdir -p "$OUT_DIR"

echo "=========================================================="
echo " Samsung Galaxy J1 Mini - Mainline Linux 7.3 Kernel Build "
echo "=========================================================="

# 1. Detect Container Engine (Podman or Docker)
CONTAINER_BIN=""
if command -v podman >/dev/null 2>&1; then
    CONTAINER_BIN="podman"
elif command -v docker >/dev/null 2>&1; then
    CONTAINER_BIN="docker"
else
    echo "[-] Error: Neither 'podman' nor 'docker' was found on your system."
    echo "[!] Please install podman or docker to build the kernel in the isolated environment."
    exit 1
fi
echo "[+] Using container runtime: $CONTAINER_BIN"

# 2. Build or verify the builder image
IMAGE_NAME="j1mini-build:latest"
echo "[+] Checking/building builder container image ($IMAGE_NAME)..."
$CONTAINER_BIN build -t "$IMAGE_NAME" -f "$BUILD_DIR/Dockerfile" "$BUILD_DIR"

# 3. Setup SSH authorized_keys if needed
SSH_KEY_FILE="$BUILD_DIR/buildkit/ssh/authorized_keys"
mkdir -p "$BUILD_DIR/buildkit/ssh"
if [ ! -s "$SSH_KEY_FILE" ]; then
    if [ -f "$HOME/.ssh/id_ed25519.pub" ]; then
        cp "$HOME/.ssh/id_ed25519.pub" "$SSH_KEY_FILE"
        echo "[+] Copied $HOME/.ssh/id_ed25519.pub to initramfs authorized_keys."
    elif [ -f "$HOME/.ssh/id_rsa.pub" ]; then
        cp "$HOME/.ssh/id_rsa.pub" "$SSH_KEY_FILE"
        echo "[+] Copied $HOME/.ssh/id_rsa.pub to initramfs authorized_keys."
    else
        echo "[+] Generating a temporary SSH key for rescue login..."
        ssh-keygen -t ed25519 -N "" -f "$BUILD_DIR/buildkit/ssh/j1mini_rescue_key" -q
        cat "$BUILD_DIR/buildkit/ssh/j1mini_rescue_key.pub" > "$SSH_KEY_FILE"
        echo "[+] Rescue private key saved to: $BUILD_DIR/buildkit/ssh/j1mini_rescue_key"
    fi
fi

# 4. Prepare Linux source tree inside a volume
VOLUME_NAME="j1mini-linux-src"
$CONTAINER_BIN volume inspect "$VOLUME_NAME" >/dev/null 2>&1 || $CONTAINER_BIN volume create "$VOLUME_NAME" >/dev/null

echo "[+] Preparing Linux v7.3.1 source tree in volume ($VOLUME_NAME)..."
$CONTAINER_BIN run --rm -v "$ROOT:/work:z" -v "$VOLUME_NAME:/src" "$IMAGE_NAME" sh -c "
    set -e
    if [ ! -d /src/linux/.git ]; then
        echo 'Cloning Linux v7.3.1 (this may take a few minutes)...'
        git clone --depth=1 --branch v7.3.1 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git /src/linux
    fi
    cd /src/linux
    if ! git log -n 200 --oneline | grep -q 'dwc2: fix descriptor DMA buffer overrun'; then
        echo 'Pulling changes from j1mini git bundle...'
        git pull /work/source/j1mini-mainline-git.bundle HEAD || {
            echo 'Applying patches from /work/source/patches...'
            git am /work/source/patches/*.patch
        }
    else
        echo 'Kernel source is already patched.'
    fi
    cp /work/source/sc7730-samsung-j1mini.dts /src/linux/arch/arm/boot/dts/unisoc/sc7730-samsung-j1mini.dts
"

# 5. Compile Kernel and Generate boot.img
echo "[+] Compiling kernel, modules, bootsplash, and generating boot.img..."
$CONTAINER_BIN run --rm -v "$ROOT:/work:z" -v "$VOLUME_NAME:/src" -w /work/build/buildkit "$IMAGE_NAME" sh -c "
    set -e
    export DTB=sc7730-samsung-j1mini.dtb
    export OUT=j1mini-mainline.img
    export EXTRA_CONFIG=/work/build/buildkit/j1mini.config
    export MKIMAGE=/work/build/tools/mkimage_j1mini.py
    export OVERLAY=/work/overlay
    /work/build/buildkit/build.sh /src/linux
"

# 6. Copy outputs
echo "[+] Packaging final output files..."
cp "$BUILD_DIR/buildkit/j1mini-mainline.img" "$OUT_DIR/boot.img"

# Odin tar.md5
cd "$OUT_DIR"
tar -H ustar -c boot.img > boot.tar
md5sum -t boot.tar >> boot.tar
mv boot.tar boot.tar.md5

# TWRP zip
"$BUILD_DIR/tools/make_twrp_zip.sh" "$OUT_DIR/boot.img" "$OUT_DIR/j1mini-mainline-twrp.zip"

echo "=========================================================="
echo " BUILD SUCCESSFUL!"
echo " Generated output files in $OUT_DIR:"
echo "   - $OUT_DIR/boot.img"
echo "   - $OUT_DIR/boot.tar.md5 (for Odin)"
echo "   - $OUT_DIR/j1mini-mainline-twrp.zip (for TWRP)"
echo "=========================================================="
