#!/bin/bash
# flash_pmos_sdcard.sh - Install postmarketOS to a microSD card for Samsung Galaxy J1 Mini
set -euo pipefail

DEV=${1:-""}
ROOTFS_DIR="${ROOTFS_DIR:-$HOME/.local/var/pmbootstrap/chroot_rootfs_samsung-j1mini3g}"

if [ -z "$DEV" ] || [ ! -b "$DEV" ]; then
    echo "Usage: sudo $0 /dev/sdX (or /dev/mmcblkX)"
    echo "Example: sudo $0 /dev/sdb"
    exit 1
fi

if [ ! -d "$ROOTFS_DIR" ]; then
    echo "[-] Error: Rootfs not found at $ROOTFS_DIR" >&2
    exit 1
fi

echo "=========================================================="
echo " Flashing postmarketOS to MicroSD ($DEV)                   "
echo " WARNING: ALL DATA ON $DEV WILL BE WIPED!                "
echo "=========================================================="
read -p "Are you sure you want to proceed? (y/N): " confirm
if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
    echo "Aborted."
    exit 0
fi

echo "[+] Creating ext4 partition with label 'pmOS_root' on $DEV..."
# Create single primary partition spanning entire card
wipefs -a "$DEV"
parted -s "$DEV" mklabel msdos
parted -s "$DEV" mkpart primary ext4 4MiB 100%

PART="${DEV}1"
[ -b "${DEV}p1" ] && PART="${DEV}p1"

echo "[+] Formatting $PART as ext4..."
mkfs.ext4 -F -L "pmOS_root" "$PART"

MOUNT_DIR=$(mktemp -d /tmp/pmos_sd.XXXXXX)
trap 'umount "$MOUNT_DIR" 2>/dev/null || true; rm -rf "$MOUNT_DIR"' EXIT

mount "$PART" "$MOUNT_DIR"
echo "[+] Copying postmarketOS files to SD card..."
tar -C "$ROOTFS_DIR" \
    --exclude="./proc/*" \
    --exclude="./sys/*" \
    --exclude="./dev/*" \
    --exclude="./run/*" \
    --exclude="./tmp/*" \
    -cf - . | tar -xpf - -C "$MOUNT_DIR"

mkdir -p "$MOUNT_DIR/proc" "$MOUNT_DIR/sys" "$MOUNT_DIR/dev" "$MOUNT_DIR/run" "$MOUNT_DIR/tmp"
chmod 1777 "$MOUNT_DIR/tmp"
sync

echo "[+] SD card installation complete! Insert SD card into J1 Mini."
