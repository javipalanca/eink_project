#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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

// ===== Parámetros a modificar =====
static const uint32_t SLIDE_MS = 30UL * 1000UL; // periodo entre slides
static const int ROT = 1;                       // vertical OK en tu panel

static const char* WIFI_SSID = "TU_WIFI_SSID";
static const char* WIFI_PASS = "TU_WIFI_PASS";
static const char* API_BASE_URL = "http://TU_API";
static const char* CABINET_ID = "vitrina-01";
static const uint32_t UPDATE_CHECK_EVERY_HOURS = 6;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 12000;
static const uint32_t HTTP_TIMEOUT_MS = 12000;
static const bool ENABLE_DEBUG_SERIAL = false;

// Layout 480x800
static const int PAD = 140; //14
static const int HEADER_H = 100; //130;                // cabecera compacta
static int NUM_SLIDES = 0;                      // se determina al contar slides

// Full refresh cada 2 rotaciones completas
static int FULL_EVERY_N_CHANGES = 1;       // valor por defecto (actualizado si NUM_SLIDES > 0)

// Estado persistente
RTC_DATA_ATTR int slide_idx = 0;
RTC_DATA_ATTR int change_count = 0;
RTC_DATA_ATTR uint32_t wake_cycle_count = 0;
RTC_DATA_ATTR char cabinet_revision[80] = "";

// ------------------------------------------------------------
// Red / API updates
// ------------------------------------------------------------

uint32_t calcUpdatePeriodCycles()
{
  const uint64_t updateMs = (uint64_t)UPDATE_CHECK_EVERY_HOURS * 3600ULL * 1000ULL;
  uint32_t cycles = (uint32_t)((updateMs + SLIDE_MS - 1) / SLIDE_MS);
  if (cycles == 0) cycles = 1;
  return cycles;
}

String trimQuotes(const String& value)
{
  if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"')
  {
    return value.substring(1, value.length() - 1);
  }
  return value;
}

void setCabinetRevision(const String& revision)
{
  memset(cabinet_revision, 0, sizeof(cabinet_revision));
  revision.substring(0, sizeof(cabinet_revision) - 1).toCharArray(cabinet_revision, sizeof(cabinet_revision));
}

String getCabinetRevision()
{
  return String(cabinet_revision);
}

String urlEncode(const String& input)
{
  String encoded;
  encoded.reserve(input.length() * 3);
  static const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < input.length(); i++)
  {
    const uint8_t c = (uint8_t)input[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
    {
      encoded += (char)c;
    }
    else
    {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

bool connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS)
  {
    yield();
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi connect failed");
    return false;
  }

  Serial.print("WiFi connected IP=");
  Serial.println(WiFi.localIP());
  return true;
}

void disconnectWiFi()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool saveTriToFile(HTTPClient& http, const String& path)
{
  File out = LittleFS.open(path, "w");
  if (!out)
  {
    Serial.printf("ERR open write %s\n", path.c_str());
    return false;
  }

  int written = http.writeToStream(&out);
  out.close();

  if (written <= 0)
  {
    Serial.printf("ERR write stream %s code=%d\n", path.c_str(), written);
    LittleFS.remove(path);
    return false;
  }

  return true;
}

bool readSlideMeta(int idx, String& cardId, String& etag)
{
  char metaPath[36];
  snprintf(metaPath, sizeof(metaPath), "/slides/slide%02d.meta", idx);

  if (!LittleFS.exists(metaPath)) return false;

  File f = LittleFS.open(metaPath, "r");
  if (!f) return false;

  cardId = f.readStringUntil('\n');
  cardId.trim();
  etag = f.readStringUntil('\n');
  etag.trim();
  f.close();
  return !cardId.isEmpty();
}

bool writeSlideMeta(int idx, const String& cardId, const String& etag)
{
  char metaPath[36];
  snprintf(metaPath, sizeof(metaPath), "/slides/slide%02d.meta", idx);

  File f = LittleFS.open(metaPath, "w");
  if (!f) return false;

  f.println(cardId);
  f.println(etag);
  f.close();
  return true;
}

String getCardId(const JsonVariantConst& card)
{
  const char* c1 = card["card_id"].as<const char*>();
  if (c1 && c1[0]) return String(c1);

  const char* c2 = card["id"].as<const char*>();
  if (c2 && c2[0]) return String(c2);

  return String();
}

bool downloadCardTri(const String& cardId, int idx)
{
  String knownCardId;
  String knownEtag;
  bool hasMeta = readSlideMeta(idx, knownCardId, knownEtag);

  HTTPClient http;
  String url = String(API_BASE_URL) + "/api/device/cabinets/" + urlEncode(CABINET_ID) + "/cards/" + urlEncode(cardId) + ".tri";
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url))
  {
    Serial.printf("ERR HTTP begin card %s\n", cardId.c_str());
    return false;
  }

  if (hasMeta && knownCardId == cardId && !knownEtag.isEmpty())
  {
    http.addHeader("If-None-Match", String("\"") + knownEtag + "\"");
  }

  int code = http.GET();
  String cardEtag = trimQuotes(http.header("ETag"));

  if (code == HTTP_CODE_NOT_MODIFIED)
  {
    if (cardEtag.isEmpty()) cardEtag = knownEtag;
    writeSlideMeta(idx, cardId, cardEtag);
    http.end();
    Serial.printf("Not modified %s (idx=%d)\n", cardId.c_str(), idx);
    return true;
  }

  if (code != HTTP_CODE_OK)
  {
    Serial.printf("ERR card GET %s code=%d\n", cardId.c_str(), code);
    http.end();
    return false;
  }

  char finalPath[32];
  char tempPath[40];
  snprintf(finalPath, sizeof(finalPath), "/slides/slide%02d.tri", idx);
  snprintf(tempPath, sizeof(tempPath), "/slides/.slide%02d.tmp", idx);

  if (!saveTriToFile(http, String(tempPath)))
  {
    http.end();
    return false;
  }

  LittleFS.remove(finalPath);
  bool renamed = LittleFS.rename(tempPath, finalPath);
  if (!renamed)
  {
    Serial.printf("ERR rename %s -> %s\n", tempPath, finalPath);
    LittleFS.remove(tempPath);
    http.end();
    return false;
  }

  http.end();
  writeSlideMeta(idx, cardId, cardEtag);
  Serial.printf("Downloaded %s -> %s\n", cardId.c_str(), finalPath);
  return true;
}

int countSlidesInFS()
{
  int slideCount = 0;
  for (int i = 0; i < 100; i++)
  {
    char filepath[32];
    snprintf(filepath, sizeof(filepath), "/slides/slide%02d.tri", i);
    if (LittleFS.exists(filepath)) slideCount++;
    else break;
  }
  return slideCount;
}

void removeSlidesFrom(int fromIdx)
{
  for (int i = fromIdx; i < 100; i++)
  {
    char filepath[32];
    snprintf(filepath, sizeof(filepath), "/slides/slide%02d.tri", i);

    char metaPath[36];
    snprintf(metaPath, sizeof(metaPath), "/slides/slide%02d.meta", i);

    bool existsTri = LittleFS.exists(filepath);
    bool existsMeta = LittleFS.exists(metaPath);
    if (!existsTri && !existsMeta) break;

    if (existsTri) LittleFS.remove(filepath);
    if (existsMeta) LittleFS.remove(metaPath);
  }
}

bool applyManifestAndDownload(HTTPClient& http)
{
  JsonDocument filter;
  filter["revision"] = true;
  filter["cards"][0]["card_id"] = true;
  filter["cards"][0]["id"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
  if (err)
  {
    Serial.printf("ERR manifest JSON: %s\n", err.c_str());
    return false;
  }

  const char* revisionC = doc["revision"].as<const char*>();
  if (!revisionC || !revisionC[0])
  {
    Serial.println("ERR manifest without revision");
    return false;
  }

  JsonArrayConst cards = doc["cards"].as<JsonArrayConst>();
  if (cards.isNull())
  {
    Serial.println("ERR manifest without cards[]");
    return false;
  }

  int idx = 0;
  for (JsonVariantConst card : cards)
  {
    String cardId = getCardId(card);
    if (cardId.isEmpty())
    {
      Serial.printf("WARN card[%d] without id\n", idx);
      continue;
    }

    if (!downloadCardTri(cardId, idx))
    {
      Serial.printf("ERR downloading card %s\n", cardId.c_str());
      return false;
    }
    idx++;
  }

  removeSlidesFrom(idx);
  setCabinetRevision(String(revisionC));
  if (idx > 0 && slide_idx >= idx) slide_idx = 0;
  Serial.printf("Manifest applied. revision=%s slides=%d\n", revisionC, idx);
  return true;
}

bool fetchAndApplyManifest(const String& knownRevision)
{
  HTTPClient http;
  String url = String(API_BASE_URL) + "/api/device/cabinets/" + urlEncode(CABINET_ID) + "/manifest";

  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url))
  {
    Serial.println("ERR HTTP begin manifest");
    return false;
  }

  if (!knownRevision.isEmpty())
  {
    http.addHeader("If-None-Match", String("\"") + knownRevision + "\"");
  }

  int code = http.GET();
  if (code == HTTP_CODE_NOT_MODIFIED)
  {
    String etag = trimQuotes(http.header("ETag"));
    if (!etag.isEmpty()) setCabinetRevision(etag);
    http.end();
    Serial.println("Manifest 304 (no changes)");
    return true;
  }

  if (code != HTTP_CODE_OK)
  {
    Serial.printf("ERR manifest GET code=%d\n", code);
    http.end();
    return false;
  }

  bool ok = applyManifestAndDownload(http);
  http.end();
  return ok;
}

void maybeCheckForUpdates(bool forceCheck)
{
  const uint32_t periodCycles = calcUpdatePeriodCycles();
  bool shouldCheck = forceCheck || wake_cycle_count == 0 || (wake_cycle_count % periodCycles == 0);
  if (!shouldCheck)
  {
    Serial.printf("Skip update check. cycle=%lu next in %lu cycles\n", (unsigned long)wake_cycle_count, (unsigned long)(periodCycles - (wake_cycle_count % periodCycles)));
    return;
  }

  if (!connectWiFi()) return;

  String revision = getCabinetRevision();
  String previousRevision = revision;

  HTTPClient http;
  String url = String(API_BASE_URL) + "/api/device/cabinets/" + urlEncode(CABINET_ID) + "/check";
  if (!revision.isEmpty())
  {
    url += "?revision=" + urlEncode(revision);
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url))
  {
    Serial.println("ERR HTTP begin check");
    disconnectWiFi();
    return;
  }

  int code = http.GET();
  if (code == HTTP_CODE_NO_CONTENT)
  {
    String etag = trimQuotes(http.header("ETag"));
    if (!etag.isEmpty()) setCabinetRevision(etag);
    Serial.println("Check 204 (no updates)");
    http.end();
    disconnectWiFi();
    return;
  }

  if (code != HTTP_CODE_OK)
  {
    Serial.printf("ERR check GET code=%d\n", code);
    http.end();
    disconnectWiFi();
    return;
  }

  String etag = trimQuotes(http.header("ETag"));
  JsonDocument filter;
  filter["has_updates"] = true;
  filter["revision"] = true;

  JsonDocument checkDoc;
  DeserializationError err = deserializeJson(checkDoc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
  http.end();
  if (err)
  {
    Serial.printf("ERR check JSON: %s\n", err.c_str());
    disconnectWiFi();
    return;
  }

  bool hasUpdates = checkDoc["has_updates"].as<bool>();
  const char* revisionJson = checkDoc["revision"].as<const char*>();
  if (revisionJson && revisionJson[0])
  {
    setCabinetRevision(String(revisionJson));
  }
  else if (!etag.isEmpty())
  {
    setCabinetRevision(etag);
  }

  if (hasUpdates)
  {
    String currentRevision = getCabinetRevision();
    Serial.printf("Updates available. revision=%s\n", currentRevision.c_str());
    fetchAndApplyManifest(previousRevision);
  }
  else
  {
    Serial.println("Check says no updates");
  }

  disconnectWiFi();
}

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
// ===== Utilidades TRI
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
  f.seek(red_off, SeekSet);
  for (uint16_t yy = 0; yy < h; yy++)
  {
    if (f.read(rowbuf, bytes_per_row) != (int)bytes_per_row)
    {
      Serial.println("ERR TRI read red");
      f.close();
      return false;
    }
    display.drawBitmap(x, y + yy, rowbuf, w, 1, GxEPD_RED);
  }

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
  const int x = 14;
  const int y = 10;

  display.drawBitmap(x, y, museu_logo_240_black, MUSEU_LOGO_240_W, MUSEU_LOGO_240_H, GxEPD_BLACK);
  display.drawBitmap(x, y, museu_logo_240_red,   MUSEU_LOGO_240_W, MUSEU_LOGO_240_H, GxEPD_RED);

  // Línea roja separadora (arriba para ganar espacio BW)
  const int sep_y = HEADER_H - 12;
  display.fillRect(14, sep_y, display.width() - 2 * 14, 6, GxEPD_RED);
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
  if (ENABLE_DEBUG_SERIAL)
  {
    Serial.begin(115200);
    delay(200);
  }
  else
  {
    delay(50);
  }

  // Mount LittleFS temprano para updates y conteo de slides
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount FAILED");
    wake_cycle_count++;
    if (ENABLE_DEBUG_SERIAL)
    {
      Serial.flush();
      Serial.end();
    }
    goDeepSleep();
    return;
  }
  Serial.println("LittleFS mounted OK");

  if (!LittleFS.exists("/slides"))
  {
    LittleFS.mkdir("/slides");
  }

  const auto cause = esp_sleep_get_wakeup_cause();
  bool firstBoot = (cause != ESP_SLEEP_WAKEUP_TIMER);

  maybeCheckForUpdates(firstBoot);

  // Contar archivos slide00.tri, slide01.tri, etc.
  int slideCount = countSlidesInFS();

  if (slideCount == 0) {
    Serial.println("No slides found in /slides/slide00.tri, slide01.tri, ...");
    wake_cycle_count++;
    if (ENABLE_DEBUG_SERIAL)
    {
      Serial.flush();
      Serial.end();
    }
    goDeepSleep();
    return;
  }

  NUM_SLIDES = slideCount;
  FULL_EVERY_N_CHANGES = 2 * NUM_SLIDES;
  Serial.printf("Found %d slides, FULL_EVERY_N_CHANGES=%d\n", NUM_SLIDES, FULL_EVERY_N_CHANGES);

  if (slide_idx >= NUM_SLIDES) slide_idx = 0;

  powerOnEPD();

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(ROT);

  bool doFull = false;

  if (cause == ESP_SLEEP_WAKEUP_TIMER)
  {
    slide_idx = (slide_idx + 1) % NUM_SLIDES;
    change_count++;
    doFull = (FULL_EVERY_N_CHANGES > 0) && ((change_count % FULL_EVERY_N_CHANGES) == 0);
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
  wake_cycle_count++;
  if (ENABLE_DEBUG_SERIAL)
  {
    Serial.flush();
    Serial.end();
  }
  goDeepSleep();
}

void loop() {}
