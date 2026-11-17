#!/bin/bash
# flash_pmos_adb.sh - 1-Click ADB Installer for postmarketOS on Samsung Galaxy J1 Mini
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BOOT_IMG="$SCRIPT_DIR/../binaries/boot.img"
ROOTFS_DIR="${ROOTFS_DIR:-$HOME/.local/var/pmbootstrap/chroot_rootfs_samsung-j1mini3g}"

if [ ! -f "$BOOT_IMG" ]; then
    echo "[-] Error: boot.img not found at $BOOT_IMG" >&2
    exit 1
fi

if [ ! -d "$ROOTFS_DIR" ]; then
    echo "[-] Error: postmarketOS rootfs directory not found at $ROOTFS_DIR" >&2
    exit 1
fi

echo "=========================================================="
echo " postmarketOS Installer for Samsung Galaxy J1 Mini 3G     "
echo " (SM-J105H / SM-J105F - Mainline Linux 7.3 + SXMO/i3)     "
echo "=========================================================="

echo "[+] Looking for Samsung Galaxy J1 Mini in TWRP Recovery..."
DEV=$(adb devices | grep -w "recovery" | awk '{print $1}' | head -n 1 || true)
if [ -z "$DEV" ]; then
    echo "[-] Phone in TWRP recovery mode not detected."
    echo "[!] Please boot the phone into TWRP (VolUp + Home + Power) and connect USB cable."
    adb wait-for-recovery
    DEV=$(adb devices | grep -w "recovery" | awk '{print $1}' | head -n 1)
fi

echo "[+] Connected device: $DEV (recovery mode)"

# 1. Flash Mainline Linux 7.3 Boot Image
echo "[+] Step 1/3: Flashing Mainline Linux 7.3 kernel to /dev/block/mmcblk0p20 (BOOT)..."
adb push "$BOOT_IMG" /dev/block/mmcblk0p20

# 2. Format and mount userdata partition
echo "[+] Step 2/3: Formatting userdata partition (/dev/block/mmcblk0p26) as ext4..."
adb shell "mke2fs -F -t ext4 /dev/block/mmcblk0p26"
adb shell "mkdir -p /data && mount -t ext4 /dev/block/mmcblk0p26 /data"

# 3. Stream postmarketOS rootfs over ADB
echo "[+] Step 3/3: Installing postmarketOS rootfs directly to phone..."
tar -C "$ROOTFS_DIR" \
    --exclude="./proc/*" \
    --exclude="./sys/*" \
    --exclude="./dev/*" \
    --exclude="./run/*" \
    --exclude="./tmp/*" \
    -cf - . | adb shell "tar -xpf - -C /data"

adb shell "mkdir -p /data/proc /data/sys /data/dev /data/run /data/tmp"
adb shell "chmod 1777 /data/tmp"
adb shell "sync && umount /data 2>/dev/null || true"

echo "=========================================================="
echo " INSTALLATION COMPLETE!                                   "
echo "=========================================================="
echo " - Kernel: Mainline Linux 7.3.1 (armv7l SMP)"
echo " - Rootfs: postmarketOS edge (Alpine Linux + SXMO i3)"
echo " - Login user: shatalovie"
echo " - Password:   1234"
echo " - Network:    USB CDC-NCM (192.168.7.2 phone, 192.168.7.1 PC)"
echo "=========================================================="
echo "[+] Rebooting into postmarketOS..."
adb reboot
echo "[+] Done! Watch phone screen for bootsplash and postmarketOS."
