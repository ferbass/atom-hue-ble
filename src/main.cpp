#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include "board.h"

// The look applied by the theme gesture. Colour bulbs use the CIE 1931 x/y coordinates;
// bulbs without colour fall back to `mireds` (153 = cool, 454 = warmest a white bulb can do).
// Set x/y to 0 to always use mireds. Brightness is 1-254.
struct Theme {
  uint8_t brightness;
  uint16_t mireds;
  float x;
  float y;
};
// Candlelight: about 1900 K at 35% brightness.
static const Theme THEME = {90, 454, 0.5381f, 0.4111f};

static const char* HUE_SERVICE_UUID    = "932c32bd-0000-47a2-835a-a8d455b859dd";
static const char* HUE_ONOFF_UUID      = "932c32bd-0002-47a2-835a-a8d455b859dd";
static const char* HUE_BRIGHTNESS_UUID = "932c32bd-0003-47a2-835a-a8d455b859dd";
static const char* HUE_MIREDS_UUID     = "932c32bd-0004-47a2-835a-a8d455b859dd";
static const char* HUE_XY_UUID         = "932c32bd-0005-47a2-835a-a8d455b859dd";
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
static const uint32_t CONNECT_GIVE_UP_MS  = 25000;
static const uint32_t PAIRING_TIMEOUT_MS  = 180000;
static const uint32_t DISCONNECT_WAIT_MS  = 1000;
static const uint32_t LOW_POWER_CPU_MHZ   = 80;

static const char* NVS_NAMESPACE = "hue";
static const char* NVS_KEY_MAC   = "mac";
static const char* NVS_KEY_TYPE  = "type";

enum class State { Scanning, Connecting, Securing, Ready, Disconnecting, WaitRetry, Resetting };

static Preferences prefs;

static State state = State::WaitRetry;
static uint32_t stateSince = 0;
static uint32_t nextAttemptAt = 0;
static uint32_t retryDelay = RETRY_MIN_MS;
static bool pairingMode = false;
static uint32_t lastActivityAt = 0;

static Action pendingAction = Action::None;
static uint32_t pendingSince = 0;

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* onOffChr = nullptr;
static NimBLERemoteCharacteristic* brightnessChr = nullptr;
static NimBLERemoteCharacteristic* miredsChr = nullptr;
static NimBLERemoteCharacteristic* xyChr = nullptr;
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
  onOffChr = brightnessChr = miredsChr = xyChr = nullptr;
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
  brightnessChr = svc->getCharacteristic(HUE_BRIGHTNESS_UUID);
  miredsChr = svc->getCharacteristic(HUE_MIREDS_UUID);
  xyChr = svc->getCharacteristic(HUE_XY_UUID);
  Serial.printf("[gatt] bulb supports:%s%s%s\n", brightnessChr ? " brightness" : "",
                miredsChr ? " white-tone" : "", xyChr ? " colour" : "");

  NimBLEAttValue v = onOffChr->readValue();
  if (v.size() < 1) {
    Serial.println("[gatt] ERROR: could not read on/off state (not bonded, or bulb rejected us)");
    clearCharacteristics();
    return false;
  }
  lightOn = v.data()[0] != 0;
  Serial.printf("[gatt] light is currently %s\n", lightOn ? "ON" : "OFF");

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

static void toggleLight() { setPower(!lightOn); }

static bool writeLook() {
  bool ok = true;
  if (brightnessChr) {
    uint8_t bri = constrain(THEME.brightness, 1, 254);
    ok &= brightnessChr->writeValue(&bri, 1, true);
  }
  if (THEME.x > 0 && THEME.y > 0 && xyChr) {
    uint16_t x = (uint16_t)(constrain(THEME.x, 0.0f, 1.0f) * 0xFFFF);
    uint16_t y = (uint16_t)(constrain(THEME.y, 0.0f, 1.0f) * 0xFFFF);
    uint8_t xy[4] = {(uint8_t)(x & 0xFF), (uint8_t)(x >> 8), (uint8_t)(y & 0xFF), (uint8_t)(y >> 8)};
    Serial.printf("[theme] colour x=%.4f y=%.4f (raw %02x%02x%02x%02x)\n", THEME.x, THEME.y, xy[0],
                  xy[1], xy[2], xy[3]);
    ok &= xyChr->writeValue(xy, sizeof(xy), true);
  } else if (THEME.mireds && miredsChr) {
    uint8_t ct[2] = {(uint8_t)(THEME.mireds & 0xFF), (uint8_t)(THEME.mireds >> 8)};
    Serial.printf("[theme] white tone %u mireds (raw %02x%02x)\n", THEME.mireds, ct[0], ct[1]);
    ok &= miredsChr->writeValue(ct, sizeof(ct), true);
  }
  return ok;
}

static void applyTheme() {
  Serial.println("[theme] applying theme");
  // Setting the look before switching on avoids a flash of the old colour. Some bulbs refuse
  // these writes while off, so retry once the light is on.
  bool lookOk = writeLook();
  if (!lightOn && !setPower(true)) return;
  if (!lookOk && !writeLook()) Serial.println("[theme] ERROR: bulb rejected part of the theme");
}

static void runAction(Action action) {
  lastActivityAt = millis();
  switch (action) {
    case Action::Toggle: toggleLight(); break;
    case Action::Theme: applyTheme(); break;
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
  Serial.flush();
  board::sleep(reason);
}

static void handlePower(uint32_t now) {
  if (!LOW_POWER_MODE || state == State::Resetting || !buttonsIdle()) return;
  uint32_t idle = now - lastActivityAt;
  if (state == State::Ready) {
    if (idle >= IDLE_SLEEP_MS) goToSleep(SleepReason::Idle);
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
  handlePower(now);
  board::show(currentMode(), lightOn, now);
  delay(LOOP_DELAY_MS);
}
