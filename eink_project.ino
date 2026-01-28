#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <SPI.h>

#include <FS.h>
#include <LittleFS.h>

#include "museu_logo_tricolor_240w.h"

#include "esp_sleep.h" 
#include "driver/rtc_io.h"

// ===== Pines e-Paper -> ESP32 =====
#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4
#define EPD_SCK  18
#define EPD_MOSI 23

// PWR del HAT conectado a GPIO 27 (RTC GPIO)
#define EPD_PWR 27

// ===== Driver TRICOLOR =====
GxEPD2_3C<GxEPD2_750c_Z08, GxEPD2_750c_Z08::HEIGHT> display(
  GxEPD2_750c_Z08(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ===== Parámetros fáciles de tocar =====
static const uint32_t SLIDE_MS = 30UL * 1000UL; // periodo entre slides
static const int ROT = 1;                       // vertical OK en tu panel

// Layout 480x800
static const int PAD = 14;
static const int HEADER_H = 130;                // cabecera compacta
static int NUM_SLIDES = 0;                      // se determina al contar slides

// Full refresh cada 2 rotaciones completas
static int FULL_EVERY_N_CHANGES = 2 * 3;       // valor por defecto (actualizado si NUM_SLIDES > 0)

// Estado persistente
RTC_DATA_ATTR int slide_idx = 0;
RTC_DATA_ATTR int change_count = 0;

// ------------------------------------------------------------
// Power
// ------------------------------------------------------------

void powerOnEPD()
{
  rtc_gpio_hold_dis((gpio_num_t)EPD_PWR);

  rtc_gpio_init((gpio_num_t)EPD_PWR);
  rtc_gpio_set_direction((gpio_num_t)EPD_PWR, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)EPD_PWR, 1);

  rtc_gpio_hold_en((gpio_num_t)EPD_PWR);
  gpio_deep_sleep_hold_en();

  delay(30);
}

void powerOffEPD()
{
  display.hibernate();
  SPI.end();

  pinMode(EPD_CS, INPUT);
  pinMode(EPD_DC, INPUT);
  pinMode(EPD_RST, INPUT);
  pinMode(EPD_BUSY, INPUT);
  pinMode(EPD_SCK, INPUT);
  pinMode(EPD_MOSI, INPUT);

  rtc_gpio_hold_dis((gpio_num_t)EPD_PWR);
  rtc_gpio_set_level((gpio_num_t)EPD_PWR, 0);
  rtc_gpio_hold_en((gpio_num_t)EPD_PWR);
  gpio_deep_sleep_hold_en();
}

void goDeepSleep()
{
  esp_sleep_enable_timer_wakeup((uint64_t)SLIDE_MS * 1000ULL);
  esp_deep_sleep_start();
}

// ------------------------------------------------------------
// Utilidades TRI
// Formato: "TRI1" + uint16 w + uint16 h (LE) + black_plane + red_plane
// ------------------------------------------------------------

static uint16_t read_u16_le(File& f)
{
  uint8_t b0 = f.read();
  uint8_t b1 = f.read();
  return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

// Dibuja un .tri de 480x670 en (x,y) usando poca RAM (fila a fila)
bool drawTriFromLittleFS(const char* path, int x, int y)
{
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.println("ERR open TRI"); return false; }

  char magic[4];
  if (f.readBytes(magic, 4) != 4 || memcmp(magic, "TRI1", 4) != 0)
  {
    Serial.println("ERR TRI magic");
    f.close();
    return false;
  }

  uint16_t w = read_u16_le(f);
  uint16_t h = read_u16_le(f);

  const uint32_t bytes_per_row = (w + 7) / 8;
  const uint32_t plane_size = bytes_per_row * h;

  if (w == 0 || h == 0) { Serial.println("ERR TRI size"); f.close(); return false; }

  // Esperado para tu cuerpo: 480x670 (con HEADER_H=130)
  if (w != 480 || h != (uint16_t)(display.height() - HEADER_H))
  {
    Serial.printf("WARN TRI dims %ux%u (expected 480x%d)\n", w, h, display.height() - HEADER_H);
  }

  const uint32_t black_off = 8;
  const uint32_t red_off   = 8 + plane_size;

  // buffer de 1 fila (480px -> 60 bytes). Si el TRI tiene otro ancho, ajusta esto.
  uint8_t rowbuf[60];
  if (bytes_per_row > sizeof(rowbuf))
  {
    Serial.println("ERR TRI rowbuf too small");
    f.close();
    return false;
  }

  // Plano negro
  f.seek(black_off, SeekSet);
  for (uint16_t yy = 0; yy < h; yy++)
  {
    if (f.read(rowbuf, bytes_per_row) != (int)bytes_per_row)
    {
      Serial.println("ERR TRI read black");
      f.close();
      return false;
    }
    display.drawBitmap(x, y + yy, rowbuf, w, 1, GxEPD_BLACK);
  }

  // Plano rojo
  /*f.seek(red_off, SeekSet);
  for (uint16_t yy = 0; yy < h; yy++)
  {
    if (f.read(rowbuf, bytes_per_row) != (int)bytes_per_row)
    {
      Serial.println("ERR TRI read red");
      f.close();
      return false;
    }
    display.drawBitmap(x, y + yy, rowbuf, w, 1, GxEPD_RED);
  }*/

  f.close();
  return true;
}

// ------------------------------------------------------------
// Limpieza fuerte para borrar “fantasmas” tras cambios de orientación
// ------------------------------------------------------------

void hardClear()
{
  display.setFullWindow();

  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());

  display.firstPage();
  do { display.fillScreen(GxEPD_BLACK); } while (display.nextPage());

  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
}

// ------------------------------------------------------------
// Dibujo cabecera / cuerpo
// ------------------------------------------------------------

void drawHeaderFull()
{
  display.fillRect(0, 0, display.width(), HEADER_H, GxEPD_WHITE);

  // Logo 240 px ancho, alineado a la izquierda
  const int x = PAD;
  const int y = 10;

  display.drawBitmap(x, y, museu_logo_240_black, MUSEU_LOGO_240_W, MUSEU_LOGO_240_H, GxEPD_BLACK);
  display.drawBitmap(x, y, museu_logo_240_red,   MUSEU_LOGO_240_W, MUSEU_LOGO_240_H, GxEPD_RED);

  // Línea roja separadora (arriba para ganar espacio BW)
  const int sep_y = HEADER_H - 12;
  display.fillRect(PAD, sep_y, display.width() - 2 * PAD, 6, GxEPD_RED);
}

// Cuerpo: carga slide TRI dinámicamente desde /slides/slideNN.tri
void drawBody(int idx)
{
  const int y0 = HEADER_H;
  const int h  = display.height() - HEADER_H;

  // Limpiar cuerpo
  display.fillRect(0, y0, display.width(), h, GxEPD_WHITE);

  // Construir nombre del archivo: slideNN.tri
  char filepath[32];
  snprintf(filepath, sizeof(filepath), "/slides/slide%02d.tri", idx);

  bool ok = drawTriFromLittleFS(filepath, 0, y0);
  if (!ok)
  {
    display.setFont(&FreeMonoBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(PAD, y0 + 60);
    display.print("Missing");
    display.setCursor(PAD, y0 + 100);
    display.print(filepath);
  }
}

// ------------------------------------------------------------
// Full / Partial
// ------------------------------------------------------------

void fullDraw(int idx)
{
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeaderFull();
    drawBody(idx);
  } while (display.nextPage());
}

void partialBodyUpdate(int idx)
{
  const int y0 = HEADER_H;
  const int h  = display.height() - HEADER_H;
  if (h <= 0) { fullDraw(idx); return; }

  // Ventana parcial SOLO cuerpo
  display.setPartialWindow(0, y0, display.width(), h);

  display.firstPage();
  do {
    drawBody(idx);
  } while (display.nextPage());
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(200);

  // Mount LittleFS temprano para contar slides
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount FAILED");
    return;
  }
  Serial.println("LittleFS mounted OK");

  // Contar archivos slide00.tri, slide01.tri, etc.
  int slideCount = 0;
  for (int i = 0; i < 100; i++) {
    char filepath[32];
    snprintf(filepath, sizeof(filepath), "/slides/slide%02d.tri", i);
    if (LittleFS.exists(filepath)) {
      Serial.printf("Found: %s\n", filepath);
      slideCount++;
    } else {
      break; // asume que no hay más slides después del primero que falta
    }
  }

  if (slideCount == 0) {
    Serial.println("No slides found in /slides/slide00.tri, slide01.tri, ...");
    return;
  }

  NUM_SLIDES = slideCount;
  FULL_EVERY_N_CHANGES = 2 * NUM_SLIDES;
  Serial.printf("Found %d slides, FULL_EVERY_N_CHANGES=%d\n", NUM_SLIDES, FULL_EVERY_N_CHANGES);

  powerOnEPD();

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(ROT);

  // Mount LittleFS (formatea si falla)
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount FAILED");
  } else {
    Serial.println("LittleFS mounted OK");
  }

  const auto cause = esp_sleep_get_wakeup_cause();
  bool doFull = false;

  if (cause == ESP_SLEEP_WAKEUP_TIMER)
  {
    slide_idx = (slide_idx + 1) % NUM_SLIDES;
    change_count++;
    doFull = (change_count % FULL_EVERY_N_CHANGES == 0);
  }
  else
  {
    // Primer arranque / tras reprogramar: limpiar fuerte para borrar restos previos
    hardClear();
    doFull = true;
  }

  if (doFull) fullDraw(slide_idx);
  else        partialBodyUpdate(slide_idx);

  powerOffEPD();
  goDeepSleep();
}

void loop() {}
