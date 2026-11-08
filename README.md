# cwmera

<p align="center">
  <img src="./images/ex.jpg" alt="cwmera showcase" width="100%">
</p>

<p align="center">
  <a href="https://github.com/gwetano/cwmera/releases">
    <img src="https://img.shields.io/badge/version-v1.0.0-black?style=for-the-badge" alt="Version">
  </a>
  <a href="https://www.espressif.com/en/products/socs/esp32-s3">
    <img src="https://img.shields.io/badge/Hardware-ESP32--S3-black?style=for-the-badge&logo=espressif" alt="ESP32-S3">
  </a>
  <img src="https://img.shields.io/badge/Sensor-OV7670%20FIFO-black?style=for-the-badge" alt="OV7670 FIFO">
  <img src="https://img.shields.io/badge/Display-ST7789%20IPS-black?style=for-the-badge" alt="ST7789 IPS">
  <a href="./LICENSE">
    <img src="https://img.shields.io/badge/License-MIT-black?style=for-the-badge" alt="License: MIT">
  </a>
</p>

<p align="center">
  <strong>A minimal, retro-inspired standalone digital camera built with ESP32-S3, OV7670 FIFO, and ST7789 IPS LCD.</strong><br>
  Instant-on shooting, high-speed direct FIFO capture, captive portal gallery for iOS/Android, and wireless GitHub Releases OTA updates.
</p>

---

## ✨ Features

* **Instant-On Shooting**: Zero boot delay. Starts up in under a second with no Wi-Fi blocking.
* **Hardware FIFO Capture**: Direct fast GPIO register reads from the AL422B FIFO chip, ensuring clean, frame-accurate QVGA (320x240) captures without tearing.
* **Fluid IPS Live Viewfinder**: Real-time preview running on an ST7789 IPS display over hardware SPI at 40 MHz.
* **JPEG Storage on LittleFS**: Photos are converted to compressed JPEG in memory and written to flash storage (`/DCIM/FOTO_xxxx.jpg`).
* **Wireless Web Gallery (Captive Portal)**: Hold the shutter button to launch `cwmera wifi` mode. Connect with any smartphone (iPhone / Android) to browse, download to camera roll, or delete photos via a responsive brutalist web app.
* **Over-The-Air (OTA) Updates via GitHub**: Check and download the latest compiled firmware directly from GitHub Releases over your home Wi-Fi with one click from the web UI.
* **Chiptune Audio Feedback**: Built-in 8-bit sound effects (shutter click, Super Mario 1-UP boot jingle, WiFi chime, error alert) with permanent mute/unmute toggle.
* **3D Printable Pocket Body**: Custom enclosure designed for a handheld point-and-shoot retro feel.

---

## 🧰 Hardware Components

| Component | Specification / Notes |
| :--- | :--- |
| **Microcontroller** | **ESP32-S3** (Dual-Core Xtensa LX7 @ 240 MHz, 8MB PSRAM, 8MB/16MB Flash) |
| **Camera Sensor** | **OmniVision OV7670 with FIFO (AL422B)**<br>• Output: QVGA (320x240) RGB565 / Grayscale<br>• SCCB / I2C Address: `0x21`<br>• Dedicated 384 KB buffer memory to prevent frame drops |
| **Display** | **ST7789 IPS LCD** (240x320 pixels, Full Color, Hardware SPI @ 40 MHz) |
| **Controls** | 2x Tactile Push Buttons (Top Shutter button + Rear Gallery button) |
| **Audio** | Passive Piezoelectric Buzzer for acoustic UI feedback |
| **Power** | 3.3V DC (or 1S LiPo battery with 3.3V LDO regulator) |

---

## 🔌 Pinout & Wiring

### 1. ST7789 IPS Display -> ESP32-S3
| Display Pin | Description | ESP32-S3 Pin |
| :--- | :--- | :--- |
| **VCC / VDD** | Power Supply (3.3V) | **3.3V** |
| **GND** | Ground | **GND** |
| **CS** | Chip Select | **GPIO 10** |
| **RESET / RST** | Hardware Reset | **GPIO 9** |
| **DC / A0 / RS** | Data / Command | **GPIO 8** |
| **SDA / MOSI** | SPI Master Out Slave In | **GPIO 11** |
| **SCK / SCLK** | SPI Clock | **GPIO 12** |
| **LED / BLK** | Backlight Power | **3.3V** |

### 2. OV7670 FIFO -> ESP32-S3
| OV7670 Pin | Function | ESP32-S3 Pin / Destination |
| :--- | :--- | :--- |
| **3.3V** | Power Supply | **3.3V** |
| **GND** | Ground | **GND** |
| **SIOD** | I2C Data (SDA) | **GPIO 4** |
| **SIOC** | I2C Clock (SCL) | **GPIO 5** |
| **VSY / VSYNC** | Vertical Sync Pulse | **GPIO 6** |
| **HREF** | Horizontal Sync | *Unconnected* |
| **PWON / PWDN** | Power Down Mode | **GND** (Always Active) |
| **RST / RESET** | Sensor Reset | **3.3V** (Pull-High) |
| **OE** | FIFO Output Enable | **GND** (Always Enabled) |
| **WR** | FIFO Write Enable | **GPIO 15** |
| **WRST** | FIFO Write Reset | **GPIO 16** |
| **RRST** | FIFO Read Reset | **GPIO 17** |
| **RCK / STR** | FIFO Read Clock | **GPIO 18** |
| **STR** | Flash Strobe | *Unconnected* |
| **PCK** | Pixel Clock | *Unconnected* (Handled by FIFO) |
| **D0** | Data Bit 0 | **GPIO 1** |
| **D1** | Data Bit 1 | **GPIO 2** |
| **D2** | Data Bit 2 | **GPIO 3** |
| **D3** | Data Bit 3 | **GPIO 7** |
| **D4** | Data Bit 4 | **GPIO 13** |
| **D5** | Data Bit 5 | **GPIO 14** |
| **D6** | Data Bit 6 | **GPIO 21** |
| **D7** | Data Bit 7 | **GPIO 47** |

### 3. Push Buttons & Buzzer
| Component | Function | ESP32-S3 Pin | Wiring |
| :--- | :--- | :--- | :--- |
| **Camera Button** | Shutter / Viewfinder / WiFi AP | **GPIO 38** | Connected between GPIO 38 and **GND** (Internal Pull-Up) |
| **Gallery Button** | Gallery / Next Photo / Delete | **GPIO 39** | Connected between GPIO 39 and **GND** (Internal Pull-Up) |
| **Passive Buzzer** | Audio Feedback (+) | **GPIO 41** | Connected between GPIO 41 and **GND** |

---

## 🕹️ Controls & Gestures

### 📷 Shutter Button (Top)
* **In Live Viewfinder:**
  * **Single Click:** Instantly captures a photo and writes it to flash memory (`/DCIM/FOTO_xxxx.jpg`).
  * **Long Press (> 450 ms):** Shows circular transmission progress ring (`0% -> 100%`). Releasing after 100% starts **`cwmera wifi`** mode.
* **In Gallery Mode:**
  * **Any Click:** Exits gallery and returns immediately to the live camera preview.

### 🖼️ Gallery Button (Rear, next to screen)
* **In Live Viewfinder:**
  * **Any Click:** Opens the on-device photo gallery showing the most recent capture.
* **In Gallery Mode:**
  * **Single Click:** Cycles to the previous photo.
  * **Long Press (> 450 ms):** Shows circular deletion countdown ring (`0% -> 100%`). Releasing at 100% deletes the current photo from flash.

### 🔇 Mute / Unmute (Both Buttons Together)
* **Press Both Buttons Simultaneously:** Toggles all audio feedback on/off.
* The state is stored permanently in Non-Volatile Storage (NVS) across reboots.

### 🌐 In Wi-Fi Mode
* **Press Any Physical Button:** Immediately shuts down the Wi-Fi server and returns to live shooting mode.

---

## 📲 Wireless Photo Transfer (iOS & Android)

<p align="center">
  <img src="./images/IMG_4010.jpg" alt="cwmera web interface" width="85%">
</p>

1. In the camera viewfinder, **hold the top button** until the progress circle fills to 100% to enter `cwmera wifi` mode.
2. On your smartphone:
   * Connect to the Wi-Fi network named **`cwmera`** (no password required).
   * If a captive portal prompt appears, copy the link (`http://192.168.4.1/`), close the prompt (select *"Use Without Internet"* on iOS), and open **Safari** or **Chrome**.
   * Navigate to: **`http://192.168.4.1/`**
3. The web app allows you to:
   * View live storage status (free space, estimated shots remaining).
   * Toggle between **grid view** and **list view**.
   * Open full-screen lightbox preview with swipe navigation.
   * Save full-resolution photos directly to your smartphone's camera roll.
   * Delete unwanted captures.

---

## 🔄 Online Firmware Updates (OTA via GitHub Releases)

`cwmera` features an integrated wireless update flow without needing a USB cable:

1. Connect to the **`cwmera`** Wi-Fi hotspot and open `http://192.168.4.1/`.
2. Tap the **firmware update icon** in the top-right header.
3. Enter your home Wi-Fi SSID and password (check *"Remember this network"* to store it securely in NVS).
4. Tap **Verify**:
   * The camera disconnects the hotspot and joins your local Wi-Fi.
   * It securely queries the GitHub API (`api.github.com/repos/gwetano/cwmera/releases/latest`).
   * If a newer release is detected, it streams and flashes the binary asset (`.bin`), rendering a live progress bar on the ST7789 screen.
   * On success, it plays the victory jingle and reboots into the new version.

---

## ⚙️ Arduino IDE Setup & Partition Scheme

1. In the Arduino Boards Manager, install **`esp32` by Espressif Systems**.
2. Select your board: **`ESP32S3 Dev Module`**.
3. Configure the following parameters:
   * **USB CDC On Boot:** `Enabled`
   * **Flash Size:** `16MB` (or `8MB` depending on your chip)
   * **PSRAM:** `OPI PSRAM`
   * **Partition Scheme:**
     * **For 16MB Flash (Recommended):** `Small App w/ OTA + Huge FS (2MB APP/2MB OTA/12MB SPIFFS)`
     * **For 8MB Flash:** `8MB with spiffs (3MB APP/1.5MB SPIFFS)`

---

## 📦 3D Printable Enclosure

The CAD files and 3D printable models (`.stl` / `.step`) are located in the [`3d_models/`](./3d_models) directory.
* **Recommended Material:** Matte Black / White PLA or PETG.
* **Layer Height:** `0.16mm` or `0.20mm`.
* **Hardware:** M2 / M2.5 screws for panel mounting.

---

## 🗺️ Project Status & What's Missing

`cwmera` is fully operational as a functional point-and-shoot camera. Here is what is planned to make it a complete open-source maker product:

- [x] Fast FIFO double-buffer capture
- [x] ST7789 IPS live preview & gallery
- [x] JPEG compression to LittleFS
- [x] Web server gallery with mobile captive portal
- [x] GitHub Releases OTA auto-updater
- [x] Chiptune audio feedback & NVS settings
- [ ] **Battery Management Circuit**: Integrated 1S LiPo charging (TP4056 or AXP2101) with voltage divider ADC battery percentage meter on LCD.
- [ ] **Schematic / Wiring Diagram**: Exported KiCad schematic and breadboard wiring diagram.
- [ ] **PlatformIO Support**: `platformio.ini` environment definition for VS Code compilation.
- [ ] **GitHub Actions CI**: Automated release pipeline that compiles `.bin` assets on new git tags.

---

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](./LICENSE) file for details.
Designed and built by [Gaetano Carbone (@gwetano)](https://github.com/gwetano).
