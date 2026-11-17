#!/usr/bin/env bash
# flash_ssh.sh - Flash a newly built boot.img directly to /dev/mmcblk0p20 over SSH
# (Requires root SSH access to postmarketOS at 192.168.7.2; leaves TWRP on p21 untouched)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOOT_IMG="${1:-$SCRIPT_DIR/../out_build/boot.img}"
TARGET="${2:-root@192.168.7.2}"

if [ ! -f "$BOOT_IMG" ]; then
    echo "[-] boot.img not found at: $BOOT_IMG" >&2
    echo "    Usage: $0 [path/to/boot.img] [user@host]" >&2
    exit 1
fi

SHA_LOCAL="$(sha256sum "$BOOT_IMG" | awk '{print $1}')"
SIZE_LOCAL="$(stat -c%s "$BOOT_IMG")"

echo "[+] Local boot.img: $BOOT_IMG ($SIZE_LOCAL bytes)"
echo "[+] SHA-256:        $SHA_LOCAL"
echo "[+] Streaming image to $TARGET:/tmp/boot.img..."
ssh -o ConnectTimeout=5 "$TARGET" 'cat > /tmp/boot.img' < "$BOOT_IMG"

echo "[+] Flashing /dev/mmcblk0p20 (KERNEL) and verifying readback..."
ssh -o ConnectTimeout=5 "$TARGET" "
set -euo pipefail
SHA_TMP=\$(sha256sum /tmp/boot.img | awk '{print \$1}')
[ \"\$SHA_TMP\" = \"$SHA_LOCAL\" ] || { echo '[-] Transfer SHA-256 mismatch!'; exit 1; }
blockdev --setrw /dev/mmcblk0p20
dd if=/tmp/boot.img of=/dev/mmcblk0p20 bs=1M conv=fsync
sync
SHA_DEV=\$(head -c $SIZE_LOCAL /dev/mmcblk0p20 | sha256sum | awk '{print \$1}')
[ \"\$SHA_DEV\" = \"$SHA_LOCAL\" ] || { echo '[-] Flash readback SHA-256 mismatch!'; exit 1; }
rm -f /tmp/boot.img
sync
echo '[+] Flash verified OK! Rebooting device...'
nohup sh -c 'sleep 1 && reboot' >/dev/null 2>&1 </dev/null &
"
