# Troubleshooting

This document shows common issues and possible solutions while using the device features.

- [Troubleshooting](#troubleshooting)
    - [Cannot See the Device on the Network](#cannot-see-the-device-on-the-network)
    - [Connection Drops or Times Out](#connection-drops-or-times-out)
    - [Upload Fails](#upload-fails)
    - [Saved Password Not Working](#saved-password-not-working)
    - [OnePage-specific issues](#onepage-specific-issues)

### Cannot See the Device on the Network

**Problem:** Browser shows "Cannot connect" or "Site can't be reached"

**Solutions:**

1. Verify both devices are on the correct network
   - Check your computer/phone Wi-Fi settings
   - In **Join Network** mode, your computer/phone and CrossPoint Reader must be on the same Wi-Fi network
   - In **Create Hotspot** mode, your computer/phone must be connected to the `CrossPoint-Reader` hotspot
2. Double-check the IP address
   - Make sure you typed it correctly
   - Include `http://` at the beginning
   - On OnePage, always use the IP address printed on screen — mDNS is not
     compiled into the C61 build, so `http://crosspoint.local/` never resolves
3. Try disabling VPN if you're using one
4. Some networks have "client isolation" enabled - use Create Hotspot mode or check with your network administrator

### Connection Drops or Times Out

**Problem:** Wi-Fi connection is unstable

**Solutions:**

1. Move closer to the Wi-Fi router, or use Create Hotspot mode for a direct connection
2. Check signal strength on the device (should be at least `||` or better)
3. Avoid interference from other devices
4. Try a different Wi-Fi network if available

### Upload Fails

**Problem:** File upload doesn't complete or shows an error

**Solutions:**

1. Check that the SD card has enough free space
2. Check that the filename is valid for the SD card filesystem
3. Try uploading a smaller file first to test
4. Refresh the browser page and try again
5. If WebSocket upload fails repeatedly, refresh the page and retry with the HTTP fallback path

### Saved Password Not Working

**Problem:** Device fails to connect with saved credentials

**Solutions:**

1. When connection fails, you'll be prompted to "Forget Network"
2. Select **Yes** to remove the saved password
3. Reconnect and enter the password again
4. Choose to save the new password

## OnePage-specific issues

### Settings page shows stale values after saving

**Problem:** A saved change (or a factory reset) does not appear when the
Settings page reloads.

**Solutions:**

1. Hard-refresh the page (Ctrl/Cmd+Shift+R) — `GET /api/settings` has no cache
   headers, so the browser may replay an old response
2. When scripting against the API, append a cache-buster:
   `/api/settings?lang=en&_=<timestamp>`

### Voice assistant / workbench does not respond

**Problem:** **AI Assistant** or **My Workbench** cannot reach the gateway.

**Solutions:**

1. Open **Settings → Cloud Services** on the web UI and check the gateway host,
   port, path and TLS settings, then use the test card — it reports whether the
   gateway is reachable and whether this device is still *pending approval*
2. Approve the device on the gateway side; an unapproved device is refused even
   when the token is correct
3. Confirm the MiMo speech-to-text URL/key card passes its test before relying
   on voice capture; the STT key and gateway token are stored in NVS and are
   never echoed back to the browser
4. The reader must stay in **File Transfer → Join Network** mode (or otherwise
   be online) for gateway traffic

### Firmware update should not be used

**Problem:** **Settings → System → Check for updates** offers a download.

That entry still polls the upstream CrossPoint releases, which ship an
ESP32-C3 image. It is not compatible with OnePage (ESP32-C61) — ignore it and
flash over USB instead; see the [README](../README.md#install-firmware).
