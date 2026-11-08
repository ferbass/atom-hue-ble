#pragma once

#include <stddef.h>
#include <stdint.h>

enum class Action : uint8_t { None, Toggle, Candle, SaveNormal, Reset, Select, Prev, Next };

struct ButtonDef {
  uint8_t pin;
  const char* name;
  Action click;
  Action doubleClick;
  // Fires on release after a hold shorter than `hold` needs.
  Action longPress;
  Action hold;
};

enum class Mode : uint8_t { Pairing, Connecting, Ready, Resetting };

enum class SleepReason : uint8_t { Idle, Unreachable, PairingTimeout };

static const uint8_t NO_ITEM = 0xFF;

// Everything a board needs to draw its status.
struct View {
  Mode mode;
  bool lightOn;
  // Name of the look the bulb is running, or nullptr when unknown.
  const char* look;
  bool menuOpen;
  const char* const* items;
  uint8_t itemCount;
  uint8_t selected;
  // Menu index of the running look, or NO_ITEM.
  uint8_t active;
};

namespace board {

extern const char* const NAME;
extern const ButtonDef BUTTONS[];
extern const size_t BUTTON_COUNT;

void begin();
bool isPressed(size_t button);
// Index of the button that woke the board from deep sleep, or -1.
int wakeButton();
// Battery voltage in millivolts, or 0 when the board cannot measure it.
uint16_t batteryMillivolts();
void show(const View& view, uint32_t now);
// Short visible acknowledgement that the normal light was saved.
void confirmSaved();
// Waits for all buttons to be released, then deep sleeps until a button is pressed.
[[noreturn]] void sleep(SleepReason reason);

}  // namespace board
