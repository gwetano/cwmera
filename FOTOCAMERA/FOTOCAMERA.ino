#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <vector>
#include <algorithm>
#include "esp_camera.h"
#include "img_converters.h"
#include "soc/gpio_reg.h"

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

// --- PULSANTE MULTIFUNZIONE ---
#define BUTTON_PIN      38
#define BACKUP_BOOT_PIN 0

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

void writeRegister(byte reg, byte val) {
  Wire.beginTransmission(0x21);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
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
  while(digitalRead(VSY_PIN) == LOW);
  digitalWrite(WRST_PIN, LOW); delayMicroseconds(5); digitalWrite(WRST_PIN, HIGH);
  while(digitalRead(VSY_PIN) == HIGH); digitalWrite(WR_PIN, HIGH);
  while(digitalRead(VSY_PIN) == LOW);  digitalWrite(WR_PIN, LOW);

  digitalWrite(RRST_PIN, LOW); delayMicroseconds(5);
  digitalWrite(STR_PIN, LOW); delayMicroseconds(2);
  digitalWrite(STR_PIN, HIGH); delayMicroseconds(2);
  digitalWrite(RRST_PIN, HIGH); delayMicroseconds(5);

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

bool displayBMP(const String &path) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;

  uint8_t header[54];
  if (f.read(header, 54) != 54 || header[0] != 'B' || header[1] != 'M') {
    f.close();
    return false;
  }

  tft.startWrite();
  tft.setAddrWindow(0, 0, 320, 240);

  uint8_t rowBytes[320 * 3];
  uint16_t rowPixels[320];

  for (int y = 0; y < 240; y++) {
    f.read(rowBytes, 320 * 3);
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
  while(digitalRead(VSY_PIN) == LOW);
  digitalWrite(WRST_PIN, LOW); delayMicroseconds(5); digitalWrite(WRST_PIN, HIGH);
  while(digitalRead(VSY_PIN) == HIGH); digitalWrite(WR_PIN, HIGH);
  while(digitalRead(VSY_PIN) == LOW);  digitalWrite(WR_PIN, LOW);

  digitalWrite(RRST_PIN, LOW); delayMicroseconds(5);
  digitalWrite(STR_PIN, LOW); delayMicroseconds(2);
  digitalWrite(STR_PIN, HIGH); delayMicroseconds(2);
  digitalWrite(RRST_PIN, HIGH); delayMicroseconds(5);

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

      frameBuf[idx++] = b1;
      frameBuf[idx++] = b2;
    }
  }

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

bool displayJPEG(const String &path) {
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

  tft.startWrite();
  tft.setAddrWindow(0, 0, 320, 240);
  uint16_t rowPixels[320];

  for (int y = 0; y < 240; y++) {
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

bool displayPhoto(const String &path) {
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    return displayJPEG(path);
  }
  return displayBMP(path);
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
  html += "body{font-family:-apple-system,sans-serif;background:#fff;color:#000;padding:20px;text-align:left;}";
  html += "h1{font-size:26px;font-weight:800;margin-bottom:6px;}";
  html += ".sub{font-size:13px;color:#666;margin-bottom:20px;}";
  html += ".box{border:1px solid #000;padding:16px;margin-bottom:20px;}";
  html += ".step{font-size:14px;margin-bottom:12px;line-height:1.4;}";
  html += ".btn{display:block;width:100%;background:#000;color:#fff;text-align:center;padding:14px;font-size:15px;font-weight:700;text-decoration:none;border:1px solid #000;}";
  html += "</style></head><body>";
  html += "<h1>cwmera</h1>";
  html += "<div class='sub'>trasmissione foto</div>";
  html += "<div class='box'>";
  html += "<div class='step'>1. tocca <strong>'Fine'</strong> in alto a destra e scegli <strong>'Usa senza Internet'</strong></div>";
  html += "<div class='step'>2. apri <strong>Safari</strong> su <strong>192.168.4.1</strong> per scaricare le foto</div>";
  html += "</div>";
  html += "<a href='http://192.168.4.1/' class='btn'>apri in safari</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleGalleryPage() {
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
  html += ".v-switch{display:flex;gap:6px;}";
  html += ".v-btn{border:1px solid #000;background:#fff;color:#000;padding:6px 10px;font-size:12px;font-weight:700;cursor:pointer;}";
  html += ".v-btn.act{background:#000;color:#fff;}";
  html += ".grid-mode{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
  html += ".list-mode{display:flex;flex-direction:column;gap:14px;}";
  html += ".card{border:1px solid #000;background:#fff;padding:6px;}";
  html += ".card img{width:100%;height:auto;display:block;margin-bottom:6px;cursor:pointer;border:1px solid #eee;}";
  html += ".actions{display:flex;gap:6px;}";
  html += ".btn-dl{flex:4;display:flex;align-items:center;justify-content:center;gap:6px;background:#000;color:#fff;border:1px solid #000;padding:10px 4px;font-size:13px;font-weight:700;text-decoration:none;cursor:pointer;}";
  html += ".btn-del{flex:1;display:flex;align-items:center;justify-content:center;background:#fff;color:#000;border:1px solid #999;padding:10px 4px;cursor:pointer;}";
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
  html += "<div><div class='brand'>cwmera</div><div class='count'>foto in memoria: <span id='c-val'></span></div></div>";
  html += "<div class='v-switch'>";
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
  html += "<a id='lb-dl' class='btn-dl' href=''>";
  html += "<svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.5'><path d='M12 3v13M6 11l6 6 6-6M3 20h18'/></svg> salva</a>";
  html += "<button id='lb-del' class='btn-del' title='elimina'>";
  html += "<svg width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='#222' stroke-width='2'><path d='M3 6h18M8 6V4h8v2M19 6v14H5V6M10 11v6M14 11v6'/></svg></button>";
  html += "</div></div>";

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
  html += "h+='<a href=\"/download?id='+n+'\" class=\"btn-dl\">';";
  html += "h+='<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#fff\" stroke-width=\"2.5\"><path d=\"M12 3v13M6 11l6 6 6-6M3 20h18\"/></svg> salva</a>';";
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

// ==============================================================================
// LOGICA GALLERIA ED INTERFACCIA A SCHERMO (STILE ORIGINARIO FOTOCAMERA)
// ==============================================================================
void showGalleryPhoto(int index) {
  if (totalPhotos == 0 || photoList.empty()) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(65, 90);
    tft.print("gwlleria vuota");
    
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(45, 130);
    tft.print("non hai scattato foto");
    tft.setCursor(35, 150);
    tft.print("doppio click per tornare a scattare");
    return;
  }

  if (index < 1) index = 1;
  if (index > totalPhotos) index = totalPhotos;

  int fileNum = photoList[index - 1];
  String path = getPhotoPath(fileNum);
  displayPhoto(path);

  // Barra Superiore
  tft.fillRect(0, 0, 320, 22, ST77XX_BLACK);
  tft.drawFastHLine(0, 22, 320, COLOR_DARKGREY);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 4);
  tft.printf("gwlleria [%d/%d]", index, totalPhotos);

  // Barra Inferiore con istruzioni
  tft.fillRect(0, 218, 320, 22, ST77XX_BLACK);
  tft.drawFastHLine(0, 218, 320, COLOR_DARKGREY);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(12, 225);
  tft.print("doppio click per tornare a scattare\ntieni premuto per eliminare");
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

// Timer sferico di eliminazione su pressione prolungata in galleria (squadrato, senza bordo laterale, sfondo bianco, barra spessa)
void handleCircularDeleteTimer() {
  if (totalPhotos == 0 || photoList.empty()) return;

  const uint16_t COLOR_DARKRED = 0xB000;

  // Box squadrato a tutta larghezza (senza bordo laterale, solo linee orizzontali)
  tft.fillRect(0, 40, 320, 160, ST77XX_WHITE);
  tft.drawFastHLine(0, 40, 320, ST77XX_BLACK);
  tft.drawFastHLine(0, 41, 320, ST77XX_BLACK);
  tft.drawFastHLine(0, 198, 320, ST77XX_BLACK);
  tft.drawFastHLine(0, 199, 320, ST77XX_BLACK);

  // Titolo elimina in rosso scuro leggibile su sfondo bianco
  tft.setTextColor(COLOR_DARKRED);
  tft.setTextSize(2);
  tft.setCursor(25, 55);
  tft.print("elimina?");

  // Sottotitolo posizionato sotto la scritta elimina
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(25, 78);
  tft.print("rilascia per annullare");

  // Anello guida di sfondo sottile
  drawArcSegment(160, 126, 20, 32, 0, 360, COLOR_LIGHTGREY);

  const unsigned long TIMER_DURATION_MS = 1600;
  unsigned long timerStart = millis();
  float lastAngle = 0.0f;
  bool completed = false;

  while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
    unsigned long elapsed = millis() - timerStart;
    float fraction = (float)elapsed / (float)TIMER_DURATION_MS;
    if (fraction > 1.0f) fraction = 1.0f;

    float currentAngle = fraction * 360.0f;
    if (currentAngle > lastAngle) {
      // Barra circolare spessa (raggio 20-32) in rosso scuro leggibile
      drawArcSegment(160, 126, 20, 32, lastAngle, currentAngle, COLOR_DARKRED);
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
    tft.fillRect(0, 40, 320, 160, ST77XX_WHITE);
    tft.drawFastHLine(0, 40, 320, ST77XX_BLACK);
    tft.drawFastHLine(0, 41, 320, ST77XX_BLACK);
    tft.drawFastHLine(0, 198, 320, ST77XX_BLACK);
    tft.drawFastHLine(0, 199, 320, ST77XX_BLACK);
    tft.setTextColor(COLOR_DARKRED);
    tft.setTextSize(2);
    tft.setCursor(25, 105);
    tft.print("eliminata!");
    delay(800);

    deleteCurrentPhoto(currentGalleryIndex);

    while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) delay(10);
    delay(100);
  } else {
    showGalleryPhoto(currentGalleryIndex);
  }
}

// Timer sferico trasmissione Wi-Fi: Bordi squadrati, sfondo bianco, testo nero, barra CIANO SPESSA
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

  while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
    unsigned long elapsed = millis() - timerStart;
    float fraction = (float)elapsed / (float)TIMER_DURATION_MS;
    if (fraction > 1.0f) fraction = 1.0f;

    float currentAngle = fraction * 360.0f;
    if (currentAngle > lastAngle) {
      // Barra Ciano spessa (raggio da 20 a 32 = 13 pixel di spessore!)
      drawArcSegment(160, 126, 20, 32, lastAngle, currentAngle, ST77XX_CYAN);
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
    tft.fillRect(40, 40, 240, 160, ST77XX_WHITE);
    tft.drawRect(40, 40, 240, 160, ST77XX_BLACK);
    tft.drawRect(41, 41, 238, 158, ST77XX_BLACK);
    tft.setTextColor(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(55, 105);
    tft.print("attivato!");
    delay(800);

    while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) delay(10);
    delay(100);

    currentMode = MODE_WIFI;
    startWiFi();
    showWiFiScreen();
  } else {
    // Annullamento sicuro: torna subito al mirino live
  }
}

void takeAndSavePhoto() {
  Serial.println(">>> SCATTO FOTO IN CORSO... <<<");

  int nextNum = photoList.empty() ? 1 : (photoList.back() + 1);
  String path = getPhotoPath(nextNum);

  tft.drawRect(0, 0, 320, 240, ST77XX_WHITE);
  tft.drawRect(1, 1, 318, 238, ST77XX_WHITE);

  tft.fillRect(45, 95, 230, 50, ST77XX_BLACK);
  tft.drawRect(45, 95, 230, 50, ST77XX_YELLOW);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(65, 110);
  tft.print("elwborando...");

  bool ok = captureAndSaveJPEG(path);

  if (ok) {
    photoList.push_back(nextNum);
    totalPhotos = photoList.size();

    tft.fillRect(45, 95, 230, 50, ST77XX_BLACK);
    tft.drawRect(45, 95, 230, 50, ST77XX_GREEN);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(2);
    tft.setCursor(75, 105);
    tft.print("FOTO SALVATA!");
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(95, 128);
    tft.printf("#%04d (%d Totali)", nextNum, totalPhotos);
    delay(1000);
  } else {
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
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) delay(10);
  
  for (int i = 0; i < 8; i++) pinMode(dataPins[i], INPUT_PULLDOWN);
  pinMode(VSY_PIN, INPUT);
  pinMode(WR_PIN, OUTPUT);
  pinMode(WRST_PIN, OUTPUT);
  pinMode(RRST_PIN, OUTPUT);
  pinMode(STR_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BACKUP_BOOT_PIN, INPUT_PULLUP);

  digitalWrite(WR_PIN, LOW); 
  digitalWrite(WRST_PIN, HIGH);
  digitalWrite(RRST_PIN, HIGH);
  digitalWrite(STR_PIN, HIGH);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1); 
  tft.invertDisplay(false);
  tft.fillScreen(ST77XX_BLACK);

  initStorage();

  // Setup I2C OV7670
  Wire.begin(SIOD_PIN, SIOC_PIN);
  delay(100);
  writeRegister(0x12, 0x80); // Reset HW
  delay(100);
  
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

    // IL TRUCCO DEL RITAGLIO: Forza esattamente 320x240 (Elimina le strisce)
    {0x17, 0x16}, {0x18, 0x04}, {0x32, 0x24}, // Taglio Orizzontale
    {0x19, 0x02}, {0x1A, 0x7A}, {0x03, 0x0A}, // Taglio Verticale

    // Rianimazione dei Colori e del DSP
    {0x13, 0xE5}, // COM8: Abilita Esposizione e Bilanciamento Bianco
    {0x14, 0x38}, // COM9: Limite luminosità
    {0x3D, 0xC0}, // COM13: Gamma Curve
    {0x4F, 0x80}, {0x50, 0x80}, {0x51, 0x00}, // Matrice Colori (Saturazione)
    {0x52, 0x22}, {0x53, 0x5E}, {0x54, 0x80},
    {0x42, 0x00}  // COM17: Spegne definitivamente il test pattern/monoscopio
  };

  for (auto& r : regs) writeRegister(r[0], r[1]);
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
  delay(1200);
}

// ==============================================================================
// LOOP PRINCIPALE
// ==============================================================================
void loop() {
  bool btnPressed = (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW);

  // --------------------------------------------------------------------------
  // MODALITA' TRASMISSIONE WI-FI
  // --------------------------------------------------------------------------
  if (currentMode == MODE_WIFI) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (btnPressed) {
      while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) delay(10);
      delay(100);

      stopWiFi();
      currentMode = MODE_CAMERA;
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(2);
      tft.setCursor(75, 110);
      tft.print("cwmera");
      delay(300);
    }
    delay(2);
    return;
  }

  // --------------------------------------------------------------------------
  // MODALITA' GALLERIA
  // --------------------------------------------------------------------------
  if (currentMode == MODE_GALLERY) {
    if (btnPressed) {
      unsigned long pressStart = millis();
      bool isLongPress = false;

      while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
        if (millis() - pressStart >= 450) {
          isLongPress = true;
          break;
        }
        delay(10);
      }

      if (isLongPress) {
        handleCircularDeleteTimer();
      } else {
        unsigned long releaseTime = millis();
        bool isDoubleClick = false;
        while (millis() - releaseTime < 450) {
          if (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
            isDoubleClick = true;
            while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) delay(10);
            delay(100);
            break;
          }
          delay(10);
        }

        if (isDoubleClick) {
          currentMode = MODE_CAMERA;
          tft.fillScreen(ST77XX_BLACK);
          tft.setTextColor(ST77XX_GREEN);
          tft.setTextSize(2);
          tft.setCursor(75, 110);
          tft.print("cwmera");
          delay(300);
        } else {
          if (totalPhotos > 0) {
            currentGalleryIndex--;
            if (currentGalleryIndex < 1) {
              currentGalleryIndex = totalPhotos;
            }
            showGalleryPhoto(currentGalleryIndex);
          }
        }
      }
    }
    delay(20);
    return;
  }

  // --------------------------------------------------------------------------
  // MODALITA' FOTOCAMERA (MIRINO STREAMING LIVE)
  // --------------------------------------------------------------------------
  if (btnPressed) {
    unsigned long pressStart = millis();
    bool isLongPress = false;

    while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
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
      unsigned long releaseTime = millis();
      bool isDoubleClick = false;
      while (millis() - releaseTime < 450) {
        if (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) {
          isDoubleClick = true;
          while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BACKUP_BOOT_PIN) == LOW) delay(10);
          delay(100);
          break;
        }
        delay(10);
      }

      if (isDoubleClick) {
        currentMode = MODE_GALLERY;
        currentGalleryIndex = totalPhotos;
        showGalleryPhoto(currentGalleryIndex);
        return;
      } else {
        takeAndSavePhoto();
        return;
      }
    }
  }

  // === ACQUISIZIONE E STREAMING LIVE AL DISPLAY ===
  while(digitalRead(VSY_PIN) == LOW);  
  digitalWrite(WRST_PIN, LOW); delayMicroseconds(5); digitalWrite(WRST_PIN, HIGH);
  while(digitalRead(VSY_PIN) == HIGH); digitalWrite(WR_PIN, HIGH);          
  while(digitalRead(VSY_PIN) == LOW);  digitalWrite(WR_PIN, LOW);           

  digitalWrite(RRST_PIN, LOW); delayMicroseconds(5);
  digitalWrite(STR_PIN, LOW); delayMicroseconds(2);
  digitalWrite(STR_PIN, HIGH); delayMicroseconds(2);
  digitalWrite(RRST_PIN, HIGH); delayMicroseconds(5);

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

      lineBuffer[x] = (b1 << 8) | b2;
    }
    tft.writePixels(lineBuffer, 320);
  }
  tft.endWrite();
}
