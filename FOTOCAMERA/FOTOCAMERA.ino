#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "soc/gpio_reg.h"

// ==============================================================================
// VERSIONE FIRMWARE & AGGIORNAMENTI OTA (GITHUB RELEASES)
// ==============================================================================
#define CWMERA_VERSION "1.1.0"
#define GITHUB_REPO    "gwetano/cwmera"

// ==============================================================================
// CONFIGURAZIONE PIN
// ==============================================================================

// --- DISPLAY (Hardware SPI 40 MHz) ---
#define TFT_CS         10
#define TFT_RST        9
#define TFT_DC         8
#define TFT_MOSI       11
#define TFT_SCLK       12
#define COLOR_DARKGREY 0x4208
#define COLOR_LIGHTGREY 0xD6BA
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);

// --- FOTOCAMERA OV7670 CON FIFO ---
#define SIOD_PIN  4
#define SIOC_PIN  5
#define VSY_PIN   6
#define WR_PIN    15
#define WRST_PIN  16
#define RRST_PIN  17
#define STR_PIN   18  // RCK: Read Clock FIFO

const int dataPins[8] = {1, 2, 3, 7, 13, 14, 21, 47};

// --- PULSANTI DI CONTROLLO ---
#define BTN_CAMERA_PIN   38  // Pulsante in alto: Mirino / Scatto / Wi-Fi
#define BTN_GALLERY_PIN  39  // Pulsante display: Galleria / Scorri / Elimina
#define BACKUP_BOOT_PIN  0   // Tasto BOOT su scheda ESP32 (backup pulsante fotocamera)

// --- BUZZER PASSIVO (FEEDBACK SONORO) ---
#define BUZZER_PIN       41  // Pin segnale (+) buzzer piezoelettrico passivo

// ==============================================================================
// STATI APPLICAZIONE E SERVER WI-FI
// ==============================================================================
enum AppMode {
  MODE_CAMERA,
  MODE_GALLERY,
  MODE_WIFI
};

AppMode currentMode = MODE_CAMERA;
int totalPhotos = 0;
int currentGalleryIndex = 0;

std::vector<int> photoList;

WebServer server(80);
DNSServer dnsServer;

// Stato richiesta OTA online da interfaccia Web
bool otaRequested = false;
unsigned long otaRequestedMillis = 0;
String otaTargetSSID = "";
String otaTargetPass = "";

// ==============================================================================
// ROUTINE FAST I/O PER FIFO OV7670
// ==============================================================================
#define STR_LOW()   REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << STR_PIN))
#define STR_HIGH()  REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << STR_PIN))

uint16_t lineBuffer[320];

static inline byte readFIFOByteFast() {
  uint32_t r0 = REG_READ(GPIO_IN_REG);
  uint32_t r1 = REG_READ(GPIO_IN1_REG);
  byte b = 0;
  if (r0 & (1UL << 1))  b |= (1 << 0);
  if (r0 & (1UL << 2))  b |= (1 << 1);
  if (r0 & (1UL << 3))  b |= (1 << 2);
  if (r0 & (1UL << 7))  b |= (1 << 3);
  if (r0 & (1UL << 13)) b |= (1 << 4);
  if (r0 & (1UL << 14)) b |= (1 << 5);
  if (r0 & (1UL << 21)) b |= (1 << 6);
  if (r1 & (1UL << (47 - 32))) b |= (1 << 7);
  return b;
}

// Sincronizzazione VSYNC pulita con rilevamento fronte ed eliminazione disallineamento byte
static inline void captureFrameToFIFO() {
  // 1. Se VSYNC e' gia' HIGH (meta' intervallo blanking), attendi prima che torni LOW
  while (digitalRead(VSY_PIN) == HIGH);

  // 2. Attendi il fronte di salita (inizio pulito dell'intervallo di blanking verticale del nuovo frame)
  while (digitalRead(VSY_PIN) == LOW);

  // 3. Resetta il puntatore di SCRITTURA della FIFO (WRST)
  digitalWrite(WRST_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(WRST_PIN, HIGH);

  // 4. Attendi il fronte di discesa (inizio esatto della trasmissione attiva dei pixel)
  while (digitalRead(VSY_PIN) == HIGH);

  // 5. Abilita la scrittura nella FIFO
  digitalWrite(WR_PIN, HIGH);

  // 6. Attendi il completamento di tutte le 240 linee (VSYNC torna HIGH)
  while (digitalRead(VSY_PIN) == LOW);

  // 7. Blocca immediatamente la scrittura nella FIFO
  digitalWrite(WR_PIN, LOW);
}

// Reset del puntatore di LETTURA della FIFO all'indirizzo 0x00000 (primo byte pixel 0)
static inline void resetFIFOReadPointer() {
  digitalWrite(RRST_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(STR_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(STR_PIN, HIGH);
  delayMicroseconds(2);
  digitalWrite(RRST_PIN, HIGH);
  delayMicroseconds(5);
}

// ==============================================================================
// FILTRO BIANCO E NERO (SATURAZIONE 0) CON HIGHLIGHT ROLL-OFF & DITHERING
// ==============================================================================
#define BLACK_AND_WHITE 1

static inline uint16_t rgb565ToGrayscale(uint16_t color, int x, int y) {
  uint32_t r = (color >> 11) & 0x1F;
  uint32_t g = (color >> 5) & 0x3F;
  uint32_t b = color & 0x1F;

  // Espansione canali a 8 bit (0..255)
  uint32_t r8 = (r << 3) | (r >> 2);
  uint32_t g8 = (g << 2) | (g >> 4);
  uint32_t b8 = (b << 3) | (b >> 2);

  // Luminanza standard Rec.601 ad alta precisione
  uint32_t luma = (r8 * 77 + g8 * 150 + b8 * 29) >> 8;

  // Compressione morbida delle alte luci (Highlight Roll-off):
  // Evita lo stacco netto a gradino e la saturazione brusca nelle zone molto illuminate
  if (luma > 210) {
    uint32_t over = luma - 210;
    luma = 210 + (over * 45) / (over + 10);
    if (luma > 255) luma = 255;
  }

  // Micro-dithering 2x2: dissolve la posterizzazione e i bordi puntinati in un gradiente continuo
  int d = ((x ^ y) & 1) ? 2 : -2;
  int luma_d = (int)luma + d;
  if (luma_d < 0) luma_d = 0;
  else if (luma_d > 255) luma_d = 255;

  // Ricostruzione RGB565 a 64 livelli continui
  uint32_t r5 = (uint32_t)luma_d >> 3;
  uint32_t g6 = (uint32_t)luma_d >> 2;
  uint32_t b5 = (uint32_t)luma_d >> 3;

  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

bool writeRegister(byte reg, byte val) {
  for (int attempt = 0; attempt < 5; attempt++) {
    Wire.beginTransmission(0x21);
    Wire.write(reg);
    Wire.write(val);
    if (Wire.endTransmission() == 0) {
      return true;
    }
    delay(3);
  }
  return false;
}

// ==============================================================================
// GESTIONE AUDIO E EFFETTI SONORI (BUZZER PASSIVO)
// ==============================================================================
Preferences soundPrefs;
bool soundEnabled = true;

void initSoundSetting() {
  soundPrefs.begin("cwmera", false);
  soundEnabled = soundPrefs.getBool("sound", true);
}

void saveSoundSetting(bool enabled) {
  soundPrefs.putBool("sound", enabled);
}

// Jingle avvio a 3 picchi stile Super Mario (Fungo verde 1-UP: C6 -> E6 -> C7)
void soundBoot() {
  if (!soundEnabled) return;
  tone(BUZZER_PIN, 1047); // C6
  delay(65);
  noTone(BUZZER_PIN);
  delay(15);
  tone(BUZZER_PIN, 1319); // E6
  delay(65);
  noTone(BUZZER_PIN);
  delay(15);
  tone(BUZZER_PIN, 2093); // C7
  delay(140);
  noTone(BUZZER_PIN);
}

void soundShutter() {
  if (!soundEnabled) return;
  tone(BUZZER_PIN, 1600);
  delay(18);
  noTone(BUZZER_PIN);
  delay(12);
  tone(BUZZER_PIN, 2400);
  delay(30);
  noTone(BUZZER_PIN);
}

void soundWiFiStart() {
  if (!soundEnabled) return;
  tone(BUZZER_PIN, 2000);
  delay(55);
  noTone(BUZZER_PIN);
  delay(40);
  tone(BUZZER_PIN, 2800);
  delay(75);
  noTone(BUZZER_PIN);
}

void soundError() {
  if (!soundEnabled) return;
  tone(BUZZER_PIN, 440);
  delay(120);
  noTone(BUZZER_PIN);
  delay(50);
  tone(BUZZER_PIN, 330);
  delay(160);
  noTone(BUZZER_PIN);
}

// ==============================================================================
// GESTIONE MEMORIA E FILE SYSTEM
// ==============================================================================
#define DCIM_DIR "/DCIM"

String getPhotoPath(int photoNumber) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%s/FOTO_%04d.jpg", DCIM_DIR, photoNumber);
  if (LittleFS.exists(buf)) return String(buf);
  snprintf(buf, sizeof(buf), "%s/FOTO_%04d.bmp", DCIM_DIR, photoNumber);
  if (LittleFS.exists(buf)) return String(buf);
  snprintf(buf, sizeof(buf), "%s/FOTO_%04d.jpg", DCIM_DIR, photoNumber);
  return String(buf);
}

void scanPhotos() {
  photoList.clear();
  File dir = LittleFS.open(DCIM_DIR);
  if (dir) {
    File f = dir.openNextFile();
    while (f) {
      String name = f.name();
      int idx = name.indexOf("FOTO_");
      if (idx >= 0) {
        String numStr = name.substring(idx + 5, idx + 9);
        int val = numStr.toInt();
        if (val > 0) {
          photoList.push_back(val);
        }
      }
      f = dir.openNextFile();
    }
  }
  std::sort(photoList.begin(), photoList.end());
  totalPhotos = photoList.size();
  Serial.printf("Foto trovate: %d\n", totalPhotos);
}

void initStorage() {
  if (!LittleFS.begin(true)) {
    Serial.println("Errore fatale: LittleFS non montato!");
    return;
  }
  if (!LittleFS.exists(DCIM_DIR)) {
    LittleFS.mkdir(DCIM_DIR);
  }
  scanPhotos();
}

void writeBMPHeader(File &f, int32_t width, int32_t height) {
  uint32_t rowSize = width * 3;
  uint32_t imageSize = rowSize * height;
  uint32_t fileSize = 54 + imageSize;

  uint8_t header[54];
  memset(header, 0, 54);

  header[0] = 'B'; header[1] = 'M';
  header[2] = (uint8_t)(fileSize);
  header[3] = (uint8_t)(fileSize >> 8);
  header[4] = (uint8_t)(fileSize >> 16);
  header[5] = (uint8_t)(fileSize >> 24);
  header[10] = 54;

  header[14] = 40;
  header[18] = (uint8_t)(width);
  header[19] = (uint8_t)(width >> 8);
  header[20] = (uint8_t)(width >> 16);
  header[21] = (uint8_t)(width >> 24);

  int32_t negHeight = -height;
  header[22] = (uint8_t)(negHeight);
  header[23] = (uint8_t)(negHeight >> 8);
  header[24] = (uint8_t)(negHeight >> 16);
  header[25] = (uint8_t)(negHeight >> 24);

  header[26] = 1;
  header[28] = 24;
  header[34] = (uint8_t)(imageSize);
  header[35] = (uint8_t)(imageSize >> 8);
  header[36] = (uint8_t)(imageSize >> 16);
  header[37] = (uint8_t)(imageSize >> 24);

  header[38] = 0x13; header[39] = 0x0B;
  header[42] = 0x13; header[43] = 0x0B;

  f.write(header, 54);
}

bool captureAndSaveBMP(const String &path) {
  captureFrameToFIFO();
  resetFIFOReadPointer();

  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return false;

  writeBMPHeader(f, 320, 240);
  uint8_t rowBytes[320 * 3];

  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 320; x++) {
      STR_LOW();
      __asm__("nop; nop;");
      byte b1 = readFIFOByteFast();
      STR_HIGH();
      __asm__("nop; nop;");

      STR_LOW();
      __asm__("nop; nop;");
      byte b2 = readFIFOByteFast();
      STR_HIGH();
      __asm__("nop; nop;");

      uint16_t color = (b1 << 8) | b2;
#if BLACK_AND_WHITE
      color = rgb565ToGrayscale(color, x, y);
#endif

      uint8_t r = ((color >> 11) & 0x1F) << 3;
      uint8_t g = ((color >> 5) & 0x3F) << 2;
      uint8_t b = (color & 0x1F) << 3;

      rowBytes[x * 3 + 0] = b;
      rowBytes[x * 3 + 1] = g;
      rowBytes[x * 3 + 2] = r;
    }
    f.write(rowBytes, 320 * 3);
  }

  f.close();
  return true;
}

bool displayBMP(const String &path, int startY = 0, int endY = 240) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;

  uint8_t header[54];
  if (f.read(header, 54) != 54 || header[0] != 'B' || header[1] != 'M') {
    f.close();
    return false;
  }

  if (startY < 0) startY = 0;
  if (endY > 240) endY = 240;
  if (startY >= endY) {
    f.close();
    return false;
  }

  if (startY > 0) {
    f.seek(54 + (size_t)startY * 320 * 3);
  }

  int drawHeight = endY - startY;
  tft.startWrite();
  tft.setAddrWindow(0, startY, 320, drawHeight);

  uint8_t rowBytes[320 * 3];
  uint16_t rowPixels[320];

  for (int y = startY; y < endY; y++) {
    if (f.read(rowBytes, 320 * 3) != 320 * 3) break;
    for (int x = 0; x < 320; x++) {
      uint8_t b = rowBytes[x * 3 + 0];
      uint8_t g = rowBytes[x * 3 + 1];
      uint8_t r = rowBytes[x * 3 + 2];
      rowPixels[x] = tft.color565(r, g, b);
    }
    tft.writePixels(rowPixels, 320);
  }
  tft.endWrite();
  f.close();
  return true;
}

bool captureAndSaveJPEG(const String &path) {
  captureFrameToFIFO();
  resetFIFOReadPointer();

  size_t frameSize = 320 * 240 * sizeof(uint16_t);
  uint8_t *frameBuf = (uint8_t*)ps_malloc(frameSize);
  if (!frameBuf) frameBuf = (uint8_t*)malloc(frameSize);
  if (!frameBuf) {
    Serial.println("Errore: RAM insufficiente per scatto JPEG!");
    return false;
  }

  int idx = 0;
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 320; x++) {
      STR_LOW();
      __asm__("nop; nop;");
      byte b1 = readFIFOByteFast();
      STR_HIGH();
      __asm__("nop; nop;");

      STR_LOW();
      __asm__("nop; nop;");
      byte b2 = readFIFOByteFast();
      STR_HIGH();
      __asm__("nop; nop;");

#if BLACK_AND_WHITE
      uint16_t c = rgb565ToGrayscale((b1 << 8) | b2, x, y);
      frameBuf[idx++] = (uint8_t)(c >> 8);
      frameBuf[idx++] = (uint8_t)(c & 0xFF);
#else
      frameBuf[idx++] = b1;
      frameBuf[idx++] = b2;
#endif
    }
  }

  // Visualizza immediatamente il fermo immagine sul display (freeze frame)
  tft.startWrite();
  tft.setAddrWindow(0, 0, 320, 240);
  uint16_t rowPixels[320];
  for (int y = 0; y < 240; y++) {
    int rowOffset = y * 320 * 2;
    for (int x = 0; x < 320; x++) {
      byte b1 = frameBuf[rowOffset + x * 2];
      byte b2 = frameBuf[rowOffset + x * 2 + 1];
      rowPixels[x] = (b1 << 8) | b2;
    }
    tft.writePixels(rowPixels, 320);
  }
  tft.endWrite();

  uint8_t *jpg_buf = NULL;
  size_t jpg_len = 0;
  bool ok = fmt2jpg(frameBuf, frameSize, 320, 240, PIXFORMAT_RGB565, 82, &jpg_buf, &jpg_len);
  free(frameBuf);

  if (!ok || !jpg_buf) {
    Serial.println("Errore compressione JPEG!");
    if (jpg_buf) free(jpg_buf);
    return false;
  }

  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    free(jpg_buf);
    return false;
  }
  size_t written = f.write(jpg_buf, jpg_len);
  f.close();
  free(jpg_buf);

  Serial.printf("JPEG salvato: %s (%d bytes)\n", path.c_str(), (int)jpg_len);
  return (written == jpg_len);
}

bool displayJPEG(const String &path, int startY = 0, int endY = 240) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  size_t jpgSize = f.size();
  uint8_t *jpgBuf = (uint8_t*)malloc(jpgSize);
  if (!jpgBuf) {
    f.close();
    return false;
  }
  f.read(jpgBuf, jpgSize);
  f.close();

  size_t rgbSize = 320 * 240 * 3;
  uint8_t *rgbBuf = (uint8_t*)ps_malloc(rgbSize);
  if (!rgbBuf) rgbBuf = (uint8_t*)malloc(rgbSize);
  if (!rgbBuf) {
    free(jpgBuf);
    return false;
  }

  bool ok = fmt2rgb888(jpgBuf, jpgSize, PIXFORMAT_JPEG, rgbBuf);
  free(jpgBuf);

  if (!ok) {
    free(rgbBuf);
    return false;
  }

  if (startY < 0) startY = 0;
  if (endY > 240) endY = 240;
  if (startY >= endY) {
    free(rgbBuf);
    return false;
  }

  int drawHeight = endY - startY;

  tft.startWrite();
  tft.setAddrWindow(0, startY, 320, drawHeight);
  uint16_t rowPixels[320];

  for (int y = startY; y < endY; y++) {
    int rowOffset = y * 320 * 3;
    for (int x = 0; x < 320; x++) {
      int px = rowOffset + x * 3;
      uint8_t b = rgbBuf[px + 0];
      uint8_t g = rgbBuf[px + 1];
      uint8_t r = rgbBuf[px + 2];
      rowPixels[x] = tft.color565(r, g, b);
    }
    tft.writePixels(rowPixels, 320);
  }
  tft.endWrite();

  free(rgbBuf);
  return true;
}

bool displayPhoto(const String &path, int startY = 0, int endY = 240) {
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    return displayJPEG(path, startY, endY);
  }
  return displayBMP(path, startY, endY);
}

void drawArcSegment(int cx, int cy, int r1, int r2, float startDeg, float endDeg, uint16_t color) {
  for (float deg = startDeg; deg <= endDeg; deg += 1.2f) {
    float rad = (deg - 90.0f) * 0.0174532925f;
    float cosA = cos(rad);
    float sinA = sin(rad);
    for (int r = r1; r <= r2; r++) {
      int px = cx + (int)(r * cosA + 0.5f);
      int py = cy + (int)(r * sinA + 0.5f);
      tft.drawPixel(px, py, color);
    }
  }
}

// ==============================================================================
// WEB SERVER E INTERFACCIA WEB (MINIMALISTA, SFONDO BIANCO, BORDI NETTI)
// ==============================================================================

bool isCaptivePortalRequest() {
  String uri = server.uri();
  if (uri.indexOf("hotspot-detect") >= 0 || uri.indexOf("canonical") >= 0 || uri.indexOf("generate_204") >= 0) {
    return true;
  }
  String host = server.hostHeader();
  if (host.length() > 0 && host != "192.168.4.1" && host != "cwmera.local") {
    return true;
  }
  return false;
}

void handleCaptivePortal() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>cwmera</title>";
  html += "<style>*{box-sizing:border-box;margin:0;padding:0;border-radius:0!important;}";
  html += "body{font-family:-apple-system,system-ui,sans-serif;background:#fff;color:#000;padding:20px;text-align:left;}";
  html += "h1{font-size:26px;font-weight:800;margin-bottom:4px;}";
  html += ".sub{font-size:13px;color:#666;margin-bottom:20px;}";
  html += ".box{border:1px solid #000;padding:16px;margin-bottom:20px;}";
  html += ".step{font-size:14px;margin-bottom:12px;line-height:1.5;}";
  html += ".step:last-child{margin-bottom:0;}";
  html += ".url-bar{display:flex;border:1px solid #000;margin-bottom:20px;}";
  html += ".url-txt{flex:1;padding:12px;font-size:15px;font-weight:700;background:#f5f5f5;font-family:monospace;display:flex;align-items:center;}";
  html += ".btn-copy{background:#000;color:#fff;border:none;padding:12px 18px;font-size:14px;font-weight:700;display:flex;align-items:center;gap:6px;cursor:pointer;}";
  html += ".btn-copy svg{stroke:#fff;}";
  html += "</style></head><body>";
  html += "<h1>cwmera</h1>";
  html += "<div class='sub'>trasmissione foto</div>";

  html += "<div class='box'>";
  html += "<div class='step'><strong>1. Copia l'indirizzo</strong> con il tasto sottostante.</div>";
  html += "<div class='step'><strong>2. Chiudi questa schermata</strong>:<br>&bull; Su iPhone tocca <strong>'Fine'</strong> in alto e scegli <strong>'Usa senza Internet'</strong>.<br>&bull; Su Android mantieni la connessione Wi-Fi dalla notifica.</div>";
  html += "<div class='step'><strong>3. Apri il browser</strong> (Safari, Chrome...) e <strong>incolla l'indirizzo</strong> per scaricare le foto nel rullino.</div>";
  html += "</div>";

  html += "<div class='url-bar'>";
  html += "<div class='url-txt' id='u-val'>http://192.168.4.1/</div>";
  html += "<button class='btn-copy' id='b-cp' onclick='copyLink()'>";
  html += "<svg width='16' height='16' viewBox='0 0 24 24' fill='none' stroke-width='2.2'><rect x='9' y='9' width='13' height='13' rx='2' ry='2'/><path d='M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1'/></svg>";
  html += "<span id='b-txt'>copia</span>";
  html += "</button>";
  html += "</div>";

  html += "<script>";
  html += "function copyLink(){";
  html += "const u='http://192.168.4.1/';";
  html += "if(navigator.clipboard&&navigator.clipboard.writeText){";
  html += "navigator.clipboard.writeText(u).then(done);";
  html += "}else{";
  html += "const t=document.createElement('textarea');t.value=u;document.body.appendChild(t);t.select();";
  html += "try{document.execCommand('copy');done();}catch(e){}";
  html += "document.body.removeChild(t);";
  html += "}}";
  html += "function done(){";
  html += "const b=document.getElementById('b-cp');const s=document.getElementById('b-txt');";
  html += "b.style.background='#222';s.innerText='copiato!';";
  html += "setTimeout(()=>{s.innerText='copia';},2000);";
  html += "}";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleGalleryPage() {
  size_t totalB = LittleFS.totalBytes();
  size_t usedB = LittleFS.usedBytes();
  size_t freeB = (totalB > usedB) ? (totalB - usedB) : 0;
  const size_t AVG_PHOTO_SIZE = 28 * 1024;
  int photosLeft = freeB / AVG_PHOTO_SIZE;

  char memStr[16];
  if (freeB >= 1024 * 1024) snprintf(memStr, sizeof(memStr), "%.1f MB", (float)freeB / (1024.0f * 1024.0f));
  else snprintf(memStr, sizeof(memStr), "%d KB", (int)(freeB / 1024));

  String jsonList = "[";
  for (size_t i = 0; i < photoList.size(); i++) {
    if (i > 0) jsonList += ",";
    jsonList += String(photoList[i]);
  }
  jsonList += "]";

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>cwmera</title><style>";
  html += "*{box-sizing:border-box;margin:0;padding:0;border-radius:0!important;}";
  html += "body{font-family:-apple-system,system-ui,sans-serif;background:#fff;color:#000;padding:14px;text-align:left;}";
  html += "header{border-bottom:2px solid #000;padding-bottom:12px;margin-bottom:16px;}";
  html += ".top-bar{display:flex;justify-content:space-between;align-items:flex-end;}";
  html += ".brand{font-size:24px;font-weight:800;letter-spacing:-0.5px;}";
  html += ".count{font-size:13px;color:#555;margin-top:4px;}";
  html += ".storage{font-size:12px;color:#666;margin-top:3px;}";
  html += ".storage strong{color:#000;}";
  html += ".v-switch{display:flex;gap:6px;}";
  html += ".v-btn{border:1px solid #000;background:#fff;color:#000;padding:6px 10px;font-size:12px;font-weight:700;cursor:pointer;}";
  html += ".v-btn.act{background:#000;color:#fff;}";
  html += ".grid-mode{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
  html += ".list-mode{display:flex;flex-direction:column;gap:14px;}";
  html += ".card{border:1px solid #000;background:#fff;padding:6px;}";
  html += ".card img{width:100%;height:auto;display:block;margin-bottom:6px;cursor:pointer;border:1px solid #eee;}";
  html += ".actions{display:flex;gap:6px;}";
  html += ".btn-dl{flex:1;display:flex;align-items:center;justify-content:center;background:#000;color:#fff;border:1px solid #000;padding:10px 4px;cursor:pointer;}";
  html += ".btn-del{flex:1;display:flex;align-items:center;justify-content:center;background:#fff;color:#000;border:1px solid #999;padding:10px 4px;cursor:pointer;}";
  html += "#btn-ota{display:flex;align-items:center;justify-content:center;padding:6px 9px;}";
  html += "#ota-m{display:none;position:fixed;inset:0;background:rgba(255,255,255,0.94);backdrop-filter:blur(3px);z-index:1000;padding:16px;align-items:center;justify-content:center;}";
  html += "#ota-m.open{display:flex;}";
  html += ".ota-box{background:#fff;border:2px solid #000;width:100%;max-width:320px;padding:18px;}";
  html += ".ota-t{font-size:18px;font-weight:800;margin-bottom:4px;}";
  html += ".ota-v{font-size:12px;color:#555;margin-bottom:16px;}";
  html += ".ota-lbl{font-size:11px;font-weight:700;text-transform:uppercase;margin-bottom:4px;display:block;}";
  html += ".ota-in{width:100%;border:1px solid #000;padding:8px 10px;font-size:14px;margin-bottom:10px;font-family:inherit;}";
  html += ".ota-chk{display:flex;align-items:center;gap:6px;font-size:12px;margin-bottom:16px;cursor:pointer;}";
  html += ".ota-actions{display:flex;gap:8px;}";
  html += ".ota-btn{flex:1;border:1px solid #000;background:#000;color:#fff;padding:10px 6px;font-size:13px;font-weight:700;cursor:pointer;text-align:center;}";
  html += ".ota-btn-sec{background:#fff;color:#000;}";
  html += ".empty{padding:30px 0;font-size:15px;color:#333;border:1px solid #000;text-align:left;padding-left:14px;margin-top:20px;}";
  html += "#lb{display:none;position:fixed;inset:0;background:#fff;z-index:999;flex-direction:column;padding:12px;}";
  html += "#lb.open{display:flex;}";
  html += ".lb-head{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #000;padding-bottom:10px;}";
  html += ".lb-title{font-size:14px;font-weight:700;}";
  html += ".lb-close{border:1px solid #000;background:#fff;color:#000;padding:6px 12px;font-size:13px;font-weight:700;cursor:pointer;}";
  html += ".lb-body{flex:1;display:flex;align-items:center;justify-content:space-between;position:relative;margin:12px 0;}";
  html += ".lb-img-wrap{width:100%;height:100%;display:flex;align-items:center;justify-content:center;}";
  html += ".lb-img-wrap img{max-width:100%;max-height:calc(100vh - 170px);object-fit:contain;border:1px solid #000;}";
  html += ".lb-nav{position:absolute;top:50%;transform:translateY(-50%);background:#fff;border:1px solid #000;width:38px;height:52px;font-size:22px;font-weight:800;display:flex;align-items:center;justify-content:center;cursor:pointer;}";
  html += ".lb-prev{left:0;}.lb-next{right:0;}";
  html += ".lb-foot{border-top:1px solid #000;padding-top:10px;display:flex;gap:8px;}";
  html += "</style></head><body>";

  html += "<header>";
  html += "<div class='top-bar'>";
  html += "<div><div class='brand'>cwmera</div>";
  html += "<div class='count'>foto in memoria: <span id='c-val'></span></div>";
  html += "<div class='storage'>memoria libera: <strong>" + String(memStr) + "</strong> (~" + String(photosLeft) + " foto)</div></div>";
  html += "<div class='v-switch'>";
  html += "<button id='btn-ota' class='v-btn' onclick='openOta()' title='aggiorna firmware'>";
  html += "<svg width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.2'><path d='M21 15v4H3v-4M7 10l5 5 5-5M12 15V3'/></svg>";
  html += "</button>";
  html += "<button id='btn-g' class='v-btn act' onclick='setView(\"grid\")'>matrice</button>";
  html += "<button id='btn-l' class='v-btn' onclick='setView(\"list\")'>elenco</button>";
  html += "</div></div></header>";

  html += "<div id='view-box'></div>";

  // Carosello Lightbox fullscreen
  html += "<div id='lb'>";
  html += "<div class='lb-head'>";
  html += "<div class='lb-title' id='lb-count'></div>";
  html += "<button onclick='closeLb()' class='lb-close'>chiudi</button>";
  html += "</div>";
  html += "<div class='lb-body'>";
  html += "<button class='lb-nav lb-prev' onclick='prevLb()'>&#8249;</button>";
  html += "<div class='lb-img-wrap'><img id='lb-img' src=''></div>";
  html += "<button class='lb-nav lb-next' onclick='nextLb()'>&#8250;</button>";
  html += "</div>";
  html += "<div class='lb-foot'>";
  html += "<a id='lb-dl' class='btn-dl' href='' title='scarica'>";
  html += "<svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.5'><path d='M12 3v13M6 11l6 6 6-6M3 20h18'/></svg></a>";
  html += "<button id='lb-del' class='btn-del' title='elimina'>";
  html += "<svg width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='#222' stroke-width='2'><path d='M3 6h18M8 6V4h8v2M19 6v14H5V6M10 11v6M14 11v6'/></svg></button>";
  html += "</div></div>";

  // Modal Aggiornamento OTA Online
  html += "<div id='ota-m'>";
  html += "<div class='ota-box'>";
  html += "<div id='ota-form'>";
  html += "<div class='ota-t'>aggiornamento</div>";
  html += "<div class='ota-v'>versione corrente: <strong>v" CWMERA_VERSION "</strong></div>";
  html += "<label class='ota-lbl'>rete wi-fi (2.4 ghz)</label>";
  html += "<input type='text' id='ota-ssid' class='ota-in' placeholder='nome rete (SSID)' autocapitalize='none' autocomplete='off'>";
  html += "<label class='ota-lbl'>password</label>";
  html += "<input type='password' id='ota-pass' class='ota-in' placeholder='password'>";
  html += "<label class='ota-chk'><input type='checkbox' id='ota-save' checked> memorizza questa rete</label>";
  html += "<div class='ota-actions'>";
  html += "<button class='ota-btn ota-btn-sec' onclick='closeOta()'>annulla</button>";
  html += "<button class='ota-btn' onclick='submitOta()'>verifica</button>";
  html += "</div></div>";
  html += "<div id='ota-wait' style='display:none;'>";
  html += "<div class='ota-t'>avviato!</div>";
  html += "<div style='font-size:13px;line-height:1.5;margin:12px 0 16px;'>cwmera si sta connettendo a <strong><span id='wait-ssid'></span></strong>.<br><br><strong>guarda lo schermo della fotocamera</strong> per seguire l'avanzamento su GitHub.</div>";
  html += "<div class='ota-v' style='margin-bottom:0;'>puoi chiudere questa pagina.</div>";
  html += "</div></div></div>";

  html += "<script>";
  html += "const photos=" + jsonList + ";";
  html += "let curIdx=0;let touchStartX=0;";
  html += "function render(){";
  html += "const vb=document.getElementById('view-box');";
  html += "document.getElementById('c-val').innerText=photos.length;";
  html += "if(!photos.length){vb.innerHTML='<div class=\"empty\">non hai scattato foto</div>';return;}";
  html += "let h='';";
  html += "for(let i=photos.length-1;i>=0;i--){";
  html += "const n=photos[i];";
  html += "h+='<div class=\"card\"><img src=\"/photo?id='+n+'\" onclick=\"openLb('+i+')\">';";
  html += "h+='<div class=\"actions\">';";
  html += "h+='<a href=\"/download?id='+n+'\" class=\"btn-dl\" title=\"scarica\">';";
  html += "h+='<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#fff\" stroke-width=\"2.5\"><path d=\"M12 3v13M6 11l6 6 6-6M3 20h18\"/></svg></a>';";
  html += "h+='<button onclick=\"delPhoto('+n+')\" class=\"btn-del\"><svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#222\" stroke-width=\"2\"><path d=\"M3 6h18M8 6V4h8v2M19 6v14H5V6M10 11v6M14 11v6\"/></svg></button>';";
  html += "h+='</div></div>';";
  html += "}";
  html += "vb.innerHTML=h;}";

  html += "function setView(m){";
  html += "const vb=document.getElementById('view-box');";
  html += "const bG=document.getElementById('btn-g');";
  html += "const bL=document.getElementById('btn-l');";
  html += "if(m==='grid'){vb.className='grid-mode';bG.className='v-btn act';bL.className='v-btn';}";
  html += "else{vb.className='list-mode';bL.className='v-btn act';bG.className='v-btn';}";
  html += "localStorage.setItem('cwmera_v',m);}";

  html += "function openLb(idx){curIdx=idx;updateLb();document.getElementById('lb').className='open';}";
  html += "function closeLb(){document.getElementById('lb').className='';}";
  html += "function updateLb(){";
  html += "const n=photos[curIdx];";
  html += "document.getElementById('lb-img').src='/photo?id='+n;";
  html += "document.getElementById('lb-count').innerText='foto '+(photos.length-curIdx)+' di '+photos.length;";
  html += "const dl=document.getElementById('lb-dl');dl.href='/download?id='+n;";
  html += "const del=document.getElementById('lb-del');del.onclick=()=>delPhoto(n);}";

  html += "function prevLb(){if(curIdx<photos.length-1){curIdx++;updateLb();}}";
  html += "function nextLb(){if(curIdx>0){curIdx--;updateLb();}}";

  html += "function delPhoto(n){if(confirm('eliminare foto #'+n+'?')){fetch('/delete?id='+n,{method:'POST'}).then(()=>location.reload());}}";

  html += "document.addEventListener('touchstart',e=>{touchStartX=e.changedTouches[0].screenX;},false);";
  html += "document.addEventListener('touchend',e=>{";
  html += "const lb=document.getElementById('lb');if(!lb||lb.className!=='open')return;";
  html += "const diffX=e.changedTouches[0].screenX-touchStartX;";
  html += "if(diffX>50)prevLb();if(diffX<-50)nextLb();},false);";

  html += "function openOta(){";
  html += "fetch('/ota-info').then(r=>r.json()).then(d=>{";
  html += "if(d.ssid)document.getElementById('ota-ssid').value=d.ssid;";
  html += "if(d.pass)document.getElementById('ota-pass').value=d.pass;";
  html += "document.getElementById('ota-m').className='open';";
  html += "}).catch(()=>{document.getElementById('ota-m').className='open';});";
  html += "}";
  html += "function closeOta(){document.getElementById('ota-m').className='';}";
  html += "function submitOta(){";
  html += "const s=document.getElementById('ota-ssid').value.trim();";
  html += "const p=document.getElementById('ota-pass').value;";
  html += "const sv=document.getElementById('ota-save').checked?'1':'0';";
  html += "if(!s){alert('inserisci il nome della rete');return;}";
  html += "document.getElementById('wait-ssid').innerText=s;";
  html += "document.getElementById('ota-form').style.display='none';";
  html += "document.getElementById('ota-wait').style.display='block';";
  html += "const fd=new FormData();fd.append('ssid',s);fd.append('pass',p);fd.append('save',sv);";
  html += "fetch('/ota-start',{method:'POST',body:fd}).catch(()=>{});";
  html += "}";

  html += "const sv=localStorage.getItem('cwmera_v')||'grid';setView(sv);render();";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleRoot() {
  if (isCaptivePortalRequest()) {
    handleCaptivePortal();
    return;
  }
  handleGalleryPage();
}

void handlePhoto() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Manca id foto");
    return;
  }
  int num = server.arg("id").toInt();
  String path = getPhotoPath(num);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "Foto non trovata");
    return;
  }
  String mime = (path.endsWith(".jpg") || path.endsWith(".jpeg")) ? "image/jpeg" : "image/bmp";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.streamFile(f, mime);
  f.close();
}

// Endpoint che forza il download reale su iOS Safari e Android (evita apertura su pagina bianca)
void handleDownload() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Manca id");
    return;
  }
  int num = server.arg("id").toInt();
  String path = getPhotoPath(num);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "Foto non trovata");
    return;
  }
  char fn[32];
  String ext = (path.endsWith(".jpg") || path.endsWith(".jpeg")) ? "jpg" : "bmp";
  snprintf(fn, sizeof(fn), "foto_%04d.%s", num, ext.c_str());
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + fn + "\"");
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.sendHeader("Connection", "close");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleDelete() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Manca id foto");
    return;
  }
  int num = server.arg("id").toInt();
  String path = getPhotoPath(num);
  LittleFS.remove(path);
  scanPhotos();
  server.send(200, "text/plain", "OK");
}

void handleOtaInfo() {
  Preferences p;
  p.begin("cwmera_net", true);
  String s = p.getString("ssid", "");
  String pw = p.getString("pass", "");
  p.end();

  String json = "{\"version\":\"" CWMERA_VERSION "\",\"ssid\":\"" + s + "\",\"pass\":\"" + pw + "\"}";
  server.send(200, "application/json", json);
}

void handleOtaStart() {
  otaTargetSSID = server.arg("ssid");
  otaTargetPass = server.arg("pass");
  bool save = (server.arg("save") == "1");

  if (save) {
    Preferences p;
    p.begin("cwmera_net", false);
    p.putString("ssid", otaTargetSSID);
    p.putString("pass", otaTargetPass);
    p.end();
  }

  server.send(200, "application/json", "{\"status\":\"ok\"}");
  otaRequested = true;
  otaRequestedMillis = millis();
}

void startWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("cwmera");
  delay(100);

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  server.on("/photo", HTTP_GET, handlePhoto);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/delete", HTTP_ANY, handleDelete);
  server.on("/ota-info", HTTP_GET, handleOtaInfo);
  server.on("/ota-start", HTTP_POST, handleOtaStart);
  server.onNotFound(handleRoot);
  server.begin();

  Serial.println("WiFi AP attivo: cwmera (192.168.4.1)");
}

void stopWiFi() {
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disattivato.");
}

// Schermo Wi-Fi su ST7789 (Bordi squadrati, sfondo bianco e testo nero come il sito)
void showWiFiScreen() {
  tft.fillScreen(ST77XX_WHITE);
  
  tft.drawRect(10, 10, 300, 220, ST77XX_BLACK);
  tft.drawRect(11, 11, 298, 218, ST77XX_BLACK);

  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(25, 25);
  tft.print("cwmera wifi");

  tft.setTextSize(1);
  tft.setCursor(25, 65);
  tft.print("1. connettiti alla rete:");
  tft.setTextSize(2);
  tft.setCursor(25, 85);
  tft.print("cwmera");

  tft.setTextSize(1);
  tft.setCursor(25, 125);
  tft.print("2. apri safari su:");
  tft.setTextSize(2);
  tft.setCursor(25, 145);
  tft.print("192.168.4.1");

  tft.drawFastHLine(10, 195, 300, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(25, 205);
  tft.print("click per tornare a scattare");
}

void waitForAnyButtonClick() {
  while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW || digitalRead(BTN_GALLERY_PIN) == LOW) {
    delay(10);
  }
  delay(50);
  while (digitalRead(BTN_CAMERA_PIN) == HIGH && digitalRead(BACKUP_BOOT_PIN) == HIGH && digitalRead(BTN_GALLERY_PIN) == HIGH) {
    delay(20);
  }
  while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW || digitalRead(BTN_GALLERY_PIN) == LOW) {
    delay(10);
  }
  delay(50);
}

void performOnlineOTA(String ssid, String pass) {
  // 1. Schermata base con bordi squadrati coerenti con showWiFiScreen()
  tft.fillScreen(ST77XX_WHITE);
  tft.drawRect(10, 10, 300, 220, ST77XX_BLACK);
  tft.drawRect(11, 11, 298, 218, ST77XX_BLACK);

  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(25, 25);
  tft.print("cwmera update");

  tft.setTextSize(1);
  tft.setCursor(25, 48);
  tft.printf("versione corrente: v%s", CWMERA_VERSION);

  tft.drawFastHLine(10, 62, 300, ST77XX_BLACK);

  // 2. Connessione Wi-Fi alla rete indicata
  tft.setCursor(25, 75);
  tft.print("1. connessione wi-fi:");
  tft.setCursor(25, 90);
  String dispSSID = ssid.length() > 18 ? ssid.substring(0, 15) + "..." : ssid;
  tft.printf("> %s", dispSSID.c_str());

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long startConn = millis();
  bool connected = false;
  int dotCount = 0;

  while (millis() - startConn < 15000) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    delay(400);
    dotCount++;
    tft.fillRect(25 + 130, 90, 45, 10, ST77XX_WHITE);
    tft.setCursor(25 + 130, 90);
    for (int d = 0; d < (dotCount % 4); d++) tft.print(".");
  }

  if (!connected) {
    tft.fillRect(25 + 130, 90, 45, 10, ST77XX_WHITE);
    tft.setCursor(220, 90);
    tft.print("[FALLITA]");
    tft.setCursor(25, 110);
    tft.print("errore: password o rete errata");
    tft.drawFastHLine(10, 195, 300, ST77XX_BLACK);
    tft.setCursor(25, 205);
    tft.print("click per tornare al mirino");
    soundError();
    waitForAnyButtonClick();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  tft.fillRect(25 + 130, 90, 45, 10, ST77XX_WHITE);
  tft.setCursor(235, 90);
  tft.print("[ OK ]");

  // 3. Controllo GitHub Releases
  tft.setCursor(25, 115);
  tft.print("2. verifica su github:");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://api.github.com/repos/" GITHUB_REPO "/releases/latest");
  http.addHeader("User-Agent", "cwmera-esp32");

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    tft.setCursor(25, 132);
    if (httpCode == 404) {
      tft.print("nessuna release su github");
    } else {
      tft.printf("errore github (HTTP %d)", httpCode);
    }
    tft.drawFastHLine(10, 195, 300, ST77XX_BLACK);
    tft.setCursor(25, 205);
    tft.print("click per tornare al mirino");
    soundError();
    waitForAnyButtonClick();
    http.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  String payload = http.getString();
  http.end();

  // Parsing tag_name della release
  String latestTag = "";
  int tagPos = payload.indexOf("\"tag_name\":");
  if (tagPos != -1) {
    int startQuote = payload.indexOf("\"", tagPos + 11);
    int endQuote = payload.indexOf("\"", startQuote + 1);
    latestTag = payload.substring(startQuote + 1, endQuote);
  }

  String cleanLatest = latestTag;
  if (cleanLatest.startsWith("v") || cleanLatest.startsWith("V")) cleanLatest = cleanLatest.substring(1);
  String cleanCurrent = CWMERA_VERSION;
  if (cleanCurrent.startsWith("v") || cleanCurrent.startsWith("V")) cleanCurrent = cleanCurrent.substring(1);

  if (cleanLatest.length() == 0 || cleanLatest.equalsIgnoreCase(cleanCurrent)) {
    tft.setCursor(25, 132);
    tft.printf("gia' aggiornato (v%s)", cleanCurrent.c_str());
    tft.drawFastHLine(10, 195, 300, ST77XX_BLACK);
    tft.setCursor(25, 205);
    tft.print("click per tornare al mirino");
    soundWiFiStart();
    waitForAnyButtonClick();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  // Nuova versione rilevata
  tft.setCursor(25, 132);
  tft.printf("trovata: v%s -> v%s", cleanCurrent.c_str(), cleanLatest.c_str());

  // Ricerca URL asset .bin
  String downloadUrl = "";
  int binPos = payload.indexOf(".bin\"");
  if (binPos != -1) {
    int urlPos = payload.lastIndexOf("\"browser_download_url\":", binPos);
    if (urlPos != -1) {
      int startQuote = payload.indexOf("\"", urlPos + 23);
      int endQuote = payload.indexOf("\"", startQuote + 1);
      downloadUrl = payload.substring(startQuote + 1, endQuote);
    }
  }

  if (downloadUrl.length() == 0) {
    downloadUrl = "https://github.com/" GITHUB_REPO "/releases/download/" + latestTag + "/cwmera.bin";
  }

  // 4. Download firmware e scrittura nella partizione OTA
  tft.setCursor(25, 153);
  tft.print("3. download firmware:");

  // Cornice progress bar
  tft.drawRect(25, 168, 270, 14, ST77XX_BLACK);

  tft.drawFastHLine(10, 195, 300, ST77XX_BLACK);
  tft.setCursor(25, 205);
  tft.print("non spegnere cwmera!");

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);

  httpUpdate.onProgress([](size_t cur, size_t total) {
    if (total > 0) {
      int fillW = (int)((cur * 266) / total);
      if (fillW > 266) fillW = 266;
      tft.fillRect(27, 170, fillW, 10, ST77XX_BLACK);
    }
  });

  t_httpUpdate_return ret = httpUpdate.update(client, downloadUrl);

  if (ret == HTTP_UPDATE_OK) {
    tft.fillScreen(ST77XX_WHITE);
    tft.drawRect(10, 10, 300, 220, ST77XX_BLACK);
    tft.drawRect(11, 11, 298, 218, ST77XX_BLACK);
    tft.setTextColor(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(25, 65);
    tft.print("AGGIORNATO!");
    tft.setTextSize(1);
    tft.setCursor(25, 105);
    tft.printf("cwmera v%s installata", cleanLatest.c_str());
    tft.setCursor(25, 135);
    tft.print("riavvio in corso...");
    
    soundBoot();
    delay(1500);
    ESP.restart();
  } else {
    tft.fillRect(12, 196, 296, 20, ST77XX_WHITE);
    tft.setCursor(25, 205);
    tft.printf("errore ota: %s", httpUpdate.getLastErrorString().c_str());
    soundError();
    waitForAnyButtonClick();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void showLiveTransitionScreen() {
  tft.fillScreen(ST77XX_WHITE);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(124, 112);
  tft.print("cwmera");
  delay(250);
}

// Barra superiore unificata per la galleria
void drawGalleryTopBar(int index, int total) {
  tft.fillRect(0, 0, 320, 23, ST77XX_BLACK);
  tft.drawFastHLine(0, 23, 320, COLOR_DARKGREY);

  // Sinistra: scritta "gwlleria" (size 2) + counter leggermente più piccolo (size 1)
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 4);
  tft.print("gwlleria");

  tft.setTextSize(1);
  tft.setCursor(8 + (8 * 12) + 6, 9);
  tft.printf("[%d/%d]", index, total);

  // Destra: Stima foto rimanenti + spazio libero
  size_t totalB = LittleFS.totalBytes();
  size_t usedB = LittleFS.usedBytes();
  size_t freeB = (totalB > usedB) ? (totalB - usedB) : 0;
  const size_t AVG_PHOTO_SIZE = 28 * 1024; // Stima media ~28 KB per scatto JPEG QVGA
  int photosLeft = freeB / AVG_PHOTO_SIZE;

  char photoBuf[16];
  snprintf(photoBuf, sizeof(photoBuf), "~%d foto", photosLeft);
  int photoWidth = strlen(photoBuf) * 6;

  char memBuf[16];
  if (freeB >= 1024 * 1024) {
    snprintf(memBuf, sizeof(memBuf), "%.1fMB", (float)freeB / (1024.0f * 1024.0f));
  } else {
    snprintf(memBuf, sizeof(memBuf), "%dKB", (int)(freeB / 1024));
  }
  int memWidth = strlen(memBuf) * 6;

  uint16_t remColor = (photosLeft < 20) ? ST77XX_RED : ST77XX_WHITE;

  // Riga 1: foto rimanenti (bianco, o rosso se sotto le 20)
  tft.setTextSize(1);
  tft.setTextColor(remColor);
  tft.setCursor(320 - 8 - photoWidth, 3);
  tft.print(photoBuf);

  // Riga 2: spazio di memoria rimanente (grigio chiaro e compatto)
  tft.setTextColor(COLOR_LIGHTGREY);
  tft.setCursor(320 - 8 - memWidth, 13);
  tft.print(memBuf);
}

// Aggiorna solo il contatore foto senza ridisegnare l'intera barra o causare sfarfallio
void updateGalleryCounter(int index, int total) {
  tft.fillRect(108, 4, 72, 16, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(110, 9);
  tft.printf("[%d/%d]", index, total);
}

// Barra inferiore con istruzioni chiare e gerarchia visiva calma
void drawGalleryBottomBar() {
  tft.fillRect(0, 222, 320, 18, ST77XX_BLACK);
  tft.drawFastHLine(0, 222, 320, COLOR_DARKGREY);
  tft.setTextSize(1);

  tft.setCursor(12, 227);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("1 click ");
  tft.setTextColor(COLOR_LIGHTGREY);
  tft.print("scorre foto   ");
  tft.setTextColor(COLOR_DARKGREY);
  tft.print("|   ");
  tft.setTextColor(ST77XX_WHITE);
  tft.print("tieni premuto ");
  tft.setTextColor(COLOR_LIGHTGREY);
  tft.print("elimina");
}

// ==============================================================================
// LOGICA GALLERIA ED INTERFACCIA A SCHERMO (STILE ORIGINARIO FOTOCAMERA)
// ==============================================================================
void showGalleryPhoto(int index, bool fullRefresh = true) {
  if (totalPhotos == 0 || photoList.empty()) {
    tft.fillScreen(ST77XX_BLACK);
    drawGalleryTopBar(0, 0);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(88, 100);
    tft.print("nessuna foto");
    
    tft.setTextSize(1);
    tft.setTextColor(COLOR_LIGHTGREY);
    tft.setCursor(55, 135);
    tft.print("premi tasto alto per scattare");
    return;
  }

  if (index < 1) index = 1;
  if (index > totalPhotos) index = totalPhotos;

  int fileNum = photoList[index - 1];
  String path = getPhotoPath(fileNum);

  // Disegna solo la finestra centrale della foto tra le due barre (y da 24 a 221)
  displayPhoto(path, 24, 222);

  if (fullRefresh) {
    // Al primo caricamento o dopo cancellazione: ridisegna le barre complete
    drawGalleryTopBar(index, totalPhotos);
    drawGalleryBottomBar();
  } else {
    // Durante lo scorrimento: aggiorna SOLTANTO il contatore foto!
    updateGalleryCounter(index, totalPhotos);
  }
}

void deleteCurrentPhoto(int index) {
  if (photoList.empty() || index < 1 || index > (int)photoList.size()) return;

  int fileNum = photoList[index - 1];
  String path = getPhotoPath(fileNum);
  LittleFS.remove(path);

  photoList.erase(photoList.begin() + (index - 1));
  totalPhotos = photoList.size();

  if (totalPhotos == 0) {
    currentGalleryIndex = 0;
    showGalleryPhoto(0);
  } else {
    if (currentGalleryIndex > totalPhotos) {
      currentGalleryIndex = totalPhotos;
    }
    showGalleryPhoto(currentGalleryIndex);
  }
}

// Timer sferico di eliminazione su pressione prolungata in galleria (banner scuro stile galleria, accenti rossi)
void handleCircularDeleteTimer() {
  if (totalPhotos == 0 || photoList.empty()) return;

  // Banner compatto a sfondo nero con bordi darkgrey in stile galleria
  tft.fillRect(0, 70, 320, 100, ST77XX_BLACK);
  tft.drawFastHLine(0, 70, 320, COLOR_DARKGREY);
  tft.drawFastHLine(0, 169, 320, COLOR_DARKGREY);

  // Titolo di stato in rosso a forte contrasto
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.setCursor(20, 86);
  tft.print("eliminazione");

  // Istruzione attiva in bianco
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(20, 114);
  tft.print("tieni premuto per confermare");

  // Istruzione di sicurezza/annullamento in grigio chiaro
  tft.setTextColor(COLOR_LIGHTGREY);
  tft.setTextSize(1);
  tft.setCursor(20, 132);
  tft.print("rilascia per annullare");

  // Anello circolare di caricamento compatto a destra
  const int CX = 255;
  const int CY = 120;
  const int R1 = 18;
  const int R2 = 27;

  // Traccia guida di sfondo in grigio scuro
  drawArcSegment(CX, CY, R1, R2, 0, 360, COLOR_DARKGREY);

  const unsigned long TIMER_DURATION_MS = 1600;
  unsigned long timerStart = millis();
  float lastAngle = 0.0f;
  bool completed = false;

  while (digitalRead(BTN_GALLERY_PIN) == LOW) {
    unsigned long elapsed = millis() - timerStart;
    float fraction = (float)elapsed / (float)TIMER_DURATION_MS;
    if (fraction > 1.0f) fraction = 1.0f;

    float currentAngle = fraction * 360.0f;
    if (currentAngle > lastAngle) {
      // Barra circolare in rosso vivo a forte contrasto sullo sfondo nero
      drawArcSegment(CX, CY, R1, R2, lastAngle, currentAngle, ST77XX_RED);
      lastAngle = currentAngle;
    }

    tft.fillRect(CX - 15, CY - 4, 30, 9, ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    int pct = (int)(fraction * 100);
    int pctX = (pct < 10) ? (CX - 7) : ((pct < 100) ? (CX - 10) : (CX - 14));
    tft.setCursor(pctX, CY - 4);
    tft.printf("%d%%", pct);

    if (fraction >= 1.0f) {
      completed = true;
      break;
    }
    delay(15);
  }

  if (completed) {
    tft.fillRect(0, 70, 320, 100, ST77XX_BLACK);
    tft.drawFastHLine(0, 70, 320, COLOR_DARKGREY);
    tft.drawFastHLine(0, 169, 320, COLOR_DARKGREY);

    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.setCursor(20, 110);
    tft.print("eliminata!");
    delay(400);

    deleteCurrentPhoto(currentGalleryIndex);

    while (digitalRead(BTN_GALLERY_PIN) == LOW) delay(10);
    delay(50);
  } else {
    showGalleryPhoto(currentGalleryIndex);
  }
}

// Timer sferico trasmissione Wi-Fi: Bordi squadrati, sfondo bianco, testo nero, barra NERA SPESSA
void handleCircularWiFiTimer() {
  tft.fillRect(40, 40, 240, 160, ST77XX_WHITE);
  tft.drawRect(40, 40, 240, 160, ST77XX_BLACK);
  tft.drawRect(41, 41, 238, 158, ST77XX_BLACK);

  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(55, 55);
  tft.print("trasmissione?");

  tft.setTextSize(1);
  tft.setCursor(55, 78);
  tft.print("lascia per annullare");

  // Anello guida di sfondo sottile
  drawArcSegment(160, 126, 20, 32, 0, 360, COLOR_LIGHTGREY);

  const unsigned long TIMER_DURATION_MS = 1600;
  unsigned long timerStart = millis();
  float lastAngle = 0.0f;
  bool completed = false;

  while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
    unsigned long elapsed = millis() - timerStart;
    float fraction = (float)elapsed / (float)TIMER_DURATION_MS;
    if (fraction > 1.0f) fraction = 1.0f;

    float currentAngle = fraction * 360.0f;
    if (currentAngle > lastAngle) {
      // Barra circolare nera spessa (raggio da 20 a 32)
      drawArcSegment(160, 126, 20, 32, lastAngle, currentAngle, ST77XX_BLACK);
      lastAngle = currentAngle;
    }

    tft.fillRect(145, 120, 32, 12, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(148, 122);
    tft.printf("%d%%", (int)(fraction * 100));

    if (fraction >= 1.0f) {
      completed = true;
      break;
    }
    delay(15);
  }

  if (completed) {
    while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) delay(10);
    delay(50);

    soundWiFiStart();
    currentMode = MODE_WIFI;
    startWiFi();
    showWiFiScreen();
  } else {
    // Annullamento sicuro: torna subito al mirino live
  }
}

void takeAndSavePhoto() {
  Serial.println(">>> SCATTO FOTO IN CORSO... <<<");

  // 1. Feedback sonoro dell'otturatore e flash bianco (shutter blink)
  soundShutter();
  tft.fillScreen(ST77XX_WHITE);
  delay(40);

  int nextNum = photoList.empty() ? 1 : (photoList.back() + 1);
  String path = getPhotoPath(nextNum);

  // 2. Acquisizione hardware, visualizzazione fermo immagine e salvataggio JPEG
  bool ok = captureAndSaveJPEG(path);

  if (ok) {
    photoList.push_back(nextNum);
    totalPhotos = photoList.size();
    // Breve pausa a elaborazione terminata per apprezzare il fermo prima del ritorno al live feed
    delay(200);
  } else {
    // Segnalazione solo in caso di errore effettivo
    tft.fillRect(45, 95, 230, 50, ST77XX_BLACK);
    tft.drawRect(45, 95, 230, 50, ST77XX_RED);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.setCursor(70, 110);
    tft.print("ERRORE FLASH!");
    delay(1000);
  }
}

// ==============================================================================
// FEEDBACK VISIVO SU SCHERMO (MUTE / UNMUTE) E CONTROLLO COMBINATO TASTI
// ==============================================================================
void showSoundFeedbackScreen(bool enabled) {
  tft.fillScreen(ST77XX_WHITE);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);

  if (enabled) {
    // "suoni attivati" (14 car * 12px = 168px) -> centrato X = (320 - 168)/2 = 76
    tft.setCursor(76, 112);
    tft.print("suoni attivati");
  } else {
    // "suoni disattivati" (17 car * 12px = 204px) -> centrato X = (320 - 204)/2 = 58
    tft.setCursor(58, 112);
    tft.print("suoni disattivati");
  }

  delay(1500);
}

void handleDualButtonToggle() {
  while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW || digitalRead(BTN_GALLERY_PIN) == LOW) {
    delay(10);
  }
  delay(80); // Debounce rilascio

  soundEnabled = !soundEnabled;
  saveSoundSetting(soundEnabled);

  showSoundFeedbackScreen(soundEnabled);

  // Ripristina la visualizzazione precedente
  if (currentMode == MODE_CAMERA) {
    showLiveTransitionScreen();
  } else if (currentMode == MODE_GALLERY) {
    showGalleryPhoto(currentGalleryIndex, true);
  } else if (currentMode == MODE_WIFI) {
    showWiFiScreen();
  }
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) delay(10);
  
  for (int i = 0; i < 8; i++) pinMode(dataPins[i], INPUT_PULLDOWN);
  pinMode(VSY_PIN, INPUT);
  pinMode(WR_PIN, OUTPUT);
  digitalWrite(WR_PIN, LOW); // Blocca immediatamente la scrittura nella FIFO
  pinMode(WRST_PIN, OUTPUT);
  digitalWrite(WRST_PIN, HIGH);
  pinMode(RRST_PIN, OUTPUT);
  digitalWrite(RRST_PIN, HIGH);
  pinMode(STR_PIN, OUTPUT);
  digitalWrite(STR_PIN, HIGH);

  pinMode(BTN_CAMERA_PIN, INPUT_PULLUP);
  pinMode(BTN_GALLERY_PIN, INPUT_PULLUP);
  pinMode(BACKUP_BOOT_PIN, INPUT_PULLUP);

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);
  initSoundSetting();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1); 
  tft.invertDisplay(false);
  tft.fillScreen(ST77XX_BLACK);

  initStorage();

  // Setup I2C OV7670
  Wire.begin(SIOD_PIN, SIOC_PIN);
  Wire.setClock(100000);
  delay(300); // Stabilizzazione alimentazione e oscillatore 24MHz all'avvio a freddo

  // Handshake I2C: attendi che il modulo OV7670 sia pronto e risponda (evita invio a vuoto all'avvio)
  for (int i = 0; i < 20; i++) {
    Wire.beginTransmission(0x21);
    if (Wire.endTransmission() == 0) break;
    delay(50);
  }

  writeRegister(0x12, 0x80); // Reset HW/SW sensore
  delay(250); // Fondamentale: tempo per ricaricare i registri di default dopo il soft reset
  
  byte regs[][2] = {
    {0x12, 0x14}, // COM7: Formato QVGA + Output RGB (Riattiva il colore!)
    {0x40, 0xD0}, // COM15: Formato RGB565
    {0x8C, 0x00}, // Disattiva RGB444
    {0x11, 0x01}, // Clock Prescaler

    // Registri di Scaling Hardware
    {0x0C, 0x04}, // COM3: Abilita Scaling
    {0x3E, 0x19}, // COM14: Scaling PCLK manuale
    {0x70, 0x3A}, // SCALING_XSC
    {0x71, 0x35}, // SCALING_YSC
    {0x72, 0x11}, // SCALING_DCWCTR
    {0x73, 0xF1}, // SCALING_PCLK_DIV
    {0xA2, 0x02}, // SCALING_PCLK_DELAY

    // Orientamento Hardware: Ruota l'immagine di 180° (Flip Verticale + Mirror Orizzontale)
    {0x1E, 0x30}, // MVFP: Bit 5 (Mirror 0x20) | Bit 4 (VFlip 0x10) = 0x30

    // IL TRUCCO DEL RITAGLIO: Forza esattamente 320x240 (Elimina le strisce)
    {0x17, 0x16}, {0x18, 0x04}, {0x32, 0x24}, // Taglio Orizzontale
    {0x19, 0x02}, {0x1A, 0x7A}, {0x03, 0x0A}, // Taglio Verticale

    // Rianimazione dei Colori e del DSP
    {0x13, 0xE5}, // COM8: Abilita AGC (guadagno automatico) e AEC (esposizione automatica)
    {0x14, 0x18}, // COM9: Limita Gain Ceiling a 4x (evita amplificazione e rumore sotto forte luce)
    {0x3B, 0x0A}, // COM11: CRUCIALE! Sblocca l'esposizione al di sotto del limite di banding (anti-whiteout all'aperto)

    // Gestione Auto-Esposizione Hardware (AEC/AGC) e Istogramma Dinamico
    {0x24, 0x75}, // AEW: Soglia superiore regione operativa stabile AEC
    {0x25, 0x63}, // AEB: Soglia inferiore regione operativa stabile AEC
    {0x26, 0xA5}, // VPT: Soglie operative veloci per transizioni repentine luce/ombra
    {0x9F, 0x78}, // HAECC1: Controllo istogramma AEC 1
    {0xA0, 0x68}, // HAECC2: Controllo istogramma AEC 2
    {0xA1, 0x03}, // Controllo fast mode istogramma

    {0x3D, 0xC0}, // COM13: Gamma Curve
    {0x4F, 0x80}, {0x50, 0x80}, {0x51, 0x00}, // Matrice Colori (Saturazione)
    {0x52, 0x22}, {0x53, 0x5E}, {0x54, 0x80},
    {0x55, 0x00}, // BRIGHT: Luminosita' neutra (offset +/-0)
    {0x56, 0x40}, // CONTRAS: Contrasto standard (1.0x)
    {0x42, 0x00}  // COM17: Spegne definitivamente il test pattern/monoscopio
  };

  for (auto& r : regs) {
    writeRegister(r[0], r[1]);
    delay(2);
  }
  Serial.println("Fotocamera Pronta!");
  
  // Schermata di avvio: sfondo bianco, cwmera nero centrato senza riquadro, sottotitolo allineato a sx del titolo
  tft.fillScreen(ST77XX_WHITE);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(4);
  tft.setCursor(88, 90);
  tft.print("cwmera");

  tft.setTextSize(1);
  tft.setCursor(88, 135);
  tft.printf("foto in memoria: %d", totalPhotos);

  soundBoot();
  delay(900);

  // Purga e allineamento iniziale FIFO: esegue due frame a vuoto per stabilizzare clock e bus
  for (int i = 0; i < 2; i++) {
    captureFrameToFIFO();
    resetFIFOReadPointer();
  }
}

// ==============================================================================
// LOOP PRINCIPALE
// ==============================================================================
void loop() {
  bool btnCamera = (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW);
  bool btnGallery = (digitalRead(BTN_GALLERY_PIN) == LOW);

  // Controllo Pressione Simultanea dei due pulsanti (Attiva/Disattiva tutti i suoni)
  if (btnCamera && btnGallery) {
    handleDualButtonToggle();
    return;
  }

  // --------------------------------------------------------------------------
  // MODALITA' TRASMISSIONE WI-FI
  // --------------------------------------------------------------------------
  if (currentMode == MODE_WIFI) {
    dnsServer.processNextRequest();
    server.handleClient();

    // Se l'utente ha richiesto l'aggiornamento OTA online dalla Web App
    if (otaRequested && (millis() - otaRequestedMillis > 400)) {
      otaRequested = false;
      stopWiFi();
      performOnlineOTA(otaTargetSSID, otaTargetPass);
      currentMode = MODE_CAMERA;
      showLiveTransitionScreen();
      return;
    }

    if (btnCamera || btnGallery) {
      if ((digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) && digitalRead(BTN_GALLERY_PIN) == LOW) {
        handleDualButtonToggle();
        return;
      }
      while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW || digitalRead(BTN_GALLERY_PIN) == LOW) delay(10);
      delay(100);

      stopWiFi();
      currentMode = MODE_CAMERA;
      showLiveTransitionScreen();
    }
    delay(2);
    return;
  }

  // --------------------------------------------------------------------------
  // MODALITA' GALLERIA
  // --------------------------------------------------------------------------
  if (currentMode == MODE_GALLERY) {
    // 1. Pulsante in alto (Camera):
    // Se cliccato o tenuto premuto in galleria, torna al feed live SOLO al rilascio.
    // Ignora la trattenuta (non va in modalità di trasmissione).
    if (btnCamera) {
      while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
        if (digitalRead(BTN_GALLERY_PIN) == LOW) {
          handleDualButtonToggle();
          return;
        }
        delay(10);
      }
      delay(50); // debounce rilascio

      currentMode = MODE_CAMERA;
      showLiveTransitionScreen();
      return;
    }

    // 2. Pulsante display (Galleria):
    // Click singolo: scorre le immagini.
    // Pressione prolungata (>= 450ms): avvia il timer di eliminazione.
    if (btnGallery) {
      unsigned long pressStart = millis();
      bool isLongPress = false;

      while (digitalRead(BTN_GALLERY_PIN) == LOW) {
        if (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
          handleDualButtonToggle();
          return;
        }
        if (millis() - pressStart >= 450) {
          isLongPress = true;
          break;
        }
        delay(10);
      }

      if (isLongPress) {
        handleCircularDeleteTimer();
      } else {
        delay(50); // debounce rilascio
        if (totalPhotos > 0) {
          currentGalleryIndex--;
          if (currentGalleryIndex < 1) {
            currentGalleryIndex = totalPhotos;
          }
          showGalleryPhoto(currentGalleryIndex, false);
        }
      }
    }
    delay(20);
    return;
  }

  // --------------------------------------------------------------------------
  // MODALITA' FOTOCAMERA (MIRINO STREAMING LIVE)
  // --------------------------------------------------------------------------
  // 1. Pulsante display (Galleria):
  // Se cliccato o tenuto premuto nel feed live, apre la galleria SOLO al rilascio,
  // ignorando qualsiasi trattenuta prolungata.
  if (btnGallery) {
    while (digitalRead(BTN_GALLERY_PIN) == LOW) {
      if (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
        handleDualButtonToggle();
        return;
      }
      delay(10);
    }
    delay(50); // debounce rilascio

    currentMode = MODE_GALLERY;
    currentGalleryIndex = totalPhotos;
    showGalleryPhoto(currentGalleryIndex);
    return;
  }

  // 2. Pulsante in alto (Camera):
  // Click singolo: scatta subito la foto!
  // Pressione prolungata (>= 450ms): avvia il timer di trasmissione Wi-Fi.
  if (btnCamera) {
    unsigned long pressStart = millis();
    bool isLongPress = false;

    while (digitalRead(BTN_CAMERA_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
      if (digitalRead(BTN_GALLERY_PIN) == LOW) {
        handleDualButtonToggle();
        return;
      }
      if (millis() - pressStart >= 450) {
        isLongPress = true;
        break;
      }
      delay(10);
    }

    if (isLongPress) {
      handleCircularWiFiTimer();
      return;
    } else {
      delay(50); // debounce rilascio
      takeAndSavePhoto();
      return;
    }
  }

  // === ACQUISIZIONE E STREAMING LIVE AL DISPLAY ===
  captureFrameToFIFO();
  resetFIFOReadPointer();

  tft.startWrite();
  tft.setAddrWindow(0, 0, 320, 240);

  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 320; x++) {
      STR_LOW();
      __asm__("nop; nop;");
      byte b1 = readFIFOByteFast();
      STR_HIGH();
      __asm__("nop; nop;");

      STR_LOW();
      __asm__("nop; nop;");
      byte b2 = readFIFOByteFast();
      STR_HIGH();
      __asm__("nop; nop;");

#if BLACK_AND_WHITE
      lineBuffer[x] = rgb565ToGrayscale((b1 << 8) | b2, x, y);
#else
      lineBuffer[x] = (b1 << 8) | b2;
#endif
    }
    tft.writePixels(lineBuffer, 320);
  }
  tft.endWrite();
}
