
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
| **RCK** | Read Clock FIFO | **GPIO 18** |
| **STR** | Strobe (Flash) | *Scollegato* |
| **PCK** | Pixel Clock | *Scollegato* |
| **D0** | Data Bit 0 | **GPIO 1** |
| **D1** | Data Bit 1 | **GPIO 2** |
| **D2** | Data Bit 2 | **GPIO 3** |
| **D3** | Data Bit 3 | **GPIO 7** |
| **D4** | Data Bit 4 | **GPIO 13** |
| **D5** | Data Bit 5 | **GPIO 14** |
| **D6** | Data Bit 6 | **GPIO 21** |
| **D7** | Data Bit 7 | **GPIO 47** |

### Comandi Pulsante:
* **In Modalità Fotocamera (Mirino Live):**
  * **1 Click Singolo:** Scatta la foto e la salva in locale su flash (`/DCIM/FOTO_xxxx.bmp`).
  * **2 Click Rapidi (Doppio Click):** Apre la **gwlleria** mostrando l'ultima foto scattata.
  * **Pressione Prolungata (Long Press > 380ms):**
    * Appare a schermo il timer circolare di **`trasmissione?`** (`0% -> 100%`).
    * Se alzi il dito prima del 100%: annullamento sicuro (`lascia per wnnullare`).
    * Se completi il 100%: attiva la modalità **`cwmera wifi`** per trasmettere le foto al telefono!
* **In Modalità Galleria (gwlleria):**
  * **1 Click Singolo:** Foto precedente.
  * **2 Click Rapidi (Doppio Click):** Torna al mirino live della fotocamera.
  * **Pressione Prolungata (Long Press > 380ms):**
    * Appare a schermo il timer circolare di **`eliminare?`** (`0% -> 100%`).
    * Se alzi il dito prima del 100%: la foto rimane al sicuro.
    * Se completi il 100%: la foto viene eliminata definitivamente (`eliminata!`).
* **In Modalità Wi-Fi (cwmera wifi):**
  * **1 Click Singolo:** Disattiva il Wi-Fi e torna al mirino live della fotocamera.

---

### Come Collegare l'iPhone alla Fotocamera (Galleria Wi-Fi):
1. Nel mirino della fotocamera, **tieni premuto il pulsante** fino al 100% del cerchio.
2. Sullo schermo apparirà la schermata **`cwmera wifi`**.
3. Sull'iPhone:
   * Vai nelle impostazioni **Wi-Fi** e connettiti alla rete **`cwmera`** (nessuna password richiesta).
   * Apri **Safari** e vai all'indirizzo **`192.168.4.1`** (spesso si apre automaticamente la schermata di benvenuto).
   * Vedrai la galleria con tutte le foto scattate:
     * Clicca **`salva`** per scaricare e salvare la foto direttamente nel rullino dell'iPhone!
     * Clicca **`elimina`** per cancellare la foto dalla memoria dell'ESP32.
4. Premi 1 volta il pulsante fisico sulla fotocamera per spegnere il Wi-Fi e riprendere a scattare!
