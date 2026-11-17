# Mainline Linux 7.3 & postmarketOS for Samsung Galaxy J1 Mini 3G (`SM-J105H` / `SM-J105F`)

[Русская версия (README_RU.md)](README_RU.md)

A complete, daily-usable port of **Mainline Linux 7.3.1** and **postmarketOS (Alpine Linux + SXMO i3)** for the **Samsung Galaxy J1 Mini 3G** (`j1mini3g`, `SM-J105H`, `SM-J105F`, Spreadtrum/Unisoc SC7730 / SC8830 / SC7727S).

Features hardware-accelerated Mali-400 graphics (Lima) with tear-free VSync compositing, working internal speaker & headphone audio over PipeWire, MELFAS MIP4 touchscreen, 480×800-tuned SXMO i3 mobile UI, GNOME Web, and plug-and-play USB networking & internet sharing.

---

## Hardware Support Matrix

| Subsystem | Status | Notes |
|---|---|---|
| **CPU (Quad Cortex-A7)** | Working | All 4 cores active in SMP (`7.3.1-j1mini+`) |
| **Display (`S6D77A1` 480×800)** | Working | Hardware `sprd-dispc` takeover driver @ 58 Hz, race-free atomic VSYNC flips |
| **GPU (`Mali-400 MP1`)** | Working | Mainline `lima` (OpenGL 2.1 / GLES 2.0, Mesa 26.2) + Xorg Glamor + `picom` VSync |
| **Touchscreen (`MELFAS MIP4`)** | Working | Multitouch over I2C (`0x48`), `libinput`, SXMO edge gestures |
| **Audio (Speaker, Headphones, Mic, Jack)** | Working | VBC r2p0 + AP DMA + SC2723 PMIC codec, ALSA UCM2 + **PipeWire / WirePlumber** |
| **Hardware Buttons** | Working | Power, Home, Volume Up/Down (`Vol Down` toggles on-screen keyboard `svkbd`) |
| **Storage (8 GB eMMC + microSD)** | Working | Full 2.1 GiB `SYSTEM` rootfs (`mmcblk0p25`) + 384 MiB swapfile |
| **USB Gadget (Network + Serial)** | Working | `CDC-NCM` Ethernet (`192.168.7.2`) + `CDC-ACM` serial console (`/dev/ttyACM0`), DWC2 PIO mode |
| **USB Internet Sharing** | Working | Auto-configured NAT + DNS + `TTL=66` (works even behind carrier hotspot tether checks) |
| **PMIC / Battery / Fuel Gauge** | Working | SC2723 PMIC regulators, EIC, ADC, FGU |
| **Wi-Fi (`Spreadtrum SC2331 / Marlin 1`)** | In Progress | SDIO `0000:2331` — driver & firmware bring-up in progress (use USB internet in the meantime) |
| **Bluetooth (`Spreadtrum SC2331`)** | In Progress | WCN combo chip bring-up in progress alongside Wi-Fi |
| **Cellular Modem** | Not supported | Proprietary Spreadtrum CP firmware stack |

---

## Hardware Specifications

- **Device**: Samsung Galaxy J1 Mini 3G (`j1mini3g`, `SM-J105H` / `SM-J105F`, `hw_revision=2`)
- **SoC**: Spreadtrum / Unisoc SC8830 / SC7727S (`SC7730` family, 28 nm HPm)
- **CPU**: Quad-core ARM Cortex-A7 @ 1.2 GHz
- **GPU**: ARM Mali-400 MP1 @ 384 MHz (32 KiB L2 cache)
- **RAM**: 768 MiB LPDDR2 (715 MiB usable + 384 MiB swapfile)
- **PMIC / Audio Codec**: Spreadtrum SC2723 (ADI bus)
- **Display**: 4.0" 480×800 TFT LCD (`S6D77A1` MIPI DSI video mode, 96 DPI tuned)
- **Storage**: 8 GB eMMC 5.0 (`mmcblk0p20` = `KERNEL`, `mmcblk0p21` = `RECOVERY`, `mmcblk0p25` = `SYSTEM`)

---

## Pre-built Release Images (GitHub Releases)

Download ready-to-flash packages from the [**Releases**](../../releases) page:

| Release Asset | Purpose |
|---|---|
| `postmarketos-j1mini-odin.tar.md5` | **1-Click Full Odin Package** (`boot.img` + sparse `system.img`) — flash in **AP** slot |
| `postmarketos-j1mini-twrp.zip` | **1-Click Full TWRP Zip** (Mainline 7.3.1 kernel + complete postmarketOS rootfs) |
| `boot.img` | Kernel-only raw boot image (`KERNEL` partition `/dev/mmcblk0p20`) |
| `boot.tar.md5` | Kernel-only Odin package (**AP** slot) |
| `j1mini-mainline-twrp.zip` | Kernel-only TWRP flashable zip |
| `SHA256SUMS.txt` | SHA-256 checksums for all release files |

---

## Installation

> **Warning**: Flashing replaces Android on the `KERNEL` (`mmcblk0p20`) and `SYSTEM` (`mmcblk0p25`) partitions. Back up any important data first! TWRP on `RECOVERY` (`mmcblk0p21`) is not overwritten.

### Method 1: Samsung Odin / Heimdall / Thor (Recommended)

1. Power off the phone.
2. Enter **Download Mode**: hold `Vol Down + Home + Power`, then press `Vol Up` to confirm.
3. Connect the phone to your PC via USB.
4. Open **Odin3** (Windows) or **Thor / Heimdall** (Linux).
5. Select `postmarketos-j1mini-odin.tar.md5` in the **AP** slot and click **Start**.
6. The phone flashes both the mainline kernel and postmarketOS rootfs, then reboots automatically into SXMO i3.

### Method 2: TWRP Recovery (1-Click Zip)

1. Copy `postmarketos-j1mini-twrp.zip` to an SD card or push it via `adb push postmarketos-j1mini-twrp.zip /tmp/`.
2. Boot into **TWRP** (`Vol Up + Home + Power`).
3. Tap **Install** → select `postmarketos-j1mini-twrp.zip` → swipe to flash.
4. Tap **Reboot System**.

### Method 3: Updating Only the Kernel over SSH (No TWRP/Odin Needed)

Once postmarketOS is running and connected over USB (`192.168.7.2`), you can flash a new `boot.img` directly with automatic SHA-256 readback verification:

```bash
./scripts/flash_ssh.sh out_build/boot.img root@192.168.7.2
```

*(Uses `blockdev --setrw /dev/mmcblk0p20` because Samsung's GPT flags the `KERNEL` partition read-only.)*

---

## Default Login & Controls

- **User**: `samsungj1mini`
- **Password**: `1234` (change immediately with `passwd` after first login!)
- **Desktop**: SXMO (Simple X Mobile) on `i3` + `picom` VSync compositor
- **Browser**: GNOME Web (`epiphany 50.6`, tuned for 480×800 & Mali-400)
- **Tools included**: `st`, `tmux`, `fish`, `htop`, `btop`, `mpv`, `ffmpeg`, `mesa-demos`
- **On-Screen Keyboard**: Press **Volume Down** to toggle `svkbd` (`480×280` anchored at the bottom) open and closed.
- **SXMO Gestures**: Swipe from screen edges (e.g. left/right/bottom edges) to open menus, switch windows, and close apps.

---

## USB Networking, SSH & Internet Sharing

When connected to a Linux host via USB, the phone exposes two USB gadget functions:

1. **USB Ethernet (`CDC-NCM`)** — phone IP `192.168.7.2/24`
2. **USB Serial Console (`CDC-ACM`)** — `/dev/ttyACM0` (`115200 8N1`)

### Quick SSH Connection

```bash
sudo ./scripts/setup_host_network.sh
ssh samsungj1mini@192.168.7.2
```

### Automatic USB Internet Sharing (NetworkManager on Linux Host)

1. Create a persistent shared USB NetworkManager profile on your Linux PC:
   ```bash
   sudo nmcli con add type ethernet con-name j1mini-usb-nat \
     ifname "*" mac 02:00:00:73:56:02 \
     ipv4.method shared ipv4.addresses 192.168.7.1/24 \
     ipv6.method link-local
   ```
2. Install the included NetworkManager dispatcher hook (handles Docker's `FORWARD DROP` policy and DNS forwarding to the host's upstream gateway):
   ```bash
   sudo install -m 755 scripts/host-usb-internet-dispatcher.sh \
     /etc/NetworkManager/dispatcher.d/99-j1mini-usb-forward
   ```
3. On the phone, `/etc/local.d/usb-internet.start` automatically sets `default via 192.168.7.1`, DNS `192.168.7.1`, and `net.ipv4.ip_default_ttl=66` so tethered traffic works reliably even through mobile hotspots.

---

## Building the Kernel from Source

Requirements: **Podman** (recommended) or **Docker** on Linux.

```bash
./build_from_source.sh
```

What `build_from_source.sh` does:
1. Builds the ARM cross-compilation container image (`debian:trixie-slim` + `gcc-arm-linux-gnueabihf`).
2. Fetches Linux `v7.3.1` into a persistent container volume (`j1mini-linux-src`) and applies `source/linux-7.3.1-j1mini-cumulative.patch`.
3. Copies all J1 Mini board/driver overrides from `overlay/` and `source/sc7730-samsung-j1mini.dts` + `source/j1mini_defconfig`.
4. Compiles `zImage`, DTB, initramfs, and generates ready-to-flash images in `out_build/`:
   - `out_build/boot.img`
   - `out_build/boot.tar.md5`
   - `out_build/j1mini-mainline-twrp.zip`

---

## Repository Layout

```text
.
├── build_from_source.sh          # Containerized mainline Linux 7.3.1 kernel builder
├── source/
│   ├── j1mini_defconfig          # Kernel configuration fragment for SM-J105H
│   ├── sc7730-samsung-j1mini.dts # Device Tree for Samsung Galaxy J1 Mini 3G
│   ├── linux-7.3.1-j1mini-cumulative.patch
│   └── patches/                  # Individual SC7730/SC8830 bring-up patch series
├── overlay/                      # Kernel driver fixes applied on top of Linux 7.3.1
│   ├── arch/arm/boot/dts/unisoc/sc7730-samsung-j1mini.dts
│   ├── arch/arm/mach-sprd/       # USB bring-up & PM domains
│   ├── drivers/gpu/drm/sprd/sprd_dispc.c   # Coherent DMA + race-free VSYNC flip latch
│   ├── drivers/input/touchscreen/melfas_mip4.c
│   ├── drivers/i2c/busses/i2c-sprd-sc8830.c
│   ├── drivers/gpio/gpio-sprd.c
│   └── drivers/usb/dwc2/
├── rootfs-overlay/               # All postmarketOS userspace configs (PipeWire, UCM2, picom, SXMO, GTK)
├── scripts/
│   ├── flash_ssh.sh              # Direct SSH kernel flasher with SHA-256 verification
│   ├── host-usb-internet-dispatcher.sh
│   ├── setup_host_network.sh
│   ├── flash_pmos_adb.sh
│   ├── flash_pmos_sdcard.sh
│   └── flash_twrp_adb.sh
├── README.md
└── README_RU.md
```

---

## Key Technical Fixes Included

1. **Zero-Artifact Hardware Display & GPU (`sprd-dispc` + `lima` + `picom`)**:
   - Enabled hardware display controller `&dispc` (`sprd-dispc`, 480×800@58 Hz) with `cached_buffers = false` (coherent DMA mapping so CPU cache flushes never overwrite Mali-400 scanout buffers).
   - Fixed a page-flip race condition in `sprd_dispc.c`: shadow layer registers are written before arming `d->event`, clearing `INT_UPDATE_DONE`, issuing `wmb()`, and latching `DPI_REG_UPDATE` atomically under `event_lock`, plus a VSYNC fallback completion to prevent 100 ms flip timeouts.
   - Added a minimal VSync-only `picom` compositor (`backend = "xrender"` accelerated via Glamor/Mali-400, `vsync = true`, `unredir-if-possible = false`) eliminating single-frontbuffer tearing and mid-tile artifacts.
   - Removed per-touch `pr_info` debug spam in `melfas_mip4.c`.
2. **Clean Audio Pipeline (PipeWire + VBC r2p0 + SC2723)**:
   - Enabled `&ap_dma`, `&vbc`, `&audio_pcm`, and `&sc2723_codec` with `simple-audio-card` (`j1mini`) and jack detection (`pmic_eic 8`).
   - Replaced PulseAudio (whose `mmap+timer` scheduling caused severe underruns and loud screeching on the 160-frame VBC ping-pong FIFO) with **PipeWire + WirePlumber + `pipewire-pulse`**.
   - Tuned PipeWire quantum and ALSA period size to `1600` frames @ `44100 Hz` (exact integer multiple of VBC's 160-frame DMA burst) with custom ALSA UCM2 profiles (`Samsung/j1mini`).
3. **480×800 Mobile Ergonomics & Lightweight Browser**:
   - Configured exact `96×96 DPI` (`DisplaySize 127 212`), compact `Xresources` / `i3` / `conky` / GTK3 / GTK4 fonts, and fixed `svkbd` geometry (`-g 480x280+0+520`) bound to Volume Down.
   - Replaced heavy Firefox with **GNOME Web (`epiphany 50.6`)** configured for Mali-400 OpenGL 2.1 (`GDK_DISABLE=gl,vulkan`, `GSK_RENDERER=cairo`, `WEBKIT_DISABLE_DMABUF_RENDERER=1`, `WEBKIT_DISABLE_COMPOSITING_MODE=1`, `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`), keeping total system RAM usage around ~170 MiB with browser open.

---

## License

Kernel patches and driver overlays are licensed under **GPL-2.0** (matching the Linux kernel). Scripts and configuration files are provided under the MIT license.
