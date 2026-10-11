# atom-hue-ble

Turns an M5Stack ATOM Lite into a one-button remote for a Philips Hue Bluetooth bulb.
Press the button, the light toggles. It talks to the bulb directly over BLE, so there is
no Hue Bridge and no Wi-Fi involved.

Tested with an ATOM Lite and a single Hue bulb.

## Build and flash

You need [PlatformIO](https://platformio.org/install/cli).

```bash
pio run -t upload
pio device monitor
```

## Pairing

The bulb only bonds with a new device while it is in pairing mode, and it only holds about
one BLE connection at a time. So the order matters:

1. Flash the ATOM. With no bulb saved, the LED blinks blue and it starts scanning.
2. In the Hue app go to Settings → Voice assistants, pick Amazon Alexa or Google Home, and
   tap **Make visible**.
3. Close the Hue app (or turn off Bluetooth on the phone) so the bulb is free.
4. Wait. The ATOM picks the Hue bulb with the strongest signal, bonds, and stores its address.
   The LED goes solid when it is done.

The visible window only lasts a minute or so. If you miss it, tap Make visible again; the ATOM
keeps retrying on its own.

From then on it connects straight to the saved bulb at boot.

## Using it

- Press: toggle the light. If the bulb is not connected, a press retries the connection right away.
- Hold for 5 seconds: forget the bulb, delete the bond and restart into pairing mode. The LED
  flashes purple. The light also toggles once, because the toggle fires on press.

LED colours:

| LED | Meaning |
| --- | --- |
| Blinking blue | Scanning / pairing |
| Warm white | Connected, light is on |
| Red | Connected, light is off |
| Blinking yellow | Lost the bulb, reconnecting |
| Fast purple | Pairing reset |

## When it does not work

- **Connect fails, or it never pairs.** Most often the Hue app is still connected to the bulb.
  Close it or disable Bluetooth on the phone.
- **"bulb did not answer the pairing request".** The bulb is not in pairing mode. Make it
  visible again.
- **"encryption failed: bond lost?"** The bulb forgot the ATOM, for example after a reset.
  Hold the button for 5 seconds and pair again.
- **Pairing never works on a bulb that has been paired with many devices.** Its bond list may
  be full. Factory reset the bulb in the Hue app and pair the ATOM first.
- **Random drops.** BLE range to the bulb is short. Keep the ATOM within 5 to 10 m.
- **"Hue light service not found".** Your bulb may use different UUIDs. The `debug-dump`
  build prints every service and characteristic after connecting:

  ```bash
  pio run -e debug-dump -t upload && pio device monitor
  ```

  Then fix `HUE_SERVICE_UUID` and `HUE_ONOFF_UUID` at the top of `src/main.cpp`.

## How it works

The bulb exposes a light control service (`932c32bd-0000-47a2-835a-a8d455b859dd`) with an
on/off characteristic (`932c32bd-0002-...`). Writing `0x01` or `0x00` to it switches the light,
and it also notifies, so the LED follows changes made from the app. The characteristic is only
accessible over an encrypted, bonded link, which is why pairing mode is needed once.

In its advertisements the bulb usually shows the 16-bit `0xFE0F` UUID rather than the light
service, so the scan matches on either, or on a name containing "Hue".

Pins, timings, colours and brightness are constants at the top of `src/main.cpp`.

## License

MIT, see [LICENSE](LICENSE).

This is a hobby project. It is not affiliated with or endorsed by Signify or Philips. The BLE
protocol is not officially documented and may change with a bulb firmware update.
