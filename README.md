
![image](./images/ex.jpg)

## HARDWARE COMPONENTS

#### ESP32-S3

* **CPU:** Dual-Core Xtensa LX7 a 240 MHz.
* **Flash Memory:** **8 MB**
* **PSRAM (Memoria Extra):** **8 MB**

#### Display: ST7789 IPS LCD

* **Resolution:** **240x320 pixel**.
* **Protocol:** SPI.

#### OV7670 with FIFO (AL422B)

* **Mative Resolution:** VGA (640x480), configured as **QVGA (320x240)** to scale it into the display.
* **Color Format:** **RGB565** (16-bit per pixel, convertito da YUV via software/registri).
* **I2C Address (SCCB):** `0x21` 
* **Integrated FIFO Memory:** Chip AL422B. The ESP32-S3 can capture the photo and read it without losing any frame.

---

### PINOUT

#### Display ST7789 -> ESP32-S3

| Pin ST7789 | Function | Pin ESP32-S3 |
| --- | --- | --- |
| **VCC** / VDD | Power Supply | **3.3V** |
| **GND** | Ground | **GND** |
| **CS** | Chip Select | **GPIO 10** |
| **RESET** / RST | Reset Hardware | **GPIO 9** |
| **DC** / A0 / RS | Data/Command | **GPIO 8** |
| **SDA** / MOSI | SPI Data | **GPIO 11** |
| **SCK** / SCLK | SPI Clock | **GPIO 12** |
| **LED** / BLK | Backillumination | **3.3V** |

#### OV7670 FIFO -> ESP32-S3

| Pin OV7670 | Funciton | Pin ESP32-S3 / Destination |
| --- | --- | --- |
| **3.3V** | Power Supply | **3.3V** |
| **GND** | Ground | **GND** |
| **SIOD** | I2C Data (SDA) | **GPIO 4** |
| **SIOC** | I2C Clock (SCL) | **GPIO 5** |
| **VSY** / VSYNC | Vertical Synch | **GPIO 6** |
| **HREF** | Horizontal Synch | -|
| **PWON** / PWDN | Power Down | **GND** |
| **RST** / RESET | Reset | **3.3V* |
| **OE** | Output Enable FIFO | **GND** |
| **WR** | Write Enable FIFO | **GPIO 15** |
| **WRST** | Write Reset FIFO | **GPIO 16** |
| **RRST** | Read Reset FIFO | **GPIO 17** |
| **STR** / RCK | Read Clock FIFO | **GPIO 18** |
| **PCK** | Pixel Clock | *Scollegato* |
| **D0** | Data Bit 0 | **GPIO 1** |
| **D1** | Data Bit 1 | **GPIO 2** |
| **D2** | Data Bit 2 | **GPIO 3** |
| **D3** | Data Bit 3 | **GPIO 7** |
| **D4** | Data Bit 4 | **GPIO 13** |
| **D5** | Data Bit 5 | **GPIO 14** |
| **D6** | Data Bit 6 | **GPIO 21** |
| **D7** | Data Bit 7 | **GPIO 47** |