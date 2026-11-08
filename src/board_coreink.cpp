#ifdef BOARD_CORE_INK

#include <Arduino.h>
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "board.h"

// The CoreInk switches its own power off when GPIO12 goes low, so it has to stay held high
// through deep sleep.
static const gpio_num_t POWER_HOLD_PIN = GPIO_NUM_12;
static const uint8_t WHEEL_UP_PIN      = 37;
static const uint8_t WHEEL_PRESS_PIN   = 38;
static const uint8_t WHEEL_DOWN_PIN    = 39;
static const uint8_t POWER_BUTTON_PIN  = 27;

// Only RTC GPIOs can wake the ESP32, and active-low buttons give two wake sources:
// EXT0 on one pin and EXT1 "all low" on a single-pin mask.
static const size_t EXT0_BUTTON = 0;
static const size_t EXT1_BUTTON = 1;

// Waking up already costs a connect, so avoid a second e-ink refresh unless it takes a while.
static const uint32_t SHOW_CONNECTING_AFTER_MS = 3000;

static const int SCREEN_W     = 200;
static const int HEADER_H     = 26;
static const int ROW_H        = 28;
static const int VISIBLE_ROWS = 5;

enum class Screen : uint8_t { Pairing, Connecting, Status, Menu, Resetting, Unreachable, PairingTimeout };

static const uint32_t NOTHING_SHOWN = 0xFFFFFFFF;
static uint32_t shownSignature = NOTHING_SHOWN;
static bool savedNote = false;
static bool menuUsed = false;
static View lastView = {};
static Mode lastMode = Mode::Connecting;
static uint32_t modeSince = 0;

static String statusLine(const View& v) {
  if (v.mode != Mode::Ready) return "connecting...";
  if (!v.lightOn) return "OFF";
  return v.look ? String("ON - ") + v.look : String("ON");
}

static void drawHeader(const View& v) {
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(statusLine(v), 6, 5);
  M5.Display.setTextDatum(top_right);
  M5.Display.drawString(String(M5.Power.getBatteryLevel()) + "%", SCREEN_W - 6, 5);
  M5.Display.drawFastHLine(0, HEADER_H, SCREEN_W, TFT_BLACK);
}

static void drawFooter(const char* hint) {
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString(savedNote ? "normal light saved" : hint, SCREEN_W / 2, 194);
}

static void drawMessage(const char* title, const char* hint) {
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_right);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawString(String(M5.Power.getBatteryLevel()) + "%", SCREEN_W - 6, 5);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString(title, SCREEN_W / 2, 92);
  drawFooter(hint);
}

static void drawStatus(const View& v) {
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_right);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawString(String(M5.Power.getBatteryLevel()) + "%", SCREEN_W - 6, 5);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.drawString(v.lightOn ? "ON" : "OFF", SCREEN_W / 2, 80);
  if (v.lightOn && v.look) {
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString(v.look, SCREEN_W / 2, 125);
  }
  drawFooter("wheel: looks  pwr: on/off");
}

static void drawMenu(const View& v) {
  drawHeader(v);
  int first = 0;
  if (v.itemCount > VISIBLE_ROWS) {
    first = constrain((int)v.selected - VISIBLE_ROWS / 2, 0, (int)v.itemCount - VISIBLE_ROWS);
  }
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextDatum(middle_left);
  for (int row = 0; row < VISIBLE_ROWS && first + row < v.itemCount; row++) {
    int item = first + row;
    int y = HEADER_H + 2 + row * ROW_H;
    bool selected = item == v.selected;
    uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
    uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
    if (selected) M5.Display.fillRect(0, y, SCREEN_W, ROW_H, bg);
    if (item == v.active) M5.Display.fillCircle(12, y + ROW_H / 2, 4, fg);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(v.items[item], 24, y + ROW_H / 2);
  }
  if (first > 0) M5.Display.fillTriangle(188, HEADER_H + 8, 194, HEADER_H + 8, 191, HEADER_H + 3, TFT_BLACK);
  if (first + VISIBLE_ROWS < v.itemCount) {
    int y = HEADER_H + 2 + VISIBLE_ROWS * ROW_H;
    M5.Display.fillTriangle(188, y - 8, 194, y - 8, 191, y - 3, TFT_BLACK);
  }
  drawFooter("press: apply");
}

static Screen pickScreen(const View& v, uint32_t now) {
  switch (v.mode) {
    case Mode::Resetting: return Screen::Resetting;
    case Mode::Pairing: return Screen::Pairing;
    default: break;
  }
  if (v.menuOpen) return Screen::Menu;
  if (v.mode == Mode::Ready) return Screen::Status;
  return now - modeSince >= SHOW_CONNECTING_AFTER_MS ? Screen::Connecting : Screen::Status;
}

static uint32_t signature(Screen screen, const View& v) {
  return (uint32_t)screen | (v.lightOn << 4) | ((v.mode == Mode::Ready) << 5) | (savedNote << 6) |
         ((uint32_t)v.selected << 8) | ((uint32_t)v.active << 16);
}

static void render(Screen screen, const View& v, bool force) {
  uint32_t sig = signature(screen, v);
  if (sig == shownSignature && !force) return;
  // Before the first connect the state is unknown, so keep whatever the screen showed at sleep.
  if (screen == Screen::Status && v.mode != Mode::Ready && !force) return;
  shownSignature = sig;

  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  switch (screen) {
    case Screen::Pairing:        drawMessage("Pairing", "Hue app: Make visible"); break;
    case Screen::Connecting:     drawMessage("Connecting", "is the Hue app open?"); break;
    case Screen::Status:         drawStatus(v); break;
    case Screen::Menu:           drawMenu(v); break;
    case Screen::Resetting:      drawMessage("Reset", "forgetting the bulb"); break;
    case Screen::Unreachable:    drawMessage("No bulb", "press to retry"); break;
    case Screen::PairingTimeout: drawMessage("Not paired", "press to pair again"); break;
  }
  M5.Display.endWrite();
}

// Runs before setup(). The CoreInk only stays on after a press of PWR once this pin is high,
// so latching it this early shortens how long PWR has to be held to start from off.
extern "C" void initVariant() {
  gpio_hold_dis(POWER_HOLD_PIN);
  pinMode(POWER_HOLD_PIN, OUTPUT);
  digitalWrite(POWER_HOLD_PIN, HIGH);
}

static void holdPower();

namespace board {

const char* const NAME = "CoreInk";

const ButtonDef BUTTONS[] = {
    {WHEEL_PRESS_PIN, "wheel press", Action::Select, Action::None, Action::SaveNormal, Action::Reset},
    {POWER_BUTTON_PIN, "power button", Action::Toggle, Action::None, Action::None, Action::None},
    {WHEEL_UP_PIN, "wheel up", Action::Prev, Action::None, Action::None, Action::None},
    {WHEEL_DOWN_PIN, "wheel down", Action::Next, Action::None, Action::None, Action::None},
};
const size_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

static void holdPower() {
  pinMode(POWER_HOLD_PIN, OUTPUT);
  digitalWrite(POWER_HOLD_PIN, HIGH);
}

static bool powerHeld() {
  return (GPIO.enable & (1UL << POWER_HOLD_PIN)) && (GPIO.out & (1UL << POWER_HOLD_PIN));
}

static int monthFromBuildDate(const char* date) {
  static const char* const MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* found = strstr(MONTHS, String(date).substring(0, 3).c_str());
  return found ? (found - MONTHS) / 3 + 1 : 1;
}

// The RTC does not run until it has been set once. The build time is close enough for logs.
static void startClockIfStopped() {
  if (!M5.Rtc.isEnabled()) return;
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year >= 2024 && !M5.Rtc.getVoltLow()) return;
  m5::rtc_datetime_t build;
  build.date.year = atoi(__DATE__ + 7);
  build.date.month = monthFromBuildDate(__DATE__);
  build.date.date = atoi(__DATE__ + 4);
  build.time.hours = atoi(__TIME__);
  build.time.minutes = atoi(__TIME__ + 3);
  build.time.seconds = atoi(__TIME__ + 6);
  M5.Rtc.setDateTime(build);
  Serial.println("[clock] RTC was not running, set it to the build time");
}

void begin() {
  auto cfg = M5.config();
  cfg.clear_display = false;
  cfg.output_power = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  // Board auto-detection probes GPIO12 with pull resistors to tell PICO-D4 boards apart. On
  // the CoreInk that pin is the power hold, so probing it cuts the power on battery.
  m5gfx::detect_config_t detect;
  detect.fixed_board = m5gfx::board_t::board_M5StackCoreInk;
  M5.Display.setDetectConfig(detect);
  M5.begin(cfg);
  holdPower();
  Serial.printf("[power] hold pin %s\n", powerHeld() ? "high" : "NOT HIGH");
  startClockIfStopped();
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  M5.Display.setRotation(0);

  for (size_t i = 0; i < BUTTON_COUNT; i++) pinMode(BUTTONS[i].pin, INPUT);
}

uint16_t batteryMillivolts() { return M5.Power.getBatteryVoltage(); }

// The BM8563 RTC runs from the battery, so it keeps counting while the CoreInk is off.
uint32_t clockSeconds() {
  if (!M5.Rtc.isEnabled()) return 0;
  auto dt = M5.Rtc.getDateTime();
  int y = dt.date.year - (dt.date.month <= 2);
  int era = y / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (dt.date.month + (dt.date.month > 2 ? -3 : 9)) + 2) / 5 + dt.date.date - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468;
  return days * 86400UL + dt.time.hours * 3600UL + dt.time.minutes * 60UL + dt.time.seconds;
}

bool isPressed(size_t button) { return digitalRead(BUTTONS[button].pin) == LOW; }

int wakeButton() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return EXT0_BUTTON;
    case ESP_SLEEP_WAKEUP_EXT1: return EXT1_BUTTON;
    default: return -1;
  }
}

void show(const View& view, uint32_t now) {
  if (view.mode != lastMode) {
    lastMode = view.mode;
    modeSince = now;
  }
  if (view.lightOn != lastView.lightOn || view.look != lastView.look) savedNote = false;
  lastView = view;
  menuUsed |= view.menuOpen;
  render(pickScreen(view, now), view, false);
  if (view.mode == Mode::Resetting) M5.Display.waitDisplay();
}

void confirmSaved() {
  savedNote = true;
  render(pickScreen(lastView, millis()), lastView, true);
}

void sleep(SleepReason reason) {
  if (reason == SleepReason::Unreachable) render(Screen::Unreachable, lastView, true);
  if (reason == SleepReason::PairingTimeout) render(Screen::PairingTimeout, lastView, true);
  if (reason == SleepReason::Idle && lastView.mode == Mode::Ready && menuUsed) {
    // Leave a clean status screen behind: the menu's fast partial refreshes leave ghosting.
    lastView.menuOpen = false;
    savedNote = false;
    M5.Display.waitDisplay();
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    render(Screen::Status, lastView, true);
  }
  M5.Display.waitDisplay();
  M5.Display.sleep();
  M5.Display.waitDisplay();

  bool held;
  do {
    held = false;
    for (size_t i = 0; i < BUTTON_COUNT; i++) held |= isPressed(i);
    delay(10);
  } while (held);
  delay(50);

  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTONS[EXT0_BUTTON].pin, 0);
  esp_sleep_enable_ext1_wakeup(1ULL << BUTTONS[EXT1_BUTTON].pin, ESP_EXT1_WAKEUP_ALL_LOW);
  gpio_hold_en(POWER_HOLD_PIN);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

}  // namespace board

#endif
