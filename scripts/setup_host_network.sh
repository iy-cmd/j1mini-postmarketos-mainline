#!/bin/bash
# setup_host_network.sh - Host-side network & NAT routing for Samsung Galaxy J1 Mini
# Automatically detects CDC-NCM USB network interface, assigns 192.168.7.1,
# enables IPv4 forwarding and iptables/nftables MASQUERADE for full internet access.

set -euo pipefail

if [ "$EUID" -ne 0 ]; then
    echo "[-] This script requires root permissions. Re-running with sudo..."
    exec sudo "$0" "$@"
fi

# 1. Enable IP forwarding
echo "[+] Enabling IPv4 forwarding..."
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# 2. Detect upstream default route interface (Wi-Fi or Ethernet)
WAN_IF=$(ip -4 route show default | awk '{print $5}' | head -n 1 || true)
if [ -z "$WAN_IF" ]; then
    echo "[!] Warning: No default WAN interface detected. Internet forwarding might not route outside."
else
    echo "[+] Detected default WAN interface: $WAN_IF"
    # Ensure NAT masquerade rule exists
    if ! iptables -t nat -C POSTROUTING -o "$WAN_IF" -s 192.168.7.0/24 -j MASQUERADE 2>/dev/null; then
        echo "[+] Adding iptables NAT MASQUERADE rule for 192.168.7.0/24 -> $WAN_IF..."
        iptables -t nat -A POSTROUTING -o "$WAN_IF" -s 192.168.7.0/24 -j MASQUERADE
    else
        echo "[*] iptables NAT MASQUERADE rule already present."
    fi
fi

# 3. Watcher loop for CDC-NCM USB interface
echo "[+] Waiting for Galaxy J1 Mini USB network interface (CDC-NCM: enx* / usb* / enp*)..."
echo "[+] Connect the phone to host via USB. Press Ctrl+C to exit."

while true; do
    for dev in $(ls /sys/class/net 2>/dev/null | grep -E '^enx020000735602|^enp.*u.*i.*|^usb'); do
        # Check if MAC matches or dev name matches
        mac=$(cat "/sys/class/net/$dev/address" 2>/dev/null || true)
        if [ "$mac" = "02:00:00:73:56:02" ] || ip link show "$dev" 2>/dev/null | grep -q "02:00:00:73:56:02"; then
            if ! ip addr show "$dev" | grep -q "192.168.7.1"; then
                echo "[$(date '+%H:%M:%S')] Found J1 Mini USB interface: $dev ($mac)"
                ip link set "$dev" up
                ip addr add 192.168.7.1/24 dev "$dev" 2>/dev/null || true
                echo "[$(date '+%H:%M:%S')] Configured 192.168.7.1/24 on $dev. Ping test: ping -c 1 192.168.7.2"
            fi
        fi
    done
    sleep 1
done
