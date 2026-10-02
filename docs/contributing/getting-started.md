# Getting Started

This guide helps you build and run the OnePage firmware locally. It is the
CrossPoint port for the self-designed **OnePage** reader (**ESP32-C61**), so the
hardware-specific steps below differ from upstream CrossPoint (X3/X4, ESP32-C3).

## Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO IDE (pioarduino)
- Python 3.8+ (PlatformIO itself needs Python 3.10+; if your `pio` runs on an
  older interpreter use `~/.platformio/penv/bin/pio`)
- `clang-format` 21+ in your `PATH` (CI uses clang-format 21)
- USB-C cable supporting data transfer
- A OnePage reader for hardware testing

If `./bin/clang-format-fix` fails with either of these errors, install clang-format 21:

- `clang-format: No such file or directory`
- `.clang-format: error: unknown key 'AlignFunctionDeclarations'`

Examples:

```sh
# Debian/Ubuntu (try this first)
sudo apt-get update && sudo apt-get install -y clang-format-21

# If the package is unavailable, add LLVM apt repo and retry
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 21
sudo apt-get update
sudo apt-get install -y clang-format-21

# macOS (Homebrew)
brew install clang-format
```

Then verify:

```sh
clang-format-21 --version
```

The reported major version must be 21 or newer.

## Clone and initialize

```sh
git clone https://github.com/zhou19830318/Reading_Companion.git
cd Reading_Companion
```

There are **no submodules** — the board SDK (`open-onepage-sdk/`: EInkDisplay,
InputManager, BatteryMonitor, SDCardManager) is vendored in the tree and wired in
through the `symlink://` entries in `platformio.ini`.

Enable the repository-managed Git hooks (required once per clone):

```sh
git config core.hooksPath .githooks
chmod +x .githooks/pre-commit
```

## Build

```sh
pio run -e onepage          # OnePage / ESP32-C61  ← the target of this repo
```

`platformio.ini` also keeps the upstream `default` (X4 / ESP32-C3), `slim` and
`gh_release*` environments; they are retained for parity with upstream but are
not what you flash onto a OnePage.

## Flash

```sh
pio run -e onepage --target upload
```

If PlatformIO's upload stage fails on a Puya-flash board, flash manually with
`esptool --no-stub` — see the [Install firmware](../../README.md#install-firmware)
section, which documents the Winbond vs Puya split.

## Web UI pages

The four browser pages in `src/network/html/` are self-contained sources that get
inlined into PROGMEM. After editing any of them:

```sh
python3 scripts/sync_web_core.py    # re-inject shared design-system CSS/JS
python3 scripts/build_html.py       # minify + gzip -> *.generated.h (runs node --check)
```

Shared styles/JS must only be changed inside `scripts/sync_web_core.py`
(`CORE_CSS` / `CORE_JS`), never directly in a page, or `--check` will report
drift. Generated `*.generated.h` files are gitignored.

## First checks before opening a PR

```sh
./bin/clang-format-fix
python3 scripts/sync_web_core.py --check
python3 scripts/build_html.py
pio run -e onepage
```

## What to read next

- [Architecture Overview](./architecture.md)
- [Development Workflow](./development-workflow.md)
- [Testing and Debugging](./testing-debugging.md)
