#!/bin/bash
# apply_patches_to_linux.sh - Helper to apply J1 Mini Mainline patches to Linux v7.3.1
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUNDLE="$SCRIPT_DIR/../source/j1mini-mainline-git.bundle"
CUMULATIVE_PATCH="$SCRIPT_DIR/../source/linux-7.3.1-j1mini-cumulative.patch"
PATCH_DIR="$SCRIPT_DIR/../source/patches"

TARGET_DIR="${1:-}"

if [ -z "$TARGET_DIR" ]; then
    echo "Usage: $0 /path/to/linux-source-tree"
    echo ""
    echo "Example:"
    echo "  git clone --depth=1 --branch v7.3.1 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux-7.3.1"
    echo "  $0 linux-7.3.1"
    exit 1
fi

if [ ! -d "$TARGET_DIR" ]; then
    echo "[-] Directory $TARGET_DIR does not exist."
    exit 1
fi

cd "$TARGET_DIR"

echo "========================================================"
echo " Applying J1 Mini Mainline Patches to $TARGET_DIR"
echo "========================================================"

if [ -f "$BUNDLE" ]; then
    echo "[+] Option A: Pulling full commit history from git bundle..."
    git pull "$BUNDLE" HEAD || {
        echo "[!] Git pull from bundle failed, falling back to patch series..."
        git am "$PATCH_DIR"/*.patch || patch -p1 < "$CUMULATIVE_PATCH"
    }
else
    echo "[+] Option B: Applying cumulative patch..."
    patch -p1 < "$CUMULATIVE_PATCH"
fi

echo "[+] Successfully applied J1 Mini Mainline patches!"
echo "[+] Copy defconfig: cp $SCRIPT_DIR/../source/j1mini_defconfig arch/arm/configs/"
