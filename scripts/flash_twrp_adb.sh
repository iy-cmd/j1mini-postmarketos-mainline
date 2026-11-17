#!/bin/bash
# flash_twrp_adb.sh - Flash Mainline boot.img to Samsung Galaxy J1 Mini via ADB (TWRP)
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BOOT_IMG="$SCRIPT_DIR/../binaries/boot.img"

if [ ! -f "$BOOT_IMG" ]; then
    echo "[-] Error: boot.img not found at $BOOT_IMG"
    exit 1
fi

echo "========================================================"
echo " Flashing Mainline Linux to Samsung Galaxy J1 Mini "
echo "========================================================"
echo "[+] Checking for ADB recovery device..."

DEV=$(adb devices | grep -w "recovery" | awk '{print $1}' | head -n 1 || true)
if [ -z "$DEV" ]; then
    echo "[-] Device in TWRP recovery not detected."
    echo "[!] Please boot Galaxy J1 Mini into TWRP (VolUp + Home + Power) and plug in USB."
    adb wait-for-recovery
    DEV=$(adb devices | grep -w "recovery" | awk '{print $1}' | head -n 1)
fi

echo "[+] Found device: $DEV (recovery)"
echo "[+] Flashing $BOOT_IMG to /dev/block/mmcblk0p20 (BOOT partition)..."
adb push "$BOOT_IMG" /dev/block/mmcblk0p20

echo "[+] Flash successful! Rebooting into Mainline Linux..."
adb reboot

echo "[+] Done. System is booting into Mainline Linux 7.3."
