#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <SPI.h>

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
static const int ROT = 1;                       // <-- vertical OK en tu panel

// Layout (480x800 en vertical con ROT=1)
static const int PAD = 14;
static const int HEADER_H = 130;                // cabecera más compacta
static const int NUM_SLIDES = 3;

// Full refresh cada 2 rotaciones completas (2*3=6)
static const int FULL_EVERY_N_CHANGES = 2 * NUM_SLIDES;

// Estado persistente
RTC_DATA_ATTR int slide_idx = 0;
RTC_DATA_ATTR int change_count = 0;

// ------------------ Power ------------------

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

// ------------------ Limpieza fuerte (borra “fantasmas” de orientación previa) ------------------

void hardClear()
{
  // 1) Blanco completo
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());

  // 2) Negro completo (ayuda a “resetear” el pigmento)
  display.firstPage();
  do { display.fillScreen(GxEPD_BLACK); } while (display.nextPage());

  // 3) Blanco completo otra vez
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
}

// ------------------ Dibujo ------------------

void drawHeaderFull()
{
  display.fillRect(0, 0, display.width(), HEADER_H, GxEPD_WHITE);

  // Logo 240 px ancho, alineado a la izquierda
  const int x = PAD;
  const int y = 10;

  // En tu versión de GxEPD2: NO hay drawBitmaps => dibujamos dos veces
  display.drawBitmap(x, y, museu_logo_240_black, MUSEU_LOGO_240_W, MUSEU_LOGO_240_H, GxEPD_BLACK);
  display.drawBitmap(x, y, museu_logo_240_red,   MUSEU_LOGO_240_W, MUSEU_LOGO_240_H, GxEPD_RED);

  // Línea roja separadora (subida para ganar espacio BW)
  const int sep_y = HEADER_H - 12;
  display.fillRect(PAD, sep_y, display.width() - 2 * PAD, 6, GxEPD_RED);
}

// Cuerpo: SOLO B/N para parciales más rápidos y sin degradar rojo
void drawBodyBW(int idx)
{
  const int y0 = HEADER_H;
  const int h  = display.height() - HEADER_H;
  display.fillRect(0, y0, display.width(), h, GxEPD_WHITE);

  const char* title = "";
  const char* subtitle = "";
  const char* bullets[4] = { "", "", "", "" };

  if (idx == 0)
  {
    title = "IBM PC PS/2";
    subtitle = "Familia PS/2 (finals 80)";
    bullets[0] = "- Arquitectura IBM, era VGA/MCGA";
    bullets[1] = "- Micro Channel (MCA) en molts models";
    bullets[2] = "- Clau en la normalitzacio del PC";
    bullets[3] = "- Icona del pas als 90";
  }
  else if (idx == 1)
  {
    title = "Apple II";
    subtitle = "Microordinador pioner (1977+)";
    bullets[0] = "- Un dels primers exits domestics";
    bullets[1] = "- Graficos i expansio per ranures";
    bullets[2] = "- Molt usat en educacio";
    bullets[3] = "- Ecosistema de software enorme";
  }
  else
  {
    title = "Commodore PET";
    subtitle = "All-in-one (1977)";
    bullets[0] = "- Integrat: monitor + teclat";
    bullets[1] = "- Popular en aules i laboratoris";
    bullets[2] = "- BASIC resident, enfoc professional";
    bullets[3] = "- Disseny iconic: \"PET\"";
  }

  display.setFont(&FreeMonoBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(PAD, y0 + 70);
  display.print(title);

  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(PAD, y0 + 105);
  display.print(subtitle);

  int y = y0 + 150;
  for (int i = 0; i < 4; i++)
  {
    display.setCursor(PAD, y);
    display.print(bullets[i]);
    y += 34;
  }
}

void fullDraw(int idx)
{
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeaderFull();
    drawBodyBW(idx);
  } while (display.nextPage());
}

void partialBodyUpdate(int idx)
{
  const int y0 = HEADER_H;
  const int h  = display.height() - HEADER_H;

  if (h <= 0) { fullDraw(idx); return; }

  display.setPartialWindow(0, y0, display.width(), h);
  display.firstPage();
  do {
    drawBodyBW(idx);
  } while (display.nextPage());
}

// ------------------ Setup ------------------

void setup()
{
  powerOnEPD();

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(ROT);

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
    // Primer arranque / tras reprogramar: limpiar fuerte para borrar restos del modo anterior
    hardClear();
    doFull = true;
  }

  if (doFull) fullDraw(slide_idx);
  else        partialBodyUpdate(slide_idx);

  powerOffEPD();
  goDeepSleep();
}

void loop() {}
