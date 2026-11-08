#ifdef BOARD_ATOM_LITE

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <esp_sleep.h>

#include "board.h"

static const uint8_t BUTTON_PIN = 39;
static const uint8_t LED_PIN    = 27;

static const uint32_t BLINK_MS      = 400;
static const uint32_t FAST_BLINK_MS = 120;

static const uint8_t LED_BRIGHTNESS = 40;
static const uint32_t COLOR_BLACK     = 0;
static const uint32_t COLOR_ON        = Adafruit_NeoPixel::Color(255, 170, 80);
static const uint32_t COLOR_OFF       = Adafruit_NeoPixel::Color(160, 0, 0);
static const uint32_t COLOR_PAIRING   = Adafruit_NeoPixel::Color(0, 0, 255);
static const uint32_t COLOR_RECONNECT = Adafruit_NeoPixel::Color(255, 160, 0);
static const uint32_t COLOR_RESET     = Adafruit_NeoPixel::Color(180, 0, 255);
static const uint32_t COLOR_SAVED     = Adafruit_NeoPixel::Color(0, 200, 40);

static Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
static uint32_t shownColor = COLOR_BLACK;

static void setLed(uint32_t color) {
  if (color == shownColor) return;
  shownColor = color;
  led.setPixelColor(0, color);
  led.show();
}

namespace board {

const char* const NAME = "ATOM Lite";

const ButtonDef BUTTONS[] = {
    {BUTTON_PIN, "button", Action::Toggle, Action::Candle, Action::SaveNormal, Action::Reset},
};
const size_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

void begin() {
  pinMode(BUTTON_PIN, INPUT);
  led.begin();
  led.setBrightness(LED_BRIGHTNESS);
  led.show();
}

bool isPressed(size_t button) { return digitalRead(BUTTONS[button].pin) == LOW; }

int wakeButton() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 ? 0 : -1; }

void show(Mode mode, bool lightOn, uint32_t now) {
  switch (mode) {
    case Mode::Resetting:
      setLed((now / FAST_BLINK_MS) % 2 ? COLOR_RESET : COLOR_BLACK);
      break;
    case Mode::Ready:
      setLed(lightOn ? COLOR_ON : COLOR_OFF);
      break;
    case Mode::Pairing:
      setLed((now / BLINK_MS) % 2 ? COLOR_PAIRING : COLOR_BLACK);
      break;
    case Mode::Connecting:
      setLed((now / BLINK_MS) % 2 ? COLOR_RECONNECT : COLOR_BLACK);
      break;
  }
}

void confirmSaved() {
  for (int i = 0; i < 2; i++) {
    setLed(COLOR_SAVED);
    delay(FAST_BLINK_MS * 2);
    setLed(COLOR_BLACK);
    delay(FAST_BLINK_MS);
  }
}

void sleep(SleepReason reason) {
  if (reason != SleepReason::Idle) {
    uint32_t color = reason == SleepReason::PairingTimeout ? COLOR_PAIRING : COLOR_OFF;
    for (int i = 0; i < 3; i++) {
      setLed(color);
      delay(FAST_BLINK_MS);
      setLed(COLOR_BLACK);
      delay(FAST_BLINK_MS);
    }
  }
  setLed(COLOR_BLACK);
  while (isPressed(0)) delay(10);
  delay(50);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

}  // namespace board

#endif
