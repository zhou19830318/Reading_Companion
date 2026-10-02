# Web Server Guide

This guide explains how to use the built-in web server for file transfer, device
settings, Wi-Fi / Cloud Services / OPDS management, and SD-card font management.

## Overview

The web server is available while the device is in **File Transfer** or
**Calibre Wireless** mode. It can:

- Upload, download, rename, move, and delete files on the SD card
- Create folders
- Edit device settings from a browser across six panels (Display, Reader,
  Controls, System, KOReader Sync, Status Bar)
- Manage saved Wi-Fi networks, the Cloud Services credentials (OpenClaw gateway +
  MiMo speech-to-text), and OPDS servers
- Restore factory settings and reboot the device
- Serve the whole UI in Chinese or English with a one-click language switch
- Upload and delete `.cpfont` SD-card font families
- Accept WebDAV clients and Calibre wireless uploads

The server does not require authentication. Use it only on trusted private
networks or in hotspot mode when you control who is connected.

## Starting File Transfer

1. From the Home screen, select **File Transfer**.
2. Choose one of the available modes:

| Mode | Use when |
|------|----------|
| **Join Network** | You want the reader to join an existing Wi-Fi network. |
| **Calibre Wireless** | You want to receive books from the CrossPoint Calibre plugin workflow. |
| **Create Hotspot** | You want the reader to create its own open Wi-Fi network. |

## Join Network Mode

1. Select **Join Network**.
2. Pick a 2.4 GHz Wi-Fi network from the scan results.
3. Enter the password if prompted.
4. Save credentials if you want the reader to reconnect automatically next time.

After connection, the reader shows:

- The connected SSID and Wi-Fi strength
- A QR code for the web URL
- The direct IP URL, for example `http://192.168.1.102/`

On the OnePage (ESP32-C61) build mDNS is stubbed out, so
`http://crosspoint.local/` does **not** resolve — always use the IP URL printed
on the screen (or encoded in the QR) from a phone, tablet, or computer on the
same network.

## Create Hotspot Mode

1. Select **Create Hotspot**.
2. Connect your phone or computer to the open Wi-Fi network:

```text
CrossPoint-Reader
```

3. Open the URL shown on the reader. On OnePage this is the hotspot IP,
   typically `http://192.168.4.1/` — mDNS is not compiled in, so
   `http://crosspoint.local/` will not resolve.

The reader displays one QR code for joining the hotspot and another QR code for
opening the web interface.

## Calibre Wireless Mode

Calibre Wireless starts the same web server in station mode, then displays setup
instructions and upload progress on the reader. Use this mode with the
CrossPoint Calibre plugin or other clients that speak the documented WebSocket
upload protocol.

For Calibre OPDS browsing, add `/opds` to the catalog URL when configuring an
OPDS server.

## Web Interface

The browser UI has four primary pages, reached from the header tabs (**Home**,
**File Manager**, **Settings**, **Fonts**). Every page shares the same header,
which carries a **中文 / EN** language button; the choice is remembered in the
browser and sent to the API as `?lang=`.

### Home

The Home page shows firmware status, network mode, IP address, device type,
uptime, and free heap.

### File Manager

The File Manager page can:

- Browse SD-card folders
- Upload files, using WebSocket upload when available and HTTP upload as a fallback
- Create folders
- Download files
- Rename files
- Move files into existing folders
- Delete one or more selected files or empty folders

Existing files with the same name are overwritten by uploads. When EPUB files
are overwritten, moved, renamed, or deleted through the web server, the matching
book cache is cleared so stale metadata is not reused.

### Settings

The Settings page is organised with a second-level chip bar, mirroring the
on-device settings menu:

| Panel | Contents |
|-------|----------|
| **Display** | Sleep screen (mode, cover fit, filter), quick resume, battery display, refresh cadence, UI theme, sunlight fading fix |
| **Reader** | Font family and size, line spacing, margins, paragraph alignment, embedded styles, focus reading, hyphenation, orientation, text anti-aliasing, ruled lines, image handling |
| **Controls** | Side/front button roles, long-press behaviour and menu, short power-button action |
| **System** | Time to sleep, hidden files, recent-book and finished-book behaviour |
| **KOReader Sync** | Username, password, sync server URL, document matching method (web-only) |
| **Status Bar** | Chapter/page count, progress bar and thickness, title, battery, clock and UTC offset, clock format, XTC status bar (web-only) |
| **Wi-Fi** | Saved networks (up to 8): SSID, password, auto-connect, last-connected marker |
| **Cloud Services** | OpenClaw gateway (host, port, path, TLS, protocol, CA path, token) with an approval/test card, plus the MiMo STT URL/key test card |
| **OPDS** | Saved OPDS servers (up to 8): title, URL, credentials |

Below the chips, one card per category is shown at a time; each panel carries its
own usage hint. The action bar under the cards provides:

- **Save Settings** — green primary button, enabled only when the current
  category has unsaved changes
- **Reset Defaults** — restores every `settings.json` field to its firmware
  default (Wi-Fi / OPDS lists and the Cloud Services secrets are untouched), then
  reloads the page
- **Reboot Device** — restarts the reader once the response has been sent

Use the **中文 / EN** button in the header to flip the whole UI language; the
settings cards are re-rendered in the selected language without losing edits.

Secrets behave consistently across the page: Wi-Fi and OPDS passwords and the
Cloud Services token / STT key are accepted on save but never returned by the
API — the browser only sees presence flags, and Cloud Services credentials are
stored in NVS flash rather than on the SD card.

### Fonts

The Fonts page lists installed SD-card font families and lets you upload
`.cpfont` files. Upload files from one font family at a time. The server validates
the font family name, filename, and `.cpfont` magic bytes before accepting the
upload.

Installed fonts appear in **Settings > Reader > Font Family** after the font
registry refreshes.

## Command Line Use

Power users can use `curl`, WebDAV clients, or WebSocket clients while the web
server is running.

Endpoint details are documented in [webserver-endpoints.md](./webserver-endpoints.md).

## Security Notes

- The HTTP server runs on port 80; the WebSocket upload server runs on port 81.
- There is no authentication. Anyone on the same network can read and write the
  SD card and change settings while the server is running.
- The server stops when you exit File Transfer or Calibre Wireless mode.
- Hotspot mode creates an open network for connectivity fallback; disconnect when done.
- Secrets never leave the device: the settings, Wi-Fi, OPDS and `/api/cloud`
  responses only report whether a credential is set (presence flags), not its
  value. Entering a blank password during an edit keeps the stored one.
- Cloud Services credentials live in NVS flash, not on the SD card, so they
  survive a factory-reset of `settings.json` and are not copied with the card.
- **Reset Defaults** clears `settings.json` only. Saved Wi-Fi networks, OPDS
  servers, KOReader credentials and Cloud Services secrets are kept; the UI
  asks for confirmation first.

## Tips

1. Use **Create Hotspot** when no trusted network is available.
2. Use the IP URL printed on the reader — `crosspoint.local` does not resolve on
   the OnePage build (mDNS is not compiled in).
3. Move closer to the router if upload progress stalls in Join Network mode.
4. Upload custom fonts through the Fonts page or copy them to `/.fonts/` or `/fonts/` on the SD card.
5. Switch the UI language with the **中文 / EN** button in the header; edits you
   have not saved yet are preserved across the switch.
6. Exit File Transfer mode when finished to conserve battery.

## Related Documentation

- [User Guide](../USER_GUIDE.md)
- [Webserver Endpoints](./webserver-endpoints.md)
- [SD Card Fonts](./sd-card-fonts.md)
- [Troubleshooting](./troubleshooting.md)
