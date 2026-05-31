#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>

// --- PIN DISPLAY ---
#define TFT_CS    10
#define TFT_RST   9
#define TFT_DC    8
#define TFT_MOSI  11
#define TFT_SCLK  12
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// --- PIN FOTOCAMERA ---
#define SIOD_PIN  4
#define SIOC_PIN  5
#define VSY_PIN   6
#define WR_PIN    15
#define WRST_PIN  16
#define RRST_PIN  17
#define STR_PIN   18

const int dataPins[8] = {1, 2, 3, 7, 13, 14, 21, 47}; 

byte readFIFOByte() {
  byte b = 0;
  if (digitalRead(dataPins[0])) b |= (1 << 0);
  if (digitalRead(dataPins[1])) b |= (1 << 1);
  if (digitalRead(dataPins[2])) b |= (1 << 2);
  if (digitalRead(dataPins[3])) b |= (1 << 3);
  if (digitalRead(dataPins[4])) b |= (1 << 4);
  if (digitalRead(dataPins[5])) b |= (1 << 5);
  if (digitalRead(dataPins[6])) b |= (1 << 6);
  if (digitalRead(dataPins[7])) b |= (1 << 7);
  return b;
}

void writeRegister(byte reg, byte val) {
  Wire.beginTransmission(0x21);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) delay(10);
  
  for (int i = 0; i < 8; i++) pinMode(dataPins[i], INPUT_PULLDOWN);
  pinMode(VSY_PIN, INPUT);
  pinMode(WR_PIN, OUTPUT);
  pinMode(WRST_PIN, OUTPUT);
  pinMode(RRST_PIN, OUTPUT);
  pinMode(STR_PIN, OUTPUT);

  digitalWrite(WR_PIN, LOW); 
  digitalWrite(WRST_PIN, HIGH);
  digitalWrite(RRST_PIN, HIGH);
  digitalWrite(STR_PIN, HIGH);

  // Setup Display
  tft.init(240, 320);
  tft.setRotation(1); 
  tft.invertDisplay(false); // Ricorda: metti 'true' se l'immagine è in "negativo"
  tft.fillScreen(ST77XX_BLACK);

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

  for(auto& r : regs) writeRegister(r[0], r[1]);
  Serial.println("Fotocamera QVGA a Colori Pronta!");
}

void loop() {
  // === FASE 1: ACQUISIZIONE SINCRONIZZATA ===
  
  // 1. Aspettiamo la Pausa della fotocamera (VSYNC = HIGH)
  while(digitalRead(VSY_PIN) == LOW);  
  
  // 2. Resettiamo il puntatore di scrittura proprio DURANTE la pausa (Zero errori all'inizio!)
  digitalWrite(WRST_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(WRST_PIN, HIGH);

  // 3. Aspettiamo che la foto inizi (VSYNC = LOW) e apriamo il rubinetto
  while(digitalRead(VSY_PIN) == HIGH); 
  digitalWrite(WR_PIN, HIGH);          
  
  // 4. Aspettiamo che la foto finisca (VSYNC = HIGH) e chiudiamo
  while(digitalRead(VSY_PIN) == LOW);  
  digitalWrite(WR_PIN, LOW);           

  // === FASE 2: LETTURA E TRASFERIMENTO ===
  
  // Resettiamo il cursore di lettura della memoria
  digitalWrite(RRST_PIN, LOW); delayMicroseconds(5);
  digitalWrite(STR_PIN, LOW); delayMicroseconds(2);
  digitalWrite(STR_PIN, HIGH); delayMicroseconds(2);
  digitalWrite(RRST_PIN, HIGH); delayMicroseconds(5);

  // Forza il display a ricevere un'immagine 320x240
  tft.setAddrWindow(0, 0, 320, 240);
  tft.startWrite();

  for (int i = 0; i < 320 * 240; i++) {
    
    // Byte Alto (Rosso/Verde)
    digitalWrite(STR_PIN, LOW);
    __asm__("nop; nop; nop;");
    byte highByte = readFIFOByte();
    digitalWrite(STR_PIN, HIGH);

    // Byte Basso (Verde/Blu)
    digitalWrite(STR_PIN, LOW);
    __asm__("nop; nop; nop;");
    byte lowByte = readFIFOByte();
    digitalWrite(STR_PIN, HIGH);

    // Unione colore: se i colori sembrano invertiti (es. facce blu), scambia highByte e lowByte!
    uint16_t color = (highByte << 8) | lowByte;
    tft.writeColor(color, 1);
  }

  tft.endWrite();
  Serial.println("Fotogramma visualizzato!");
}