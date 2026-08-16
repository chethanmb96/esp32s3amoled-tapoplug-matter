# Tapo P116M Matter AMOLED Display Controller

A high-performance **Matter (CHIP) Controller & Energy Monitor Dashboard** built for the **LILYGO T-Display-S3 AMOLED** (ESP32-S3) to display real-time telemetry from the **TP-Link Tapo P116M Smart Plug**.

[![Build & CI](https://github.com/chethanmb96/esp32s3amoled-tapoplug-matter/actions/workflows/ci.yml/badge.svg)](https://github.com/chethanmb96/esp32s3amoled-tapoplug-matter/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/chethanmb96/esp32s3amoled-tapoplug-matter?color=blue)](https://github.com/chethanmb96/esp32s3amoled-tapoplug-matter/releases)

---

## 📱 AMOLED Display Layout

```text
┌──────────────────────────────────────────────────────────────────┐
│  ● TAPO P116M                                            [ ON ]  │
│                                                                  │
│                                                                  │
│                            34 W                                  │
│                                                                  │
│                                                                  │
│   VOLTAGE                                              CURRENT   │
│    243V                                                 0.22A    │
└──────────────────────────────────────────────────────────────────┘
```

---

## 📦 Pre-Built Binaries (Quick Flash)

Pre-built binaries and **all-in-one merged flash images (`tapo-matter-display-merged.bin`)** are automatically built for every release via GitHub Actions.

You can flash a release in a single command without compiling:

```bash
# Flash the all-in-one merged binary at 0x0:
esptool.py --chip esp32s3 -p COM3 -b 460800 write_flash 0x0 tapo-matter-display-merged.bin
```

Download the latest binary from the **[Releases](https://github.com/chethanmb96/esp32s3amoled-tapoplug-matter/releases)** page.

---

## 🌟 Features

- **⚡ Real-Time Active Power Readout**: Massive, ultra-smooth anti-aliased digital display in **Watts (`W`)** or **Kilowatts (`kW`)** with mathematical baseline alignment.
- **🔌 Line Telemetry**: Live **Voltage (`V`)** and **Current (`A`)** readouts with zero decimal clutter.
- **💡 Live Relay Indicator**: Instantaneous **`ON`** (Emerald) / **`OFF`** (Crimson) status.
- **🚀 Native Matter Subscriptions**: Uses native Matter `subscribe_command` to receive millisecond-level telemetry pushes directly from the plug without polling lag.
- **🖤 Pure OLED Dark Design**: High-contrast `#000000` AMOLED aesthetics with subpixel anti-aliased vector typography.
- **🛡️ Commercial DAC Support**: Built-in `PermissiveDeviceAttestationVerifier` to seamlessly accept TP-Link production certificates (`Vendor ID: 0x131B`).

---

## 🖥️ Hardware Required

1. **[LILYGO T-Display-S3 AMOLED](https://www.lilygo.cc/products/t-display-s3-amoled)**:
   - Chip: ESP32-S3 Dual-Core Xtensa LX7 (240 MHz)
   - Display: 1.64" RM67162 QSPI AMOLED (536 &times; 240 RGB565)
   - Memory: 16 MB Flash, 8 MB Octal PSRAM
2. **TP-Link Tapo P116M Smart Plug** (or any Matter 1.3+ compatible energy-monitoring smart plug).
3. 2.4 GHz Wi-Fi Network.

---

## 📁 Repository Structure

```
.
├── main/
│   ├── CMakeLists.txt              # Component build rules
│   ├── idf_component.yml           # ESP-Matter component dependency
│   ├── Kconfig.projbuild           # Project configuration menu
│   ├── rm67162.h / rm67162.cpp     # QSPI AMOLED DMA driver (with internal SRAM chunking)
│   ├── display_ui.h / display_ui.cpp# High-contrast UI & smooth vector font engine
│   ├── font_8x16.h                 # Basic ASCII font bitmap
│   └── tapo-matter-display.cpp     # Matter commissioner, DAC verifier & subscription loop
├── CMakeLists.txt                  # Project-level CMakeLists
├── partitions.csv                  # 16 MB Flash partition scheme (Matter app + NVS storage)
├── sdkconfig.defaults              # Hardware defaults (16MB flash, Octal PSRAM, Controller mode)
└── README.md                       # Documentation
```

---

## 🚀 Getting Started

### 1. Prerequisites

Make sure you have **ESP-IDF v5.1+** installed and sourced:

```bash
# Clone and export ESP-IDF
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh
source ./export.sh
```

### 2. Clone the Repository

```bash
git clone https://github.com/chethanmb96/esp32s3amoled-tapoplug-matter.git
cd esp32s3amoled-tapoplug-matter
```

### 3. Configure Credentials

Run `menuconfig` to enter your Wi-Fi credentials and Tapo Matter Pairing Code:

```bash
idf.py menuconfig
```

Navigate to:
> **Tapo P116M Configuration**
- **Wi-Fi SSID**: Your 2.4 GHz Wi-Fi SSID
- **Wi-Fi Password**: Your Wi-Fi password
- **Matter Setup Code**: 11-digit pairing code from Tapo app (*e.g., `34476103845`*)
- **Matter Node ID**: `0x1234` (default)

> [!NOTE]
> All credentials configured in `menuconfig` are saved to your local `sdkconfig` file, which is excluded from Git via `.gitignore` to ensure secrets are never leaked.

### 4. Build the Firmware

```bash
idf.py build
```

*(Tip: For fast incremental builds during development, use `ninja -C build`)*

### 5. Flash and Monitor

Connect your T-Display-S3 AMOLED via USB-C:

```bash
idf.py -p /dev/ttyACM0 flash monitor
# or on Windows:
idf.py -p COM3 flash monitor
```

---

## ⚙️ How It Works

1. **Boot & QSPI AMOLED Init**:
   The RM67162 display initializes over QSPI in under 150ms and allocates a 536 &times; 240 16-bit framebuffer in external PSRAM.
2. **Wi-Fi & Matter Commissioner Handshake**:
   The ESP32-S3 connects to the local Wi-Fi, loads Fabric certificates from NVS storage, and spins up the Matter commissioner client on port `5580`.
3. **Attestation & Subscription**:
   - Commercial DAC certificates from TP-Link (`0x131B`) are validated.
   - The device subscribes to **Endpoint 1** attributes with `min_interval = 0` and `max_interval = 1`.
4. **Attribute Decoding**:
   - **Active Power (`W`)**: Matter 1.3+ `ElectricalPowerMeasurement` (Cluster `0x0090:0x0008` / `0x000D` in mW). Fallback: Legacy Cluster `0x0B04:0x050B`.
   - **RMS Voltage (`V`)**: Cluster `0x0090:0x000B` in mV. Fallback: Cluster `0x0B04:0x0505`.
   - **RMS Current (`A`)**: Cluster `0x0090:0x000C` in mA. Fallback: Cluster `0x0B04:0x0508`.
   - **Relay State**: Cluster `0x0006:0x0000` (`OnOff`).
5. **Smooth Vector Rendering**:
   Frame updates are rasterized through the subpixel anti-aliased typography engine with FreeRTOS mutex synchronization to eliminate SPI DMA bus collisions.

---

## 🔒 Security & Privacy

- **Zero Hardcoded Secrets**: All network credentials, setup codes, and tokens are strictly configured via Kconfig and excluded from version control.
- **Local Operation**: All Matter communications happen strictly over the local network via UDP/IPv6. No external cloud connections are required.

---

## 📄 License

MIT License. Feel free to use, modify, and distribute for personal and commercial projects.
