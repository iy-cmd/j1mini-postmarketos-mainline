#!/bin/sh
# NetworkManager dispatcher hook for sharing Linux host internet with J1 Mini over USB (192.168.7.0/24)
# Install to: /etc/NetworkManager/dispatcher.d/99-j1mini-usb-forward (chmod +x)
#
# Also configure NetworkManager USB connection with shared IPv4:
#   nmcli con add type ethernet con-name j1mini-usb-nat \
#     ifname "*" mac 02:00:00:73:56:02 \
#     ipv4.method shared ipv4.addresses 192.168.7.1/24 ipv6.method link-local

ACTION="$2"
case "$ACTION" in
  up|dhcp4-change)
    # Allow forwarding when Docker sets FORWARD policy to DROP
    iptables -C DOCKER-USER -s 192.168.7.0/24 -j ACCEPT 2>/dev/null || \
      iptables -I DOCKER-USER -s 192.168.7.0/24 -j ACCEPT
    iptables -C DOCKER-USER -d 192.168.7.0/24 -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null || \
      iptables -I DOCKER-USER -d 192.168.7.0/24 -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT

    # Forward DNS requests sent to 192.168.7.1:53 to the host's active default gateway
    GW=$(ip -4 route show default | awk '{print $3; exit}')
    if [ -n "$GW" ]; then
      iptables -t nat -F J1MINI-DNS 2>/dev/null || iptables -t nat -N J1MINI-DNS
      iptables -t nat -C PREROUTING -s 192.168.7.0/24 -d 192.168.7.1 -p udp --dport 53 -j J1MINI-DNS 2>/dev/null || \
        iptables -t nat -I PREROUTING -s 192.168.7.0/24 -d 192.168.7.1 -p udp --dport 53 -j J1MINI-DNS
      iptables -t nat -C PREROUTING -s 192.168.7.0/24 -d 192.168.7.1 -p tcp --dport 53 -j J1MINI-DNS 2>/dev/null || \
        iptables -t nat -I PREROUTING -s 192.168.7.0/24 -d 192.168.7.1 -p tcp --dport 53 -j J1MINI-DNS
      iptables -t nat -A J1MINI-DNS -j DNAT --to-destination "$GW"
    fi
    ;;
esac
