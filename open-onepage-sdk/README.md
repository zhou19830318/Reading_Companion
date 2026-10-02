# OnePage SDK — Arduino/PlatformIO board support (ESP32-C61)

Board-support package (BSP) for the self-designed **OnePage** e-reader (**ESP32-C61**): display, input, battery, and SD-card drivers, plus dev tools. Designed to be included as a **git submodule** in a PlatformIO project (the `crosspoint-onepage` firmware uses it at `open-onepage-sdk/`).

> **Based on the [OpenX4 E-Paper Community SDK](https://github.com/open-x4-epaper/community-sdk)** (MIT). This is the OnePage/ESP32-C61 adaptation — drivers reworked for the C61 hardware (Arduino framework). A separate ESP-IDF SDK (`onepage-reader-sdk-idf`) is planned for the future native firmware.

## Contents

```
onepage-reader-sdk-arduino/
├── libs/
│   ├── hardware/
│   │   ├── BatteryMonitor/     battery gauge
│   │   ├── InputManager/       buttons (front ADC ladder + side keys + power), #ifdef ONEPAGE_C61
│   │   └── SDCardManager/      SD card
│   └── display/
│       └── EInkDisplay/        SSD1677 e-paper driver (incl. command/waveform sequences)
└── tools/
```

Each lib is self-contained and PlatformIO-friendly (`library.json`).

## Adding to a PlatformIO project

```bash
git submodule add git@github.com:MoveCall/onepage-reader-sdk-arduino.git open-onepage-sdk
```

Then reference the libs in `platformio.ini`:

```ini
lib_deps =
  BatteryMonitor=symlink://open-onepage-sdk/libs/hardware/BatteryMonitor
  InputManager=symlink://open-onepage-sdk/libs/hardware/InputManager
  EInkDisplay=symlink://open-onepage-sdk/libs/display/EInkDisplay
  SDCardManager=symlink://open-onepage-sdk/libs/hardware/SDCardManager
```

## License

MIT — see [LICENSE](./LICENSE). Preserves the OpenX4 community-sdk copyright plus the OnePage C61 port.
