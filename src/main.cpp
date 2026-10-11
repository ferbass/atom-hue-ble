#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

static const char* HUE_SERVICE_UUID = "932c32bd-0000-47a2-835a-a8d455b859dd";
static const char* HUE_ONOFF_UUID   = "932c32bd-0002-47a2-835a-a8d455b859dd";
// Hue bulbs usually advertise only this 16-bit Signify UUID, not the 128-bit light service.
static const uint16_t HUE_ADV_UUID16 = 0xFE0F;
static const char* HUE_NAME_HINT    = "Hue";

static const uint8_t BUTTON_PIN = 39;
static const uint8_t LED_PIN    = 27;

static const uint32_t DEBOUNCE_MS        = 50;
static const uint32_t RESET_HOLD_MS      = 5000;
static const uint32_t RESET_FLASH_MS     = 1500;
static const uint32_t SCAN_WINDOW_MS     = 5000;
static const uint32_t CONNECT_TIMEOUT_MS = 10000;
static const uint32_t SECURE_TIMEOUT_MS  = 15000;
static const uint32_t RETRY_MIN_MS       = 2000;
static const uint32_t RETRY_MAX_MS       = 30000;
static const uint32_t BLINK_MS           = 400;
static const uint32_t FAST_BLINK_MS      = 120;

static const uint8_t LED_BRIGHTNESS = 40;
static const uint32_t COLOR_ON = Adafruit_NeoPixel::Color(255, 170, 80);
static const uint32_t COLOR_OFF = Adafruit_NeoPixel::Color(160, 0, 0);
static const uint32_t COLOR_PAIRING = Adafruit_NeoPixel::Color(0, 0, 255);
static const uint32_t COLOR_RECONNECT = Adafruit_NeoPixel::Color(255, 160, 0);
static const uint32_t COLOR_RESET = Adafruit_NeoPixel::Color(180, 0, 255);

static const char* NVS_NAMESPACE = "hue";
static const char* NVS_KEY_MAC   = "mac";
static const char* NVS_KEY_TYPE  = "type";

enum class State { Scanning, Connecting, Securing, Ready, Disconnecting, WaitRetry, Resetting };

static const uint32_t COLOR_BLACK = 0;
static Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
static uint32_t shownColor = COLOR_BLACK;
static Preferences prefs;

static State state = State::WaitRetry;
static uint32_t stateSince = 0;
static uint32_t nextAttemptAt = 0;
static uint32_t retryDelay = RETRY_MIN_MS;
static bool pairingMode = false;

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* onOffChr = nullptr;
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

static bool btnRaw = false;
static bool btnStable = false;
static bool btnLongFired = false;
static uint32_t btnChangedAt = 0;
static uint32_t btnPressedAt = 0;

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

static void scheduleRetry() {
  onOffChr = nullptr;
  nextAttemptAt = millis() + retryDelay;
  Serial.printf("[ble] retrying in %lu ms\n", (unsigned long)retryDelay);
  if (!pairingMode) retryDelay = min(retryDelay * 2, RETRY_MAX_MS);
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

  NimBLEAttValue v = onOffChr->readValue();
  if (v.size() < 1) {
    Serial.println("[gatt] ERROR: could not read on/off state (not bonded, or bulb rejected us)");
    onOffChr = nullptr;
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

static void toggleLight() {
  if (!onOffChr) return;
  uint8_t value = lightOn ? 0x00 : 0x01;
  Serial.printf("[gatt] writing 0x%02x (turn %s)\n", value, value ? "ON" : "OFF");
  if (onOffChr->writeValue(&value, 1, true)) {
    lightOn = value != 0;
    Serial.println("[gatt] write ok");
  } else {
    Serial.println("[gatt] ERROR: write failed");
  }
}

static void beginReset() {
  Serial.println("[reset] long press: clearing saved bulb and all bonds");
  prefs.clear();
  NimBLEDevice::getScan()->stop();
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteAllBonds();
  setState(State::Resetting);
}

static void onButtonPress() {
  switch (state) {
    case State::Ready:
      toggleLight();
      break;
    case State::WaitRetry:
      Serial.println("[btn] press while disconnected: reconnecting now");
      retryDelay = RETRY_MIN_MS;
      startAttempt();
      break;
    default:
      Serial.println("[btn] press ignored: connection in progress");
      break;
  }
}

static void handleButton(uint32_t now) {
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  if (raw != btnRaw) {
    btnRaw = raw;
    btnChangedAt = now;
  }
  if (raw != btnStable && now - btnChangedAt >= DEBOUNCE_MS) {
    btnStable = raw;
    if (btnStable) {
      btnPressedAt = now;
      btnLongFired = false;
      onButtonPress();
    }
  }
  if (btnStable && !btnLongFired && now - btnPressedAt >= RESET_HOLD_MS) {
    btnLongFired = true;
    beginReset();
  }
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
        Serial.println("[ble] ready - press the button to toggle");
        setState(State::Ready);
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

static void updateLed(uint32_t now) {
  uint32_t color;
  if (state == State::Resetting) {
    color = (now / FAST_BLINK_MS) % 2 ? COLOR_RESET : COLOR_BLACK;
  } else if (state == State::Ready) {
    color = lightOn ? COLOR_ON : COLOR_OFF;
  } else {
    color = (now / BLINK_MS) % 2 ? (pairingMode ? COLOR_PAIRING : COLOR_RECONNECT) : COLOR_BLACK;
  }
  if (color != shownColor) {
    shownColor = color;
    led.setPixelColor(0, color);
    led.show();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT);

  led.begin();
  led.setBrightness(LED_BRIGHTNESS);
  led.show();

  Serial.println("\nATOM Lite -> Hue BLE toggle");
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
  handleButton(now);
  handleBleEvents();
  handleTimers();
  updateLed(now);
}
