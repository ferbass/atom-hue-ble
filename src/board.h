#pragma once

#include <stddef.h>
#include <stdint.h>

enum class Action : uint8_t { None, Toggle, Theme, Reset };

struct ButtonDef {
  uint8_t pin;
  const char* name;
  Action click;
  Action doubleClick;
  Action hold;
};

enum class Mode : uint8_t { Pairing, Connecting, Ready, Resetting };

enum class SleepReason : uint8_t { Idle, Unreachable, PairingTimeout };

namespace board {

extern const char* const NAME;
extern const ButtonDef BUTTONS[];
extern const size_t BUTTON_COUNT;

void begin();
bool isPressed(size_t button);
// Index of the button that woke the board from deep sleep, or -1.
int wakeButton();
void show(Mode mode, bool lightOn, uint32_t now);
// Waits for all buttons to be released, then deep sleeps until a button is pressed.
[[noreturn]] void sleep(SleepReason reason);

}  // namespace board
