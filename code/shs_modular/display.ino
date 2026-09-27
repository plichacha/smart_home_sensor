// ============================================================================
// display.ino — Russian clock + rotating sensor cards, ST7789 240x240
// Исправленная версия:
//   - убрана причина сильного мерцания: больше нет полной перерисовки экрана
//     каждую секунду/при каждом новом измерении;
//   - часы и дата перерисовываются только при изменении минуты;
//   - карточка датчиков обновляется отдельно;
//   - на странице CO₂ оставлен VOC, но без длинного русского заголовка;
//   - под показаниями выводится понятный статус «НОРМА» / «ПОВЫШЕНО» и т.п.;
//   - «КАЛИБРОВКА» заменена на обычную человеческую оценку качества воздуха.
// ============================================================================

#include "config.h"

#if USE_DISPLAY

#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <qrcode.h>
#include <time.h>
#include <math.h>
#include <string.h>

static Arduino_DataBus *bus = new Arduino_ESP32SPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_SCLK, PIN_MOSI, PIN_MISO);

static Arduino_GFX *gfx = new Arduino_ST7789(
    bus, PIN_LCD_RST, DEFAULT_LCD_ROTATION, true,
    240, 240, 0, 0, 0, 80);

// -----------------------------------------------------------------------------
// Цвета
// -----------------------------------------------------------------------------
#define BG_COLOR      RGB565(0x0B, 0x11, 0x18)
#define CARD_COLOR    RGB565(0x14, 0x1E, 0x28)
#define TEXT_COLOR    RGB565(0xF4, 0xF7, 0xFA)
#define MUTED_COLOR   RGB565(0x92, 0xA4, 0xB5)
#define ACCENT_COLOR  RGB565(0x72, 0xD6, 0xC2)
#define WARN_COLOR    RGB565(0xFF, 0xC8, 0x66)
#define BAD_COLOR     RGB565(0xFF, 0x77, 0x77)
#define WHITE_BG      0xFFFF
#define QR_DARK       0x0000
#define QR_TEXT       RGB565(0x33, 0x33, 0x33)

// -----------------------------------------------------------------------------
// Состояние
// -----------------------------------------------------------------------------
static bool wifiOk = false;
static bool blinkOn = true;
static uint32_t lastBlink = 0;

static bool screenLocked = false;
static uint32_t screenUnlockAt = 0;

static SensorPacket latestPacket;
static bool havePacket = false;

static uint8_t page = 0;
static uint32_t lastPageChange = 0;
static const uint32_t PAGE_MS = 5000;
static const uint8_t PAGE_COUNT = 4;

// Время последней отрисованной минуты.
static int lastDrawnMinute = -1;
static bool forceClockRedraw = true;

// Обновление данных карточки — не чаще раза в секунду.
static uint32_t lastCardRedraw = 0;
static bool forceCardRedraw = true;

// U8g2 fonts: Cyrillic for Russian text, numeric font for the large clock.
static const uint8_t *FONT_RU      = u8g2_font_8x13_t_cyrillic;
static const uint8_t *FONT_RU_BIG  = u8g2_font_10x20_t_cyrillic;
static const uint8_t *FONT_CLOCK   = u8g2_font_logisoso42_tn;

// -----------------------------------------------------------------------------
// Оценки
// -----------------------------------------------------------------------------
static uint16_t iaqColor(float v) {
  if (isnan(v)) return MUTED_COLOR;
  if (v <= 50)  return ACCENT_COLOR;
  if (v <= 100) return RGB565(0xA8, 0xD8, 0x3A);
  if (v <= 150) return WARN_COLOR;
  if (v <= 200) return RGB565(0xFF, 0x9F, 0x40);
  return BAD_COLOR;
}

static uint16_t co2Color(float v) {
  if (isnan(v)) return MUTED_COLOR;
  if (v <= 800)  return ACCENT_COLOR;
  if (v <= 1000) return RGB565(0xA8, 0xD8, 0x3A);
  if (v <= 1500) return WARN_COLOR;
  if (v <= 2000) return RGB565(0xFF, 0x9F, 0x40);
  return BAD_COLOR;
}

// Упрощённая бытовая шкала для экрана.
// Это именно UI-оценка, а не отдельный датчик качества воздуха.
static const char *iaqText(float v) {
  if (isnan(v)) return "нет данных";
  if (v <= 50)  return "ИДЕАЛЬНО";
  if (v <= 100) return "ХОРОШО";
  if (v <= 150) return "НОРМА";
  if (v <= 200) return "ПЛОХО";
  return "УЖАСНО";
}

static const char *co2Text(float v) {
  if (isnan(v)) return "нет данных";
  if (v <= 1000) return "НОРМА";
  if (v <= 1500) return "ПОВЫШЕНО";
  return "ПЛОХО";
}

// VOC — это Breath VOC equivalent из BSEC. Для простого интерфейса используем
// мягкую трёхступенчатую оценку; сами численные показания VOC не меняются.
static const char *vocText(float v) {
  if (isnan(v)) return "нет данных";
  if (v <= 1.0f) return "НОРМА";
  if (v <= 3.0f) return "ПОВЫШЕНО";
  return "ПЛОХО";
}

// -----------------------------------------------------------------------------
// Шрифты / текст
// -----------------------------------------------------------------------------
static void setRuFont(bool big = false) {
  gfx->setFont(big ? FONT_RU_BIG : FONT_RU);
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
}

static void setClockFont() {
  gfx->setFont(FONT_CLOCK);
  gfx->setUTF8Print(false);
  gfx->setTextWrap(false);
}

static void centeredText(const char *s, int16_t y, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;

  gfx->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor((240 - (int16_t)w) / 2, y);
  gfx->print(s);
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------
static void drawWifiIcon(uint16_t color) {
  const int16_t cx = 218;
  const int16_t cy = 17;

  gfx->fillCircle(cx, cy, 9, color);
  gfx->fillCircle(cx, cy, 7, BG_COLOR);
  gfx->fillCircle(cx, cy, 5, color);
  gfx->fillCircle(cx, cy, 3, BG_COLOR);
  gfx->fillCircle(cx, cy, 1, color);

  gfx->fillRect(cx - 10, cy + 1, 21, 9, BG_COLOR);
  gfx->fillTriangle(cx, cy, cx - 11, cy - 11, cx - 11, cy, BG_COLOR);
  gfx->fillTriangle(cx, cy, cx + 11, cy - 11, cx + 11, cy, BG_COLOR);
}

static void drawTopStatus() {
  gfx->fillRect(194, 0, 46, 30, BG_COLOR);
  if (wifiOk || blinkOn) {
    drawWifiIcon(wifiOk ? ACCENT_COLOR : BAD_COLOR);
  }
}

// -----------------------------------------------------------------------------
// Часы / дата
// -----------------------------------------------------------------------------
static bool getClockText(char *buf, size_t n) {
  struct tm tmNow;

  if (!getLocalTime(&tmNow, 20)) {
    snprintf(buf, n, "--:--");
    return false;
  }

  tmNow.tm_hour = (tmNow.tm_hour + 2) % 24;

  strftime(buf, n, "%H:%M", &tmNow);
  return true;
}

static void drawClockArea() {
  // Очищаем только верхнюю часть, а не весь экран.
  gfx->fillRect(0, 0, 240, 99, BG_COLOR);

  char t[8];
  bool valid = getClockText(t, sizeof(t));

  setClockFont();
  gfx->setTextColor(valid ? TEXT_COLOR : MUTED_COLOR);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(t, 0, 64, &x1, &y1, &w, &h);
  gfx->setCursor((240 - (int16_t)w) / 2, 66);
  gfx->print(t);

  struct tm tmNow;

  if (getLocalTime(&tmNow, 5)) {
    static const char *days[] = {
      "воскресенье",
      "понедельник",
      "вторник",
      "среда",
      "четверг",
      "пятница",
      "суббота"
    };

    char date[40];
    snprintf(date, sizeof(date), "%s, %02d.%02d.%04d",
             days[tmNow.tm_wday], tmNow.tm_mday,
             tmNow.tm_mon + 1, tmNow.tm_year + 1900);

    setRuFont(false);
    centeredText(date, 86, MUTED_COLOR);

    lastDrawnMinute = tmNow.tm_min;
  } else {
    setRuFont(false);
    centeredText("нет времени — проверьте Wi-Fi", 86, MUTED_COLOR);
    lastDrawnMinute = -1;
  }

  forceClockRedraw = false;
}

static void drawClockIfNeeded() {
  if (forceClockRedraw) {
    drawClockArea();
    return;
  }

  struct tm tmNow;
  if (!getLocalTime(&tmNow, 5)) {
    if (lastDrawnMinute != -1) {
      drawClockArea();
    }
    return;
  }

  if (tmNow.tm_min != lastDrawnMinute) {
    drawClockArea();
  }
}

// -----------------------------------------------------------------------------
// Карточки
// -----------------------------------------------------------------------------
static void drawCardShell(const char *title, uint16_t accent) {
  gfx->fillRoundRect(8, 100, 224, 128, 16, CARD_COLOR);
  gfx->fillRoundRect(8, 100, 6, 128, 3, accent);

  setRuFont(true);
  gfx->setTextColor(accent);
  gfx->setCursor(24, 124);
  gfx->print(title);
}

static void drawBigValue(const char *value, const char *unit, uint16_t color) {
  setClockFont();
  gfx->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(value, 0, 174, &x1, &y1, &w, &h);

  int16_t x = 120 - (int16_t)w / 2 - (unit[0] ? 10 : 0);
  gfx->setCursor(x, 174);
  gfx->print(value);

  if (unit[0]) {
    setRuFont(false);
    gfx->setTextColor(MUTED_COLOR);
    gfx->setCursor(x + w + 8, 171);
    gfx->print(unit);
  }
}

static void drawStatusLine(const char *label, const char *value, uint16_t color,
                           int16_t y = 214) {
  setRuFont(false);

  if (label && label[0]) {
    gfx->setTextColor(MUTED_COLOR);
    gfx->setCursor(24, y);
    gfx->print(label);
  }

  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(value, 0, y, &x1, &y1, &w, &h);
  gfx->setCursor(216 - w, y);
  gfx->print(value);
}

static void drawCard() {
  char a[24];
  char b[32];

  switch (page) {
    case 0: {
      drawCardShell("Качество воздуха", iaqColor(latestPacket.iaq));

      if (!havePacket || isnan(latestPacket.iaq)) {
        drawBigValue("--", "", MUTED_COLOR);
        drawStatusLine("статус", "нет данных", MUTED_COLOR);
      } else {
        snprintf(a, sizeof(a), "%.0f", latestPacket.iaq);
        drawBigValue(a, "IAQ", iaqColor(latestPacket.iaq));
        drawStatusLine("оценка", iaqText(latestPacket.iaq),
                       iaqColor(latestPacket.iaq));
      }
      break;
    }

    case 1: {
      drawCardShell("Температура и влажность", ACCENT_COLOR);

      if (!havePacket || isnan(latestPacket.temperature)) {
        drawBigValue("--", "°C", MUTED_COLOR);
      } else {
        snprintf(a, sizeof(a), "%.1f", latestPacket.temperature);
        drawBigValue(a, "°C", TEXT_COLOR);
      }

      if (!havePacket || isnan(latestPacket.humidity)) {
        snprintf(b, sizeof(b), "-- %%");
      } else {
        snprintf(b, sizeof(b), "%.0f %%", latestPacket.humidity);
      }

      drawStatusLine("влажность", b, TEXT_COLOR);
      break;
    }

    case 2: {
      // Важно: оставляем VOC, но НЕ пишем длинную русскую надпись
      // «летучие вещества» сверху. Заголовок только CO₂.
      drawCardShell("CO2", co2Color(latestPacket.co2));

      if (!havePacket || isnan(latestPacket.co2)) {
        drawBigValue("--", "ppm", MUTED_COLOR);
        drawStatusLine("CO2", "нет данных", MUTED_COLOR, 198);
      } else {
        snprintf(a, sizeof(a), "%.0f", latestPacket.co2);
        drawBigValue(a, "ppm", co2Color(latestPacket.co2));
        drawStatusLine("CO2", co2Text(latestPacket.co2),
                       co2Color(latestPacket.co2), 198);
      }

      // VOC остаётся отдельной строкой внизу карточки.
      // Статус выводим рядом с VOC, чтобы он не пересекался с основным числом.
      if (!havePacket || isnan(latestPacket.voc)) {
        snprintf(b, sizeof(b), "--");
        drawStatusLine("VOC", b, MUTED_COLOR, 220);
      } else {
        snprintf(b, sizeof(b), "%.1f %s", latestPacket.voc, vocText(latestPacket.voc));
        uint16_t vocColor = latestPacket.voc <= 1.0f
                              ? ACCENT_COLOR
                              : (latestPacket.voc <= 3.0f ? WARN_COLOR : BAD_COLOR);
        drawStatusLine("VOC", b, vocColor, 220);
      }
      break;
    }

    default: {
      drawCardShell("Давление", ACCENT_COLOR);

      if (!havePacket || isnan(latestPacket.pressure)) {
        drawBigValue("----", "hPa", MUTED_COLOR);
      } else {
        snprintf(a, sizeof(a), "%.0f", latestPacket.pressure);
        drawBigValue(a, "hPa", TEXT_COLOR);
      }

      drawStatusLine("статус", "НОРМА", ACCENT_COLOR);
      break;
    }
  }

  // Индикатор текущей страницы.
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    uint16_t c = (i == page) ? TEXT_COLOR : RGB565(0x45, 0x55, 0x65);
    gfx->fillCircle(102 + i * 12, 236, (i == page) ? 3 : 2, c);
  }

  forceCardRedraw = false;
  lastCardRedraw = millis();
}

static void drawFullPage() {
  gfx->fillScreen(BG_COLOR);
  drawClockArea();
  drawTopStatus();
  drawCard();
}

static void redrawCardIfNeeded(bool force = false) {
  const uint32_t now = millis();
  if (!force && !forceCardRedraw && now - lastCardRedraw < 1000) return;

  // Меняем только область карточки. Верхняя часть с часами не трогается.
  gfx->fillRect(0, 99, 240, 141, BG_COLOR);
  drawCard();
}

// -----------------------------------------------------------------------------
// Public display API
// -----------------------------------------------------------------------------
static void drawStatus(const char *msg, uint16_t color) {
  gfx->fillRect(0, 100, 240, 140, BG_COLOR);
  setRuFont(true);
  centeredText(msg, 145, color);
}

void displayInit() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed");
  }

  gfx->setRotation(settings.lcdRotation & 0x03);
  gfx->setTextWrap(false);

  latestPacket = SensorPacket();
  havePacket = false;
  page = 0;
  lastPageChange = millis();
  lastCardRedraw = 0;
  forceClockRedraw = true;
  forceCardRedraw = true;
  lastDrawnMinute = -1;

  drawFullPage();
}

void displayStatus(const char *msg, uint16_t color) {
  if (screenLocked) return;
  drawStatus(msg, color);
}

void displayAccuracy(uint8_t accuracy) {
  // Отдельно не рисуем: пользовательский интерфейс показывает обычную
  // человеческую оценку IAQ вместо слова «калибровка».
  (void)accuracy;
}

void displayUpdate(const SensorPacket &p) {
  latestPacket = p;
  havePacket = true;
}

void displayNoSensor() {
  latestPacket = SensorPacket();
  havePacket = false;
  forceCardRedraw = true;
}

void displaySetWifiStatus(bool connected) {
  if (wifiOk == connected) return;

  wifiOk = connected;
  blinkOn = true;

  if (!screenLocked) {
    drawTopStatus();
  }
}

void displaySetMqttStatus(bool connected) {
  // Основной UI показывает состояние Wi-Fi; MQTT остаётся виден в Home Assistant.
  (void)connected;
}

void displayTick() {
   if (screenLocked) {
    if (screenUnlockAt &&
        (int32_t)(millis() - screenUnlockAt) >= 0) {
      displayResume();
    }
    return;
  }

  const uint32_t now = millis();

  // Переключаем карточку примерно раз в 5 секунд.
  if (now - lastPageChange >= PAGE_MS) {
    lastPageChange = now;
    page = (page + 1) % PAGE_COUNT;

    // Только здесь происходит перерисовка карточки.
    redrawCardIfNeeded(true);
  }

  // Часы обновляются только при смене минуты.
  drawClockIfNeeded();

  if (!wifiOk && now - lastBlink >= 500) {
    lastBlink = now;
    blinkOn = !blinkOn;
    drawTopStatus();
  }
}

// -----------------------------------------------------------------------------
// Setup portal
// -----------------------------------------------------------------------------
static void drawPortalField(
  int16_t y,
  const char *caption,
  const char *value,
  uint16_t color
) {
  gfx->fillRect(0, y, 240, 38, BG_COLOR);

  setRuFont(false);
  gfx->setTextColor(MUTED_COLOR);
  gfx->setCursor(12, y + 11);
  gfx->print(caption);

  setRuFont(true);
  gfx->setTextColor(color);
  gfx->setCursor(12, y + 31);
  gfx->print(value);
}

void displayPortal(const char *apName, const char *deviceId) {
  screenLocked = true;
  screenUnlockAt = 0;

  gfx->fillScreen(BG_COLOR);

  setRuFont(true);
  gfx->setTextColor(TEXT_COLOR);
  gfx->setCursor(12, 24);
  gfx->print("Настройка");

  drawPortalField(34,  "Wi-Fi сеть:", apName, ACCENT_COLOR);
  drawPortalField(74,  "Откройте:", "192.168.4.1", TEXT_COLOR);
  drawPortalField(114, "ID устройства:", deviceId, TEXT_COLOR);
  displayPortalSensor("проверка...", COLOR_INFO);

  setRuFont(false);
  gfx->setTextColor(MUTED_COLOR);
  gfx->setCursor(12, 226);
  gfx->print("Подключитесь к сети с экрана");
}

void displayPortalSensor(const char *msg, uint16_t color) {
  if (!screenLocked) return;
  drawPortalField(154, "Датчик BME680:", msg, color);
}

// -----------------------------------------------------------------------------
// QR screen
// -----------------------------------------------------------------------------
static void drawQrCallback(esp_qrcode_handle_t qr) {
  int size = esp_qrcode_get_size(qr);
  if (size <= 0) return;

  gfx->fillScreen(WHITE_BG);

  const int16_t header = 24;
  const int16_t avail = 240 - header - 8;
  const int16_t scale = avail / size;

  if (scale < 1) return;

  const int16_t dim = size * scale;
  const int16_t ox = (240 - dim) / 2;
  const int16_t oy = header + (avail - dim) / 2;

  setRuFont(false);
  gfx->setTextColor(QR_TEXT);
  gfx->setCursor(10, 16);
  gfx->print("Откройте камеру");

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qr, x, y)) {
        gfx->fillRect(ox + x * scale, oy + y * scale,
                      scale, scale, QR_DARK);
      }
    }
  }
}

void displayQr(const char *url, uint32_t showMs) {
  screenLocked = true;
  screenUnlockAt = millis() + showMs;

  if (screenUnlockAt == 0) screenUnlockAt = 1;

  esp_qrcode_config_t cfg = {
    .display_func = drawQrCallback,
    .max_qrcode_version = 5,
    .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
  };

  if (esp_qrcode_generate(&cfg, url) != ESP_OK) {
    displayResume();
  }
}

void displayResume() {
  screenLocked = false;
  screenUnlockAt = 0;

  gfx->setRotation(settings.lcdRotation & 0x03);
  lastPageChange = millis();
  forceClockRedraw = true;
  forceCardRedraw = true;
  lastDrawnMinute = -1;
  drawFullPage();
}

#else

void displayInit() {}
void displayStatus(const char *, uint16_t) {}
void displayAccuracy(uint8_t) {}
void displayUpdate(const SensorPacket &) {}
void displayNoSensor() {}
void displaySetWifiStatus(bool) {}
void displaySetMqttStatus(bool) {}
void displayTick() {}
void displayPortal(const char *, const char *) {}
void displayPortalSensor(const char *, uint16_t) {}
void displayQr(const char *, uint32_t) {}
void displayResume() {}

#endif
