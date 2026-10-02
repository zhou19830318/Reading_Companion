# Testing and Debugging

The firmware runs on real hardware, so debugging usually combines local build checks and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
python3 scripts/sync_web_core.py --check   # shared web CSS/JS drift gate
python3 scripts/build_html.py              # regenerate the inlined web pages
pio run -e onepage
```

## Flash and monitor

Flash firmware:

```sh
pio run -e onepage --target upload
```

If the upload stage fails on a Puya-flash board, flash manually with
`esptool --no-stub` (see the [README](../../README.md#install-firmware)).

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## OnePage serial commands

The main loop reads `CMD:` lines from the debug serial port, which makes the UI
drivable without hands on the buttons (see `src/main.cpp`):

| Command | Effect |
|---------|--------|
| `CMD:SCREENSHOT` | Dumps the whole framebuffer (`SCREENSHOT_START:<len>` … raw bytes … `SCREENSHOT_END`) plus a pointer/CRC line |
| `CMD:KEY back\|ok\|left\|right\|up\|down` | Injects one synthetic press, released on the next `update()` |
| `CMD:GO_VOICE`, `CMD:GO_WORKBENCH` | Jump straight to the voice assistant / workbench screens |
| `CMD:VOICE_TX <text>` | Stands in for the STT step: sends `<text>` as a chat round trip to the gateway |

Notes:

- The framebuffer dump is 800×480 at 1 bit per pixel and needs a 90° rotation to
  look right on a screen-grab viewer.
- `CMD:KEY` deliberately excludes **Power** — that key feeds the sleep and
  screenshot combinations — so a device already in the sleep overlay cannot be
  woken over serial. Recover with an RTS reset instead:
  `esptool --chip esp32c61 --before default-reset --after hard-reset -p <port> read_mac`.
- `GET /api/settings` is served without cache headers, so a browser can show a
  stale settings list after a write. Append a cache-buster (`?_=<timestamp>`)
  when verifying a change from the UI or a script.

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
