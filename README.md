# atom-hue-ble

Turns an M5Stack ATOM Lite or CoreInk into a one-button remote for a Philips Hue Bluetooth
bulb. Press the button, the light toggles. Double-click, and it switches to your favourite
look. It talks to the bulb directly over BLE, so there is no Hue Bridge and no Wi-Fi involved.

Tested with an ATOM Lite and a single Hue bulb. The CoreInk build and the low-power mode
build cleanly but have not been tested on hardware yet.

## Builds

| Environment | Board | Behaviour |
| --- | --- | --- |
| `m5stack-atom` (default) | ATOM Lite | Stays connected. Instant toggle, but drains a battery in hours. |
| `m5stack-atom-lowpower` | ATOM Lite | Deep sleeps between presses. |
| `m5stack-coreink` | CoreInk | Deep sleeps between presses, shows the state on the e-ink screen. |
| `debug-dump` | ATOM Lite | Prints the bulb's GATT table after connecting. |

In low-power builds a press wakes the board. It reconnects to the bulb, runs the action and
stays awake for 5 seconds so quick follow-up presses are instant, then goes back to sleep.
A press takes about 1 to 3 seconds to reach the light, in exchange for battery life measured
in weeks instead of hours (estimated, not measured yet). As a bonus, the bulb is free for the Hue app while the board sleeps.

If the bulb cannot be reached within 25 seconds, the board gives up and sleeps
(ATOM: three red flashes, CoreInk: "No bulb"). Press again to retry. Pairing mode gives up
after 3 minutes the same way.

On the ATOM Lite the RGB LED still draws around 1 mA while dark, so sleep current is not
as low as the ESP32 alone. The CoreInk is the better choice for battery use.

## Build and flash

You need [PlatformIO](https://platformio.org/install/cli).

```bash
pio run -t upload                              # ATOM Lite, always connected
pio run -e m5stack-atom-lowpower -t upload     # ATOM Lite, low power
pio run -e m5stack-coreink -t upload           # CoreInk
pio device monitor
```

On battery, press the CoreInk's power button once to start it. After that the firmware keeps
it powered and sleeps between presses.

## Pairing

The bulb only bonds with a new device while it is in pairing mode, and it only holds about
one BLE connection at a time. So the order matters:

1. Flash the board. With no bulb saved, it starts scanning (ATOM: LED blinks blue,
   CoreInk: "Pairing" on screen).
2. In the Hue app go to Settings → Voice assistants, pick Amazon Alexa or Google Home, and
   tap **Make visible**.
3. Close the Hue app (or turn off Bluetooth on the phone) so the bulb is free.
4. Wait. The board picks the Hue bulb with the strongest signal, bonds, and stores its
   address. The ATOM LED goes solid, or the CoreInk shows ON/OFF, when it is done.

The visible window only lasts a minute or so. If you miss it, tap Make visible again; the board
keeps retrying on its own.

From then on it connects straight to the saved bulb at boot.

## Using it

ATOM Lite (the only button):

- Click: toggle the light.
- Double-click: apply the theme (see below). It also switches the light on.
- Hold for 5 seconds: forget the bulb, delete the bond and restart into pairing mode. The LED
  flashes purple.

CoreInk (the wheel on the side):

- Press the wheel: toggle the light. Double-press: apply the theme.
- Roll the wheel up: apply the theme.
- Hold the wheel pressed for 5 seconds: forget the bulb and pair again.

The top button and wheel-down are unused, because the ESP32 can only be woken from deep
sleep by two of the CoreInk's buttons.

A single click waits 350 ms to make sure it is not a double-click, before toggling. If the
bulb is not connected, the action is queued and the connection is retried right away.

ATOM LED colours (in low-power mode the LED is only lit while the board is awake):

| LED | Meaning |
| --- | --- |
| Blinking blue | Scanning / pairing |
| Warm white | Connected, light is on |
| Red | Connected, light is off |
| Blinking yellow | Lost the bulb, reconnecting |
| Fast purple | Pairing reset |

### Theme

The theme is set at the top of `src/main.cpp`. The default is candlelight:

```cpp
static const Theme THEME = {90, 454, 0.5381f, 0.4111f};
```

The fields are brightness (1 to 254), a white tone in mireds, and CIE x/y for a colour. Colour
bulbs use x/y. Bulbs without colour fall back to mireds, and 454 (about 2200 K) is the warmest
they can do. Candlelight is about 1900 K, so only colour bulbs get the full effect.

The Hue app's animated "Candle" effect cannot be reproduced this way: the firmware sets a
still colour and brightness.

#### Copying a look from the Hue app

1. Set the look in the Hue app, then close the app so the bulb is free.
2. Flash `debug-dump` and open the monitor:

   ```bash
   pio run -e debug-dump -t upload && pio device monitor
   ```

3. In the dump, read these values. All of them are little-endian.

   | Characteristic | Meaning | Example `value=` | Decoded |
   | --- | --- | --- | --- |
   | `932c32bd-0003` | brightness | `5a` | 90 |
   | `932c32bd-0004` | mireds | `c601` | 0x01c6 = 454 |
   | `932c32bd-0005` | x, y | `c0893d69` | 0x89c0 / 65535 = 0.5381, 0x693d / 65535 = 0.4111 |

4. Put the numbers in `THEME`, flash your normal build and double-click. The serial log shows
   `[theme] colour x=... (raw ...)`. Flash `debug-dump` again and check that the raw bytes
   match what the bulb reports.

## When it does not work

- **Connect fails, or it never pairs.** Most often the Hue app is still connected to the bulb.
  Close it or disable Bluetooth on the phone.
- **"bulb did not answer the pairing request".** The bulb is not in pairing mode. Make it
  visible again.
- **"encryption failed: bond lost?"** The bulb forgot the ATOM, for example after a reset.
  Hold the button (CoreInk: the wheel) for 5 seconds and pair again.
- **Pairing never works on a bulb that has been paired with many devices.** Its bond list may
  be full. Factory reset the bulb in the Hue app and pair the ATOM first.
- **Random drops.** BLE range to the bulb is short. Keep the board within 5 to 10 m.
- **Theme only changes brightness.** White-only bulbs have no colour or white-tone
  characteristic, and colour values are ignored when `mireds` is not 0.
- **"Hue light service not found".** Your bulb may use different UUIDs. The `debug-dump`
  build prints every service and characteristic after connecting:

  ```bash
  pio run -e debug-dump -t upload && pio device monitor
  ```

  Then fix `HUE_SERVICE_UUID` and `HUE_ONOFF_UUID` at the top of `src/main.cpp`.

## How it works

The bulb exposes a light control service (`932c32bd-0000-47a2-835a-a8d455b859dd`) with an
on/off characteristic (`932c32bd-0002-...`). Writing `0x01` or `0x00` to it switches the light,
and it also notifies, so the LED follows changes made from the app. The theme writes
brightness (`0003`), white tone (`0004`) or colour (`0005`) on the same service. The characteristic is only
accessible over an encrypted, bonded link, which is why pairing mode is needed once.

In its advertisements the bulb usually shows the 16-bit `0xFE0F` UUID rather than the light
service, so the scan matches on either, or on a name containing "Hue".

Pins, timings, colours and brightness are constants at the top of `src/main.cpp`.

## License

MIT, see [LICENSE](LICENSE).

This is a hobby project. It is not affiliated with or endorsed by Signify or Philips. The BLE
protocol is not officially documented and may change with a bulb firmware update.
