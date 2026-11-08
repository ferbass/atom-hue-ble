#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include "board.h"

// Looks are written to the bulb's combined state characteristic (0007) as a list of
// [type, length, value...] entries, the format HueBLE (github.com/flip-dots/HueBLE) documents:
// 01 power, 02 brightness (1-254), 03 mireds (LE), 04 CIE x/y (LE, scaled to 0xFFFF),
// 05 transition (LE, 100 ms units), 06 effect (01 candle, 02 fireplace, ...), 08 effect speed.
static const uint8_t TLV_POWER        = 0x01;
static const uint8_t TLV_EFFECT       = 0x06;
static const uint8_t TLV_EFFECT_SPEED = 0x08;

// Steady 2730 K at full brightness. Holding the button 2 s replaces it with the current look.
static const uint8_t DEFAULT_NORMAL[] = {0x01, 0x01, 0x01, 0x02, 0x01, 0xfe, 0x03, 0x02, 0x6e, 0x01};
// The Hue app's Candle effect, captured from an LCA002 bulb.
static const uint8_t CANDLE[] = {0x01, 0x01, 0x01, 0x02, 0x01, 0x42, 0x04, 0x04, 0x70,
                                 0x7d, 0xf5, 0x68, 0x06, 0x01, 0x01, 0x08, 0x01, 0x80};
static const size_t MAX_LOOK_LEN = 32;
static const uint8_t EFFECT_SPEED = 0x80;

struct Look {
  const char* name;
  uint8_t effect;
};
// Effect ids as listed by HueBLE. The menu only shows the ones the bulb reports supporting.
static const Look LOOKS[] = {
    {"Normal", 0x00},  {"Candle", 0x01},     {"Fireplace", 0x02}, {"Prism", 0x03},
    {"Sunrise", 0x09}, {"Sparkle", 0x0a},    {"Opal", 0x0b},      {"Glisten", 0x0c},
    {"Sunset", 0x0d},  {"Underwater", 0x0e}, {"Cosmos", 0x0f},    {"Sunbeam", 0x10},
    {"Enchant", 0x11},
};
static const uint8_t LOOK_COUNT  = sizeof(LOOKS) / sizeof(LOOKS[0]);
static const uint8_t NORMAL_LOOK = 0;
static const uint8_t CANDLE_LOOK = 1;
// In the capabilities characteristic (0001), entry 04 is a bitmask of supported effect ids.
static const uint8_t CAPS_EFFECTS = 0x04;

static const char* HUE_SERVICE_UUID    = "932c32bd-0000-47a2-835a-a8d455b859dd";
static const char* HUE_CAPS_UUID       = "932c32bd-0001-47a2-835a-a8d455b859dd";
static const char* HUE_ONOFF_UUID      = "932c32bd-0002-47a2-835a-a8d455b859dd";
static const char* HUE_STATE_UUID      = "932c32bd-0007-47a2-835a-a8d455b859dd";
// Hue bulbs usually advertise only this 16-bit Signify UUID, not the 128-bit light service.
static const uint16_t HUE_ADV_UUID16 = 0xFE0F;
static const char* HUE_NAME_HINT     = "Hue";

#ifdef LOW_POWER
static constexpr bool LOW_POWER_MODE = true;
#else
static constexpr bool LOW_POWER_MODE = false;
#endif

static const uint32_t DEBOUNCE_MS        = 50;
static const uint32_t DOUBLE_CLICK_MS    = 350;
static const uint32_t SAVE_HOLD_MS       = 2000;
static const uint32_t RESET_HOLD_MS      = 5000;
static const uint32_t RESET_FLASH_MS     = 1500;
static const uint32_t SCAN_WINDOW_MS     = 5000;
static const uint32_t CONNECT_TIMEOUT_MS = 10000;
static const uint32_t SECURE_TIMEOUT_MS  = 15000;
static const uint32_t RETRY_MIN_MS       = 2000;
static const uint32_t RETRY_MAX_MS       = 30000;
static const uint32_t ACTION_MAX_AGE_MS  = 15000;
static const uint32_t LOOP_DELAY_MS      = 5;

// Low-power mode only.
static const uint32_t IDLE_SLEEP_MS       = 5000;
static const uint32_t MENU_IDLE_SLEEP_MS  = 15000;
static const uint32_t CONNECT_GIVE_UP_MS  = 25000;
static const uint32_t PAIRING_TIMEOUT_MS  = 180000;
static const uint32_t DISCONNECT_WAIT_MS  = 1000;
static const uint32_t LOW_POWER_CPU_MHZ   = 80;

static const char* NVS_NAMESPACE = "hue";
static const char* NVS_KEY_MAC   = "mac";
static const char* NVS_KEY_TYPE  = "type";
static const char* NVS_KEY_NORMAL = "normal";
static const char* NVS_KEY_EFFECTS = "effects";
static const char* NVS_KEY_EVENTS  = "events";
static const size_t EVENT_HISTORY  = 24;
static const uint8_t EVENT_SLEEP   = 0xFE;

// Kept in flash so it survives the board losing power: a boot (reset reason + wake source) or
// a sleep (reason), with the battery voltage and a timestamp when the board has a clock.
struct Event {
  uint32_t seconds;
  uint16_t millivolts;
  uint8_t kind;
  uint8_t detail;
};

static void recordEvent(uint8_t kind, uint8_t detail, bool print);

enum class State { Scanning, Connecting, Securing, Ready, Disconnecting, WaitRetry, Resetting };

static Preferences prefs;

static State state = State::WaitRetry;
static uint32_t stateSince = 0;
static uint32_t nextAttemptAt = 0;
static uint32_t retryDelay = RETRY_MIN_MS;
static bool pairingMode = false;
static uint32_t lastActivityAt = 0;

// Survives deep sleep, so the screen can name the running look before reconnecting.
RTC_DATA_ATTR static uint8_t activeLook = NORMAL_LOOK;
static uint64_t supportedEffects = ~0ULL;
static bool menuOpen = false;
static uint8_t menuSelected = 0;
static uint8_t menuCount = 0;
static uint8_t menuLooks[LOOK_COUNT];
static const char* menuNames[LOOK_COUNT];

static Action pendingAction = Action::None;
static uint32_t pendingSince = 0;

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* onOffChr = nullptr;
static NimBLERemoteCharacteristic* stateChr = nullptr;
static NimBLEAddress target;
static NimBLEAddress candidate;
static int candidateRssi = -128;
static bool haveCandidate = false;

static volatile bool lightOn = false;
static volatile bool evtScanEnd = false;
static volatile bool evtConnected = false;
static volatile bool evtConnectFail = false;
static volatile bool evtDisconnected = false;
static volatile bool evtAuthDone = false;
static volatile bool evtNotify = false;
static volatile bool authEncrypted = false;
static volatile bool authBonded = false;
static volatile int lastReason = 0;

struct ButtonState {
  bool raw;
  bool stable;
  bool holdFired;
  uint8_t clicks;
  uint32_t changedAt;
  uint32_t pressedAt;
  uint32_t releasedAt;
};

static const size_t MAX_BUTTONS = 4;
static ButtonState buttons[MAX_BUTTONS];

static void setState(State s) {
  state = s;
  stateSince = millis();
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    bool byService = dev->isAdvertisingService(NimBLEUUID(HUE_SERVICE_UUID)) ||
                     dev->isAdvertisingService(NimBLEUUID(HUE_ADV_UUID16));
    bool byName = dev->haveName() && dev->getName().find(HUE_NAME_HINT) != std::string::npos;
    Serial.printf("[scan] %s rssi=%d name='%s'%s\n", dev->getAddress().toString().c_str(),
                  dev->getRSSI(), dev->getName().c_str(),
                  byService ? " <- Hue service" : (byName ? " <- Hue name" : ""));
    if (!byService && !byName) return;
    if (!haveCandidate || dev->getRSSI() > candidateRssi) {
      candidate = dev->getAddress();
      candidateRssi = dev->getRSSI();
      haveCandidate = true;
    }
  }

  void onScanEnd(const NimBLEScanResults&, int) override { evtScanEnd = true; }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { evtConnected = true; }

  void onConnectFail(NimBLEClient*, int reason) override {
    lastReason = reason;
    evtConnectFail = true;
  }

  void onDisconnect(NimBLEClient*, int reason) override {
    lastReason = reason;
    evtDisconnected = true;
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    authEncrypted = info.isEncrypted();
    authBonded = info.isBonded();
    evtAuthDone = true;
  }
};

static ScanCallbacks scanCallbacks;
static ClientCallbacks clientCallbacks;

static void printPairingHelp() {
  Serial.println();
  Serial.println("=== PAIRING MODE ===");
  Serial.println("Put the bulb in pairing mode (Hue app -> light -> Make discoverable,");
  Serial.println("or power-cycle the bulb), then wait.");
  Serial.println("Close the Hue app / disable phone Bluetooth: the bulb accepts ~1 connection.");
  Serial.println();
}

static void startScan() {
  haveCandidate = false;
  candidateRssi = -128;
  evtScanEnd = false;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  if (scan->start(SCAN_WINDOW_MS, false, true)) {
    Serial.println("[scan] scanning for Hue bulbs...");
    setState(State::Scanning);
  } else {
    Serial.println("[scan] ERROR: failed to start scan");
    nextAttemptAt = millis() + RETRY_MIN_MS;
    setState(State::WaitRetry);
  }
}

static void clearCharacteristics() {
  onOffChr = stateChr = nullptr;
}

static void scheduleRetry() {
  clearCharacteristics();
  nextAttemptAt = millis() + retryDelay;
  Serial.printf("[ble] retrying in %lu ms\n", (unsigned long)retryDelay);
  if (!pairingMode && !LOW_POWER_MODE) retryDelay = min(retryDelay * 2, RETRY_MAX_MS);
  setState(State::WaitRetry);
}

static void failAndRetry() {
  if (client->isConnected() && client->disconnect()) {
    setState(State::Disconnecting);
  } else {
    scheduleRetry();
  }
}

static void startConnect() {
  evtConnected = evtConnectFail = evtDisconnected = evtAuthDone = false;
  Serial.printf("[ble] connecting to %s (addr type %u)...\n", target.toString().c_str(),
                target.getType());
  if (client->connect(target, true, true, true)) {
    setState(State::Connecting);
  } else {
    Serial.println("[ble] ERROR: could not start connection attempt");
    scheduleRetry();
  }
}

static void startAttempt() {
  if (pairingMode) {
    startScan();
  } else {
    startConnect();
  }
}

static void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 1) return;
  lightOn = data[0] != 0;
  evtNotify = true;
}

#ifdef HUE_DEBUG_DUMP
static void dumpGatt() {
  Serial.println("[dump] ---- GATT table ----");
  for (NimBLERemoteService* svc : client->getServices(true)) {
    Serial.printf("[dump] service %s\n", svc->getUUID().toString().c_str());
    for (NimBLERemoteCharacteristic* chr : svc->getCharacteristics(true)) {
      Serial.printf("[dump]   char %s handle=0x%04x [%s%s%s%s%s]", chr->getUUID().toString().c_str(),
                    chr->getHandle(), chr->canRead() ? "R" : "", chr->canWrite() ? "W" : "",
                    chr->canWriteNoResponse() ? "w" : "", chr->canNotify() ? "N" : "",
                    chr->canIndicate() ? "I" : "");
      if (chr->canRead()) {
        NimBLEAttValue v = chr->readValue();
        Serial.print(" value=");
        for (size_t i = 0; i < v.size(); i++) Serial.printf("%02x", v.data()[i]);
      }
      Serial.println();
    }
  }
  Serial.println("[dump] ---- end ----");
}
#endif

static bool findEntry(const uint8_t* data, size_t len, uint8_t type, const uint8_t** value,
                      uint8_t* valueLen) {
  for (size_t i = 0; i + 2 <= len;) {
    uint8_t n = data[i + 1];
    if (i + 2 + n > len) return false;
    if (data[i] == type) {
      *value = data + i + 2;
      *valueLen = n;
      return true;
    }
    i += 2 + n;
  }
  return false;
}

static void buildMenu() {
  menuCount = 0;
  for (uint8_t i = 0; i < LOOK_COUNT; i++) {
    uint8_t effect = LOOKS[i].effect;
    if (effect != 0 && !((supportedEffects >> effect) & 1)) continue;
    menuLooks[menuCount] = i;
    menuNames[menuCount] = LOOKS[i].name;
    menuCount++;
  }
}

static uint8_t menuIndexOf(uint8_t look) {
  for (uint8_t i = 0; i < menuCount; i++) {
    if (menuLooks[i] == look) return i;
  }
  return NO_ITEM;
}

static uint8_t lookForEffect(uint8_t effect) {
  for (uint8_t i = 0; i < LOOK_COUNT; i++) {
    if (LOOKS[i].effect == effect) return i;
  }
  return NORMAL_LOOK;
}

static void readCapabilities(NimBLERemoteService* svc) {
  NimBLERemoteCharacteristic* caps = svc->getCharacteristic(HUE_CAPS_UUID);
  if (!caps) return;
  NimBLEAttValue v = caps->readValue();
  const uint8_t* mask;
  uint8_t n;
  if (!findEntry(v.data(), v.size(), CAPS_EFFECTS, &mask, &n)) return;
  uint64_t effects = 0;
  for (uint8_t i = 0; i < n && i < 8; i++) effects |= (uint64_t)mask[i] << (8 * i);
  if (effects != supportedEffects) {
    supportedEffects = effects;
    prefs.putULong64(NVS_KEY_EFFECTS, effects);
    buildMenu();
  }
  Serial.printf("[gatt] %u looks available\n", menuCount);
}

static void readActiveLook() {
  if (!stateChr) return;
  NimBLEAttValue v = stateChr->readValue();
  const uint8_t* effect;
  uint8_t n;
  bool hasEffect = findEntry(v.data(), v.size(), TLV_EFFECT, &effect, &n) && n == 1;
  activeLook = hasEffect ? lookForEffect(effect[0]) : NORMAL_LOOK;
  Serial.printf("[gatt] running look: %s\n", LOOKS[activeLook].name);
}

static bool setupGatt() {
  NimBLERemoteService* svc = client->getService(HUE_SERVICE_UUID);
  if (!svc) {
    Serial.println("[gatt] ERROR: Hue light service not found (build env 'debug-dump' to inspect)");
    return false;
  }
  onOffChr = svc->getCharacteristic(HUE_ONOFF_UUID);
  if (!onOffChr) {
    Serial.println("[gatt] ERROR: on/off characteristic not found (build env 'debug-dump' to inspect)");
    return false;
  }
  stateChr = svc->getCharacteristic(HUE_STATE_UUID);
  if (!stateChr) Serial.println("[gatt] no combined state characteristic: only on/off is available");

  NimBLEAttValue v = onOffChr->readValue();
  if (v.size() < 1) {
    Serial.println("[gatt] ERROR: could not read on/off state (not bonded, or bulb rejected us)");
    clearCharacteristics();
    return false;
  }
  lightOn = v.data()[0] != 0;
  Serial.printf("[gatt] light is currently %s\n", lightOn ? "ON" : "OFF");
  readCapabilities(svc);
  readActiveLook();

  if (onOffChr->canNotify()) {
    bool ok = onOffChr->subscribe(true, onNotify);
    Serial.printf("[gatt] notifications %s\n", ok ? "enabled" : "FAILED to enable");
  } else {
    Serial.println("[gatt] on/off characteristic does not support notifications");
  }
  return true;
}

static void saveTarget() {
  prefs.putString(NVS_KEY_MAC, target.toString().c_str());
  prefs.putUChar(NVS_KEY_TYPE, target.getType());
  Serial.printf("[nvs] saved bulb %s\n", target.toString().c_str());
}

static bool setPower(bool on) {
  uint8_t value = on ? 0x01 : 0x00;
  Serial.printf("[gatt] turning %s\n", on ? "ON" : "OFF");
  if (!onOffChr->writeValue(&value, 1, true)) {
    Serial.println("[gatt] ERROR: on/off write failed");
    return false;
  }
  lightOn = on;
  return true;
}

static void printHex(const char* label, const uint8_t* data, size_t len) {
  Serial.print(label);
  for (size_t i = 0; i < len; i++) Serial.printf("%02x", data[i]);
  Serial.println();
}

static bool writeLook(const char* name, const uint8_t* look, size_t len) {
  Serial.printf("[look] %s: ", name);
  printHex("", look, len);
  if (!stateChr->writeValue(look, len, true)) {
    Serial.println("[look] ERROR: bulb rejected the look");
    return false;
  }
  lightOn = true;
  return true;
}

static bool turnOnNormal() {
  uint8_t look[MAX_LOOK_LEN];
  size_t n = prefs.isKey(NVS_KEY_NORMAL) ? prefs.getBytes(NVS_KEY_NORMAL, look, sizeof(look)) : 0;
  if (n == 0) {
    memcpy(look, DEFAULT_NORMAL, sizeof(DEFAULT_NORMAL));
    n = sizeof(DEFAULT_NORMAL);
  }
  return writeLook("normal", look, n);
}

static void toggleLight() {
  if (lightOn || !stateChr) {
    setPower(!lightOn);
  } else if (turnOnNormal()) {
    activeLook = NORMAL_LOOK;
  }
}

static void applyLook(uint8_t look) {
  if (!stateChr || look >= LOOK_COUNT) return;
  bool ok;
  if (look == NORMAL_LOOK) {
    ok = turnOnNormal();
  } else if (look == CANDLE_LOOK) {
    ok = writeLook("candle", CANDLE, sizeof(CANDLE));
  } else {
    uint8_t cmd[] = {TLV_POWER, 1, 1, TLV_EFFECT, 1, LOOKS[look].effect, TLV_EFFECT_SPEED, 1, EFFECT_SPEED};
    ok = writeLook(LOOKS[look].name, cmd, sizeof(cmd));
  }
  if (ok) activeLook = look;
}

static void openMenu() {
  menuOpen = true;
  uint8_t i = menuIndexOf(activeLook);
  menuSelected = i == NO_ITEM ? 0 : i;
}

static void moveMenu(int step) {
  if (!menuOpen) {
    openMenu();
  } else if (menuCount) {
    menuSelected = (menuSelected + menuCount + step) % menuCount;
  }
}

// Copies a state snapshot with power forced on and effects removed, so the normal light is
// always steady. Returns 0 if the snapshot is malformed or too long.
static size_t buildNormal(const uint8_t* src, size_t len, uint8_t* out) {
  size_t n = 0;
  out[n++] = TLV_POWER;
  out[n++] = 1;
  out[n++] = 1;
  for (size_t i = 0; i < len;) {
    if (i + 2 > len) return 0;
    uint8_t type = src[i];
    size_t entry = 2 + src[i + 1];
    if (i + entry > len) return 0;
    if (type != TLV_POWER && type != TLV_EFFECT && type != TLV_EFFECT_SPEED) {
      if (n + entry > MAX_LOOK_LEN) return 0;
      memcpy(out + n, src + i, entry);
      n += entry;
    }
    i += entry;
  }
  return n;
}

static void saveNormal() {
  if (!stateChr) return;
  NimBLEAttValue v = stateChr->readValue();
  uint8_t look[MAX_LOOK_LEN];
  size_t n = buildNormal(v.data(), v.size(), look);
  if (v.size() == 0 || n <= 3) {
    printHex("[look] ERROR: could not read a usable state: ", v.data(), v.size());
    return;
  }
  prefs.putBytes(NVS_KEY_NORMAL, look, n);
  printHex("[look] saved normal: ", look, n);
  board::confirmSaved();
}

static void runAction(Action action) {
  lastActivityAt = millis();
  switch (action) {
    case Action::Toggle: toggleLight(); break;
    case Action::Candle: applyLook(CANDLE_LOOK); break;
    case Action::Select: applyLook(menuLooks[menuSelected]); break;
    case Action::SaveNormal: saveNormal(); break;
    default: break;
  }
}

static void beginReset() {
  Serial.println("[reset] long press: clearing saved bulb and all bonds");
  pendingAction = Action::None;
  prefs.clear();
  NimBLEDevice::getScan()->stop();
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteAllBonds();
  setState(State::Resetting);
}

static void requestAction(Action action) {
  if (action == Action::None) return;
  if (action == Action::Reset) {
    beginReset();
    return;
  }
  if (action == Action::Prev || action == Action::Next) {
    moveMenu(action == Action::Next ? 1 : -1);
    return;
  }
  if (action == Action::Select && !menuOpen) {
    openMenu();
    return;
  }
  if (state == State::Ready) {
    runAction(action);
    return;
  }
  if (!pairingMode) {
    pendingAction = action;
    pendingSince = millis();
    Serial.println("[btn] not connected yet: action queued");
  }
  if (state == State::WaitRetry) {
    Serial.println("[btn] reconnecting now");
    retryDelay = RETRY_MIN_MS;
    startAttempt();
  }
}

static void runPendingAction() {
  Action action = pendingAction;
  pendingAction = Action::None;
  if (action != Action::None && millis() - pendingSince <= ACTION_MAX_AGE_MS) runAction(action);
}

static void initButtons(uint32_t now) {
  int woke = board::wakeButton();
  for (size_t i = 0; i < board::BUTTON_COUNT; i++) {
    ButtonState& b = buttons[i];
    b = ButtonState{};
    b.raw = b.stable = board::isPressed(i);
    b.changedAt = b.pressedAt = b.releasedAt = now;
    // The press that woke us happened before boot. If it is already released, count it.
    if ((int)i == woke && !b.stable) b.clicks = 1;
  }
  if (woke >= 0) {
    Serial.printf("[btn] woken by %s\n", board::BUTTONS[woke].name);
    lastActivityAt = now;
  }
}

static void handleButton(size_t i, uint32_t now) {
  const ButtonDef& def = board::BUTTONS[i];
  ButtonState& b = buttons[i];

  bool raw = board::isPressed(i);
  if (raw != b.raw) {
    b.raw = raw;
    b.changedAt = now;
  }
  if (raw != b.stable && now - b.changedAt >= DEBOUNCE_MS) {
    b.stable = raw;
    lastActivityAt = now;
    if (b.stable) {
      b.pressedAt = now;
      b.holdFired = false;
    } else if (!b.holdFired && now - b.pressedAt >= SAVE_HOLD_MS && def.longPress != Action::None) {
      b.clicks = 0;
      Serial.printf("[btn] %s long press\n", def.name);
      requestAction(def.longPress);
    } else if (!b.holdFired) {
      b.clicks++;
      b.releasedAt = now;
    }
  }

  if (b.stable && !b.holdFired && def.hold != Action::None && now - b.pressedAt >= RESET_HOLD_MS) {
    b.holdFired = true;
    b.clicks = 0;
    requestAction(def.hold);
    return;
  }

  if (b.stable || b.clicks == 0) return;
  if (def.doubleClick == Action::None) {
    b.clicks = 0;
    requestAction(def.click);
  } else if (b.clicks >= 2) {
    b.clicks = 0;
    Serial.printf("[btn] %s double click\n", def.name);
    requestAction(def.doubleClick);
  } else if (now - b.releasedAt >= DOUBLE_CLICK_MS) {
    b.clicks = 0;
    requestAction(def.click);
  }
}

static bool buttonsIdle() {
  for (size_t i = 0; i < board::BUTTON_COUNT; i++) {
    if (buttons[i].stable || buttons[i].clicks) return false;
  }
  return true;
}

static void handleBleEvents() {
  if (evtNotify) {
    evtNotify = false;
    Serial.printf("[gatt] notify: light is now %s\n", lightOn ? "ON" : "OFF");
  }

  if (evtScanEnd) {
    evtScanEnd = false;
    if (state == State::Scanning) {
      if (haveCandidate) {
        target = candidate;
        Serial.printf("[scan] picked %s (rssi %d)\n", target.toString().c_str(), candidateRssi);
        startConnect();
      } else {
        Serial.println("[scan] no Hue bulb found, scanning again");
        startScan();
      }
    }
  }

  if (evtConnectFail) {
    evtConnectFail = false;
    Serial.printf("[ble] connect failed (reason %d). Is the phone app connected to the bulb?\n",
                  lastReason);
    if (state == State::Connecting) scheduleRetry();
  }

  if (evtConnected) {
    evtConnected = false;
    if (state == State::Connecting) {
      Serial.println("[ble] connected, starting encryption/bonding...");
      if (client->secureConnection(true)) {
        setState(State::Securing);
      } else {
        Serial.println("[sec] ERROR: could not start security procedure");
        failAndRetry();
      }
    }
  }

  if (evtAuthDone) {
    evtAuthDone = false;
    if (state == State::Securing) {
      Serial.printf("[sec] encrypted=%d bonded=%d\n", authEncrypted, authBonded);
#ifdef HUE_DEBUG_DUMP
      dumpGatt();
#endif
      if (!authEncrypted) {
        Serial.println(pairingMode
                           ? "[sec] pairing rejected: bulb is not in pairing mode (make it discoverable)"
                           : "[sec] encryption failed: bond lost? Hold the button 5 s to re-pair");
        failAndRetry();
      } else if (setupGatt()) {
        if (pairingMode) {
          saveTarget();
          pairingMode = false;
        }
        retryDelay = RETRY_MIN_MS;
        lastActivityAt = millis();
        Serial.println("[ble] ready - press the button to toggle");
        setState(State::Ready);
        runPendingAction();
      } else {
        failAndRetry();
      }
    }
  }

  if (evtDisconnected) {
    evtDisconnected = false;
    Serial.printf("[ble] disconnected (reason %d)\n", lastReason);
    if (state != State::Resetting && state != State::WaitRetry) scheduleRetry();
  }
}

static void handleTimers() {
  uint32_t now = millis();
  switch (state) {
    case State::WaitRetry:
      if ((int32_t)(now - nextAttemptAt) >= 0) startAttempt();
      break;
    case State::Securing:
      if (now - stateSince >= SECURE_TIMEOUT_MS) {
        Serial.println(pairingMode
                           ? "[sec] ERROR: bulb did not answer the pairing request - is it in pairing mode?"
                           : "[sec] ERROR: security procedure timed out");
        failAndRetry();
      }
      break;
    case State::Resetting:
      if (now - stateSince >= RESET_FLASH_MS) {
        Serial.println("[reset] restarting into pairing mode");
        Serial.flush();
        ESP.restart();
      }
      break;
    default:
      break;
  }
}

static void goToSleep(SleepReason reason) {
  Serial.printf("[power] going to sleep (%s)\n", reason == SleepReason::Idle          ? "idle"
                                                 : reason == SleepReason::Unreachable ? "bulb unreachable"
                                                                                      : "pairing timed out");
  NimBLEDevice::getScan()->stop();
  if (client->isConnected() && client->disconnect()) {
    uint32_t start = millis();
    while (client->isConnected() && millis() - start < DISCONNECT_WAIT_MS) delay(10);
  }
  recordEvent(EVENT_SLEEP, (uint8_t)reason, false);
  Serial.flush();
  board::sleep(reason);
}

static void handlePower() {
  if (!LOW_POWER_MODE || state == State::Resetting || !buttonsIdle()) return;
  uint32_t idle = millis() - lastActivityAt;
  if (state == State::Ready) {
    if (idle >= (menuOpen ? MENU_IDLE_SLEEP_MS : IDLE_SLEEP_MS)) goToSleep(SleepReason::Idle);
  } else if (pairingMode) {
    if (idle >= PAIRING_TIMEOUT_MS) goToSleep(SleepReason::PairingTimeout);
  } else if (idle >= CONNECT_GIVE_UP_MS) {
    goToSleep(SleepReason::Unreachable);
  }
}

static Mode currentMode() {
  switch (state) {
    case State::Ready: return Mode::Ready;
    case State::Resetting: return Mode::Resetting;
    default: return pairingMode ? Mode::Pairing : Mode::Connecting;
  }
}

static const char* resetName(uint8_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
  }
}

static const char* wakeName(uint8_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0: return board::BUTTONS[0].name;
    case ESP_SLEEP_WAKEUP_EXT1: return board::BUTTON_COUNT > 1 ? board::BUTTONS[1].name : "ext1";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "-";
    default: return "other";
  }
}

static const char* sleepName(uint8_t reason) {
  switch ((SleepReason)reason) {
    case SleepReason::Idle: return "idle";
    case SleepReason::Unreachable: return "unreachable";
    case SleepReason::PairingTimeout: return "pairing timeout";
  }
  return "?";
}

static void recordEvent(uint8_t kind, uint8_t detail, bool print) {
  Event history[EVENT_HISTORY] = {};
  size_t count = 0;
  if (prefs.isKey(NVS_KEY_EVENTS)) {
    count = prefs.getBytes(NVS_KEY_EVENTS, history, sizeof(history)) / sizeof(Event);
  }
  if (count == EVENT_HISTORY) {
    memmove(history, history + 1, sizeof(Event) * (EVENT_HISTORY - 1));
    count--;
  }
  history[count++] = {board::clockSeconds(), board::batteryMillivolts(), kind, detail};
  prefs.putBytes(NVS_KEY_EVENTS, history, count * sizeof(Event));
  if (!print) return;

  uint32_t now = history[count - 1].seconds;
  Serial.println("[log] events, oldest first (time relative to this boot):");
  for (size_t i = 0; i < count; i++) {
    const Event& e = history[i];
    char when[16] = "";
    if (now) snprintf(when, sizeof(when), "%+lds", (long)(e.seconds - now));
    if (e.kind == EVENT_SLEEP) {
      Serial.printf("[log] %9s  sleep      %-16s battery=%u mV\n", when, sleepName(e.detail), e.millivolts);
    } else {
      Serial.printf("[log] %9s  %-10s wake=%-11s battery=%u mV\n", when, resetName(e.kind),
                    wakeName(e.detail), e.millivolts);
    }
  }
}

static View currentView() {
  View v = {};
  v.mode = currentMode();
  v.lightOn = lightOn;
  v.look = LOOKS[activeLook < LOOK_COUNT ? activeLook : NORMAL_LOOK].name;
  v.menuOpen = menuOpen;
  v.items = menuNames;
  v.itemCount = menuCount;
  v.selected = menuSelected;
  v.active = menuIndexOf(activeLook);
  return v;
}

void setup() {
  uint32_t now = millis();
  if (LOW_POWER_MODE) setCpuFrequencyMhz(LOW_POWER_CPU_MHZ);
  Serial.begin(115200);
  board::begin();
  initButtons(now);
  lastActivityAt = now;

  Serial.printf("\n%s -> Hue BLE toggle%s\n", board::NAME, LOW_POWER_MODE ? " (low power)" : "");
#ifdef HUE_DEBUG_DUMP
  Serial.println("[dump] debug build: GATT table is dumped after every connect");
#endif

  NimBLEDevice::init("atom-hue");
  NimBLEDevice::setPower(9);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks, false);
  client->setConnectTimeout(CONNECT_TIMEOUT_MS);

  prefs.begin(NVS_NAMESPACE, false);
  if (prefs.isKey("boots")) prefs.remove("boots");
  recordEvent((uint8_t)esp_reset_reason(), (uint8_t)esp_sleep_get_wakeup_cause(), true);
  supportedEffects = prefs.getULong64(NVS_KEY_EFFECTS, ~0ULL);
  buildMenu();
  String mac = prefs.isKey(NVS_KEY_MAC) ? prefs.getString(NVS_KEY_MAC, "") : String();
  if (mac.length() == 17) {
    target = NimBLEAddress(std::string(mac.c_str()), prefs.getUChar(NVS_KEY_TYPE, BLE_ADDR_PUBLIC));
    Serial.printf("[nvs] saved bulb %s, %d bond(s) stored\n", mac.c_str(), NimBLEDevice::getNumBonds());
    startConnect();
  } else {
    Serial.println("[nvs] no saved bulb");
    pairingMode = true;
    NimBLEDevice::deleteAllBonds();
    printPairingHelp();
    startScan();
  }
}

void loop() {
  uint32_t now = millis();
  for (size_t i = 0; i < board::BUTTON_COUNT; i++) handleButton(i, now);
  handleBleEvents();
  handleTimers();
  handlePower();
  board::show(currentView(), now);
  delay(LOOP_DELAY_MS);
}
