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

// Only RTC GPIOs can wake the ESP32, and active-low buttons give two wake sources:
// EXT0 on one pin and EXT1 "all low" on a single-pin mask.
static const size_t EXT0_BUTTON = 0;
static const size_t EXT1_BUTTON = 1;

// Waking up already costs a connect, so avoid a second e-ink refresh unless it takes a while.
static const uint32_t SHOW_CONNECTING_AFTER_MS = 3000;

enum class Screen : uint8_t { None, Pairing, Connecting, On, Off, Resetting, Unreachable, PairingTimeout };

static Screen shownScreen = Screen::None;
static bool savedNote = false;
static Mode lastMode = Mode::Connecting;
static uint32_t modeSince = 0;

static void draw(Screen screen, bool force = false) {
  if (screen == shownScreen && !force) return;
  if (screen != shownScreen && !force) savedNote = false;
  shownScreen = screen;

  const char* title = "";
  const char* hint = "";
  switch (screen) {
    case Screen::None: return;
    case Screen::Pairing:        title = "Pairing";     hint = "Hue app: Make visible"; break;
    case Screen::Connecting:     title = "Connecting";  hint = "is the Hue app open?"; break;
    case Screen::On:             title = "ON";          hint = "press: toggle  up: candle"; break;
    case Screen::Off:            title = "OFF";         hint = "press: toggle  up: candle"; break;
    case Screen::Resetting:      title = "Reset";       hint = "forgetting the bulb"; break;
    case Screen::Unreachable:    title = "No bulb";     hint = "press to retry"; break;
    case Screen::PairingTimeout: title = "Not paired";  hint = "press to pair again"; break;
  }

  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

  M5.Display.setTextDatum(top_left);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawString("Hue", 8, 8);
  M5.Display.setTextDatum(top_right);
  M5.Display.drawString(String(M5.Power.getBatteryLevel()) + "%", 192, 8);

  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(strlen(title) <= 3 ? &fonts::FreeSansBold24pt7b : &fonts::FreeSansBold18pt7b);
  M5.Display.drawString(title, 100, 96);

  M5.Display.setTextDatum(bottom_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawString(savedNote ? "normal light saved" : hint, 100, 192);
  M5.Display.endWrite();
}

namespace board {

const char* const NAME = "CoreInk";

const ButtonDef BUTTONS[] = {
    {WHEEL_PRESS_PIN, "wheel press", Action::Toggle, Action::Candle, Action::SaveNormal, Action::Reset},
    {WHEEL_UP_PIN, "wheel up", Action::Candle, Action::None, Action::None, Action::None},
};
const size_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

void begin() {
  pinMode(POWER_HOLD_PIN, OUTPUT);
  digitalWrite(POWER_HOLD_PIN, HIGH);
  gpio_hold_dis(POWER_HOLD_PIN);

  auto cfg = M5.config();
  cfg.clear_display = false;
  cfg.output_power = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  M5.Display.setRotation(0);

  for (size_t i = 0; i < BUTTON_COUNT; i++) pinMode(BUTTONS[i].pin, INPUT);
}

bool isPressed(size_t button) { return digitalRead(BUTTONS[button].pin) == LOW; }

int wakeButton() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return EXT0_BUTTON;
    case ESP_SLEEP_WAKEUP_EXT1: return EXT1_BUTTON;
    default: return -1;
  }
}

void show(Mode mode, bool lightOn, uint32_t now) {
  if (mode != lastMode) {
    lastMode = mode;
    modeSince = now;
  }
  switch (mode) {
    case Mode::Pairing:
      draw(Screen::Pairing);
      break;
    case Mode::Connecting:
      if (now - modeSince >= SHOW_CONNECTING_AFTER_MS) draw(Screen::Connecting);
      break;
    case Mode::Ready:
      draw(lightOn ? Screen::On : Screen::Off);
      break;
    case Mode::Resetting:
      draw(Screen::Resetting);
      M5.Display.waitDisplay();
      break;
  }
}

void confirmSaved() {
  savedNote = true;
  draw(shownScreen == Screen::None ? Screen::On : shownScreen, true);
}

void sleep(SleepReason reason) {
  if (reason == SleepReason::Unreachable) draw(Screen::Unreachable);
  if (reason == SleepReason::PairingTimeout) draw(Screen::PairingTimeout);
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
