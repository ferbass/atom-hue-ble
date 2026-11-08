# atom-hue-ble

Turns an M5Stack ATOM Lite or CoreInk into a one-button remote for a Philips Hue Bluetooth
bulb. Click to switch your normal light on or off, double-click for the Hue Candle effect. It talks to the bulb directly over BLE, so there is no Hue Bridge and no Wi-Fi involved.

Tested with an ATOM Lite (both builds) and a Hue White and Color Ambiance bulb (LCA002,
firmware 1.163.1). The CoreInk build compiles but has not been tested on hardware yet.

## Builds

| Environment | Board | Behaviour |
| --- | --- | --- |
| `m5stack-atom` (default) | ATOM Lite | Stays connected. Instant toggle, but drains a battery in hours. |
| `m5stack-atom-lowpower` | ATOM Lite | Deep sleeps between presses. |
| `m5stack-coreink` | CoreInk | Deep sleeps between presses, shows the state on the e-ink screen. |
| `debug-dump` | ATOM Lite | Prints the bulb's GATT table after connecting. |

In low-power builds a press wakes the board. It reconnects to the bulb, runs the action and
stays awake for 5 seconds so quick follow-up presses are instant, then goes back to sleep.
A press takes about 1 to 3 seconds to reach the light, in exchange for much longer battery life. As a bonus, the bulb is free for the Hue app while the board sleeps.

If the bulb cannot be reached within 25 seconds, the board gives up and sleeps
(ATOM: three red flashes, CoreInk: "No bulb"). Press again to retry. Pairing mode gives up
after 3 minutes the same way.

Each press costs about 8 seconds awake (connect, act, 5 s idle). Between presses the ESP32
draws almost nothing, so battery life is set by the board's own sleep current. On the ATOM
Lite the RGB LED draws around 1 mA even when dark, and the battery base adds its own drain,
so expect days to a couple of weeks rather than months (estimated, not measured). The CoreInk
is built for battery use and should last much longer. A USB power meter between the charger
and the board shows the real sleep current.

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

- Click: if the light is on, switch it off. If it is off, switch it on as your normal light.
- Double-click: Candle effect. The bulb runs the flicker itself, so the board can sleep.
- Hold 2 seconds and release: save the bulb's current look as your normal light. The LED blinks
  green twice.
- Hold 5 seconds: forget the bulb, delete the bond and restart into pairing mode. The LED
  flashes purple.

CoreInk (the wheel on the side):

- Press the wheel: same as the ATOM click. Double-press: Candle.
- Roll the wheel up: Candle.
- Hold the wheel 2 seconds: save the normal light. 5 seconds: forget the bulb and pair again.

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

### Normal light

Out of the box the normal light is a steady 2730 K at full brightness. To change it, set the
look you want in the Hue app, close the app, then hold the button for 2 seconds. Brightness,
white tone and colour are saved; any effect is dropped so the normal light never flickers.
The saved look is kept across sleep and reflashing, and is cleared by the 5 second reset.

To change the double-click effect, edit `CANDLE` at the top of `src/main.cpp`. The effect id
is the byte after `06 01`: 01 candle, 02 fireplace, 03 prism, 09 sunrise, 0a sparkle,
0b opal, 0c glisten, 0d sunset, 0e underwater, 0f cosmos, 10 sunbeam, 11 enchant.

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
- **Click only switches on and off, double-click does nothing.** The bulb has no combined
  state characteristic (`0007`), which older firmware lacks. Update the bulb in the Hue app.
- **"Hue light service not found".** Your bulb may use different UUIDs. The `debug-dump`
  build prints every service and characteristic after connecting:

  ```bash
  pio run -e debug-dump -t upload && pio device monitor
  ```

  Then fix `HUE_SERVICE_UUID` and `HUE_ONOFF_UUID` at the top of `src/main.cpp`.

## How it works

The bulb exposes a light control service (`932c32bd-0000-47a2-835a-a8d455b859dd`). Its on/off
characteristic (`0002`) takes `0x01` or `0x00` and notifies, so the LED follows changes made
from the app. The combined state characteristic (`0007`) takes a list of
`[type, length, value]` entries in one write:

| Type | Value |
| --- | --- |
| `01` | power, 1 byte |
| `02` | brightness, 1 to 254 |
| `03` | white tone in mireds, 2 bytes little-endian |
| `04` | CIE x and y, 2 bytes each, little-endian, scaled to 0xFFFF |
| `05` | transition time, 2 bytes little-endian, in 100 ms units |
| `06` | effect id (`00` none, `01` candle, ...) |
| `08` | effect speed |

Reading it returns the current state in the same format, with the colour entry when the bulb is
in colour mode and the white tone entry when it is in white mode. Leaving the effect entry out
of a write stops a running effect. The format matches
[HueBLE](https://github.com/flip-dots/HueBLE).

These characteristics are only accessible over an encrypted, bonded link, which is why pairing
mode is needed once.

In its advertisements the bulb usually shows the 16-bit `0xFE0F` UUID rather than the light
service, so the scan matches on either, or on a name containing "Hue".

Timings, the default normal light and the Candle command are constants at the top of
`src/main.cpp`. Pins and LED colours are in `src/board_atom.cpp` and `src/board_coreink.cpp`.

## License

MIT, see [LICENSE](LICENSE).

This is a hobby project. It is not affiliated with or endorsed by Signify or Philips. The BLE
protocol is not officially documented and may change with a bulb firmware update.
